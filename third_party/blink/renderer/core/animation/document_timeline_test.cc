/*
 * Copyright (c) 2013, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "third_party/blink/renderer/core/animation/document_timeline.h"

#include "base/test/simple_test_tick_clock.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/animation/animation_clock.h"
#include "third_party/blink/renderer/core/animation/animation_effect.h"
#include "third_party/blink/renderer/core/animation/keyframe_effect.h"
#include "third_party/blink/renderer/core/animation/keyframe_effect_model.h"
#include "third_party/blink/renderer/core/animation/pending_animations.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/dom/qualified_name.h"
#include "third_party/blink/renderer/core/testing/page_test_base.h"
#include "third_party/blink/renderer/platform/animation/compositor_animation_timeline.h"
#include "third_party/blink/renderer/platform/heap/heap.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"

// NaN has the special property that NaN != NaN.
#define EXPECT_NAN(x) EXPECT_NE(x, x)

namespace {
base::TimeTicks TimeTicksFromMillisecondsD(double seconds) {
  return base::TimeTicks() + base::TimeDelta::FromMillisecondsD(seconds);
}
}  // namespace

namespace blink {

class MockPlatformTiming : public DocumentTimeline::PlatformTiming {
 public:
  MOCK_METHOD1(WakeAfter, void(base::TimeDelta));

  void Trace(blink::Visitor* visitor) override {
    DocumentTimeline::PlatformTiming::Trace(visitor);
  }
};

class TestDocumentTimeline : public DocumentTimeline {
 public:
  TestDocumentTimeline(Document* document)
      : DocumentTimeline(document, base::TimeDelta(), nullptr),
        schedule_next_service_called_(false) {}
  void ScheduleServiceOnNextFrame() override {
    DocumentTimeline::ScheduleServiceOnNextFrame();
    schedule_next_service_called_ = true;
  }
  void Trace(blink::Visitor* visitor) override {
    DocumentTimeline::Trace(visitor);
  }
  bool ScheduleNextServiceCalled() const {
    return schedule_next_service_called_;
  }
  void ResetScheduleNextServiceCalled() {
    schedule_next_service_called_ = false;
  }

 private:
  bool schedule_next_service_called_;
};

class AnimationDocumentTimelineTest : public PageTestBase {
 protected:
  void SetUp() override {
    PageTestBase::SetUp(IntSize());
    document = &GetDocument();
    GetAnimationClock().ResetTimeForTesting();
    GetAnimationClock().SetAllowedToDynamicallyUpdateTime(false);
    element =
        MakeGarbageCollected<Element>(QualifiedName::Null(), document.Get());
    document->Timeline().ResetForTesting();
    platform_timing = MakeGarbageCollected<MockPlatformTiming>();
    timeline = MakeGarbageCollected<TestDocumentTimeline>(document);
    timeline->SetTimingForTesting(platform_timing);

    timeline->ResetForTesting();
    ASSERT_EQ(0, timeline->currentTime());
  }

  void TearDown() override {
    document.Release();
    element.Release();
    timeline.Release();
    ThreadState::Current()->CollectAllGarbageForTesting();
  }

  void UpdateClockAndService(double time_ms) {
    GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(time_ms));
    GetPendingAnimations().Update(nullptr, false);
    timeline->ServiceAnimations(kTimingUpdateForAnimationFrame);
    timeline->ScheduleNextService();
  }

  KeyframeEffectModelBase* CreateEmptyEffectModel() {
    return MakeGarbageCollected<StringKeyframeEffectModel>(
        StringKeyframeVector());
  }

  Persistent<Document> document;
  Persistent<Element> element;
  Persistent<TestDocumentTimeline> timeline;
  Timing timing;
  Persistent<MockPlatformTiming> platform_timing;

  double MinimumDelay() { return DocumentTimeline::kMinimumDelay; }
};

class AnimationDocumentTimelineRealTimeTest : public PageTestBase {
 protected:
  void SetUp() override {
    PageTestBase::SetUp(IntSize());
    document = &GetDocument();
    timeline = document->Timeline();
    GetAnimationClock().SetAllowedToDynamicallyUpdateTime(false);
  }

  void TearDown() override {
    document.Release();
    timeline.Release();
    ThreadState::Current()->CollectAllGarbageForTesting();
  }

  Persistent<Document> document;
  Persistent<DocumentTimeline> timeline;
};

TEST_F(AnimationDocumentTimelineTest, EmptyKeyframeAnimation) {
  auto* effect =
      MakeGarbageCollected<StringKeyframeEffectModel>(StringKeyframeVector());
  auto* keyframe_effect =
      MakeGarbageCollected<KeyframeEffect>(element.Get(), effect, timing);

  timeline->Play(keyframe_effect);

  UpdateClockAndService(0);
  EXPECT_FLOAT_EQ(0, timeline->currentTime());
  EXPECT_FALSE(keyframe_effect->IsInEffect());

  UpdateClockAndService(1000);
  EXPECT_FLOAT_EQ(1000, timeline->currentTime());
}

TEST_F(AnimationDocumentTimelineTest, EmptyForwardsKeyframeAnimation) {
  auto* effect =
      MakeGarbageCollected<StringKeyframeEffectModel>(StringKeyframeVector());
  timing.fill_mode = Timing::FillMode::FORWARDS;
  auto* keyframe_effect =
      MakeGarbageCollected<KeyframeEffect>(element.Get(), effect, timing);

  timeline->Play(keyframe_effect);

  UpdateClockAndService(0);
  EXPECT_EQ(0, timeline->currentTime());
  EXPECT_TRUE(keyframe_effect->IsInEffect());

  UpdateClockAndService(1000);
  EXPECT_FLOAT_EQ(1000, timeline->currentTime());
}

TEST_F(AnimationDocumentTimelineTest, ZeroTime) {
  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(1000));
  EXPECT_EQ(1000, timeline->currentTime());

  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(2000));
  EXPECT_EQ(2000, timeline->currentTime());
}

TEST_F(AnimationDocumentTimelineTest, CurrentTimeSeconds) {
  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(2000));
  EXPECT_EQ(2, timeline->CurrentTimeSeconds().value());
  EXPECT_EQ(2000, timeline->currentTime());

  auto* document_without_frame = MakeGarbageCollected<Document>();
  auto* inactive_timeline = MakeGarbageCollected<DocumentTimeline>(
      document_without_frame, base::TimeDelta(), platform_timing);

  EXPECT_FALSE(inactive_timeline->CurrentTimeSeconds());
  EXPECT_NAN(inactive_timeline->currentTime());
  bool is_null = false;
  inactive_timeline->currentTime(is_null);
  EXPECT_TRUE(is_null);
}

TEST_F(AnimationDocumentTimelineTest, PlaybackRateNormal) {
  base::TimeTicks zero_time = timeline->ZeroTime();

  timeline->SetPlaybackRate(1.0);
  EXPECT_EQ(1.0, timeline->PlaybackRate());
  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(1000));
  EXPECT_EQ(zero_time, timeline->ZeroTime());
  EXPECT_EQ(1000, timeline->currentTime());

  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(2000));
  EXPECT_EQ(zero_time, timeline->ZeroTime());
  EXPECT_EQ(2000, timeline->currentTime());
}

TEST_F(AnimationDocumentTimelineTest, PlaybackRateNormalWithOriginTime) {
  base::TimeDelta origin_time = base::TimeDelta::FromMilliseconds(-1000);
  DocumentTimeline* timeline = MakeGarbageCollected<DocumentTimeline>(
      document.Get(), origin_time, platform_timing);
  timeline->ResetForTesting();

  EXPECT_EQ(1.0, timeline->PlaybackRate());
  EXPECT_EQ(base::TimeTicks() + origin_time, timeline->ZeroTime());
  EXPECT_EQ(1000, timeline->currentTime());

  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(100));
  EXPECT_EQ(base::TimeTicks() + origin_time, timeline->ZeroTime());
  EXPECT_EQ(1100, timeline->currentTime());

  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(200));
  EXPECT_EQ(base::TimeTicks() + origin_time, timeline->ZeroTime());
  EXPECT_EQ(1200, timeline->currentTime());
}

TEST_F(AnimationDocumentTimelineTest, PlaybackRatePause) {
  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(1000));
  EXPECT_EQ(base::TimeTicks(), timeline->ZeroTime());
  EXPECT_EQ(1000, timeline->currentTime());

  timeline->SetPlaybackRate(0.0);
  EXPECT_EQ(0.0, timeline->PlaybackRate());
  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(2000));
  EXPECT_EQ(TimeTicksFromMillisecondsD(1000), timeline->ZeroTime());
  EXPECT_EQ(1000, timeline->currentTime());

  timeline->SetPlaybackRate(1.0);
  EXPECT_EQ(1.0, timeline->PlaybackRate());
  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(4000));
  EXPECT_EQ(TimeTicksFromMillisecondsD(1000), timeline->ZeroTime());
  EXPECT_EQ(3000, timeline->currentTime());
}

TEST_F(AnimationDocumentTimelineTest, PlaybackRatePauseWithOriginTime) {
  base::TimeDelta origin_time = base::TimeDelta::FromMilliseconds(-1000);
  DocumentTimeline* timeline = MakeGarbageCollected<DocumentTimeline>(
      document.Get(), origin_time, platform_timing);
  timeline->ResetForTesting();

  EXPECT_EQ(base::TimeTicks() + origin_time, timeline->ZeroTime());
  EXPECT_EQ(1000, timeline->currentTime());
  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(100));
  EXPECT_EQ(base::TimeTicks() + origin_time, timeline->ZeroTime());
  EXPECT_EQ(1100, timeline->currentTime());

  timeline->SetPlaybackRate(0.0);
  EXPECT_EQ(0.0, timeline->PlaybackRate());
  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(200));
  EXPECT_EQ(TimeTicksFromMillisecondsD(1100), timeline->ZeroTime());
  EXPECT_EQ(1100, timeline->currentTime());

  timeline->SetPlaybackRate(1.0);
  EXPECT_EQ(1.0, timeline->PlaybackRate());
  EXPECT_EQ(TimeTicksFromMillisecondsD(-900), timeline->ZeroTime());
  EXPECT_EQ(1100, timeline->currentTime());

  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(400));
  EXPECT_EQ(TimeTicksFromMillisecondsD(-900), timeline->ZeroTime());
  EXPECT_EQ(1300, timeline->currentTime());
}

TEST_F(AnimationDocumentTimelineTest, PlaybackRateSlow) {
  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(1000));
  EXPECT_EQ(base::TimeTicks(), timeline->ZeroTime());
  EXPECT_EQ(1000, timeline->currentTime());

  timeline->SetPlaybackRate(0.5);
  EXPECT_EQ(0.5, timeline->PlaybackRate());
  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(3000));
  EXPECT_EQ(TimeTicksFromMillisecondsD(-1000), timeline->ZeroTime());
  EXPECT_EQ(2000, timeline->currentTime());

  timeline->SetPlaybackRate(1.0);
  EXPECT_EQ(1.0, timeline->PlaybackRate());
  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(4000));
  EXPECT_EQ(TimeTicksFromMillisecondsD(1000), timeline->ZeroTime());
  EXPECT_EQ(3000, timeline->currentTime());
}

TEST_F(AnimationDocumentTimelineTest, PlaybackRateFast) {
  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(1000));
  EXPECT_EQ(base::TimeTicks(), timeline->ZeroTime());
  EXPECT_EQ(1000, timeline->currentTime());

  timeline->SetPlaybackRate(2.0);
  EXPECT_EQ(2.0, timeline->PlaybackRate());
  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(3000));
  EXPECT_EQ(TimeTicksFromMillisecondsD(500), timeline->ZeroTime());
  EXPECT_EQ(5000, timeline->currentTime());

  timeline->SetPlaybackRate(1.0);
  EXPECT_EQ(1.0, timeline->PlaybackRate());
  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(4000));
  EXPECT_EQ(TimeTicksFromMillisecondsD(-2000), timeline->ZeroTime());
  EXPECT_EQ(6000, timeline->currentTime());
}

TEST_F(AnimationDocumentTimelineTest, PlaybackRateFastWithOriginTime) {
  DocumentTimeline* timeline = MakeGarbageCollected<DocumentTimeline>(
      document.Get(), base::TimeDelta::FromSeconds(-1000), platform_timing);
  timeline->ResetForTesting();

  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(100000));
  EXPECT_EQ(TimeTicksFromMillisecondsD(-1000000), timeline->ZeroTime());
  EXPECT_EQ(1100000, timeline->currentTime());

  timeline->SetPlaybackRate(2.0);
  EXPECT_EQ(2.0, timeline->PlaybackRate());
  EXPECT_EQ(TimeTicksFromMillisecondsD(-450000), timeline->ZeroTime());
  EXPECT_EQ(1100000, timeline->currentTime());

  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(300000));
  EXPECT_EQ(TimeTicksFromMillisecondsD(-450000), timeline->ZeroTime());
  EXPECT_EQ(1500000, timeline->currentTime());

  timeline->SetPlaybackRate(1.0);
  EXPECT_EQ(1.0, timeline->PlaybackRate());
  EXPECT_EQ(TimeTicksFromMillisecondsD(-1200000), timeline->ZeroTime());
  EXPECT_EQ(1500000, timeline->currentTime());

  GetAnimationClock().UpdateTime(TimeTicksFromMillisecondsD(400000));
  EXPECT_EQ(TimeTicksFromMillisecondsD(-1200000), timeline->ZeroTime());
  EXPECT_EQ(1600000, timeline->currentTime());
}

TEST_F(AnimationDocumentTimelineTest, PauseForTesting) {
  float seek_time = 1;
  timing.fill_mode = Timing::FillMode::FORWARDS;
  auto* anim1 = MakeGarbageCollected<KeyframeEffect>(
      element.Get(), CreateEmptyEffectModel(), timing);
  auto* anim2 = MakeGarbageCollected<KeyframeEffect>(
      element.Get(), CreateEmptyEffectModel(), timing);
  Animation* animation1 = timeline->Play(anim1);
  Animation* animation2 = timeline->Play(anim2);
  timeline->PauseAnimationsForTesting(seek_time);

  EXPECT_FLOAT_EQ(seek_time * 1000, animation1->currentTime());
  EXPECT_FLOAT_EQ(seek_time * 1000, animation2->currentTime());
}

TEST_F(AnimationDocumentTimelineTest, DelayBeforeAnimationStart) {
  timing.iteration_duration = AnimationTimeDelta::FromSecondsD(2);
  timing.start_delay = 5;

  auto* keyframe_effect = MakeGarbageCollected<KeyframeEffect>(
      element.Get(), CreateEmptyEffectModel(), timing);

  timeline->Play(keyframe_effect);

  // TODO: Put the animation startTime in the future when we add the capability
  // to change animation startTime
  EXPECT_CALL(*platform_timing, WakeAfter(base::TimeDelta::FromSecondsD(
                                    timing.start_delay - MinimumDelay())));
  UpdateClockAndService(0);

  EXPECT_CALL(*platform_timing,
              WakeAfter(base::TimeDelta::FromSecondsD(timing.start_delay -
                                                      MinimumDelay() - 1.5)));
  UpdateClockAndService(1500);

  timeline->ScheduleServiceOnNextFrame();

  timeline->ResetScheduleNextServiceCalled();
  UpdateClockAndService(4980);
  EXPECT_TRUE(timeline->ScheduleNextServiceCalled());
}

TEST_F(AnimationDocumentTimelineTest, UseAnimationAfterTimelineDeref) {
  Animation* animation = timeline->Play(nullptr);
  timeline.Clear();
  // Test passes if this does not crash.
  animation->setStartTime(0, false);
}

TEST_F(AnimationDocumentTimelineTest, PlayAfterDocumentDeref) {
  timing.iteration_duration = AnimationTimeDelta::FromSecondsD(2);
  timing.start_delay = 5;

  DocumentTimeline* timeline = &document->Timeline();
  document = nullptr;

  auto* keyframe_effect = MakeGarbageCollected<KeyframeEffect>(
      nullptr, CreateEmptyEffectModel(), timing);
  // Test passes if this does not crash.
  timeline->Play(keyframe_effect);
}

// Regression test for https://crbug.com/995806, ensuring that we do dynamically
// progress the time when outside a rendering loop (so that we can serve e.g.
// setInterval), but also that we *only* dynamically progress the time when
// outside a rendering loop (so that we are mostly spec compliant).
TEST_F(AnimationDocumentTimelineTest,
       PredictionBehaviorOnlyAppliesOutsideRenderingLoop) {
  base::SimpleTestTickClock test_clock;
  GetAnimationClock().OverrideDynamicClockForTesting(&test_clock);
  ASSERT_EQ(GetAnimationClock().CurrentTime(), test_clock.NowTicks());

  // As long as we are inside the rendering loop, we shouldn't update even
  // across tasks.
  base::TimeTicks before_time = GetAnimationClock().CurrentTime();
  test_clock.Advance(base::TimeDelta::FromSeconds(1));
  EXPECT_EQ(GetAnimationClock().CurrentTime(), before_time);

  AnimationClock::NotifyTaskStart();
  test_clock.Advance(base::TimeDelta::FromSeconds(1));
  EXPECT_EQ(GetAnimationClock().CurrentTime(), before_time);

  // Once we leave the rendering loop, however, it is valid for the time to
  // increase *once* per task.
  GetAnimationClock().SetAllowedToDynamicallyUpdateTime(true);
  EXPECT_GT(GetAnimationClock().CurrentTime(), before_time);

  // The clock shouldn't tick again until we change task, however.
  base::TimeTicks current_time = GetAnimationClock().CurrentTime();
  test_clock.Advance(base::TimeDelta::FromSeconds(1));
  EXPECT_EQ(GetAnimationClock().CurrentTime(), current_time);
  AnimationClock::NotifyTaskStart();
  EXPECT_GT(GetAnimationClock().CurrentTime(), current_time);
}

// Ensure that origin time is correctly calculated even when the animation
// clock has not yet been initialized.
TEST_F(AnimationDocumentTimelineRealTimeTest,
       PlaybackRateChangeUninitalizedAnimationClock) {
  GetAnimationClock().ResetTimeForTesting();
  EXPECT_TRUE(GetAnimationClock().CurrentTime().is_null());
  EXPECT_FALSE(
      document->Loader()->GetTiming().ReferenceMonotonicTime().is_null());

  base::TimeDelta origin_time = base::TimeDelta::FromSeconds(1000);
  DocumentTimeline* timeline =
      MakeGarbageCollected<DocumentTimeline>(document.Get(), origin_time);
  timeline->SetPlaybackRate(0.5);
  EXPECT_EQ(origin_time * 2,
            timeline->ZeroTime() - document->Timeline().ZeroTime());
}

}  // namespace blink
