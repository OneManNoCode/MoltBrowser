// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "media/base/molt_portable_media.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "gpu/command_buffer/client/test_shared_image_interface.h"
#include "media/base/decoder_factory.h"
#include "media/base/media_util.h"
#include "media/base/molt_portable_scale.h"
#include "media/base/video_decoder.h"
#include "media/base/video_frame.h"
#include "media/base/video_frame_converter.h"
#include "media/gfx/mappable_shared_image_video_frame_pool.h"
#include "media/gfx/paint_canvas_video_renderer.h"
#include "media/renderers/default_decoder_factory.h"
#include "media/video/mock_gpu_video_accelerator_factories.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

// MoltBrowser's hook in third_party/opus/src/celt/celt.c.
extern "C" int opus_moltbrowser_portable(void);

namespace media {
namespace {

using ::testing::Return;

class MoltPortableMediaTest : public testing::Test {
 protected:
  ~MoltPortableMediaTest() override {
    SetPortableMediaProcessForTesting(false);
  }

  static scoped_refptr<VideoFrame> Frame(VideoPixelFormat format,
                                         const gfx::Size& size,
                                         uint32_t seed) {
    auto frame = VideoFrame::CreateFrame(format, size, gfx::Rect(size), size,
                                         base::TimeDelta());
    EXPECT_TRUE(frame);
    uint32_t s = seed;
    for (size_t plane = 0; plane < VideoFrame::NumPlanes(format); ++plane) {
      base::span<uint8_t> data = frame->GetWritableVisiblePlaneData(plane);
      const size_t rows = static_cast<size_t>(frame->GetVisibleRows(plane));
      const size_t row_bytes =
          static_cast<size_t>(frame->GetVisibleRowBytes(plane));
      for (size_t y = 0; y < rows; ++y) {
        for (size_t x = 0; x < row_bytes; ++x) {
          s = s * 1664525u + 1013904223u;
          // 16-bit samples stay below 1024 (10-bit content).
          data[y * frame->stride(plane) + x] =
              static_cast<uint8_t>(frame->BitDepth() > 8 && (x & 1)
                                       ? (s >> 24) & 3
                                       : s >> 24);
        }
      }
    }
    return frame;
  }

  // The plane scaled on its own with PortableScalePlane(), for comparison.
  static std::vector<uint8_t> ScaledPlane(const VideoFrame& src,
                                          const VideoFrame& dest,
                                          size_t plane) {
    const int channels = VideoFrame::BytesPerElement(src.format(), plane);
    const size_t row = static_cast<size_t>(dest.GetVisibleColumns(plane)) *
                       static_cast<size_t>(channels);
    std::vector<uint8_t> out(row *
                             static_cast<size_t>(dest.GetVisibleRows(plane)));
    EXPECT_TRUE(molt::PortableScalePlane(
        src.GetVisiblePlaneData(plane), src.stride(plane),
        src.GetVisibleColumns(plane), src.GetVisibleRows(plane), out, row,
        dest.GetVisibleColumns(plane), dest.GetVisibleRows(plane), channels));
    return out;
  }

  static std::vector<uint8_t> VisiblePlane(const VideoFrame& frame,
                                           size_t plane) {
    std::vector<uint8_t> out;
    const size_t row_bytes =
        static_cast<size_t>(frame.GetVisibleRowBytes(plane));
    base::span<const uint8_t> data = frame.GetVisiblePlaneData(plane);
    for (int y = 0; y < frame.GetVisibleRows(plane); ++y) {
      auto row = data.subspan(static_cast<size_t>(y) * frame.stride(plane),
                              row_bytes);
      out.insert(out.end(), row.begin(), row.end());
    }
    return out;
  }

  base::test::TaskEnvironment task_environment_;
};

TEST_F(MoltPortableMediaTest, TurnsOnTheProcessAndLibopus) {
  EXPECT_FALSE(IsPortableMediaProcess());
  EXPECT_EQ(opus_moltbrowser_portable(), 0);
  SetPortableMediaProcessForTesting(true);
  EXPECT_TRUE(IsPortableMediaProcess());
  EXPECT_EQ(opus_moltbrowser_portable(), 1);
  SetPortableMediaProcessForTesting(false);
  EXPECT_FALSE(IsPortableMediaProcess());
  EXPECT_EQ(opus_moltbrowser_portable(), 0);
}

TEST_F(MoltPortableMediaTest, PortableScaleFrameScalesEveryPlane) {
  for (VideoPixelFormat format :
       {PIXEL_FORMAT_I420, PIXEL_FORMAT_I420A, PIXEL_FORMAT_I444,
        PIXEL_FORMAT_NV12, PIXEL_FORMAT_ARGB, PIXEL_FORMAT_ABGR}) {
    SCOPED_TRACE(VideoPixelFormatToString(format));
    auto src = Frame(format, gfx::Size(96, 54), 7);
    auto dest = VideoFrame::CreateFrame(format, gfx::Size(40, 70),
                                        gfx::Rect(40, 70), gfx::Size(40, 70),
                                        base::TimeDelta());
    ASSERT_TRUE(PortableScaleFrame(*src, *dest));
    for (size_t plane = 0; plane < VideoFrame::NumPlanes(format); ++plane) {
      EXPECT_EQ(VisiblePlane(*dest, plane), ScaledPlane(*src, *dest, plane))
          << "plane " << plane;
    }
  }
}

TEST_F(MoltPortableMediaTest, PortableScaleFrameHighBitDepth) {
  auto src = Frame(PIXEL_FORMAT_YUV420P10, gfx::Size(64, 36), 9);
  auto dest = VideoFrame::CreateFrame(PIXEL_FORMAT_YUV420P10, gfx::Size(30, 18),
                                      gfx::Rect(30, 18), gfx::Size(30, 18),
                                      base::TimeDelta());
  ASSERT_TRUE(PortableScaleFrame(*src, *dest));
  for (size_t plane = 0; plane < 3; ++plane) {
    std::vector<uint16_t> expected(
        static_cast<size_t>(dest->GetVisibleColumns(plane)) *
        static_cast<size_t>(dest->GetVisibleRows(plane)));
    ASSERT_TRUE(molt::PortableScalePlane16(
        base::subtle::reinterpret_span<const uint16_t>(
            src->GetVisiblePlaneData(plane)),
        src->stride(plane) / 2, src->GetVisibleColumns(plane),
        src->GetVisibleRows(plane), expected,
        static_cast<size_t>(dest->GetVisibleColumns(plane)),
        dest->GetVisibleColumns(plane), dest->GetVisibleRows(plane), 1));
    std::vector<uint8_t> bytes;
    for (uint16_t v : expected) {
      bytes.push_back(static_cast<uint8_t>(v));
      bytes.push_back(static_cast<uint8_t>(v >> 8));
    }
    EXPECT_EQ(VisiblePlane(*dest, plane), bytes) << "plane " << plane;
  }
}

TEST_F(MoltPortableMediaTest, PortableScaleFrameRefusesMismatchedPlanes) {
  auto i420 = Frame(PIXEL_FORMAT_I420, gfx::Size(32, 32), 1);
  auto nv12 = VideoFrame::CreateFrame(PIXEL_FORMAT_NV12, gfx::Size(16, 16),
                                      gfx::Rect(16, 16), gfx::Size(16, 16),
                                      base::TimeDelta());
  auto p10 = VideoFrame::CreateFrame(PIXEL_FORMAT_YUV420P10, gfx::Size(16, 16),
                                     gfx::Rect(16, 16), gfx::Size(16, 16),
                                     base::TimeDelta());
  EXPECT_FALSE(PortableScaleFrame(*i420, *nv12));
  EXPECT_FALSE(PortableScaleFrame(*i420, *p10));
}

TEST_F(MoltPortableMediaTest, ConverterScalesPortablyOnlyInPortableMode) {
  auto src = Frame(PIXEL_FORMAT_I420, gfx::Size(128, 72), 3);
  auto expected = VideoFrame::CreateFrame(PIXEL_FORMAT_I420, gfx::Size(48, 30),
                                          gfx::Rect(48, 30), gfx::Size(48, 30),
                                          base::TimeDelta());
  ASSERT_TRUE(PortableScaleFrame(*src, *expected));

  VideoFrameConverter converter;
  auto portable = VideoFrame::CreateFrame(PIXEL_FORMAT_I420, gfx::Size(48, 30),
                                          gfx::Rect(48, 30), gfx::Size(48, 30),
                                          base::TimeDelta());
  SetPortableMediaProcessForTesting(true);
  ASSERT_TRUE(converter.ConvertAndScale(*src, *portable).is_ok());
  for (size_t plane = 0; plane < 3; ++plane) {
    EXPECT_EQ(VisiblePlane(*portable, plane), VisiblePlane(*expected, plane));
  }

  // Same size: a format conversion, libyuv's (identical on every CPU).
  auto same = VideoFrame::CreateFrame(PIXEL_FORMAT_I420, gfx::Size(128, 72),
                                      gfx::Rect(128, 72), gfx::Size(128, 72),
                                      base::TimeDelta());
  ASSERT_TRUE(converter.ConvertAndScale(*src, *same).is_ok());
  EXPECT_EQ(VisiblePlane(*same, 0), VisiblePlane(*src, 0));

  // RGB input is scaled before it is converted.
  auto argb = Frame(PIXEL_FORMAT_XRGB, gfx::Size(128, 72), 5);
  auto from_rgb = VideoFrame::CreateFrame(PIXEL_FORMAT_I420, gfx::Size(48, 30),
                                          gfx::Rect(48, 30), gfx::Size(48, 30),
                                          base::TimeDelta());
  EXPECT_TRUE(converter.ConvertAndScale(*argb, *from_rgb).is_ok());

  SetPortableMediaProcessForTesting(false);
  auto normal = VideoFrame::CreateFrame(PIXEL_FORMAT_I420, gfx::Size(48, 30),
                                        gfx::Rect(48, 30), gfx::Size(48, 30),
                                        base::TimeDelta());
  ASSERT_TRUE(converter.ConvertAndScale(*src, *normal).is_ok());
}

// An external decoder factory: it stands for the GPU's decoders.
class CountingDecoderFactory : public DecoderFactory {
 public:
  explicit CountingDecoderFactory(int* calls) : calls_(calls) {}
  void CreateVideoDecoders(
      scoped_refptr<base::SequencedTaskRunner> task_runner,
      GpuVideoAcceleratorFactories* gpu_factories,
      MediaLog* media_log,
      RequestOverlayInfoCB request_overlay_info_cb,
      const gfx::ColorSpace& target_color_space,
      std::vector<std::unique_ptr<VideoDecoder>>* video_decoders) override {
    ++*calls_;
  }

 private:
  raw_ptr<int> calls_;
};

TEST_F(MoltPortableMediaTest, NoHardwareVideoDecodersInPortableMode) {
  auto task_runner = base::SequencedTaskRunner::GetCurrentDefault();
  MockGpuVideoAcceleratorFactories gpu_factories(nullptr);
  EXPECT_CALL(gpu_factories, GetTaskRunner())
      .WillRepeatedly(Return(task_runner));
  NullMediaLog media_log;
  for (bool portable : {false, true}) {
    SCOPED_TRACE(portable);
    SetPortableMediaProcessForTesting(portable);
    int external_calls = 0;
    DefaultDecoderFactory factory(
        std::make_unique<CountingDecoderFactory>(&external_calls));
    std::vector<std::unique_ptr<VideoDecoder>> decoders;
    factory.CreateVideoDecoders(task_runner, &gpu_factories, &media_log,
                                RequestOverlayInfoCB(), gfx::ColorSpace(),
                                &decoders);
    EXPECT_EQ(external_calls, portable ? 0 : 1);
    EXPECT_FALSE(decoders.empty());  // the build's software decoders
  }
}

TEST_F(MoltPortableMediaTest, FramesStayInCpuMemoryInPortableMode) {
  auto sii = base::MakeRefCounted<gpu::TestSharedImageInterface>();
  MockGpuVideoAcceleratorFactories gpu_factories(sii.get());
  auto task_runner = base::SequencedTaskRunner::GetCurrentDefault();
  MappableSharedImageVideoFramePool pool(task_runner, task_runner.get(),
                                         &gpu_factories);
  SetPortableMediaProcessForTesting(true);
  auto frame = Frame(PIXEL_FORMAT_I420, gfx::Size(16, 16), 2);
  scoped_refptr<VideoFrame> result;
  pool.MaybeCreateHardwareFrame(
      frame, base::BindOnce([](scoped_refptr<VideoFrame>* out,
                               scoped_refptr<VideoFrame> f) { *out = f; },
                            &result));
  // At once, the same frame: no copy to a shared image.
  EXPECT_EQ(result, frame);
}

TEST_F(MoltPortableMediaTest, NoGpuYuvConversionInPortableMode) {
  EXPECT_TRUE(PaintCanvasVideoRenderer::
                  IsPixelFormatSupportedForYuvSharedImageConversion(
                      PIXEL_FORMAT_I420));
  SetPortableMediaProcessForTesting(true);
  for (VideoPixelFormat format :
       {PIXEL_FORMAT_I420, PIXEL_FORMAT_NV12, PIXEL_FORMAT_I420A}) {
    EXPECT_FALSE(PaintCanvasVideoRenderer::
                     IsPixelFormatSupportedForYuvSharedImageConversion(format));
  }
  auto frame = Frame(PIXEL_FORMAT_I420, gfx::Size(16, 16), 4);
  EXPECT_FALSE(PaintCanvasVideoRenderer::CanUseCopyVideoFrameToSharedImage(
      *frame));
}

}  // namespace
}  // namespace media
