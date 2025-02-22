// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/ng/custom/custom_layout_work_task.h"

#include "third_party/blink/renderer/bindings/core/v8/serialization/serialized_script_value.h"
#include "third_party/blink/renderer/core/layout/ng/custom/custom_intrinsic_sizes.h"
#include "third_party/blink/renderer/core/layout/ng/custom/custom_layout_child.h"
#include "third_party/blink/renderer/core/layout/ng/custom/custom_layout_constraints_options.h"
#include "third_party/blink/renderer/core/layout/ng/custom/custom_layout_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/ng_block_node.h"
#include "third_party/blink/renderer/core/layout/ng/ng_constraint_space_builder.h"
#include "third_party/blink/renderer/core/layout/ng/ng_layout_result.h"
#include "third_party/blink/renderer/core/layout/ng/ng_length_utils.h"
#include "third_party/blink/renderer/core/layout/ng/ng_space_utils.h"

namespace blink {

CustomLayoutWorkTask::CustomLayoutWorkTask(CustomLayoutChild* child,
                                           CustomLayoutToken* token,
                                           ScriptPromiseResolver* resolver,
                                           const TaskType type)
    : CustomLayoutWorkTask(child, token, resolver, nullptr, nullptr, type) {}

CustomLayoutWorkTask::CustomLayoutWorkTask(
    CustomLayoutChild* child,
    CustomLayoutToken* token,
    ScriptPromiseResolver* resolver,
    const CustomLayoutConstraintsOptions* options,
    scoped_refptr<SerializedScriptValue> constraint_data,
    const TaskType type)
    : child_(child),
      token_(token),
      resolver_(resolver),
      options_(options),
      constraint_data_(std::move(constraint_data)),
      type_(type) {}

CustomLayoutWorkTask::~CustomLayoutWorkTask() = default;

void CustomLayoutWorkTask::Run(const NGBlockNode& parent,
                               const NGConstraintSpace& parent_space,
                               const ComputedStyle& parent_style,
                               const NGBoxStrut& border_scrollbar_padding) {
  DCHECK(token_->IsValid());
  NGLayoutInputNode child = child_->GetLayoutNode();

  if (type_ == CustomLayoutWorkTask::TaskType::kIntrinsicSizes) {
    RunIntrinsicSizesTask(parent, parent_space, parent_style,
                          border_scrollbar_padding, child);
  } else {
    DCHECK_EQ(type_, CustomLayoutWorkTask::TaskType::kLayoutFragment);
    RunLayoutFragmentTask(parent_space, parent_style, child);
  }
}

void CustomLayoutWorkTask::RunLayoutFragmentTask(
    const NGConstraintSpace& parent_space,
    const ComputedStyle& parent_style,
    NGLayoutInputNode child) {
  DCHECK_EQ(type_, CustomLayoutWorkTask::TaskType::kLayoutFragment);
  DCHECK(options_ && resolver_);

  NGConstraintSpaceBuilder builder(parent_space, child.Style().GetWritingMode(),
                                   /* is_new_fc */ true);
  SetOrthogonalFallbackInlineSizeIfNeeded(parent_style, child, &builder);

  bool is_fixed_inline_size = false;
  bool is_fixed_block_size = false;
  LogicalSize available_size;
  LogicalSize percentage_size;

  if (options_->hasFixedInlineSize()) {
    is_fixed_inline_size = true;
    available_size.inline_size =
        LayoutUnit::FromDoubleRound(options_->fixedInlineSize());
  } else {
    available_size.inline_size =
        options_->hasAvailableInlineSize() &&
                options_->availableInlineSize() >= 0.0
            ? LayoutUnit::FromDoubleRound(options_->availableInlineSize())
            : LayoutUnit();
  }

  if (options_->hasFixedBlockSize()) {
    is_fixed_block_size = true;
    available_size.block_size =
        LayoutUnit::FromDoubleRound(options_->fixedBlockSize());
  } else {
    available_size.block_size =
        options_->hasAvailableBlockSize() &&
                options_->availableBlockSize() >= 0.0
            ? LayoutUnit::FromDoubleRound(options_->availableBlockSize())
            : LayoutUnit();
  }

  if (options_->hasPercentageInlineSize() &&
      options_->percentageInlineSize() >= 0.0) {
    percentage_size.inline_size =
        LayoutUnit::FromDoubleRound(options_->percentageInlineSize());
  } else if (options_->hasAvailableInlineSize() &&
             options_->availableInlineSize() >= 0.0) {
    percentage_size.inline_size =
        LayoutUnit::FromDoubleRound(options_->availableInlineSize());
  }

  if (options_->hasPercentageBlockSize() &&
      options_->percentageBlockSize() >= 0.0) {
    percentage_size.block_size =
        LayoutUnit::FromDoubleRound(options_->percentageBlockSize());
  } else if (options_->hasAvailableBlockSize() &&
             options_->availableBlockSize() >= 0.0) {
    percentage_size.block_size =
        LayoutUnit::FromDoubleRound(options_->availableBlockSize());
  } else {
    percentage_size.block_size = kIndefiniteSize;
  }

  builder.SetTextDirection(child.Style().Direction());
  builder.SetAvailableSize(available_size);
  builder.SetPercentageResolutionSize(percentage_size);
  builder.SetReplacedPercentageResolutionSize(percentage_size);
  builder.SetIsShrinkToFit(child.Style().LogicalWidth().IsAuto());
  builder.SetIsFixedInlineSize(is_fixed_inline_size);
  builder.SetIsFixedBlockSize(is_fixed_block_size);
  if (child.IsLayoutNGCustom())
    builder.SetCustomLayoutData(std::move(constraint_data_));
  auto space = builder.ToConstraintSpace();
  auto result = To<NGBlockNode>(child).Layout(space, nullptr /* break_token */);

  LogicalSize size = result->PhysicalFragment().Size().ConvertToLogical(
      parent_space.GetWritingMode());

  resolver_->Resolve(MakeGarbageCollected<CustomLayoutFragment>(
      child_, token_, std::move(result), size,
      resolver_->GetScriptState()->GetIsolate()));
}

void CustomLayoutWorkTask::RunIntrinsicSizesTask(
    const NGBlockNode& parent,
    const NGConstraintSpace& parent_space,
    const ComputedStyle& parent_style,
    const NGBoxStrut& border_scrollbar_padding,
    NGLayoutInputNode child) {
  DCHECK_EQ(type_, CustomLayoutWorkTask::TaskType::kIntrinsicSizes);
  DCHECK(resolver_);

  // TODO(almaher) should use border_padding instead of
  // border_scrollbar_padding.
  LayoutUnit child_percentage_resolution_block_size =
      CalculateChildPercentageBlockSizeForMinMax(
          parent_space, parent, border_scrollbar_padding,
          parent_space.PercentageResolutionBlockSize());

  MinMaxSizeInput input(child_percentage_resolution_block_size);
  MinMaxSize sizes =
      ComputeMinAndMaxContentContribution(parent_style, child, input);
  resolver_->Resolve(MakeGarbageCollected<CustomIntrinsicSizes>(
      child_, token_, sizes.min_size, sizes.max_size));
}

}  // namespace blink
