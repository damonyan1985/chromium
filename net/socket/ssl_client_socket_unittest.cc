// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/ssl_client_socket.h"

#include <errno.h>
#include <string.h>

#include <algorithm>
#include <tuple>
#include <utility>

#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/files/file_util.h"
#include "base/location.h"
#include "base/memory/ref_counted.h"
#include "base/optional.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/stl_util.h"
#include "base/strings/stringprintf.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "crypto/rsa_private_key.h"
#include "net/base/address_list.h"
#include "net/base/completion_once_callback.h"
#include "net/base/features.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/network_isolation_key.h"
#include "net/base/test_completion_callback.h"
#include "net/cert/asn1_util.h"
#include "net/cert/ct_policy_enforcer.h"
#include "net/cert/ct_policy_status.h"
#include "net/cert/ct_verifier.h"
#include "net/cert/do_nothing_ct_verifier.h"
#include "net/cert/mock_cert_verifier.h"
#include "net/cert/signed_certificate_timestamp_and_status.h"
#include "net/cert/test_root_certs.h"
#include "net/cert/x509_util.h"
#include "net/der/input.h"
#include "net/der/parser.h"
#include "net/der/tag.h"
#include "net/dns/host_resolver.h"
#include "net/http/transport_security_state.h"
#include "net/http/transport_security_state_test_util.h"
#include "net/log/net_log_event_type.h"
#include "net/log/net_log_source.h"
#include "net/log/test_net_log.h"
#include "net/log/test_net_log_util.h"
#include "net/socket/client_socket_factory.h"
#include "net/socket/client_socket_handle.h"
#include "net/socket/socket_test_util.h"
#include "net/socket/ssl_server_socket.h"
#include "net/socket/stream_socket.h"
#include "net/socket/tcp_client_socket.h"
#include "net/socket/tcp_server_socket.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "net/ssl/ssl_client_session_cache.h"
#include "net/ssl/ssl_config_service.h"
#include "net/ssl/ssl_connection_status_flags.h"
#include "net/ssl/ssl_handshake_details.h"
#include "net/ssl/ssl_info.h"
#include "net/ssl/ssl_server_config.h"
#include "net/ssl/test_ssl_config_service.h"
#include "net/ssl/test_ssl_private_key.h"
#include "net/test/cert_test_util.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "net/test/gtest_util.h"
#include "net/test/key_util.h"
#include "net/test/spawned_test_server/spawned_test_server.h"
#include "net/test/test_data_directory.h"
#include "net/test/test_with_task_environment.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/platform_test.h"
#include "third_party/boringssl/src/include/openssl/bio.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/pem.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"
#include "url/gurl.h"
#include "url/origin.h"

using net::test::IsError;
using net::test::IsOk;

using testing::_;
using testing::AnyOf;
using testing::Return;
using testing::Truly;

namespace net {

class NetLogWithSource;

namespace {

// When passed to |MakeHashValueVector|, this will generate a key pin that is
// sha256/AA...=, and hence will cause pin validation success with the TestSPKI
// pin from transport_security_state_static.pins. ("A" is the 0th element of the
// base-64 alphabet.)
const uint8_t kGoodHashValueVectorInput = 0;

// When passed to |MakeHashValueVector|, this will generate a key pin that is
// not sha256/AA...=, and hence will cause pin validation failure with the
// TestSPKI pin.
const uint8_t kBadHashValueVectorInput = 3;

// ReadBufferingStreamSocket is a wrapper for an existing StreamSocket that
// will ensure a certain amount of data is internally buffered before
// satisfying a Read() request. It exists to mimic OS-level internal
// buffering, but in a way to guarantee that X number of bytes will be
// returned to callers of Read(), regardless of how quickly the OS receives
// them from the TestServer.
class ReadBufferingStreamSocket : public WrappedStreamSocket {
 public:
  explicit ReadBufferingStreamSocket(std::unique_ptr<StreamSocket> transport);
  ~ReadBufferingStreamSocket() override = default;

  // Socket implementation:
  int Read(IOBuffer* buf,
           int buf_len,
           CompletionOnceCallback callback) override;

  int ReadIfReady(IOBuffer* buf,
                  int buf_len,
                  CompletionOnceCallback callback) override;

  // Sets the internal buffer to |size|. This must not be greater than
  // the largest value supplied to Read() - that is, it does not handle
  // having "leftovers" at the end of Read().
  // Each call to Read() will be prevented from completion until at least
  // |size| data has been read.
  // Set to 0 to turn off buffering, causing Read() to transparently
  // read via the underlying transport.
  void SetBufferSize(int size);

 private:
  enum State {
    STATE_NONE,
    STATE_READ,
    STATE_READ_COMPLETE,
  };

  int DoLoop(int result);
  int DoRead();
  int DoReadComplete(int result);
  void OnReadCompleted(int result);

  State state_;
  scoped_refptr<GrowableIOBuffer> read_buffer_;
  int buffer_size_;

  scoped_refptr<IOBuffer> user_read_buf_;
  CompletionOnceCallback user_read_callback_;
};

ReadBufferingStreamSocket::ReadBufferingStreamSocket(
    std::unique_ptr<StreamSocket> transport)
    : WrappedStreamSocket(std::move(transport)),
      read_buffer_(base::MakeRefCounted<GrowableIOBuffer>()),
      buffer_size_(0) {}

void ReadBufferingStreamSocket::SetBufferSize(int size) {
  DCHECK(!user_read_buf_);
  buffer_size_ = size;
  read_buffer_->SetCapacity(size);
}

int ReadBufferingStreamSocket::Read(IOBuffer* buf,
                                    int buf_len,
                                    CompletionOnceCallback callback) {
  DCHECK(!user_read_buf_);
  if (buffer_size_ == 0)
    return transport_->Read(buf, buf_len, std::move(callback));
  int rv = ReadIfReady(buf, buf_len, std::move(callback));
  if (rv == ERR_IO_PENDING)
    user_read_buf_ = buf;
  return rv;
}

int ReadBufferingStreamSocket::ReadIfReady(IOBuffer* buf,
                                           int buf_len,
                                           CompletionOnceCallback callback) {
  DCHECK(!user_read_buf_);
  if (buffer_size_ == 0)
    return transport_->ReadIfReady(buf, buf_len, std::move(callback));

  if (read_buffer_->RemainingCapacity() == 0) {
    memcpy(buf->data(), read_buffer_->StartOfBuffer(),
           read_buffer_->capacity());
    read_buffer_->set_offset(0);
    return read_buffer_->capacity();
  }

  if (buf_len < buffer_size_)
    return ERR_UNEXPECTED;

  state_ = STATE_READ;
  int rv = DoLoop(OK);
  if (rv == ERR_IO_PENDING)
    user_read_callback_ = std::move(callback);
  return rv;
}

int ReadBufferingStreamSocket::DoLoop(int result) {
  int rv = result;
  do {
    State current_state = state_;
    state_ = STATE_NONE;
    switch (current_state) {
      case STATE_READ:
        rv = DoRead();
        break;
      case STATE_READ_COMPLETE:
        rv = DoReadComplete(rv);
        break;
      case STATE_NONE:
      default:
        NOTREACHED() << "Unexpected state: " << current_state;
        rv = ERR_UNEXPECTED;
        break;
    }
  } while (rv != ERR_IO_PENDING && state_ != STATE_NONE);
  return rv;
}

int ReadBufferingStreamSocket::DoRead() {
  state_ = STATE_READ_COMPLETE;
  return transport_->Read(
      read_buffer_.get(), read_buffer_->RemainingCapacity(),
      base::BindOnce(&ReadBufferingStreamSocket::OnReadCompleted,
                     base::Unretained(this)));
}

int ReadBufferingStreamSocket::DoReadComplete(int result) {
  state_ = STATE_NONE;

  if (result <= 0)
    return result;

  read_buffer_->set_offset(read_buffer_->offset() + result);
  if (read_buffer_->RemainingCapacity() > 0) {
    state_ = STATE_READ;
    return OK;
  }

  // If ReadIfReady() is called by the user and this is an asynchronous
  // completion, notify the user that read can be retried.
  if (user_read_buf_ == nullptr)
    return OK;

  memcpy(user_read_buf_->data(),
         read_buffer_->StartOfBuffer(),
         read_buffer_->capacity());
  read_buffer_->set_offset(0);
  return read_buffer_->capacity();
}

void ReadBufferingStreamSocket::OnReadCompleted(int result) {
  DCHECK_NE(ERR_IO_PENDING, result);
  DCHECK(user_read_callback_);

  result = DoLoop(result);
  if (result == ERR_IO_PENDING)
    return;
  user_read_buf_ = nullptr;
  std::move(user_read_callback_).Run(result);
}

// Simulates synchronously receiving an error during Read() or Write()
class SynchronousErrorStreamSocket : public WrappedStreamSocket {
 public:
  explicit SynchronousErrorStreamSocket(std::unique_ptr<StreamSocket> transport)
      : WrappedStreamSocket(std::move(transport)) {}
  ~SynchronousErrorStreamSocket() override = default;

  // Socket implementation:
  int Read(IOBuffer* buf,
           int buf_len,
           CompletionOnceCallback callback) override;
  int ReadIfReady(IOBuffer* buf,
                  int buf_len,
                  CompletionOnceCallback callback) override;
  int Write(IOBuffer* buf,
            int buf_len,
            CompletionOnceCallback callback,
            const NetworkTrafficAnnotationTag& traffic_annotation) override;

  // Sets the next Read() call and all future calls to return |error|.
  // If there is already a pending asynchronous read, the configured error
  // will not be returned until that asynchronous read has completed and Read()
  // is called again.
  void SetNextReadError(int error) {
    DCHECK_GE(0, error);
    have_read_error_ = true;
    pending_read_error_ = error;
  }

  // Sets the next Write() call and all future calls to return |error|.
  // If there is already a pending asynchronous write, the configured error
  // will not be returned until that asynchronous write has completed and
  // Write() is called again.
  void SetNextWriteError(int error) {
    DCHECK_GE(0, error);
    have_write_error_ = true;
    pending_write_error_ = error;
  }

 private:
  bool have_read_error_ = false;
  int pending_read_error_ = OK;

  bool have_write_error_ = false;
  int pending_write_error_ = OK;

  DISALLOW_COPY_AND_ASSIGN(SynchronousErrorStreamSocket);
};

int SynchronousErrorStreamSocket::Read(IOBuffer* buf,
                                       int buf_len,
                                       CompletionOnceCallback callback) {
  if (have_read_error_)
    return pending_read_error_;
  return transport_->Read(buf, buf_len, std::move(callback));
}

int SynchronousErrorStreamSocket::ReadIfReady(IOBuffer* buf,
                                              int buf_len,
                                              CompletionOnceCallback callback) {
  if (have_read_error_)
    return pending_read_error_;
  return transport_->ReadIfReady(buf, buf_len, std::move(callback));
}

int SynchronousErrorStreamSocket::Write(
    IOBuffer* buf,
    int buf_len,
    CompletionOnceCallback callback,
    const NetworkTrafficAnnotationTag& traffic_annotation) {
  if (have_write_error_)
    return pending_write_error_;
  return transport_->Write(buf, buf_len, std::move(callback),
                           traffic_annotation);
}

// FakeBlockingStreamSocket wraps an existing StreamSocket and simulates the
// underlying transport needing to complete things asynchronously in a
// deterministic manner (e.g.: independent of the TestServer and the OS's
// semantics).
class FakeBlockingStreamSocket : public WrappedStreamSocket {
 public:
  explicit FakeBlockingStreamSocket(std::unique_ptr<StreamSocket> transport)
      : WrappedStreamSocket(std::move(transport)) {}
  ~FakeBlockingStreamSocket() override = default;

  // Socket implementation:
  int Read(IOBuffer* buf,
           int buf_len,
           CompletionOnceCallback callback) override;
  int ReadIfReady(IOBuffer* buf,
                  int buf_len,
                  CompletionOnceCallback callback) override;
  int Write(IOBuffer* buf,
            int buf_len,
            CompletionOnceCallback callback,
            const NetworkTrafficAnnotationTag& traffic_annotation) override;

  int pending_read_result() const { return pending_read_result_; }
  IOBuffer* pending_read_buf() const { return pending_read_buf_.get(); }

  // Blocks read results on the socket. Reads will not complete until
  // UnblockReadResult() has been called and a result is ready from the
  // underlying transport. Note: if BlockReadResult() is called while there is a
  // hanging asynchronous Read(), that Read is blocked.
  void BlockReadResult();
  void UnblockReadResult();

  // Replaces the pending read with |data|. Returns true on success or false if
  // the caller's reads were too small.
  bool ReplaceReadResult(const std::string& data);

  // Waits for the blocked Read() call to be complete at the underlying
  // transport.
  void WaitForReadResult();

  // Causes the next call to Write() to return ERR_IO_PENDING, not beginning the
  // underlying transport until UnblockWrite() has been called. Note: if there
  // is a pending asynchronous write, it is NOT blocked. For purposes of
  // blocking writes, data is considered to have reached the underlying
  // transport as soon as Write() is called.
  void BlockWrite();
  void UnblockWrite();

  // Waits for the blocked Write() call to be scheduled.
  void WaitForWrite();

 private:
  // Handles completion from the underlying transport read.
  void OnReadCompleted(int result);

  // Handles async completion of ReadIfReady().
  void CompleteReadIfReady(scoped_refptr<IOBuffer> buffer, int rv);

  // Finishes the current read.
  void ReturnReadResult();

  // Callback for writes.
  void CallPendingWriteCallback(int result);

  // True if read callbacks are blocked.
  bool should_block_read_ = false;

  // Used to buffer result returned by a completed ReadIfReady().
  std::string read_if_ready_buf_;

  // Non-null if there is a pending ReadIfReady().
  CompletionOnceCallback read_if_ready_callback_;

  // The buffer for the pending read, or NULL if not consumed.
  scoped_refptr<IOBuffer> pending_read_buf_;

  // The size of the pending read buffer, or -1 if not set.
  int pending_read_buf_len_ = -1;

  // The user callback for the pending read call.
  CompletionOnceCallback pending_read_callback_;

  // The result for the blocked read callback, or ERR_IO_PENDING if not
  // completed.
  int pending_read_result_ = ERR_IO_PENDING;

  // WaitForReadResult() wait loop.
  std::unique_ptr<base::RunLoop> read_loop_;

  // True if write calls are blocked.
  bool should_block_write_ = false;

  // The buffer for the pending write, or NULL if not scheduled.
  scoped_refptr<IOBuffer> pending_write_buf_;

  // The callback for the pending write call.
  CompletionOnceCallback pending_write_callback_;

  // The length for the pending write, or -1 if not scheduled.
  int pending_write_len_ = -1;

  // WaitForWrite() wait loop.
  std::unique_ptr<base::RunLoop> write_loop_;
};

int FakeBlockingStreamSocket::Read(IOBuffer* buf,
                                   int len,
                                   CompletionOnceCallback callback) {
  DCHECK(!pending_read_buf_);
  DCHECK(pending_read_callback_.is_null());
  DCHECK_EQ(ERR_IO_PENDING, pending_read_result_);
  DCHECK(!callback.is_null());

  int rv = transport_->Read(
      buf, len,
      base::BindOnce(&FakeBlockingStreamSocket::OnReadCompleted,
                     base::Unretained(this)));
  if (rv == ERR_IO_PENDING || should_block_read_) {
    // Save the callback to be called later.
    pending_read_buf_ = buf;
    pending_read_buf_len_ = len;
    pending_read_callback_ = std::move(callback);
    // Save the read result.
    if (rv != ERR_IO_PENDING) {
      OnReadCompleted(rv);
      rv = ERR_IO_PENDING;
    }
  }
  return rv;
}

int FakeBlockingStreamSocket::ReadIfReady(IOBuffer* buf,
                                          int len,
                                          CompletionOnceCallback callback) {
  if (!read_if_ready_buf_.empty()) {
    // If ReadIfReady() is used, asynchronous reads with a large enough buffer
    // and no BlockReadResult() are supported by this class. Explicitly check
    // that |should_block_read_| doesn't apply and |len| is greater than the
    // size of the buffered data.
    CHECK(!should_block_read_);
    CHECK_GE(len, static_cast<int>(read_if_ready_buf_.size()));
    int rv = read_if_ready_buf_.size();
    memcpy(buf->data(), read_if_ready_buf_.data(), rv);
    read_if_ready_buf_.clear();
    return rv;
  }
  scoped_refptr<IOBuffer> buf_copy = base::MakeRefCounted<IOBuffer>(len);
  int rv = Read(buf_copy.get(), len,
                base::BindOnce(&FakeBlockingStreamSocket::CompleteReadIfReady,
                               base::Unretained(this), buf_copy));
  if (rv > 0)
    memcpy(buf->data(), buf_copy->data(), rv);
  if (rv == ERR_IO_PENDING)
    read_if_ready_callback_ = std::move(callback);
  return rv;
}

int FakeBlockingStreamSocket::Write(
    IOBuffer* buf,
    int len,
    CompletionOnceCallback callback,
    const NetworkTrafficAnnotationTag& traffic_annotation) {
  DCHECK(buf);
  DCHECK_LE(0, len);

  if (!should_block_write_)
    return transport_->Write(buf, len, std::move(callback), traffic_annotation);

  // Schedule the write, but do nothing.
  DCHECK(!pending_write_buf_.get());
  DCHECK_EQ(-1, pending_write_len_);
  DCHECK(pending_write_callback_.is_null());
  DCHECK(!callback.is_null());
  pending_write_buf_ = buf;
  pending_write_len_ = len;
  pending_write_callback_ = std::move(callback);

  // Stop the write loop, if any.
  if (write_loop_)
    write_loop_->Quit();
  return ERR_IO_PENDING;
}

void FakeBlockingStreamSocket::BlockReadResult() {
  DCHECK(!should_block_read_);
  should_block_read_ = true;
}

void FakeBlockingStreamSocket::UnblockReadResult() {
  DCHECK(should_block_read_);
  should_block_read_ = false;

  // If the operation has since completed, return the result to the caller.
  if (pending_read_result_ != ERR_IO_PENDING)
    ReturnReadResult();
}

bool FakeBlockingStreamSocket::ReplaceReadResult(const std::string& data) {
  DCHECK(should_block_read_);
  DCHECK_NE(ERR_IO_PENDING, pending_read_result_);
  DCHECK(pending_read_buf_);
  DCHECK_NE(-1, pending_read_buf_len_);

  if (static_cast<size_t>(pending_read_buf_len_) < data.size())
    return false;

  memcpy(pending_read_buf_->data(), data.data(), data.size());
  pending_read_result_ = data.size();
  return true;
}

void FakeBlockingStreamSocket::WaitForReadResult() {
  DCHECK(should_block_read_);
  DCHECK(!read_loop_);

  if (pending_read_result_ != ERR_IO_PENDING)
    return;
  read_loop_.reset(new base::RunLoop);
  read_loop_->Run();
  read_loop_.reset();
  DCHECK_NE(ERR_IO_PENDING, pending_read_result_);
}

void FakeBlockingStreamSocket::BlockWrite() {
  DCHECK(!should_block_write_);
  should_block_write_ = true;
}

void FakeBlockingStreamSocket::CallPendingWriteCallback(int rv) {
  std::move(pending_write_callback_).Run(rv);
}

void FakeBlockingStreamSocket::UnblockWrite() {
  DCHECK(should_block_write_);
  should_block_write_ = false;

  // Do nothing if UnblockWrite() was called after BlockWrite(),
  // without a Write() in between.
  if (!pending_write_buf_.get())
    return;

  int rv = transport_->Write(
      pending_write_buf_.get(), pending_write_len_,
      base::BindOnce(&FakeBlockingStreamSocket::CallPendingWriteCallback,
                     base::Unretained(this)),
      TRAFFIC_ANNOTATION_FOR_TESTS);

  pending_write_buf_ = nullptr;
  pending_write_len_ = -1;
  if (rv != ERR_IO_PENDING) {
    std::move(pending_write_callback_).Run(rv);
  }
}

void FakeBlockingStreamSocket::WaitForWrite() {
  DCHECK(should_block_write_);
  DCHECK(!write_loop_);

  if (pending_write_buf_.get())
    return;
  write_loop_.reset(new base::RunLoop);
  write_loop_->Run();
  write_loop_.reset();
  DCHECK(pending_write_buf_.get());
}

void FakeBlockingStreamSocket::OnReadCompleted(int result) {
  DCHECK_EQ(ERR_IO_PENDING, pending_read_result_);
  DCHECK(!pending_read_callback_.is_null());

  pending_read_result_ = result;

  if (should_block_read_) {
    // Defer the result until UnblockReadResult is called.
    if (read_loop_)
      read_loop_->Quit();
    return;
  }

  ReturnReadResult();
}

void FakeBlockingStreamSocket::CompleteReadIfReady(scoped_refptr<IOBuffer> buf,
                                                   int rv) {
  DCHECK(read_if_ready_callback_);
  DCHECK(read_if_ready_buf_.empty());
  DCHECK(!should_block_read_);
  if (rv > 0)
    read_if_ready_buf_ = std::string(buf->data(), buf->data() + rv);
  std::move(read_if_ready_callback_).Run(rv > 0 ? OK : rv);
}

void FakeBlockingStreamSocket::ReturnReadResult() {
  int result = pending_read_result_;
  pending_read_result_ = ERR_IO_PENDING;
  pending_read_buf_ = nullptr;
  pending_read_buf_len_ = -1;
  std::move(pending_read_callback_).Run(result);
}

// CountingStreamSocket wraps an existing StreamSocket and maintains a count of
// reads and writes on the socket.
class CountingStreamSocket : public WrappedStreamSocket {
 public:
  explicit CountingStreamSocket(std::unique_ptr<StreamSocket> transport)
      : WrappedStreamSocket(std::move(transport)),
        read_count_(0),
        write_count_(0) {}
  ~CountingStreamSocket() override = default;

  // Socket implementation:
  int Read(IOBuffer* buf,
           int buf_len,
           CompletionOnceCallback callback) override {
    read_count_++;
    return transport_->Read(buf, buf_len, std::move(callback));
  }
  int Write(IOBuffer* buf,
            int buf_len,
            CompletionOnceCallback callback,
            const NetworkTrafficAnnotationTag& traffic_annotation) override {
    write_count_++;
    return transport_->Write(buf, buf_len, std::move(callback),
                             traffic_annotation);
  }

  int read_count() const { return read_count_; }
  int write_count() const { return write_count_; }

 private:
  int read_count_;
  int write_count_;
};

// A helper class that will delete |socket| when the callback is invoked.
class DeleteSocketCallback : public TestCompletionCallbackBase {
 public:
  explicit DeleteSocketCallback(StreamSocket* socket) : socket_(socket) {}
  ~DeleteSocketCallback() override = default;

  CompletionOnceCallback callback() {
    return base::BindOnce(&DeleteSocketCallback::OnComplete,
                          base::Unretained(this));
  }

 private:
  void OnComplete(int result) {
    if (socket_) {
      delete socket_;
      socket_ = nullptr;
    } else {
      ADD_FAILURE() << "Deleting socket twice";
    }
    SetResult(result);
  }

  StreamSocket* socket_;

  DISALLOW_COPY_AND_ASSIGN(DeleteSocketCallback);
};

// A mock ExpectCTReporter that remembers the latest violation that was
// reported and the number of violations reported.
class MockExpectCTReporter : public TransportSecurityState::ExpectCTReporter {
 public:
  MockExpectCTReporter() : num_failures_(0) {}
  ~MockExpectCTReporter() override = default;

  void OnExpectCTFailed(const HostPortPair& host_port_pair,
                        const GURL& report_uri,
                        base::Time expiration,
                        const X509Certificate* validated_certificate_chain,
                        const X509Certificate* served_certificate_chain,
                        const SignedCertificateTimestampAndStatusList&
                            signed_certificate_timestamps) override {
    num_failures_++;
    host_port_pair_ = host_port_pair;
    report_uri_ = report_uri;
    served_certificate_chain_ = served_certificate_chain;
    validated_certificate_chain_ = validated_certificate_chain;
    signed_certificate_timestamps_ = signed_certificate_timestamps;
  }

  const HostPortPair& host_port_pair() { return host_port_pair_; }
  const GURL& report_uri() { return report_uri_; }
  uint32_t num_failures() { return num_failures_; }
  const X509Certificate* served_certificate_chain() {
    return served_certificate_chain_;
  }
  const X509Certificate* validated_certificate_chain() {
    return validated_certificate_chain_;
  }
  const SignedCertificateTimestampAndStatusList&
  signed_certificate_timestamps() {
    return signed_certificate_timestamps_;
  }

 private:
  HostPortPair host_port_pair_;
  GURL report_uri_;
  uint32_t num_failures_;
  const X509Certificate* served_certificate_chain_;
  const X509Certificate* validated_certificate_chain_;
  SignedCertificateTimestampAndStatusList signed_certificate_timestamps_;
};

// A mock CTVerifier that records every call to Verify but doesn't verify
// anything.
class MockCTVerifier : public CTVerifier {
 public:
  MOCK_METHOD6(Verify,
               void(base::StringPiece,
                    X509Certificate*,
                    base::StringPiece,
                    base::StringPiece,
                    SignedCertificateTimestampAndStatusList*,
                    const NetLogWithSource&));
};

// A mock CTPolicyEnforcer that returns a custom verification result.
class MockCTPolicyEnforcer : public CTPolicyEnforcer {
 public:
  MOCK_METHOD3(CheckCompliance,
               ct::CTPolicyCompliance(X509Certificate* cert,
                                      const ct::SCTList&,
                                      const NetLogWithSource&));
};

class MockRequireCTDelegate : public TransportSecurityState::RequireCTDelegate {
 public:
  MOCK_METHOD3(IsCTRequiredForHost,
               CTRequirementLevel(const std::string& host,
                                  const X509Certificate* chain,
                                  const HashValueVector& hashes));
};

class ManySmallRecordsHttpResponse : public test_server::HttpResponse {
 public:
  static std::unique_ptr<test_server::HttpResponse> HandleRequest(
      const test_server::HttpRequest& request) {
    if (request.relative_url != "/ssl-many-small-records") {
      return nullptr;
    }

    // Write ~26K of data, in 1350 byte chunks
    return std::make_unique<ManySmallRecordsHttpResponse>(/*chunk_size=*/1350,
                                                          /*chunk_count=*/20);
  }

  ManySmallRecordsHttpResponse(size_t chunk_size, size_t chunk_count)
      : chunk_size_(chunk_size), chunk_count_(chunk_count) {}

  void SendResponse(const test_server::SendBytesCallback& send,
                    test_server::SendCompleteCallback done) override {
    std::string headers = base::StringPrintf(
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Length: %zu\r\n"
        "Content-Type: text/plain\r\n\r\n",
        chunk_size_ * chunk_count_);
    send.Run(headers, base::BindOnce(&SendChunks, chunk_size_, chunk_count_,
                                     send, std::move(done)));
  }

 private:
  static void SendChunks(size_t chunk_size,
                         size_t chunk_count,
                         const test_server::SendBytesCallback& send,
                         test_server::SendCompleteCallback done) {
    if (chunk_count == 0) {
      std::move(done).Run();
      return;
    }

    std::string chunk(chunk_size, '*');
    // This assumes that splitting output into separate |send| calls will
    // produce separate TLS records.
    send.Run(chunk, base::BindOnce(&SendChunks, chunk_size, chunk_count - 1,
                                   send, std::move(done)));
  }

  size_t chunk_size_;
  size_t chunk_count_;
};

class SSLClientSocketTest : public PlatformTest, public WithTaskEnvironment {
 public:
  SSLClientSocketTest()
      : socket_factory_(ClientSocketFactory::GetDefaultFactory()),
        ssl_config_service_(
            std::make_unique<TestSSLConfigService>(SSLContextConfig())),
        cert_verifier_(std::make_unique<MockCertVerifier>()),
        transport_security_state_(std::make_unique<TransportSecurityState>()),
        ct_verifier_(std::make_unique<DoNothingCTVerifier>()),
        ct_policy_enforcer_(std::make_unique<MockCTPolicyEnforcer>()),
        ssl_client_session_cache_(std::make_unique<SSLClientSessionCache>(
            SSLClientSessionCache::Config())),
        context_(std::make_unique<SSLClientContext>(
            ssl_config_service_.get(),
            cert_verifier_.get(),
            transport_security_state_.get(),
            ct_verifier_.get(),
            ct_policy_enforcer_.get(),
            ssl_client_session_cache_.get())) {
    cert_verifier_->set_default_result(OK);
    cert_verifier_->set_async(true);

    EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(_, _, _))
        .WillRepeatedly(
            Return(ct::CTPolicyCompliance::CT_POLICY_COMPLIES_VIA_SCTS));
  }

 protected:
  // The address of the test server, after calling StartEmbeddedTestServer() or
  // StartTestServer().
  const AddressList& addr() const { return addr_; }

  // The hostname of the test server, after calling StartEmbeddedTestServer() or
  // StartTestServer().
  const HostPortPair& host_port_pair() const { return host_port_pair_; }

  // The EmbeddedTestServer object, after calling StartEmbeddedTestServer().
  EmbeddedTestServer* embedded_test_server() {
    return embedded_test_server_.get();
  }

  // The SpawnedTestServer object, after calling StartTestServer().
  const SpawnedTestServer* spawned_test_server() const {
    return spawned_test_server_.get();
  }

  // Starts the embedded test server with the specified parameters. Returns true
  // on success.
  bool StartEmbeddedTestServer(EmbeddedTestServer::ServerCertificate cert,
                               const SSLServerConfig& server_config) {
    spawned_test_server_ = nullptr;
    embedded_test_server_ =
        std::make_unique<EmbeddedTestServer>(EmbeddedTestServer::TYPE_HTTPS);
    RegisterEmbeddedTestServerHandlers(embedded_test_server_.get());
    embedded_test_server_->SetSSLConfig(cert, server_config);
    if (!embedded_test_server_->Start()) {
      LOG(ERROR) << "Could not start EmbeddedTestServer";
      return false;
    }

    if (!embedded_test_server_->GetAddressList(&addr_)) {
      LOG(ERROR) << "Could not get EmbeddedTestServer address list";
      return false;
    }
    host_port_pair_ = embedded_test_server_->host_port_pair();
    return true;
  }

  // May be overridden by the subclass to customize the EmbeddedTestServer.
  virtual void RegisterEmbeddedTestServerHandlers(EmbeddedTestServer* server) {
    server->AddDefaultHandlers(base::FilePath());
    server->RegisterRequestHandler(
        base::BindRepeating(&ManySmallRecordsHttpResponse::HandleRequest));
  }

  // Starts the spawned test server with SSL configuration |ssl_options|.
  // Returns true on success. Prefer StartEmbeddedTestServer().
  bool StartTestServer(const SpawnedTestServer::SSLOptions& ssl_options) {
    embedded_test_server_ = nullptr;
    spawned_test_server_ = std::make_unique<SpawnedTestServer>(
        SpawnedTestServer::TYPE_HTTPS, ssl_options, base::FilePath());
    if (!spawned_test_server_->Start()) {
      LOG(ERROR) << "Could not start SpawnedTestServer";
      return false;
    }

    if (!spawned_test_server_->GetAddressList(&addr_)) {
      LOG(ERROR) << "Could not get SpawnedTestServer address list";
      return false;
    }
    host_port_pair_ = spawned_test_server_->host_port_pair();
    return true;
  }

  std::unique_ptr<SSLClientSocket> CreateSSLClientSocket(
      std::unique_ptr<StreamSocket> transport_socket,
      const HostPortPair& host_and_port,
      const SSLConfig& ssl_config) {
    return socket_factory_->CreateSSLClientSocket(
        context_.get(), std::move(transport_socket), host_and_port, ssl_config);
  }

  // Create an SSLClientSocket object and use it to connect to a test server,
  // then wait for connection results. This must be called after a successful
  // StartTestServer() call.
  //
  // |ssl_config| The SSL configuration to use.
  // |host_port_pair| The hostname and port to use at the SSL layer. (The
  //     socket connection will still be made to |spawned_test_server_|.)
  // |result| will retrieve the ::Connect() result value.
  //
  // Returns true on success, false otherwise. Success means that the SSL
  // socket could be created and its Connect() was called, not that the
  // connection itself was a success.
  bool CreateAndConnectSSLClientSocketWithHost(
      const SSLConfig& ssl_config,
      const HostPortPair& host_port_pair,
      int* result) {
    std::unique_ptr<StreamSocket> transport(
        new TCPClientSocket(addr_, nullptr, &log_, NetLogSource()));
    int rv = callback_.GetResult(transport->Connect(callback_.callback()));
    if (rv != OK) {
      LOG(ERROR) << "Could not connect to SpawnedTestServer";
      return false;
    }

    sock_ =
        CreateSSLClientSocket(std::move(transport), host_port_pair, ssl_config);
    EXPECT_FALSE(sock_->IsConnected());

    *result = callback_.GetResult(sock_->Connect(callback_.callback()));
    return true;
  }

  bool CreateAndConnectSSLClientSocket(const SSLConfig& ssl_config,
                                       int* result) {
    return CreateAndConnectSSLClientSocketWithHost(ssl_config, host_port_pair(),
                                                   result);
  }

  // Adds the server certificate with provided cert status.
  // Must be called after StartTestServer has been called.
  void AddServerCertStatusToSSLConfig(CertStatus status,
                                      SSLConfig* ssl_config) {
    ASSERT_TRUE(spawned_test_server() || embedded_test_server());
    // Find out the certificate the server is using.
    scoped_refptr<X509Certificate> server_cert;
    if (spawned_test_server()) {
      server_cert = spawned_test_server()->GetCertificate();
    } else {
      server_cert = embedded_test_server()->GetCertificate();
    }
    // Get the MockCertVerifier to verify it as an EV cert.
    CertVerifyResult verify_result;
    verify_result.cert_status = status;
    verify_result.verified_cert = server_cert;
    cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);
  }

  RecordingTestNetLog log_;
  ClientSocketFactory* socket_factory_;
  std::unique_ptr<TestSSLConfigService> ssl_config_service_;
  std::unique_ptr<MockCertVerifier> cert_verifier_;
  std::unique_ptr<TransportSecurityState> transport_security_state_;
  std::unique_ptr<DoNothingCTVerifier> ct_verifier_;
  std::unique_ptr<MockCTPolicyEnforcer> ct_policy_enforcer_;
  std::unique_ptr<SSLClientSessionCache> ssl_client_session_cache_;
  std::unique_ptr<SSLClientContext> context_;
  std::unique_ptr<SSLClientSocket> sock_;

 private:
  std::unique_ptr<SpawnedTestServer> spawned_test_server_;
  std::unique_ptr<EmbeddedTestServer> embedded_test_server_;
  TestCompletionCallback callback_;
  AddressList addr_;
  HostPortPair host_port_pair_;
};

enum ReadIfReadyTransport {
  // ReadIfReady() is implemented by the underlying transport.
  READ_IF_READY_SUPPORTED,
  // ReadIfReady() is not implemented by the underlying transport.
  READ_IF_READY_NOT_SUPPORTED,
};

enum ReadIfReadySSL {
  // Test reads by calling ReadIfReady() on the SSL socket.
  TEST_SSL_READ_IF_READY,
  // Test reads by calling Read() on the SSL socket.
  TEST_SSL_READ,
};

class StreamSocketWithoutReadIfReady : public WrappedStreamSocket {
 public:
  explicit StreamSocketWithoutReadIfReady(
      std::unique_ptr<StreamSocket> transport)
      : WrappedStreamSocket(std::move(transport)) {}

  int ReadIfReady(IOBuffer* buf,
                  int buf_len,
                  CompletionOnceCallback callback) override {
    return ERR_READ_IF_READY_NOT_IMPLEMENTED;
  }

  int CancelReadIfReady() override { return ERR_READ_IF_READY_NOT_IMPLEMENTED; }
};

class ClientSocketFactoryWithoutReadIfReady : public ClientSocketFactory {
 public:
  explicit ClientSocketFactoryWithoutReadIfReady(ClientSocketFactory* factory)
      : factory_(factory) {}

  std::unique_ptr<DatagramClientSocket> CreateDatagramClientSocket(
      DatagramSocket::BindType bind_type,
      NetLog* net_log,
      const NetLogSource& source) override {
    return factory_->CreateDatagramClientSocket(bind_type, net_log, source);
  }

  std::unique_ptr<TransportClientSocket> CreateTransportClientSocket(
      const AddressList& addresses,
      std::unique_ptr<SocketPerformanceWatcher> socket_performance_watcher,
      NetLog* net_log,
      const NetLogSource& source) override {
    return factory_->CreateTransportClientSocket(
        addresses, std::move(socket_performance_watcher), net_log, source);
  }

  std::unique_ptr<SSLClientSocket> CreateSSLClientSocket(
      SSLClientContext* context,
      std::unique_ptr<StreamSocket> stream_socket,
      const HostPortPair& host_and_port,
      const SSLConfig& ssl_config) override {
    stream_socket = std::make_unique<StreamSocketWithoutReadIfReady>(
        std::move(stream_socket));
    return factory_->CreateSSLClientSocket(context, std::move(stream_socket),
                                           host_and_port, ssl_config);
  }

  std::unique_ptr<ProxyClientSocket> CreateProxyClientSocket(
      std::unique_ptr<StreamSocket> stream_socket,
      const std::string& user_agent,
      const HostPortPair& endpoint,
      const ProxyServer& proxy_server,
      HttpAuthController* http_auth_controller,
      bool tunnel,
      bool using_spdy,
      NextProto negotiated_protocol,
      ProxyDelegate* proxy_delegate,
      const NetworkTrafficAnnotationTag& traffic_annotation) override {
    return factory_->CreateProxyClientSocket(
        std::move(stream_socket), user_agent, endpoint, proxy_server,
        http_auth_controller, tunnel, using_spdy, negotiated_protocol,
        proxy_delegate, traffic_annotation);
  }

 private:
  ClientSocketFactory* const factory_;
};

std::vector<uint16_t> GetTLSVersions() {
  return {SSL_PROTOCOL_VERSION_TLS1, SSL_PROTOCOL_VERSION_TLS1_1,
          SSL_PROTOCOL_VERSION_TLS1_2, SSL_PROTOCOL_VERSION_TLS1_3};
}

base::Optional<SpawnedTestServer::SSLOptions::TLSMaxVersion>
ProtocolVersionToSpawnedTestServer(uint16_t version) {
  switch (version) {
    case SSL_PROTOCOL_VERSION_TLS1:
      return SpawnedTestServer::SSLOptions::TLS_MAX_VERSION_TLS1_0;
    case SSL_PROTOCOL_VERSION_TLS1_1:
      return SpawnedTestServer::SSLOptions::TLS_MAX_VERSION_TLS1_1;
    case SSL_PROTOCOL_VERSION_TLS1_2:
      return SpawnedTestServer::SSLOptions::TLS_MAX_VERSION_TLS1_2;
    case SSL_PROTOCOL_VERSION_TLS1_3:
      // SpawnedTestServer does not support TLS 1.3.
      return base::nullopt;
    default:
      ADD_FAILURE() << "Unknown version " << version;
      return base::nullopt;
  }
}

class SSLClientSocketVersionTest
    : public SSLClientSocketTest,
      public ::testing::WithParamInterface<uint16_t> {
 protected:
  uint16_t version() const { return GetParam(); }

  SSLServerConfig GetServerConfig() {
    SSLServerConfig config;
    config.version_max = version();
    config.version_min = version();
    return config;
  }
};

// If GetParam(), try ReadIfReady() and fall back to Read() if needed.
class SSLClientSocketReadTest
    : public SSLClientSocketTest,
      public ::testing::WithParamInterface<
          std::tuple<ReadIfReadyTransport, ReadIfReadySSL, uint16_t>> {
 protected:
  SSLClientSocketReadTest() : SSLClientSocketTest() {
    if (!read_if_ready_supported()) {
      wrapped_socket_factory_ =
          std::make_unique<ClientSocketFactoryWithoutReadIfReady>(
              socket_factory_);
      socket_factory_ = wrapped_socket_factory_.get();
    }
  }

  // Convienient wrapper to call Read()/ReadIfReady() depending on whether
  // ReadyIfReady() is enabled.
  int Read(StreamSocket* socket,
           IOBuffer* buf,
           int buf_len,
           CompletionOnceCallback callback) {
    if (test_ssl_read_if_ready())
      return socket->ReadIfReady(buf, buf_len, std::move(callback));
    return socket->Read(buf, buf_len, std::move(callback));
  }

  // Wait for Read()/ReadIfReady() to complete.
  int WaitForReadCompletion(StreamSocket* socket,
                            IOBuffer* buf,
                            int buf_len,
                            TestCompletionCallback* callback,
                            int rv) {
    if (!test_ssl_read_if_ready())
      return callback->GetResult(rv);
    while (rv == ERR_IO_PENDING) {
      rv = callback->GetResult(rv);
      if (rv != OK)
        return rv;
      rv = socket->ReadIfReady(buf, buf_len, callback->callback());
    }
    return rv;
  }

  // Calls Read()/ReadIfReady() and waits for it to return data.
  int ReadAndWaitForCompletion(StreamSocket* socket,
                               IOBuffer* buf,
                               int buf_len) {
    TestCompletionCallback callback;
    int rv = Read(socket, buf, buf_len, callback.callback());
    return WaitForReadCompletion(socket, buf, buf_len, &callback, rv);
  }

  SSLServerConfig GetServerConfig() {
    SSLServerConfig config;
    config.version_max = version();
    config.version_min = version();
    return config;
  }

  bool test_ssl_read_if_ready() const {
    return std::get<1>(GetParam()) == TEST_SSL_READ_IF_READY;
  }

  bool read_if_ready_supported() const {
    return std::get<0>(GetParam()) == READ_IF_READY_SUPPORTED;
  }

  uint16_t version() const { return std::get<2>(GetParam()); }

 private:
  std::unique_ptr<ClientSocketFactory> wrapped_socket_factory_;
};

INSTANTIATE_TEST_SUITE_P(
    All,
    SSLClientSocketReadTest,
    ::testing::Combine(::testing::Values(READ_IF_READY_SUPPORTED,
                                         READ_IF_READY_NOT_SUPPORTED),
                       ::testing::Values(TEST_SSL_READ_IF_READY, TEST_SSL_READ),
                       ::testing::ValuesIn(GetTLSVersions())));

// Verifies the correctness of GetSSLCertRequestInfo.
class SSLClientSocketCertRequestInfoTest : public SSLClientSocketVersionTest {
 protected:
  // Connects to the test server and returns the SSLCertRequestInfo reported by
  // the socket.
  scoped_refptr<SSLCertRequestInfo> GetCertRequest() {
    int rv;
    if (!CreateAndConnectSSLClientSocket(SSLConfig(), &rv)) {
      return nullptr;
    }
    EXPECT_THAT(rv, IsError(ERR_SSL_CLIENT_AUTH_CERT_NEEDED));

    auto request_info = base::MakeRefCounted<SSLCertRequestInfo>();
    sock_->GetSSLCertRequestInfo(request_info.get());
    sock_->Disconnect();
    EXPECT_FALSE(sock_->IsConnected());
    EXPECT_TRUE(host_port_pair().Equals(request_info->host_and_port));

    return request_info;
  }
};

class SSLClientSocketFalseStartTest : public SSLClientSocketTest {
 protected:
  // Creates an SSLClientSocket with |client_config| attached to a
  // FakeBlockingStreamSocket, returning both in |*out_raw_transport| and
  // |*out_sock|. The FakeBlockingStreamSocket is owned by the SSLClientSocket,
  // so |*out_raw_transport| is a raw pointer.
  //
  // The client socket will begin a connect using |callback| but stop before the
  // server's finished message is received. The finished message will be blocked
  // in |*out_raw_transport|. To complete the handshake and successfully read
  // data, the caller must unblock reads on |*out_raw_transport|. (Note that, if
  // the client successfully false started, |callback.WaitForResult()| will
  // return OK without unblocking transport reads. But Read() will still block.)
  //
  // Must be called after StartTestServer is called.
  void CreateAndConnectUntilServerFinishedReceived(
      const SSLConfig& client_config,
      TestCompletionCallback* callback,
      FakeBlockingStreamSocket** out_raw_transport,
      std::unique_ptr<SSLClientSocket>* out_sock) {
    CHECK(spawned_test_server());

    std::unique_ptr<StreamSocket> real_transport(
        new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
    std::unique_ptr<FakeBlockingStreamSocket> transport(
        new FakeBlockingStreamSocket(std::move(real_transport)));
    int rv = callback->GetResult(transport->Connect(callback->callback()));
    EXPECT_THAT(rv, IsOk());

    FakeBlockingStreamSocket* raw_transport = transport.get();
    std::unique_ptr<SSLClientSocket> sock = CreateSSLClientSocket(
        std::move(transport), spawned_test_server()->host_port_pair(),
        client_config);

    // Connect. Stop before the client processes the first server leg
    // (ServerHello, etc.)
    raw_transport->BlockReadResult();
    rv = sock->Connect(callback->callback());
    EXPECT_THAT(rv, IsError(ERR_IO_PENDING));
    raw_transport->WaitForReadResult();

    // Release the ServerHello and wait for the client to write
    // ClientKeyExchange, etc. (A proxy for waiting for the entirety of the
    // server's leg to complete, since it may span multiple reads.)
    EXPECT_FALSE(callback->have_result());
    raw_transport->BlockWrite();
    raw_transport->UnblockReadResult();
    raw_transport->WaitForWrite();

    // And, finally, release that and block the next server leg
    // (ChangeCipherSpec, Finished).
    raw_transport->BlockReadResult();
    raw_transport->UnblockWrite();

    *out_raw_transport = raw_transport;
    *out_sock = std::move(sock);
  }

  void TestFalseStart(const SpawnedTestServer::SSLOptions& server_options,
                      const SSLConfig& client_config,
                      bool expect_false_start) {
    ASSERT_TRUE(StartTestServer(server_options));

    TestCompletionCallback callback;
    FakeBlockingStreamSocket* raw_transport = nullptr;
    std::unique_ptr<SSLClientSocket> sock;
    ASSERT_NO_FATAL_FAILURE(CreateAndConnectUntilServerFinishedReceived(
        client_config, &callback, &raw_transport, &sock));

    if (expect_false_start) {
      // When False Starting, the handshake should complete before receiving the
      // Change Cipher Spec and Finished messages.
      //
      // Note: callback.have_result() may not be true without waiting. The NSS
      // state machine sometimes lives on a separate thread, so this thread may
      // not yet have processed the signal that the handshake has completed.
      int rv = callback.WaitForResult();
      EXPECT_THAT(rv, IsOk());
      EXPECT_TRUE(sock->IsConnected());

      const char request_text[] = "GET / HTTP/1.0\r\n\r\n";
      static const int kRequestTextSize =
          static_cast<int>(base::size(request_text) - 1);
      scoped_refptr<IOBuffer> request_buffer =
          base::MakeRefCounted<IOBuffer>(kRequestTextSize);
      memcpy(request_buffer->data(), request_text, kRequestTextSize);

      // Write the request.
      rv = callback.GetResult(sock->Write(request_buffer.get(),
                                          kRequestTextSize, callback.callback(),
                                          TRAFFIC_ANNOTATION_FOR_TESTS));
      EXPECT_EQ(kRequestTextSize, rv);

      // The read will hang; it's waiting for the peer to complete the
      // handshake, and the handshake is still blocked.
      scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
      rv = sock->Read(buf.get(), 4096, callback.callback());

      // After releasing reads, the connection proceeds.
      raw_transport->UnblockReadResult();
      rv = callback.GetResult(rv);
      EXPECT_LT(0, rv);
    } else {
      // False Start is not enabled, so the handshake will not complete because
      // the server second leg is blocked.
      base::RunLoop().RunUntilIdle();
      EXPECT_FALSE(callback.have_result());
    }
  }
};

// Sends an HTTP request on the socket and reads the response. This may be used
// to ensure some data has been consumed from the server.
int MakeHTTPRequest(StreamSocket* socket) {
  base::StringPiece request = "GET / HTTP/1.0\r\n\r\n";
  TestCompletionCallback callback;
  while (!request.empty()) {
    auto request_buffer =
        base::MakeRefCounted<StringIOBuffer>(request.as_string());
    int rv = callback.GetResult(
        socket->Write(request_buffer.get(), request_buffer->size(),
                      callback.callback(), TRAFFIC_ANNOTATION_FOR_TESTS));
    if (rv < 0) {
      return rv;
    }
    request = request.substr(rv);
  }

  auto response_buffer = base::MakeRefCounted<IOBuffer>(1024);
  int rv = callback.GetResult(
      socket->Read(response_buffer.get(), 1024, callback.callback()));
  if (rv < 0) {
    return rv;
  }
  return OK;
}

// Provides a response to the 0RTT request indicating whether it was received
// as early data.
class ZeroRTTResponse : public test_server::HttpResponse {
 public:
  ZeroRTTResponse(bool zero_rtt) : zero_rtt_(zero_rtt) {}
  ~ZeroRTTResponse() override {}

  void SendResponse(const test_server::SendBytesCallback& send,
                    test_server::SendCompleteCallback done) override {
    std::string response;
    if (zero_rtt_) {
      response = "1";
    } else {
      response = "0";
    }

    // Since the EmbeddedTestServer doesn't keep the socket open by default, it
    // is explicitly kept alive to allow the remaining leg of the 0RTT handshake
    // to be received after the early data.
    send.Run(response, base::BindOnce([]() {}));
  }

 private:
  bool zero_rtt_;

  DISALLOW_COPY_AND_ASSIGN(ZeroRTTResponse);
};

std::unique_ptr<test_server::HttpResponse> HandleZeroRTTRequest(
    const test_server::HttpRequest& request) {
  if (request.GetURL().path() != "/zerortt")
    return nullptr;
  bool zero_rtt = false;
  if (request.headers.find("Early-Data") != request.headers.end()) {
    if (request.headers.at("Early-Data") == "1") {
      zero_rtt = true;
    }
  }

  return std::unique_ptr<ZeroRTTResponse>(new ZeroRTTResponse(zero_rtt));
}

class SSLClientSocketZeroRTTTest : public SSLClientSocketTest {
 protected:
  SSLClientSocketZeroRTTTest() : SSLClientSocketTest() {
    SSLContextConfig config;
    config.version_max = SSL_PROTOCOL_VERSION_TLS1_3;
    ssl_config_service_->UpdateSSLConfigAndNotify(config);
  }

  bool StartServer() {
    SSLServerConfig server_config;
    server_config.early_data_enabled = true;
    server_config.version_max = SSL_PROTOCOL_VERSION_TLS1_3;
    return StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, server_config);
  }

  void RegisterEmbeddedTestServerHandlers(EmbeddedTestServer* server) override {
    SSLClientSocketTest::RegisterEmbeddedTestServerHandlers(server);
    server->RegisterRequestHandler(base::BindRepeating(&HandleZeroRTTRequest));
  }

  void SetServerConfig(SSLServerConfig server_config) {
    embedded_test_server()->ResetSSLConfig(net::EmbeddedTestServer::CERT_OK,
                                           server_config);
  }

  // Makes a new connection to the test server and returns a
  // FakeBlockingStreamSocket which may be used to block transport I/O.
  //
  // Most tests should call BlockReadResult() before calling Connect(). This
  // avoid race conditions by controlling the order of events. 0-RTT typically
  // races the ServerHello from the server with early data from the client. If
  // the ServerHello arrives before client calls Write(), the data may be sent
  // with 1-RTT keys rather than 0-RTT keys.
  FakeBlockingStreamSocket* MakeClient(bool early_data_enabled) {
    SSLConfig ssl_config;
    ssl_config.early_data_enabled = early_data_enabled;

    real_transport_.reset(
        new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
    std::unique_ptr<FakeBlockingStreamSocket> transport(
        new FakeBlockingStreamSocket(std::move(real_transport_)));
    FakeBlockingStreamSocket* raw_transport = transport.get();

    int rv = callback_.GetResult(transport->Connect(callback_.callback()));
    EXPECT_THAT(rv, IsOk());

    ssl_socket_ = CreateSSLClientSocket(std::move(transport), host_port_pair(),
                                        ssl_config);
    EXPECT_FALSE(ssl_socket_->IsConnected());

    return raw_transport;
  }

  int Connect() {
    return callback_.GetResult(ssl_socket_->Connect(callback_.callback()));
  }

  int WriteAndWait(base::StringPiece request) {
    scoped_refptr<IOBuffer> request_buffer =
        base::MakeRefCounted<IOBuffer>(request.size());
    memcpy(request_buffer->data(), request.data(), request.size());
    return callback_.GetResult(
        ssl_socket_->Write(request_buffer.get(), request.size(),
                           callback_.callback(), TRAFFIC_ANNOTATION_FOR_TESTS));
  }

  int ReadAndWait(IOBuffer* buf, size_t len) {
    return callback_.GetResult(
        ssl_socket_->Read(buf, len, callback_.callback()));
  }

  bool GetSSLInfo(SSLInfo* ssl_info) {
    return ssl_socket_->GetSSLInfo(ssl_info);
  }

  bool RunInitialConnection() {
    if (MakeClient(true) == nullptr)
      return false;

    EXPECT_THAT(Connect(), IsOk());

    // Use the socket for an HTTP request to ensure we've processed the
    // post-handshake TLS 1.3 ticket.
    EXPECT_THAT(MakeHTTPRequest(ssl_socket_.get()), IsOk());

    SSLInfo ssl_info;
    EXPECT_TRUE(GetSSLInfo(&ssl_info));
    return SSLInfo::HANDSHAKE_FULL == ssl_info.handshake_type;
  }

  SSLClientSocket* ssl_socket() { return ssl_socket_.get(); }

 private:
  TestCompletionCallback callback_;
  std::unique_ptr<StreamSocket> real_transport_;
  std::unique_ptr<SSLClientSocket> ssl_socket_;
};

// Returns a serialized unencrypted TLS 1.2 alert record for the given alert
// value.
std::string FormatTLS12Alert(uint8_t alert) {
  std::string ret;
  // ContentType.alert
  ret.push_back(21);
  // Record-layer version. Assume TLS 1.2.
  ret.push_back(0x03);
  ret.push_back(0x03);
  // Record length.
  ret.push_back(0);
  ret.push_back(2);
  // AlertLevel.fatal.
  ret.push_back(2);
  // The alert itself.
  ret.push_back(alert);
  return ret;
}

// A CertVerifier that never returns on any requests.
class HangingCertVerifier : public CertVerifier {
 public:
  int num_active_requests() const { return num_active_requests_; }

  void WaitForRequest() {
    if (!num_active_requests_) {
      run_loop_.Run();
    }
  }

  int Verify(const RequestParams& params,
             CertVerifyResult* verify_result,
             CompletionOnceCallback callback,
             std::unique_ptr<Request>* out_req,
             const NetLogWithSource& net_log) override {
    *out_req = std::make_unique<HangingRequest>(this);
    return ERR_IO_PENDING;
  }

  void SetConfig(const Config& config) override {}

 private:
  class HangingRequest : public Request {
   public:
    explicit HangingRequest(HangingCertVerifier* verifier)
        : verifier_(verifier) {
      verifier_->num_active_requests_++;
      verifier_->run_loop_.Quit();
    }

    ~HangingRequest() override { verifier_->num_active_requests_--; }

   private:
    HangingCertVerifier* verifier_;
  };

  base::RunLoop run_loop_;
  int num_active_requests_ = 0;
};

}  // namespace

// TODO(950069): Add testing for frame_origin in NetworkIsolationKey
// using kAppendInitiatingFrameOriginToNetworkIsolationKey.

INSTANTIATE_TEST_SUITE_P(TLSVersion,
                         SSLClientSocketVersionTest,
                         ::testing::ValuesIn(GetTLSVersions()));

TEST_P(SSLClientSocketVersionTest, Connect) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  TestCompletionCallback callback;
  RecordingTestNetLog log;
  std::unique_ptr<StreamSocket> transport(
      new TCPClientSocket(addr(), nullptr, &log, NetLogSource()));
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());

  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));

  EXPECT_FALSE(sock->IsConnected());

  rv = sock->Connect(callback.callback());

  auto entries = log.GetEntries();
  EXPECT_TRUE(LogContainsBeginEvent(entries, 5, NetLogEventType::SSL_CONNECT));
  if (rv == ERR_IO_PENDING)
    rv = callback.WaitForResult();
  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock->IsConnected());
  entries = log.GetEntries();
  EXPECT_TRUE(LogContainsEndEvent(entries, -1, NetLogEventType::SSL_CONNECT));

  sock->Disconnect();
  EXPECT_FALSE(sock->IsConnected());
}

TEST_P(SSLClientSocketVersionTest, ConnectSyncVerify) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  cert_verifier_->set_async(false);
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsError(OK));
}

TEST_P(SSLClientSocketVersionTest, ConnectExpired) {
  ASSERT_TRUE(StartEmbeddedTestServer(EmbeddedTestServer::CERT_EXPIRED,
                                      GetServerConfig()));

  cert_verifier_->set_default_result(ERR_CERT_DATE_INVALID);

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsError(ERR_CERT_DATE_INVALID));

  // Rather than testing whether or not the underlying socket is connected,
  // test that the handshake has finished. This is because it may be
  // desirable to disconnect the socket before showing a user prompt, since
  // the user may take indefinitely long to respond.
  auto entries = log_.GetEntries();
  EXPECT_TRUE(LogContainsEndEvent(entries, -1, NetLogEventType::SSL_CONNECT));
}

TEST_P(SSLClientSocketVersionTest, ConnectExpiredSyncVerify) {
  ASSERT_TRUE(StartEmbeddedTestServer(EmbeddedTestServer::CERT_EXPIRED,
                                      GetServerConfig()));

  cert_verifier_->set_default_result(ERR_CERT_DATE_INVALID);
  cert_verifier_->set_async(false);

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsError(ERR_CERT_DATE_INVALID));
}

// Test that SSLClientSockets may be destroyed while waiting on a certificate
// verification.
TEST_P(SSLClientSocketVersionTest, SocketDestroyedDuringVerify) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  HangingCertVerifier verifier;
  context_ = std::make_unique<SSLClientContext>(
      ssl_config_service_.get(), &verifier, transport_security_state_.get(),
      ct_verifier_.get(), ct_policy_enforcer_.get(),
      ssl_client_session_cache_.get());

  TestCompletionCallback callback;
  auto transport =
      std::make_unique<TCPClientSocket>(addr(), nullptr, &log_, NetLogSource());
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  ASSERT_THAT(rv, IsOk());

  std::unique_ptr<SSLClientSocket> sock = CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig());
  rv = sock->Connect(callback.callback());
  EXPECT_THAT(rv, IsError(ERR_IO_PENDING));

  // The socket should attempt a certificate verification.
  verifier.WaitForRequest();
  EXPECT_EQ(1, verifier.num_active_requests());

  // Destroying the socket should cancel it.
  sock = nullptr;
  EXPECT_EQ(0, verifier.num_active_requests());

  context_ = nullptr;
}

TEST_P(SSLClientSocketVersionTest, ConnectMismatched) {
  ASSERT_TRUE(StartEmbeddedTestServer(EmbeddedTestServer::CERT_MISMATCHED_NAME,
                                      GetServerConfig()));

  cert_verifier_->set_default_result(ERR_CERT_COMMON_NAME_INVALID);

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsError(ERR_CERT_COMMON_NAME_INVALID));

  // Rather than testing whether or not the underlying socket is connected,
  // test that the handshake has finished. This is because it may be
  // desirable to disconnect the socket before showing a user prompt, since
  // the user may take indefinitely long to respond.
  auto entries = log_.GetEntries();
  EXPECT_TRUE(LogContainsEndEvent(entries, -1, NetLogEventType::SSL_CONNECT));
}

// Tests that certificates parsable by SSLClientSocket's internal SSL
// implementation, but not X509Certificate are treated as fatal connection
// errors. This is a regression test for https://crbug.com/91341.
TEST_P(SSLClientSocketVersionTest, ConnectBadValidity) {
  ASSERT_TRUE(StartEmbeddedTestServer(EmbeddedTestServer::CERT_BAD_VALIDITY,
                                      GetServerConfig()));
  cert_verifier_->set_default_result(ERR_CERT_DATE_INVALID);

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  EXPECT_THAT(rv, IsError(ERR_CERT_DATE_INVALID));
}

// Ignoring the certificate error from an invalid certificate should
// allow a complete connection.
TEST_P(SSLClientSocketVersionTest, ConnectBadValidityIgnoreCertErrors) {
  ASSERT_TRUE(StartEmbeddedTestServer(EmbeddedTestServer::CERT_BAD_VALIDITY,
                                      GetServerConfig()));
  cert_verifier_->set_default_result(ERR_CERT_DATE_INVALID);

  SSLConfig ssl_config;
  ssl_config.ignore_certificate_errors = true;
  int rv;
  CreateAndConnectSSLClientSocket(ssl_config, &rv);
  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock_->IsConnected());
}

// Attempt to connect to a page which requests a client certificate. It should
// return an error code on connect.
TEST_P(SSLClientSocketVersionTest, ConnectClientAuthCertRequested) {
  SSLServerConfig server_config = GetServerConfig();
  server_config.client_cert_type = SSLServerConfig::OPTIONAL_CLIENT_CERT;
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, server_config));

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsError(ERR_SSL_CLIENT_AUTH_CERT_NEEDED));

  auto entries = log_.GetEntries();
  EXPECT_TRUE(LogContainsEndEvent(entries, -1, NetLogEventType::SSL_CONNECT));
  EXPECT_FALSE(sock_->IsConnected());
}

// Connect to a server requesting optional client authentication. Send it a
// null certificate. It should allow the connection.
TEST_P(SSLClientSocketVersionTest, ConnectClientAuthSendNullCert) {
  SSLServerConfig server_config = GetServerConfig();
  server_config.client_cert_type = SSLServerConfig::OPTIONAL_CLIENT_CERT;
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, server_config));

  // Our test server accepts certificate-less connections.
  context_->SetClientCertificate(host_port_pair(), nullptr, nullptr);

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsOk());

  // We responded to the server's certificate request with a Certificate
  // message with no client certificate in it.  ssl_info.client_cert_sent
  // should be false in this case.
  SSLInfo ssl_info;
  sock_->GetSSLInfo(&ssl_info);
  EXPECT_FALSE(ssl_info.client_cert_sent);

  sock_->Disconnect();
  EXPECT_FALSE(sock_->IsConnected());
}

// TODO(wtc): Add unit tests for IsConnectedAndIdle:
//   - Server closes an SSL connection (with a close_notify alert message).
//   - Server closes the underlying TCP connection directly.
//   - Server sends data unexpectedly.

// Tests that the socket can be read from successfully. Also test that a peer's
// close_notify alert is successfully processed without error.
TEST_P(SSLClientSocketReadTest, Read) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  EXPECT_EQ(0, transport->GetTotalReceivedBytes());

  int rv = callback.GetResult(transport->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());

  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));
  EXPECT_EQ(0, sock->GetTotalReceivedBytes());

  rv = callback.GetResult(sock->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());

  // Number of network bytes received should increase because of SSL socket
  // establishment.
  EXPECT_GT(sock->GetTotalReceivedBytes(), 0);

  const char request_text[] = "GET / HTTP/1.0\r\n\r\n";
  scoped_refptr<IOBuffer> request_buffer =
      base::MakeRefCounted<IOBuffer>(base::size(request_text) - 1);
  memcpy(request_buffer->data(), request_text, base::size(request_text) - 1);

  rv = callback.GetResult(
      sock->Write(request_buffer.get(), base::size(request_text) - 1,
                  callback.callback(), TRAFFIC_ANNOTATION_FOR_TESTS));
  EXPECT_EQ(static_cast<int>(base::size(request_text) - 1), rv);

  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  int64_t unencrypted_bytes_read = 0;
  int64_t network_bytes_read_during_handshake = sock->GetTotalReceivedBytes();
  do {
    rv = ReadAndWaitForCompletion(sock.get(), buf.get(), 4096);
    EXPECT_GE(rv, 0);
    if (rv >= 0) {
      unencrypted_bytes_read += rv;
    }
  } while (rv > 0);
  EXPECT_GT(unencrypted_bytes_read, 0);
  // Reading the payload should increase the number of bytes on network layer.
  EXPECT_GT(sock->GetTotalReceivedBytes(), network_bytes_read_during_handshake);
  // Number of bytes received on the network after the handshake should be
  // higher than the number of encrypted bytes read.
  EXPECT_GE(sock->GetTotalReceivedBytes() - network_bytes_read_during_handshake,
            unencrypted_bytes_read);

  // The peer should have cleanly closed the connection with a close_notify.
  EXPECT_EQ(0, rv);
}

// Tests that SSLClientSocket properly handles when the underlying transport
// synchronously fails a transport write in during the handshake.
TEST_F(SSLClientSocketTest, Connect_WithSynchronousError) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, SSLServerConfig()));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  std::unique_ptr<SynchronousErrorStreamSocket> transport(
      new SynchronousErrorStreamSocket(std::move(real_transport)));
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());

  SynchronousErrorStreamSocket* raw_transport = transport.get();
  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));

  raw_transport->SetNextWriteError(ERR_CONNECTION_RESET);

  rv = callback.GetResult(sock->Connect(callback.callback()));
  EXPECT_THAT(rv, IsError(ERR_CONNECTION_RESET));
  EXPECT_FALSE(sock->IsConnected());
}

// Tests that the SSLClientSocket properly handles when the underlying transport
// synchronously returns an error code - such as if an intermediary terminates
// the socket connection uncleanly.
// This is a regression test for http://crbug.com/238536
TEST_P(SSLClientSocketReadTest, Read_WithSynchronousError) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  std::unique_ptr<SynchronousErrorStreamSocket> transport(
      new SynchronousErrorStreamSocket(std::move(real_transport)));
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());

  SSLConfig config;
  config.disable_post_handshake_peek_for_testing = true;
  SynchronousErrorStreamSocket* raw_transport = transport.get();
  std::unique_ptr<SSLClientSocket> sock(
      CreateSSLClientSocket(std::move(transport), host_port_pair(), config));

  rv = callback.GetResult(sock->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock->IsConnected());

  const char request_text[] = "GET / HTTP/1.0\r\n\r\n";
  static const int kRequestTextSize =
      static_cast<int>(base::size(request_text) - 1);
  scoped_refptr<IOBuffer> request_buffer =
      base::MakeRefCounted<IOBuffer>(kRequestTextSize);
  memcpy(request_buffer->data(), request_text, kRequestTextSize);

  rv = callback.GetResult(sock->Write(request_buffer.get(), kRequestTextSize,
                                      callback.callback(),
                                      TRAFFIC_ANNOTATION_FOR_TESTS));
  EXPECT_EQ(kRequestTextSize, rv);

  // Simulate an unclean/forcible shutdown.
  raw_transport->SetNextReadError(ERR_CONNECTION_RESET);

  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);

  // Note: This test will hang if this bug has regressed. Simply checking that
  // rv != ERR_IO_PENDING is insufficient, as ERR_IO_PENDING is a legitimate
  // result when using a dedicated task runner for NSS.
  rv = ReadAndWaitForCompletion(sock.get(), buf.get(), 4096);
  EXPECT_THAT(rv, IsError(ERR_CONNECTION_RESET));
}

// Tests that the SSLClientSocket properly handles when the underlying transport
// asynchronously returns an error code while writing data - such as if an
// intermediary terminates the socket connection uncleanly.
// This is a regression test for http://crbug.com/249848
TEST_P(SSLClientSocketVersionTest, Write_WithSynchronousError) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  // Note: |error_socket|'s ownership is handed to |transport|, but a pointer
  // is retained in order to configure additional errors.
  std::unique_ptr<SynchronousErrorStreamSocket> error_socket(
      new SynchronousErrorStreamSocket(std::move(real_transport)));
  SynchronousErrorStreamSocket* raw_error_socket = error_socket.get();
  std::unique_ptr<FakeBlockingStreamSocket> transport(
      new FakeBlockingStreamSocket(std::move(error_socket)));
  FakeBlockingStreamSocket* raw_transport = transport.get();
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());

  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));

  rv = callback.GetResult(sock->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock->IsConnected());

  const char request_text[] = "GET / HTTP/1.0\r\n\r\n";
  static const int kRequestTextSize =
      static_cast<int>(base::size(request_text) - 1);
  scoped_refptr<IOBuffer> request_buffer =
      base::MakeRefCounted<IOBuffer>(kRequestTextSize);
  memcpy(request_buffer->data(), request_text, kRequestTextSize);

  // Simulate an unclean/forcible shutdown on the underlying socket.
  // However, simulate this error asynchronously.
  raw_error_socket->SetNextWriteError(ERR_CONNECTION_RESET);
  raw_transport->BlockWrite();

  // This write should complete synchronously, because the TLS ciphertext
  // can be created and placed into the outgoing buffers independent of the
  // underlying transport.
  rv = callback.GetResult(sock->Write(request_buffer.get(), kRequestTextSize,
                                      callback.callback(),
                                      TRAFFIC_ANNOTATION_FOR_TESTS));
  EXPECT_EQ(kRequestTextSize, rv);

  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);

  rv = sock->Read(buf.get(), 4096, callback.callback());
  EXPECT_THAT(rv, IsError(ERR_IO_PENDING));

  // Now unblock the outgoing request, having it fail with the connection
  // being reset.
  raw_transport->UnblockWrite();

  // Note: This will cause an inifite loop if this bug has regressed. Simply
  // checking that rv != ERR_IO_PENDING is insufficient, as ERR_IO_PENDING
  // is a legitimate result when using a dedicated task runner for NSS.
  rv = callback.GetResult(rv);
  EXPECT_THAT(rv, IsError(ERR_CONNECTION_RESET));
}

// If there is a Write failure at the transport with no follow-up Read, although
// the write error will not be returned to the client until a future Read or
// Write operation, SSLClientSocket should not spin attempting to re-write on
// the socket. This is a regression test for part of https://crbug.com/381160.
TEST_P(SSLClientSocketVersionTest, Write_WithSynchronousErrorNoRead) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, SSLServerConfig()));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  // Note: intermediate sockets' ownership are handed to |sock|, but a pointer
  // is retained in order to query them.
  std::unique_ptr<SynchronousErrorStreamSocket> error_socket(
      new SynchronousErrorStreamSocket(std::move(real_transport)));
  SynchronousErrorStreamSocket* raw_error_socket = error_socket.get();
  std::unique_ptr<CountingStreamSocket> counting_socket(
      new CountingStreamSocket(std::move(error_socket)));
  CountingStreamSocket* raw_counting_socket = counting_socket.get();
  int rv = callback.GetResult(counting_socket->Connect(callback.callback()));
  ASSERT_THAT(rv, IsOk());

  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(counting_socket), host_port_pair(), SSLConfig()));

  rv = callback.GetResult(sock->Connect(callback.callback()));
  ASSERT_THAT(rv, IsOk());
  ASSERT_TRUE(sock->IsConnected());

  // Simulate an unclean/forcible shutdown on the underlying socket.
  raw_error_socket->SetNextWriteError(ERR_CONNECTION_RESET);

  const char request_text[] = "GET / HTTP/1.0\r\n\r\n";
  static const int kRequestTextSize =
      static_cast<int>(base::size(request_text) - 1);
  scoped_refptr<IOBuffer> request_buffer =
      base::MakeRefCounted<IOBuffer>(kRequestTextSize);
  memcpy(request_buffer->data(), request_text, kRequestTextSize);

  // This write should complete synchronously, because the TLS ciphertext
  // can be created and placed into the outgoing buffers independent of the
  // underlying transport.
  rv = callback.GetResult(sock->Write(request_buffer.get(), kRequestTextSize,
                                      callback.callback(),
                                      TRAFFIC_ANNOTATION_FOR_TESTS));
  ASSERT_EQ(kRequestTextSize, rv);

  // Let the event loop spin for a little bit of time. Even on platforms where
  // pumping the state machine involve thread hops, there should be no further
  // writes on the transport socket.
  //
  // TODO(davidben): Avoid the arbitrary timeout?
  int old_write_count = raw_counting_socket->write_count();
  base::RunLoop loop;
  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE, loop.QuitClosure(), base::TimeDelta::FromMilliseconds(100));
  loop.Run();
  EXPECT_EQ(old_write_count, raw_counting_socket->write_count());
}

// Test the full duplex mode, with Read and Write pending at the same time.
// This test also serves as a regression test for http://crbug.com/29815.
TEST_P(SSLClientSocketReadTest, Read_FullDuplex) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsOk());

  // Issue a "hanging" Read first.
  TestCompletionCallback callback;
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  int read_rv = Read(sock_.get(), buf.get(), 4096, callback.callback());
  // We haven't written the request, so there should be no response yet.
  ASSERT_THAT(read_rv, IsError(ERR_IO_PENDING));

  // Write the request.
  // The request is padded with a User-Agent header to a size that causes the
  // memio circular buffer (4k bytes) in SSLClientSocketNSS to wrap around.
  // This tests the fix for http://crbug.com/29815.
  std::string request_text = "GET / HTTP/1.1\r\nUser-Agent: long browser name ";
  for (int i = 0; i < 3770; ++i)
    request_text.push_back('*');
  request_text.append("\r\n\r\n");
  scoped_refptr<IOBuffer> request_buffer =
      base::MakeRefCounted<StringIOBuffer>(request_text);

  TestCompletionCallback callback2;  // Used for Write only.
  rv = callback2.GetResult(
      sock_->Write(request_buffer.get(), request_text.size(),
                   callback2.callback(), TRAFFIC_ANNOTATION_FOR_TESTS));
  EXPECT_EQ(static_cast<int>(request_text.size()), rv);

  // Now get the Read result.
  read_rv =
      WaitForReadCompletion(sock_.get(), buf.get(), 4096, &callback, read_rv);
  EXPECT_GT(read_rv, 0);
}

// Attempts to Read() and Write() from an SSLClientSocketNSS in full duplex
// mode when the underlying transport is blocked on sending data. When the
// underlying transport completes due to an error, it should invoke both the
// Read() and Write() callbacks. If the socket is deleted by the Read()
// callback, the Write() callback should not be invoked.
// Regression test for http://crbug.com/232633
TEST_P(SSLClientSocketReadTest, Read_DeleteWhilePendingFullDuplex) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  // Note: |error_socket|'s ownership is handed to |transport|, but a pointer
  // is retained in order to configure additional errors.
  std::unique_ptr<SynchronousErrorStreamSocket> error_socket(
      new SynchronousErrorStreamSocket(std::move(real_transport)));
  SynchronousErrorStreamSocket* raw_error_socket = error_socket.get();
  std::unique_ptr<FakeBlockingStreamSocket> transport(
      new FakeBlockingStreamSocket(std::move(error_socket)));
  FakeBlockingStreamSocket* raw_transport = transport.get();

  int rv = callback.GetResult(transport->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());

  SSLConfig config;
  config.disable_post_handshake_peek_for_testing = true;
  std::unique_ptr<SSLClientSocket> sock =
      CreateSSLClientSocket(std::move(transport), host_port_pair(), config);

  rv = callback.GetResult(sock->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock->IsConnected());

  std::string request_text = "GET / HTTP/1.1\r\nUser-Agent: long browser name ";
  request_text.append(20 * 1024, '*');
  request_text.append("\r\n\r\n");
  scoped_refptr<DrainableIOBuffer> request_buffer =
      base::MakeRefCounted<DrainableIOBuffer>(
          base::MakeRefCounted<StringIOBuffer>(request_text),
          request_text.size());

  // Simulate errors being returned from the underlying Read() and Write() ...
  raw_error_socket->SetNextReadError(ERR_CONNECTION_RESET);
  raw_error_socket->SetNextWriteError(ERR_CONNECTION_RESET);
  // ... but have those errors returned asynchronously. Because the Write() will
  // return first, this will trigger the error.
  raw_transport->BlockReadResult();
  raw_transport->BlockWrite();

  // Enqueue a Read() before calling Write(), which should "hang" due to
  // the ERR_IO_PENDING caused by SetReadShouldBlock() and thus return.
  SSLClientSocket* raw_sock = sock.get();
  DeleteSocketCallback read_callback(sock.release());
  scoped_refptr<IOBuffer> read_buf = base::MakeRefCounted<IOBuffer>(4096);
  rv = Read(raw_sock, read_buf.get(), 4096, read_callback.callback());

  // Ensure things didn't complete synchronously, otherwise |sock| is invalid.
  ASSERT_THAT(rv, IsError(ERR_IO_PENDING));
  ASSERT_FALSE(read_callback.have_result());

  // Attempt to write the remaining data. OpenSSL will return that its blocked
  // because the underlying transport is blocked.
  rv = raw_sock->Write(request_buffer.get(), request_buffer->BytesRemaining(),
                       callback.callback(), TRAFFIC_ANNOTATION_FOR_TESTS);
  ASSERT_THAT(rv, IsError(ERR_IO_PENDING));
  ASSERT_FALSE(callback.have_result());

  // Now unblock Write(), which will invoke OnSendComplete and (eventually)
  // call the Read() callback, deleting the socket and thus aborting calling
  // the Write() callback.
  raw_transport->UnblockWrite();

  // |read_callback| deletes |sock| so if ReadIfReady() is used, we will get OK
  // asynchronously but can't continue reading because the socket is gone.
  rv = read_callback.WaitForResult();
  if (test_ssl_read_if_ready()) {
    EXPECT_THAT(rv, IsOk());
  } else {
    EXPECT_THAT(rv, IsError(ERR_CONNECTION_RESET));
  }

  // The Write callback should not have been called.
  EXPECT_FALSE(callback.have_result());
}

// Tests that the SSLClientSocket does not crash if data is received on the
// transport socket after a failing write. This can occur if we have a Write
// error in a SPDY socket.
// Regression test for http://crbug.com/335557
TEST_P(SSLClientSocketReadTest, Read_WithWriteError) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  // Note: |error_socket|'s ownership is handed to |transport|, but a pointer
  // is retained in order to configure additional errors.
  std::unique_ptr<SynchronousErrorStreamSocket> error_socket(
      new SynchronousErrorStreamSocket(std::move(real_transport)));
  SynchronousErrorStreamSocket* raw_error_socket = error_socket.get();
  std::unique_ptr<FakeBlockingStreamSocket> transport(
      new FakeBlockingStreamSocket(std::move(error_socket)));
  FakeBlockingStreamSocket* raw_transport = transport.get();

  int rv = callback.GetResult(transport->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());

  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));

  rv = callback.GetResult(sock->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock->IsConnected());

  // Send a request so there is something to read from the socket.
  const char request_text[] = "GET / HTTP/1.0\r\n\r\n";
  static const int kRequestTextSize =
      static_cast<int>(base::size(request_text) - 1);
  scoped_refptr<IOBuffer> request_buffer =
      base::MakeRefCounted<IOBuffer>(kRequestTextSize);
  memcpy(request_buffer->data(), request_text, kRequestTextSize);

  rv = callback.GetResult(sock->Write(request_buffer.get(), kRequestTextSize,
                                      callback.callback(),
                                      TRAFFIC_ANNOTATION_FOR_TESTS));
  EXPECT_EQ(kRequestTextSize, rv);

  // Start a hanging read.
  TestCompletionCallback read_callback;
  raw_transport->BlockReadResult();
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  rv = Read(sock.get(), buf.get(), 4096, read_callback.callback());
  EXPECT_THAT(rv, IsError(ERR_IO_PENDING));

  // Perform another write, but have it fail. Write a request larger than the
  // internal socket buffers so that the request hits the underlying transport
  // socket and detects the error.
  std::string long_request_text =
      "GET / HTTP/1.1\r\nUser-Agent: long browser name ";
  long_request_text.append(20 * 1024, '*');
  long_request_text.append("\r\n\r\n");
  scoped_refptr<DrainableIOBuffer> long_request_buffer =
      base::MakeRefCounted<DrainableIOBuffer>(
          base::MakeRefCounted<StringIOBuffer>(long_request_text),
          long_request_text.size());

  raw_error_socket->SetNextWriteError(ERR_CONNECTION_RESET);

  // Write as much data as possible until hitting an error.
  do {
    rv = callback.GetResult(sock->Write(
        long_request_buffer.get(), long_request_buffer->BytesRemaining(),
        callback.callback(), TRAFFIC_ANNOTATION_FOR_TESTS));
    if (rv > 0) {
      long_request_buffer->DidConsume(rv);
      // Abort if the entire input is ever consumed. The input is larger than
      // the SSLClientSocket's write buffers.
      ASSERT_LT(0, long_request_buffer->BytesRemaining());
    }
  } while (rv > 0);

  EXPECT_THAT(rv, IsError(ERR_CONNECTION_RESET));

  // At this point the Read result is available. Transport write errors are
  // surfaced through Writes. See https://crbug.com/249848.
  rv = WaitForReadCompletion(sock.get(), buf.get(), 4096, &read_callback, rv);
  EXPECT_THAT(rv, IsError(ERR_CONNECTION_RESET));

  // Release the read. This does not cause a crash.
  raw_transport->UnblockReadResult();
  base::RunLoop().RunUntilIdle();
}

// Tests that SSLClientSocket fails the handshake if the underlying
// transport is cleanly closed.
TEST_F(SSLClientSocketTest, Connect_WithZeroReturn) {
  // There is no need to vary by TLS version because this test never reads a
  // response from the server.
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, SSLServerConfig()));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  std::unique_ptr<SynchronousErrorStreamSocket> transport(
      new SynchronousErrorStreamSocket(std::move(real_transport)));
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());

  SynchronousErrorStreamSocket* raw_transport = transport.get();
  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));

  raw_transport->SetNextReadError(0);

  rv = callback.GetResult(sock->Connect(callback.callback()));
  EXPECT_THAT(rv, IsError(ERR_CONNECTION_CLOSED));
  EXPECT_FALSE(sock->IsConnected());
}

// Tests that SSLClientSocket returns a Read of size 0 if the underlying socket
// is cleanly closed, but the peer does not send close_notify.
// This is a regression test for https://crbug.com/422246
TEST_P(SSLClientSocketReadTest, Read_WithZeroReturn) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  std::unique_ptr<SynchronousErrorStreamSocket> transport(
      new SynchronousErrorStreamSocket(std::move(real_transport)));
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());

  SSLConfig config;
  config.disable_post_handshake_peek_for_testing = true;
  SynchronousErrorStreamSocket* raw_transport = transport.get();
  std::unique_ptr<SSLClientSocket> sock(
      CreateSSLClientSocket(std::move(transport), host_port_pair(), config));

  rv = callback.GetResult(sock->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock->IsConnected());

  raw_transport->SetNextReadError(0);
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  rv = ReadAndWaitForCompletion(sock.get(), buf.get(), 4096);
  EXPECT_EQ(0, rv);
}

// Tests that SSLClientSocket cleanly returns a Read of size 0 if the
// underlying socket is cleanly closed asynchronously.
// This is a regression test for https://crbug.com/422246
TEST_P(SSLClientSocketReadTest, Read_WithAsyncZeroReturn) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  std::unique_ptr<SynchronousErrorStreamSocket> error_socket(
      new SynchronousErrorStreamSocket(std::move(real_transport)));
  SynchronousErrorStreamSocket* raw_error_socket = error_socket.get();
  std::unique_ptr<FakeBlockingStreamSocket> transport(
      new FakeBlockingStreamSocket(std::move(error_socket)));
  FakeBlockingStreamSocket* raw_transport = transport.get();
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());

  SSLConfig config;
  config.disable_post_handshake_peek_for_testing = true;
  std::unique_ptr<SSLClientSocket> sock(
      CreateSSLClientSocket(std::move(transport), host_port_pair(), config));

  rv = callback.GetResult(sock->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock->IsConnected());

  raw_error_socket->SetNextReadError(0);
  raw_transport->BlockReadResult();
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  TestCompletionCallback read_callback;
  rv = Read(sock.get(), buf.get(), 4096, read_callback.callback());
  EXPECT_THAT(rv, IsError(ERR_IO_PENDING));

  raw_transport->UnblockReadResult();
  rv = WaitForReadCompletion(sock.get(), buf.get(), 4096, &read_callback, rv);
  EXPECT_EQ(0, rv);
}

// Tests that fatal alerts from the peer are processed. This is a regression
// test for https://crbug.com/466303.
TEST_P(SSLClientSocketReadTest, Read_WithFatalAlert) {
  SpawnedTestServer::SSLOptions ssl_options;
  auto tls_max_version = ProtocolVersionToSpawnedTestServer(version());
  if (!tls_max_version) {
    return;
  }
  ssl_options.tls_max_version = *tls_max_version;
  ssl_options.alert_after_handshake = true;
  ASSERT_TRUE(StartTestServer(ssl_options));

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsOk());

  // Receive the fatal alert.
  TestCompletionCallback callback;
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  EXPECT_EQ(ERR_SSL_PROTOCOL_ERROR,
            ReadAndWaitForCompletion(sock_.get(), buf.get(), 4096));
}

TEST_P(SSLClientSocketReadTest, Read_SmallChunks) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsOk());

  const char request_text[] = "GET / HTTP/1.0\r\n\r\n";
  scoped_refptr<IOBuffer> request_buffer =
      base::MakeRefCounted<IOBuffer>(base::size(request_text) - 1);
  memcpy(request_buffer->data(), request_text, base::size(request_text) - 1);

  TestCompletionCallback callback;
  rv = callback.GetResult(
      sock_->Write(request_buffer.get(), base::size(request_text) - 1,
                   callback.callback(), TRAFFIC_ANNOTATION_FOR_TESTS));
  EXPECT_EQ(static_cast<int>(base::size(request_text) - 1), rv);

  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(1);
  do {
    rv = ReadAndWaitForCompletion(sock_.get(), buf.get(), 1);
    EXPECT_GE(rv, 0);
  } while (rv > 0);
}

TEST_P(SSLClientSocketReadTest, Read_ManySmallRecords) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  TestCompletionCallback callback;

  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  std::unique_ptr<ReadBufferingStreamSocket> transport(
      new ReadBufferingStreamSocket(std::move(real_transport)));
  ReadBufferingStreamSocket* raw_transport = transport.get();
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  ASSERT_THAT(rv, IsOk());

  SSLConfig config;
  config.disable_post_handshake_peek_for_testing = true;
  std::unique_ptr<SSLClientSocket> sock(
      CreateSSLClientSocket(std::move(transport), host_port_pair(), config));

  rv = callback.GetResult(sock->Connect(callback.callback()));
  ASSERT_THAT(rv, IsOk());
  ASSERT_TRUE(sock->IsConnected());

  const char request_text[] = "GET /ssl-many-small-records HTTP/1.0\r\n\r\n";
  scoped_refptr<IOBuffer> request_buffer =
      base::MakeRefCounted<IOBuffer>(base::size(request_text) - 1);
  memcpy(request_buffer->data(), request_text, base::size(request_text) - 1);

  rv = callback.GetResult(
      sock->Write(request_buffer.get(), base::size(request_text) - 1,
                  callback.callback(), TRAFFIC_ANNOTATION_FOR_TESTS));
  ASSERT_GT(rv, 0);
  ASSERT_EQ(static_cast<int>(base::size(request_text) - 1), rv);

  // Note: This relies on SSLClientSocketNSS attempting to read up to 17K of
  // data (the max SSL record size) at a time. Ensure that at least 15K worth
  // of SSL data is buffered first. The 15K of buffered data is made up of
  // many smaller SSL records (the TestServer writes along 1350 byte
  // plaintext boundaries), although there may also be a few records that are
  // smaller or larger, due to timing and SSL False Start.
  // 15K was chosen because 15K is smaller than the 17K (max) read issued by
  // the SSLClientSocket implementation, and larger than the minimum amount
  // of ciphertext necessary to contain the 8K of plaintext requested below.
  raw_transport->SetBufferSize(15000);

  scoped_refptr<IOBuffer> buffer = base::MakeRefCounted<IOBuffer>(8192);
  rv = ReadAndWaitForCompletion(sock.get(), buffer.get(), 8192);
  ASSERT_EQ(rv, 8192);
}

TEST_P(SSLClientSocketReadTest, Read_Interrupted) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsOk());

  const char request_text[] = "GET / HTTP/1.0\r\n\r\n";
  scoped_refptr<IOBuffer> request_buffer =
      base::MakeRefCounted<IOBuffer>(base::size(request_text) - 1);
  memcpy(request_buffer->data(), request_text, base::size(request_text) - 1);

  TestCompletionCallback callback;
  rv = callback.GetResult(
      sock_->Write(request_buffer.get(), base::size(request_text) - 1,
                   callback.callback(), TRAFFIC_ANNOTATION_FOR_TESTS));
  EXPECT_EQ(static_cast<int>(base::size(request_text) - 1), rv);

  // Do a partial read and then exit.  This test should not crash!
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(512);
  rv = ReadAndWaitForCompletion(sock_.get(), buf.get(), 512);
  EXPECT_GT(rv, 0);
}

TEST_P(SSLClientSocketReadTest, Read_FullLogging) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  TestCompletionCallback callback;
  RecordingTestNetLog log;
  log.SetObserverCaptureMode(NetLogCaptureMode::kEverything);
  std::unique_ptr<StreamSocket> transport(
      new TCPClientSocket(addr(), nullptr, &log, NetLogSource()));
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());

  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));

  rv = callback.GetResult(sock->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock->IsConnected());

  const char request_text[] = "GET / HTTP/1.0\r\n\r\n";
  scoped_refptr<IOBuffer> request_buffer =
      base::MakeRefCounted<IOBuffer>(base::size(request_text) - 1);
  memcpy(request_buffer->data(), request_text, base::size(request_text) - 1);

  rv = callback.GetResult(
      sock->Write(request_buffer.get(), base::size(request_text) - 1,
                  callback.callback(), TRAFFIC_ANNOTATION_FOR_TESTS));
  EXPECT_EQ(static_cast<int>(base::size(request_text) - 1), rv);

  auto entries = log.GetEntries();
  size_t last_index = ExpectLogContainsSomewhereAfter(
      entries, 5, NetLogEventType::SSL_SOCKET_BYTES_SENT,
      NetLogEventPhase::NONE);

  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  for (;;) {
    rv = ReadAndWaitForCompletion(sock.get(), buf.get(), 4096);
    EXPECT_GE(rv, 0);
    if (rv <= 0)
      break;

    entries = log.GetEntries();
    last_index = ExpectLogContainsSomewhereAfter(
        entries, last_index + 1, NetLogEventType::SSL_SOCKET_BYTES_RECEIVED,
        NetLogEventPhase::NONE);
  }
}

// Regression test for http://crbug.com/42538
TEST_F(SSLClientSocketTest, PrematureApplicationData) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, SSLServerConfig()));

  static const unsigned char application_data[] = {
      0x17, 0x03, 0x01, 0x00, 0x4a, 0x02, 0x00, 0x00, 0x46, 0x03, 0x01, 0x4b,
      0xc2, 0xf8, 0xb2, 0xc1, 0x56, 0x42, 0xb9, 0x57, 0x7f, 0xde, 0x87, 0x46,
      0xf7, 0xa3, 0x52, 0x42, 0x21, 0xf0, 0x13, 0x1c, 0x9c, 0x83, 0x88, 0xd6,
      0x93, 0x0c, 0xf6, 0x36, 0x30, 0x05, 0x7e, 0x20, 0xb5, 0xb5, 0x73, 0x36,
      0x53, 0x83, 0x0a, 0xfc, 0x17, 0x63, 0xbf, 0xa0, 0xe4, 0x42, 0x90, 0x0d,
      0x2f, 0x18, 0x6d, 0x20, 0xd8, 0x36, 0x3f, 0xfc, 0xe6, 0x01, 0xfa, 0x0f,
      0xa5, 0x75, 0x7f, 0x09, 0x00, 0x04, 0x00, 0x16, 0x03, 0x01, 0x11, 0x57,
      0x0b, 0x00, 0x11, 0x53, 0x00, 0x11, 0x50, 0x00, 0x06, 0x22, 0x30, 0x82,
      0x06, 0x1e, 0x30, 0x82, 0x05, 0x06, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02,
      0x0a};

  // All reads and writes complete synchronously (async=false).
  MockRead data_reads[] = {
      MockRead(SYNCHRONOUS, reinterpret_cast<const char*>(application_data),
               base::size(application_data)),
      MockRead(SYNCHRONOUS, OK),
  };

  StaticSocketDataProvider data(data_reads, base::span<MockWrite>());

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> transport(
      new MockTCPClientSocket(addr(), nullptr, &data));
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());

  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));

  rv = callback.GetResult(sock->Connect(callback.callback()));
  EXPECT_THAT(rv, IsError(ERR_SSL_PROTOCOL_ERROR));
}

TEST_F(SSLClientSocketTest, CipherSuiteDisables) {
  // Rather than exhaustively disabling every AES_128_CBC ciphersuite defined at
  // http://www.iana.org/assignments/tls-parameters/tls-parameters.xml, only
  // disabling those cipher suites that the test server actually implements.
  const uint16_t kCiphersToDisable[] = {
      0x002f,  // TLS_RSA_WITH_AES_128_CBC_SHA
      0x0033,  // TLS_DHE_RSA_WITH_AES_128_CBC_SHA
      0xc013,  // TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA
  };

  SpawnedTestServer::SSLOptions ssl_options;
  // Enable only AES_128_CBC on the test server.
  ssl_options.bulk_ciphers = SpawnedTestServer::SSLOptions::BULK_CIPHER_AES128;
  ASSERT_TRUE(StartTestServer(ssl_options));

  SSLContextConfig ssl_context_config;
  for (size_t i = 0; i < base::size(kCiphersToDisable); ++i)
    ssl_context_config.disabled_cipher_suites.push_back(kCiphersToDisable[i]);
  ssl_config_service_->UpdateSSLConfigAndNotify(ssl_context_config);

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsError(ERR_SSL_VERSION_OR_CIPHER_MISMATCH));
}

// When creating an SSLClientSocket, it is allowed to pass in a
// ClientSocketHandle that is not obtained from a client socket pool.
// Here we verify that such a simple ClientSocketHandle, not associated with any
// client socket pool, can be destroyed safely.
TEST_F(SSLClientSocketTest, ClientSocketHandleNotFromPool) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, SSLServerConfig()));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());

  std::unique_ptr<SSLClientSocket> sock(socket_factory_->CreateSSLClientSocket(
      context_.get(), std::move(transport), host_port_pair(), SSLConfig()));

  EXPECT_FALSE(sock->IsConnected());
  rv = callback.GetResult(sock->Connect(callback.callback()));
  EXPECT_THAT(rv, IsOk());
}

// Verifies that SSLClientSocket::ExportKeyingMaterial return a success
// code and different keying label results in different keying material.
TEST_P(SSLClientSocketVersionTest, ExportKeyingMaterial) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock_->IsConnected());

  const int kKeyingMaterialSize = 32;
  const char kKeyingLabel1[] = "client-socket-test-1";
  const char kKeyingContext1[] = "";
  unsigned char client_out1[kKeyingMaterialSize];
  memset(client_out1, 0, sizeof(client_out1));
  rv = sock_->ExportKeyingMaterial(kKeyingLabel1, false, kKeyingContext1,
                                   client_out1, sizeof(client_out1));
  EXPECT_EQ(rv, OK);

  const char kKeyingLabel2[] = "client-socket-test-2";
  unsigned char client_out2[kKeyingMaterialSize];
  memset(client_out2, 0, sizeof(client_out2));
  rv = sock_->ExportKeyingMaterial(kKeyingLabel2, false, kKeyingContext1,
                                   client_out2, sizeof(client_out2));
  EXPECT_EQ(rv, OK);
  EXPECT_NE(memcmp(client_out1, client_out2, kKeyingMaterialSize), 0);

  const char kKeyingContext2[] = "context";
  rv = sock_->ExportKeyingMaterial(kKeyingLabel1, true, kKeyingContext2,
                                   client_out2, sizeof(client_out2));
  EXPECT_EQ(rv, OK);
  EXPECT_NE(memcmp(client_out1, client_out2, kKeyingMaterialSize), 0);

  // Prior to TLS 1.3, using an empty context should give different key material
  // from not using a context at all. In TLS 1.3, the distinction is deprecated
  // and they are the same.
  memset(client_out2, 0, sizeof(client_out2));
  rv = sock_->ExportKeyingMaterial(kKeyingLabel1, true, kKeyingContext1,
                                   client_out2, sizeof(client_out2));
  EXPECT_EQ(rv, OK);
  if (version() >= SSL_PROTOCOL_VERSION_TLS1_3) {
    EXPECT_EQ(memcmp(client_out1, client_out2, kKeyingMaterialSize), 0);
  } else {
    EXPECT_NE(memcmp(client_out1, client_out2, kKeyingMaterialSize), 0);
  }
}

TEST(SSLClientSocket, SerializeNextProtos) {
  NextProtoVector next_protos;
  next_protos.push_back(kProtoHTTP11);
  next_protos.push_back(kProtoHTTP2);
  static std::vector<uint8_t> serialized =
      SSLClientSocket::SerializeNextProtos(next_protos);
  ASSERT_EQ(12u, serialized.size());
  EXPECT_EQ(8, serialized[0]);  // length("http/1.1")
  EXPECT_EQ('h', serialized[1]);
  EXPECT_EQ('t', serialized[2]);
  EXPECT_EQ('t', serialized[3]);
  EXPECT_EQ('p', serialized[4]);
  EXPECT_EQ('/', serialized[5]);
  EXPECT_EQ('1', serialized[6]);
  EXPECT_EQ('.', serialized[7]);
  EXPECT_EQ('1', serialized[8]);
  EXPECT_EQ(2, serialized[9]);  // length("h2")
  EXPECT_EQ('h', serialized[10]);
  EXPECT_EQ('2', serialized[11]);
}

// Test that the server certificates are properly retrieved from the underlying
// SSL stack.
TEST_P(SSLClientSocketVersionTest, VerifyServerChainProperlyOrdered) {
  // The connection does not have to be successful.
  cert_verifier_->set_default_result(ERR_CERT_INVALID);

  // Set up a test server with CERT_CHAIN_WRONG_ROOT.
  // This makes the server present redundant-server-chain.pem, which contains
  // intermediate certificates.
  ASSERT_TRUE(StartEmbeddedTestServer(EmbeddedTestServer::CERT_CHAIN_WRONG_ROOT,
                                      GetServerConfig()));

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsError(ERR_CERT_INVALID));
  EXPECT_FALSE(sock_->IsConnected());

  // When given option CERT_CHAIN_WRONG_ROOT, SpawnedTestServer will present
  // certs from redundant-server-chain.pem.
  CertificateList server_certs =
      CreateCertificateListFromFile(GetTestCertsDirectory(),
                                    "redundant-server-chain.pem",
                                    X509Certificate::FORMAT_AUTO);

  // Get the server certificate as received client side.
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  scoped_refptr<X509Certificate> server_certificate = ssl_info.unverified_cert;

  // Get the intermediates as received  client side.
  const auto& server_intermediates = server_certificate->intermediate_buffers();

  // Check that the unverified server certificate chain is properly retrieved
  // from the underlying ssl stack.
  ASSERT_EQ(4U, server_certs.size());

  EXPECT_TRUE(x509_util::CryptoBufferEqual(server_certificate->cert_buffer(),
                                           server_certs[0]->cert_buffer()));

  ASSERT_EQ(3U, server_intermediates.size());

  EXPECT_TRUE(x509_util::CryptoBufferEqual(server_intermediates[0].get(),
                                           server_certs[1]->cert_buffer()));
  EXPECT_TRUE(x509_util::CryptoBufferEqual(server_intermediates[1].get(),
                                           server_certs[2]->cert_buffer()));
  EXPECT_TRUE(x509_util::CryptoBufferEqual(server_intermediates[2].get(),
                                           server_certs[3]->cert_buffer()));

  sock_->Disconnect();
  EXPECT_FALSE(sock_->IsConnected());
}

// This tests that SSLInfo contains a properly re-constructed certificate
// chain. That, in turn, verifies that GetSSLInfo is giving us the chain as
// verified, not the chain as served by the server. (They may be different.)
//
// CERT_CHAIN_WRONG_ROOT is redundant-server-chain.pem. It contains A
// (end-entity) -> B -> C, and C is signed by D. redundant-validated-chain.pem
// contains a chain of A -> B -> C2, where C2 is the same public key as C, but
// a self-signed root. Such a situation can occur when a new root (C2) is
// cross-certified by an old root (D) and has two different versions of its
// floating around. Servers may supply C2 as an intermediate, but the
// SSLClientSocket should return the chain that was verified, from
// verify_result, instead.
TEST_P(SSLClientSocketVersionTest, VerifyReturnChainProperlyOrdered) {
  // By default, cause the CertVerifier to treat all certificates as
  // expired.
  cert_verifier_->set_default_result(ERR_CERT_DATE_INVALID);

  CertificateList unverified_certs = CreateCertificateListFromFile(
      GetTestCertsDirectory(), "redundant-server-chain.pem",
      X509Certificate::FORMAT_AUTO);
  ASSERT_EQ(4u, unverified_certs.size());

  // We will expect SSLInfo to ultimately contain this chain.
  CertificateList certs =
      CreateCertificateListFromFile(GetTestCertsDirectory(),
                                    "redundant-validated-chain.pem",
                                    X509Certificate::FORMAT_AUTO);
  ASSERT_EQ(3U, certs.size());

  ASSERT_TRUE(certs[0]->EqualsExcludingChain(unverified_certs[0].get()));

  std::vector<bssl::UniquePtr<CRYPTO_BUFFER>> temp_intermediates;
  temp_intermediates.push_back(bssl::UpRef(certs[1]->cert_buffer()));
  temp_intermediates.push_back(bssl::UpRef(certs[2]->cert_buffer()));

  CertVerifyResult verify_result;
  verify_result.verified_cert = X509Certificate::CreateFromBuffer(
      bssl::UpRef(certs[0]->cert_buffer()), std::move(temp_intermediates));
  ASSERT_TRUE(verify_result.verified_cert);

  // Add a rule that maps the server cert (A) to the chain of A->B->C2
  // rather than A->B->C.
  cert_verifier_->AddResultForCert(certs[0].get(), verify_result, OK);

  // Load and install the root for the validated chain.
  scoped_refptr<X509Certificate> root_cert = ImportCertFromFile(
      GetTestCertsDirectory(), "redundant-validated-chain-root.pem");
  ASSERT_NE(static_cast<X509Certificate*>(nullptr), root_cert.get());
  ScopedTestRoot scoped_root(root_cert.get());

  // Set up a test server with CERT_CHAIN_WRONG_ROOT.
  ASSERT_TRUE(StartEmbeddedTestServer(EmbeddedTestServer::CERT_CHAIN_WRONG_ROOT,
                                      GetServerConfig()));

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock_->IsConnected());

  auto entries = log_.GetEntries();
  EXPECT_TRUE(LogContainsEndEvent(entries, -1, NetLogEventType::SSL_CONNECT));

  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  // Verify that SSLInfo contains the corrected re-constructed chain A -> B
  // -> C2.
  ASSERT_TRUE(ssl_info.cert);
  const auto& intermediates = ssl_info.cert->intermediate_buffers();
  ASSERT_EQ(2U, intermediates.size());
  EXPECT_TRUE(x509_util::CryptoBufferEqual(ssl_info.cert->cert_buffer(),
                                           certs[0]->cert_buffer()));
  EXPECT_TRUE(x509_util::CryptoBufferEqual(intermediates[0].get(),
                                           certs[1]->cert_buffer()));
  EXPECT_TRUE(x509_util::CryptoBufferEqual(intermediates[1].get(),
                                           certs[2]->cert_buffer()));

  // Verify that SSLInfo also contains the chain as received from the server.
  ASSERT_TRUE(ssl_info.unverified_cert);
  const auto& served_intermediates =
      ssl_info.unverified_cert->intermediate_buffers();
  ASSERT_EQ(3U, served_intermediates.size());
  EXPECT_TRUE(x509_util::CryptoBufferEqual(ssl_info.cert->cert_buffer(),
                                           unverified_certs[0]->cert_buffer()));
  EXPECT_TRUE(x509_util::CryptoBufferEqual(served_intermediates[0].get(),
                                           unverified_certs[1]->cert_buffer()));
  EXPECT_TRUE(x509_util::CryptoBufferEqual(served_intermediates[1].get(),
                                           unverified_certs[2]->cert_buffer()));
  EXPECT_TRUE(x509_util::CryptoBufferEqual(served_intermediates[2].get(),
                                           unverified_certs[3]->cert_buffer()));

  sock_->Disconnect();
  EXPECT_FALSE(sock_->IsConnected());
}

INSTANTIATE_TEST_SUITE_P(TLSVersion,
                         SSLClientSocketCertRequestInfoTest,
                         ::testing::ValuesIn(GetTLSVersions()));

TEST_P(SSLClientSocketCertRequestInfoTest,
       DontRequestClientCertsIfServerCertInvalid) {
  SSLServerConfig config = GetServerConfig();
  config.client_cert_type = SSLServerConfig::OPTIONAL_CLIENT_CERT;
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_EXPIRED, config));

  cert_verifier_->set_default_result(ERR_CERT_DATE_INVALID);
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsError(ERR_CERT_DATE_INVALID));
}

TEST_P(SSLClientSocketCertRequestInfoTest, NoAuthorities) {
  SSLServerConfig config = GetServerConfig();
  config.client_cert_type = SSLServerConfig::OPTIONAL_CLIENT_CERT;
  ASSERT_TRUE(StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, config));
  scoped_refptr<SSLCertRequestInfo> request_info = GetCertRequest();
  ASSERT_TRUE(request_info.get());
  EXPECT_EQ(0u, request_info->cert_authorities.size());
}

TEST_P(SSLClientSocketCertRequestInfoTest, TwoAuthorities) {
  const unsigned char kThawteDN[] = {
      0x30, 0x4c, 0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13,
      0x02, 0x5a, 0x41, 0x31, 0x25, 0x30, 0x23, 0x06, 0x03, 0x55, 0x04, 0x0a,
      0x13, 0x1c, 0x54, 0x68, 0x61, 0x77, 0x74, 0x65, 0x20, 0x43, 0x6f, 0x6e,
      0x73, 0x75, 0x6c, 0x74, 0x69, 0x6e, 0x67, 0x20, 0x28, 0x50, 0x74, 0x79,
      0x29, 0x20, 0x4c, 0x74, 0x64, 0x2e, 0x31, 0x16, 0x30, 0x14, 0x06, 0x03,
      0x55, 0x04, 0x03, 0x13, 0x0d, 0x54, 0x68, 0x61, 0x77, 0x74, 0x65, 0x20,
      0x53, 0x47, 0x43, 0x20, 0x43, 0x41};

  const unsigned char kDiginotarDN[] = {
      0x30, 0x5f, 0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13,
      0x02, 0x4e, 0x4c, 0x31, 0x12, 0x30, 0x10, 0x06, 0x03, 0x55, 0x04, 0x0a,
      0x13, 0x09, 0x44, 0x69, 0x67, 0x69, 0x4e, 0x6f, 0x74, 0x61, 0x72, 0x31,
      0x1a, 0x30, 0x18, 0x06, 0x03, 0x55, 0x04, 0x03, 0x13, 0x11, 0x44, 0x69,
      0x67, 0x69, 0x4e, 0x6f, 0x74, 0x61, 0x72, 0x20, 0x52, 0x6f, 0x6f, 0x74,
      0x20, 0x43, 0x41, 0x31, 0x20, 0x30, 0x1e, 0x06, 0x09, 0x2a, 0x86, 0x48,
      0x86, 0xf7, 0x0d, 0x01, 0x09, 0x01, 0x16, 0x11, 0x69, 0x6e, 0x66, 0x6f,
      0x40, 0x64, 0x69, 0x67, 0x69, 0x6e, 0x6f, 0x74, 0x61, 0x72, 0x2e, 0x6e,
      0x6c};

  SSLServerConfig config = GetServerConfig();
  config.client_cert_type = SSLServerConfig::OPTIONAL_CLIENT_CERT;
  config.cert_authorities.push_back(
      std::string(std::begin(kThawteDN), std::end(kThawteDN)));
  config.cert_authorities.push_back(
      std::string(std::begin(kDiginotarDN), std::end(kDiginotarDN)));
  ASSERT_TRUE(StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, config));
  scoped_refptr<SSLCertRequestInfo> request_info = GetCertRequest();
  ASSERT_TRUE(request_info.get());
  EXPECT_EQ(config.cert_authorities, request_info->cert_authorities);
}

TEST_P(SSLClientSocketCertRequestInfoTest, CertKeyTypes) {
  SpawnedTestServer::SSLOptions ssl_options;
  auto tls_max_version = ProtocolVersionToSpawnedTestServer(version());
  if (!tls_max_version) {
    return;
  }
  ssl_options.tls_max_version = *tls_max_version;
  ssl_options.request_client_certificate = true;
  ssl_options.client_cert_types.push_back(CLIENT_CERT_RSA_SIGN);
  ssl_options.client_cert_types.push_back(CLIENT_CERT_ECDSA_SIGN);
  ASSERT_TRUE(StartTestServer(ssl_options));
  scoped_refptr<SSLCertRequestInfo> request_info = GetCertRequest();
  ASSERT_TRUE(request_info.get());
  ASSERT_EQ(2u, request_info->cert_key_types.size());
  EXPECT_EQ(CLIENT_CERT_RSA_SIGN, request_info->cert_key_types[0]);
  EXPECT_EQ(CLIENT_CERT_ECDSA_SIGN, request_info->cert_key_types[1]);
}

// Tests that the Certificate Transparency (RFC 6962) TLS extension is
// supported.
TEST_F(SSLClientSocketTest, ConnectSignedCertTimestampsTLSExtension) {
  // Encoding of SCT List containing 'test'.
  base::StringPiece sct_ext("\x00\x06\x00\x04test", 8);

  SpawnedTestServer::SSLOptions ssl_options;
  ssl_options.signed_cert_timestamps_tls_ext = sct_ext.as_string();
  ASSERT_TRUE(StartTestServer(ssl_options));

  MockCTVerifier ct_verifier;
  context_ = std::make_unique<SSLClientContext>(
      ssl_config_service_.get(), cert_verifier_.get(),
      transport_security_state_.get(), &ct_verifier, ct_policy_enforcer_.get(),
      ssl_client_session_cache_.get());

  // Check that the SCT list is extracted from the TLS extension as expected,
  // while also simulating that it was an unparsable response.
  SignedCertificateTimestampAndStatusList sct_list;
  EXPECT_CALL(ct_verifier, Verify(_, _, _, sct_ext, _, _))
      .WillOnce(testing::SetArgPointee<4>(sct_list));

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsOk());

  EXPECT_TRUE(sock_->signed_cert_timestamps_received_);

  sock_ = nullptr;
  context_ = nullptr;
}

// Test that when a CT verifier and a CTPolicyEnforcer are defined, and
// the EV certificate used conforms to the CT/EV policy, its EV status
// is maintained.
TEST_P(SSLClientSocketVersionTest, EVCertStatusMaintainedForCompliantCert) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  SSLConfig ssl_config;
  AddServerCertStatusToSSLConfig(CERT_STATUS_IS_EV, &ssl_config);

  // Emulate compliance of the certificate to the policy.
  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(_, _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_COMPLIES_VIA_SCTS));

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  EXPECT_THAT(rv, IsOk());

  SSLInfo result;
  ASSERT_TRUE(sock_->GetSSLInfo(&result));

  EXPECT_TRUE(result.cert_status & CERT_STATUS_IS_EV);
}

// Test that when a CT verifier and a CTPolicyEnforcer are defined, but
// the EV certificate used does not conform to the CT/EV policy, its EV status
// is removed.
TEST_P(SSLClientSocketVersionTest, EVCertStatusRemovedForNonCompliantCert) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  SSLConfig ssl_config;
  AddServerCertStatusToSSLConfig(CERT_STATUS_IS_EV, &ssl_config);

  // Emulate non-compliance of the certificate to the policy.
  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(_, _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_NOT_ENOUGH_SCTS));

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  EXPECT_THAT(rv, IsOk());

  SSLInfo result;
  ASSERT_TRUE(sock_->GetSSLInfo(&result));

  EXPECT_FALSE(result.cert_status & CERT_STATUS_IS_EV);
  EXPECT_TRUE(result.cert_status & CERT_STATUS_CT_COMPLIANCE_FAILED);
}

// Test that when an EV certificate does not conform to the CT policy and its EV
// status is removed, the corresponding histogram is recorded correctly.
TEST_P(SSLClientSocketVersionTest, NonCTCompliantEVHistogram) {
  const char kHistogramName[] = "Net.CertificateTransparency.EVCompliance2.SSL";
  base::HistogramTester histograms;
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // Certificate is trusted and chains to a public root.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = true;
  verify_result.verified_cert = server_cert;
  verify_result.cert_status = CERT_STATUS_IS_EV;
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  // Emulate non-compliance of the certificate to the policy.
  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(_, _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_NOT_ENOUGH_SCTS));

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  EXPECT_THAT(rv, IsOk());

  SSLInfo result;
  ASSERT_TRUE(sock_->GetSSLInfo(&result));

  EXPECT_FALSE(result.cert_status & CERT_STATUS_IS_EV);
  // The histogram should have been recorded with the CT compliance status.
  histograms.ExpectUniqueSample(
      kHistogramName,
      static_cast<int>(ct::CTPolicyCompliance::CT_POLICY_NOT_ENOUGH_SCTS), 1);
}

// Test that when an EV certificate does conform to the CT policy and its EV
// status is not removed, the corresponding histogram is recorded correctly.
TEST_P(SSLClientSocketVersionTest, CTCompliantEVHistogram) {
  const char kHistogramName[] = "Net.CertificateTransparency.EVCompliance2.SSL";
  base::HistogramTester histograms;
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // Certificate is trusted and chains to a public root.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = true;
  verify_result.verified_cert = server_cert;
  verify_result.cert_status = CERT_STATUS_IS_EV;
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  // Emulate non-compliance of the certificate to the policy.
  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(_, _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_COMPLIES_VIA_SCTS));

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  EXPECT_THAT(rv, IsOk());

  SSLInfo result;
  ASSERT_TRUE(sock_->GetSSLInfo(&result));

  EXPECT_TRUE(result.cert_status & CERT_STATUS_IS_EV);
  // The histogram should have been recorded with the CT compliance status.
  histograms.ExpectUniqueSample(
      kHistogramName,
      static_cast<int>(ct::CTPolicyCompliance::CT_POLICY_COMPLIES_VIA_SCTS), 1);
}

// Tests that OCSP stapling is requested, as per Certificate Transparency (RFC
// 6962).
TEST_F(SSLClientSocketTest, ConnectSignedCertTimestampsEnablesOCSP) {
  SpawnedTestServer::SSLOptions ssl_options;
  ssl_options.staple_ocsp_response = true;
  // The test server currently only knows how to generate OCSP responses
  // for a freshly minted certificate.
  ssl_options.server_certificate = SpawnedTestServer::SSLOptions::CERT_AUTO;

  ASSERT_TRUE(StartTestServer(ssl_options));

  SSLConfig ssl_config;

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  EXPECT_THAT(rv, IsOk());

  EXPECT_TRUE(sock_->stapled_ocsp_response_received_);
}

// Tests that IsConnectedAndIdle and WasEverUsed behave as expected.
TEST_P(SSLClientSocketVersionTest, ReuseStates) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));

  // The socket was just connected. It should be idle because it is speaking
  // HTTP. Although the transport has been used for the handshake, WasEverUsed()
  // returns false.
  EXPECT_TRUE(sock_->IsConnected());
  EXPECT_TRUE(sock_->IsConnectedAndIdle());
  EXPECT_FALSE(sock_->WasEverUsed());

  const char kRequestText[] = "GET / HTTP/1.0\r\n\r\n";
  const size_t kRequestLen = base::size(kRequestText) - 1;
  scoped_refptr<IOBuffer> request_buffer =
      base::MakeRefCounted<IOBuffer>(kRequestLen);
  memcpy(request_buffer->data(), kRequestText, kRequestLen);

  TestCompletionCallback callback;
  rv = callback.GetResult(sock_->Write(request_buffer.get(), kRequestLen,
                                       callback.callback(),
                                       TRAFFIC_ANNOTATION_FOR_TESTS));
  EXPECT_EQ(static_cast<int>(kRequestLen), rv);

  // The socket has now been used.
  EXPECT_TRUE(sock_->WasEverUsed());

  // TODO(davidben): Read one byte to ensure the test server has responded and
  // then assert IsConnectedAndIdle is false. This currently doesn't work
  // because SSLClientSocketImpl doesn't check the implementation's internal
  // buffer. Call SSL_pending.
}

// Tests that |is_fatal_cert_error| does not get set for a certificate error,
// on a non-HSTS host.
TEST_P(SSLClientSocketVersionTest, IsFatalErrorNotSetOnNonFatalError) {
  cert_verifier_->set_default_result(ERR_CERT_DATE_INVALID);
  ASSERT_TRUE(StartEmbeddedTestServer(EmbeddedTestServer::CERT_CHAIN_WRONG_ROOT,
                                      GetServerConfig()));
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_FALSE(ssl_info.is_fatal_cert_error);
}

// Tests that |is_fatal_cert_error| gets set for a certificate error on an
// HSTS host.
TEST_P(SSLClientSocketVersionTest, IsFatalErrorSetOnFatalError) {
  cert_verifier_->set_default_result(ERR_CERT_DATE_INVALID);
  ASSERT_TRUE(StartEmbeddedTestServer(EmbeddedTestServer::CERT_CHAIN_WRONG_ROOT,
                                      GetServerConfig()));
  int rv;
  const base::Time expiry =
      base::Time::Now() + base::TimeDelta::FromSeconds(1000);
  transport_security_state_->AddHSTS(host_port_pair().host(), expiry, true);
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_TRUE(ssl_info.is_fatal_cert_error);
}

// Tests that IsConnectedAndIdle treats a socket as idle even if a Write hasn't
// been flushed completely out of SSLClientSocket's internal buffers. This is a
// regression test for https://crbug.com/466147.
TEST_P(SSLClientSocketVersionTest, ReusableAfterWrite) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  std::unique_ptr<FakeBlockingStreamSocket> transport(
      new FakeBlockingStreamSocket(std::move(real_transport)));
  FakeBlockingStreamSocket* raw_transport = transport.get();
  ASSERT_THAT(callback.GetResult(transport->Connect(callback.callback())),
              IsOk());

  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));
  ASSERT_THAT(callback.GetResult(sock->Connect(callback.callback())), IsOk());

  // Block any application data from reaching the network.
  raw_transport->BlockWrite();

  // Write a partial HTTP request.
  const char kRequestText[] = "GET / HTTP/1.0";
  const size_t kRequestLen = base::size(kRequestText) - 1;
  scoped_refptr<IOBuffer> request_buffer =
      base::MakeRefCounted<IOBuffer>(kRequestLen);
  memcpy(request_buffer->data(), kRequestText, kRequestLen);

  // Although transport writes are blocked, SSLClientSocketImpl completes the
  // outer Write operation.
  EXPECT_EQ(static_cast<int>(kRequestLen),
            callback.GetResult(sock->Write(request_buffer.get(), kRequestLen,
                                           callback.callback(),
                                           TRAFFIC_ANNOTATION_FOR_TESTS)));

  // The Write operation is complete, so the socket should be treated as
  // reusable, in case the server returns an HTTP response before completely
  // consuming the request body. In this case, we assume the server will
  // properly drain the request body before trying to read the next request.
  EXPECT_TRUE(sock->IsConnectedAndIdle());
}

// Tests that basic session resumption works.
TEST_P(SSLClientSocketVersionTest, SessionResumption) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  // First, perform a full handshake.
  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_FULL, ssl_info.handshake_type);

  // TLS 1.2 with False Start and TLS 1.3 cause the ticket to arrive later, so
  // use the socket to ensure the session ticket has been picked up.
  EXPECT_THAT(MakeHTTPRequest(sock_.get()), IsOk());

  // The next connection should resume.
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
  sock_.reset();

  // Using a different HostPortPair uses a different session cache key.
  std::unique_ptr<StreamSocket> transport(
      new TCPClientSocket(addr(), nullptr, &log_, NetLogSource()));
  TestCompletionCallback callback;
  ASSERT_THAT(callback.GetResult(transport->Connect(callback.callback())),
              IsOk());
  std::unique_ptr<SSLClientSocket> sock = CreateSSLClientSocket(
      std::move(transport), HostPortPair("example.com", 443), ssl_config);
  ASSERT_THAT(callback.GetResult(sock->Connect(callback.callback())), IsOk());
  ASSERT_TRUE(sock->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_FULL, ssl_info.handshake_type);
  sock.reset();

  ssl_client_session_cache_->Flush();

  // After clearing the session cache, the next handshake doesn't resume.
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_FULL, ssl_info.handshake_type);
}

namespace {

// FakePeerAddressSocket wraps a |StreamSocket|, forwarding all calls except
// that it provides a given answer for |GetPeerAddress|.
class FakePeerAddressSocket : public WrappedStreamSocket {
 public:
  FakePeerAddressSocket(std::unique_ptr<StreamSocket> socket,
                        const IPEndPoint& address)
      : WrappedStreamSocket(std::move(socket)), address_(address) {}
  ~FakePeerAddressSocket() override {}

  int GetPeerAddress(IPEndPoint* address) const override {
    *address = address_;
    return OK;
  }

 private:
  const IPEndPoint address_;
};

}  // namespace

TEST_F(SSLClientSocketTest, SessionResumption_RSA) {
  for (bool use_rsa : {false, true}) {
    SCOPED_TRACE(use_rsa);

    SpawnedTestServer::SSLOptions ssl_options;
    ssl_options.key_exchanges =
        use_rsa ? SpawnedTestServer::SSLOptions::KEY_EXCHANGE_RSA
                : SpawnedTestServer::SSLOptions::KEY_EXCHANGE_ECDHE_RSA;
    ASSERT_TRUE(StartTestServer(ssl_options));
    SSLConfig ssl_config;
    ssl_client_session_cache_->Flush();

    for (int i = 0; i < 3; i++) {
      SCOPED_TRACE(i);

      std::unique_ptr<StreamSocket> transport(
          new TCPClientSocket(addr(), nullptr, &log_, NetLogSource()));
      TestCompletionCallback callback;
      ASSERT_THAT(callback.GetResult(transport->Connect(callback.callback())),
                  IsOk());
      // The third handshake sees a different destination IP address.
      IPEndPoint fake_peer_address(IPAddress(1, 1, 1, i == 2 ? 2 : 1), 443);
      auto socket = std::make_unique<FakePeerAddressSocket>(
          std::move(transport), fake_peer_address);
      std::unique_ptr<SSLClientSocket> sock = CreateSSLClientSocket(
          std::move(socket), HostPortPair("example.com", 443), ssl_config);
      ASSERT_THAT(callback.GetResult(sock->Connect(callback.callback())),
                  IsOk());
      SSLInfo ssl_info;
      ASSERT_TRUE(sock->GetSSLInfo(&ssl_info));
      sock.reset();

      switch (i) {
        case 0:
          // Initial handshake should be a full handshake.
          EXPECT_EQ(SSLInfo::HANDSHAKE_FULL, ssl_info.handshake_type);
          break;
        case 1:
          // Second handshake should resume.
          EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
          break;
        case 2:
          // Third handshake gets a different IP address and, if the
          // session used RSA key exchange, it should not resume.
          EXPECT_EQ(
              use_rsa ? SSLInfo::HANDSHAKE_FULL : SSLInfo::HANDSHAKE_RESUME,
              ssl_info.handshake_type);
          break;
        default:
          NOTREACHED();
      }
    }
  }
}

// Tests that ALPN works with session resumption.
// Failed on Android, see https://crbug.com/1014556.
#if defined(OS_ANDROID)
#define MAYBE_SessionResumptionAlpn DISABLED_SessionResumptionAlpn
#else
#define MAYBE_SessionResumptionAlpn SessionResumptionAlpn
#endif
TEST_F(SSLClientSocketTest, MAYBE_SessionResumptionAlpn) {
  SpawnedTestServer::SSLOptions ssl_options;
  ssl_options.alpn_protocols.push_back("h2");
  ssl_options.alpn_protocols.push_back("http/1.1");
  ASSERT_TRUE(StartTestServer(ssl_options));

  // First, perform a full handshake.
  SSLConfig ssl_config;
  ssl_config.alpn_protos.push_back(kProtoHTTP2);
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_FULL, ssl_info.handshake_type);
  EXPECT_EQ(kProtoHTTP2, sock_->GetNegotiatedProtocol());

  // TLS 1.2 with False Start and TLS 1.3 cause the ticket to arrive later, so
  // use the socket to ensure the session ticket has been picked up.
  EXPECT_THAT(MakeHTTPRequest(sock_.get()), IsOk());

  // The next connection should resume; ALPN should be renegotiated.
  ssl_config.alpn_protos.clear();
  ssl_config.alpn_protos.push_back(kProtoHTTP11);
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
  EXPECT_EQ(kProtoHTTP11, sock_->GetNegotiatedProtocol());
}

// Tests that the session cache is not sharded by NetworkIsolationKey if the
// feature is disabled.
TEST_P(SSLClientSocketVersionTest,
       SessionResumptionNetworkIsolationKeyDisabled) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndDisableFeature(
      features::kPartitionSSLSessionsByNetworkIsolationKey);

  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  // First, perform a full handshake.
  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_FULL, ssl_info.handshake_type);

  // TLS 1.2 with False Start and TLS 1.3 cause the ticket to arrive later, so
  // use the socket to ensure the session ticket has been picked up. Do this for
  // every connection to avoid problems with TLS 1.3 single-use tickets.
  EXPECT_THAT(MakeHTTPRequest(sock_.get()), IsOk());

  // The next connection should resume.
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
  EXPECT_THAT(MakeHTTPRequest(sock_.get()), IsOk());
  sock_.reset();

  // Using a different NetworkIsolationKey shares session cache key because
  // sharding is disabled.
  const auto kOriginA = url::Origin::Create(GURL("https://a.test"));
  ssl_config.network_isolation_key = NetworkIsolationKey(kOriginA, kOriginA);
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
  EXPECT_THAT(MakeHTTPRequest(sock_.get()), IsOk());
  sock_.reset();

  const auto kOriginB = url::Origin::Create(GURL("https://a.test"));
  ssl_config.network_isolation_key = NetworkIsolationKey(kOriginB, kOriginB);
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
  EXPECT_THAT(MakeHTTPRequest(sock_.get()), IsOk());
  sock_.reset();
}

// Tests that the session cache is sharded by NetworkIsolationKey if the
// feature is enabled.
TEST_P(SSLClientSocketVersionTest,
       SessionResumptionNetworkIsolationKeyEnabled) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      features::kPartitionSSLSessionsByNetworkIsolationKey);

  const auto kOriginA = url::Origin::Create(GURL("https://a.test"));
  const auto kOriginB = url::Origin::Create(GURL("https://b.test"));
  const NetworkIsolationKey kNetworkIsolationKeyA(kOriginA, kOriginA);
  const NetworkIsolationKey kNetworkIsolationKeyB(kOriginB, kOriginB);

  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  // First, perform a full handshake.
  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_FULL, ssl_info.handshake_type);

  // TLS 1.2 with False Start and TLS 1.3 cause the ticket to arrive later, so
  // use the socket to ensure the session ticket has been picked up. Do this for
  // every connection to avoid problems with TLS 1.3 single-use tickets.
  EXPECT_THAT(MakeHTTPRequest(sock_.get()), IsOk());

  // The next connection should resume.
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
  EXPECT_THAT(MakeHTTPRequest(sock_.get()), IsOk());
  sock_.reset();

  // Using a different NetworkIsolationKey uses a different session cache key.
  ssl_config.network_isolation_key = kNetworkIsolationKeyA;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_FULL, ssl_info.handshake_type);
  EXPECT_THAT(MakeHTTPRequest(sock_.get()), IsOk());
  sock_.reset();

  // We, however, can resume under that newly-established session.
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
  EXPECT_THAT(MakeHTTPRequest(sock_.get()), IsOk());
  sock_.reset();

  // Repeat with another non-null key.
  ssl_config.network_isolation_key = kNetworkIsolationKeyB;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_FULL, ssl_info.handshake_type);
  EXPECT_THAT(MakeHTTPRequest(sock_.get()), IsOk());
  sock_.reset();

  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
  EXPECT_THAT(MakeHTTPRequest(sock_.get()), IsOk());
  sock_.reset();

  // b.test does not evict a.test's session.
  ssl_config.network_isolation_key = kNetworkIsolationKeyA;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
  EXPECT_THAT(MakeHTTPRequest(sock_.get()), IsOk());
  sock_.reset();
}

// Tests that connections with certificate errors do not add entries to the
// session cache.
TEST_P(SSLClientSocketVersionTest, CertificateErrorNoResume) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  cert_verifier_->set_default_result(ERR_CERT_COMMON_NAME_INVALID);

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsError(ERR_CERT_COMMON_NAME_INVALID));

  cert_verifier_->set_default_result(OK);

  // The next connection should perform a full handshake.
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_THAT(rv, IsOk());
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_FULL, ssl_info.handshake_type);
}

// Test that DHE is removed.
TEST_F(SSLClientSocketTest, NoDHE) {
  SpawnedTestServer::SSLOptions ssl_options;
  ssl_options.key_exchanges =
      SpawnedTestServer::SSLOptions::KEY_EXCHANGE_DHE_RSA;
  ASSERT_TRUE(StartTestServer(ssl_options));

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  EXPECT_THAT(rv, IsError(ERR_SSL_VERSION_OR_CIPHER_MISMATCH));
}

TEST_F(SSLClientSocketTest, RequireECDHE) {
  // Run test server without ECDHE.
  SpawnedTestServer::SSLOptions ssl_options;
  ssl_options.key_exchanges = SpawnedTestServer::SSLOptions::KEY_EXCHANGE_RSA;
  ASSERT_TRUE(StartTestServer(ssl_options));

  SSLConfig config;
  config.require_ecdhe = true;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(config, &rv));
  EXPECT_THAT(rv, IsError(ERR_SSL_VERSION_OR_CIPHER_MISMATCH));
}

TEST_F(SSLClientSocketFalseStartTest, FalseStartEnabled) {
  // False Start requires ALPN, ECDHE, and an AEAD.
  SpawnedTestServer::SSLOptions server_options;
  server_options.key_exchanges =
      SpawnedTestServer::SSLOptions::KEY_EXCHANGE_ECDHE_RSA;
  server_options.bulk_ciphers =
      SpawnedTestServer::SSLOptions::BULK_CIPHER_AES128GCM;
  server_options.alpn_protocols.push_back("http/1.1");
  SSLConfig client_config;
  client_config.alpn_protos.push_back(kProtoHTTP11);
  ASSERT_NO_FATAL_FAILURE(TestFalseStart(server_options, client_config, true));
}

// Test that False Start is disabled without ALPN.
TEST_F(SSLClientSocketFalseStartTest, NoAlpn) {
  SpawnedTestServer::SSLOptions server_options;
  server_options.key_exchanges =
      SpawnedTestServer::SSLOptions::KEY_EXCHANGE_ECDHE_RSA;
  server_options.bulk_ciphers =
      SpawnedTestServer::SSLOptions::BULK_CIPHER_AES128GCM;
  SSLConfig client_config;
  client_config.alpn_protos.clear();
  ASSERT_NO_FATAL_FAILURE(
      TestFalseStart(server_options, client_config, false));
}

// Test that False Start is disabled with plain RSA ciphers.
TEST_F(SSLClientSocketFalseStartTest, RSA) {
  SpawnedTestServer::SSLOptions server_options;
  server_options.key_exchanges =
      SpawnedTestServer::SSLOptions::KEY_EXCHANGE_RSA;
  server_options.bulk_ciphers =
      SpawnedTestServer::SSLOptions::BULK_CIPHER_AES128GCM;
  server_options.alpn_protocols.push_back("http/1.1");
  SSLConfig client_config;
  client_config.alpn_protos.push_back(kProtoHTTP11);
  ASSERT_NO_FATAL_FAILURE(
      TestFalseStart(server_options, client_config, false));
}

// Test that False Start is disabled without an AEAD.
TEST_F(SSLClientSocketFalseStartTest, NoAEAD) {
  SpawnedTestServer::SSLOptions server_options;
  server_options.key_exchanges =
      SpawnedTestServer::SSLOptions::KEY_EXCHANGE_ECDHE_RSA;
  server_options.bulk_ciphers =
      SpawnedTestServer::SSLOptions::BULK_CIPHER_AES128;
  server_options.alpn_protocols.push_back("http/1.1");
  SSLConfig client_config;
  client_config.alpn_protos.push_back(kProtoHTTP11);
  ASSERT_NO_FATAL_FAILURE(TestFalseStart(server_options, client_config, false));
}

// Test that sessions are resumable after receiving the server Finished message.
TEST_F(SSLClientSocketFalseStartTest, SessionResumption) {
  // Start a server.
  SpawnedTestServer::SSLOptions server_options;
  server_options.key_exchanges =
      SpawnedTestServer::SSLOptions::KEY_EXCHANGE_ECDHE_RSA;
  server_options.bulk_ciphers =
      SpawnedTestServer::SSLOptions::BULK_CIPHER_AES128GCM;
  server_options.alpn_protocols.push_back("http/1.1");
  SSLConfig client_config;
  client_config.alpn_protos.push_back(kProtoHTTP11);

  // Let a full handshake complete with False Start.
  ASSERT_NO_FATAL_FAILURE(
      TestFalseStart(server_options, client_config, true));

  // Make a second connection.
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(client_config, &rv));
  EXPECT_THAT(rv, IsOk());

  // It should resume the session.
  SSLInfo ssl_info;
  EXPECT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
}

// Test that the client completes the handshake in the background and installs
// new sessions, even if the socket isn't used. This also avoids a theoretical
// deadlock if NewSessionTicket is sufficiently large that neither it nor the
// client's HTTP/1.1 POST fit in transport windows.
TEST_F(SSLClientSocketFalseStartTest, CompleteHandshakeWithoutRequest) {
  // Start a server.
  SpawnedTestServer::SSLOptions server_options;
  server_options.key_exchanges =
      SpawnedTestServer::SSLOptions::KEY_EXCHANGE_ECDHE_RSA;
  server_options.bulk_ciphers =
      SpawnedTestServer::SSLOptions::BULK_CIPHER_AES128GCM;
  server_options.alpn_protocols.push_back("http/1.1");
  ASSERT_TRUE(StartTestServer(server_options));

  SSLConfig client_config;
  client_config.alpn_protos.push_back(kProtoHTTP11);

  // Start a handshake up to the server Finished message.
  TestCompletionCallback callback;
  FakeBlockingStreamSocket* raw_transport = nullptr;
  std::unique_ptr<SSLClientSocket> sock;
  ASSERT_NO_FATAL_FAILURE(CreateAndConnectUntilServerFinishedReceived(
      client_config, &callback, &raw_transport, &sock));

  // Wait for the server Finished to arrive, release it, and allow
  // SSLClientSocket to process it. This should install a session.
  // SpawnedTestServer, however, writes data in small chunks, so, even though it
  // is only sending 51 bytes, it may take a few iterations to complete.
  while (ssl_client_session_cache_->size() == 0) {
    raw_transport->WaitForReadResult();
    raw_transport->UnblockReadResult();
    base::RunLoop().RunUntilIdle();
    raw_transport->BlockReadResult();
  }

  // Drop the old socket. This is needed because the Python test server can't
  // service two sockets in parallel.
  sock.reset();

  // Make a second connection.
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(client_config, &rv));
  EXPECT_THAT(rv, IsOk());

  // It should resume the session.
  SSLInfo ssl_info;
  EXPECT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
}

// Test that False Started sessions are not resumable before receiving the
// server Finished message.
TEST_F(SSLClientSocketFalseStartTest, NoSessionResumptionBeforeFinished) {
  // Start a server.
  SpawnedTestServer::SSLOptions server_options;
  server_options.key_exchanges =
      SpawnedTestServer::SSLOptions::KEY_EXCHANGE_ECDHE_RSA;
  server_options.bulk_ciphers =
      SpawnedTestServer::SSLOptions::BULK_CIPHER_AES128GCM;
  server_options.alpn_protocols.push_back("http/1.1");
  ASSERT_TRUE(StartTestServer(server_options));

  SSLConfig client_config;
  client_config.alpn_protos.push_back(kProtoHTTP11);

  // Start a handshake up to the server Finished message.
  TestCompletionCallback callback;
  FakeBlockingStreamSocket* raw_transport1 = nullptr;
  std::unique_ptr<SSLClientSocket> sock1;
  ASSERT_NO_FATAL_FAILURE(CreateAndConnectUntilServerFinishedReceived(
      client_config, &callback, &raw_transport1, &sock1));
  // Although raw_transport1 has the server Finished blocked, the handshake
  // still completes.
  EXPECT_THAT(callback.WaitForResult(), IsOk());

  // Continue to block the client (|sock1|) from processing the Finished
  // message, but allow it to arrive on the socket. This ensures that, from the
  // server's point of view, it has completed the handshake and added the
  // session to its session cache.
  //
  // The actual read on |sock1| will not complete until the Finished message is
  // processed; however, pump the underlying transport so that it is read from
  // the socket. NOTE: This may flakily pass if the server's final flight
  // doesn't come in one Read.
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  int rv = sock1->Read(buf.get(), 4096, callback.callback());
  EXPECT_THAT(rv, IsError(ERR_IO_PENDING));
  raw_transport1->WaitForReadResult();

  // Drop the old socket. This is needed because the Python test server can't
  // service two sockets in parallel.
  sock1.reset();

  // Start a second connection.
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(client_config, &rv));
  EXPECT_THAT(rv, IsOk());

  // No session resumption because the first connection never received a server
  // Finished message.
  SSLInfo ssl_info;
  EXPECT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_FULL, ssl_info.handshake_type);
}

// Test that False Started sessions are not resumable if the server Finished
// message was bad.
TEST_F(SSLClientSocketFalseStartTest, NoSessionResumptionBadFinished) {
  // Start a server.
  SpawnedTestServer::SSLOptions server_options;
  server_options.key_exchanges =
      SpawnedTestServer::SSLOptions::KEY_EXCHANGE_ECDHE_RSA;
  server_options.bulk_ciphers =
      SpawnedTestServer::SSLOptions::BULK_CIPHER_AES128GCM;
  server_options.alpn_protocols.push_back("http/1.1");
  ASSERT_TRUE(StartTestServer(server_options));

  SSLConfig client_config;
  client_config.alpn_protos.push_back(kProtoHTTP11);

  // Start a handshake up to the server Finished message.
  TestCompletionCallback callback;
  FakeBlockingStreamSocket* raw_transport1 = nullptr;
  std::unique_ptr<SSLClientSocket> sock1;
  ASSERT_NO_FATAL_FAILURE(CreateAndConnectUntilServerFinishedReceived(
      client_config, &callback, &raw_transport1, &sock1));
  // Although raw_transport1 has the server Finished blocked, the handshake
  // still completes.
  EXPECT_THAT(callback.WaitForResult(), IsOk());

  // Continue to block the client (|sock1|) from processing the Finished
  // message, but allow it to arrive on the socket. This ensures that, from the
  // server's point of view, it has completed the handshake and added the
  // session to its session cache.
  //
  // The actual read on |sock1| will not complete until the Finished message is
  // processed; however, pump the underlying transport so that it is read from
  // the socket.
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  int rv = sock1->Read(buf.get(), 4096, callback.callback());
  EXPECT_THAT(rv, IsError(ERR_IO_PENDING));
  raw_transport1->WaitForReadResult();

  // The server's second leg, or part of it, is now received but not yet sent to
  // |sock1|. Before doing so, break the server's second leg.
  int bytes_read = raw_transport1->pending_read_result();
  ASSERT_LT(0, bytes_read);
  raw_transport1->pending_read_buf()->data()[bytes_read - 1]++;

  // Unblock the Finished message. |sock1->Read| should now fail.
  raw_transport1->UnblockReadResult();
  EXPECT_THAT(callback.GetResult(rv), IsError(ERR_SSL_PROTOCOL_ERROR));

  // Drop the old socket. This is needed because the Python test server can't
  // service two sockets in parallel.
  sock1.reset();

  // Start a second connection.
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(client_config, &rv));
  EXPECT_THAT(rv, IsOk());

  // No session resumption because the first connection never received a server
  // Finished message.
  SSLInfo ssl_info;
  EXPECT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_FULL, ssl_info.handshake_type);
}

// Server preference should win in ALPN.
TEST_F(SSLClientSocketTest, Alpn) {
  SpawnedTestServer::SSLOptions server_options;
  server_options.alpn_protocols.push_back("h2");
  server_options.alpn_protocols.push_back("http/1.1");
  ASSERT_TRUE(StartTestServer(server_options));

  SSLConfig client_config;
  client_config.alpn_protos.push_back(kProtoHTTP11);
  client_config.alpn_protos.push_back(kProtoHTTP2);

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(client_config, &rv));
  EXPECT_THAT(rv, IsOk());

  EXPECT_EQ(kProtoHTTP2, sock_->GetNegotiatedProtocol());
}

// If the server supports ALPN but the client does not, then ALPN is not used.
TEST_F(SSLClientSocketTest, AlpnClientDisabled) {
  SpawnedTestServer::SSLOptions server_options;
  server_options.alpn_protocols.push_back("foo");
  ASSERT_TRUE(StartTestServer(server_options));

  SSLConfig client_config;

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(client_config, &rv));
  EXPECT_THAT(rv, IsOk());

  EXPECT_EQ(kProtoUnknown, sock_->GetNegotiatedProtocol());
}

// Connect to a server requesting client authentication, do not send
// any client certificates. It should refuse the connection.
TEST_F(SSLClientSocketTest, NoCert) {
  SpawnedTestServer::SSLOptions ssl_options;
  ssl_options.request_client_certificate = true;
  ASSERT_TRUE(StartTestServer(ssl_options));

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));

  EXPECT_THAT(rv, IsError(ERR_SSL_CLIENT_AUTH_CERT_NEEDED));
  EXPECT_FALSE(sock_->IsConnected());
}

// Connect to a server requesting client authentication, and send it
// an empty certificate.
TEST_F(SSLClientSocketTest, SendEmptyCert) {
  SpawnedTestServer::SSLOptions ssl_options;
  ssl_options.request_client_certificate = true;
  ssl_options.client_authorities.push_back(
      GetTestClientCertsDirectory().AppendASCII("client_1_ca.pem"));

  ASSERT_TRUE(StartTestServer(ssl_options));

  context_->SetClientCertificate(host_port_pair(), nullptr, nullptr);

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));

  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock_->IsConnected());

  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_FALSE(ssl_info.client_cert_sent);
}

// Connect to a server requesting client authentication. Send it a
// matching certificate. It should allow the connection.
TEST_F(SSLClientSocketTest, SendGoodCert) {
  SpawnedTestServer::SSLOptions ssl_options;
  ssl_options.request_client_certificate = true;
  ssl_options.client_authorities.push_back(
      GetTestClientCertsDirectory().AppendASCII("client_1_ca.pem"));

  ASSERT_TRUE(StartTestServer(ssl_options));

  base::FilePath certs_dir = GetTestCertsDirectory();
  context_->SetClientCertificate(
      host_port_pair(), ImportCertFromFile(certs_dir, "client_1.pem"),
      key_util::LoadPrivateKeyOpenSSL(certs_dir.AppendASCII("client_1.key")));

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));

  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock_->IsConnected());

  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_TRUE(ssl_info.client_cert_sent);

  sock_->Disconnect();
  EXPECT_FALSE(sock_->IsConnected());
}

// When client certificate preferences change, the session cache should be
// cleared so the client certificate preferences are applied.
TEST_F(SSLClientSocketTest, ClearSessionCacheOnClientCertChange) {
  SSLServerConfig server_config;
  // TLS 1.3 reports client certificate errors after the handshake, so test at
  // TLS 1.2 for simplicity.
  server_config.version_max = SSL_PROTOCOL_VERSION_TLS1_2;
  server_config.client_cert_type = SSLServerConfig::REQUIRE_CLIENT_CERT;
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, server_config));

  // Connecting without a client certificate will fail with
  // ERR_SSL_CLIENT_AUTH_CERT_NEEDED.
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsError(ERR_SSL_CLIENT_AUTH_CERT_NEEDED));

  // Configure a client certificate.
  base::FilePath certs_dir = GetTestCertsDirectory();
  context_->SetClientCertificate(
      host_port_pair(), ImportCertFromFile(certs_dir, "client_1.pem"),
      key_util::LoadPrivateKeyOpenSSL(certs_dir.AppendASCII("client_1.key")));

  // Now the connection succeeds.
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock_->IsConnected());

  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_TRUE(ssl_info.client_cert_sent);
  EXPECT_EQ(ssl_info.handshake_type, SSLInfo::HANDSHAKE_FULL);

  // Make a second connection. This should resume the session from the previous
  // connection.
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock_->IsConnected());

  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_TRUE(ssl_info.client_cert_sent);
  EXPECT_EQ(ssl_info.handshake_type, SSLInfo::HANDSHAKE_RESUME);

  // Clear the client certificate preference.
  context_->ClearClientCertificate(host_port_pair());

  // Connections return to failing, rather than resume the previous session.
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsError(ERR_SSL_CLIENT_AUTH_CERT_NEEDED));

  // Establish a new session with the correct client certificate.
  context_->SetClientCertificate(
      host_port_pair(), ImportCertFromFile(certs_dir, "client_1.pem"),
      key_util::LoadPrivateKeyOpenSSL(certs_dir.AppendASCII("client_1.key")));
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsOk());
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_TRUE(ssl_info.client_cert_sent);
  EXPECT_EQ(ssl_info.handshake_type, SSLInfo::HANDSHAKE_FULL);

  // Switch to continuing without a client certificate.
  context_->SetClientCertificate(host_port_pair(), nullptr, nullptr);

  // This also clears the session cache and the new preference is applied.
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  EXPECT_THAT(rv, IsError(ERR_BAD_SSL_CLIENT_AUTH_CERT));
}

HashValueVector MakeHashValueVector(uint8_t value) {
  HashValueVector out;
  HashValue hash(HASH_VALUE_SHA256);
  memset(hash.data(), value, hash.size());
  out.push_back(hash);
  return out;
}

// Test that |ssl_info.pkp_bypassed| is set when a local trust anchor causes
// pinning to be bypassed.
TEST_P(SSLClientSocketVersionTest, PKPBypassedSet) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));
  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // The certificate needs to be trusted, but chain to a local root with
  // different public key hashes than specified in the pin.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = false;
  verify_result.verified_cert = server_cert;
  verify_result.public_key_hashes =
      MakeHashValueVector(kBadHashValueVectorInput);
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  transport_security_state_->EnableStaticPinsForTesting();
  ScopedTransportSecurityStateSource scoped_security_state_source;

  SSLConfig ssl_config;
  int rv;
  HostPortPair new_host_port_pair("example.test", host_port_pair().port());
  ASSERT_TRUE(CreateAndConnectSSLClientSocketWithHost(ssl_config,
                                                      new_host_port_pair, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(sock_->IsConnected());

  EXPECT_TRUE(ssl_info.pkp_bypassed);
  EXPECT_FALSE(ssl_info.cert_status & CERT_STATUS_PINNED_KEY_MISSING);
}

TEST_P(SSLClientSocketVersionTest, PKPEnforced) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));
  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // Certificate is trusted, but chains to a public root that doesn't match the
  // pin hashes.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = true;
  verify_result.verified_cert = server_cert;
  verify_result.public_key_hashes =
      MakeHashValueVector(kBadHashValueVectorInput);
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  transport_security_state_->EnableStaticPinsForTesting();
  ScopedTransportSecurityStateSource scoped_security_state_source;

  SSLConfig ssl_config;
  int rv;
  HostPortPair new_host_port_pair("example.test", host_port_pair().port());
  ASSERT_TRUE(CreateAndConnectSSLClientSocketWithHost(ssl_config,
                                                      new_host_port_pair, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  EXPECT_THAT(rv, IsError(ERR_SSL_PINNED_KEY_NOT_IN_CERT_CHAIN));
  EXPECT_TRUE(ssl_info.cert_status & CERT_STATUS_PINNED_KEY_MISSING);
  EXPECT_FALSE(sock_->IsConnected());

  EXPECT_FALSE(ssl_info.pkp_bypassed);
}

struct KeyUsageTest {
  SpawnedTestServer::SSLOptions::ServerCertificate server_cert;
  SpawnedTestServer::SSLOptions::KeyExchange key_exchange;
  bool known_root;
  bool success;
};

class SSLClientSocketKeyUsageTest
    : public SSLClientSocketTest,
      public ::testing::WithParamInterface<struct KeyUsageTest> {};

const struct KeyUsageTest kKeyUsageTests[] = {
    // Known Root: Success iff keyUsage allows the key exchange method
    {SpawnedTestServer::SSLOptions::CERT_KEY_USAGE_RSA_ENCIPHERMENT,
     SpawnedTestServer::SSLOptions::KEY_EXCHANGE_ECDHE_RSA, true, false},
    {SpawnedTestServer::SSLOptions::CERT_KEY_USAGE_RSA_DIGITAL_SIGNATURE,
     SpawnedTestServer::SSLOptions::KEY_EXCHANGE_ECDHE_RSA, true, true},
    {SpawnedTestServer::SSLOptions::CERT_KEY_USAGE_RSA_ENCIPHERMENT,
     SpawnedTestServer::SSLOptions::KEY_EXCHANGE_RSA, true, true},
    {SpawnedTestServer::SSLOptions::CERT_KEY_USAGE_RSA_DIGITAL_SIGNATURE,
     SpawnedTestServer::SSLOptions::KEY_EXCHANGE_RSA, true, false},
    // Unknown Root: Always succeeds
    {SpawnedTestServer::SSLOptions::CERT_KEY_USAGE_RSA_ENCIPHERMENT,
     SpawnedTestServer::SSLOptions::KEY_EXCHANGE_ECDHE_RSA, false, true},
    {SpawnedTestServer::SSLOptions::CERT_KEY_USAGE_RSA_DIGITAL_SIGNATURE,
     SpawnedTestServer::SSLOptions::KEY_EXCHANGE_ECDHE_RSA, false, true},
    {SpawnedTestServer::SSLOptions::CERT_KEY_USAGE_RSA_ENCIPHERMENT,
     SpawnedTestServer::SSLOptions::KEY_EXCHANGE_RSA, false, true},
    {SpawnedTestServer::SSLOptions::CERT_KEY_USAGE_RSA_DIGITAL_SIGNATURE,
     SpawnedTestServer::SSLOptions::KEY_EXCHANGE_RSA, false, true},
};

TEST_P(SSLClientSocketKeyUsageTest, RSAKeyUsageEnforcedForKnownRoot) {
  const KeyUsageTest test = GetParam();
  SpawnedTestServer::SSLOptions ssl_options(test.server_cert);
  ssl_options.key_exchanges = test.key_exchange;
  ASSERT_TRUE(StartTestServer(ssl_options));
  scoped_refptr<X509Certificate> server_cert =
      spawned_test_server()->GetCertificate();

  // Certificate is trusted.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = test.known_root;
  verify_result.verified_cert = server_cert;
  verify_result.public_key_hashes =
      MakeHashValueVector(kGoodHashValueVectorInput);
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  if (test.success) {
    EXPECT_THAT(rv, IsOk());
    EXPECT_TRUE(sock_->IsConnected());
  } else {
    EXPECT_THAT(rv, IsError(ERR_SSL_KEY_USAGE_INCOMPATIBLE));
    EXPECT_FALSE(sock_->IsConnected());
  }
}

INSTANTIATE_TEST_SUITE_P(RSAKeyUsageInstantiation,
                         SSLClientSocketKeyUsageTest,
                         ::testing::ValuesIn(kKeyUsageTests));

// Test that when CT is required (in this case, by the delegate), the
// absence of CT information is a socket error.
TEST_P(SSLClientSocketVersionTest, CTIsRequired) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));
  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // Certificate is trusted and chains to a public root.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = true;
  verify_result.verified_cert = server_cert;
  verify_result.public_key_hashes =
      MakeHashValueVector(kGoodHashValueVectorInput);
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  // Set up CT
  MockRequireCTDelegate require_ct_delegate;
  transport_security_state_->SetRequireCTDelegate(&require_ct_delegate);
  EXPECT_CALL(require_ct_delegate, IsCTRequiredForHost(_, _, _))
      .WillRepeatedly(Return(TransportSecurityState::RequireCTDelegate::
                                 CTRequirementLevel::NOT_REQUIRED));
  EXPECT_CALL(require_ct_delegate,
              IsCTRequiredForHost(host_port_pair().host(), _, _))
      .WillRepeatedly(Return(TransportSecurityState::RequireCTDelegate::
                                 CTRequirementLevel::REQUIRED));
  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_NOT_ENOUGH_SCTS));

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  EXPECT_THAT(rv, IsError(ERR_CERTIFICATE_TRANSPARENCY_REQUIRED));
  EXPECT_TRUE(ssl_info.cert_status &
              CERT_STATUS_CERTIFICATE_TRANSPARENCY_REQUIRED);
  EXPECT_FALSE(sock_->IsConnected());
}

// Test that when CT is required, setting ignore_certificate_errors
// ignores errors in CT.
TEST_P(SSLClientSocketVersionTest, IgnoreCertificateErrorsBypassesRequiredCT) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));
  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // Certificate is trusted and chains to a public root.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = true;
  verify_result.verified_cert = server_cert;
  verify_result.public_key_hashes =
      MakeHashValueVector(kGoodHashValueVectorInput);
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  // Set up CT
  MockRequireCTDelegate require_ct_delegate;
  transport_security_state_->SetRequireCTDelegate(&require_ct_delegate);
  EXPECT_CALL(require_ct_delegate, IsCTRequiredForHost(_, _, _))
      .WillRepeatedly(Return(TransportSecurityState::RequireCTDelegate::
                                 CTRequirementLevel::NOT_REQUIRED));
  EXPECT_CALL(require_ct_delegate,
              IsCTRequiredForHost(host_port_pair().host(), _, _))
      .WillRepeatedly(Return(TransportSecurityState::RequireCTDelegate::
                                 CTRequirementLevel::REQUIRED));
  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_NOT_ENOUGH_SCTS));

  SSLConfig ssl_config;
  ssl_config.ignore_certificate_errors = true;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  EXPECT_THAT(rv, IsOk());
  EXPECT_TRUE(ssl_info.cert_status &
              CERT_STATUS_CERTIFICATE_TRANSPARENCY_REQUIRED);
  EXPECT_TRUE(sock_->IsConnected());
}

// Test that the CT compliance status is recorded in a histogram.
TEST_P(SSLClientSocketVersionTest, CTComplianceStatusHistogram) {
  const char kHistogramName[] =
      "Net.CertificateTransparency.ConnectionComplianceStatus2.SSL";
  base::HistogramTester histograms;

  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));
  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // Certificate is trusted.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = true;
  verify_result.verified_cert = server_cert;
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  // Set up CT.
  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_NOT_DIVERSE_SCTS));

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  // The histogram should have been recorded with the CT compliance status.
  histograms.ExpectUniqueSample(
      kHistogramName,
      static_cast<int>(ct::CTPolicyCompliance::CT_POLICY_NOT_DIVERSE_SCTS), 1);
}

// Test that the CT compliance status histogram is not recorded for
// locally-installed roots.
TEST_P(SSLClientSocketVersionTest, CTComplianceStatusHistogramLocalRoot) {
  const char kHistogramName[] =
      "Net.CertificateTransparency.ConnectionComplianceStatus2.SSL";
  base::HistogramTester histograms;

  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));
  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // Certificate is trusted but chains to a local root.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = false;
  verify_result.verified_cert = server_cert;
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  // Set up CT.
  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_NOT_DIVERSE_SCTS));

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  histograms.ExpectTotalCount(kHistogramName, 0);
}

// Test that when CT is required (in this case, by an Expect-CT opt-in) and the
// connection is compliant, the histogram for CT-required connections is
// recorded properly.
TEST_P(SSLClientSocketVersionTest, CTRequiredHistogramCompliant) {
  const char kHistogramName[] =
      "Net.CertificateTransparency.CTRequiredConnectionComplianceStatus2.SSL";
  base::HistogramTester histograms;

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);

  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));
  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // Certificate is trusted and chains to a public root.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = true;
  verify_result.verified_cert = server_cert;
  verify_result.public_key_hashes =
      MakeHashValueVector(kGoodHashValueVectorInput);
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  // Set up the Expect-CT opt-in.
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  transport_security_state_->AddExpectCT(host_port_pair().host(), expiry,
                                         true /* enforce */,
                                         GURL("https://example-report.test"));
  MockExpectCTReporter reporter;
  transport_security_state_->SetExpectCTReporter(&reporter);

  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_COMPLIES_VIA_SCTS));

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  // The histogram should have been recorded with the CT compliance status.
  histograms.ExpectUniqueSample(
      kHistogramName,
      static_cast<int>(ct::CTPolicyCompliance::CT_POLICY_COMPLIES_VIA_SCTS), 1);
}

// Test that when CT is not required and the connection is compliant, the
// histogram for CT-required connections is not recorded.
TEST_P(SSLClientSocketVersionTest, CTNotRequiredHistogram) {
  const char kHistogramName[] =
      "Net.CertificateTransparency.CTRequiredConnectionComplianceStatus2.SSL";
  base::HistogramTester histograms;

  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));
  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // Certificate is trusted and chains to a private root, so CT is not required.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = false;
  verify_result.verified_cert = server_cert;
  verify_result.public_key_hashes =
      MakeHashValueVector(kGoodHashValueVectorInput);
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_COMPLIES_VIA_SCTS));

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  // The histogram should not have been recorded because CT was not required for
  // the connection.
  histograms.ExpectTotalCount(kHistogramName, 0);
}

// Test that when CT is required (in this case, by an Expect-CT opt-in), the
// absence of CT information is recorded in the histogram for CT-required
// connections.
TEST_P(SSLClientSocketVersionTest, CTRequiredHistogramNonCompliant) {
  const char kHistogramName[] =
      "Net.CertificateTransparency.CTRequiredConnectionComplianceStatus2.SSL";
  base::HistogramTester histograms;

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);

  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));
  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // Certificate is trusted and chains to a public root.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = true;
  verify_result.verified_cert = server_cert;
  verify_result.public_key_hashes =
      MakeHashValueVector(kGoodHashValueVectorInput);
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  // Set up the Expect-CT opt-in.
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  transport_security_state_->AddExpectCT(host_port_pair().host(), expiry,
                                         true /* enforce */,
                                         GURL("https://example-report.test"));
  MockExpectCTReporter reporter;
  transport_security_state_->SetExpectCTReporter(&reporter);

  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_NOT_ENOUGH_SCTS));

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  EXPECT_THAT(rv, IsError(ERR_CERTIFICATE_TRANSPARENCY_REQUIRED));

  // The histogram should have been recorded with the CT compliance status.
  histograms.ExpectUniqueSample(
      kHistogramName,
      static_cast<int>(ct::CTPolicyCompliance::CT_POLICY_NOT_ENOUGH_SCTS), 1);
}

// Test that when CT is required (in this case, by an Expect-CT opt-in) but the
// connection is not compliant, the relevant flag is set on the SSLInfo.
TEST_P(SSLClientSocketVersionTest, CTRequirementsFlagNotMet) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));
  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // Certificate is trusted and chains to a public root.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = true;
  verify_result.verified_cert = server_cert;
  verify_result.public_key_hashes =
      MakeHashValueVector(kGoodHashValueVectorInput);
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  // Set up the Expect-CT opt-in.
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  transport_security_state_->AddExpectCT(host_port_pair().host(), expiry,
                                         true /* enforce */, GURL());

  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_NOT_ENOUGH_SCTS));

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_TRUE(ssl_info.ct_policy_compliance_required);
}

// Test that when CT is required (in this case, by an Expect-CT opt-in) and the
// connection is compliant, the relevant flag is set on the SSLInfo.
TEST_P(SSLClientSocketVersionTest, CTRequirementsFlagMet) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));
  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // Certificate is trusted and chains to a public root.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = true;
  verify_result.verified_cert = server_cert;
  verify_result.public_key_hashes =
      MakeHashValueVector(kGoodHashValueVectorInput);
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  // Set up the Expect-CT opt-in.
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  transport_security_state_->AddExpectCT(host_port_pair().host(), expiry,
                                         true /* enforce */, GURL());

  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_COMPLIES_VIA_SCTS));

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));
  EXPECT_TRUE(ssl_info.ct_policy_compliance_required);
}

// Test that when CT is required (in this case, by a CT delegate), the CT
// required histogram is not recorded for a locally installed root.
TEST_P(SSLClientSocketVersionTest, CTRequiredHistogramNonCompliantLocalRoot) {
  const char kHistogramName[] =
      "Net.CertificateTransparency.CTRequiredConnectionComplianceStatus2.SSL";
  base::HistogramTester histograms;

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);

  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));
  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = false;
  verify_result.verified_cert = server_cert;
  verify_result.public_key_hashes =
      MakeHashValueVector(kGoodHashValueVectorInput);
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  // Set up the CT requirement and failure to comply.
  base::ScopedClosureRunner cleanup(base::BindOnce(
      &TransportSecurityState::SetShouldRequireCTForTesting, nullptr));
  bool require_ct = true;
  TransportSecurityState::SetShouldRequireCTForTesting(&require_ct);
  MockRequireCTDelegate require_ct_delegate;
  transport_security_state_->SetRequireCTDelegate(&require_ct_delegate);
  EXPECT_CALL(require_ct_delegate, IsCTRequiredForHost(_, _, _))
      .WillRepeatedly(Return(TransportSecurityState::RequireCTDelegate::
                                 CTRequirementLevel::REQUIRED));
  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_NOT_ENOUGH_SCTS));

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  EXPECT_THAT(rv, IsError(ERR_CERTIFICATE_TRANSPARENCY_REQUIRED));

  histograms.ExpectTotalCount(kHistogramName, 0);
}

// Test that when CT is required (in this case, by an Expect-CT opt-in), the
// absence of CT information is a socket error.
TEST_P(SSLClientSocketVersionTest, CTIsRequiredByExpectCT) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);

  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));
  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // Certificate is trusted and chains to a public root.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = true;
  verify_result.verified_cert = server_cert;
  verify_result.public_key_hashes =
      MakeHashValueVector(kGoodHashValueVectorInput);
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  // Set up the Expect-CT opt-in.
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  transport_security_state_->AddExpectCT(host_port_pair().host(), expiry,
                                         true /* enforce */,
                                         GURL("https://example-report.test"));
  MockExpectCTReporter reporter;
  transport_security_state_->SetExpectCTReporter(&reporter);

  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_NOT_ENOUGH_SCTS));

  SSLConfig ssl_config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  EXPECT_THAT(rv, IsError(ERR_CERTIFICATE_TRANSPARENCY_REQUIRED));
  EXPECT_TRUE(ssl_info.cert_status &
              CERT_STATUS_CERTIFICATE_TRANSPARENCY_REQUIRED);
  EXPECT_FALSE(sock_->IsConnected());

  EXPECT_EQ(1u, reporter.num_failures());
  EXPECT_EQ(GURL("https://example-report.test"), reporter.report_uri());
  EXPECT_EQ(ssl_info.unverified_cert.get(),
            reporter.served_certificate_chain());
  EXPECT_EQ(ssl_info.cert.get(), reporter.validated_certificate_chain());
  EXPECT_EQ(0u, reporter.signed_certificate_timestamps().size());

  transport_security_state_->ClearReportCachesForTesting();
  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_NOT_DIVERSE_SCTS));
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  EXPECT_THAT(rv, IsError(ERR_CERTIFICATE_TRANSPARENCY_REQUIRED));
  EXPECT_TRUE(ssl_info.cert_status &
              CERT_STATUS_CERTIFICATE_TRANSPARENCY_REQUIRED);
  EXPECT_FALSE(sock_->IsConnected());

  EXPECT_EQ(2u, reporter.num_failures());
  EXPECT_EQ(GURL("https://example-report.test"), reporter.report_uri());
  EXPECT_EQ(ssl_info.unverified_cert.get(),
            reporter.served_certificate_chain());
  EXPECT_EQ(ssl_info.cert.get(), reporter.validated_certificate_chain());
  EXPECT_EQ(0u, reporter.signed_certificate_timestamps().size());

  // If the connection is CT compliant, then there should be no socket error nor
  // a report.
  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_COMPLIES_VIA_SCTS));
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  EXPECT_EQ(net::OK, rv);
  EXPECT_FALSE(ssl_info.cert_status &
               CERT_STATUS_CERTIFICATE_TRANSPARENCY_REQUIRED);
  EXPECT_TRUE(sock_->IsConnected());
  EXPECT_EQ(2u, reporter.num_failures());

  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_BUILD_NOT_TIMELY));
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(ssl_config, &rv));
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  EXPECT_EQ(net::OK, rv);
  EXPECT_FALSE(ssl_info.cert_status &
               CERT_STATUS_CERTIFICATE_TRANSPARENCY_REQUIRED);
  EXPECT_TRUE(sock_->IsConnected());
  EXPECT_EQ(2u, reporter.num_failures());
}

// When both PKP and CT are required for a host, and both fail, the more
// serious error is that the pin validation failed.
TEST_P(SSLClientSocketVersionTest, PKPMoreImportantThanCT) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));
  scoped_refptr<X509Certificate> server_cert =
      embedded_test_server()->GetCertificate();

  // Certificate is trusted, but chains to a public root that doesn't match the
  // pin hashes.
  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = true;
  verify_result.verified_cert = server_cert;
  verify_result.public_key_hashes =
      MakeHashValueVector(kBadHashValueVectorInput);
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  transport_security_state_->EnableStaticPinsForTesting();
  ScopedTransportSecurityStateSource scoped_security_state_source;

  const char kCTHost[] = "pkp-expect-ct.preloaded.test";

  // Set up CT.
  MockRequireCTDelegate require_ct_delegate;
  transport_security_state_->SetRequireCTDelegate(&require_ct_delegate);
  EXPECT_CALL(require_ct_delegate, IsCTRequiredForHost(_, _, _))
      .WillRepeatedly(Return(TransportSecurityState::RequireCTDelegate::
                                 CTRequirementLevel::NOT_REQUIRED));
  EXPECT_CALL(require_ct_delegate, IsCTRequiredForHost(kCTHost, _, _))
      .WillRepeatedly(Return(TransportSecurityState::RequireCTDelegate::
                                 CTRequirementLevel::REQUIRED));
  EXPECT_CALL(*ct_policy_enforcer_, CheckCompliance(server_cert.get(), _, _))
      .WillRepeatedly(
          Return(ct::CTPolicyCompliance::CT_POLICY_NOT_ENOUGH_SCTS));

  SSLConfig ssl_config;
  int rv;
  HostPortPair ct_host_port_pair(kCTHost, host_port_pair().port());
  ASSERT_TRUE(CreateAndConnectSSLClientSocketWithHost(ssl_config,
                                                      ct_host_port_pair, &rv));
  SSLInfo ssl_info;
  ASSERT_TRUE(sock_->GetSSLInfo(&ssl_info));

  EXPECT_THAT(rv, IsError(ERR_SSL_PINNED_KEY_NOT_IN_CERT_CHAIN));
  EXPECT_TRUE(ssl_info.cert_status & CERT_STATUS_PINNED_KEY_MISSING);
  EXPECT_TRUE(ssl_info.cert_status &
              CERT_STATUS_CERTIFICATE_TRANSPARENCY_REQUIRED);
  EXPECT_FALSE(sock_->IsConnected());
}

// Test that handshake_failure alerts at the ServerHello are mapped to
// ERR_SSL_VERSION_OR_CIPHER_MISMATCH.
TEST_F(SSLClientSocketTest, HandshakeFailureServerHello) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, SSLServerConfig()));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  std::unique_ptr<FakeBlockingStreamSocket> transport(
      new FakeBlockingStreamSocket(std::move(real_transport)));
  FakeBlockingStreamSocket* raw_transport = transport.get();
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  ASSERT_THAT(rv, IsOk());

  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));

  // Connect. Stop before the client processes ServerHello.
  raw_transport->BlockReadResult();
  rv = sock->Connect(callback.callback());
  ASSERT_THAT(rv, IsError(ERR_IO_PENDING));
  raw_transport->WaitForReadResult();

  // Replace it with an alert.
  raw_transport->ReplaceReadResult(
      FormatTLS12Alert(40 /* AlertDescription.handshake_failure */));
  raw_transport->UnblockReadResult();

  rv = callback.GetResult(rv);
  EXPECT_THAT(rv, IsError(ERR_SSL_VERSION_OR_CIPHER_MISMATCH));
}

// Test that handshake_failure alerts after the ServerHello but without a
// CertificateRequest are mapped to ERR_SSL_PROTOCOL_ERROR.
TEST_F(SSLClientSocketTest, HandshakeFailureNoClientCerts) {
  SSLServerConfig server_config;
  server_config.version_max = SSL_PROTOCOL_VERSION_TLS1_2;
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, server_config));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  std::unique_ptr<FakeBlockingStreamSocket> transport(
      new FakeBlockingStreamSocket(std::move(real_transport)));
  FakeBlockingStreamSocket* raw_transport = transport.get();
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  ASSERT_THAT(rv, IsOk());

  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));

  // Connect. Stop before the client processes ServerHello.
  raw_transport->BlockReadResult();
  rv = sock->Connect(callback.callback());
  ASSERT_THAT(rv, IsError(ERR_IO_PENDING));
  raw_transport->WaitForReadResult();

  // Release the ServerHello and wait for the client to write its second flight.
  raw_transport->BlockWrite();
  raw_transport->UnblockReadResult();
  raw_transport->WaitForWrite();

  // Wait for the server's final flight.
  raw_transport->BlockReadResult();
  raw_transport->UnblockWrite();
  raw_transport->WaitForReadResult();

  // Replace it with an alert.
  raw_transport->ReplaceReadResult(
      FormatTLS12Alert(40 /* AlertDescription.handshake_failure */));
  raw_transport->UnblockReadResult();

  rv = callback.GetResult(rv);
  EXPECT_THAT(rv, IsError(ERR_SSL_PROTOCOL_ERROR));
}

// Test that handshake_failure alerts after the ServerHello map to
// ERR_BAD_SSL_CLIENT_AUTH_CERT if a client certificate was requested but not
// supplied. TLS does not have an alert for this case, so handshake_failure is
// common. See https://crbug.com/646567.
TEST_F(SSLClientSocketTest, LateHandshakeFailureMissingClientCerts) {
  // Request a client certificate.
  SSLServerConfig server_config;
  server_config.version_max = SSL_PROTOCOL_VERSION_TLS1_2;
  server_config.client_cert_type = SSLServerConfig::OPTIONAL_CLIENT_CERT;
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, server_config));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  std::unique_ptr<FakeBlockingStreamSocket> transport(
      new FakeBlockingStreamSocket(std::move(real_transport)));
  FakeBlockingStreamSocket* raw_transport = transport.get();
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  ASSERT_THAT(rv, IsOk());

  // Send no client certificate.
  context_->SetClientCertificate(host_port_pair(), nullptr, nullptr);
  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));

  // Connect. Stop before the client processes ServerHello.
  raw_transport->BlockReadResult();
  rv = sock->Connect(callback.callback());
  ASSERT_THAT(rv, IsError(ERR_IO_PENDING));
  raw_transport->WaitForReadResult();

  // Release the ServerHello and wait for the client to write its second flight.
  raw_transport->BlockWrite();
  raw_transport->UnblockReadResult();
  raw_transport->WaitForWrite();

  // Wait for the server's final flight.
  raw_transport->BlockReadResult();
  raw_transport->UnblockWrite();
  raw_transport->WaitForReadResult();

  // Replace it with an alert.
  raw_transport->ReplaceReadResult(
      FormatTLS12Alert(40 /* AlertDescription.handshake_failure */));
  raw_transport->UnblockReadResult();

  rv = callback.GetResult(rv);
  EXPECT_THAT(rv, IsError(ERR_BAD_SSL_CLIENT_AUTH_CERT));
}

// Test that handshake_failure alerts after the ServerHello map to
// ERR_SSL_PROTOCOL_ERROR if received after sending a client certificate. It is
// assumed servers will send a more appropriate alert in this case.
TEST_F(SSLClientSocketTest, LateHandshakeFailureSendClientCerts) {
  // Request a client certificate.
  SSLServerConfig server_config;
  server_config.version_max = SSL_PROTOCOL_VERSION_TLS1_2;
  server_config.client_cert_type = SSLServerConfig::OPTIONAL_CLIENT_CERT;
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, server_config));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  std::unique_ptr<FakeBlockingStreamSocket> transport(
      new FakeBlockingStreamSocket(std::move(real_transport)));
  FakeBlockingStreamSocket* raw_transport = transport.get();
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  ASSERT_THAT(rv, IsOk());

  // Send a client certificate.
  base::FilePath certs_dir = GetTestCertsDirectory();
  context_->SetClientCertificate(
      host_port_pair(), ImportCertFromFile(certs_dir, "client_1.pem"),
      key_util::LoadPrivateKeyOpenSSL(certs_dir.AppendASCII("client_1.key")));
  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));

  // Connect. Stop before the client processes ServerHello.
  raw_transport->BlockReadResult();
  rv = sock->Connect(callback.callback());
  ASSERT_THAT(rv, IsError(ERR_IO_PENDING));
  raw_transport->WaitForReadResult();

  // Release the ServerHello and wait for the client to write its second flight.
  raw_transport->BlockWrite();
  raw_transport->UnblockReadResult();
  raw_transport->WaitForWrite();

  // Wait for the server's final flight.
  raw_transport->BlockReadResult();
  raw_transport->UnblockWrite();
  raw_transport->WaitForReadResult();

  // Replace it with an alert.
  raw_transport->ReplaceReadResult(
      FormatTLS12Alert(40 /* AlertDescription.handshake_failure */));
  raw_transport->UnblockReadResult();

  rv = callback.GetResult(rv);
  EXPECT_THAT(rv, IsError(ERR_SSL_PROTOCOL_ERROR));
}

// Test that access_denied alerts are mapped to ERR_SSL_PROTOCOL_ERROR if
// received on a connection not requesting client certificates. This is an
// incorrect use of the alert but is common. See https://crbug.com/630883.
TEST_F(SSLClientSocketTest, AccessDeniedNoClientCerts) {
  // Request a client certificate.
  SSLServerConfig server_config;
  server_config.version_max = SSL_PROTOCOL_VERSION_TLS1_2;
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, server_config));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  std::unique_ptr<FakeBlockingStreamSocket> transport(
      new FakeBlockingStreamSocket(std::move(real_transport)));
  FakeBlockingStreamSocket* raw_transport = transport.get();
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  ASSERT_THAT(rv, IsOk());

  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));

  // Connect. Stop before the client processes ServerHello.
  raw_transport->BlockReadResult();
  rv = sock->Connect(callback.callback());
  ASSERT_THAT(rv, IsError(ERR_IO_PENDING));
  raw_transport->WaitForReadResult();

  // Release the ServerHello and wait for the client to write its second flight.
  raw_transport->BlockWrite();
  raw_transport->UnblockReadResult();
  raw_transport->WaitForWrite();

  // Wait for the server's final flight.
  raw_transport->BlockReadResult();
  raw_transport->UnblockWrite();
  raw_transport->WaitForReadResult();

  // Replace it with an alert.
  raw_transport->ReplaceReadResult(
      FormatTLS12Alert(49 /* AlertDescription.access_denied */));
  raw_transport->UnblockReadResult();

  rv = callback.GetResult(rv);
  EXPECT_THAT(rv, IsError(ERR_SSL_PROTOCOL_ERROR));
}

// Test that access_denied alerts are mapped to ERR_BAD_SSL_CLIENT_AUTH_CERT if
// received on a connection requesting client certificates.
TEST_F(SSLClientSocketTest, AccessDeniedClientCerts) {
  // Request a client certificate.
  SSLServerConfig server_config;
  server_config.version_max = SSL_PROTOCOL_VERSION_TLS1_2;
  server_config.client_cert_type = SSLServerConfig::OPTIONAL_CLIENT_CERT;
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, server_config));

  TestCompletionCallback callback;
  std::unique_ptr<StreamSocket> real_transport(
      new TCPClientSocket(addr(), nullptr, nullptr, NetLogSource()));
  std::unique_ptr<FakeBlockingStreamSocket> transport(
      new FakeBlockingStreamSocket(std::move(real_transport)));
  FakeBlockingStreamSocket* raw_transport = transport.get();
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  ASSERT_THAT(rv, IsOk());

  // Send a client certificate.
  base::FilePath certs_dir = GetTestCertsDirectory();
  context_->SetClientCertificate(
      host_port_pair(), ImportCertFromFile(certs_dir, "client_1.pem"),
      key_util::LoadPrivateKeyOpenSSL(certs_dir.AppendASCII("client_1.key")));
  std::unique_ptr<SSLClientSocket> sock(CreateSSLClientSocket(
      std::move(transport), host_port_pair(), SSLConfig()));

  // Connect. Stop before the client processes ServerHello.
  raw_transport->BlockReadResult();
  rv = sock->Connect(callback.callback());
  ASSERT_THAT(rv, IsError(ERR_IO_PENDING));
  raw_transport->WaitForReadResult();

  // Release the ServerHello and wait for the client to write its second flight.
  raw_transport->BlockWrite();
  raw_transport->UnblockReadResult();
  raw_transport->WaitForWrite();

  // Wait for the server's final flight.
  raw_transport->BlockReadResult();
  raw_transport->UnblockWrite();
  raw_transport->WaitForReadResult();

  // Replace it with an alert.
  raw_transport->ReplaceReadResult(
      FormatTLS12Alert(49 /* AlertDescription.access_denied */));
  raw_transport->UnblockReadResult();

  rv = callback.GetResult(rv);
  EXPECT_THAT(rv, IsError(ERR_BAD_SSL_CLIENT_AUTH_CERT));
}

// Test the client can send application data before the ServerHello comes in.
TEST_F(SSLClientSocketZeroRTTTest, ZeroRTTEarlyDataBeforeServerHello) {
  ASSERT_TRUE(StartServer());
  ASSERT_TRUE(RunInitialConnection());

  // Make a 0-RTT Connection. Connect() and Write() complete even though the
  // ServerHello is blocked.
  FakeBlockingStreamSocket* socket = MakeClient(true);
  socket->BlockReadResult();
  ASSERT_THAT(Connect(), IsOk());
  constexpr base::StringPiece kRequest = "GET /zerortt HTTP/1.0\r\n\r\n";
  EXPECT_EQ(static_cast<int>(kRequest.size()), WriteAndWait(kRequest));

  // Release the ServerHello. Now reads complete.
  socket->UnblockReadResult();
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  int size = ReadAndWait(buf.get(), 4096);
  EXPECT_GT(size, 0);
  EXPECT_EQ('1', buf->data()[size - 1]);

  SSLInfo ssl_info;
  ASSERT_TRUE(GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
}

// Test that the client sends 1-RTT data if the ServerHello happens to come in
// before Write() is called. See https://crbug.com/950706.
TEST_F(SSLClientSocketZeroRTTTest, ZeroRTTEarlyDataAfterServerHello) {
  ASSERT_TRUE(StartServer());
  ASSERT_TRUE(RunInitialConnection());

  // Make a 0-RTT Connection. Connect() completes even though the ServerHello is
  // blocked.
  FakeBlockingStreamSocket* socket = MakeClient(true);
  socket->BlockReadResult();
  ASSERT_THAT(Connect(), IsOk());

  // Wait for the ServerHello to come in and for SSLClientSocket to process it.
  socket->WaitForReadResult();
  socket->UnblockReadResult();
  base::RunLoop().RunUntilIdle();

  // Now write to the socket.
  constexpr base::StringPiece kRequest = "GET /zerortt HTTP/1.0\r\n\r\n";
  EXPECT_EQ(static_cast<int>(kRequest.size()), WriteAndWait(kRequest));

  // Although the socket was created in early data state and the client never
  // explicitly called ReaD() or ConfirmHandshake(), SSLClientSocketImpl
  // internally consumed the ServerHello and switch keys. The server then
  // responds with '0'.
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  int size = ReadAndWait(buf.get(), 4096);
  EXPECT_GT(size, 0);
  EXPECT_EQ('0', buf->data()[size - 1]);

  SSLInfo ssl_info;
  ASSERT_TRUE(GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
}

// Check that 0RTT is confirmed after a Write and Read.
TEST_F(SSLClientSocketZeroRTTTest, ZeroRTTConfirmedAfterRead) {
  ASSERT_TRUE(StartServer());
  ASSERT_TRUE(RunInitialConnection());

  // Make a 0-RTT Connection. Connect() and Write() complete even though the
  // ServerHello is blocked.
  FakeBlockingStreamSocket* socket = MakeClient(true);
  socket->BlockReadResult();
  ASSERT_THAT(Connect(), IsOk());
  constexpr base::StringPiece kRequest = "GET /zerortt HTTP/1.0\r\n\r\n";
  EXPECT_EQ(static_cast<int>(kRequest.size()), WriteAndWait(kRequest));

  socket->UnblockReadResult();
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  int size = ReadAndWait(buf.get(), 4096);
  EXPECT_GT(size, 0);
  EXPECT_EQ('1', buf->data()[size - 1]);

  // After the handshake is confirmed, ConfirmHandshake should return
  // synchronously.
  TestCompletionCallback callback;
  ASSERT_THAT(ssl_socket()->ConfirmHandshake(callback.callback()), IsOk());

  SSLInfo ssl_info;
  ASSERT_TRUE(GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
}

// Test that writes wait for the ServerHello once it has reached the early data
// limit.
TEST_F(SSLClientSocketZeroRTTTest, ZeroRTTEarlyDataLimit) {
  ASSERT_TRUE(StartServer());
  ASSERT_TRUE(RunInitialConnection());

  // Make a 0-RTT Connection. Connect() completes even though the ServerHello is
  // blocked.
  FakeBlockingStreamSocket* socket = MakeClient(true);
  socket->BlockReadResult();
  ASSERT_THAT(Connect(), IsOk());

  // EmbeddedTestServer uses BoringSSL's hard-coded early data limit, which is
  // below 16k.
  constexpr size_t kRequestSize = 16 * 1024;
  std::string request = "GET /zerortt HTTP/1.0\r\n";
  while (request.size() < kRequestSize) {
    request += "The-Answer-To-Life-The-Universe-And-Everything: 42\r\n";
  }
  request += "\r\n";

  // Writing the large input should not succeed. It is blocked on the
  // ServerHello.
  TestCompletionCallback write_callback;
  auto write_buf = base::MakeRefCounted<StringIOBuffer>(request);
  int write_rv = ssl_socket()->Write(write_buf.get(), request.size(),
                                     write_callback.callback(),
                                     TRAFFIC_ANNOTATION_FOR_TESTS);
  ASSERT_THAT(write_rv, IsError(ERR_IO_PENDING));

  // The Write should have issued a read for the ServerHello, so
  // WaitForReadResult has something to wait for.
  socket->WaitForReadResult();
  EXPECT_TRUE(socket->pending_read_result());

  // Queue a read. It should be blocked on the ServerHello.
  TestCompletionCallback read_callback;
  auto read_buf = base::MakeRefCounted<IOBuffer>(4096);
  int read_rv =
      ssl_socket()->Read(read_buf.get(), 4096, read_callback.callback());
  ASSERT_THAT(read_rv, IsError(ERR_IO_PENDING));

  // Also queue a ConfirmHandshake. It should also be blocked on ServerHello.
  TestCompletionCallback confirm_callback;
  int confirm_rv = ssl_socket()->ConfirmHandshake(confirm_callback.callback());
  ASSERT_THAT(confirm_rv, IsError(ERR_IO_PENDING));

  // Double-check the write was not accidentally blocked on the network.
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(write_callback.have_result());

  // At this point, the maximum possible number of events are all blocked on the
  // same thing. Release the ServerHello. All three should complete.
  socket->UnblockReadResult();
  EXPECT_EQ(static_cast<int>(request.size()),
            write_callback.GetResult(write_rv));
  EXPECT_THAT(confirm_callback.GetResult(confirm_rv), IsOk());
  int size = read_callback.GetResult(read_rv);
  ASSERT_GT(size, 0);
  EXPECT_EQ('1', read_buf->data()[size - 1]);

  SSLInfo ssl_info;
  ASSERT_TRUE(GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
}

TEST_F(SSLClientSocketZeroRTTTest, ZeroRTTNoZeroRTTOnResume) {
  ASSERT_TRUE(StartServer());
  ASSERT_TRUE(RunInitialConnection());

  SSLServerConfig server_config;
  server_config.early_data_enabled = false;
  server_config.version_max = SSL_PROTOCOL_VERSION_TLS1_3;

  SetServerConfig(server_config);

  // 0-RTT Connection
  FakeBlockingStreamSocket* socket = MakeClient(true);
  socket->BlockReadResult();
  ASSERT_THAT(Connect(), IsOk());
  constexpr base::StringPiece kRequest = "GET /zerortt HTTP/1.0\r\n\r\n";
  EXPECT_EQ(static_cast<int>(kRequest.size()), WriteAndWait(kRequest));
  socket->UnblockReadResult();

  // Expect early data to be rejected.
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  int rv = ReadAndWait(buf.get(), 4096);
  EXPECT_EQ(ERR_EARLY_DATA_REJECTED, rv);
  rv = WriteAndWait(kRequest);
  EXPECT_EQ(ERR_EARLY_DATA_REJECTED, rv);
}

// Test that the ConfirmHandshake successfully completes the handshake and that
// it blocks until the server's leg has been received.
TEST_F(SSLClientSocketZeroRTTTest, ZeroRTTConfirmHandshake) {
  ASSERT_TRUE(StartServer());
  ASSERT_TRUE(RunInitialConnection());

  // 0-RTT Connection
  FakeBlockingStreamSocket* socket = MakeClient(true);
  socket->BlockReadResult();
  ASSERT_THAT(Connect(), IsOk());

  // The ServerHello is blocked, so ConfirmHandshake should not complete.
  TestCompletionCallback callback;
  ASSERT_EQ(ERR_IO_PENDING,
            ssl_socket()->ConfirmHandshake(callback.callback()));
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(callback.have_result());

  // Release the ServerHello. ConfirmHandshake now completes.
  socket->UnblockReadResult();
  ASSERT_THAT(callback.GetResult(ERR_IO_PENDING), IsOk());

  constexpr base::StringPiece kRequest = "GET /zerortt HTTP/1.0\r\n\r\n";
  EXPECT_EQ(static_cast<int>(kRequest.size()), WriteAndWait(kRequest));

  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  int size = ReadAndWait(buf.get(), 4096);
  EXPECT_GT(size, 0);
  EXPECT_EQ('0', buf->data()[size - 1]);

  SSLInfo ssl_info;
  ASSERT_TRUE(GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
}

// Test that an early read does not break during zero RTT.
TEST_F(SSLClientSocketZeroRTTTest, ZeroRTTReadBeforeWrite) {
  ASSERT_TRUE(StartServer());
  ASSERT_TRUE(RunInitialConnection());

  // Make a 0-RTT Connection. Connect() completes even though the ServerHello is
  // blocked.
  FakeBlockingStreamSocket* socket = MakeClient(true);
  socket->BlockReadResult();
  ASSERT_THAT(Connect(), IsOk());

  // Read() does not make progress.
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  TestCompletionCallback read_callback;
  ASSERT_EQ(ERR_IO_PENDING,
            ssl_socket()->Read(buf.get(), 4096, read_callback.callback()));

  // Write() completes, even though reads are blocked.
  constexpr base::StringPiece kRequest = "GET /zerortt HTTP/1.0\r\n\r\n";
  EXPECT_EQ(static_cast<int>(kRequest.size()), WriteAndWait(kRequest));

  // Release the ServerHello, etc. The Read() now completes.
  socket->UnblockReadResult();
  int size = read_callback.GetResult(ERR_IO_PENDING);
  EXPECT_GT(size, 0);
  EXPECT_EQ('1', buf->data()[size - 1]);

  SSLInfo ssl_info;
  ASSERT_TRUE(GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
}

TEST_F(SSLClientSocketZeroRTTTest, ZeroRTTDoubleConfirmHandshake) {
  ASSERT_TRUE(StartServer());
  ASSERT_TRUE(RunInitialConnection());

  // 0-RTT Connection
  MakeClient(true);
  ASSERT_THAT(Connect(), IsOk());
  TestCompletionCallback callback;
  ASSERT_THAT(
      callback.GetResult(ssl_socket()->ConfirmHandshake(callback.callback())),
      IsOk());
  // After the handshake is confirmed, ConfirmHandshake should return
  // synchronously.
  ASSERT_THAT(ssl_socket()->ConfirmHandshake(callback.callback()), IsOk());
  constexpr base::StringPiece kRequest = "GET /zerortt HTTP/1.0\r\n\r\n";
  EXPECT_EQ(static_cast<int>(kRequest.size()), WriteAndWait(kRequest));

  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  int size = ReadAndWait(buf.get(), 4096);
  EXPECT_GT(size, 0);
  EXPECT_EQ('0', buf->data()[size - 1]);

  SSLInfo ssl_info;
  ASSERT_TRUE(GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
}

TEST_F(SSLClientSocketZeroRTTTest, ZeroRTTParallelReadConfirm) {
  ASSERT_TRUE(StartServer());
  ASSERT_TRUE(RunInitialConnection());

  // 0-RTT Connection
  FakeBlockingStreamSocket* socket = MakeClient(true);
  socket->BlockReadResult();
  ASSERT_THAT(Connect(), IsOk());

  constexpr base::StringPiece kRequest = "GET /zerortt HTTP/1.0\r\n\r\n";
  EXPECT_EQ(static_cast<int>(kRequest.size()), WriteAndWait(kRequest));

  // The ServerHello is blocked, so ConfirmHandshake should not complete.
  TestCompletionCallback callback;
  ASSERT_EQ(ERR_IO_PENDING,
            ssl_socket()->ConfirmHandshake(callback.callback()));
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(callback.have_result());

  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  TestCompletionCallback read_callback;
  ASSERT_EQ(ERR_IO_PENDING,
            ssl_socket()->Read(buf.get(), 4096, read_callback.callback()));
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(read_callback.have_result());

  // Release the ServerHello. ConfirmHandshake now completes.
  socket->UnblockReadResult();
  ASSERT_THAT(callback.WaitForResult(), IsOk());

  int result = read_callback.WaitForResult();
  EXPECT_GT(result, 0);
  EXPECT_EQ('1', buf->data()[result - 1]);

  SSLInfo ssl_info;
  ASSERT_TRUE(GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);
}

// Basic test for dumping memory stats.
TEST_P(SSLClientSocketReadTest, DumpMemoryStats) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, GetServerConfig()));

  // This test compares the memory usage when there is and isn't a pending read
  // on the socket, so disable the post-handshake peek.
  SSLConfig config;
  config.disable_post_handshake_peek_for_testing = true;

  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(config, &rv));
  EXPECT_THAT(rv, IsOk());
  StreamSocket::SocketMemoryStats stats;
  sock_->DumpMemoryStats(&stats);
  EXPECT_EQ(0u, stats.buffer_size);
  EXPECT_EQ(1u, stats.cert_count);
  EXPECT_LT(0u, stats.cert_size);
  EXPECT_EQ(stats.cert_size, stats.total_size);

  // Read the response without writing a request, so the read will be pending.
  TestCompletionCallback read_callback;
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  rv = Read(sock_.get(), buf.get(), 4096, read_callback.callback());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  // Dump memory again and check that |buffer_size| contain the read buffer.
  StreamSocket::SocketMemoryStats stats2;
  sock_->DumpMemoryStats(&stats2);

  if (read_if_ready_supported()) {
    EXPECT_EQ(0u, stats2.buffer_size);
    EXPECT_EQ(stats.cert_size, stats2.total_size);
  } else {
    EXPECT_EQ(17 * 1024u, stats2.buffer_size);
    EXPECT_LT(17 * 1024u, stats2.total_size);
  }
  EXPECT_EQ(1u, stats2.cert_count);
  EXPECT_LT(0u, stats2.cert_size);

  // Drop the socket. It has a pending read with a reference to |read_callback|,
  // so the socket must be dropped before the test returns.
  sock_ = nullptr;
}

TEST_P(SSLClientSocketReadTest, IdleAfterRead) {
  // Set up a TCP server.
  TCPServerSocket server_listener(nullptr, NetLogSource());
  ASSERT_THAT(
      server_listener.Listen(IPEndPoint(IPAddress::IPv4Localhost(), 0), 1),
      IsOk());
  IPEndPoint server_address;
  ASSERT_THAT(server_listener.GetLocalAddress(&server_address), IsOk());

  // Connect a TCP client and server socket.
  TestCompletionCallback server_callback;
  std::unique_ptr<StreamSocket> server_transport;
  int server_rv =
      server_listener.Accept(&server_transport, server_callback.callback());

  TestCompletionCallback client_callback;
  std::unique_ptr<TCPClientSocket> client_transport(new TCPClientSocket(
      AddressList(server_address), nullptr, nullptr, NetLogSource()));
  int client_rv = client_transport->Connect(client_callback.callback());

  EXPECT_THAT(server_callback.GetResult(server_rv), IsOk());
  EXPECT_THAT(client_callback.GetResult(client_rv), IsOk());

  // Set up an SSL server.
  base::FilePath certs_dir = GetTestCertsDirectory();
  scoped_refptr<net::X509Certificate> cert =
      ImportCertFromFile(certs_dir, "ok_cert.pem");
  ASSERT_TRUE(cert);
  bssl::UniquePtr<EVP_PKEY> pkey =
      key_util::LoadEVP_PKEYFromPEM(certs_dir.AppendASCII("ok_cert.pem"));
  ASSERT_TRUE(pkey);
  std::unique_ptr<crypto::RSAPrivateKey> key =
      crypto::RSAPrivateKey::CreateFromKey(pkey.get());
  ASSERT_TRUE(key);
  std::unique_ptr<SSLServerContext> server_context =
      CreateSSLServerContext(cert.get(), *key.get(), GetServerConfig());

  // Complete the SSL handshake on both sides.
  std::unique_ptr<SSLClientSocket> client(CreateSSLClientSocket(
      std::move(client_transport), HostPortPair::FromIPEndPoint(server_address),
      SSLConfig()));
  std::unique_ptr<SSLServerSocket> server(
      server_context->CreateSSLServerSocket(std::move(server_transport)));

  server_rv = server->Handshake(server_callback.callback());
  client_rv = client->Connect(client_callback.callback());

  EXPECT_THAT(server_callback.GetResult(server_rv), IsOk());
  EXPECT_THAT(client_callback.GetResult(client_rv), IsOk());

  // Write a single record on the server.
  scoped_refptr<IOBuffer> write_buf = base::MakeRefCounted<StringIOBuffer>("a");
  server_rv = server->Write(write_buf.get(), 1, server_callback.callback(),
                            TRAFFIC_ANNOTATION_FOR_TESTS);

  // Read that record on the server, but with a much larger buffer than
  // necessary.
  scoped_refptr<IOBuffer> read_buf = base::MakeRefCounted<IOBuffer>(1024);
  client_rv =
      Read(client.get(), read_buf.get(), 1024, client_callback.callback());

  EXPECT_EQ(1, server_callback.GetResult(server_rv));
  EXPECT_EQ(1, WaitForReadCompletion(client.get(), read_buf.get(), 1024,
                                     &client_callback, client_rv));

  // At this point the client socket should be idle.
  EXPECT_TRUE(client->IsConnectedAndIdle());

  // The read buffer should be released.
  StreamSocket::SocketMemoryStats stats;
  client->DumpMemoryStats(&stats);
  EXPECT_EQ(0u, stats.buffer_size);
  EXPECT_EQ(1u, stats.cert_count);
  EXPECT_LT(0u, stats.cert_size);
  EXPECT_EQ(stats.cert_size, stats.total_size);
}

// Test that certificate errors are properly reported when the underlying
// transport is itself a TLS connection, such as when tunneling over an HTTPS
// proxy. See https://crbug.com/959305.
TEST_F(SSLClientSocketTest, SSLOverSSLBadCertificate) {
  // Load a pair of certificates.
  base::FilePath certs_dir = GetTestCertsDirectory();
  scoped_refptr<net::X509Certificate> ok_cert =
      ImportCertFromFile(certs_dir, "ok_cert.pem");
  ASSERT_TRUE(ok_cert);
  bssl::UniquePtr<EVP_PKEY> ok_pkey =
      key_util::LoadEVP_PKEYFromPEM(certs_dir.AppendASCII("ok_cert.pem"));
  ASSERT_TRUE(ok_pkey);

  scoped_refptr<net::X509Certificate> expired_cert =
      ImportCertFromFile(certs_dir, "expired_cert.pem");
  ASSERT_TRUE(expired_cert);
  bssl::UniquePtr<EVP_PKEY> expired_pkey =
      key_util::LoadEVP_PKEYFromPEM(certs_dir.AppendASCII("expired_cert.pem"));
  ASSERT_TRUE(expired_pkey);

  CertVerifyResult expired_result;
  expired_result.verified_cert = expired_cert;
  expired_result.cert_status = CERT_STATUS_DATE_INVALID;
  cert_verifier_->AddResultForCert(expired_cert, expired_result,
                                   ERR_CERT_DATE_INVALID);

  // Set up a TCP server.
  TCPServerSocket server_listener(nullptr, NetLogSource());
  ASSERT_THAT(
      server_listener.Listen(IPEndPoint(IPAddress::IPv4Localhost(), 0), 1),
      IsOk());
  IPEndPoint server_address;
  ASSERT_THAT(server_listener.GetLocalAddress(&server_address), IsOk());

  // Connect a TCP client and server socket.
  TestCompletionCallback server_callback;
  std::unique_ptr<StreamSocket> server_transport;
  int server_rv =
      server_listener.Accept(&server_transport, server_callback.callback());

  TestCompletionCallback client_callback;
  auto client_transport = std::make_unique<TCPClientSocket>(
      AddressList(server_address), nullptr, nullptr, NetLogSource());
  int client_rv = client_transport->Connect(client_callback.callback());

  ASSERT_THAT(server_callback.GetResult(server_rv), IsOk());
  ASSERT_THAT(client_callback.GetResult(client_rv), IsOk());

  // Set up a pair of SSL servers.
  std::unique_ptr<crypto::RSAPrivateKey> ok_key =
      crypto::RSAPrivateKey::CreateFromKey(ok_pkey.get());
  ASSERT_TRUE(ok_key);
  std::unique_ptr<SSLServerContext> ok_server_context =
      CreateSSLServerContext(ok_cert.get(), *ok_key.get(), SSLServerConfig());

  std::unique_ptr<crypto::RSAPrivateKey> expired_key =
      crypto::RSAPrivateKey::CreateFromKey(expired_pkey.get());
  ASSERT_TRUE(expired_key);
  std::unique_ptr<SSLServerContext> expired_server_context =
      CreateSSLServerContext(expired_cert.get(), *expired_key.get(),
                             SSLServerConfig());

  // Complete the proxy SSL handshake with ok_cert.pem. This should succeed.
  std::unique_ptr<SSLClientSocket> client =
      CreateSSLClientSocket(std::move(client_transport),
                            HostPortPair("proxy.test", 443), SSLConfig());
  std::unique_ptr<SSLServerSocket> server =
      ok_server_context->CreateSSLServerSocket(std::move(server_transport));

  client_rv = client->Connect(client_callback.callback());
  server_rv = server->Handshake(server_callback.callback());
  ASSERT_THAT(client_callback.GetResult(client_rv), IsOk());
  ASSERT_THAT(server_callback.GetResult(server_rv), IsOk());

  // Run the tunneled SSL handshake on with expired_cert.pem. This should fail.
  client = CreateSSLClientSocket(std::move(client),
                                 HostPortPair("server.test", 443), SSLConfig());
  server = expired_server_context->CreateSSLServerSocket(std::move(server));

  client_rv = client->Connect(client_callback.callback());
  server_rv = server->Handshake(server_callback.callback());

  // The client should observe the bad certificate error.
  EXPECT_THAT(client_callback.GetResult(client_rv),
              IsError(ERR_CERT_DATE_INVALID));
  SSLInfo ssl_info;
  ASSERT_TRUE(client->GetSSLInfo(&ssl_info));
  EXPECT_EQ(ssl_info.cert_status, expired_result.cert_status);

  // TODO(https://crbug.com/912383): The server sees
  // ERR_BAD_SSL_CLIENT_AUTH_CERT because its peer (the client) alerts it with
  // bad_certificate. The alert-mapping code assumes it is running on a client,
  // so it translates bad_certificate to ERR_BAD_SSL_CLIENT_AUTH_CERT, which
  // shouldn't be the error for a bad server certificate.
  EXPECT_THAT(server_callback.GetResult(server_rv),
              IsError(ERR_BAD_SSL_CLIENT_AUTH_CERT));
}

TEST_F(SSLClientSocketTest, Tag) {
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, SSLServerConfig()));

  RecordingTestNetLog log;
  std::unique_ptr<StreamSocket> transport(
      new TCPClientSocket(addr(), nullptr, &log, NetLogSource()));

  MockTaggingStreamSocket* tagging_sock =
      new MockTaggingStreamSocket(std::move(transport));

  // |sock| takes ownership of |tagging_sock|, but keep a
  // non-owning pointer to it.
  std::unique_ptr<SSLClientSocket> sock(
      CreateSSLClientSocket(std::unique_ptr<StreamSocket>(tagging_sock),
                            host_port_pair(), SSLConfig()));

  EXPECT_EQ(tagging_sock->tag(), SocketTag());
#if defined(OS_ANDROID)
  SocketTag tag(0x12345678, 0x87654321);
  sock->ApplySocketTag(tag);
  EXPECT_EQ(tagging_sock->tag(), tag);
#endif  // OS_ANDROID
}

class TLS13DowngradeTest
    : public SSLClientSocketTest,
      public ::testing::WithParamInterface<
          std::tuple<SpawnedTestServer::SSLOptions::TLSMaxVersion,
                     /* simulate_tls13_downgrade */ bool,
                     /* enable_for_local_anchors */ bool,
                     /* known_root */ bool>> {
 public:
  TLS13DowngradeTest() {}
  ~TLS13DowngradeTest() {}

  SpawnedTestServer::SSLOptions::TLSMaxVersion tls_max_version() const {
    return std::get<0>(GetParam());
  }

  bool simulate_tls13_downgrade() const { return std::get<1>(GetParam()); }
  bool enable_for_local_anchors() const { return std::get<2>(GetParam()); }
  bool known_root() const { return std::get<3>(GetParam()); }
};

INSTANTIATE_TEST_SUITE_P(
    All,
    TLS13DowngradeTest,
    ::testing::Combine(
        ::testing::Values(
            SpawnedTestServer::SSLOptions::TLS_MAX_VERSION_TLS1_0,
            SpawnedTestServer::SSLOptions::TLS_MAX_VERSION_TLS1_1,
            SpawnedTestServer::SSLOptions::TLS_MAX_VERSION_TLS1_2),
        ::testing::Values(false, true),
        ::testing::Values(false, true),
        ::testing::Values(false, true)));

TEST_P(TLS13DowngradeTest, DowngradeEnforced) {
  SpawnedTestServer::SSLOptions ssl_options;
  ssl_options.simulate_tls13_downgrade = simulate_tls13_downgrade();
  ssl_options.tls_max_version = tls_max_version();
  ASSERT_TRUE(StartTestServer(ssl_options));
  scoped_refptr<X509Certificate> server_cert =
      spawned_test_server()->GetCertificate();

  SSLContextConfig config;
  config.version_max = SSL_PROTOCOL_VERSION_TLS1_3;
  config.tls13_hardening_for_local_anchors_enabled = enable_for_local_anchors();
  ssl_config_service_->UpdateSSLConfigAndNotify(config);

  CertVerifyResult verify_result;
  verify_result.is_issued_by_known_root = known_root();
  verify_result.verified_cert = server_cert;
  cert_verifier_->ClearRules();
  cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);

  bool should_enforce = known_root() || enable_for_local_anchors();

  ssl_client_session_cache_->Flush();
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(SSLConfig(), &rv));
  if (should_enforce && simulate_tls13_downgrade()) {
    EXPECT_THAT(rv, IsError(ERR_TLS13_DOWNGRADE_DETECTED));
    EXPECT_FALSE(sock_->IsConnected());
  } else {
    EXPECT_THAT(rv, IsOk());
    EXPECT_TRUE(sock_->IsConnected());
  }
}

struct TLS13DowngradeMetricsParams {
  bool downgrade;
  bool known_root;
  SpawnedTestServer::SSLOptions::KeyExchange key_exchanges;
  bool tls13_experiment_host;
  int expect_downgrade_type;
};

const TLS13DowngradeMetricsParams kTLS13DowngradeMetricsParams[] = {
    // Not a downgrade.
    {false, true, SpawnedTestServer::SSLOptions::KeyExchange::KEY_EXCHANGE_ANY,
     false, -1},
    {false, true, SpawnedTestServer::SSLOptions::KeyExchange::KEY_EXCHANGE_ANY,
     true, -1},
    // Downgrades with a known root.
    {true, true, SpawnedTestServer::SSLOptions::KeyExchange::KEY_EXCHANGE_RSA,
     false, 0},
    {true, true, SpawnedTestServer::SSLOptions::KeyExchange::KEY_EXCHANGE_RSA,
     true, 0},
    {true, true,
     SpawnedTestServer::SSLOptions::KeyExchange::KEY_EXCHANGE_ECDHE_RSA, false,
     1},
    {true, true,
     SpawnedTestServer::SSLOptions::KeyExchange::KEY_EXCHANGE_ECDHE_RSA, true,
     1},
    // Downgrades with an unknown root.
    {true, false, SpawnedTestServer::SSLOptions::KeyExchange::KEY_EXCHANGE_RSA,
     false, 2},
    {true, false, SpawnedTestServer::SSLOptions::KeyExchange::KEY_EXCHANGE_RSA,
     true, 2},
    {true, false,
     SpawnedTestServer::SSLOptions::KeyExchange::KEY_EXCHANGE_ECDHE_RSA, false,
     3},
    {true, false,
     SpawnedTestServer::SSLOptions::KeyExchange::KEY_EXCHANGE_ECDHE_RSA, true,
     3},
};

namespace {
namespace test_default {
#include "net/http/transport_security_state_static_unittest_default.h"
}  // namespace test_default
}  // namespace

class TLS13DowngradeMetricsTest
    : public SSLClientSocketTest,
      public ::testing::WithParamInterface<TLS13DowngradeMetricsParams> {
 public:
  TLS13DowngradeMetricsTest() {
    // Switch the static preload list, so the tests using mail.google.com below
    // do not trip the usual pins.
    SetTransportSecurityStateSourceForTesting(&test_default::kHSTSSource);
  }
  ~TLS13DowngradeMetricsTest() {
    SetTransportSecurityStateSourceForTesting(nullptr);
  }
};

INSTANTIATE_TEST_SUITE_P(All,
                         TLS13DowngradeMetricsTest,
                         ::testing::ValuesIn(kTLS13DowngradeMetricsParams));

TEST_P(TLS13DowngradeMetricsTest, Metrics) {
  const TLS13DowngradeMetricsParams& params = GetParam();
  base::HistogramTester histograms;

  SpawnedTestServer::SSLOptions ssl_options;
  ssl_options.simulate_tls13_downgrade = params.downgrade;
  ssl_options.key_exchanges = params.key_exchanges;
  ASSERT_TRUE(StartTestServer(ssl_options));

  HostPortPair host_port_pair = spawned_test_server()->host_port_pair();
  if (params.tls13_experiment_host) {
    host_port_pair.set_host("mail.google.com");
  }

  if (params.known_root) {
    scoped_refptr<X509Certificate> server_cert =
        spawned_test_server()->GetCertificate();

    // Certificate is trusted and chains to a public root.
    CertVerifyResult verify_result;
    verify_result.is_issued_by_known_root = true;
    verify_result.verified_cert = server_cert;
    cert_verifier_->AddResultForCert(server_cert.get(), verify_result, OK);
  }

  auto transport =
      std::make_unique<TCPClientSocket>(addr(), nullptr, &log_, NetLogSource());
  TestCompletionCallback callback;
  int rv = callback.GetResult(transport->Connect(callback.callback()));
  ASSERT_THAT(rv, IsOk());

  SSLContextConfig config;
  config.version_max = SSL_PROTOCOL_VERSION_TLS1_3;
  ssl_config_service_->UpdateSSLConfigAndNotify(config);

  std::unique_ptr<SSLClientSocket> ssl_socket =
      CreateSSLClientSocket(std::move(transport), host_port_pair, SSLConfig());
  rv = callback.GetResult(ssl_socket->Connect(callback.callback()));
  EXPECT_THAT(rv, AnyOf(IsOk(), IsError(ERR_TLS13_DOWNGRADE_DETECTED)));

  histograms.ExpectUniqueSample("Net.SSLTLS13Downgrade", params.downgrade, 1);
  if (params.tls13_experiment_host) {
    histograms.ExpectUniqueSample("Net.SSLTLS13DowngradeTLS13Experiment",
                                  params.downgrade, 1);
  } else {
    histograms.ExpectTotalCount("Net.SSLTLS13DowngradeTLS13Experiment", 0);
  }

  if (params.downgrade) {
    histograms.ExpectUniqueSample("Net.SSLTLS13DowngradeType",
                                  params.expect_downgrade_type, 1);
  } else {
    histograms.ExpectTotalCount("Net.SSLTLS13DowngradeType", 0);
  }

  if (params.tls13_experiment_host && params.downgrade) {
    histograms.ExpectUniqueSample("Net.SSLTLS13DowngradeTypeTLS13Experiment",
                                  params.expect_downgrade_type, 1);
  } else {
    histograms.ExpectTotalCount("Net.SSLTLS13DowngradeTypeTLS13Experiment", 0);
  }
}

struct SSLHandshakeDetailsParams {
  bool alpn;
  bool early_data;
  uint16_t version;
  SSLHandshakeDetails expected_initial;
  SSLHandshakeDetails expected_resume;
};

const SSLHandshakeDetailsParams kSSLHandshakeDetailsParams[] = {
    // TLS 1.0 and 1.1 never do False Start.
    {false /* no ALPN */, false /* no early data */, SSL_PROTOCOL_VERSION_TLS1,
     SSLHandshakeDetails::kTLS12Full, SSLHandshakeDetails::kTLS12Resume},
    {false /* no ALPN */, false /* no early data */,
     SSL_PROTOCOL_VERSION_TLS1_1, SSLHandshakeDetails::kTLS12Full,
     SSLHandshakeDetails::kTLS12Resume},

    // TLS 1.2 does False Start if ALPN is enabled.
    {false /* no ALPN */, false /* no early data */,
     SSL_PROTOCOL_VERSION_TLS1_2, SSLHandshakeDetails::kTLS12Full,
     SSLHandshakeDetails::kTLS12Resume},
    {true /* ALPN */, false /* no early data */, SSL_PROTOCOL_VERSION_TLS1_2,
     SSLHandshakeDetails::kTLS12FalseStart, SSLHandshakeDetails::kTLS12Resume},

    // TLS 1.3 supports full handshakes, resumption, and 0-RTT.
    {false /* no ALPN */, false /* no early data */,
     SSL_PROTOCOL_VERSION_TLS1_3, SSLHandshakeDetails::kTLS13Full,
     SSLHandshakeDetails::kTLS13Resume},
    {false /* no ALPN */, true /* early data */, SSL_PROTOCOL_VERSION_TLS1_3,
     SSLHandshakeDetails::kTLS13Full, SSLHandshakeDetails::kTLS13Early},
};

class SSLHandshakeDetailsTest
    : public SSLClientSocketTest,
      public ::testing::WithParamInterface<SSLHandshakeDetailsParams> {};

INSTANTIATE_TEST_SUITE_P(All,
                         SSLHandshakeDetailsTest,
                         ::testing::ValuesIn(kSSLHandshakeDetailsParams));

TEST_P(SSLHandshakeDetailsTest, Metrics) {
  // Enable all test features in the server.
  SSLServerConfig server_config;
  server_config.version_min = SSL_PROTOCOL_VERSION_TLS1;
  server_config.version_max = SSL_PROTOCOL_VERSION_TLS1_3;
  server_config.early_data_enabled = true;
  server_config.alpn_protos = {kProtoHTTP11};
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, server_config));

  SSLContextConfig client_context_config;
  client_context_config.version_min = GetParam().version;
  client_context_config.version_max = GetParam().version;
  ssl_config_service_->UpdateSSLConfigAndNotify(client_context_config);

  SSLConfig client_config;
  client_config.version_min_override = GetParam().version;
  client_config.version_max_override = GetParam().version;
  client_config.early_data_enabled = GetParam().early_data;
  if (GetParam().alpn) {
    client_config.alpn_protos = {kProtoHTTP11};
  }

  SSLVersion version;
  switch (GetParam().version) {
    case SSL_PROTOCOL_VERSION_TLS1:
      version = SSL_CONNECTION_VERSION_TLS1;
      break;
    case SSL_PROTOCOL_VERSION_TLS1_1:
      version = SSL_CONNECTION_VERSION_TLS1_1;
      break;
    case SSL_PROTOCOL_VERSION_TLS1_2:
      version = SSL_CONNECTION_VERSION_TLS1_2;
      break;
    case SSL_PROTOCOL_VERSION_TLS1_3:
      version = SSL_CONNECTION_VERSION_TLS1_3;
      break;
    default:
      FAIL() << GetParam().version;
  }

  // Make the initial connection.
  {
    base::HistogramTester histograms;
    int rv;
    ASSERT_TRUE(CreateAndConnectSSLClientSocket(client_config, &rv));
    EXPECT_THAT(rv, IsOk());

    // Sanity-check the socket matches the test parameters.
    SSLInfo info;
    ASSERT_TRUE(sock_->GetSSLInfo(&info));
    EXPECT_EQ(version, SSLConnectionStatusToVersion(info.connection_status));
    EXPECT_EQ(SSLInfo::HANDSHAKE_FULL, info.handshake_type);
    EXPECT_EQ(GetParam().alpn, sock_->WasAlpnNegotiated());

    histograms.ExpectUniqueSample("Net.SSLHandshakeDetails",
                                  GetParam().expected_initial, 1);

    // TLS 1.2 with False Start and TLS 1.3 cause the ticket to arrive later, so
    // use the socket to ensure the session ticket has been picked up.
    EXPECT_THAT(MakeHTTPRequest(sock_.get()), IsOk());
  }

  // Make a resumption connection.
  {
    base::HistogramTester histograms;
    int rv;
    ASSERT_TRUE(CreateAndConnectSSLClientSocket(client_config, &rv));
    EXPECT_THAT(rv, IsOk());

    // Sanity-check the socket matches the test parameters.
    SSLInfo info;
    ASSERT_TRUE(sock_->GetSSLInfo(&info));
    EXPECT_EQ(version, SSLConnectionStatusToVersion(info.connection_status));
    EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, info.handshake_type);
    EXPECT_EQ(GetParam().alpn, sock_->WasAlpnNegotiated());

    histograms.ExpectUniqueSample("Net.SSLHandshakeDetails",
                                  GetParam().expected_resume, 1);
  }
}

TEST_F(SSLClientSocketZeroRTTTest, EarlyDataReasonNewSession) {
  const char kReasonHistogram[] = "Net.SSLHandshakeEarlyDataReason";

  ASSERT_TRUE(StartServer());
  base::HistogramTester histograms;
  ASSERT_TRUE(RunInitialConnection());
  histograms.ExpectUniqueSample(kReasonHistogram,
                                ssl_early_data_no_session_offered, 1);
}

// Test 0-RTT logging when the server declines to resume a connection.
TEST_F(SSLClientSocketZeroRTTTest, EarlyDataReasonNoResume) {
  const char kReasonHistogram[] = "Net.SSLHandshakeEarlyDataReason";

  ASSERT_TRUE(StartServer());
  ASSERT_TRUE(RunInitialConnection());

  SSLServerConfig server_config;
  server_config.early_data_enabled = false;
  server_config.version_max = SSL_PROTOCOL_VERSION_TLS1_3;

  SetServerConfig(server_config);

  base::HistogramTester histograms;

  // 0-RTT Connection
  FakeBlockingStreamSocket* socket = MakeClient(true);
  socket->BlockReadResult();
  ASSERT_THAT(Connect(), IsOk());
  constexpr base::StringPiece kRequest = "GET /zerortt HTTP/1.0\r\n\r\n";
  EXPECT_EQ(static_cast<int>(kRequest.size()), WriteAndWait(kRequest));
  socket->UnblockReadResult();

  // Expect early data to be rejected.
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  int rv = ReadAndWait(buf.get(), 4096);
  EXPECT_EQ(ERR_EARLY_DATA_REJECTED, rv);

  histograms.ExpectUniqueSample(kReasonHistogram,
                                ssl_early_data_session_not_resumed, 1);
}

// Test 0-RTT logging in the standard ConfirmHandshake-after-acceptance case.
TEST_F(SSLClientSocketZeroRTTTest, EarlyDataReasonZeroRTT) {
  const char kReasonHistogram[] = "Net.SSLHandshakeEarlyDataReason";

  ASSERT_TRUE(StartServer());
  ASSERT_TRUE(RunInitialConnection());

  // 0-RTT Connection
  base::HistogramTester histograms;
  MakeClient(true);
  ASSERT_THAT(Connect(), IsOk());
  TestCompletionCallback callback;
  ASSERT_THAT(
      callback.GetResult(ssl_socket()->ConfirmHandshake(callback.callback())),
      IsOk());
  histograms.ExpectUniqueSample(kReasonHistogram, ssl_early_data_accepted, 1);
}

// Check that we're correctly logging 0-rtt success when the handshake
// concludes during a Read.
TEST_F(SSLClientSocketZeroRTTTest, EarlyDataReasonReadServerHello) {
  const char kReasonHistogram[] = "Net.SSLHandshakeEarlyDataReason";
  ASSERT_TRUE(StartServer());
  ASSERT_TRUE(RunInitialConnection());

  // 0-RTT Connection
  base::HistogramTester histograms;
  MakeClient(true);
  ASSERT_THAT(Connect(), IsOk());
  constexpr base::StringPiece kRequest = "GET /zerortt HTTP/1.0\r\n\r\n";
  EXPECT_EQ(static_cast<int>(kRequest.size()), WriteAndWait(kRequest));

  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(4096);
  int size = ReadAndWait(buf.get(), 4096);
  EXPECT_GT(size, 0);
  EXPECT_EQ('1', buf->data()[size - 1]);

  SSLInfo ssl_info;
  ASSERT_TRUE(GetSSLInfo(&ssl_info));
  EXPECT_EQ(SSLInfo::HANDSHAKE_RESUME, ssl_info.handshake_type);

  histograms.ExpectUniqueSample(kReasonHistogram, ssl_early_data_accepted, 1);
}

TEST_F(SSLClientSocketTest, VersionOverride) {
  // Enable all test features in the server.
  SSLServerConfig server_config;
  server_config.version_max = SSL_PROTOCOL_VERSION_TLS1_2;
  ASSERT_TRUE(
      StartEmbeddedTestServer(EmbeddedTestServer::CERT_OK, server_config));

  SSLContextConfig context_config;
  context_config.version_min = SSL_PROTOCOL_VERSION_TLS1_1;
  context_config.version_max = SSL_PROTOCOL_VERSION_TLS1_1;
  ssl_config_service_->UpdateSSLConfigAndNotify(context_config);

  // Connecting normally uses the global configuration.
  SSLConfig config;
  int rv;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(config, &rv));
  EXPECT_THAT(rv, IsOk());
  SSLInfo info;
  ASSERT_TRUE(sock_->GetSSLInfo(&info));
  EXPECT_EQ(SSL_CONNECTION_VERSION_TLS1_1,
            SSLConnectionStatusToVersion(info.connection_status));

  // Individual sockets may override the maximum version.
  config.version_max_override = SSL_PROTOCOL_VERSION_TLS1_2;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(config, &rv));
  EXPECT_THAT(rv, IsOk());
  ASSERT_TRUE(sock_->GetSSLInfo(&info));
  EXPECT_EQ(SSL_CONNECTION_VERSION_TLS1_2,
            SSLConnectionStatusToVersion(info.connection_status));

  // Individual sockets may also override the minimum version.
  config.version_min_override = SSL_PROTOCOL_VERSION_TLS1_3;
  config.version_max_override = SSL_PROTOCOL_VERSION_TLS1_3;
  ASSERT_TRUE(CreateAndConnectSSLClientSocket(config, &rv));
  EXPECT_THAT(rv, IsError(ERR_SSL_VERSION_OR_CIPHER_MISMATCH));
}

}  // namespace net
