// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_TAB_GROUPS_TAB_GROUP_VISUAL_DATA_H_
#define COMPONENTS_TAB_GROUPS_TAB_GROUP_VISUAL_DATA_H_

#include <stddef.h>

#include "base/component_export.h"
#include "base/macros.h"
#include "base/strings/string16.h"
#include "third_party/skia/include/core/SkColor.h"

namespace tab_groups {

class COMPONENT_EXPORT(TAB_GROUPS) TabGroupVisualData {
 public:
  // Construct a TabGroupVisualData with placeholder name and random color.
  TabGroupVisualData();

  TabGroupVisualData(base::string16 title, SkColor color);

  TabGroupVisualData(const TabGroupVisualData& other) = default;
  TabGroupVisualData(TabGroupVisualData&& other) = default;

  TabGroupVisualData& operator=(const TabGroupVisualData& other) = default;
  TabGroupVisualData& operator=(TabGroupVisualData&& other) = default;

  base::string16 title() const { return title_; }
  SkColor color() const { return color_; }

  // Checks whether two instances are visually equivalent.
  bool operator==(const TabGroupVisualData& other) const {
    return title_ == other.title_ && color_ == other.color_;
  }
  bool operator!=(const TabGroupVisualData& other) const {
    return !(*this == other);
  }

 private:
  base::string16 title_;
  SkColor color_;
};

}  // namespace tab_groups

#endif  // COMPONENTS_TAB_GROUPS_TAB_GROUP_VISUAL_DATA_H_
