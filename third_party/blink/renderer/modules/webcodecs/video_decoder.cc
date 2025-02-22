// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/webcodecs/video_decoder.h"
#include "third_party/blink/renderer/core/dom/dom_exception.h"
#include "third_party/blink/renderer/core/streams/readable_stream.h"
#include "third_party/blink/renderer/core/streams/writable_stream.h"
#include "third_party/blink/renderer/modules/webcodecs/video_decoder_init_parameters.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"

namespace blink {

// static
VideoDecoder* VideoDecoder::Create() {
  return MakeGarbageCollected<VideoDecoder>();
}

VideoDecoder::VideoDecoder() = default;

ScriptPromise VideoDecoder::Initialize(
    ScriptState* script_state,
    const VideoDecoderInitParameters* params) {
  return ScriptPromise::RejectWithDOMException(
      script_state,
      MakeGarbageCollected<DOMException>(DOMExceptionCode::kNotSupportedError,
                                         "Not implemented yet."));
}

ScriptPromise VideoDecoder::Flush(ScriptState* script_state) {
  return ScriptPromise::RejectWithDOMException(
      script_state,
      MakeGarbageCollected<DOMException>(DOMExceptionCode::kNotSupportedError,
                                         "Not implemented yet."));
}

void VideoDecoder::Close() {}

ReadableStream* VideoDecoder::readable() const {
  return nullptr;
}

WritableStream* VideoDecoder::writable() const {
  return nullptr;
}

void VideoDecoder::Trace(Visitor* visitor) {
  ScriptWrappable::Trace(visitor);
}

}  // namespace blink
