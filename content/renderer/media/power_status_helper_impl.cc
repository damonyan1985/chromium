// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/media/power_status_helper_impl.h"

#include "base/metrics/histogram_macros.h"
#include "services/device/public/mojom/battery_status.mojom.h"

using ::device::mojom::BatteryStatusPtr;

namespace content {

namespace {

static constexpr const char* kBatteryDeltaHistogram =
    "Media.PlaybackPower.BatteryDelta";
static constexpr const char* kElapsedTimeHistogram =
    "Media.PlaybackPower.ElapsedTime";

// Minimum enum value that we'll generate, inclusive.
static constexpr int kMinEnumValue = 0;

// Maximum enum value that we'll generate, inclusive.
static constexpr int kMaxEnumValue =
    PowerStatusHelperImpl::Bits::kCodecBitsH264 |
    PowerStatusHelperImpl::Bits::kCodecBitsVP9Profile0 |
    PowerStatusHelperImpl::Bits::kCodecBitsVP9Profile2 |

    PowerStatusHelperImpl::Bits::kResolution360p |
    PowerStatusHelperImpl::Bits::kResolution720p |
    PowerStatusHelperImpl::Bits::kResolution1080p |

    PowerStatusHelperImpl::Bits::kFrameRate30 |
    PowerStatusHelperImpl::Bits::kFrameRate60 |

    PowerStatusHelperImpl::Bits::kFullScreenNo |
    PowerStatusHelperImpl::Bits::kFullScreenYes;

// UMA buckets are always [uma_min, uma_max).  The first bucket is an implicit
// underflow bucket [0, uma_min), and the last is the overflow bucket
// [uma_max, infinity).  The underflow bucket isn't counted, but the overflow
// bucket is.

// Minimum bucket number.  Since we always get an underflow bucket, we choose
// the minimum bucket to be one higher than the minimum enum value.  That way.
// the minimum enum goes to the underflow bucket.
// //tools/metrics/histograms/README.md#count-histograms_choosing-min-and-max
// for details.
static constexpr int kMinUmaValue = kMinEnumValue + 1;

// We want to avoid using the overflow bucket. See
// //tools/metrics/histograms/README.md#count-histograms_choosing-min-and-max
// and UMA_HISTOGRAM_EXACT_LINEAR for details.

// Max value as reported to UMA, which is the lower bound of the overflow
// bucket.  Add one, since we want the overflow bucket to be unused.
static constexpr int kMaxUmaValue = kMaxEnumValue + 1;

// Number of buckets we want, which includes the overflow bucket but not the
// implicit underflow bucket.
static constexpr int kNumUmaBuckets = kMaxUmaValue - kMinUmaValue + 1;

// For example, if kMinEnum == 0 and kMaxEnum == 5 (inclusive), then:
// kMinUma = 1 (0 is implicit), kMaxUma = 6, and we'll want the implicit
// underflow bucket plus kNumUmaBuckets = 6 more.
// [0, 1)  << implicit underflow bucket, kMinEnum goes here
// [1, 2)
// [2, 3)
// [3, 4)
// [4, 5)
// [5, 6)  << kMaxEnum goes here
// [6, infinity)  << unused overflow bucket.

// We can have more, but shouldn't without talking to metrics folks.
static_assert(kNumUmaBuckets < 100, "Too many buckets");

}  // namespace

PowerStatusHelperImpl::PowerStatusHelperImpl(
    CreateBatteryMonitorCB create_battery_monitor_cb)
    : create_battery_monitor_cb_(std::move(create_battery_monitor_cb)) {}

PowerStatusHelperImpl::~PowerStatusHelperImpl() = default;

// static
base::Optional<int> PowerStatusHelperImpl::BucketFor(
    bool is_playing,
    bool has_video,
    media::VideoCodec codec,
    media::VideoCodecProfile profile,
    gfx::Size natural_size,
    bool is_fullscreen,
    base::TimeDelta average_duration) {
  if (!is_playing)
    return {};

  if (!has_video)
    return {};

  int bucket = 0;

  if (codec == media::VideoCodec::kCodecH264)
    bucket |= Bits::kCodecBitsH264;
  else if (profile == media::VP9PROFILE_PROFILE0)
    bucket |= Bits::kCodecBitsVP9Profile0;
  else if (profile == media::VP9PROFILE_PROFILE2)
    bucket |= Bits::kCodecBitsVP9Profile2;
  else
    return {};

  // We could take into account rotation, but ignore it for now.
  if (natural_size == gfx::Size(640, 360))
    bucket |= kResolution360p;
  else if (natural_size == gfx::Size(1280, 720))
    bucket |= kResolution720p;
  else if (natural_size == gfx::Size(1920, 1080))
    bucket |= kResolution1080p;
  else
    return {};

  // Estimate the frame rate.
  if (average_duration >= base::TimeDelta::FromSecondsD(1. / 62) &&
      average_duration <= base::TimeDelta::FromSecondsD(1. / 58)) {
    bucket |= kFrameRate60;
  } else if (average_duration >= base::TimeDelta::FromSecondsD(1. / 31) &&
             average_duration <= base::TimeDelta::FromSecondsD(1. / 29)) {
    bucket |= kFrameRate30;
  } else {
    return {};
  }

  bucket |= is_fullscreen ? kFullScreenYes : kFullScreenNo;

  return bucket;
}

// static
const char* PowerStatusHelperImpl::BatteryDeltaHistogram() {
  return kBatteryDeltaHistogram;
}

// static
const char* PowerStatusHelperImpl::ElapsedTimeHistogram() {
  return kElapsedTimeHistogram;
}

void PowerStatusHelperImpl::SetIsPlaying(bool is_playing) {
  is_playing_ = is_playing;
  OnAnyStateChange();
}

void PowerStatusHelperImpl::SetMetadata(
    const media::PipelineMetadata& metadata) {
  has_video_ = metadata.has_video;
  codec_ = metadata.video_decoder_config.codec();
  profile_ = metadata.video_decoder_config.profile();
  natural_size_ = metadata.video_decoder_config.natural_size();
  OnAnyStateChange();
}

void PowerStatusHelperImpl::SetIsFullscreen(bool is_fullscreen) {
  is_fullscreen_ = is_fullscreen;
  OnAnyStateChange();
}

void PowerStatusHelperImpl::SetAverageDuration(
    base::TimeDelta average_duration) {
  average_duration_ = average_duration;
  OnAnyStateChange();
}

void PowerStatusHelperImpl::UpdatePowerExperimentState(bool state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  experiment_state_ = state;
  OnAnyStateChange();
}

void PowerStatusHelperImpl::OnAnyStateChange() {
  base::Optional<int> old_bucket = current_bucket_;
  current_bucket_.reset();

  // If we're the power experiment, then we might have a bucket.  Else, we
  // definitely don't.
  if (experiment_state_) {
    current_bucket_ =
        BucketFor(is_playing_, has_video_, codec_, profile_, natural_size_,
                  is_fullscreen_, average_duration_);
  }

  // If we're changing buckets, then request power updates with a new generation
  // id.  This lets us separate readings from the old bucket.
  if (current_bucket_ && (!old_bucket || *current_bucket_ != *old_bucket)) {
    // Also reset the baseline, in case we're changing buckets.  We don't want
    // to include any battery drain that should have been in the first bucket.
    StartMonitoring();
  } else if (old_bucket && !current_bucket_) {
    // We don't need power updates, but we had them before.
    StopMonitoring();
  }
}

void PowerStatusHelperImpl::OnBatteryStatus(
    device::mojom::BatteryStatusPtr battery_status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  QueryNextStatus();

  if (battery_status->charging) {
    // If we're charging, then wait until we stop.  Take a new baseline then.
    battery_level_baseline_.reset();
    return;
  }

  // Compute the amount of time since our last update.  Note that, if this is
  // the first status update since we (re)started monitoring, then the baseline
  // should be unset, so |elapsed| will be ignored.  That's good, since it could
  // be quite far in the past since we've had an update.
  const base::TimeTicks now = base::TimeTicks::Now();

  // Convert to floating point 0-100 from 0-1.
  const float current_level = battery_status->level * 100;

  // If we don't have a baseline, then use |current_level| and |now|.  In the
  // future, we might want to wait until the battery drain is reported twice,
  // since we don't know how much of a fractional percent remains in this
  // initial baseline.  For now, just ignore that.
  if (!battery_level_baseline_) {
    battery_level_baseline_ = current_level;
    last_update_ = now;
    return;
  }

  // Second or later update since we started monitoring / stopped charging.
  // Compute the battery used.  Note that positive numbers indicate that the
  // battery has gone down.
  const float delta = *battery_level_baseline_ - current_level;

  DCHECK(current_bucket_);
  DCHECK_GE(delta, 0.);

  // See if we can record some nonzero battery drain and elapsed time, when
  // converted to int.  We can only record ints in UMA.
  const int delta_int = static_cast<int>(delta);
  const base::TimeDelta elapsed = now - last_update_;
  const int elapsed_msec = elapsed.InMilliseconds();
  if (delta_int > 0 && elapsed_msec > 0) {
    // Record that we consumed |delta_int| battery percent in |elapsed_msec|.
    base::LinearHistogram::FactoryGet(
        BatteryDeltaHistogram(), kMinUmaValue, kMaxUmaValue, kNumUmaBuckets,
        base::HistogramBase::kUmaTargetedHistogramFlag)
        ->AddCount(*current_bucket_, delta_int);

    base::LinearHistogram::FactoryGet(
        ElapsedTimeHistogram(), kMinUmaValue, kMaxUmaValue, kNumUmaBuckets,
        base::HistogramBase::kUmaTargetedHistogramFlag)
        ->AddCount(*current_bucket_, elapsed_msec);

    // Update the baseline to |current_level|, but include any fractional
    // unrecorded amount so that we can record it later.
    battery_level_baseline_ = current_level + (delta - delta_int);
    // Don't bother remembering any fractional msec.
    last_update_ = now;
  }
}

void PowerStatusHelperImpl::StartMonitoring() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!battery_monitor_.is_bound()) {
    auto pending = create_battery_monitor_cb_.Run();
    if (!pending.is_valid())
      return;
    battery_monitor_.Bind(std::move(pending));

    // In case it's not available for some reason, do nothing.
    if (!battery_monitor_.is_bound())
      return;

    // Start querying for status as long as we're connected.
    QueryNextStatus();
  }

  // Any baseline that we had should be reset, since we're called to start or
  // restart monitoring when our bucket changes.
  battery_level_baseline_.reset();
}

void PowerStatusHelperImpl::StopMonitoring() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  battery_monitor_.reset();
}

void PowerStatusHelperImpl::QueryNextStatus() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(battery_monitor_.is_bound());

  // Remember that overlapping calls are not allowed by BatteryMonitor, and are
  // treated as a connection error.  Unretained since we own |battery_monitor_|.
  battery_monitor_->QueryNextStatus(base::BindOnce(
      &PowerStatusHelperImpl::OnBatteryStatus, base::Unretained(this)));
}

}  // namespace content
