// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_PUBLIC_COMMON_INPUT_WEB_MOUSE_EVENT_H_
#define THIRD_PARTY_BLINK_PUBLIC_COMMON_INPUT_WEB_MOUSE_EVENT_H_

#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_menu_source_type.h"
#include "third_party/blink/public/common/input/web_pointer_properties.h"

namespace blink {

class WebGestureEvent;

// WebMouseEvent --------------------------------------------------------------

class BLINK_COMMON_EXPORT WebMouseEvent : public WebInputEvent,
                                          public WebPointerProperties {
 public:
  static constexpr PointerId kMousePointerId = std::numeric_limits<int>::max();

  int click_count;

  // Only used for contextmenu events.
  WebMenuSourceType menu_source_type;

  WebMouseEvent(Type type_param,
                gfx::PointF position,
                gfx::PointF global_position,
                Button button_param,
                int click_count_param,
                int modifiers_param,
                base::TimeTicks time_stamp_param,
                WebMenuSourceType menu_source_type_param = kMenuSourceNone,
                PointerId id_param = kMousePointerId)
      : WebInputEvent(sizeof(WebMouseEvent),
                      type_param,
                      modifiers_param,
                      time_stamp_param),
        WebPointerProperties(id_param,
                             PointerType::kMouse,
                             button_param,
                             position,
                             global_position),
        click_count(click_count_param),
        menu_source_type(menu_source_type_param) {
    DCHECK_GE(type_param, kMouseTypeFirst);
    DCHECK_LE(type_param, kMouseTypeLast);
  }

  WebMouseEvent(Type type_param,
                int modifiers_param,
                base::TimeTicks time_stamp_param,
                PointerId id_param = kMousePointerId)
      : WebMouseEvent(sizeof(WebMouseEvent),
                      type_param,
                      modifiers_param,
                      time_stamp_param,
                      id_param) {}

  WebMouseEvent() : WebMouseEvent(sizeof(WebMouseEvent), kMousePointerId) {}

  bool FromTouch() const {
    return (GetModifiers() & kIsCompatibilityEventForTouch) != 0;
  }

  WebMouseEvent(Type type_param,
                const WebGestureEvent&,
                Button button_param,
                int click_count_param,
                int modifiers_param,
                base::TimeTicks time_stamp_param,
                PointerId id_param = kMousePointerId);

  gfx::PointF PositionInRootFrame() const;

  // Sets any scaled values to be their computed values and sets |frame_scale_|
  // back to 1 and |frame_translate_| X and Y coordinates back to 0.
  WebMouseEvent FlattenTransform() const;

 protected:
  WebMouseEvent(unsigned size_param, PointerId id_param)
      : WebInputEvent(size_param), WebPointerProperties(id_param) {}

  WebMouseEvent(unsigned size_param,
                Type type,
                int modifiers,
                base::TimeTicks time_stamp,
                PointerId id_param)
      : WebInputEvent(size_param, type, modifiers, time_stamp),
        WebPointerProperties(id_param) {}

  void FlattenTransformSelf();

 private:
  void SetMenuSourceType(WebInputEvent::Type);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_PUBLIC_COMMON_INPUT_WEB_MOUSE_EVENT_H_
