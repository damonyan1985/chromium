// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/mediastream/mock_media_stream_video_sink.h"

#include "media/base/bind_to_current_loop.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {

MockMediaStreamVideoSink::MockMediaStreamVideoSink()
    : number_of_frames_(0),
      enabled_(true),
      format_(media::PIXEL_FORMAT_UNKNOWN),
      state_(blink::WebMediaStreamSource::kReadyStateLive) {}

MockMediaStreamVideoSink::~MockMediaStreamVideoSink() {}

blink::VideoCaptureDeliverFrameCB
MockMediaStreamVideoSink::GetDeliverFrameCB() {
  return media::BindToCurrentLoop(
      WTF::BindRepeating(&MockMediaStreamVideoSink::DeliverVideoFrame,
                         weak_factory_.GetWeakPtr()));
}

EncodedVideoFrameCB MockMediaStreamVideoSink::GetDeliverEncodedVideoFrameCB() {
  return media::BindToCurrentLoop(
      WTF::BindRepeating(&MockMediaStreamVideoSink::DeliverEncodedVideoFrame,
                         weak_factory_.GetWeakPtr()));
}

void MockMediaStreamVideoSink::DeliverVideoFrame(
    scoped_refptr<media::VideoFrame> frame,
    base::TimeTicks estimated_capture_time) {
  ++number_of_frames_;
  format_ = frame->format();
  frame_size_ = frame->natural_size();
  last_frame_ = std::move(frame);
  OnVideoFrame();
}

void MockMediaStreamVideoSink::DeliverEncodedVideoFrame(
    scoped_refptr<EncodedVideoFrame> frame,
    base::TimeTicks estimated_capture_time) {
  OnEncodedVideoFrame();
}

void MockMediaStreamVideoSink::OnReadyStateChanged(
    blink::WebMediaStreamSource::ReadyState state) {
  state_ = state;
}

void MockMediaStreamVideoSink::OnEnabledChanged(bool enabled) {
  enabled_ = enabled;
}

void MockMediaStreamVideoSink::OnContentHintChanged(
    WebMediaStreamTrack::ContentHintType content_hint) {
  content_hint_ = content_hint;
}

}  // namespace blink
