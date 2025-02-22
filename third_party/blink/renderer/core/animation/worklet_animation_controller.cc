// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/animation/worklet_animation_controller.h"

#include "third_party/blink/renderer/core/animation/document_timeline.h"
#include "third_party/blink/renderer/core/animation/scroll_timeline.h"
#include "third_party/blink/renderer/core/animation/worklet_animation_base.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/platform/graphics/animation_worklet_mutator_dispatcher_impl.h"
#include "third_party/blink/renderer/platform/graphics/main_thread_mutator_client.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

namespace {

int GetId(const WorkletAnimationBase& animation) {
  return animation.GetWorkletAnimationId().animation_id;
}

}  // namespace

WorkletAnimationController::WorkletAnimationController(Document* document)
    : document_(document) {}

WorkletAnimationController::~WorkletAnimationController() = default;

void WorkletAnimationController::AttachAnimation(
    WorkletAnimationBase& animation) {
  DCHECK(IsMainThread());
  DCHECK(!pending_animations_.Contains(&animation));
  DCHECK(!animations_.Contains(GetId(animation)));
  pending_animations_.insert(&animation);

  DCHECK_EQ(document_, animation.GetDocument());
  if (LocalFrameView* view = animation.GetDocument()->View())
    view->ScheduleAnimation();
}

void WorkletAnimationController::DetachAnimation(
    WorkletAnimationBase& animation) {
  DCHECK(IsMainThread());
  pending_animations_.erase(&animation);
  animations_.erase(GetId(animation));
}

void WorkletAnimationController::InvalidateAnimation(
    WorkletAnimationBase& animation) {
  DCHECK(IsMainThread());
  pending_animations_.insert(&animation);
  if (LocalFrameView* view = animation.GetDocument()->View())
    view->ScheduleAnimation();
}

void WorkletAnimationController::UpdateAnimationStates() {
  DCHECK(IsMainThread());
  HeapHashSet<Member<WorkletAnimationBase>> animations;
  animations.swap(pending_animations_);
  for (const auto& animation : animations) {
    animation->UpdateCompositingState();
    if (animation->IsActiveAnimation())
      animations_.insert(GetId(*animation), animation);
  }
  if (!animations_.IsEmpty() && document_->View())
    document_->View()->ScheduleAnimation();
}

void WorkletAnimationController::UpdateAnimationTimings(
    TimingUpdateReason reason) {
  DCHECK(IsMainThread());
  // Worklet animations inherited time values are only ever updated once per
  // animation frame. This means the inherited time does not change outside of
  // the frame so return early in the on-demand case.
  if (reason == kTimingUpdateOnDemand)
    return;

  MutateAnimations();
  ApplyAnimationTimings(reason);
}

void WorkletAnimationController::ScrollSourceCompositingStateChanged(
    Node* node) {
  DCHECK(ScrollTimeline::HasActiveScrollTimeline(node));
  for (const auto& animation : animations_.Values()) {
    if (animation->GetTimeline()->IsScrollTimeline() &&
        To<ScrollTimeline>(animation->GetTimeline())->scrollSource() == node) {
      InvalidateAnimation(*animation);
    }
  }
}

base::WeakPtr<AnimationWorkletMutatorDispatcherImpl>
WorkletAnimationController::EnsureMainThreadMutatorDispatcher(
    scoped_refptr<base::SingleThreadTaskRunner>* mutator_task_runner) {
  base::WeakPtr<AnimationWorkletMutatorDispatcherImpl> mutator_dispatcher;
  if (!mutator_task_runner_) {
    main_thread_mutator_client_ =
        AnimationWorkletMutatorDispatcherImpl::CreateMainThreadClient(
            &mutator_dispatcher, &mutator_task_runner_);
    main_thread_mutator_client_->SetDelegate(this);
  }

  DCHECK(main_thread_mutator_client_);
  DCHECK(mutator_task_runner_);
  DCHECK(mutator_dispatcher);
  *mutator_task_runner = mutator_task_runner_;
  return mutator_dispatcher;
}

// TODO(yigu): Currently one animator name is synced back per registration.
// Eventually all registered names should be synced in batch once a module
// completes its loading in the worklet scope. https://crbug.com/920722.
void WorkletAnimationController::SynchronizeAnimatorName(
    const String& animator_name) {
  animator_names_.insert(animator_name);
}

bool WorkletAnimationController::IsAnimatorRegistered(
    const String& animator_name) const {
  return animator_names_.Contains(animator_name);
}

void WorkletAnimationController::SetMutationUpdate(
    std::unique_ptr<AnimationWorkletOutput> output_state) {
  if (!output_state)
    return;

  for (auto& to_update : output_state->animations) {
    int id = to_update.worklet_animation_id.animation_id;
    if (auto* animation = animations_.at(id))
      animation->SetOutputState(to_update);
  }
}

void WorkletAnimationController::MutateAnimations() {
  if (!main_thread_mutator_client_)
    return;

  main_thread_mutator_client_->Mutator()->MutateSynchronously(
      CollectAnimationStates());
}

std::unique_ptr<AnimationWorkletDispatcherInput>
WorkletAnimationController::CollectAnimationStates() {
  std::unique_ptr<AnimationWorkletDispatcherInput> result =
      std::make_unique<AnimationWorkletDispatcherInput>();

  for (auto& animation : animations_.Values())
    animation->UpdateInputState(result.get());

  return result;
}

void WorkletAnimationController::ApplyAnimationTimings(
    TimingUpdateReason reason) {
  for (const auto& animation : animations_.Values())
    animation->Update(reason);
}

void WorkletAnimationController::Trace(blink::Visitor* visitor) {
  visitor->Trace(pending_animations_);
  visitor->Trace(animations_);
  visitor->Trace(document_);
}

}  // namespace blink
