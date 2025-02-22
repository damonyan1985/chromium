/*
 * Copyright (C) 2010 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "third_party/blink/renderer/core/fileapi/blob.h"

#include <memory>
#include <utility>

#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/fetch/blob_bytes_consumer.h"
#include "third_party/blink/renderer/core/fetch/body_stream_buffer.h"
#include "third_party/blink/renderer/core/fileapi/blob_property_bag.h"
#include "third_party/blink/renderer/core/fileapi/file_reader_loader.h"
#include "third_party/blink/renderer/core/fileapi/file_reader_loader_client.h"
#include "third_party/blink/renderer/core/frame/web_feature.h"
#include "third_party/blink/renderer/core/url/dom_url.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/blob/blob_url.h"
#include "third_party/blink/renderer/platform/instrumentation/use_counter.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {

// TODO(https://crbug.com/989876): This is not used any more, refactor
// PublicURLManager to deprecate this.
class NullURLRegistry final : public URLRegistry {
 public:
  void RegisterURL(SecurityOrigin*, const KURL&, URLRegistrable*) override {}
  void UnregisterURL(const KURL&) override {}
};

// Helper class to asynchronously read from a Blob using a FileReaderLoader.
// Each client is only good for one Blob read operation.
// Each instance owns itself and will delete itself in the callbacks.
// This class is not thread-safe.
class BlobFileReaderClient : public blink::FileReaderLoaderClient {
 public:
  BlobFileReaderClient(
      const scoped_refptr<BlobDataHandle> blob_data_handle,
      const scoped_refptr<base::SingleThreadTaskRunner> task_runner,
      const FileReaderLoader::ReadType read_type,
      ScriptPromiseResolver* resolver)
      : loader_(std::make_unique<FileReaderLoader>(read_type,
                                                   this,
                                                   std::move(task_runner))),
        resolver_(resolver),
        read_type_(read_type) {
    if (read_type_ == FileReaderLoader::kReadAsText) {
      loader_->SetEncoding("UTF-8");
    }
    loader_->Start(std::move(blob_data_handle));
  }

  ~BlobFileReaderClient() override = default;
  void DidStartLoading() override {}
  void DidReceiveData() override {}
  void DidFail(FileErrorCode error_code) override {
    resolver_->Reject(file_error::CreateDOMException(error_code));
    delete this;
  }

  void DidFinishLoading() override {
    if (read_type_ == FileReaderLoader::kReadAsText) {
      String result = loader_->StringResult();
      resolver_->Resolve(result);
    } else if (read_type_ == FileReaderLoader::kReadAsArrayBuffer) {
      DOMArrayBuffer* result = loader_->ArrayBufferResult();
      resolver_->Resolve(result);
    } else {
      NOTREACHED() << "Unknown ReadType supplied to BlobFileReaderClient";
    }
    delete this;
  }

 private:
  const std::unique_ptr<FileReaderLoader> loader_;
  Persistent<ScriptPromiseResolver> resolver_;
  const FileReaderLoader::ReadType read_type_;
};

Blob::Blob(scoped_refptr<BlobDataHandle> data_handle)
    : blob_data_handle_(std::move(data_handle)) {}

Blob::~Blob() = default;

// static
Blob* Blob::Create(
    ExecutionContext* context,
    const HeapVector<ArrayBufferOrArrayBufferViewOrBlobOrUSVString>& blob_parts,
    const BlobPropertyBag* options) {
  DCHECK(options->hasType());

  DCHECK(options->hasEndings());
  bool normalize_line_endings_to_native = (options->endings() == "native");
  if (normalize_line_endings_to_native)
    UseCounter::Count(context, WebFeature::kFileAPINativeLineEndings);
  UseCounter::Count(context, WebFeature::kCreateObjectBlob);

  auto blob_data = std::make_unique<BlobData>();
  blob_data->SetContentType(NormalizeType(options->type()));

  PopulateBlobData(blob_data.get(), blob_parts,
                   normalize_line_endings_to_native);

  uint64_t blob_size = blob_data->length();
  return MakeGarbageCollected<Blob>(
      BlobDataHandle::Create(std::move(blob_data), blob_size));
}

Blob* Blob::Create(const unsigned char* data,
                   size_t size,
                   const String& content_type) {
  DCHECK(data);

  auto blob_data = std::make_unique<BlobData>();
  blob_data->SetContentType(content_type);
  blob_data->AppendBytes(data, size);
  uint64_t blob_size = blob_data->length();

  return MakeGarbageCollected<Blob>(
      BlobDataHandle::Create(std::move(blob_data), blob_size));
}

// static
void Blob::PopulateBlobData(
    BlobData* blob_data,
    const HeapVector<ArrayBufferOrArrayBufferViewOrBlobOrUSVString>& parts,
    bool normalize_line_endings_to_native) {
  for (const auto& item : parts) {
    if (item.IsArrayBuffer()) {
      DOMArrayBuffer* array_buffer = item.GetAsArrayBuffer();
      blob_data->AppendBytes(array_buffer->Data(),
                             array_buffer->ByteLengthAsSizeT());
    } else if (item.IsArrayBufferView()) {
      DOMArrayBufferView* array_buffer_view =
          item.GetAsArrayBufferView().View();
      blob_data->AppendBytes(array_buffer_view->BaseAddress(),
                             array_buffer_view->byteLengthAsSizeT());
    } else if (item.IsBlob()) {
      item.GetAsBlob()->AppendTo(*blob_data);
    } else if (item.IsUSVString()) {
      blob_data->AppendText(item.GetAsUSVString(),
                            normalize_line_endings_to_native);
    } else {
      NOTREACHED();
    }
  }
}

// static
void Blob::ClampSliceOffsets(uint64_t size, int64_t& start, int64_t& end) {
  DCHECK_NE(size, std::numeric_limits<uint64_t>::max());

  // Convert the negative value that is used to select from the end.
  if (start < 0)
    start = start + size;
  if (end < 0)
    end = end + size;

  // Clamp the range if it exceeds the size limit.
  if (start < 0)
    start = 0;
  if (end < 0)
    end = 0;
  if (static_cast<uint64_t>(start) >= size) {
    start = 0;
    end = 0;
  } else if (end < start) {
    end = start;
  } else if (static_cast<uint64_t>(end) > size) {
    end = size;
  }
}

Blob* Blob::slice(int64_t start,
                  int64_t end,
                  const String& content_type,
                  ExceptionState& exception_state) const {
  uint64_t size = this->size();
  ClampSliceOffsets(size, start, end);

  uint64_t length = end - start;
  auto blob_data = std::make_unique<BlobData>();
  blob_data->SetContentType(NormalizeType(content_type));
  blob_data->AppendBlob(blob_data_handle_, start, length);
  return MakeGarbageCollected<Blob>(
      BlobDataHandle::Create(std::move(blob_data), length));
}

ReadableStream* Blob::stream(ScriptState* script_state) const {
  BodyStreamBuffer* body_buffer = BodyStreamBuffer::Create(
      script_state,
      MakeGarbageCollected<BlobBytesConsumer>(
          ExecutionContext::From(script_state), blob_data_handle_),
      nullptr);

  return body_buffer->Stream();
}

blink::ScriptPromise Blob::text(ScriptState* script_state) {
  auto read_type = FileReaderLoader::kReadAsText;
  return ReadBlobInternal(script_state, read_type);
}

blink::ScriptPromise Blob::arrayBuffer(ScriptState* script_state) {
  auto read_type = FileReaderLoader::kReadAsArrayBuffer;
  return ReadBlobInternal(script_state, read_type);
}

blink::ScriptPromise Blob::ReadBlobInternal(
    ScriptState* script_state,
    FileReaderLoader::ReadType read_type) {
  ScriptPromiseResolver* resolver =
      MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  auto promise = resolver->Promise();

  new BlobFileReaderClient(blob_data_handle_,
                           ExecutionContext::From(script_state)
                               ->GetTaskRunner(TaskType::kFileReading),
                           read_type, resolver);

  return promise;
}

void Blob::AppendTo(BlobData& blob_data) const {
  blob_data.AppendBlob(blob_data_handle_, 0, blob_data_handle_->size());
}

URLRegistry& Blob::Registry() const {
  DEFINE_THREAD_SAFE_STATIC_LOCAL(NullURLRegistry, instance, ());
  return instance;
}

bool Blob::IsMojoBlob() {
  return true;
}

void Blob::CloneMojoBlob(mojo::PendingReceiver<mojom::blink::Blob> receiver) {
  blob_data_handle_->CloneBlobRemote(std::move(receiver));
}

mojo::PendingRemote<mojom::blink::Blob> Blob::AsMojoBlob() {
  return blob_data_handle_->CloneBlobRemote();
}

// static
String Blob::NormalizeType(const String& type) {
  if (type.IsNull())
    return g_empty_string;
  const size_t length = type.length();
  if (length > 65535)
    return g_empty_string;
  if (type.Is8Bit()) {
    const LChar* chars = type.Characters8();
    for (size_t i = 0; i < length; ++i) {
      if (chars[i] < 0x20 || chars[i] > 0x7e)
        return g_empty_string;
    }
  } else {
    const UChar* chars = type.Characters16();
    for (size_t i = 0; i < length; ++i) {
      if (chars[i] < 0x0020 || chars[i] > 0x007e)
        return g_empty_string;
    }
  }
  return type.DeprecatedLower();
}

}  // namespace blink
