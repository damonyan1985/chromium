// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/ng/mathml/layout_ng_mathml_block.h"

#include "third_party/blink/renderer/core/layout/layout_analyzer.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/layout/ng/ng_block_node.h"
#include "third_party/blink/renderer/core/layout/ng/ng_constraint_space.h"
#include "third_party/blink/renderer/core/layout/ng/ng_layout_result.h"
#include "third_party/blink/renderer/core/layout/ng/ng_out_of_flow_positioned_node.h"
#include "third_party/blink/renderer/core/layout/ng/ng_physical_box_fragment.h"

namespace blink {

LayoutNGMathMLBlock::LayoutNGMathMLBlock(MathMLElement* element)
    : LayoutNGMixin<LayoutBlock>(element) {
  DCHECK(element);
}

void LayoutNGMathMLBlock::UpdateBlockLayout(bool relayout_children) {
  LayoutAnalyzer::BlockScope analyzer(*this);

  if (IsOutOfFlowPositioned()) {
    UpdateOutOfFlowBlockLayout();
    return;
  }

  NGConstraintSpace constraint_space =
      NGConstraintSpace::CreateFromLayoutObject(
          *this, !View()->GetLayoutState()->Next() /* is_layout_root */);

  scoped_refptr<const NGLayoutResult> result =
      NGBlockNode(this).Layout(constraint_space);

  for (const auto& descendant :
       result->PhysicalFragment().OutOfFlowPositionedDescendants())
    descendant.node.UseLegacyOutOfFlowPositioning();
}

bool LayoutNGMathMLBlock::IsOfType(LayoutObjectType type) const {
  return type == kLayoutObjectMathML ||
         (type == kLayoutObjectMathMLRoot && GetNode() &&
          GetNode()->HasTagName(mathml_names::kMathTag)) ||
         LayoutNGMixin<LayoutBlock>::IsOfType(type);
}

bool LayoutNGMathMLBlock::IsChildAllowed(LayoutObject* child,
                                         const ComputedStyle&) const {
  return child->GetNode() && child->GetNode()->IsMathMLElement();
}

}  // namespace blink
