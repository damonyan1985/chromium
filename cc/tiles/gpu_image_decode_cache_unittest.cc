// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/tiles/gpu_image_decode_cache.h"

#include <memory>
#include <vector>

#include "base/feature_list.h"
#include "base/test/scoped_feature_list.h"
#include "cc/paint/draw_image.h"
#include "cc/paint/image_transfer_cache_entry.h"
#include "cc/paint/paint_image_builder.h"
#include "cc/test/fake_paint_image_generator.h"
#include "cc/test/skia_common.h"
#include "cc/test/test_tile_task_runner.h"
#include "cc/test/transfer_cache_test_helper.h"
#include "components/viz/test/test_context_provider.h"
#include "components/viz/test/test_gles2_interface.h"
#include "gpu/command_buffer/client/raster_implementation_gles.h"
#include "gpu/command_buffer/common/command_buffer_id.h"
#include "gpu/command_buffer/common/constants.h"
#include "gpu/config/gpu_finch_features.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkImageGenerator.h"
#include "third_party/skia/include/core/SkRefCnt.h"
#include "third_party/skia/include/gpu/GrBackendSurface.h"
#include "third_party/skia/include/gpu/GrContext.h"

using testing::_;
using testing::StrictMock;

namespace cc {
namespace {

class FakeDiscardableManager {
 public:
  void SetGLES2Interface(viz::TestGLES2Interface* gl) { gl_ = gl; }
  void Initialize(GLuint texture_id) {
    EXPECT_EQ(textures_.end(), textures_.find(texture_id));
    textures_[texture_id] = kHandleLockedStart;
    live_textures_count_++;
  }
  void Unlock(GLuint texture_id) {
    EXPECT_NE(textures_.end(), textures_.find(texture_id));
    ExpectLocked(texture_id);
    textures_[texture_id]--;
  }
  bool Lock(GLuint texture_id) {
    EnforceLimit();

    EXPECT_NE(textures_.end(), textures_.find(texture_id));
    if (textures_[texture_id] >= kHandleUnlocked) {
      textures_[texture_id]++;
      return true;
    }
    return false;
  }

  void DeleteTexture(GLuint texture_id) {
    if (textures_.end() == textures_.find(texture_id))
      return;

    ExpectLocked(texture_id);
    textures_[texture_id] = kHandleDeleted;
    live_textures_count_--;
  }

  void set_cached_textures_limit(size_t limit) {
    cached_textures_limit_ = limit;
  }

  void ExpectLocked(GLuint texture_id) {
    EXPECT_TRUE(textures_.end() != textures_.find(texture_id));

    // Any value > kHandleLockedStart represents a locked texture. As we
    // increment this value with each lock, we need the entire range and can't
    // add additional values > kHandleLockedStart in the future.
    EXPECT_GE(textures_[texture_id], kHandleLockedStart);
    EXPECT_LE(textures_[texture_id], kHandleLockedEnd);
  }

 private:
  void EnforceLimit() {
    for (auto it = textures_.begin(); it != textures_.end(); ++it) {
      if (live_textures_count_ <= cached_textures_limit_)
        return;
      if (it->second != kHandleUnlocked)
        continue;

      it->second = kHandleDeleted;
      gl_->TestGLES2Interface::DeleteTextures(1, &it->first);
      live_textures_count_--;
    }
  }

  const int32_t kHandleDeleted = 0;
  const int32_t kHandleUnlocked = 1;
  const int32_t kHandleLockedStart = 2;
  const int32_t kHandleLockedEnd = std::numeric_limits<int32_t>::max();

  std::map<GLuint, int32_t> textures_;
  size_t live_textures_count_ = 0;
  size_t cached_textures_limit_ = std::numeric_limits<size_t>::max();
  viz::TestGLES2Interface* gl_ = nullptr;
};

class FakeGPUImageDecodeTestGLES2Interface : public viz::TestGLES2Interface,
                                             public viz::TestContextSupport {
 public:
  explicit FakeGPUImageDecodeTestGLES2Interface(
      FakeDiscardableManager* discardable_manager,
      TransferCacheTestHelper* transfer_cache_helper,
      bool advertise_accelerated_decoding)
      : extension_string_(
            "GL_EXT_texture_format_BGRA8888 GL_OES_rgb8_rgba8 "
            "GL_OES_texture_npot GL_EXT_texture_rg "
            "GL_OES_texture_half_float GL_OES_texture_half_float_linear"),
        discardable_manager_(discardable_manager),
        transfer_cache_helper_(transfer_cache_helper),
        advertise_accelerated_decoding_(advertise_accelerated_decoding) {}

  ~FakeGPUImageDecodeTestGLES2Interface() override {
    // All textures / framebuffers / renderbuffers should be cleaned up.
    EXPECT_EQ(0u, NumTextures());
    EXPECT_EQ(0u, NumFramebuffers());
    EXPECT_EQ(0u, NumRenderbuffers());
  }

  void InitializeDiscardableTextureCHROMIUM(GLuint texture_id) override {
    discardable_manager_->Initialize(texture_id);
  }
  void UnlockDiscardableTextureCHROMIUM(GLuint texture_id) override {
    discardable_manager_->Unlock(texture_id);
  }
  bool LockDiscardableTextureCHROMIUM(GLuint texture_id) override {
    return discardable_manager_->Lock(texture_id);
  }

  bool ThreadSafeShallowLockDiscardableTexture(uint32_t texture_id) override {
    return discardable_manager_->Lock(texture_id);
  }
  void CompleteLockDiscardableTexureOnContextThread(
      uint32_t texture_id) override {}

  void* MapTransferCacheEntry(uint32_t serialized_size) override {
    mapped_entry_size_ = serialized_size;
    mapped_entry_.reset(new uint8_t[serialized_size]);
    return mapped_entry_.get();
  }

  void UnmapAndCreateTransferCacheEntry(uint32_t type, uint32_t id) override {
    transfer_cache_helper_->CreateEntryDirect(
        MakeEntryKey(type, id),
        base::make_span(mapped_entry_.get(), mapped_entry_size_));
    mapped_entry_ = nullptr;
    mapped_entry_size_ = 0;
  }

  bool ThreadsafeLockTransferCacheEntry(uint32_t type, uint32_t id) override {
    return transfer_cache_helper_->LockEntryDirect(MakeEntryKey(type, id));
  }
  void UnlockTransferCacheEntries(
      const std::vector<std::pair<uint32_t, uint32_t>>& entries) override {
    std::vector<std::pair<TransferCacheEntryType, uint32_t>> keys;
    keys.reserve(entries.size());
    for (const auto& e : entries)
      keys.emplace_back(MakeEntryKey(e.first, e.second));
    transfer_cache_helper_->UnlockEntriesDirect(keys);
  }
  void DeleteTransferCacheEntry(uint32_t type, uint32_t id) override {
    transfer_cache_helper_->DeleteEntryDirect(MakeEntryKey(type, id));
  }

  bool IsJpegDecodeAccelerationSupported() const override {
    return advertise_accelerated_decoding_;
  }

  bool IsWebPDecodeAccelerationSupported() const override {
    return advertise_accelerated_decoding_;
  }

  bool CanDecodeWithHardwareAcceleration(
      const ImageHeaderMetadata* image_metadata) const override {
    // Only advertise hardware accelerated decoding for the current use cases
    // (JPEG and WebP).
    if (image_metadata && (image_metadata->image_type == ImageType::kJPEG ||
                           image_metadata->image_type == ImageType::kWEBP)) {
      return advertise_accelerated_decoding_;
    }
    return false;
  }

  std::pair<TransferCacheEntryType, uint32_t> MakeEntryKey(uint32_t type,
                                                           uint32_t id) {
    DCHECK_LE(type, static_cast<uint32_t>(TransferCacheEntryType::kLast));
    return std::make_pair(static_cast<TransferCacheEntryType>(type), id);
  }

  // viz::TestGLES2Interface:
  const GLubyte* GetString(GLenum name) override {
    switch (name) {
      case GL_EXTENSIONS:
        return reinterpret_cast<const GLubyte*>(extension_string_.c_str());
      case GL_VERSION:
        return reinterpret_cast<const GLubyte*>("4.0 Null GL");
      case GL_SHADING_LANGUAGE_VERSION:
        return reinterpret_cast<const GLubyte*>("4.20.8 Null GLSL");
      case GL_VENDOR:
        return reinterpret_cast<const GLubyte*>("Null Vendor");
      case GL_RENDERER:
        return reinterpret_cast<const GLubyte*>("The Null (Non-)Renderer");
    }
    return nullptr;
  }
  void GetIntegerv(GLenum name, GLint* params) override {
    switch (name) {
      case GL_MAX_TEXTURE_IMAGE_UNITS:
        *params = 8;
        return;
      case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
        *params = 8;
        return;
      case GL_MAX_RENDERBUFFER_SIZE:
        *params = 2048;
        return;
      case GL_MAX_VERTEX_ATTRIBS:
        *params = 8;
        return;
      default:
        break;
    }
    TestGLES2Interface::GetIntegerv(name, params);
  }
  void DeleteTextures(GLsizei n, const GLuint* textures) override {
    for (GLsizei i = 0; i < n; i++) {
      discardable_manager_->DeleteTexture(textures[i]);
    }
    TestGLES2Interface::DeleteTextures(n, textures);
  }

 private:
  const std::string extension_string_;
  FakeDiscardableManager* discardable_manager_;
  TransferCacheTestHelper* transfer_cache_helper_;
  bool advertise_accelerated_decoding_ = false;
  size_t mapped_entry_size_ = 0;
  std::unique_ptr<uint8_t[]> mapped_entry_;
};

class MockRasterImplementation : public gpu::raster::RasterImplementationGLES {
 public:
  explicit MockRasterImplementation(gpu::gles2::GLES2Interface* gl)
      : RasterImplementationGLES(gl) {}
  ~MockRasterImplementation() override = default;

  gpu::SyncToken ScheduleImageDecode(base::span<const uint8_t> encoded_data,
                                     const gfx::Size& output_size,
                                     uint32_t transfer_cache_entry_id,
                                     const gfx::ColorSpace& target_color_space,
                                     bool needs_mips) override {
    DoScheduleImageDecode(output_size, transfer_cache_entry_id,
                          target_color_space, needs_mips);
    if (!next_accelerated_decode_fails_) {
      return gpu::SyncToken(gpu::CommandBufferNamespace::GPU_IO,
                            gpu::CommandBufferId::FromUnsafeValue(1u),
                            next_release_count_++);
    }
    return gpu::SyncToken();
  }

  void SetAcceleratedDecodingFailed() { next_accelerated_decode_fails_ = true; }

  MOCK_METHOD4(DoScheduleImageDecode,
               void(const gfx::Size& /* output_size */,
                    uint32_t /* transfer_cache_entry_id */,
                    const gfx::ColorSpace& /* target_color_space */,
                    bool /* needs_mips */));

 private:
  bool next_accelerated_decode_fails_ = false;
  uint64_t next_release_count_ = 1u;
};

class GPUImageDecodeTestMockContextProvider : public viz::TestContextProvider {
 public:
  static scoped_refptr<GPUImageDecodeTestMockContextProvider> Create(
      FakeDiscardableManager* discardable_manager,
      TransferCacheTestHelper* transfer_cache_helper,
      bool advertise_accelerated_decoding) {
    auto support = std::make_unique<FakeGPUImageDecodeTestGLES2Interface>(
        discardable_manager, transfer_cache_helper,
        advertise_accelerated_decoding);
    auto gl = std::make_unique<FakeGPUImageDecodeTestGLES2Interface>(
        discardable_manager, transfer_cache_helper,
        false /* advertise_accelerated_decoding */);
    auto raster =
        std::make_unique<StrictMock<MockRasterImplementation>>(gl.get());
    return new GPUImageDecodeTestMockContextProvider(
        std::move(support), std::move(gl), std::move(raster));
  }

 private:
  ~GPUImageDecodeTestMockContextProvider() override = default;
  GPUImageDecodeTestMockContextProvider(
      std::unique_ptr<viz::TestContextSupport> support,
      std::unique_ptr<viz::TestGLES2Interface> gl,
      std::unique_ptr<gpu::raster::RasterInterface> raster)
      : TestContextProvider(std::move(support),
                            std::move(gl),
                            std::move(raster),
                            true) {}
};

SkMatrix CreateMatrix(const SkSize& scale) {
  SkMatrix matrix;
  matrix.setScale(scale.width(), scale.height());
  return matrix;
}

#define EXPECT_TRUE_IF_NOT_USING_TRANSFER_CACHE(condition) \
  if (!use_transfer_cache_)                                \
    EXPECT_TRUE(condition);

#define EXPECT_FALSE_IF_NOT_USING_TRANSFER_CACHE(condition) \
  if (!use_transfer_cache_)                                 \
    EXPECT_FALSE(condition);

size_t kGpuMemoryLimitBytes = 96 * 1024 * 1024;

class GpuImageDecodeCacheTest
    : public ::testing::TestWithParam<
          std::tuple<SkColorType,
                     bool /* use_transfer_cache */,
                     bool /* do_yuv_decode */,
                     bool /* allow_accelerated_jpeg_decoding */,
                     bool /* allow_accelerated_webp_decoding */,
                     bool /* advertise_accelerated_decoding */>> {
 public:
  void SetUp() override {
    std::vector<base::Feature> enabled_features;
    allow_accelerated_jpeg_decoding_ = std::get<3>(GetParam());
    if (allow_accelerated_jpeg_decoding_)
      enabled_features.push_back(features::kVaapiJpegImageDecodeAcceleration);
    allow_accelerated_webp_decoding_ = std::get<4>(GetParam());
    if (allow_accelerated_webp_decoding_)
      enabled_features.push_back(features::kVaapiWebPImageDecodeAcceleration);
    feature_list_.InitWithFeatures(enabled_features,
                                   {} /* disabled_features */);
    advertise_accelerated_decoding_ = std::get<5>(GetParam());
    context_provider_ = GPUImageDecodeTestMockContextProvider::Create(
        &discardable_manager_, &transfer_cache_helper_,
        advertise_accelerated_decoding_);
    discardable_manager_.SetGLES2Interface(
        context_provider_->UnboundTestContextGL());
    context_provider_->BindToCurrentThread();
    {
      viz::RasterContextProvider::ScopedRasterContextLock context_lock(
          context_provider_.get());
      transfer_cache_helper_.SetGrContext(context_provider_->GrContext());
      max_texture_size_ =
          context_provider_->ContextCapabilities().max_texture_size;
    }
    color_type_ = std::get<0>(GetParam());
    use_transfer_cache_ = std::get<1>(GetParam());
    do_yuv_decode_ = std::get<2>(GetParam());
  }

  std::unique_ptr<GpuImageDecodeCache> CreateCache(
      size_t memory_limit_bytes = kGpuMemoryLimitBytes) {
    return std::make_unique<GpuImageDecodeCache>(
        context_provider_.get(), use_transfer_cache_, color_type_,
        memory_limit_bytes, max_texture_size_,
        PaintImage::kDefaultGeneratorClientId);
  }

  // Returns dimensions for an image that will not fit in GPU memory and hence
  // triggers software fallback.
  gfx::Size GetLargeImageSize() const {
    return gfx::Size(1, max_texture_size_ + 1);
  }

  // Returns dimensions for an image that will fit in GPU memory.
  gfx::Size GetNormalImageSize() const {
    int dimension = std::min(100, max_texture_size_ - 1);
    return gfx::Size(dimension, dimension);
  }

  PaintImage CreatePaintImageInternal(
      const gfx::Size& size,
      sk_sp<SkColorSpace> color_space = nullptr,
      PaintImage::Id id = PaintImage::kInvalidId) {
    const bool allocate_encoded_memory = true;
    return CreateDiscardablePaintImage(size, color_space,
                                       allocate_encoded_memory, id, color_type_,
                                       do_yuv_decode_);
  }

  PaintImage CreateLargePaintImageForSoftwareFallback() {
    return CreateLargePaintImageForSoftwareFallback(GetLargeImageSize());
  }

  // Create an image that's too large to upload and will trigger falling back to
  // software rendering and decoded data storage.
  PaintImage CreateLargePaintImageForSoftwareFallback(
      const gfx::Size test_image_size) {
    CHECK(test_image_size.width() > max_texture_size_ ||
          test_image_size.height() > max_texture_size_);
    SkImageInfo info = SkImageInfo::Make(
        test_image_size.width(), test_image_size.height(), color_type_,
        kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    sk_sp<FakePaintImageGenerator> generator;
    if (do_yuv_decode_) {
      generator = sk_make_sp<FakePaintImageGenerator>(
          info, GetYUV420SizeInfo(test_image_size));
      generator->SetExpectFallbackToRGB();
    } else {
      generator = sk_make_sp<FakePaintImageGenerator>(info);
    }
    PaintImage image = PaintImageBuilder::WithDefault()
                           .set_id(PaintImage::GetNextId())
                           .set_paint_image_generator(generator)
                           .TakePaintImage();
    return image;
  }

  PaintImage CreateBitmapImageInternal(const gfx::Size& size) {
    DCHECK(!do_yuv_decode_);
    return CreateBitmapImage(size, color_type_);
  }

  gfx::ColorSpace DefaultColorSpace() {
    if (color_type_ != kRGBA_F16_SkColorType)
      return gfx::ColorSpace::CreateSRGB();
    return gfx::ColorSpace(gfx::ColorSpace::PrimaryID::SMPTEST432_1,  // P3
                           gfx::ColorSpace::TransferID::LINEAR);
  }

  DrawImage CreateDrawImageInternal(
      const PaintImage& paint_image,
      const SkMatrix& matrix = SkMatrix::I(),
      gfx::ColorSpace* color_space = nullptr,
      SkFilterQuality filter_quality = kMedium_SkFilterQuality,
      SkIRect* src_rect = nullptr,
      size_t frame_index = PaintImage::kDefaultFrameIndex) {
    SkIRect src_rectangle;
    gfx::ColorSpace cs;
    if (!src_rect) {
      src_rectangle =
          SkIRect::MakeWH(paint_image.width(), paint_image.height());
      src_rect = &src_rectangle;
    }
    if (!color_space) {
      cs = DefaultColorSpace();
      color_space = &cs;
    }
    return DrawImage(paint_image, *src_rect, filter_quality, matrix,
                     frame_index, *color_space);
  }

  GPUImageDecodeTestMockContextProvider* context_provider() {
    return context_provider_.get();
  }

  size_t GetBytesNeededForSingleImage(gfx::Size image_dimensions) {
    // TODO(crbug.com/915972): Assumes YUV 420.
    if (do_yuv_decode_) {
      return GetYUV420SizeInfo(image_dimensions).computeTotalBytes();
    }
    const size_t test_image_area_bytes =
        base::checked_cast<size_t>(image_dimensions.GetArea());
    base::CheckedNumeric<size_t> bytes_for_rgb_image_safe(
        test_image_area_bytes);
    bytes_for_rgb_image_safe *= SkColorTypeBytesPerPixel(color_type_);
    return bytes_for_rgb_image_safe.ValueOrDie();
  }

  void SetCachedTexturesLimit(size_t limit) {
    discardable_manager_.set_cached_textures_limit(limit);
    transfer_cache_helper_.SetCachedItemsLimit(limit);
  }

  // If this is an image-backed DecodedDrawImage, does nothing. Otherwise this
  // retreives the image from the transfer cache and builds a new
  // DecodedDrawImage.
  DecodedDrawImage EnsureImageBacked(DecodedDrawImage&& draw_image) {
    if (draw_image.transfer_cache_entry_id()) {
      EXPECT_TRUE(use_transfer_cache_);
      auto* image_entry =
          transfer_cache_helper_.GetEntryAs<ServiceImageTransferCacheEntry>(
              *draw_image.transfer_cache_entry_id());
      if (draw_image.transfer_cache_entry_needs_mips())
        image_entry->EnsureMips();
      DecodedDrawImage new_draw_image(
          image_entry->image(), draw_image.src_rect_offset(),
          draw_image.scale_adjustment(), draw_image.filter_quality(),
          draw_image.is_budgeted());
      return new_draw_image;
    }

    return std::move(draw_image);
  }

  sk_sp<SkImage> GetLastTransferredImage() {
    auto& key = transfer_cache_helper_.GetLastAddedEntry();
    ServiceTransferCacheEntry* entry =
        transfer_cache_helper_.GetEntryInternal(key.first, key.second);
    if (!entry)
      return nullptr;
    CHECK_EQ(TransferCacheEntryType::kImage, entry->Type());
    return static_cast<ServiceImageTransferCacheEntry*>(entry)->image();
  }

  void CompareAllPlanesToMippedVersions(
      GpuImageDecodeCache* cache,
      const DrawImage& draw_image,
      const base::Optional<uint32_t> transfer_cache_id,
      bool should_have_mips) {
    for (size_t i = 0; i < SkYUVASizeInfo::kMaxCount; ++i) {
      // TODO(crbug.com/910276): Skip alpha plane until supported in cache.
      if (i != SkYUVAIndex::kA_Index) {
        sk_sp<SkImage> original_uploaded_plane;
        if (use_transfer_cache_) {
          DCHECK(transfer_cache_id.has_value());
          const uint32_t id = transfer_cache_id.value();
          auto* image_entry =
              transfer_cache_helper_.GetEntryAs<ServiceImageTransferCacheEntry>(
                  id);
          original_uploaded_plane = image_entry->GetPlaneImage(i);
        } else {
          original_uploaded_plane =
              cache->GetUploadedPlaneForTesting(draw_image, i);
        }

        ASSERT_TRUE(original_uploaded_plane);
        auto plane_with_mips = original_uploaded_plane->makeTextureImage(
            context_provider()->GrContext(), GrMipMapped::kYes);
        ASSERT_TRUE(plane_with_mips);
        EXPECT_EQ(should_have_mips, original_uploaded_plane == plane_with_mips);
      }
    }
  }

  void VerifyUploadedPlaneSizes(
      GpuImageDecodeCache* cache,
      const DrawImage& draw_image,
      const base::Optional<uint32_t> transfer_cache_id,
      const SkISize plane_sizes[SkYUVASizeInfo::kMaxCount]) {
    for (size_t i = 0; i < SkYUVASizeInfo::kMaxCount; ++i) {
      // TODO(crbug.com/910276): Skip alpha plane until supported in cache.
      if (i != SkYUVAIndex::kA_Index) {
        sk_sp<SkImage> uploaded_plane;
        if (use_transfer_cache_) {
          DCHECK(transfer_cache_id.has_value());
          const uint32_t id = transfer_cache_id.value();
          auto* image_entry =
              transfer_cache_helper_.GetEntryAs<ServiceImageTransferCacheEntry>(
                  id);
          uploaded_plane = image_entry->GetPlaneImage(i);
        } else {
          uploaded_plane = cache->GetUploadedPlaneForTesting(draw_image, i);
        }
        ASSERT_TRUE(uploaded_plane);
        EXPECT_EQ(plane_sizes[i], uploaded_plane->dimensions());
      }
    }
  }

 protected:
  base::test::ScopedFeatureList feature_list_;

  // The order of these members is important because |context_provider_| depends
  // on |discardable_manager_| and |transfer_cache_helper_|.
  FakeDiscardableManager discardable_manager_;
  TransferCacheTestHelper transfer_cache_helper_;
  scoped_refptr<GPUImageDecodeTestMockContextProvider> context_provider_;

  bool use_transfer_cache_;
  SkColorType color_type_;
  bool do_yuv_decode_;
  bool allow_accelerated_jpeg_decoding_;
  bool allow_accelerated_webp_decoding_;
  bool advertise_accelerated_decoding_;
  int max_texture_size_ = 0;
};

TEST_P(GpuImageDecodeCacheTest, GetTaskForImageSameImage) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());

  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(1.5f, 1.5f)));
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  DrawImage another_draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(1.5f, 1.5f)));
  ImageDecodeCache::TaskResult another_result = cache->GetTaskForImageAndRef(
      another_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(another_result.need_unref);
  EXPECT_TRUE(result.task.get() == another_result.task.get());

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  cache->UnrefImage(draw_image);
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetTaskForImageSmallerScale) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(1.5f, 1.5f)));
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  // |result| is an upload task which depends on a decode task.
  EXPECT_EQ(result.task->dependencies().size(), 1u);
  EXPECT_TRUE(result.task->dependencies()[0]);

  DrawImage another_draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult another_result = cache->GetTaskForImageAndRef(
      another_draw_image, ImageDecodeCache::TracingInfo());

  // |another_draw_image| represents previous image but at a different scale.
  // It still has one dependency (decoding), and its upload task is equivalent
  // to the larger decoded textures being uploaded.
  EXPECT_EQ(another_result.task->dependencies().size(), 1u);
  EXPECT_TRUE(another_result.task->dependencies()[0]);

  EXPECT_TRUE(another_result.need_unref);
  EXPECT_TRUE(result.task.get() == another_result.task.get());

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  cache->UnrefImage(draw_image);
  cache->UnrefImage(another_draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetTaskForImageLowerQuality) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  SkMatrix matrix = CreateMatrix(SkSize::Make(0.4f, 0.4f));
  DrawImage draw_image = CreateDrawImageInternal(image, matrix);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  DrawImage another_draw_image = CreateDrawImageInternal(
      image, matrix, nullptr /* color_space */, kLow_SkFilterQuality);
  ImageDecodeCache::TaskResult another_result = cache->GetTaskForImageAndRef(
      another_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(another_result.need_unref);
  EXPECT_TRUE(result.task.get() == another_result.task.get());

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  cache->UnrefImage(draw_image);
  cache->UnrefImage(another_draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetTaskForImageDifferentImage) {
  auto cache = CreateCache();

  PaintImage first_image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage first_draw_image = CreateDrawImageInternal(
      first_image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult first_result = cache->GetTaskForImageAndRef(
      first_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(first_result.need_unref);
  EXPECT_TRUE(first_result.task);

  PaintImage second_image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage second_draw_image = CreateDrawImageInternal(
      second_image, CreateMatrix(SkSize::Make(0.25f, 0.25f)));
  ImageDecodeCache::TaskResult second_result = cache->GetTaskForImageAndRef(
      second_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(second_result.need_unref);
  EXPECT_TRUE(second_result.task);
  EXPECT_TRUE(first_result.task.get() != second_result.task.get());

  TestTileTaskRunner::ProcessTask(first_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(first_result.task.get());
  TestTileTaskRunner::ProcessTask(second_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(second_result.task.get());

  cache->UnrefImage(first_draw_image);
  cache->UnrefImage(second_draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetTaskForImageLargerScale) {
  auto cache = CreateCache();

  PaintImage first_image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage first_draw_image = CreateDrawImageInternal(
      first_image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult first_result = cache->GetTaskForImageAndRef(
      first_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(first_result.need_unref);
  EXPECT_TRUE(first_result.task);

  TestTileTaskRunner::ProcessTask(first_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(first_result.task.get());

  cache->UnrefImage(first_draw_image);

  DrawImage second_draw_image = CreateDrawImageInternal(first_image);
  ImageDecodeCache::TaskResult second_result = cache->GetTaskForImageAndRef(
      second_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(second_result.need_unref);
  EXPECT_TRUE(second_result.task);
  EXPECT_TRUE(first_result.task.get() != second_result.task.get());

  DrawImage third_draw_image = CreateDrawImageInternal(
      first_image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult third_result = cache->GetTaskForImageAndRef(
      third_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(third_result.need_unref);
  EXPECT_TRUE(third_result.task.get() == second_result.task.get());

  TestTileTaskRunner::ProcessTask(second_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(second_result.task.get());

  cache->UnrefImage(second_draw_image);
  cache->UnrefImage(third_draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetTaskForImageLargerScaleNoReuse) {
  auto cache = CreateCache();
  PaintImage first_image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage first_draw_image = CreateDrawImageInternal(
      first_image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult first_result = cache->GetTaskForImageAndRef(
      first_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(first_result.need_unref);
  EXPECT_TRUE(first_result.task);

  DrawImage second_draw_image = CreateDrawImageInternal(first_image);
  ImageDecodeCache::TaskResult second_result = cache->GetTaskForImageAndRef(
      second_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(second_result.need_unref);
  EXPECT_TRUE(second_result.task);
  EXPECT_TRUE(first_result.task.get() != second_result.task.get());

  DrawImage third_draw_image = CreateDrawImageInternal(
      first_image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult third_result = cache->GetTaskForImageAndRef(
      third_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(third_result.need_unref);
  EXPECT_TRUE(third_result.task.get() == first_result.task.get());

  TestTileTaskRunner::ProcessTask(first_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(first_result.task.get());
  TestTileTaskRunner::ProcessTask(second_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(second_result.task.get());

  cache->UnrefImage(first_draw_image);
  cache->UnrefImage(second_draw_image);
  cache->UnrefImage(third_draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetTaskForImageHigherQuality) {
  auto cache = CreateCache();
  SkMatrix matrix = CreateMatrix(SkSize::Make(0.4f, 0.4f));
  PaintImage first_image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage first_draw_image = CreateDrawImageInternal(
      first_image, matrix, nullptr /* color_space */, kLow_SkFilterQuality);
  ImageDecodeCache::TaskResult first_result = cache->GetTaskForImageAndRef(
      first_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(first_result.need_unref);
  EXPECT_TRUE(first_result.task);

  TestTileTaskRunner::ProcessTask(first_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(first_result.task.get());

  cache->UnrefImage(first_draw_image);

  DrawImage second_draw_image = CreateDrawImageInternal(
      first_image, matrix, nullptr /* color_space */, kMedium_SkFilterQuality);
  ImageDecodeCache::TaskResult second_result = cache->GetTaskForImageAndRef(
      second_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(second_result.need_unref);

  EXPECT_TRUE(second_result.task);
  EXPECT_TRUE(first_result.task.get() != second_result.task.get());
  TestTileTaskRunner::ProcessTask(second_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(second_result.task.get());
  cache->UnrefImage(second_draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetTaskForImageAlreadyDecodedAndLocked) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);
  EXPECT_EQ(result.task->dependencies().size(), 1u);
  EXPECT_TRUE(result.task->dependencies()[0]);

  // Run the decode but don't complete it (this will keep the decode locked).
  TestTileTaskRunner::ScheduleTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::RunTask(result.task->dependencies()[0].get());

  // Cancel the upload.
  TestTileTaskRunner::CancelTask(result.task.get());
  TestTileTaskRunner::CompleteTask(result.task.get());

  // Get the image again - we should have an upload task, but no dependent
  // decode task, as the decode was already locked.
  ImageDecodeCache::TaskResult another_result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(another_result.need_unref);
  EXPECT_TRUE(another_result.task);
  EXPECT_EQ(another_result.task->dependencies().size(), 0u);

  TestTileTaskRunner::ProcessTask(another_result.task.get());

  // Finally, complete the original decode task.
  TestTileTaskRunner::CompleteTask(result.task->dependencies()[0].get());

  cache->UnrefImage(draw_image);
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetTaskForImageAlreadyDecodedNotLocked) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);
  EXPECT_EQ(result.task->dependencies().size(), 1u);
  EXPECT_TRUE(result.task->dependencies()[0]);

  // Run the decode.
  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());

  // Cancel the upload.
  TestTileTaskRunner::CancelTask(result.task.get());
  TestTileTaskRunner::CompleteTask(result.task.get());

  // Unref the image.
  cache->UnrefImage(draw_image);

  // Get the image again - we should have an upload task and a dependent decode
  // task - this dependent task will typically just re-lock the image.
  ImageDecodeCache::TaskResult another_result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(another_result.need_unref);
  EXPECT_TRUE(another_result.task);
  EXPECT_EQ(another_result.task->dependencies().size(), 1u);
  EXPECT_TRUE(result.task->dependencies()[0]);

  TestTileTaskRunner::ProcessTask(another_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(another_result.task.get());

  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetTaskForImageAlreadyUploaded) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);
  EXPECT_EQ(result.task->dependencies().size(), 1u);
  EXPECT_TRUE(result.task->dependencies()[0]);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ScheduleTask(result.task.get());
  TestTileTaskRunner::RunTask(result.task.get());
  TestTileTaskRunner::CompleteTask(result.task.get());

  ImageDecodeCache::TaskResult another_result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(another_result.need_unref);
  EXPECT_FALSE(another_result.task);

  cache->UnrefImage(draw_image);
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetTaskForImageCanceledGetsNewTask) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());

  ImageDecodeCache::TaskResult another_result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(another_result.task.get() == result.task.get());

  // Didn't run the task, so cancel it.
  TestTileTaskRunner::CancelTask(result.task.get());
  TestTileTaskRunner::CompleteTask(result.task.get());

  // Fully cancel everything (so the raster would unref things).
  cache->UnrefImage(draw_image);
  cache->UnrefImage(draw_image);

  // Here a new task is created.
  ImageDecodeCache::TaskResult third_result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(third_result.need_unref);
  EXPECT_TRUE(third_result.task);
  EXPECT_FALSE(third_result.task.get() == result.task.get());

  TestTileTaskRunner::ProcessTask(third_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(third_result.task.get());

  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetTaskForImageCanceledWhileReffedGetsNewTask) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  ASSERT_GT(result.task->dependencies().size(), 0u);
  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());

  ImageDecodeCache::TaskResult another_result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(another_result.need_unref);
  EXPECT_TRUE(another_result.task.get() == result.task.get());

  // Didn't run the task, so cancel it.
  TestTileTaskRunner::CancelTask(result.task.get());
  TestTileTaskRunner::CompleteTask(result.task.get());

  // 2 Unrefs, so that the decode is unlocked as well.
  cache->UnrefImage(draw_image);
  cache->UnrefImage(draw_image);

  // Note that here, everything is reffed, but a new task is created. This is
  // possible with repeated schedule/cancel operations.
  ImageDecodeCache::TaskResult third_result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(third_result.need_unref);
  EXPECT_TRUE(third_result.task);
  EXPECT_FALSE(third_result.task.get() == result.task.get());

  ASSERT_GT(third_result.task->dependencies().size(), 0u);
  TestTileTaskRunner::ProcessTask(third_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(third_result.task.get());

  // Unref!
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetTaskForImageUploadCanceledButDecodeRun) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);
  EXPECT_EQ(result.task->dependencies().size(), 1u);
  EXPECT_TRUE(result.task->dependencies()[0]);

  // Cancel the upload.
  TestTileTaskRunner::CancelTask(result.task.get());
  TestTileTaskRunner::CompleteTask(result.task.get());

  // Unref the image.
  cache->UnrefImage(draw_image);

  // Run the decode task. Even though the image only has a decode ref at this
  // point, this should complete successfully.
  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
}

TEST_P(GpuImageDecodeCacheTest, NoTaskForImageAlreadyFailedDecoding) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  // Didn't run the task, so cancel it.
  TestTileTaskRunner::CancelTask(result.task.get());
  TestTileTaskRunner::CompleteTask(result.task.get());

  cache->SetImageDecodingFailedForTesting(draw_image);

  ImageDecodeCache::TaskResult another_result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_FALSE(another_result.need_unref);
  EXPECT_EQ(another_result.task.get(), nullptr);

  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetDecodedImageForDraw) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);
  EXPECT_EQ(result.task->dependencies().size(), 1u);
  EXPECT_TRUE(result.task->dependencies()[0]);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.image()->isTextureBacked());
  EXPECT_TRUE(decoded_draw_image.is_budgeted());
  EXPECT_FALSE(cache->DiscardableIsLockedForTesting(draw_image));

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetLargeDecodedImageForDraw) {
  auto cache = CreateCache();
  PaintImage image = CreateLargePaintImageForSoftwareFallback();
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(1.0f, 1.0f)));
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.is_budgeted());
  EXPECT_FALSE(decoded_draw_image.image()->isTextureBacked());
  EXPECT_TRUE_IF_NOT_USING_TRANSFER_CACHE(
      cache->DiscardableIsLockedForTesting(draw_image));

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
  EXPECT_FALSE(cache->DiscardableIsLockedForTesting(draw_image));
}

TEST_P(GpuImageDecodeCacheTest, GetDecodedImageForDrawAtRasterDecode) {
  auto cache = CreateCache();
  cache->SetWorkingSetLimitsForTesting(0 /* max_bytes */, 0 /* max_items */);

  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(1.0f, 1.0f)));

  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_FALSE(result.need_unref);
  EXPECT_FALSE(result.task);

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_FALSE(decoded_draw_image.is_budgeted());
  EXPECT_TRUE(decoded_draw_image.image()->isTextureBacked());
  EXPECT_FALSE(decoded_draw_image.is_budgeted());
  EXPECT_FALSE(cache->DiscardableIsLockedForTesting(draw_image));

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);

  // Our 0 working set size shouldn't prevent caching of unlocked discardable,
  // so our single entry should be cached.
  EXPECT_EQ(cache->GetNumCacheEntriesForTesting(), 1u);
}

TEST_P(GpuImageDecodeCacheTest, GetDecodedImageForDrawLargerScale) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)),
                              nullptr /* color_space */, kLow_SkFilterQuality);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  DrawImage larger_draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(1.5f, 1.5f)));
  ImageDecodeCache::TaskResult larger_result = cache->GetTaskForImageAndRef(
      larger_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(larger_result.need_unref);
  EXPECT_TRUE(larger_result.task);

  TestTileTaskRunner::ProcessTask(larger_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(larger_result.task.get());

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.is_budgeted());
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.image()->isTextureBacked());
  EXPECT_FALSE(cache->DiscardableIsLockedForTesting(draw_image));

  DecodedDrawImage larger_decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(larger_draw_image));
  EXPECT_TRUE(larger_decoded_draw_image.image());
  EXPECT_TRUE(larger_decoded_draw_image.is_budgeted());
  EXPECT_TRUE(larger_decoded_draw_image.image()->isTextureBacked());
  EXPECT_FALSE(cache->DiscardableIsLockedForTesting(draw_image));

  EXPECT_FALSE(decoded_draw_image.image() == larger_decoded_draw_image.image());

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
  cache->DrawWithImageFinished(larger_draw_image, larger_decoded_draw_image);
  cache->UnrefImage(larger_draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetDecodedImageForDrawHigherQuality) {
  auto cache = CreateCache();
  SkMatrix matrix = CreateMatrix(SkSize::Make(0.5f, 0.5f));
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image = CreateDrawImageInternal(
      image, matrix, nullptr /* color_space */, kLow_SkFilterQuality);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  DrawImage higher_quality_draw_image = CreateDrawImageInternal(image, matrix);
  ImageDecodeCache::TaskResult hq_result = cache->GetTaskForImageAndRef(
      higher_quality_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(hq_result.need_unref);
  EXPECT_TRUE(hq_result.task);
  TestTileTaskRunner::ProcessTask(hq_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(hq_result.task.get());

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.is_budgeted());
  EXPECT_TRUE(decoded_draw_image.image()->isTextureBacked());
  EXPECT_FALSE(cache->DiscardableIsLockedForTesting(draw_image));

  DecodedDrawImage larger_decoded_draw_image = EnsureImageBacked(
      cache->GetDecodedImageForDraw(higher_quality_draw_image));
  EXPECT_TRUE(larger_decoded_draw_image.image());
  EXPECT_TRUE(larger_decoded_draw_image.image()->isTextureBacked());
  EXPECT_FALSE(cache->DiscardableIsLockedForTesting(draw_image));

  EXPECT_FALSE(decoded_draw_image.image() == larger_decoded_draw_image.image());

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
  cache->DrawWithImageFinished(higher_quality_draw_image,
                               larger_decoded_draw_image);
  cache->UnrefImage(higher_quality_draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetDecodedImageForDrawNegative) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(-0.5f, 0.5f)));
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.is_budgeted());
  const int expected_width =
      image.width() * std::abs(draw_image.scale().width());
  const int expected_height =
      image.height() * std::abs(draw_image.scale().height());
  EXPECT_EQ(decoded_draw_image.image()->width(), expected_width);
  EXPECT_EQ(decoded_draw_image.image()->height(), expected_height);
  EXPECT_TRUE(decoded_draw_image.image()->isTextureBacked());
  EXPECT_FALSE(cache->DiscardableIsLockedForTesting(draw_image));

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetLargeScaledDecodedImageForDraw) {
  auto cache = CreateCache();
  PaintImage image = CreateLargePaintImageForSoftwareFallback(
      gfx::Size(GetLargeImageSize().width(), GetLargeImageSize().height() * 2));
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)),
                              nullptr /* color_space */, kHigh_SkFilterQuality);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.is_budgeted());
  // The mip level scale should never go below 0 in any dimension.
  EXPECT_EQ(GetLargeImageSize().width(), decoded_draw_image.image()->width());
  EXPECT_EQ(GetLargeImageSize().height(), decoded_draw_image.image()->height());

  EXPECT_EQ(decoded_draw_image.filter_quality(), kMedium_SkFilterQuality);

  EXPECT_FALSE(decoded_draw_image.image()->isTextureBacked());
  EXPECT_TRUE_IF_NOT_USING_TRANSFER_CACHE(
      cache->DiscardableIsLockedForTesting(draw_image));

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
  EXPECT_FALSE(cache->DiscardableIsLockedForTesting(draw_image));
}

TEST_P(GpuImageDecodeCacheTest, AtRasterUsedDirectlyIfSpaceAllows) {
  auto cache = CreateCache();
  const gfx::Size test_image_size = GetNormalImageSize();
  cache->SetWorkingSetLimitsForTesting(0 /* max_bytes */, 0 /* max_items */);

  PaintImage image = CreatePaintImageInternal(test_image_size);
  DrawImage draw_image = CreateDrawImageInternal(image);

  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_FALSE(result.need_unref);
  EXPECT_FALSE(result.task);

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.image()->isTextureBacked());
  EXPECT_FALSE(decoded_draw_image.is_budgeted());
  EXPECT_FALSE(cache->DiscardableIsLockedForTesting(draw_image));
  cache->DrawWithImageFinished(draw_image, decoded_draw_image);

  // Increase memory limit to allow the image and attempt to use the same image.
  // It should be available for ref.
  const size_t bytes_for_test_image =
      GetBytesNeededForSingleImage(test_image_size);
  cache->SetWorkingSetLimitsForTesting(bytes_for_test_image /* max_bytes */,
                                       256 /* max_items */);
  ImageDecodeCache::TaskResult another_result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(another_result.need_unref);
  EXPECT_FALSE(another_result.task);
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest,
       GetDecodedImageForDrawAtRasterDecodeMultipleTimes) {
  auto cache = CreateCache();
  cache->SetWorkingSetLimitsForTesting(0 /* max_bytes */, 0 /* max_items */);

  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.image()->isTextureBacked());
  EXPECT_FALSE(decoded_draw_image.is_budgeted());
  EXPECT_FALSE(cache->DiscardableIsLockedForTesting(draw_image));

  DecodedDrawImage another_decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_FALSE(another_decoded_draw_image.is_budgeted());
  EXPECT_EQ(decoded_draw_image.image()->uniqueID(),
            another_decoded_draw_image.image()->uniqueID());

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->DrawWithImageFinished(draw_image, another_decoded_draw_image);

  // Our 0 working set size shouldn't prevent caching of unlocked discardable,
  // so our single entry should be cached.
  EXPECT_EQ(cache->GetNumCacheEntriesForTesting(), 1u);
}

TEST_P(GpuImageDecodeCacheTest,
       GetLargeDecodedImageForDrawAtRasterDecodeMultipleTimes) {
  auto cache = CreateCache();
  cache->SetWorkingSetLimitsForTesting(0 /* max_bytes */, 0 /* max_items */);

  PaintImage image = CreateLargePaintImageForSoftwareFallback();
  DrawImage draw_image = CreateDrawImageInternal(image);

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_FALSE(decoded_draw_image.is_budgeted());
  EXPECT_FALSE(decoded_draw_image.image()->isTextureBacked());
  EXPECT_TRUE_IF_NOT_USING_TRANSFER_CACHE(
      cache->DiscardableIsLockedForTesting(draw_image));

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  EXPECT_FALSE(cache->DiscardableIsLockedForTesting(draw_image));

  DecodedDrawImage second_decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(second_decoded_draw_image.image());
  EXPECT_FALSE(decoded_draw_image.is_budgeted());
  EXPECT_FALSE(second_decoded_draw_image.image()->isTextureBacked());
  EXPECT_TRUE_IF_NOT_USING_TRANSFER_CACHE(
      cache->DiscardableIsLockedForTesting(draw_image));

  cache->DrawWithImageFinished(draw_image, second_decoded_draw_image);
  EXPECT_FALSE(cache->DiscardableIsLockedForTesting(draw_image));
}

TEST_P(GpuImageDecodeCacheTest, ZeroSizedImagesAreSkipped) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.f, 0.f)));

  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_FALSE(result.task);
  EXPECT_FALSE(result.need_unref);

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_FALSE(decoded_draw_image.image());

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
}

TEST_P(GpuImageDecodeCacheTest, NonOverlappingSrcRectImagesAreSkipped) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image(image,
                       SkIRect::MakeXYWH(image.width() + 1, image.height() + 1,
                                         image.width(), image.height()),
                       kMedium_SkFilterQuality,
                       CreateMatrix(SkSize::Make(1.f, 1.f)),
                       PaintImage::kDefaultFrameIndex, DefaultColorSpace());

  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_FALSE(result.task);
  EXPECT_FALSE(result.need_unref);

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_FALSE(decoded_draw_image.image());

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
}

TEST_P(GpuImageDecodeCacheTest, CanceledTasksDoNotCountAgainstBudget) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  SkIRect src_rect = SkIRect::MakeXYWH(0, 0, image.width(), image.height());
  DrawImage draw_image = CreateDrawImageInternal(
      image, CreateMatrix(SkSize::Make(1.f, 1.f)), nullptr /* color_space */,
      kMedium_SkFilterQuality, &src_rect);

  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_NE(0u, cache->GetNumCacheEntriesForTesting());
  EXPECT_TRUE(result.task);
  EXPECT_TRUE(result.need_unref);

  TestTileTaskRunner::CancelTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::CompleteTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::CancelTask(result.task.get());
  TestTileTaskRunner::CompleteTask(result.task.get());

  cache->UnrefImage(draw_image);
  EXPECT_EQ(0u, cache->GetWorkingSetBytesForTesting());
}

TEST_P(GpuImageDecodeCacheTest, ShouldAggressivelyFreeResources) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  {
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    EXPECT_TRUE(result.task);

    TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
    TestTileTaskRunner::ProcessTask(result.task.get());

    cache->UnrefImage(draw_image);

    // We should now have data image in our cache.
    EXPECT_GT(cache->GetNumCacheEntriesForTesting(), 0u);

    // Tell our cache to aggressively free resources.
    cache->SetShouldAggressivelyFreeResources(true);
    EXPECT_EQ(0u, cache->GetNumCacheEntriesForTesting());
  }

  // Attempting to upload a new image should succeed, but the image should not
  // be cached past its use.
  {
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    EXPECT_TRUE(result.task);

    TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
    TestTileTaskRunner::ProcessTask(result.task.get());
    cache->UnrefImage(draw_image);

    EXPECT_EQ(cache->GetNumCacheEntriesForTesting(), 0u);
  }

  // We now tell the cache to not aggressively free resources. The image may
  // now be cached past its use.
  cache->SetShouldAggressivelyFreeResources(false);
  {
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    EXPECT_TRUE(result.task);

    TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
    TestTileTaskRunner::ProcessTask(result.task.get());
    cache->UnrefImage(draw_image);

    EXPECT_GT(cache->GetNumCacheEntriesForTesting(), 0u);
  }
}

TEST_P(GpuImageDecodeCacheTest, OrphanedImagesFreeOnReachingZeroRefs) {
  auto cache = CreateCache();
  // Create a downscaled image.
  PaintImage first_image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage first_draw_image = CreateDrawImageInternal(
      first_image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult first_result = cache->GetTaskForImageAndRef(
      first_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(first_result.need_unref);
  EXPECT_TRUE(first_result.task);

  // The budget should account for exactly one image.
  EXPECT_EQ(cache->GetWorkingSetBytesForTesting(),
            cache->GetDrawImageSizeForTesting(first_draw_image));

  // Create a larger version of |first_image|, this should immediately free the
  // memory used by |first_image| for the smaller scale.
  DrawImage second_draw_image = CreateDrawImageInternal(
      first_image, CreateMatrix(SkSize::Make(1.0f, 1.0f)));
  ImageDecodeCache::TaskResult second_result = cache->GetTaskForImageAndRef(
      second_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(second_result.need_unref);
  EXPECT_TRUE(second_result.task);
  EXPECT_TRUE(first_result.task.get() != second_result.task.get());

  TestTileTaskRunner::ProcessTask(second_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(second_result.task.get());

  cache->UnrefImage(second_draw_image);

  // Unref the first image, it was orphaned, so it should be immediately
  // deleted.
  TestTileTaskRunner::ProcessTask(first_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(first_result.task.get());
  cache->UnrefImage(first_draw_image);

  // The cache should have exactly one image.
  EXPECT_EQ(1u, cache->GetNumCacheEntriesForTesting());
  EXPECT_EQ(0u, cache->GetInUseCacheEntriesForTesting());
}

TEST_P(GpuImageDecodeCacheTest, OrphanedZeroRefImagesImmediatelyDeleted) {
  auto cache = CreateCache();
  // Create a downscaled image.
  PaintImage first_image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage first_draw_image = CreateDrawImageInternal(
      first_image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult first_result = cache->GetTaskForImageAndRef(
      first_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(first_result.need_unref);
  EXPECT_TRUE(first_result.task);

  TestTileTaskRunner::ProcessTask(first_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(first_result.task.get());
  cache->UnrefImage(first_draw_image);

  // The budget should account for exactly one image.
  EXPECT_EQ(cache->GetNumCacheEntriesForTesting(), 1u);
  EXPECT_EQ(cache->GetInUseCacheEntriesForTesting(), 0u);

  // Create a larger version of |first_image|, this should immediately free the
  // memory used by |first_image| for the smaller scale.
  DrawImage second_draw_image = CreateDrawImageInternal(first_image);
  ImageDecodeCache::TaskResult second_result = cache->GetTaskForImageAndRef(
      second_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(second_result.need_unref);
  EXPECT_TRUE(second_result.task);
  EXPECT_TRUE(first_result.task.get() != second_result.task.get());

  TestTileTaskRunner::ProcessTask(second_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(second_result.task.get());

  cache->UnrefImage(second_draw_image);

  // The budget should account for exactly one image.
  EXPECT_EQ(cache->GetNumCacheEntriesForTesting(), 1u);
  EXPECT_EQ(cache->GetInUseCacheEntriesForTesting(), 0u);
}

TEST_P(GpuImageDecodeCacheTest, QualityCappedAtMedium) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  SkMatrix matrix = CreateMatrix(SkSize::Make(0.4f, 0.4f));

  // Create an image with kLow_FilterQuality.
  DrawImage low_draw_image = CreateDrawImageInternal(
      image, matrix, nullptr /* color_space */, kLow_SkFilterQuality);
  ImageDecodeCache::TaskResult low_result = cache->GetTaskForImageAndRef(
      low_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(low_result.need_unref);
  EXPECT_TRUE(low_result.task);

  // Get the same image at kMedium_SkFilterQuality. We can't re-use low, so we
  // should get a new task/ref.
  DrawImage medium_draw_image = CreateDrawImageInternal(image);
  ImageDecodeCache::TaskResult medium_result = cache->GetTaskForImageAndRef(
      medium_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(medium_result.need_unref);
  EXPECT_TRUE(medium_result.task.get());
  EXPECT_FALSE(low_result.task.get() == medium_result.task.get());

  // Get the same image at kHigh_SkFilterQuality. We should re-use medium.
  DrawImage high_quality_draw_image = CreateDrawImageInternal(
      image, matrix, nullptr /* color_space */, kHigh_SkFilterQuality);
  ImageDecodeCache::TaskResult high_quality_result =
      cache->GetTaskForImageAndRef(high_quality_draw_image,
                                   ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(high_quality_result.need_unref);
  EXPECT_TRUE(medium_result.task.get() == high_quality_result.task.get());

  TestTileTaskRunner::ProcessTask(low_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(low_result.task.get());
  TestTileTaskRunner::ProcessTask(medium_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(medium_result.task.get());

  cache->UnrefImage(low_draw_image);
  cache->UnrefImage(medium_draw_image);
  cache->UnrefImage(high_quality_draw_image);
}

// Ensure that switching to a mipped version of an image after the initial
// cache entry creation doesn't cause a buffer overflow/crash.
TEST_P(GpuImageDecodeCacheTest, GetDecodedImageForDrawMipUsageChange) {
  auto cache = CreateCache();
  // Create an image decode task and cache entry that does not need mips.
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image = CreateDrawImageInternal(image);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  // Cancel the task without ever using it.
  TestTileTaskRunner::CancelTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::CompleteTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::CancelTask(result.task.get());
  TestTileTaskRunner::CompleteTask(result.task.get());

  cache->UnrefImage(draw_image);

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());

  // Do an at-raster decode of the above image that *does* require mips.
  DrawImage draw_image_mips =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.6f, 0.6f)));
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image_mips));
  cache->DrawWithImageFinished(draw_image_mips, decoded_draw_image);
}

TEST_P(GpuImageDecodeCacheTest, OutOfRasterDecodeTask) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  SkMatrix matrix = CreateMatrix(SkSize::Make(1.0f, 1.0f));
  DrawImage draw_image = CreateDrawImageInternal(
      image, matrix, nullptr /* color_space */, kLow_SkFilterQuality);

  ImageDecodeCache::TaskResult result =
      cache->GetOutOfRasterDecodeTaskForImageAndRef(draw_image);
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);
  EXPECT_TRUE(cache->IsInInUseCacheForTesting(draw_image));

  // Run the decode task.
  TestTileTaskRunner::ProcessTask(result.task.get());

  // The image should remain in the cache till we unref it.
  EXPECT_TRUE(cache->IsInInUseCacheForTesting(draw_image));
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, ZeroCacheNormalWorkingSet) {
  SetCachedTexturesLimit(0);
  auto cache = CreateCache();
  // Add an image to the cache-> Due to normal working set, this should produce
  // a task and a ref.
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image = CreateDrawImageInternal(image);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);
  EXPECT_EQ(result.task->dependencies().size(), 1u);
  EXPECT_TRUE(result.task->dependencies()[0]);

  // Run the task.
  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  // Request the same image - it should be cached.
  ImageDecodeCache::TaskResult second_result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(second_result.need_unref);
  EXPECT_FALSE(second_result.task);

  // Unref both images.
  cache->UnrefImage(draw_image);
  cache->UnrefImage(draw_image);

  // Ensure the unref is processed:
  cache->ReduceCacheUsage();

  // Get the image again. As it was fully unreffed, it is no longer in the
  // working set and will be evicted due to 0 cache size.
  ImageDecodeCache::TaskResult third_result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(third_result.need_unref);
  EXPECT_TRUE(third_result.task);
  EXPECT_EQ(third_result.task->dependencies().size(), 1u);
  EXPECT_TRUE(third_result.task->dependencies()[0]);

  TestTileTaskRunner::ProcessTask(third_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(third_result.task.get());

  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, SmallCacheNormalWorkingSet) {
  // Cache will fit one image.
  SetCachedTexturesLimit(1);
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image = CreateDrawImageInternal(image);

  PaintImage image2 = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image2(
      image2, SkIRect::MakeWH(image2.width(), image2.height()),
      kMedium_SkFilterQuality, CreateMatrix(SkSize::Make(1.0f, 1.0f)),
      PaintImage::kDefaultFrameIndex, DefaultColorSpace());

  // Add an image to the cache and un-ref it.
  {
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    EXPECT_TRUE(result.task);
    EXPECT_EQ(result.task->dependencies().size(), 1u);
    EXPECT_TRUE(result.task->dependencies()[0]);

    // Run the task and unref the image.
    TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
    TestTileTaskRunner::ProcessTask(result.task.get());
    cache->UnrefImage(draw_image);
  }

  // Request the same image - it should be cached.
  {
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    EXPECT_FALSE(result.task);
    cache->UnrefImage(draw_image);
  }

  // Add a new image to the cache It should push out the old one.
  {
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image2, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    EXPECT_TRUE(result.task);
    EXPECT_EQ(result.task->dependencies().size(), 1u);
    EXPECT_TRUE(result.task->dependencies()[0]);

    // Run the task and unref the image.
    TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
    TestTileTaskRunner::ProcessTask(result.task.get());
    cache->UnrefImage(draw_image2);
  }

  // Request the second image - it should be cached.
  {
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image2, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    EXPECT_FALSE(result.task);
    cache->UnrefImage(draw_image2);
  }

  // Request the first image - it should have been evicted and return a new
  // task.
  {
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    EXPECT_TRUE(result.task);
    EXPECT_EQ(result.task->dependencies().size(), 1u);
    EXPECT_TRUE(result.task->dependencies()[0]);

    // Run the task and unref the image.
    TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
    TestTileTaskRunner::ProcessTask(result.task.get());
    cache->UnrefImage(draw_image);
  }
}

TEST_P(GpuImageDecodeCacheTest, ClearCache) {
  auto cache = CreateCache();
  for (int i = 0; i < 10; ++i) {
    PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
    DrawImage draw_image = CreateDrawImageInternal(image);
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    EXPECT_TRUE(result.task);
    TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
    TestTileTaskRunner::ProcessTask(result.task.get());
    cache->UnrefImage(draw_image);
  }

  // We should now have images in our cache.
  EXPECT_EQ(cache->GetNumCacheEntriesForTesting(), 10u);

  // Tell our cache to clear resources.
  cache->ClearCache();

  // We should now have nothing in our cache.
  EXPECT_EQ(cache->GetNumCacheEntriesForTesting(), 0u);
}

TEST_P(GpuImageDecodeCacheTest, ClearCacheInUse) {
  auto cache = CreateCache();
  // Create an image but keep it reffed so it can't be immediately freed.
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image = CreateDrawImageInternal(image);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);
  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  // We should now have data image in our cache.
  EXPECT_GT(cache->GetWorkingSetBytesForTesting(), 0u);
  EXPECT_EQ(cache->GetNumCacheEntriesForTesting(), 1u);

  // Tell our cache to clear resources.
  cache->ClearCache();
  // We should still have data, as we can't clear the in-use entry.
  EXPECT_GT(cache->GetWorkingSetBytesForTesting(), 0u);
  // But the num (persistent) entries should be 0, as the entry is orphaned.
  EXPECT_EQ(cache->GetNumCacheEntriesForTesting(), 0u);

  // Unref the image, it should immidiately delete, leaving our cache empty.
  cache->UnrefImage(draw_image);
  EXPECT_EQ(cache->GetWorkingSetBytesForTesting(), 0u);
  EXPECT_EQ(cache->GetNumCacheEntriesForTesting(), 0u);
}

TEST_P(GpuImageDecodeCacheTest, GetTaskForImageDifferentColorSpace) {
  auto cache = CreateCache();
  gfx::ColorSpace color_space_a = gfx::ColorSpace::CreateSRGB();
  gfx::ColorSpace color_space_b = gfx::ColorSpace::CreateXYZD50();

  PaintImage first_image = CreatePaintImageInternal(gfx::Size(100, 100));
  DrawImage first_draw_image = CreateDrawImageInternal(
      first_image, CreateMatrix(SkSize::Make(1.0f, 1.0f)), &color_space_a);
  ImageDecodeCache::TaskResult first_result = cache->GetTaskForImageAndRef(
      first_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(first_result.need_unref);
  EXPECT_TRUE(first_result.task);

  DrawImage second_draw_image = CreateDrawImageInternal(
      first_image, CreateMatrix(SkSize::Make(1.0f, 1.0f)), &color_space_b);
  ImageDecodeCache::TaskResult second_result = cache->GetTaskForImageAndRef(
      second_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(second_result.need_unref);
  EXPECT_TRUE(second_result.task);
  EXPECT_TRUE(first_result.task.get() != second_result.task.get());

  DrawImage third_draw_image = CreateDrawImageInternal(
      first_image, CreateMatrix(SkSize::Make(1.0f, 1.0f)), &color_space_a);
  ImageDecodeCache::TaskResult third_result = cache->GetTaskForImageAndRef(
      third_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(third_result.need_unref);
  EXPECT_TRUE(third_result.task.get() == first_result.task.get());

  TestTileTaskRunner::ProcessTask(first_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(first_result.task.get());
  TestTileTaskRunner::ProcessTask(second_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(second_result.task.get());

  cache->UnrefImage(first_draw_image);
  cache->UnrefImage(second_draw_image);
  cache->UnrefImage(third_draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetTaskForLargeImageNonSRGBColorSpace) {
  auto cache = CreateCache();
  gfx::ColorSpace color_space = gfx::ColorSpace::CreateXYZD50();
  PaintImage image = CreateLargePaintImageForSoftwareFallback();
  DrawImage draw_image = CreateDrawImageInternal(
      image, CreateMatrix(SkSize::Make(1.0f, 1.0f)), &color_space);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, CacheDecodesExpectedFrames) {
  auto cache = CreateCache();
  std::vector<FrameMetadata> frames = {
      FrameMetadata(true, base::TimeDelta::FromMilliseconds(2)),
      FrameMetadata(true, base::TimeDelta::FromMilliseconds(3)),
      FrameMetadata(true, base::TimeDelta::FromMilliseconds(4)),
      FrameMetadata(true, base::TimeDelta::FromMilliseconds(5)),
  };
  const gfx::Size test_image_size = GetNormalImageSize();
  SkImageInfo info =
      SkImageInfo::Make(test_image_size.width(), test_image_size.height(),
                        color_type_, kPremul_SkAlphaType);
  sk_sp<FakePaintImageGenerator> generator =
      do_yuv_decode_ ? sk_make_sp<FakePaintImageGenerator>(
                           info, GetYUV420SizeInfo(test_image_size), frames)
                     : sk_make_sp<FakePaintImageGenerator>(info, frames);
  PaintImage image = PaintImageBuilder::WithDefault()
                         .set_id(PaintImage::GetNextId())
                         .set_paint_image_generator(generator)
                         .TakePaintImage();

  viz::ContextProvider::ScopedContextLock context_lock(context_provider());

  SkFilterQuality quality = kMedium_SkFilterQuality;
  DrawImage draw_image(image, SkIRect::MakeWH(image.width(), image.height()),
                       quality, CreateMatrix(SkSize::Make(1.0f, 1.0f)), 1u,
                       DefaultColorSpace());
  auto decoded_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  ASSERT_TRUE(decoded_image.image());
  ASSERT_EQ(generator->frames_decoded().size(), 1u);
  EXPECT_EQ(generator->frames_decoded().count(1u), 1u);
  generator->reset_frames_decoded();
  cache->DrawWithImageFinished(draw_image, decoded_image);

  // Scaled.
  DrawImage scaled_draw_image(draw_image, 0.5f, 2u,
                              draw_image.target_color_space());
  decoded_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(scaled_draw_image));
  ASSERT_TRUE(decoded_image.image());
  ASSERT_EQ(generator->frames_decoded().size(), 1u);
  EXPECT_EQ(generator->frames_decoded().count(2u), 1u);
  generator->reset_frames_decoded();
  cache->DrawWithImageFinished(scaled_draw_image, decoded_image);

  // Subset.
  const int32_t subset_width = 5;
  const int32_t subset_height = 5;
  ASSERT_LT(subset_width, test_image_size.width());
  ASSERT_LT(subset_height, test_image_size.height());
  DrawImage subset_draw_image(
      image, SkIRect::MakeWH(subset_width, subset_height), quality,
      CreateMatrix(SkSize::Make(1.0f, 1.0f)), 3u, DefaultColorSpace());
  decoded_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(subset_draw_image));
  ASSERT_TRUE(decoded_image.image());
  ASSERT_EQ(generator->frames_decoded().size(), 1u);
  EXPECT_EQ(generator->frames_decoded().count(3u), 1u);
  generator->reset_frames_decoded();
  cache->DrawWithImageFinished(subset_draw_image, decoded_image);
}

TEST_P(GpuImageDecodeCacheTest, OrphanedDataCancelledWhileReplaced) {
  auto cache = CreateCache();
  // Create a downscaled image.
  PaintImage first_image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage first_draw_image = CreateDrawImageInternal(
      first_image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  ImageDecodeCache::TaskResult first_result = cache->GetTaskForImageAndRef(
      first_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(first_result.need_unref);
  EXPECT_TRUE(first_result.task);

  // The cache should have exactly one image.
  EXPECT_EQ(1u, cache->GetNumCacheEntriesForTesting());

  // Create a larger version of |first_image|, this should immediately free
  // the memory used by |first_image| for the smaller scale.
  DrawImage second_draw_image = CreateDrawImageInternal(first_image);
  ImageDecodeCache::TaskResult second_result = cache->GetTaskForImageAndRef(
      second_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(second_result.need_unref);
  EXPECT_TRUE(second_result.task);
  EXPECT_TRUE(first_result.task.get() != second_result.task.get());

  // The cache should have two images.
  EXPECT_EQ(1u, cache->GetNumCacheEntriesForTesting());

  // Cancel and unref the first image, it was orphaned, so it should be
  // immediately deleted.
  TestTileTaskRunner::CancelTask(first_result.task->dependencies()[0].get());
  TestTileTaskRunner::CompleteTask(first_result.task->dependencies()[0].get());
  TestTileTaskRunner::CancelTask(first_result.task.get());
  TestTileTaskRunner::CompleteTask(first_result.task.get());
  cache->UnrefImage(first_draw_image);

  // The cache should have exactly one image.
  EXPECT_EQ(1u, cache->GetNumCacheEntriesForTesting());

  // Unref the second image. It is persistent, and should remain in the cache.
  TestTileTaskRunner::ProcessTask(second_result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(second_result.task.get());
  cache->UnrefImage(second_draw_image);

  // The cache should have exactly one image.
  EXPECT_EQ(1u, cache->GetNumCacheEntriesForTesting());
  EXPECT_EQ(0u, cache->GetInUseCacheEntriesForTesting());
}

TEST_P(GpuImageDecodeCacheTest, AlreadyBudgetedImagesAreNotAtRaster) {
  auto cache = CreateCache();
  const gfx::Size test_image_size = GetNormalImageSize();

  PaintImage image = CreatePaintImageInternal(test_image_size);
  DrawImage draw_image = CreateDrawImageInternal(image);
  const size_t bytes_for_test_image =
      GetBytesNeededForSingleImage(test_image_size);

  // Allow a single small image and lock it.
  cache->SetWorkingSetLimitsForTesting(bytes_for_test_image,
                                       1u /* max_items */);

  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  // Try locking the same image again, its already budgeted so it shouldn't be
  // at-raster.
  result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.is_budgeted());
  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, ImageBudgetingByCount) {
  auto cache = CreateCache();
  const gfx::Size test_image_size = GetNormalImageSize();

  // Allow a single image by count. Use a high byte limit as we want to test the
  // count restriction.
  const size_t bytes_for_test_image =
      GetBytesNeededForSingleImage(test_image_size);
  cache->SetWorkingSetLimitsForTesting(
      bytes_for_test_image * 100 /* max_bytes */, 1u /* max_items */);
  PaintImage image = CreatePaintImageInternal(test_image_size);
  DrawImage draw_image = CreateDrawImageInternal(image);

  // The image counts against our budget.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.is_budgeted());

  // Try another image, it shouldn't be budgeted and should be at-raster.
  PaintImage second_paint_image =
      CreatePaintImageInternal(GetNormalImageSize());
  DrawImage second_draw_image = CreateDrawImageInternal(second_paint_image);

  // Should be at raster.
  ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
      second_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_FALSE(result.need_unref);
  EXPECT_FALSE(result.task);
  // Image retrieved from at-raster decode should not be budgeted.
  DecodedDrawImage second_decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(second_draw_image));
  EXPECT_TRUE(second_decoded_draw_image.image());
  EXPECT_FALSE(second_decoded_draw_image.is_budgeted());

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->DrawWithImageFinished(second_draw_image, second_decoded_draw_image);
}

TEST_P(GpuImageDecodeCacheTest, ImageBudgetingBySize) {
  auto cache = CreateCache();
  const gfx::Size test_image_size = GetNormalImageSize();

  PaintImage image = CreatePaintImageInternal(test_image_size);
  DrawImage draw_image = CreateDrawImageInternal(image);

  const size_t bytes_for_test_image =
      GetBytesNeededForSingleImage(test_image_size);

  // Allow a single small image by size. Don't restrict the
  // items limit as we want to test the size limit.
  cache->SetWorkingSetLimitsForTesting(bytes_for_test_image,
                                       256 /* max_items */);

  // The image counts against our budget.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.is_budgeted());

  // Try another image, it shouldn't be budgeted and should be at-raster.
  PaintImage test_paint_image = CreatePaintImageInternal(test_image_size);
  DrawImage second_draw_image = CreateDrawImageInternal(test_paint_image);

  // Should be at raster.
  ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
      second_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_FALSE(result.need_unref);
  EXPECT_FALSE(result.task);
  // Image retrieved from at-raster decode should not be budgeted.
  DecodedDrawImage second_decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(second_draw_image));
  EXPECT_TRUE(second_decoded_draw_image.image());
  EXPECT_FALSE(second_decoded_draw_image.is_budgeted());

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->DrawWithImageFinished(second_draw_image, second_decoded_draw_image);
}

TEST_P(GpuImageDecodeCacheTest,
       ColorConversionDuringDecodeForLargeImageNonSRGBColorSpace) {
  auto cache = CreateCache();
  gfx::ColorSpace color_space = gfx::ColorSpace::CreateXYZD50();

  PaintImage image = CreateLargePaintImageForSoftwareFallback();
  DrawImage draw_image = CreateDrawImageInternal(
      image, CreateMatrix(SkSize::Make(1.0f, 1.0f)), &color_space);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      cache->GetDecodedImageForDraw(draw_image);

  sk_sp<SkColorSpace> target_color_space = cache->SupportsColorSpaceConversion()
                                               ? color_space.ToSkColorSpace()
                                               : nullptr;

  if (use_transfer_cache_) {
    // If using the transfer cache, the color conversion should be applied
    // there as well, even if it is a software image.
    sk_sp<SkImage> service_image = GetLastTransferredImage();
    ASSERT_TRUE(image);
    EXPECT_FALSE(service_image->isTextureBacked());
    EXPECT_EQ(image.width(), service_image->width());
    EXPECT_EQ(image.height(), service_image->height());

    // Color space should be logically equal to the original color space.
    EXPECT_TRUE(SkColorSpace::Equals(service_image->colorSpace(),
                                     target_color_space.get()));
  } else {
    sk_sp<SkImage> decoded_image =
        cache->GetSWImageDecodeForTesting(draw_image);
    // Ensure that the "uploaded" image we get back is the same as the decoded
    // image we've cached.
    EXPECT_TRUE(decoded_image == decoded_draw_image.image());
    // Ensure that the SW decoded image had colorspace conversion applied.
    EXPECT_TRUE(SkColorSpace::Equals(decoded_image->colorSpace(),
                                     target_color_space.get()));
  }

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest,
       ColorConversionDuringUploadForSmallImageNonSRGBColorSpace) {
  auto cache = CreateCache();
  gfx::ColorSpace color_space = gfx::ColorSpace::CreateDisplayP3D65();

  PaintImage image = CreatePaintImageInternal(gfx::Size(11, 12));
  DrawImage draw_image = CreateDrawImageInternal(
      image, CreateMatrix(SkSize::Make(1.0f, 1.0f)), &color_space);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      cache->GetDecodedImageForDraw(draw_image);

  sk_sp<SkColorSpace> target_color_space = cache->SupportsColorSpaceConversion()
                                               ? color_space.ToSkColorSpace()
                                               : nullptr;

  if (use_transfer_cache_) {
    // If using the transfer cache, the color conversion should be applied
    // there during upload.
    sk_sp<SkImage> service_image = GetLastTransferredImage();
    ASSERT_TRUE(image);
    EXPECT_TRUE(service_image->isTextureBacked());
    EXPECT_EQ(image.width(), service_image->width());
    EXPECT_EQ(image.height(), service_image->height());

    if (!do_yuv_decode_) {
      // Color space should be logically equal to the original color space.
      EXPECT_TRUE(SkColorSpace::Equals(service_image->colorSpace(),
                                       target_color_space.get()));
    }
  } else {
    // Ensure that the HW uploaded image had color space conversion applied.
    EXPECT_TRUE(SkColorSpace::Equals(decoded_draw_image.image()->colorSpace(),
                                     target_color_space.get()));
  }

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, NonLazyImageUploadNoScale) {
  if (do_yuv_decode_) {
    // YUV bitmap images do not happen, so this test will always skip for YUV.
    return;
  }
  auto cache = CreateCache();

  PaintImage image = CreateBitmapImageInternal(GetNormalImageSize());
  DrawImage draw_image = CreateDrawImageInternal(image);
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.is_budgeted());
  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  // For non-lazy images used at the original scale, no cpu component should be
  // cached
  EXPECT_FALSE(cache->GetSWImageDecodeForTesting(draw_image));
}

TEST_P(GpuImageDecodeCacheTest, NonLazyImageUploadTaskHasNoDeps) {
  if (do_yuv_decode_) {
    // YUV bitmap images do not happen, so this test will always skip for YUV.
    return;
  }
  auto cache = CreateCache();

  PaintImage image = CreateBitmapImageInternal(GetNormalImageSize());
  DrawImage draw_image = CreateDrawImageInternal(image);
  auto result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);
  EXPECT_TRUE(result.task->dependencies().empty());
  TestTileTaskRunner::ProcessTask(result.task.get());

  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, NonLazyImageUploadTaskCancelled) {
  if (do_yuv_decode_) {
    // YUV bitmap images do not happen, so this test will always skip for YUV.
    return;
  }
  auto cache = CreateCache();

  PaintImage image = CreateBitmapImageInternal(GetNormalImageSize());
  DrawImage draw_image = CreateDrawImageInternal(image);
  auto result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);
  EXPECT_TRUE(result.task->dependencies().empty());
  TestTileTaskRunner::CancelTask(result.task.get());
  TestTileTaskRunner::CompleteTask(result.task.get());

  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, NonLazyImageLargeImageColorConverted) {
  if (do_yuv_decode_) {
    // YUV bitmap images do not happen, so this test will always skip for YUV.
    return;
  }
  auto cache = CreateCache();
  const bool should_cache_sw_image =
      cache->SupportsColorSpaceConversion() && !use_transfer_cache_;

  PaintImage image = CreateBitmapImageInternal(GetLargeImageSize());
  gfx::ColorSpace color_space = gfx::ColorSpace::CreateDisplayP3D65();
  DrawImage draw_image = CreateDrawImageInternal(
      image, CreateMatrix(SkSize::Make(1.0f, 1.0f)), &color_space);
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.is_budgeted());
  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  // For non-lazy images color converted during scaling, cpu component should be
  // cached.
  auto sw_image = cache->GetSWImageDecodeForTesting(draw_image);
  ASSERT_EQ(!!sw_image, should_cache_sw_image);
  if (should_cache_sw_image) {
    EXPECT_TRUE(SkColorSpace::Equals(sw_image->colorSpace(),
                                     color_space.ToSkColorSpace().get()));
  }
}

TEST_P(GpuImageDecodeCacheTest, NonLazyImageUploadDownscaled) {
  if (do_yuv_decode_) {
    // YUV bitmap images do not happen, so this test will always skip for YUV.
    return;
  }
  auto cache = CreateCache();

  PaintImage image = CreateBitmapImageInternal(GetNormalImageSize());
  DrawImage draw_image =
      CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.is_budgeted());
  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
}

TEST_P(GpuImageDecodeCacheTest, KeepOnlyLast2ContentIds) {
  auto cache = CreateCache();

  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  const PaintImage::Id paint_image_id = PaintImage::GetNextId();
  std::vector<DrawImage> draw_images;
  std::vector<DecodedDrawImage> decoded_draw_images;

  for (int i = 0; i < 10; ++i) {
    PaintImage image = CreatePaintImageInternal(
        GetNormalImageSize(), SkColorSpace::MakeSRGB(), paint_image_id);
    DrawImage draw_image =
        CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.5f, 0.5f)));
    DecodedDrawImage decoded_draw_image =
        EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));

    draw_images.push_back(draw_image);
    decoded_draw_images.push_back(decoded_draw_image);

    if (i == 0)
      continue;

    // We should only have the last 2 entries in the persistent cache, even
    // though everything is in the in use cache.
    EXPECT_EQ(cache->GetNumCacheEntriesForTesting(), 2u);
    EXPECT_EQ(cache->GetInUseCacheEntriesForTesting(), i + 1u);
    EXPECT_TRUE(cache->IsInPersistentCacheForTesting(draw_images[i]));
    EXPECT_TRUE(cache->IsInPersistentCacheForTesting(draw_images[i - 1]));
  }

  for (int i = 0; i < 10; ++i) {
    cache->DrawWithImageFinished(draw_images[i], decoded_draw_images[i]);
  }

  // We have a single tracked entry, that gets cleared once we purge the cache.
  EXPECT_EQ(cache->paint_image_entries_count_for_testing(), 1u);
  cache->OnMemoryPressure(
      base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_CRITICAL);
  EXPECT_EQ(cache->paint_image_entries_count_for_testing(), 0u);
}

TEST_P(GpuImageDecodeCacheTest, DecodeToScale) {
  if (do_yuv_decode_) {
    // TODO(crbug.com/927437): Modify test after decoding to scale for YUV is
    // implemented.
    return;
  }
  auto cache = CreateCache();

  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  const SkISize full_size = SkISize::Make(100, 100);
  const std::vector<SkISize> supported_sizes = {SkISize::Make(25, 25),
                                                SkISize::Make(50, 50)};
  const std::vector<FrameMetadata> frames = {FrameMetadata()};
  const SkImageInfo info =
      SkImageInfo::MakeN32Premul(full_size.width(), full_size.height(),
                                 DefaultColorSpace().ToSkColorSpace());
  sk_sp<FakePaintImageGenerator> generator =
      sk_make_sp<FakePaintImageGenerator>(info, frames, true, supported_sizes);
  PaintImage paint_image = PaintImageBuilder::WithDefault()
                               .set_id(PaintImage::GetNextId())
                               .set_paint_image_generator(generator)
                               .TakePaintImage();

  DrawImage draw_image = CreateDrawImageInternal(
      paint_image, CreateMatrix(SkSize::Make(0.5, 0.5)));
  DecodedDrawImage decoded_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  const int expected_width =
      paint_image.width() * std::abs(draw_image.scale().width());
  const int expected_height =
      paint_image.height() * std::abs(draw_image.scale().height());
  ASSERT_TRUE(decoded_image.image());
  EXPECT_EQ(decoded_image.image()->width(), expected_width);
  EXPECT_EQ(decoded_image.image()->height(), expected_height);

  // We should have requested a scaled decode from the generator.
  ASSERT_EQ(generator->decode_infos().size(), 1u);
  EXPECT_EQ(generator->decode_infos().at(0).width(), expected_width);
  EXPECT_EQ(generator->decode_infos().at(0).height(), expected_height);

  cache->DrawWithImageFinished(draw_image, decoded_image);
}

TEST_P(GpuImageDecodeCacheTest, DecodeToScaleNoneQuality) {
  if (do_yuv_decode_) {
    // TODO(crbug.com/927437): Modify test after decoding to scale for YUV is
    // implemented.
    return;
  }
  auto cache = CreateCache();

  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  SkISize full_size = SkISize::Make(100, 100);
  std::vector<SkISize> supported_sizes = {SkISize::Make(25, 25),
                                          SkISize::Make(50, 50)};
  std::vector<FrameMetadata> frames = {FrameMetadata()};
  sk_sp<FakePaintImageGenerator> generator =
      sk_make_sp<FakePaintImageGenerator>(
          SkImageInfo::MakeN32Premul(full_size.width(), full_size.height(),
                                     DefaultColorSpace().ToSkColorSpace()),
          frames, true, supported_sizes);
  PaintImage paint_image = PaintImageBuilder::WithDefault()
                               .set_id(PaintImage::GetNextId())
                               .set_paint_image_generator(generator)
                               .TakePaintImage();

  DrawImage draw_image =
      CreateDrawImageInternal(paint_image, CreateMatrix(SkSize::Make(0.5, 0.5)),
                              nullptr /* color_space */, kNone_SkFilterQuality);
  DecodedDrawImage decoded_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  ASSERT_TRUE(decoded_image.image());
  const int expected_drawn_width =
      paint_image.width() * std::abs(draw_image.scale().width());
  const int expected_drawn_height =
      paint_image.height() * std::abs(draw_image.scale().height());
  EXPECT_EQ(decoded_image.image()->width(), expected_drawn_width);
  EXPECT_EQ(decoded_image.image()->height(), expected_drawn_height);

  // We should have requested the original decode from the generator.
  ASSERT_EQ(generator->decode_infos().size(), 1u);
  EXPECT_EQ(generator->decode_infos().at(0).width(), full_size.width());
  EXPECT_EQ(generator->decode_infos().at(0).height(), full_size.height());
  cache->DrawWithImageFinished(draw_image, decoded_image);
}

TEST_P(GpuImageDecodeCacheTest, BasicMips) {
  auto decode_and_check_mips = [this](SkFilterQuality filter_quality,
                                      SkSize scale, gfx::ColorSpace color_space,
                                      bool should_have_mips) {
    auto cache = CreateCache();

    PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
    DrawImage draw_image = CreateDrawImageInternal(
        image, CreateMatrix(scale), &color_space, filter_quality);
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    EXPECT_TRUE(result.task);

    TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
    TestTileTaskRunner::ProcessTask(result.task.get());

    // Must hold context lock before calling GetDecodedImageForDraw /
    // DrawWithImageFinished.
    viz::ContextProvider::ScopedContextLock context_lock(context_provider());
    // Pull out transfer cache ID from the DecodedDrawImage while it still has
    // it attached.
    DecodedDrawImage serialized_decoded_draw_image =
        cache->GetDecodedImageForDraw(draw_image);
    const base::Optional<uint32_t> transfer_cache_entry_id =
        serialized_decoded_draw_image.transfer_cache_entry_id();
    DecodedDrawImage decoded_draw_image =
        EnsureImageBacked(std::move(serialized_decoded_draw_image));
    EXPECT_TRUE(decoded_draw_image.image());
    EXPECT_TRUE(decoded_draw_image.image()->isTextureBacked());

    if (do_yuv_decode_) {
      // Skia will flatten a YUV SkImage upon calling makeTextureImage. Thus,
      // we must separately request mips for each plane and compare to the
      // original uploaded planes.
      CompareAllPlanesToMippedVersions(
          cache.get(), draw_image, transfer_cache_entry_id, should_have_mips);
    } else {
      sk_sp<SkImage> image_with_mips =
          decoded_draw_image.image()->makeTextureImage(
              context_provider()->GrContext(), GrMipMapped::kYes);
      EXPECT_EQ(should_have_mips,
                image_with_mips == decoded_draw_image.image());
    }
    cache->DrawWithImageFinished(draw_image, decoded_draw_image);
    cache->UnrefImage(draw_image);
  };

  // No scale == no mips.
  decode_and_check_mips(kMedium_SkFilterQuality, SkSize::Make(1.0f, 1.0f),
                        DefaultColorSpace(), false);
  // Full mip level scale == no mips
  decode_and_check_mips(kMedium_SkFilterQuality, SkSize::Make(0.5f, 0.5f),
                        DefaultColorSpace(), false);
  // Low filter quality == no mips
  decode_and_check_mips(kLow_SkFilterQuality, SkSize::Make(0.6f, 0.6f),
                        DefaultColorSpace(), false);
  // None filter quality == no mips
  decode_and_check_mips(kNone_SkFilterQuality, SkSize::Make(0.6f, 0.6f),
                        DefaultColorSpace(), false);
  // Medium filter quality == mips
  decode_and_check_mips(kMedium_SkFilterQuality, SkSize::Make(0.6f, 0.6f),
                        DefaultColorSpace(), true);
  // Color conversion preserves mips
  decode_and_check_mips(kMedium_SkFilterQuality, SkSize::Make(0.6f, 0.6f),
                        gfx::ColorSpace::CreateXYZD50(), true);
}

TEST_P(GpuImageDecodeCacheTest, MipsAddedSubsequentDraw) {
  auto cache = CreateCache();

  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());

  // Create an image with no scaling. It will not have mips.
  {
    DrawImage draw_image = CreateDrawImageInternal(image);
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    EXPECT_TRUE(result.task);

    TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
    TestTileTaskRunner::ProcessTask(result.task.get());

    // Must hold context lock before calling GetDecodedImageForDraw /
    // DrawWithImageFinished.
    viz::ContextProvider::ScopedContextLock context_lock(context_provider());
    // Pull out transfer cache ID from the DecodedDrawImage while it still has
    // it attached.
    DecodedDrawImage serialized_decoded_draw_image =
        cache->GetDecodedImageForDraw(draw_image);
    const base::Optional<uint32_t> transfer_cache_entry_id =
        serialized_decoded_draw_image.transfer_cache_entry_id();
    DecodedDrawImage decoded_draw_image =
        EnsureImageBacked(std::move(serialized_decoded_draw_image));
    EXPECT_TRUE(decoded_draw_image.image());
    EXPECT_TRUE(decoded_draw_image.image()->isTextureBacked());

    // No mips should be generated.
    if (do_yuv_decode_) {
      // Skia will flatten a YUV SkImage upon calling makeTextureImage. Thus,
      // we must separately request mips for each plane and compare to the
      // original uploaded planes.
      CompareAllPlanesToMippedVersions(cache.get(), draw_image,
                                       transfer_cache_entry_id,
                                       false /* should_have_mips */);
    } else {
      sk_sp<SkImage> image_with_mips =
          decoded_draw_image.image()->makeTextureImage(
              context_provider()->GrContext(), GrMipMapped::kYes);
      ASSERT_TRUE(image_with_mips);
      EXPECT_NE(image_with_mips, decoded_draw_image.image());
    }
    cache->DrawWithImageFinished(draw_image, decoded_draw_image);
    cache->UnrefImage(draw_image);
  }

  // Call ReduceCacheUsage to clean up.
  cache->ReduceCacheUsage();

  // Request the same image again, but this time with a scale. We should get
  // no new task (re-uses the existing image), but mips should have been
  // added.
  {
    DrawImage draw_image =
        CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.6f, 0.6f)));
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    EXPECT_FALSE(result.task);

    // Must hold context lock before calling GetDecodedImageForDraw /
    // DrawWithImageFinished.
    viz::ContextProvider::ScopedContextLock context_lock(context_provider());
    // Pull out transfer cache ID from the DecodedDrawImage while it still has
    // it attached.
    DecodedDrawImage serialized_decoded_draw_image =
        cache->GetDecodedImageForDraw(draw_image);
    const base::Optional<uint32_t> transfer_cache_entry_id =
        serialized_decoded_draw_image.transfer_cache_entry_id();
    DecodedDrawImage decoded_draw_image =
        EnsureImageBacked(std::move(serialized_decoded_draw_image));

    EXPECT_TRUE(decoded_draw_image.image());
    EXPECT_TRUE(decoded_draw_image.image()->isTextureBacked());

    // Mips should be generated
    if (do_yuv_decode_) {
      // Skia will flatten a YUV SkImage upon calling makeTextureImage. Thus,
      // we must separately request mips for each plane and compare to the
      // original uploaded planes.
      CompareAllPlanesToMippedVersions(cache.get(), draw_image,
                                       transfer_cache_entry_id,
                                       true /* should_have_mips */);
    } else {
      sk_sp<SkImage> image_with_mips =
          decoded_draw_image.image()->makeTextureImage(
              context_provider()->GrContext(), GrMipMapped::kYes);
      EXPECT_EQ(image_with_mips, decoded_draw_image.image());
    }
    cache->DrawWithImageFinished(draw_image, decoded_draw_image);
    cache->UnrefImage(draw_image);
  }
}

TEST_P(GpuImageDecodeCacheTest, MipsAddedWhileOriginalInUse) {
  auto cache = CreateCache();
  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());

  struct Decode {
    DrawImage image;
    DecodedDrawImage decoded_image;
  };
  std::vector<Decode> images_to_unlock;

  // Create an image with no scaling. It will not have mips.
  {
    DrawImage draw_image = CreateDrawImageInternal(image);
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    EXPECT_TRUE(result.task);

    TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
    TestTileTaskRunner::ProcessTask(result.task.get());

    // Must hold context lock before calling GetDecodedImageForDraw /
    // DrawWithImageFinished.
    viz::ContextProvider::ScopedContextLock context_lock(context_provider());
    // Pull out transfer cache ID from the DecodedDrawImage while it still has
    // it attached.
    DecodedDrawImage serialized_decoded_draw_image =
        cache->GetDecodedImageForDraw(draw_image);
    const base::Optional<uint32_t> transfer_cache_entry_id =
        serialized_decoded_draw_image.transfer_cache_entry_id();
    DecodedDrawImage decoded_draw_image =
        EnsureImageBacked(std::move(serialized_decoded_draw_image));
    ASSERT_TRUE(decoded_draw_image.image());
    ASSERT_TRUE(decoded_draw_image.image()->isTextureBacked());

    // No mips should be generated.
    if (do_yuv_decode_) {
      // Skia will flatten a YUV SkImage upon calling makeTextureImage. Thus,
      // we must separately request mips for each plane and compare to the
      // original uploaded planes.
      CompareAllPlanesToMippedVersions(cache.get(), draw_image,
                                       transfer_cache_entry_id,
                                       false /* should_have_mips */);
    } else {
      sk_sp<SkImage> image_with_mips =
          decoded_draw_image.image()->makeTextureImage(
              context_provider()->GrContext(), GrMipMapped::kYes);
      EXPECT_NE(image_with_mips, decoded_draw_image.image());
    }
    images_to_unlock.push_back({draw_image, decoded_draw_image});
  }

  // Second decode with mips.
  {
    DrawImage draw_image =
        CreateDrawImageInternal(image, CreateMatrix(SkSize::Make(0.6f, 0.6f)));
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    EXPECT_FALSE(result.task);

    // Must hold context lock before calling GetDecodedImageForDraw /
    // DrawWithImageFinished.
    viz::ContextProvider::ScopedContextLock context_lock(context_provider());
    // Pull out transfer cache ID from the DecodedDrawImage while it still has
    // it attached.
    DecodedDrawImage serialized_decoded_draw_image =
        cache->GetDecodedImageForDraw(draw_image);
    const base::Optional<uint32_t> transfer_cache_entry_id =
        serialized_decoded_draw_image.transfer_cache_entry_id();
    DecodedDrawImage decoded_draw_image =
        EnsureImageBacked(std::move(serialized_decoded_draw_image));

    ASSERT_TRUE(decoded_draw_image.image());
    ASSERT_TRUE(decoded_draw_image.image()->isTextureBacked());

    // Mips should be generated.
    if (do_yuv_decode_) {
      // Skia will flatten a YUV SkImage upon calling makeTextureImage. Thus,
      // we must separately request mips for each plane and compare to the
      // original uploaded planes.
      CompareAllPlanesToMippedVersions(cache.get(), draw_image,
                                       transfer_cache_entry_id,
                                       true /* should_have_mips */);
    } else {
      sk_sp<SkImage> image_with_mips =
          decoded_draw_image.image()->makeTextureImage(
              context_provider()->GrContext(), GrMipMapped::kYes);
      EXPECT_EQ(image_with_mips, decoded_draw_image.image());
    }
    images_to_unlock.push_back({draw_image, decoded_draw_image});
  }

  // Reduce cache usage to make sure anything marked for deletion is actually
  // deleted.
  cache->ReduceCacheUsage();

  {
    // All images which are currently ref-ed must have locked textures.
    viz::ContextProvider::ScopedContextLock context_lock(context_provider());
    for (const auto& draw_and_decoded_draw_image : images_to_unlock) {
      if (!use_transfer_cache_) {
        if (do_yuv_decode_) {
          DrawImage draw_image = draw_and_decoded_draw_image.image;
          for (size_t i = 0; i < SkYUVASizeInfo::kMaxCount; ++i) {
            // TODO(crbug.com/910276): Skip alpha plane until supported in
            // cache.
            if (i != SkYUVAIndex::kA_Index) {
              SkImage* plane_image =
                  cache->GetUploadedPlaneForTesting(draw_image, i).get();
              discardable_manager_.ExpectLocked(
                  GpuImageDecodeCache::GlIdFromSkImage(plane_image));
            }
          }
        } else {
          discardable_manager_.ExpectLocked(
              GpuImageDecodeCache::GlIdFromSkImage(
                  draw_and_decoded_draw_image.decoded_image.image().get()));
        }
      }
      cache->DrawWithImageFinished(draw_and_decoded_draw_image.image,
                                   draw_and_decoded_draw_image.decoded_image);
      cache->UnrefImage(draw_and_decoded_draw_image.image);
    }
  }
}

TEST_P(GpuImageDecodeCacheTest,
       OriginalYUVDecodeScaledDrawCorrectlyMipsPlanes) {
  // This test creates an image that will be YUV decoded and drawn at 80% scale.
  // Because the final size is between mip levels, we expect the image to be
  // decoded and uploaded at original size (mip level 0 for all planes) but to
  // have mips attached since kMedium_SkFilterQuality uses bilinear filtering
  // between mip levels.
  if (!do_yuv_decode_) {
    // The YUV case may choose different mip levels between chroma and luma
    // planes.
    return;
  }
  auto cache = CreateCache();
  SkFilterQuality filter_quality = kMedium_SkFilterQuality;
  SkSize requires_decode_at_original_scale = SkSize::Make(0.8f, 0.8f);

  PaintImage image = CreatePaintImageInternal(GetNormalImageSize());
  DrawImage draw_image(image, SkIRect::MakeWH(image.width(), image.height()),
                       filter_quality,
                       CreateMatrix(requires_decode_at_original_scale),
                       PaintImage::kDefaultFrameIndex, DefaultColorSpace());
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  // Pull out transfer cache ID from the DecodedDrawImage while it still has
  // it attached.
  DecodedDrawImage serialized_decoded_draw_image =
      cache->GetDecodedImageForDraw(draw_image);
  const base::Optional<uint32_t> transfer_cache_entry_id =
      serialized_decoded_draw_image.transfer_cache_entry_id();
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(std::move(serialized_decoded_draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.image()->isTextureBacked());

  // Skia will flatten a YUV SkImage upon calling makeTextureImage. Thus, we
  // must separately request mips for each plane and compare to the original
  // uploaded planes.
  CompareAllPlanesToMippedVersions(cache.get(), draw_image,
                                   transfer_cache_entry_id,
                                   true /* should_have_mips */);
  SkYUVASizeInfo yuv_size_info = GetYUV420SizeInfo(GetNormalImageSize());
  VerifyUploadedPlaneSizes(cache.get(), draw_image, transfer_cache_entry_id,
                           yuv_size_info.fSizes);

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, ScaledYUVDecodeScaledDrawCorrectlyMipsPlanes) {
  // This test creates an image that will be YUV decoded and drawn at 45% scale.
  // Because the final size is between mip levels, we expect the image to be
  // decoded and uploaded at half its original size (mip level 1 for Y plane but
  // level 0 for chroma planes) and to have mips attached since
  // kMedium_SkFilterQuality uses bilinear filtering between mip levels.
  if (!do_yuv_decode_) {
    // The YUV case may choose different mip levels between chroma and luma
    // planes.
    return;
  }
  auto cache = CreateCache();
  SkFilterQuality filter_quality = kMedium_SkFilterQuality;
  SkSize less_than_half_scale = SkSize::Make(0.45f, 0.45f);

  gfx::Size image_size = GetNormalImageSize();
  PaintImage image = CreatePaintImageInternal(image_size);
  DrawImage draw_image(image, SkIRect::MakeWH(image.width(), image.height()),
                       filter_quality, CreateMatrix(less_than_half_scale),
                       PaintImage::kDefaultFrameIndex, DefaultColorSpace());
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  EXPECT_TRUE(result.task);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  // Pull out transfer cache ID from the DecodedDrawImage while it still has
  // it attached.
  DecodedDrawImage serialized_decoded_draw_image =
      cache->GetDecodedImageForDraw(draw_image);
  const base::Optional<uint32_t> transfer_cache_entry_id =
      serialized_decoded_draw_image.transfer_cache_entry_id();
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(std::move(serialized_decoded_draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.image()->isTextureBacked());

  // Skia will flatten a YUV SkImage upon calling makeTextureImage. Thus, we
  // must separately request mips for each plane and compare to the original
  // uploaded planes.
  CompareAllPlanesToMippedVersions(cache.get(), draw_image,
                                   transfer_cache_entry_id,
                                   true /* should_have_mips */);

  // Because we intend to draw this image at 0.45 x 0.45 scale, we will upload
  // the Y plane at mip level 1 (corresponding to half the original size). The
  // chroma planes (U and V) should be uploaded at the same size as the Y plane,
  // corresponding to mip level 0, because the largest dimensions greater than
  // or equal to target dimensions for them is their original size.
  SkISize mipped_plane_sizes[SkYUVASizeInfo::kMaxCount];
  mipped_plane_sizes[SkYUVAIndex::kY_Index] = SkISize::Make(
      (image_size.width() + 1) / 2, (image_size.height() + 1) / 2);
  mipped_plane_sizes[SkYUVAIndex::kU_Index] =
      mipped_plane_sizes[SkYUVAIndex::kY_Index];
  mipped_plane_sizes[SkYUVAIndex::kV_Index] =
      mipped_plane_sizes[SkYUVAIndex::kY_Index];
  VerifyUploadedPlaneSizes(cache.get(), draw_image, transfer_cache_entry_id,
                           mipped_plane_sizes);

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheTest, GetBorderlineLargeDecodedImageForDraw) {
  // We will create a texture that's at the maximum size the GPU says it can
  // support for uploads.
  auto cache = CreateCache();

  PaintImage almost_too_large_image =
      CreatePaintImageInternal(gfx::Size(max_texture_size_, max_texture_size_));
  DrawImage draw_image = CreateDrawImageInternal(almost_too_large_image);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());

  EXPECT_TRUE(result.need_unref);
  ASSERT_TRUE(result.task);
  ASSERT_EQ(result.task->dependencies().size(), 1u);
  ASSERT_TRUE(result.task->dependencies()[0]);

  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  DecodedDrawImage decoded_draw_image =
      EnsureImageBacked(cache->GetDecodedImageForDraw(draw_image));
  EXPECT_TRUE(decoded_draw_image.image());
  EXPECT_TRUE(decoded_draw_image.image()->isTextureBacked());
  EXPECT_TRUE(decoded_draw_image.is_budgeted());
  EXPECT_FALSE(cache->DiscardableIsLockedForTesting(draw_image));

  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
}

SkColorType test_color_types[] = {kN32_SkColorType, kARGB_4444_SkColorType,
                                  kRGBA_F16_SkColorType};
bool false_array[] = {false};
bool true_array[] = {true};

INSTANTIATE_TEST_SUITE_P(
    GpuImageDecodeCacheTestsInProcessRaster,
    GpuImageDecodeCacheTest,
    testing::Combine(
        testing::ValuesIn(test_color_types),
        testing::ValuesIn(false_array) /* use_transfer_cache */,
        testing::Bool() /* do_yuv_decode */,
        testing::ValuesIn(false_array) /* allow_accelerated_jpeg_decoding */,
        testing::ValuesIn(false_array) /* allow_accelerated_webp_decoding */,
        testing::ValuesIn(false_array) /* advertise_accelerated_decoding */));

INSTANTIATE_TEST_SUITE_P(
    GpuImageDecodeCacheTestsOOPR,
    GpuImageDecodeCacheTest,
    testing::Combine(
        testing::ValuesIn(test_color_types),
        testing::ValuesIn(true_array) /* use_transfer_cache */,
        testing::Bool() /* do_yuv_decode */,
        testing::ValuesIn(false_array) /* allow_accelerated_jpeg_decoding */,
        testing::ValuesIn(false_array) /* allow_accelerated_webp_decoding */,
        testing::ValuesIn(false_array) /* advertise_accelerated_decoding */));

class GpuImageDecodeCacheWithAcceleratedDecodesTest
    : public GpuImageDecodeCacheTest {
 public:
  PaintImage CreatePaintImageForDecodeAcceleration(
      ImageType image_type = ImageType::kJPEG,
      YUVSubsampling yuv_subsampling = YUVSubsampling::k420) {
    // Create a valid image metadata for hardware acceleration.
    ImageHeaderMetadata image_data{};
    image_data.image_size = gfx::Size(123, 45);
    image_data.image_type = image_type;
    image_data.yuv_subsampling = yuv_subsampling;
    image_data.all_data_received_prior_to_decode = true;
    image_data.has_embedded_color_profile = false;
    image_data.jpeg_is_progressive = false;
    image_data.webp_is_non_extended_lossy = true;

    SkImageInfo info = SkImageInfo::Make(
        image_data.image_size.width(), image_data.image_size.height(),
        color_type_, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    sk_sp<FakePaintImageGenerator> generator;
    if (do_yuv_decode_) {
      generator = sk_make_sp<FakePaintImageGenerator>(
          info, GetYUV420SizeInfo(image_data.image_size));
    } else {
      generator = sk_make_sp<FakePaintImageGenerator>(info);
    }
    generator->SetImageHeaderMetadata(image_data);
    PaintImage image = PaintImageBuilder::WithDefault()
                           .set_id(PaintImage::GetNextId())
                           .set_paint_image_generator(generator)
                           .TakePaintImage();
    return image;
  }

  StrictMock<MockRasterImplementation>* raster_implementation() const {
    return static_cast<StrictMock<MockRasterImplementation>*>(
        context_provider_->RasterInterface());
  }
};

TEST_P(GpuImageDecodeCacheWithAcceleratedDecodesTest,
       RequestAcceleratedDecodeSuccessfully) {
  std::vector<std::pair<YUVSubsampling, size_t>>
      subsamplings_and_expected_data_sizes{{YUVSubsampling::k420, 8387u},
                                           {YUVSubsampling::k422, 11115u},
                                           {YUVSubsampling::k444, 16605u}};
  for (const auto& subsampling_and_expected_data_size :
       subsamplings_and_expected_data_sizes) {
    auto cache = CreateCache();
    const gfx::ColorSpace target_color_space = gfx::ColorSpace::CreateSRGB();
    ASSERT_TRUE(target_color_space.IsValid());
    const PaintImage image = CreatePaintImageForDecodeAcceleration(
        ImageType::kJPEG, subsampling_and_expected_data_size.first);
    const SkFilterQuality quality = kHigh_SkFilterQuality;
    DrawImage draw_image(image, SkIRect::MakeWH(image.width(), image.height()),
                         quality, CreateMatrix(SkSize::Make(0.75f, 0.75f)),
                         PaintImage::kDefaultFrameIndex, target_color_space);
    ImageDecodeCache::TaskResult result = cache->GetTaskForImageAndRef(
        draw_image, ImageDecodeCache::TracingInfo());
    EXPECT_TRUE(result.need_unref);
    ASSERT_TRUE(result.task);
    EXPECT_TRUE(result.can_do_hardware_accelerated_decode);
    EXPECT_EQ(cache->GetWorkingSetBytesForTesting(),
              subsampling_and_expected_data_size.second);

    // Accelerated decodes should not produce decode tasks.
    ASSERT_TRUE(result.task->dependencies().empty());
    ASSERT_TRUE(image.GetImageHeaderMetadata());
    EXPECT_CALL(
        *raster_implementation(),
        DoScheduleImageDecode(image.GetImageHeaderMetadata()->image_size, _,
                              gfx::ColorSpace(), _))
        .Times(1);
    TestTileTaskRunner::ProcessTask(result.task.get());

    // Must hold context lock before calling GetDecodedImageForDraw /
    // DrawWithImageFinished.
    viz::ContextProvider::ScopedContextLock context_lock(context_provider());
    const DecodedDrawImage decoded_draw_image =
        cache->GetDecodedImageForDraw(draw_image);
    EXPECT_TRUE(decoded_draw_image.transfer_cache_entry_id().has_value());
    cache->DrawWithImageFinished(draw_image, decoded_draw_image);
    cache->UnrefImage(draw_image);
  }
}

TEST_P(GpuImageDecodeCacheWithAcceleratedDecodesTest,
       RequestAcceleratedDecodeSuccessfullyWithColorSpaceConversion) {
  auto cache = CreateCache();
  const gfx::ColorSpace target_color_space = gfx::ColorSpace::CreateXYZD50();
  ASSERT_TRUE(target_color_space.IsValid());
  const PaintImage image = CreatePaintImageForDecodeAcceleration();
  const SkFilterQuality quality = kHigh_SkFilterQuality;
  DrawImage draw_image(image, SkIRect::MakeWH(image.width(), image.height()),
                       quality, CreateMatrix(SkSize::Make(0.75f, 0.75f)),
                       PaintImage::kDefaultFrameIndex, target_color_space);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  ASSERT_TRUE(result.task);
  EXPECT_TRUE(result.can_do_hardware_accelerated_decode);

  // Accelerated decodes should not produce decode tasks.
  ASSERT_TRUE(result.task->dependencies().empty());
  ASSERT_TRUE(image.GetImageHeaderMetadata());
  EXPECT_CALL(*raster_implementation(),
              DoScheduleImageDecode(
                  image.GetImageHeaderMetadata()->image_size, _,
                  cache->SupportsColorSpaceConversion() ? target_color_space
                                                        : gfx::ColorSpace(),
                  _))
      .Times(1);
  TestTileTaskRunner::ProcessTask(result.task.get());

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  const DecodedDrawImage decoded_draw_image =
      cache->GetDecodedImageForDraw(draw_image);
  EXPECT_TRUE(decoded_draw_image.transfer_cache_entry_id().has_value());
  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheWithAcceleratedDecodesTest,
       AcceleratedDecodeRequestFails) {
  auto cache = CreateCache();
  const gfx::ColorSpace target_color_space = gfx::ColorSpace::CreateXYZD50();
  ASSERT_TRUE(target_color_space.IsValid());
  const PaintImage image = CreatePaintImageForDecodeAcceleration();
  const SkFilterQuality quality = kHigh_SkFilterQuality;
  DrawImage draw_image(image, SkIRect::MakeWH(image.width(), image.height()),
                       quality, CreateMatrix(SkSize::Make(0.75f, 0.75f)),
                       PaintImage::kDefaultFrameIndex, target_color_space);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  ASSERT_TRUE(result.task);
  EXPECT_TRUE(result.can_do_hardware_accelerated_decode);

  // Accelerated decodes should not produce decode tasks.
  ASSERT_TRUE(result.task->dependencies().empty());
  raster_implementation()->SetAcceleratedDecodingFailed();
  ASSERT_TRUE(image.GetImageHeaderMetadata());
  EXPECT_CALL(*raster_implementation(),
              DoScheduleImageDecode(
                  image.GetImageHeaderMetadata()->image_size, _,
                  cache->SupportsColorSpaceConversion() ? target_color_space
                                                        : gfx::ColorSpace(),
                  _))
      .Times(1);
  TestTileTaskRunner::ProcessTask(result.task.get());

  // Attempting to get another task for the image should result in no task
  // because the decode is considered to have failed before.
  ImageDecodeCache::TaskResult result_after_run =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_FALSE(result_after_run.need_unref);
  EXPECT_FALSE(result_after_run.task);
  EXPECT_TRUE(result_after_run.can_do_hardware_accelerated_decode);

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  const DecodedDrawImage decoded_draw_image =
      cache->GetDecodedImageForDraw(draw_image);
  EXPECT_FALSE(decoded_draw_image.transfer_cache_entry_id().has_value());
  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheWithAcceleratedDecodesTest,
       CannotRequestAcceleratedDecodeBecauseOfStandAloneDecode) {
  auto cache = CreateCache();
  const gfx::ColorSpace target_color_space = gfx::ColorSpace::CreateSRGB();
  ASSERT_TRUE(target_color_space.IsValid());
  const PaintImage image = CreatePaintImageForDecodeAcceleration();
  const SkFilterQuality quality = kHigh_SkFilterQuality;
  DrawImage draw_image(image, SkIRect::MakeWH(image.width(), image.height()),
                       quality, CreateMatrix(SkSize::Make(1.0f, 1.0f)),
                       PaintImage::kDefaultFrameIndex, target_color_space);
  ImageDecodeCache::TaskResult result =
      cache->GetOutOfRasterDecodeTaskForImageAndRef(draw_image);
  EXPECT_TRUE(result.need_unref);
  ASSERT_TRUE(result.task);
  EXPECT_FALSE(result.can_do_hardware_accelerated_decode);

  // A non-accelerated standalone decode should produce only a decode task.
  ASSERT_TRUE(result.task->dependencies().empty());
  TestTileTaskRunner::ProcessTask(result.task.get());
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheWithAcceleratedDecodesTest,
       CannotRequestAcceleratedDecodeBecauseOfNonZeroUploadMipLevel) {
  auto cache = CreateCache();
  const gfx::ColorSpace target_color_space = gfx::ColorSpace::CreateSRGB();
  ASSERT_TRUE(target_color_space.IsValid());
  const PaintImage image = CreatePaintImageForDecodeAcceleration();
  const SkFilterQuality quality = kHigh_SkFilterQuality;
  DrawImage draw_image(image, SkIRect::MakeWH(image.width(), image.height()),
                       quality, CreateMatrix(SkSize::Make(0.5f, 0.5f)),
                       PaintImage::kDefaultFrameIndex, target_color_space);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  ASSERT_TRUE(result.task);
  EXPECT_FALSE(result.can_do_hardware_accelerated_decode);

  // A non-accelerated normal decode should produce a decode dependency.
  ASSERT_EQ(result.task->dependencies().size(), 1u);
  ASSERT_TRUE(result.task->dependencies()[0]);
  TestTileTaskRunner::ProcessTask(result.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(result.task.get());
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheWithAcceleratedDecodesTest,
       RequestAcceleratedDecodeSuccessfullyAfterCancellation) {
  auto cache = CreateCache();
  const gfx::ColorSpace target_color_space = gfx::ColorSpace::CreateSRGB();
  ASSERT_TRUE(target_color_space.IsValid());
  const PaintImage image = CreatePaintImageForDecodeAcceleration();
  const SkFilterQuality quality = kHigh_SkFilterQuality;
  DrawImage draw_image(image, SkIRect::MakeWH(image.width(), image.height()),
                       quality, CreateMatrix(SkSize::Make(0.75f, 0.75f)),
                       PaintImage::kDefaultFrameIndex, target_color_space);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(result.need_unref);
  ASSERT_TRUE(result.task);
  EXPECT_TRUE(result.can_do_hardware_accelerated_decode);

  // Accelerated decodes should not produce decode tasks.
  ASSERT_TRUE(result.task->dependencies().empty());

  // Cancel the upload.
  TestTileTaskRunner::CancelTask(result.task.get());
  TestTileTaskRunner::CompleteTask(result.task.get());

  // Get the image again - we should have an upload task.
  ImageDecodeCache::TaskResult another_result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(another_result.need_unref);
  ASSERT_TRUE(another_result.task);
  EXPECT_TRUE(another_result.can_do_hardware_accelerated_decode);
  EXPECT_EQ(another_result.task->dependencies().size(), 0u);
  ASSERT_TRUE(image.GetImageHeaderMetadata());
  EXPECT_CALL(*raster_implementation(),
              DoScheduleImageDecode(image.GetImageHeaderMetadata()->image_size,
                                    _, gfx::ColorSpace(), _))
      .Times(1);
  TestTileTaskRunner::ProcessTask(another_result.task.get());

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  const DecodedDrawImage decoded_draw_image =
      cache->GetDecodedImageForDraw(draw_image);
  EXPECT_TRUE(decoded_draw_image.transfer_cache_entry_id().has_value());
  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
  cache->UnrefImage(draw_image);
  cache->UnrefImage(draw_image);
}

TEST_P(GpuImageDecodeCacheWithAcceleratedDecodesTest,
       RequestAcceleratedDecodeSuccessfullyAtRasterTime) {
  // We force at-raster decodes by setting the cache memory limit to 0 bytes.
  auto cache = CreateCache(0u /* memory_limit_bytes */);
  const gfx::ColorSpace target_color_space = gfx::ColorSpace::CreateSRGB();
  ASSERT_TRUE(target_color_space.IsValid());
  const PaintImage image = CreatePaintImageForDecodeAcceleration();
  const SkFilterQuality quality = kHigh_SkFilterQuality;
  DrawImage draw_image(image, SkIRect::MakeWH(image.width(), image.height()),
                       quality, CreateMatrix(SkSize::Make(0.75f, 0.75f)),
                       PaintImage::kDefaultFrameIndex, target_color_space);
  ImageDecodeCache::TaskResult result =
      cache->GetTaskForImageAndRef(draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_FALSE(result.need_unref);
  EXPECT_FALSE(result.task);
  EXPECT_TRUE(result.is_at_raster_decode);
  EXPECT_TRUE(result.can_do_hardware_accelerated_decode);

  // Must hold context lock before calling GetDecodedImageForDraw /
  // DrawWithImageFinished.
  EXPECT_CALL(*raster_implementation(),
              DoScheduleImageDecode(image.GetImageHeaderMetadata()->image_size,
                                    _, gfx::ColorSpace(), _))
      .Times(1);
  viz::ContextProvider::ScopedContextLock context_lock(context_provider());
  const DecodedDrawImage decoded_draw_image =
      cache->GetDecodedImageForDraw(draw_image);
  EXPECT_TRUE(decoded_draw_image.transfer_cache_entry_id().has_value());
  cache->DrawWithImageFinished(draw_image, decoded_draw_image);
}

INSTANTIATE_TEST_SUITE_P(
    GpuImageDecodeCacheTestsOOPR,
    GpuImageDecodeCacheWithAcceleratedDecodesTest,
    testing::Combine(
        testing::ValuesIn(test_color_types),
        testing::ValuesIn(true_array) /* use_transfer_cache */,
        testing::Bool() /* do_yuv_decode */,
        testing::ValuesIn(true_array) /* allow_accelerated_jpeg_decoding */,
        testing::ValuesIn(true_array) /* allow_accelerated_webp_decoding */,
        testing::ValuesIn(true_array) /* advertise_accelerated_decoding */));

class GpuImageDecodeCacheWithAcceleratedDecodesFlagsTest
    : public GpuImageDecodeCacheWithAcceleratedDecodesTest {};

TEST_P(GpuImageDecodeCacheWithAcceleratedDecodesFlagsTest,
       RequestAcceleratedDecodeSuccessfully) {
  auto cache = CreateCache();
  const SkFilterQuality quality = kHigh_SkFilterQuality;
  const gfx::ColorSpace target_color_space = gfx::ColorSpace::CreateSRGB();
  ASSERT_TRUE(target_color_space.IsValid());

  // Try a JPEG image.
  const PaintImage jpeg_image =
      CreatePaintImageForDecodeAcceleration(ImageType::kJPEG);
  DrawImage jpeg_draw_image(
      jpeg_image, SkIRect::MakeWH(jpeg_image.width(), jpeg_image.height()),
      quality, CreateMatrix(SkSize::Make(0.75f, 0.75f)),
      PaintImage::kDefaultFrameIndex, target_color_space);
  ImageDecodeCache::TaskResult jpeg_task = cache->GetTaskForImageAndRef(
      jpeg_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(jpeg_task.need_unref);
  ASSERT_TRUE(jpeg_task.task);
  // If the hardware decoder claims support for the image (i.e.,
  // |advertise_accelerated_decoding_| is true) and the feature flag for the
  // image type is on (i.e., |allow_accelerated_jpeg_decoding_| is true), we
  // should expect hardware acceleration. In that path, there is only an upload
  // task without a decode dependency since the decode will be done in the GPU
  // process. In the alternative path (software decoding), the upload task
  // depends on a decode task that runs in the renderer.
  EXPECT_EQ(advertise_accelerated_decoding_,
            jpeg_task.can_do_hardware_accelerated_decode);
  if (advertise_accelerated_decoding_ && allow_accelerated_jpeg_decoding_) {
    ASSERT_TRUE(jpeg_task.task->dependencies().empty());
    ASSERT_TRUE(jpeg_image.GetImageHeaderMetadata());
    EXPECT_CALL(
        *raster_implementation(),
        DoScheduleImageDecode(jpeg_image.GetImageHeaderMetadata()->image_size,
                              _, gfx::ColorSpace(), _))
        .Times(1);
  } else {
    ASSERT_EQ(jpeg_task.task->dependencies().size(), 1u);
    ASSERT_TRUE(jpeg_task.task->dependencies()[0]);
    TestTileTaskRunner::ProcessTask(jpeg_task.task->dependencies()[0].get());
  }
  TestTileTaskRunner::ScheduleTask(jpeg_task.task.get());

  // After scheduling the task, trying to get another task for the image should
  // result in the original task.
  ImageDecodeCache::TaskResult jpeg_task_again = cache->GetTaskForImageAndRef(
      jpeg_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(jpeg_task_again.need_unref);
  EXPECT_EQ(jpeg_task_again.task.get(), jpeg_task.task.get());
  EXPECT_EQ(advertise_accelerated_decoding_,
            jpeg_task_again.can_do_hardware_accelerated_decode);

  TestTileTaskRunner::RunTask(jpeg_task.task.get());
  TestTileTaskRunner::CompleteTask(jpeg_task.task.get());
  testing::Mock::VerifyAndClearExpectations(raster_implementation());

  // After running the tasks, trying to get another task for the image should
  // result in no task.
  jpeg_task = cache->GetTaskForImageAndRef(jpeg_draw_image,
                                           ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(jpeg_task.need_unref);
  EXPECT_FALSE(jpeg_task.task);
  EXPECT_EQ(advertise_accelerated_decoding_,
            jpeg_task.can_do_hardware_accelerated_decode);
  cache->UnrefImage(jpeg_draw_image);
  cache->UnrefImage(jpeg_draw_image);
  cache->UnrefImage(jpeg_draw_image);

  // Try a WebP image.
  const PaintImage webp_image =
      CreatePaintImageForDecodeAcceleration(ImageType::kWEBP);
  DrawImage webp_draw_image(
      webp_image, SkIRect::MakeWH(webp_image.width(), webp_image.height()),
      quality, CreateMatrix(SkSize::Make(0.75f, 0.75f)),
      PaintImage::kDefaultFrameIndex, target_color_space);
  ImageDecodeCache::TaskResult webp_task = cache->GetTaskForImageAndRef(
      webp_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(webp_task.need_unref);
  ASSERT_TRUE(webp_task.task);
  EXPECT_EQ(advertise_accelerated_decoding_,
            webp_task.can_do_hardware_accelerated_decode);
  if (advertise_accelerated_decoding_ && allow_accelerated_webp_decoding_) {
    ASSERT_TRUE(webp_task.task->dependencies().empty());
    ASSERT_TRUE(webp_image.GetImageHeaderMetadata());
    EXPECT_CALL(
        *raster_implementation(),
        DoScheduleImageDecode(webp_image.GetImageHeaderMetadata()->image_size,
                              _, gfx::ColorSpace(), _))
        .Times(1);
  } else {
    ASSERT_EQ(webp_task.task->dependencies().size(), 1u);
    ASSERT_TRUE(webp_task.task->dependencies()[0]);
    TestTileTaskRunner::ProcessTask(webp_task.task->dependencies()[0].get());
  }
  TestTileTaskRunner::ProcessTask(webp_task.task.get());
  testing::Mock::VerifyAndClearExpectations(raster_implementation());

  // The image should have been cached.
  webp_task = cache->GetTaskForImageAndRef(webp_draw_image,
                                           ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(webp_task.need_unref);
  EXPECT_FALSE(webp_task.task);
  EXPECT_EQ(advertise_accelerated_decoding_,
            webp_task.can_do_hardware_accelerated_decode);
  cache->UnrefImage(webp_draw_image);
  cache->UnrefImage(webp_draw_image);

  // Try a PNG image (which should not be hardware accelerated).
  const PaintImage png_image =
      CreatePaintImageForDecodeAcceleration(ImageType::kPNG);
  DrawImage png_draw_image(
      png_image, SkIRect::MakeWH(jpeg_image.width(), jpeg_image.height()),
      quality, CreateMatrix(SkSize::Make(0.75f, 0.75f)),
      PaintImage::kDefaultFrameIndex, target_color_space);
  ImageDecodeCache::TaskResult png_task = cache->GetTaskForImageAndRef(
      png_draw_image, ImageDecodeCache::TracingInfo());
  EXPECT_TRUE(png_task.need_unref);
  ASSERT_TRUE(png_task.task);
  EXPECT_FALSE(png_task.can_do_hardware_accelerated_decode);
  ASSERT_EQ(png_task.task->dependencies().size(), 1u);
  ASSERT_TRUE(png_task.task->dependencies()[0]);
  TestTileTaskRunner::ProcessTask(png_task.task->dependencies()[0].get());
  TestTileTaskRunner::ProcessTask(png_task.task.get());
  cache->UnrefImage(png_draw_image);
}

INSTANTIATE_TEST_SUITE_P(
    GpuImageDecodeCacheTestsOOPR,
    GpuImageDecodeCacheWithAcceleratedDecodesFlagsTest,
    testing::Combine(testing::Values(kN32_SkColorType),
                     testing::ValuesIn(true_array) /* use_transfer_cache */,
                     testing::Bool() /* do_yuv_decode */,
                     testing::Bool() /* allow_accelerated_jpeg_decoding */,
                     testing::Bool() /* allow_accelerated_webp_decoding */,
                     testing::Bool() /* advertise_accelerated_decoding */));

#undef EXPECT_TRUE_IF_NOT_USING_TRANSFER_CACHE
#undef EXPECT_FALSE_IF_NOT_USING_TRANSFER_CACHE

}  // namespace
}  // namespace cc
