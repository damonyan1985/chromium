// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/accessibility/ax_node_position.h"

#include "base/strings/string_util.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/accessibility/ax_tree_manager_map.h"

namespace ui {

AXTree* AXNodePosition::tree_ = nullptr;

// static
AXNodePosition::AXPositionInstance AXNodePosition::CreatePosition(
    const AXNode& node,
    int child_index_or_text_offset,
    ax::mojom::TextAffinity affinity) {
  if (!node.tree())
    return CreateNullPosition();

  AXTreeID tree_id = node.tree()->GetAXTreeID();
  if (node.IsText())
    return CreateTextPosition(tree_id, node.id(), child_index_or_text_offset,
                              affinity);

  return CreateTreePosition(tree_id, node.id(), child_index_or_text_offset);
}

AXNodePosition::AXNodePosition() = default;

AXNodePosition::~AXNodePosition() = default;

AXNodePosition::AXNodePosition(const AXNodePosition& other)
    : AXPosition<AXNodePosition, AXNode>(other) {}

AXNodePosition::AXPositionInstance AXNodePosition::Clone() const {
  return AXPositionInstance(new AXNodePosition(*this));
}

void AXNodePosition::AnchorChild(int child_index,
                                 AXTreeID* tree_id,
                                 AXNode::AXID* child_id) const {
  DCHECK(tree_id);
  DCHECK(child_id);

  if (!GetAnchor() || child_index < 0 || child_index >= AnchorChildCount()) {
    *tree_id = AXTreeIDUnknown();
    *child_id = AXNode::kInvalidAXID;
    return;
  }

  AXNode* child = nullptr;

  const AXTreeManager* child_tree_manager =
      AXTreeManagerMap::GetInstance().GetManagerForChildTree(*GetAnchor());
  if (child_tree_manager) {
    // The child node exists in a separate tree from its parent.
    child = child_tree_manager->GetRootAsAXNode();
    *tree_id = child_tree_manager->GetTreeID();
  } else {
    child = GetAnchor()->children()[size_t{child_index}];
    *tree_id = this->tree_id();
  }

  DCHECK(child);
  *child_id = child->id();
}

int AXNodePosition::AnchorChildCount() const {
  if (!GetAnchor())
    return 0;

  const AXTreeManager* child_tree_manager =
      AXTreeManagerMap::GetInstance().GetManagerForChildTree(*GetAnchor());
  if (child_tree_manager) {
    return 1;
  }

  return int{GetAnchor()->children().size()};
}

int AXNodePosition::AnchorIndexInParent() const {
  return GetAnchor() ? int{GetAnchor()->index_in_parent()} : INVALID_INDEX;
}

base::stack<AXNode*> AXNodePosition::GetAncestorAnchors() const {
  base::stack<AXNode*> anchors;
  AXNode* current_anchor = GetAnchor();

  AXNode::AXID current_anchor_id = GetAnchor()->id();
  AXTreeID current_tree_id = this->tree_id();

  AXNode::AXID parent_anchor_id = AXNode::kInvalidAXID;
  AXTreeID parent_tree_id = AXTreeIDUnknown();

  while (current_anchor) {
    anchors.push(current_anchor);
    current_anchor = GetParent(
        current_anchor /*child*/, current_tree_id /*child_tree_id*/,
        &parent_tree_id /*parent_tree_id*/, &parent_anchor_id /*parent_id*/);

    current_anchor_id = parent_anchor_id;
    current_tree_id = parent_tree_id;
  }
  return anchors;
}

void AXNodePosition::AnchorParent(AXTreeID* tree_id,
                                  AXNode::AXID* parent_id) const {
  DCHECK(tree_id);
  DCHECK(parent_id);

  *tree_id = AXTreeIDUnknown();
  *parent_id = AXNode::kInvalidAXID;

  if (!GetAnchor())
    return;

  AXNode* parent =
      GetParent(GetAnchor() /*child*/, this->tree_id() /*child_tree_id*/,
                tree_id /*parent_tree_id*/, parent_id /*parent_id*/);

  if (!parent) {
    *tree_id = AXTreeIDUnknown();
    *parent_id = AXNode::kInvalidAXID;
  }
}

AXNode* AXNodePosition::GetNodeInTree(AXTreeID tree_id,
                                      AXNode::AXID node_id) const {
  if (node_id == AXNode::kInvalidAXID)
    return nullptr;

  // Used for testing via AXNodePosition::SetTree
  if (AXNodePosition::tree_)
    return AXNodePosition::tree_->GetFromId(node_id);

  AXTreeManager* manager = AXTreeManagerMap::GetInstance().GetManager(tree_id);
  if (manager)
    return manager->GetNodeFromTree(tree_id, node_id);

  return nullptr;
}

base::string16 AXNodePosition::GetText() const {
  if (IsNullPosition())
    return {};

  const AXNode* anchor = GetAnchor();
  DCHECK(anchor);
  base::string16 value = GetAnchor()->data().GetString16Attribute(
      ax::mojom::StringAttribute::kValue);
  if (!value.empty())
    return value;

  if (anchor->IsText()) {
    return anchor->data().GetString16Attribute(
        ax::mojom::StringAttribute::kName);
  }

  base::string16 text;
  for (int i = 0; i < AnchorChildCount(); ++i)
    text += CreateChildPositionAt(i)->GetText();

  return text;
}

bool AXNodePosition::IsInLineBreak() const {
  if (IsNullPosition())
    return false;
  DCHECK(GetAnchor());
  return GetAnchor()->IsLineBreak();
}

bool AXNodePosition::IsInTextObject() const {
  if (IsNullPosition())
    return false;
  DCHECK(GetAnchor());
  return GetAnchor()->IsText();
}

bool AXNodePosition::IsInWhiteSpace() const {
  if (IsNullPosition())
    return false;
  DCHECK(GetAnchor());
  return GetAnchor()->IsLineBreak() ||
         base::ContainsOnlyChars(GetText(), base::kWhitespaceUTF16);
}

// This override is an optimized version AXPosition::MaxTextOffset. Instead of
// concatenating the strings in GetText() to then get their text length, we sum
// the lengths of the individual strings. This is faster than concatenating the
// strings first and then taking their length, especially when the process
// is recursive.
int AXNodePosition::MaxTextOffset() const {
  if (IsNullPosition())
    return INVALID_OFFSET;

  const AXNode* anchor = GetAnchor();
  DCHECK(anchor);
  base::string16 value = GetAnchor()->data().GetString16Attribute(
      ax::mojom::StringAttribute::kValue);
  if (!value.empty())
    return value.length();

  if (anchor->IsText()) {
    return anchor->data()
        .GetString16Attribute(ax::mojom::StringAttribute::kName)
        .length();
  }

  int text_length = 0;
  for (int i = 0; i < AnchorChildCount(); ++i)
    text_length += CreateChildPositionAt(i)->MaxTextOffset();

  return text_length;
}

bool AXNodePosition::IsInLineBreakingObject() const {
  if (IsNullPosition())
    return false;
  DCHECK(GetAnchor());
  return GetAnchor()->data().GetBoolAttribute(
             ax::mojom::BoolAttribute::kIsLineBreakingObject) &&
         !GetAnchor()->IsInListMarker();
}

ax::mojom::Role AXNodePosition::GetRole() const {
  if (IsNullPosition())
    return ax::mojom::Role::kNone;
  DCHECK(GetAnchor());
  return GetAnchor()->data().role;
}

AXNodeTextStyles AXNodePosition::GetTextStyles() const {
  // Check either the current anchor or its parent for text styles.
  AXNodeTextStyles current_anchor_text_styles =
      !IsNullPosition() ? GetAnchor()->data().GetTextStyles()
                        : AXNodeTextStyles();
  if (current_anchor_text_styles.IsUnset()) {
    AXPositionInstance parent = CreateParentPosition();
    if (!parent->IsNullPosition())
      return parent->GetAnchor()->data().GetTextStyles();
  }
  return current_anchor_text_styles;
}

std::vector<int32_t> AXNodePosition::GetWordStartOffsets() const {
  if (IsNullPosition())
    return std::vector<int32_t>();
  DCHECK(GetAnchor());
  return GetAnchor()->data().GetIntListAttribute(
      ax::mojom::IntListAttribute::kWordStarts);
}

std::vector<int32_t> AXNodePosition::GetWordEndOffsets() const {
  if (IsNullPosition())
    return std::vector<int32_t>();
  DCHECK(GetAnchor());
  return GetAnchor()->data().GetIntListAttribute(
      ax::mojom::IntListAttribute::kWordEnds);
}

AXNode::AXID AXNodePosition::GetNextOnLineID(AXNode::AXID node_id) const {
  if (IsNullPosition())
    return AXNode::kInvalidAXID;
  AXNode* node = GetNodeInTree(tree_id(), node_id);
  int next_on_line_id;
  if (!node || !node->data().GetIntAttribute(
                   ax::mojom::IntAttribute::kNextOnLineId, &next_on_line_id)) {
    return AXNode::kInvalidAXID;
  }
  return static_cast<AXNode::AXID>(next_on_line_id);
}

AXNode::AXID AXNodePosition::GetPreviousOnLineID(AXNode::AXID node_id) const {
  if (IsNullPosition())
    return AXNode::kInvalidAXID;
  AXNode* node = GetNodeInTree(tree_id(), node_id);
  int previous_on_line_id;
  if (!node ||
      !node->data().GetIntAttribute(ax::mojom::IntAttribute::kPreviousOnLineId,
                                    &previous_on_line_id)) {
    return AXNode::kInvalidAXID;
  }
  return static_cast<AXNode::AXID>(previous_on_line_id);
}

AXNode* AXNodePosition::GetParent(AXNode* child,
                                  AXTreeID child_tree_id,
                                  AXTreeID* parent_tree_id,
                                  AXNode::AXID* parent_id) {
  DCHECK(parent_tree_id);
  DCHECK(parent_id);

  *parent_tree_id = AXTreeIDUnknown();
  *parent_id = AXNode::kInvalidAXID;

  if (!child)
    return nullptr;

  AXNode* parent = child->parent();
  *parent_tree_id = child_tree_id;

  if (!parent) {
    AXTreeManager* manager =
        AXTreeManagerMap::GetInstance().GetManager(child_tree_id);
    if (manager) {
      parent = manager->GetParentNodeFromParentTreeAsAXNode();
      *parent_tree_id = manager->GetParentTreeID();
    }
  }

  if (!parent) {
    *parent_tree_id = AXTreeIDUnknown();
    return parent;
  }

  *parent_id = parent->id();
  return parent;
}

}  // namespace ui
