// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/scheduler/main_thread/user_model.h"

namespace blink {
namespace scheduler {

UserModel::UserModel()
    : pending_input_event_count_(0),
      is_gesture_active_(false),
      is_gesture_expected_(false) {}

void UserModel::DidStartProcessingInputEvent(blink::WebInputEvent::Type type,
                                             const base::TimeTicks now) {
  last_input_signal_time_ = now;
  if (type == blink::WebInputEvent::kTouchStart ||
      type == blink::WebInputEvent::kGestureScrollBegin ||
      type == blink::WebInputEvent::kGesturePinchBegin) {
    // Only update stats once per gesture.
    if (!is_gesture_active_)
      last_gesture_start_time_ = now;

    is_gesture_active_ = true;
  }

  // We need to track continuous gestures seperatly for scroll detection
  // because taps should not be confused with scrolls.
  if (type == blink::WebInputEvent::kGestureScrollBegin ||
      type == blink::WebInputEvent::kGestureScrollEnd ||
      type == blink::WebInputEvent::kGestureScrollUpdate ||
      type == blink::WebInputEvent::kGestureFlingStart ||
      type == blink::WebInputEvent::kGestureFlingCancel ||
      type == blink::WebInputEvent::kGesturePinchBegin ||
      type == blink::WebInputEvent::kGesturePinchEnd ||
      type == blink::WebInputEvent::kGesturePinchUpdate) {
    last_continuous_gesture_time_ = now;
  }

  // If the gesture has ended, clear |is_gesture_active_| and record a UMA
  // metric that tracks its duration.
  if (type == blink::WebInputEvent::kGestureScrollEnd ||
      type == blink::WebInputEvent::kGesturePinchEnd ||
      type == blink::WebInputEvent::kGestureFlingStart ||
      type == blink::WebInputEvent::kTouchEnd) {
    is_gesture_active_ = false;
  }

  TRACE_COUNTER1(TRACE_DISABLED_BY_DEFAULT("renderer.scheduler"),
                 "is_gesture_active", is_gesture_active_);

  pending_input_event_count_++;
}

void UserModel::DidFinishProcessingInputEvent(const base::TimeTicks now) {
  last_input_signal_time_ = now;
  if (pending_input_event_count_ > 0)
    pending_input_event_count_--;
}

base::TimeDelta UserModel::TimeLeftInUserGesture(base::TimeTicks now) const {
  base::TimeDelta escalated_priority_duration =
      base::TimeDelta::FromMilliseconds(kGestureEstimationLimitMillis);

  // If the input event is still pending, go into input prioritized policy and
  // check again later.
  if (pending_input_event_count_ > 0)
    return escalated_priority_duration;
  if (last_input_signal_time_.is_null() ||
      last_input_signal_time_ + escalated_priority_duration < now) {
    return base::TimeDelta();
  }
  return last_input_signal_time_ + escalated_priority_duration - now;
}

bool UserModel::IsGestureExpectedSoon(
    const base::TimeTicks now,
    base::TimeDelta* prediction_valid_duration) {
  bool was_gesture_expected = is_gesture_expected_;
  is_gesture_expected_ =
      IsGestureExpectedSoonImpl(now, prediction_valid_duration);

  // Track when we start expecting a gesture so we can work out later if a
  // gesture actually happened.
  if (!was_gesture_expected && is_gesture_expected_)
    last_gesture_expected_start_time_ = now;
  return is_gesture_expected_;
}

bool UserModel::IsGestureExpectedSoonImpl(
    const base::TimeTicks now,
    base::TimeDelta* prediction_valid_duration) const {
  if (is_gesture_active_) {
    if (IsGestureExpectedToContinue(now, prediction_valid_duration))
      return false;
    *prediction_valid_duration =
        base::TimeDelta::FromMilliseconds(kExpectSubsequentGestureMillis);
    return true;
  } else {
    // If we've have a finished a gesture then a subsequent gesture is deemed
    // likely.
    base::TimeDelta expect_subsequent_gesture_for =
        base::TimeDelta::FromMilliseconds(kExpectSubsequentGestureMillis);
    if (last_continuous_gesture_time_.is_null() ||
        last_continuous_gesture_time_ + expect_subsequent_gesture_for <= now) {
      return false;
    }
    *prediction_valid_duration =
        last_continuous_gesture_time_ + expect_subsequent_gesture_for - now;
    return true;
  }
}

bool UserModel::IsGestureExpectedToContinue(
    const base::TimeTicks now,
    base::TimeDelta* prediction_valid_duration) const {
  if (!is_gesture_active_)
    return false;

  base::TimeDelta median_gesture_duration =
      base::TimeDelta::FromMilliseconds(kMedianGestureDurationMillis);
  base::TimeTicks expected_gesture_end_time =
      last_gesture_start_time_ + median_gesture_duration;

  if (expected_gesture_end_time > now) {
    *prediction_valid_duration = expected_gesture_end_time - now;
    return true;
  }
  return false;
}

void UserModel::Reset(base::TimeTicks now) {
  last_input_signal_time_ = base::TimeTicks();
  last_gesture_start_time_ = base::TimeTicks();
  last_continuous_gesture_time_ = base::TimeTicks();
  last_gesture_expected_start_time_ = base::TimeTicks();
  last_reset_time_ = now;
  is_gesture_active_ = false;
  is_gesture_expected_ = false;
  pending_input_event_count_ = 0;
}

void UserModel::AsValueInto(base::trace_event::TracedValue* state) const {
  state->BeginDictionary("user_model");
  state->SetInteger("pending_input_event_count", pending_input_event_count_);
  state->SetDouble(
      "last_input_signal_time",
      (last_input_signal_time_ - base::TimeTicks()).InMillisecondsF());
  state->SetDouble(
      "last_gesture_start_time",
      (last_gesture_start_time_ - base::TimeTicks()).InMillisecondsF());
  state->SetDouble(
      "last_continuous_gesture_time",
      (last_continuous_gesture_time_ - base::TimeTicks()).InMillisecondsF());
  state->SetDouble("last_gesture_expected_start_time",
                   (last_gesture_expected_start_time_ - base::TimeTicks())
                       .InMillisecondsF());
  state->SetDouble("last_reset_time",
                   (last_reset_time_ - base::TimeTicks()).InMillisecondsF());
  state->SetBoolean("is_gesture_expected", is_gesture_expected_);
  state->SetBoolean("is_gesture_active", is_gesture_active_);
  state->EndDictionary();
}

}  // namespace scheduler
}  // namespace blink
