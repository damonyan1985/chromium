// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_TABS_TAB_GROUP_MODEL_H_
#define CHROME_BROWSER_UI_TABS_TAB_GROUP_MODEL_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <memory>
#include <vector>

#include "base/optional.h"

class TabGroup;
class TabGroupController;

namespace tab_groups {
class TabGroupId;
class TabGroupVisualData;
}  // namespace tab_groups

// A model for all tab groups with at least one tab in the tabstrip. Keeps a map
// of tab_groups::TabGroupIds and TabGroups and provides an API for maintaining
// it. It is owned and used primarily by TabStripModel, which handles
// tab-to-group correspondences and any groupedness state changes on tabs.
// TabStipModel then notifies TabStrip of any groupedness state changes that
// need to be reflected in the view.
class TabGroupModel {
 public:
  explicit TabGroupModel(TabGroupController* controller);
  ~TabGroupModel();

  // Registers a tab group and returns the newly registered group. It will
  // initially be empty, but the expectation is that at least one tab will be
  // added to it immediately.
  TabGroup* AddTabGroup(
      tab_groups::TabGroupId id,
      base::Optional<tab_groups::TabGroupVisualData> visual_data);

  // Returns whether a tab group with the given |id| exists.
  bool ContainsTabGroup(tab_groups::TabGroupId id) const;

  // Returns the tab group with the given |id|. The group must exist.
  TabGroup* GetTabGroup(tab_groups::TabGroupId id) const;

  // Removes the tab group with the given |id| from the registry. Should be
  // called whenever the group becomes empty.
  void RemoveTabGroup(tab_groups::TabGroupId id);

  std::vector<tab_groups::TabGroupId> ListTabGroups() const;

 private:
  std::map<tab_groups::TabGroupId, std::unique_ptr<TabGroup>> groups_;

  TabGroupController* controller_;
};

#endif  // CHROME_BROWSER_UI_TABS_TAB_GROUP_MODEL_H_
