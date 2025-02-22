// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/optional.h"
#include "media/gpu/chromeos/fourcc.h"

#include "base/logging.h"
#include "media/gpu/buildflags.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(USE_V4L2_CODEC)
#include <linux/videodev2.h>
#endif  // BUILDFLAG(USE_V4L2_CODEC)
#if BUILDFLAG(USE_VAAPI)
#include <va/va.h>
#endif  // BUILDFLAG(USE_VAAPI)

namespace media {

#if BUILDFLAG(USE_V4L2_CODEC)
// Checks that converting a V4L2 pixel format to Fourcc and back to V4L2
// yields the same format as the original one.
static void CheckFromV4L2PixFmtAndBack(uint32_t fmt) {
  base::Optional<Fourcc> fourcc = Fourcc::FromV4L2PixFmt(fmt);
  EXPECT_NE(fourcc, base::nullopt);
  EXPECT_EQ(fourcc->ToV4L2PixFmt(), fmt);
}

TEST(FourccTest, V4L2PixFmtToV4L2PixFmt) {
  // Temporary defined in v4l2/v4l2_device.h
  static constexpr uint32_t V4L2_MM21 = ComposeFourcc('M', 'M', '2', '1');

  CheckFromV4L2PixFmtAndBack(V4L2_PIX_FMT_ABGR32);
#ifdef V4L2_PIX_FMT_RGBA32
  V4L2PixFmtIsEqual(V4L2_PIX_FMT_RGBA32);
#endif
  CheckFromV4L2PixFmtAndBack(V4L2_PIX_FMT_XBGR32);
#ifdef V4L2_PIX_FMT_RGBX32
  V4L2PixFmtIsEqual(V4L2_PIX_FMT_RGBX32);
#endif
  CheckFromV4L2PixFmtAndBack(V4L2_PIX_FMT_RGB32);
  CheckFromV4L2PixFmtAndBack(V4L2_PIX_FMT_YUV420);
  CheckFromV4L2PixFmtAndBack(V4L2_PIX_FMT_YVU420);
  CheckFromV4L2PixFmtAndBack(V4L2_PIX_FMT_YUV420M);
  CheckFromV4L2PixFmtAndBack(V4L2_PIX_FMT_YVU420M);
  CheckFromV4L2PixFmtAndBack(V4L2_PIX_FMT_YUYV);
  CheckFromV4L2PixFmtAndBack(V4L2_PIX_FMT_NV12);
  CheckFromV4L2PixFmtAndBack(V4L2_PIX_FMT_NV21);
  CheckFromV4L2PixFmtAndBack(V4L2_PIX_FMT_NV12M);
  CheckFromV4L2PixFmtAndBack(V4L2_PIX_FMT_YUV422M);
  CheckFromV4L2PixFmtAndBack(V4L2_PIX_FMT_MT21C);
  CheckFromV4L2PixFmtAndBack(V4L2_MM21);
}

TEST(FourccTest, V4L2PixFmtToVideoPixelFormat) {
  EXPECT_EQ(PIXEL_FORMAT_NV12,
            Fourcc::FromV4L2PixFmt(V4L2_PIX_FMT_NV12)->ToVideoPixelFormat());
  EXPECT_EQ(PIXEL_FORMAT_NV12,
            Fourcc::FromV4L2PixFmt(V4L2_PIX_FMT_NV12M)->ToVideoPixelFormat());

  EXPECT_EQ(PIXEL_FORMAT_NV12,
            Fourcc::FromV4L2PixFmt(V4L2_PIX_FMT_MT21C)->ToVideoPixelFormat());
  EXPECT_EQ(PIXEL_FORMAT_NV12,
            Fourcc::FromV4L2PixFmt(ComposeFourcc('M', 'M', '2', '1'))
                ->ToVideoPixelFormat());

  EXPECT_EQ(PIXEL_FORMAT_I420,
            Fourcc::FromV4L2PixFmt(V4L2_PIX_FMT_YUV420)->ToVideoPixelFormat());
  EXPECT_EQ(PIXEL_FORMAT_I420,
            Fourcc::FromV4L2PixFmt(V4L2_PIX_FMT_YUV420M)->ToVideoPixelFormat());

  EXPECT_EQ(PIXEL_FORMAT_YV12,
            Fourcc::FromV4L2PixFmt(V4L2_PIX_FMT_YVU420)->ToVideoPixelFormat());
  EXPECT_EQ(PIXEL_FORMAT_YV12,
            Fourcc::FromV4L2PixFmt(V4L2_PIX_FMT_YVU420M)->ToVideoPixelFormat());

  EXPECT_EQ(PIXEL_FORMAT_I422,
            Fourcc::FromV4L2PixFmt(V4L2_PIX_FMT_YUV422M)->ToVideoPixelFormat());

  // Noted that previously in V4L2Device::V4L2PixFmtToVideoPixelFormat(),
  // V4L2_PIX_FMT_RGB32 maps to PIXEL_FORMAT_ARGB. However, the mapping was
  // wrong. It should be mapped to PIXEL_FORMAT_BGRA.
  EXPECT_EQ(PIXEL_FORMAT_BGRA,
            Fourcc::FromV4L2PixFmt(V4L2_PIX_FMT_RGB32)->ToVideoPixelFormat());

  // Randomly pick an unmapped v4l2 fourcc.
  EXPECT_EQ(base::nullopt, Fourcc::FromV4L2PixFmt(V4L2_PIX_FMT_Z16));
}

TEST(FourccTest, VideoPixelFormatToV4L2PixFmt) {
  EXPECT_EQ(
      V4L2_PIX_FMT_NV12,
      Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_NV12, true)->ToV4L2PixFmt());
  EXPECT_EQ(
      V4L2_PIX_FMT_NV12M,
      Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_NV12, false)->ToV4L2PixFmt());

  EXPECT_EQ(
      V4L2_PIX_FMT_YUV420,
      Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_I420, true)->ToV4L2PixFmt());
  EXPECT_EQ(
      V4L2_PIX_FMT_YUV420M,
      Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_I420, false)->ToV4L2PixFmt());

  EXPECT_EQ(
      V4L2_PIX_FMT_YVU420,
      Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_YV12, true)->ToV4L2PixFmt());
  EXPECT_EQ(
      V4L2_PIX_FMT_YVU420M,
      Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_YV12, false)->ToV4L2PixFmt());
}
#endif  // BUILDFLAG(USE_V4L2_CODEC)

#if BUILDFLAG(USE_VAAPI)
// Checks that converting a VaFourCC to Fourcc and back to VaFourCC
// yields the same format as the original one.
static void CheckFromVAFourCCAndBack(uint32_t va_fourcc) {
  base::Optional<Fourcc> fourcc = Fourcc::FromVAFourCC(va_fourcc);
  EXPECT_NE(fourcc, base::nullopt);
  base::Optional<uint32_t> to_va_fourcc = fourcc->ToVAFourCC();
  EXPECT_NE(to_va_fourcc, base::nullopt);
  EXPECT_EQ(*to_va_fourcc, va_fourcc);
}

TEST(FourccTest, FromVaFourCCAndBack) {
  CheckFromVAFourCCAndBack(VA_FOURCC_I420);
  CheckFromVAFourCCAndBack(VA_FOURCC_NV12);
  CheckFromVAFourCCAndBack(VA_FOURCC_NV21);
  CheckFromVAFourCCAndBack(VA_FOURCC_YV12);
  CheckFromVAFourCCAndBack(VA_FOURCC_YUY2);
  CheckFromVAFourCCAndBack(VA_FOURCC_RGBA);
  CheckFromVAFourCCAndBack(VA_FOURCC_RGBX);
  CheckFromVAFourCCAndBack(VA_FOURCC_BGRA);
  CheckFromVAFourCCAndBack(VA_FOURCC_BGRX);
  CheckFromVAFourCCAndBack(VA_FOURCC_ARGB);
}

TEST(FourccTest, VAFourCCToVideoPixelFormat) {
  EXPECT_EQ(PIXEL_FORMAT_I420,
            Fourcc::FromVAFourCC(VA_FOURCC_I420)->ToVideoPixelFormat());
  EXPECT_EQ(PIXEL_FORMAT_NV12,
            Fourcc::FromVAFourCC(VA_FOURCC_NV12)->ToVideoPixelFormat());
  EXPECT_EQ(PIXEL_FORMAT_NV21,
            Fourcc::FromVAFourCC(VA_FOURCC_NV21)->ToVideoPixelFormat());
  EXPECT_EQ(PIXEL_FORMAT_YV12,
            Fourcc::FromVAFourCC(VA_FOURCC_YV12)->ToVideoPixelFormat());
  EXPECT_EQ(PIXEL_FORMAT_YUY2,
            Fourcc::FromVAFourCC(VA_FOURCC_YUY2)->ToVideoPixelFormat());
  EXPECT_EQ(PIXEL_FORMAT_ABGR,
            Fourcc::FromVAFourCC(VA_FOURCC_RGBA)->ToVideoPixelFormat());
  EXPECT_EQ(PIXEL_FORMAT_XBGR,
            Fourcc::FromVAFourCC(VA_FOURCC_RGBX)->ToVideoPixelFormat());
  EXPECT_EQ(PIXEL_FORMAT_ARGB,
            Fourcc::FromVAFourCC(VA_FOURCC_BGRA)->ToVideoPixelFormat());
  EXPECT_EQ(PIXEL_FORMAT_XRGB,
            Fourcc::FromVAFourCC(VA_FOURCC_BGRX)->ToVideoPixelFormat());
}

TEST(FourccTest, VideoPixelFormatToVAFourCC) {
  EXPECT_EQ(static_cast<uint32_t>(VA_FOURCC_I420),
            *Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_I420)->ToVAFourCC());
  EXPECT_EQ(static_cast<uint32_t>(VA_FOURCC_NV12),
            *Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_NV12)->ToVAFourCC());
  EXPECT_EQ(static_cast<uint32_t>(VA_FOURCC_NV21),
            *Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_NV21)->ToVAFourCC());
  EXPECT_EQ(static_cast<uint32_t>(VA_FOURCC_YV12),
            *Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_YV12)->ToVAFourCC());
  EXPECT_EQ(static_cast<uint32_t>(VA_FOURCC_YUY2),
            *Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_YUY2)->ToVAFourCC());
  EXPECT_EQ(static_cast<uint32_t>(VA_FOURCC_RGBA),
            *Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_ABGR)->ToVAFourCC());
  EXPECT_EQ(static_cast<uint32_t>(VA_FOURCC_RGBX),
            *Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_XBGR)->ToVAFourCC());
  EXPECT_EQ(static_cast<uint32_t>(VA_FOURCC_BGRA),
            *Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_ARGB)->ToVAFourCC());
  EXPECT_EQ(static_cast<uint32_t>(VA_FOURCC_BGRX),
            *Fourcc::FromVideoPixelFormat(PIXEL_FORMAT_XRGB)->ToVAFourCC());
}
#endif  // BUILDFLAG(USE_VAAPI)

}  // namespace media
