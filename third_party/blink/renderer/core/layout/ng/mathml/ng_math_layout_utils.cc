// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/ng/mathml/ng_math_layout_utils.h"

#include "third_party/blink/renderer/core/layout/ng/ng_block_node.h"
#include "third_party/blink/renderer/core/layout/ng/ng_constraint_space_builder.h"
#include "third_party/blink/renderer/core/layout/ng/ng_length_utils.h"
#include "third_party/blink/renderer/core/layout/ng/ng_space_utils.h"

namespace blink {

NGConstraintSpace CreateConstraintSpaceForMathChild(
    const NGBlockNode& parent_node,
    const LogicalSize& child_available_size,
    const NGConstraintSpace& parent_constraint_space,
    const NGLayoutInputNode& child) {
  const ComputedStyle& parent_style = parent_node.Style();
  const ComputedStyle& child_style = child.Style();
  DCHECK(child.CreatesNewFormattingContext());
  NGConstraintSpaceBuilder space_builder(parent_constraint_space,
                                         child_style.GetWritingMode(),
                                         true /* is_new_fc */);
  SetOrthogonalFallbackInlineSizeIfNeeded(parent_style, child, &space_builder);

  space_builder.SetAvailableSize(child_available_size);
  space_builder.SetPercentageResolutionSize(child_available_size);
  space_builder.SetReplacedPercentageResolutionSize(child_available_size);

  // TODO(rbuis): add target stretch sizes.

  space_builder.SetTextDirection(child_style.Direction());

  // TODO(rbuis): add ink baselines?
  space_builder.AddBaselineRequest(
      {NGBaselineAlgorithmType::kFirstLine, kAlphabeticBaseline});
  return space_builder.ToConstraintSpace();
}

}  // namespace blink
