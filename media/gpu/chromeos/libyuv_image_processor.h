// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_GPU_CHROMEOS_LIBYUV_IMAGE_PROCESSOR_H_
#define MEDIA_GPU_CHROMEOS_LIBYUV_IMAGE_PROCESSOR_H_

#include <memory>
#include <vector>

#include "base/macros.h"
#include "media/gpu/chromeos/image_processor_backend.h"
#include "media/gpu/media_gpu_export.h"
#include "ui/gfx/geometry/rect.h"

namespace media {

class VideoFrameMapper;

// A software image processor which uses libyuv to perform format conversion.
// It expects input VideoFrame is mapped into CPU space, and output VideoFrame
// is allocated in user space.
class MEDIA_GPU_EXPORT LibYUVImageProcessor : public ImageProcessorBackend {
 public:
  // Factory method to create LibYUVImageProcessor to convert video format
  // specified in input_config and output_config. Provided |error_cb| will be
  // posted to the same thread Create() is called if an error occurs after
  // initialization.
  // Returns nullptr if it fails to create LibYUVImageProcessor.
  static std::unique_ptr<ImageProcessorBackend> Create(
      const PortConfig& input_config,
      const PortConfig& output_config,
      const std::vector<OutputMode>& preferred_output_modes,
      ErrorCB error_cb,
      scoped_refptr<base::SequencedTaskRunner> backend_task_runner);

  // ImageProcessorBackend override
  void Process(scoped_refptr<VideoFrame> input_frame,
               scoped_refptr<VideoFrame> output_frame,
               FrameReadyCB cb) override;

 private:
  LibYUVImageProcessor(
      std::unique_ptr<VideoFrameMapper> video_frame_mapper,
      scoped_refptr<VideoFrame> intermediate_frame,
      const PortConfig& input_config,
      const PortConfig& output_config,
      OutputMode output_mode,
      ErrorCB error_cb,
      scoped_refptr<base::SequencedTaskRunner> backend_task_runner);
  ~LibYUVImageProcessor() override;

  void NotifyError();

  // Execute Libyuv function for the conversion from |input| to |output|.
  int DoConversion(const VideoFrame* const input, VideoFrame* const output);

  const gfx::Rect input_visible_rect_;
  const gfx::Rect output_visible_rect_;

  std::unique_ptr<VideoFrameMapper> video_frame_mapper_;

  // A VideoFrame for intermediate format conversion when there is no direct
  // conversion method in libyuv, e.g., RGBA -> I420 (pivot) -> NV12.
  scoped_refptr<VideoFrame> intermediate_frame_;

  DISALLOW_COPY_AND_ASSIGN(LibYUVImageProcessor);
};

}  // namespace media

#endif  // MEDIA_GPU_CHROMEOS_LIBYUV_IMAGE_PROCESSOR_H_
