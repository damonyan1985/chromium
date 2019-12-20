// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/command_buffer/service/shared_image_backing_egl_image.h"

#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/command_buffer/service/shared_image_representation.h"
#include "gpu/command_buffer/service/shared_image_representation_skia_gl.h"
#include "gpu/command_buffer/service/texture_definition.h"
#include "gpu/command_buffer/service/texture_manager.h"
#include "ui/gl/gl_fence_egl.h"
#include "ui/gl/gl_utils.h"
#include "ui/gl/scoped_binders.h"

namespace gpu {

// Implementation of SharedImageRepresentationGLTexture which uses GL texture
// which is an EGLImage sibling.
class SharedImageRepresentationEglImageGLTexture
    : public SharedImageRepresentationGLTexture {
 public:
  SharedImageRepresentationEglImageGLTexture(SharedImageManager* manager,
                                             SharedImageBacking* backing,
                                             MemoryTypeTracker* tracker,
                                             gles2::Texture* texture)
      : SharedImageRepresentationGLTexture(manager, backing, tracker),
        texture_(texture) {}

  ~SharedImageRepresentationEglImageGLTexture() override {
    EndAccess();

    if (texture_)
      texture_->RemoveLightweightRef(has_context());
  }

  bool BeginAccess(GLenum mode) override {
    if (mode == GL_SHARED_IMAGE_ACCESS_MODE_READ_CHROMIUM) {
      if (!egl_backing()->BeginRead(this))
        return false;
      mode_ = RepresentationAccessMode::kRead;
    } else if (mode == GL_SHARED_IMAGE_ACCESS_MODE_READWRITE_CHROMIUM) {
      if (!egl_backing()->BeginWrite())
        return false;
      mode_ = RepresentationAccessMode::kWrite;
    } else {
      NOTREACHED();
    }
    return true;
  }

  void EndAccess() override {
    if (mode_ == RepresentationAccessMode::kNone)
      return;

    std::unique_ptr<gl::GLFenceEGL> egl_fence = gl::GLFenceEGL::Create();
    // Pass this fence to its backing.
    if (mode_ == RepresentationAccessMode::kRead) {
      egl_backing()->EndRead(this, std::move(egl_fence));
    } else if (mode_ == RepresentationAccessMode::kWrite) {
      egl_backing()->EndWrite(std::move(egl_fence));

      if (texture_) {
        if (texture_->IsLevelCleared(texture_->target(), 0))
          backing()->SetCleared();
      }
    } else {
      NOTREACHED();
    }
    mode_ = RepresentationAccessMode::kNone;
  }

  gles2::Texture* GetTexture() override { return texture_; }

 private:
  SharedImageBackingEglImage* egl_backing() {
    return static_cast<SharedImageBackingEglImage*>(backing());
  }

  gles2::Texture* texture_;
  RepresentationAccessMode mode_ = RepresentationAccessMode::kNone;
  DISALLOW_COPY_AND_ASSIGN(SharedImageRepresentationEglImageGLTexture);
};

SharedImageBackingEglImage::SharedImageBackingEglImage(
    const Mailbox& mailbox,
    viz::ResourceFormat format,
    const gfx::Size& size,
    const gfx::ColorSpace& color_space,
    uint32_t usage,
    size_t estimated_size,
    GLuint gl_format,
    GLuint gl_type)
    : ClearTrackingSharedImageBacking(mailbox,
                                      format,
                                      size,
                                      color_space,
                                      usage,
                                      estimated_size,
                                      true /*is_thread_safe*/),
      gl_format_(gl_format),
      gl_type_(gl_type) {}

SharedImageBackingEglImage::~SharedImageBackingEglImage() {}

void SharedImageBackingEglImage::Update(
    std::unique_ptr<gfx::GpuFence> in_fence) {
  NOTREACHED();
}

bool SharedImageBackingEglImage::ProduceLegacyMailbox(
    MailboxManager* mailbox_manager) {
  // This backing doe not support legacy mailbox system.
  return false;
}

std::unique_ptr<SharedImageRepresentationGLTexture>
SharedImageBackingEglImage::ProduceGLTexture(SharedImageManager* manager,
                                             MemoryTypeTracker* tracker) {
  auto* texture = GenEGLImageSibling();
  if (!texture)
    return nullptr;
  return std::make_unique<SharedImageRepresentationEglImageGLTexture>(
      manager, this, tracker, texture);
}

std::unique_ptr<SharedImageRepresentationSkia>
SharedImageBackingEglImage::ProduceSkia(
    SharedImageManager* manager,
    MemoryTypeTracker* tracker,
    scoped_refptr<SharedContextState> context_state) {
  auto* texture = GenEGLImageSibling();
  if (!texture)
    return nullptr;

  auto gl_representation =
      std::make_unique<SharedImageRepresentationEglImageGLTexture>(
          manager, this, tracker, std::move(texture));
  return SharedImageRepresentationSkiaGL::Create(std::move(gl_representation),
                                                 std::move(context_state),
                                                 manager, this, tracker);
}

bool SharedImageBackingEglImage::BeginWrite() {
  AutoLock auto_lock(this);

  if (is_writing_ || !active_readers_.empty()) {
    DLOG(ERROR) << "BeginWrite should only be called when there are no other "
                   "readers or writers";
    return false;
  }
  is_writing_ = true;

  // When multiple threads wants to write to the same backing, writer needs to
  // wait on previous reads and writes to be finished.
  if (!read_fences_.empty()) {
    for (const auto& read_fence : read_fences_) {
      read_fence.second->ServerWait();
    }
    // Once all the read fences have been waited upon, its safe to clear all of
    // them. Note that when there is an active writer, no one can read and hence
    // can not update |read_fences_|.
    read_fences_.clear();
  }
  if (write_fence_)
    write_fence_->ServerWait();

  return true;
}

void SharedImageBackingEglImage::EndWrite(
    std::unique_ptr<gl::GLFenceEGL> end_write_fence) {
  AutoLock auto_lock(this);

  if (!is_writing_) {
    DLOG(ERROR) << "Attempt to end write to a SharedImageBacking without a "
                   "successful begin write";
    return;
  }

  is_writing_ = false;
  write_fence_ = std::move(end_write_fence);
}

bool SharedImageBackingEglImage::BeginRead(
    const SharedImageRepresentation* reader) {
  AutoLock auto_lock(this);

  if (is_writing_) {
    DLOG(ERROR) << "BeginRead should only be called when there are no writers";
    return false;
  }

  if (active_readers_.contains(reader)) {
    LOG(ERROR) << "BeginRead was called twice on the same representation";
    return false;
  }
  active_readers_.insert(reader);
  if (write_fence_)
    write_fence_->ServerWait();

  return true;
}

void SharedImageBackingEglImage::EndRead(
    const SharedImageRepresentation* reader,
    std::unique_ptr<gl::GLFenceEGL> end_read_fence) {
  AutoLock auto_lock(this);

  if (!active_readers_.contains(reader)) {
    DLOG(ERROR) << "Attempt to end read to a SharedImageBacking without a "
                   "successful begin read";
    return;
  }
  active_readers_.erase(reader);
  read_fences_[gl::g_current_gl_context] = std::move(end_read_fence);
}

gles2::Texture* SharedImageBackingEglImage::GenEGLImageSibling() {
  // Create a gles2::texture.
  GLenum target = GL_TEXTURE_2D;
  gl::GLApi* api = gl::g_current_gl_context;
  GLuint service_id = 0;
  api->glGenTexturesFn(1, &service_id);

  gl::ScopedTextureBinder texture_binder(target, service_id);

  api->glTexParameteriFn(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  api->glTexParameteriFn(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  api->glTexParameteriFn(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  api->glTexParameteriFn(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  auto* texture = new gles2::Texture(service_id);
  texture->SetLightweightRef();
  texture->SetTarget(target, 1 /*max_levels*/);
  texture->sampler_state_.min_filter = GL_LINEAR;
  texture->sampler_state_.mag_filter = GL_LINEAR;
  texture->sampler_state_.wrap_t = GL_CLAMP_TO_EDGE;
  texture->sampler_state_.wrap_s = GL_CLAMP_TO_EDGE;

  // If the backing is already cleared, no need to clear it again.
  gfx::Rect cleared_rect;
  if (IsCleared())
    cleared_rect = gfx::Rect(size());

  // Set the level info.
  texture->SetLevelInfo(target, 0, gl_format_, size().width(), size().height(),
                        1, 0, gl_format_, gl_type_, cleared_rect);

  // Note that we needed to use |bind_egl_image| flag and add some additional
  // logic to handle it in order to make the locks
  // more granular since BindToTexture() do not need to be behind the lock.
  // We don't need to bind the |egl_image_buffer_| first time when it's created.
  bool bind_egl_image = true;
  scoped_refptr<gles2::NativeImageBuffer> buffer;
  {
    AutoLock auto_lock(this);
    if (!egl_image_buffer_) {
      // Allocate memory for texture object if this is the first EGLImage
      // target/sibling. Memory for EGLImage will not be created if we don't
      // allocate memory for the texture object.
      api->glTexImage2DFn(target, 0, gl_format_, size().width(),
                          size().height(), 0, gl_format_, gl_type_, nullptr);

      // Use service id of the texture as a source to create the native buffer.
      egl_image_buffer_ = gles2::NativeImageBuffer::Create(service_id);
      if (!egl_image_buffer_) {
        texture->RemoveLightweightRef(have_context());
        return nullptr;
      }
      bind_egl_image = false;
    }
    buffer = egl_image_buffer_;
  }
  if (bind_egl_image) {
    // If we already have the |egl_image_buffer_|, just bind it to the new
    // texture to make it an EGLImage sibling.
    buffer->BindToTexture(target);
  }

  texture->SetImmutable(true /*immutable*/, false /*immutable_storage*/);
  return texture;
}

}  // namespace gpu
