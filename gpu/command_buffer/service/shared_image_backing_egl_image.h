// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GPU_COMMAND_BUFFER_SERVICE_SHARED_IMAGE_BACKING_EGL_IMAGE_H_
#define GPU_COMMAND_BUFFER_SERVICE_SHARED_IMAGE_BACKING_EGL_IMAGE_H_

#include "base/memory/scoped_refptr.h"
#include "components/viz/common/resources/resource_format.h"
#include "gpu/command_buffer/service/shared_image_backing.h"
#include "ui/gfx/buffer_types.h"
#include "ui/gl/gl_bindings.h"

namespace gl {
class GLFenceEGL;
}  // namespace gl

namespace gpu {
class SharedImageRepresentationGLTexture;
class SharedImageRepresentationSkia;
struct Mailbox;

namespace gles2 {
class NativeImageBuffer;
class Texture;
}  // namespace gles2

// Implementation of SharedImageBacking that is used to create EGLImage targets
// from the same EGLImage object. Hence all the representations created from
// this backing uses EGL Image siblings. This backing is thread safe across
// different threads running different GL contexts not part of same shared
// group. This is achieved by using locks and fences for proper synchronization.
class SharedImageBackingEglImage : public ClearTrackingSharedImageBacking {
 public:
  SharedImageBackingEglImage(const Mailbox& mailbox,
                             viz::ResourceFormat format,
                             const gfx::Size& size,
                             const gfx::ColorSpace& color_space,
                             uint32_t usage,
                             size_t estimated_size,
                             GLuint gl_format,
                             GLuint gl_type);

  ~SharedImageBackingEglImage() override;

  void Update(std::unique_ptr<gfx::GpuFence> in_fence) override;
  bool ProduceLegacyMailbox(MailboxManager* mailbox_manager) override;

  bool BeginWrite();
  void EndWrite(std::unique_ptr<gl::GLFenceEGL> end_write_fence);
  bool BeginRead(const SharedImageRepresentation* reader);
  void EndRead(const SharedImageRepresentation* reader,
               std::unique_ptr<gl::GLFenceEGL> end_read_fence);

 protected:
  std::unique_ptr<SharedImageRepresentationGLTexture> ProduceGLTexture(
      SharedImageManager* manager,
      MemoryTypeTracker* tracker) override;

  std::unique_ptr<SharedImageRepresentationSkia> ProduceSkia(
      SharedImageManager* manager,
      MemoryTypeTracker* tracker,
      scoped_refptr<SharedContextState> context_state) override;

 private:
  friend class SharedImageRepresentationEglImageGLTexture;

  // Use to create EGLImage texture target from the same EGLImage object.
  gles2::Texture* GenEGLImageSibling();

  const GLuint gl_format_;
  const GLuint gl_type_;

  // This class encapsulates the EGLImage object for android.
  scoped_refptr<gles2::NativeImageBuffer> egl_image_buffer_ GUARDED_BY(lock_);

  // All reads and writes must wait for exiting writes to complete.
  std::unique_ptr<gl::GLFenceEGL> write_fence_ GUARDED_BY(lock_);
  bool is_writing_ GUARDED_BY(lock_) = false;

  // All writes must wait for existing reads to complete. For a given GL
  // context, we only need to keep the most recent fence. Waiting on the most
  // recent read fence is enough to make sure all past read fences have been
  // signalled.
  base::flat_map<gl::GLApi*, std::unique_ptr<gl::GLFenceEGL>> read_fences_
      GUARDED_BY(lock_);
  base::flat_set<const SharedImageRepresentation*> active_readers_
      GUARDED_BY(lock_);
  DISALLOW_COPY_AND_ASSIGN(SharedImageBackingEglImage);
};

}  // namespace gpu

#endif  // GPU_COMMAND_BUFFER_SERVICE_SHARED_IMAGE_BACKING_EGL_IMAGE_H_
