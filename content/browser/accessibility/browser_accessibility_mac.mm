// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <Cocoa/Cocoa.h>

#import "content/browser/accessibility/browser_accessibility_mac.h"

#include "base/task/post_task.h"
#include "base/time/time.h"
#import "content/browser/accessibility/browser_accessibility_cocoa.h"
#include "content/browser/accessibility/browser_accessibility_manager_mac.h"

namespace content {

// Static.
BrowserAccessibility* BrowserAccessibility::Create() {
  return new BrowserAccessibilityMac();
}

BrowserAccessibilityMac::BrowserAccessibilityMac()
    : browser_accessibility_cocoa_(NULL) {}

bool BrowserAccessibilityMac::IsNative() const {
  return true;
}

void BrowserAccessibilityMac::NativeReleaseReference() {
  // Detach this object from |browser_accessibility_cocoa_| so it
  // no longer has a pointer to this object.
  [browser_accessibility_cocoa_ detach];
  // Now, release it - but at this point, other processes may have a
  // reference to the cocoa object.
  [browser_accessibility_cocoa_ release];
  // Finally, it's safe to delete this since we've detached.
  delete this;
}

void BrowserAccessibilityMac::OnDataChanged() {
  BrowserAccessibility::OnDataChanged();

  if (browser_accessibility_cocoa_) {
    [browser_accessibility_cocoa_ childrenChanged];
    return;
  }

  // We take ownership of the Cocoa object here.
  browser_accessibility_cocoa_ =
      [[BrowserAccessibilityCocoa alloc] initWithObject:this];
}

// Replace a native object and refocus if it had focus.
// This will force VoiceOver to re-announce it, and refresh Braille output.
void BrowserAccessibilityMac::ReplaceNativeObject() {
  BrowserAccessibilityCocoa* old_native_obj = browser_accessibility_cocoa_;
  browser_accessibility_cocoa_ =
      [[BrowserAccessibilityCocoa alloc] initWithObject:this];

  // Replace child in parent.
  BrowserAccessibility* parent = PlatformGetParent();
  if (!parent)
    return;

  base::scoped_nsobject<NSMutableArray> new_children;
  NSArray* old_children = [ToBrowserAccessibilityCocoa(parent) children];
  for (uint i = 0; i < [old_children count]; ++i) {
    BrowserAccessibilityCocoa* child = [old_children objectAtIndex:i];
    if (child == old_native_obj)
      [new_children addObject:browser_accessibility_cocoa_];
    else
      [new_children addObject:child];
  }
  [ToBrowserAccessibilityCocoa(parent) swapChildren:&new_children];

  // If focused, fire a focus notification on the new native object.
  if (manager_->GetFocus() == this) {
    NSAccessibilityPostNotification(
        browser_accessibility_cocoa_,
        NSAccessibilityFocusedUIElementChangedNotification);
  }

  // Destroy after a delay so that VO is securely on the new focus first,
  // otherwise the focus event will not be announced.
  // We use 1000ms; however, this magic number isn't necessary to avoid
  // use-after-free or anything scary like that. The worst case scenario if this
  // gets destroyed, too early is that VoiceOver announces the wrong thing once.
  base::scoped_nsobject<BrowserAccessibilityCocoa> retained_destroyed_node(
      [old_native_obj retain]);

  base::PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](base::scoped_nsobject<BrowserAccessibilityCocoa> destroyed) {
            if (destroyed && [destroyed instanceActive]) {
              // Follow destruction pattern from NativeReleaseReference().
              [destroyed detach];
              [destroyed release];
            }
          },
          std::move(retained_destroyed_node)),
      base::TimeDelta::FromMilliseconds(1000));
}

uint32_t BrowserAccessibilityMac::PlatformChildCount() const {
  uint32_t child_count = BrowserAccessibility::PlatformChildCount();

  // If this is a table, include the extra fake nodes generated by
  // AXTableInfo, for the column nodes and the table header container, all of
  // which are only important on macOS.
  const std::vector<ui::AXNode*>* extra_mac_nodes = node()->GetExtraMacNodes();
  if (!extra_mac_nodes)
    return child_count;

  return child_count + extra_mac_nodes->size();
}

BrowserAccessibility* BrowserAccessibilityMac::PlatformGetChild(
    uint32_t child_index) const {
  if (child_index < BrowserAccessibility::PlatformChildCount())
    return BrowserAccessibility::PlatformGetChild(child_index);

  if (child_index >= PlatformChildCount())
    return nullptr;

  // If this is a table, include the extra fake nodes generated by
  // AXTableInfo, for the column nodes and the table header container, all of
  // which are only important on macOS.
  const std::vector<ui::AXNode*>* extra_mac_nodes = node()->GetExtraMacNodes();
  if (!extra_mac_nodes || extra_mac_nodes->empty())
    return nullptr;

  child_index -= BrowserAccessibility::PlatformChildCount();
  if (child_index < extra_mac_nodes->size())
    return manager_->GetFromAXNode((*extra_mac_nodes)[child_index]);

  return nullptr;
}

BrowserAccessibility* BrowserAccessibilityMac::PlatformGetFirstChild() const {
  return PlatformGetChild(0);
}

BrowserAccessibility* BrowserAccessibilityMac::PlatformGetLastChild() const {
  const std::vector<ui::AXNode*>* extra_mac_nodes = node()->GetExtraMacNodes();
  if (extra_mac_nodes && !extra_mac_nodes->empty())
    return manager_->GetFromAXNode(extra_mac_nodes->back());
  return BrowserAccessibility::PlatformGetLastChild();
}

BrowserAccessibility* BrowserAccessibilityMac::PlatformGetNextSibling() const {
  BrowserAccessibility* parent = PlatformGetParent();
  if (parent) {
    uint32_t next_child_index = node()->GetUnignoredIndexInParent() + 1;
    if (next_child_index >= parent->InternalChildCount() &&
        next_child_index < parent->PlatformChildCount()) {
      // get the extra_mac_node
      return parent->PlatformGetChild(next_child_index);
    } else if (next_child_index >= parent->PlatformChildCount()) {
      return nullptr;
    }
  }
  return BrowserAccessibility::PlatformGetNextSibling();
}

BrowserAccessibility* BrowserAccessibilityMac::PlatformGetPreviousSibling()
    const {
  BrowserAccessibility* parent = PlatformGetParent();
  if (parent) {
    uint32_t previous_child_index = node()->GetUnignoredIndexInParent() - 1;
    if (previous_child_index >= parent->InternalChildCount() &&
        previous_child_index < parent->PlatformChildCount()) {
      // get the extra_mac_node
      return parent->PlatformGetChild(previous_child_index);
    } else if (previous_child_index < 0) {
      return nullptr;
    }
  }
  return BrowserAccessibility::PlatformGetPreviousSibling();
}

const BrowserAccessibilityCocoa* ToBrowserAccessibilityCocoa(
    const BrowserAccessibility* obj) {
  DCHECK(obj);
  DCHECK(obj->IsNative());
  return static_cast<const BrowserAccessibilityMac*>(obj)->native_view();
}

BrowserAccessibilityCocoa* ToBrowserAccessibilityCocoa(
    BrowserAccessibility* obj) {
  DCHECK(obj);
  DCHECK(obj->IsNative());
  return static_cast<BrowserAccessibilityMac*>(obj)->native_view();
}

}  // namespace content
