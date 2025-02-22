// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/video/video_encoder_info.h"

namespace media {

ScalingSettings::ScalingSettings() = default;
ScalingSettings::ScalingSettings(int min_qp, int max_qp)
    : min_qp(min_qp), max_qp(max_qp) {}
ScalingSettings::~ScalingSettings() = default;

ResolutionBitrateLimit::ResolutionBitrateLimit() = default;
ResolutionBitrateLimit::ResolutionBitrateLimit(const ResolutionBitrateLimit&) =
    default;
ResolutionBitrateLimit::ResolutionBitrateLimit(const gfx::Size& frame_size,
                                               int min_start_bitrate_bps,
                                               int min_bitrate_bps,
                                               int max_bitrate_bps)
    : frame_size(frame_size),
      min_start_bitrate_bps(min_start_bitrate_bps),
      min_bitrate_bps(min_bitrate_bps),
      max_bitrate_bps(max_bitrate_bps) {}
ResolutionBitrateLimit::~ResolutionBitrateLimit() = default;

VideoEncoderInfo::VideoEncoderInfo() = default;
VideoEncoderInfo::VideoEncoderInfo(const VideoEncoderInfo&) = default;
VideoEncoderInfo::~VideoEncoderInfo() = default;

}  // namespace media
