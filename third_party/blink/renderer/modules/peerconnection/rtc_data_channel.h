/*
 * Copyright (C) 2012 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_PEERCONNECTION_RTC_DATA_CHANNEL_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_PEERCONNECTION_RTC_DATA_CHANNEL_H_

#include <memory>

#include "base/gtest_prod_util.h"
#include "base/single_thread_task_runner.h"
#include "base/threading/thread_checker.h"
#include "third_party/blink/renderer/bindings/core/v8/active_script_wrappable.h"
#include "third_party/blink/renderer/core/execution_context/context_lifecycle_observer.h"
#include "third_party/blink/renderer/core/typed_arrays/array_buffer_view_helpers.h"
#include "third_party/blink/renderer/modules/event_target_modules.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/timer.h"
#include "third_party/webrtc/api/peer_connection_interface.h"

namespace blink {

class Blob;
class DOMArrayBuffer;
class DOMArrayBufferView;
class ExceptionState;
class RTCPeerConnectionHandlerPlatform;

class MODULES_EXPORT RTCDataChannel final
    : public EventTargetWithInlineData,
      public ActiveScriptWrappable<RTCDataChannel>,
      public ContextLifecycleObserver {
  USING_GARBAGE_COLLECTED_MIXIN(RTCDataChannel);
  DEFINE_WRAPPERTYPEINFO();
  USING_PRE_FINALIZER(RTCDataChannel, Dispose);

 public:
  RTCDataChannel(ExecutionContext*,
                 scoped_refptr<webrtc::DataChannelInterface> channel,
                 RTCPeerConnectionHandlerPlatform* peer_connection_handler);
  ~RTCDataChannel() override;

  String label() const;

  // DEPRECATED
  bool reliable() const;

  bool ordered() const;
  uint16_t maxPacketLifeTime(bool&) const;
  uint16_t maxRetransmits(bool&) const;
  String protocol() const;
  bool negotiated() const;
  uint16_t id(bool& is_null) const;
  String readyState() const;
  unsigned bufferedAmount() const;

  unsigned bufferedAmountLowThreshold() const;
  void setBufferedAmountLowThreshold(unsigned);

  String binaryType() const;
  void setBinaryType(const String&, ExceptionState&);

  void send(const String&, ExceptionState&);
  void send(DOMArrayBuffer*, ExceptionState&);
  void send(NotShared<DOMArrayBufferView>, ExceptionState&);
  void send(Blob*, ExceptionState&);

  void close();

  DEFINE_ATTRIBUTE_EVENT_LISTENER(open, kOpen)
  DEFINE_ATTRIBUTE_EVENT_LISTENER(bufferedamountlow, kBufferedamountlow)
  DEFINE_ATTRIBUTE_EVENT_LISTENER(error, kError)
  DEFINE_ATTRIBUTE_EVENT_LISTENER(close, kClose)
  DEFINE_ATTRIBUTE_EVENT_LISTENER(message, kMessage)

  // EventTarget
  const AtomicString& InterfaceName() const override;
  ExecutionContext* GetExecutionContext() const override;

  // ContextLifecycleObserver
  void ContextDestroyed(ExecutionContext*) override;

  // ScriptWrappable
  bool HasPendingActivity() const override;

  void Trace(blink::Visitor*) override;

 private:
  friend class Observer;
  // Implementation of webrtc::DataChannelObserver that receives events on
  // webrtc's signaling thread and forwards them over to the main thread for
  // handling. Since the |blink_channel_|'s lifetime is scoped potentially
  // narrower than the |webrtc_channel_|, the observer is reference counted to
  // make sure all callbacks have a valid pointer but won't do anything if the
  // |blink_channel_| has gone away.
  class Observer : public WTF::ThreadSafeRefCounted<RTCDataChannel::Observer>,
                   public webrtc::DataChannelObserver {
   public:
    Observer(scoped_refptr<base::SingleThreadTaskRunner> main_thread,
             RTCDataChannel* blink_channel,
             scoped_refptr<webrtc::DataChannelInterface> channel);
    ~Observer() override;

    // Returns a reference to |webrtc_channel_|. Typically called from the main
    // thread except for on observer registration, done in a synchronous call to
    // the signaling thread (safe because the call is synchronous).
    const scoped_refptr<webrtc::DataChannelInterface>& channel() const;

    // Clears the |blink_channel_| reference, disassociates this observer from
    // the |webrtc_channel_| and releases the |webrtc_channel_| pointer. Must be
    // called on the main thread.
    void Unregister();

    // webrtc::DataChannelObserver implementation, called from signaling thread.
    void OnStateChange() override;
    void OnBufferedAmountChange(uint64_t sent_data_size) override;
    void OnMessage(const webrtc::DataBuffer& buffer) override;

   private:
    // webrtc::DataChannelObserver implementation on the main thread.
    void OnStateChangeImpl(webrtc::DataChannelInterface::DataState state);
    void OnBufferedAmountChangeImpl(unsigned sent_data_size);
    void OnMessageImpl(std::unique_ptr<webrtc::DataBuffer> buffer);

    const scoped_refptr<base::SingleThreadTaskRunner> main_thread_;
    WeakPersistent<RTCDataChannel> blink_channel_;
    scoped_refptr<webrtc::DataChannelInterface> webrtc_channel_;
  };

  void OnStateChange(webrtc::DataChannelInterface::DataState state);
  void OnBufferedAmountChange(unsigned previous_amount);
  void OnMessage(std::unique_ptr<webrtc::DataBuffer> buffer);

  void Dispose();

  void ScheduleDispatchEvent(Event*);
  void ScheduledEventTimerFired(TimerBase*);

  const scoped_refptr<webrtc::DataChannelInterface>& channel() const;
  bool SendRawData(const char* data, size_t length);

  webrtc::DataChannelInterface::DataState state_;

  enum BinaryType { kBinaryTypeBlob, kBinaryTypeArrayBuffer };
  BinaryType binary_type_;

  TaskRunnerTimer<RTCDataChannel> scheduled_event_timer_;
  HeapVector<Member<Event>> scheduled_events_;
  FRIEND_TEST_ALL_PREFIXES(RTCDataChannelTest, Open);
  FRIEND_TEST_ALL_PREFIXES(RTCDataChannelTest, Close);
  FRIEND_TEST_ALL_PREFIXES(RTCDataChannelTest, Message);
  FRIEND_TEST_ALL_PREFIXES(RTCDataChannelTest, BufferedAmountLow);

  unsigned buffered_amount_low_threshold_;
  unsigned buffered_amount_;
  bool stopped_;
  scoped_refptr<Observer> observer_;
  THREAD_CHECKER(thread_checker_);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_PEERCONNECTION_RTC_DATA_CHANNEL_H_
