// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "media/base/molt_portable_media.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/memory/aligned_memory.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/threading/thread.h"
#include "build/build_config.h"
#include "gpu/command_buffer/client/test_shared_image_interface.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/base/channel_layout.h"
#include "media/base/channel_mixer.h"
#include "media/base/decoder_factory.h"
#include "media/base/media_util.h"
#include "media/base/molt_portable_scale.h"
#include "media/base/sinc_resampler.h"
#include "media/base/vector_math.h"
#include "media/base/vector_math_testing.h"
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

#if defined(ARCH_CPU_X86_64)
#include <xmmintrin.h>
#endif

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

// A microphone has no platform effects in a portable media process; other
// processes keep the device's.
TEST_F(MoltPortableMediaTest, NoCaptureEffectsInPortableMode) {
  constexpr int kEffects = AudioParameters::ECHO_CANCELLER |
                           AudioParameters::NOISE_SUPPRESSION |
                           AudioParameters::AUTOMATIC_GAIN_CONTROL |
                           AudioParameters::VOICE_ISOLATION_SUPPORTED;
  EXPECT_EQ(PortableCaptureEffects(kEffects), kEffects);
  SetPortableMediaProcessForTesting(true);
  EXPECT_EQ(PortableCaptureEffects(kEffects), AudioParameters::NO_EFFECTS);
}

// ScopedPortableAudioThread: the portable audio code on the calling thread
// while it lives; scopes nest, and other threads don't see them.
TEST_F(MoltPortableMediaTest, PortableAudioThreadScope) {
  EXPECT_FALSE(IsPortableAudioThread());
  {
    ScopedPortableAudioThread on(true);
    EXPECT_TRUE(IsPortableAudioThread());
    {
      ScopedPortableAudioThread off(false);
      EXPECT_FALSE(IsPortableAudioThread());
    }
    EXPECT_TRUE(IsPortableAudioThread());
    bool other_thread_portable = true;
    base::Thread thread("other");
    ASSERT_TRUE(thread.Start());
    base::RunLoop run_loop;
    thread.task_runner()->PostTaskAndReply(
        FROM_HERE,
        base::BindOnce([](bool* out) { *out = IsPortableAudioThread(); },
                       &other_thread_portable),
        run_loop.QuitClosure());
    run_loop.Run();
    EXPECT_FALSE(other_thread_portable);
  }
  EXPECT_FALSE(IsPortableAudioThread());
  // A portable media process computes portably on every thread.
  SetPortableMediaProcessForTesting(true);
  EXPECT_TRUE(IsPortableAudioThread());
}

// In a scope, the audio service's mixing code computes as a portable media
// process does: FMAC is the plain C sum, and a SincResampler built in one
// gives the samples of one built and run in a portable media process (the
// portable kernel, the C convolution).
TEST_F(MoltPortableMediaTest, PortableAudioThreadMixesWithTheCCode) {
  constexpr size_t kFrames = 480;
  auto src =
      base::AlignedUninit<float>(kFrames, vector_math::kRequiredAlignment);
  auto scoped =
      base::AlignedUninit<float>(kFrames, vector_math::kRequiredAlignment);
  auto plain =
      base::AlignedUninit<float>(kFrames, vector_math::kRequiredAlignment);
  uint32_t s = 7;
  for (size_t i = 0; i < kFrames; ++i) {
    s = s * 1664525u + 1013904223u;
    src[i] = static_cast<int32_t>(s) / 2147483648.0f;
    scoped[i] = plain[i] = 0.25f - i / 4096.0f;
  }
  {
    ScopedPortableAudioThread on(true);
    vector_math::FMAC(src, 0.7071f, scoped);
  }
  vector_math::FMAC_C(src, 0.7071f, plain);
  EXPECT_EQ(base::span(scoped), base::span(plain));

  // 44.1 kHz to 48 kHz, as the mixing resamples a 44.1 kHz stream.
  auto resample = [](bool scope) {
    ScopedPortableAudioThread on(scope);
    uint32_t phase = 0;
    SincResampler resampler(
        44100.0 / 48000.0, 441,
        base::BindRepeating(
            [](uint32_t* phase, base::span<float> dest) {
              for (float& x : dest) {
                *phase += 97391548u;  // about 1 kHz at 44.1 kHz
                x = static_cast<int32_t>(*phase) / 4294967296.0f;
              }
            },
            &phase));
    std::vector<float> out(480 * 20);
    for (size_t at = 0; at < out.size(); at += 480) {
      resampler.Resample(base::span(out).subspan(at, 480u));
    }
    return out;
  };
  const std::vector<float> in_scope = resample(true);
  SetPortableMediaProcessForTesting(true);
  const std::vector<float> in_portable_process = resample(false);
  EXPECT_EQ(in_scope, in_portable_process);
}

uint32_t Bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, sizeof(b));
  return b;
}

// The portable floor: magnitudes below 2^-63 become zeros of their sign; the
// floor itself and everything else, infinities and NaNs included, stay.
TEST_F(MoltPortableMediaTest, FlushTinyPortableSamplesKeepsTheFloor) {
  EXPECT_EQ(kPortableAudioFloor * kPortableAudioFloor,
            std::numeric_limits<float>::min());
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  std::vector<float> samples = {0x1p-63f,
                                -0x1p-63f,
                                0x1.fffffep-64f,
                                -0x1p-64f,
                                std::numeric_limits<float>::min(),
                                std::numeric_limits<float>::denorm_min(),
                                -1e-40f,
                                0.0f,
                                -0.0f,
                                1.0f,
                                -3e-19f,
                                inf,
                                -inf,
                                nan};
  const std::vector<float> in = samples;
  FlushTinyPortableSamples(samples);
  const std::vector<uint32_t> expected = {
      Bits(0x1p-63f), Bits(-0x1p-63f), Bits(0.0f),  Bits(-0.0f), Bits(0.0f),
      Bits(0.0f),     Bits(-0.0f),     Bits(0.0f),  Bits(-0.0f), Bits(1.0f),
      Bits(-3e-19f),  Bits(inf),       Bits(-inf),  Bits(nan)};
  ASSERT_EQ(samples.size(), expected.size());
  for (size_t i = 0; i < samples.size(); ++i) {
    EXPECT_EQ(Bits(samples[i]), expected[i]) << i;
    EXPECT_EQ(Bits(FlushTinyPortableSample(in[i])), expected[i]) << i;
  }

  // Every channel of a bus.
  auto bus = AudioBus::Create(2, 3);
  for (int ch = 0; ch < 2; ++ch) {
    bus->channel(ch)[0] = 1e-30f;
    bus->channel(ch)[1] = 0.5f;
    bus->channel(ch)[2] = -1e-25f;
  }
  FlushTinyPortableSamples(*bus);
  for (int ch = 0; ch < 2; ++ch) {
    EXPECT_EQ(Bits(bus->channel(ch)[0]), Bits(0.0f));
    EXPECT_EQ(bus->channel(ch)[1], 0.5f);
    EXPECT_EQ(Bits(bus->channel(ch)[2]), Bits(-0.0f));
  }
}

// The same floor for doubles (an IIR filter's history): below 2^-63 a zero of
// the sign, far above the subnormal doubles; everything else stays.
TEST_F(MoltPortableMediaTest, FlushTinyPortableSampleFloorsDoubles) {
  const double floor = 0x1p-63;
  EXPECT_EQ(FlushTinyPortableSample(floor), floor);
  EXPECT_EQ(FlushTinyPortableSample(-floor), -floor);
  EXPECT_EQ(FlushTinyPortableSample(0x1.fffffffffffffp-64), 0.0);
  const double negative = FlushTinyPortableSample(-0x1p-64);
  EXPECT_EQ(negative, 0.0);
  EXPECT_TRUE(std::signbit(negative));
  EXPECT_EQ(FlushTinyPortableSample(std::numeric_limits<double>::min()), 0.0);
  EXPECT_EQ(FlushTinyPortableSample(std::numeric_limits<double>::denorm_min()),
            0.0);
  // Doubles at or above the floor stay with all their bits, also ones a float
  // can't hold.
  EXPECT_EQ(FlushTinyPortableSample(0x1.0000000000001p-63),
            0x1.0000000000001p-63);
  EXPECT_EQ(FlushTinyPortableSample(1e300), 1e300);
  EXPECT_TRUE(std::isinf(
      FlushTinyPortableSample(std::numeric_limits<double>::infinity())));
  EXPECT_TRUE(std::isnan(
      FlushTinyPortableSample(std::numeric_limits<double>::quiet_NaN())));
}

// Sets the CPU's flush-to-zero modes the way Chromium's disablers do (x86:
// MXCSR FTZ and DAZ; arm64: FPCR.FZ) for a test, and puts back the mode it
// found.
class ScopedFlushModeForTesting {
 public:
  ScopedFlushModeForTesting() {
#if defined(ARCH_CPU_X86_64)
    saved_ = _mm_getcsr();
    _mm_setcsr(saved_ | 0x8040u);
#elif defined(ARCH_CPU_ARM64)
    uint64_t control;
    asm volatile("mrs %x[c], fpcr" : [c] "=r"(control));
    saved_ = control;
    control |= uint64_t{1} << 24;
    asm volatile("msr fpcr, %x[c]" : : [c] "r"(control));
#endif
  }
  ~ScopedFlushModeForTesting() {
#if defined(ARCH_CPU_X86_64)
    _mm_setcsr(static_cast<unsigned int>(saved_));
#elif defined(ARCH_CPU_ARM64)
    asm volatile("msr fpcr, %x[c]" : : [c] "r"(saved_));
#endif
  }

 private:
  uint64_t saved_ = 0;
};

// ScopedPortableSubnormals turns the flush modes off while it lives and puts
// back what it found: inside it a product below FLT_MIN is the IEEE 754
// subnormal on every CPU.
TEST_F(MoltPortableMediaTest, ScopedPortableSubnormalsComputesSubnormals) {
#if defined(ARCH_CPU_X86_64) || defined(ARCH_CPU_ARM64)
  volatile float tiny = 0x1p-70f;
  const auto product = [&tiny] { return tiny * tiny; };  // 2^-140
  ASSERT_FALSE(ScopedPortableSubnormals::FlushModeIsOn());
  EXPECT_EQ(product(), 0x1p-140f);
  {
    ScopedFlushModeForTesting flush;
    EXPECT_TRUE(ScopedPortableSubnormals::FlushModeIsOn());
    EXPECT_EQ(product(), 0.0f);
    {
      ScopedPortableSubnormals portable;
      EXPECT_FALSE(ScopedPortableSubnormals::FlushModeIsOn());
      EXPECT_EQ(product(), 0x1p-140f);
      {
        ScopedPortableSubnormals nested;
        EXPECT_EQ(product(), 0x1p-140f);
      }
      EXPECT_FALSE(ScopedPortableSubnormals::FlushModeIsOn());
    }
    EXPECT_TRUE(ScopedPortableSubnormals::FlushModeIsOn());
    EXPECT_EQ(product(), 0.0f);
  }
  EXPECT_FALSE(ScopedPortableSubnormals::FlushModeIsOn());
  // Without a flush mode, the scope changes nothing.
  {
    ScopedPortableSubnormals portable;
    EXPECT_FALSE(ScopedPortableSubnormals::FlushModeIsOn());
  }
  EXPECT_FALSE(ScopedPortableSubnormals::FlushModeIsOn());
#else
  GTEST_SKIP() << "no flush-to-zero mode to test on this architecture";
#endif
}

// Where a SincResampler computes portably, its kernel has no coefficient
// below the floor (the window's zeros are zeros), and an input below the floor
// resamples exactly as silence does; elsewhere the kernel and the input are
// the CPU's, as before.
TEST_F(MoltPortableMediaTest, PortableResamplerFlushesItsKernelAndInput) {
  auto run = [](bool portable, float level) {
    ScopedPortableAudioThread scope(portable);
    SincResampler resampler(
        176400.0 / 48000.0, 441,
        base::BindRepeating(
            [](float level, base::span<float> dest) {
              for (size_t i = 0; i < dest.size(); ++i) {
                dest[i] = (i & 1) ? level : -level;
              }
            },
            level));
    size_t tiny_coefficients = 0;
    for (float k : resampler.get_kernel_for_testing()) {
      if (k != 0.0f && std::fabs(k) < kPortableAudioFloor) {
        ++tiny_coefficients;
      }
    }
    std::vector<float> out(480 * 4);
    for (size_t at = 0; at < out.size(); at += 480) {
      resampler.Resample(base::span(out).subspan(at, 480u));
    }
    return std::make_pair(tiny_coefficients, out);
  };
  // 176.4 kHz to 48 kHz has coefficients of 4.7e-21 at the window's zeros.
  EXPECT_GT(run(false, 0.0f).first, 0u);
  EXPECT_EQ(run(true, 0.0f).first, 0u);

  const std::vector<float> silence = run(true, 0.0f).second;
  EXPECT_EQ(run(true, 0x1p-64f).second, silence);
  EXPECT_EQ(run(true, -1e-30f).second, silence);
  EXPECT_NE(run(true, 0x1p-62f).second, silence);
  // Unchanged outside the portable code.
  EXPECT_NE(run(false, 1e-30f).second, run(false, 0.0f).second);
}

// A microphone's format as a portable process's pages see it: the same for
// every device; other processes see the device's own.
TEST_F(MoltPortableMediaTest, PortableCaptureParametersAreTheSameForAll) {
  AudioParameters mono_44k(
      AudioParameters::AUDIO_PCM_LOW_LATENCY, ChannelLayoutConfig::Mono(),
      44100, 441, AudioParameters::HardwareCapabilities(128, 4096, 441, false));
  mono_44k.set_effects(AudioParameters::ECHO_CANCELLER |
                       AudioParameters::NOISE_SUPPRESSION);
  const AudioParameters stereo_96k(AudioParameters::AUDIO_PCM_LOW_LATENCY,
                                   ChannelLayoutConfig::Stereo(), 96000, 256);
  EXPECT_TRUE(PortableCaptureParameters(mono_44k).Equals(mono_44k));
  EXPECT_TRUE(PortableCaptureParameters(stereo_96k).Equals(stereo_96k));

  SetPortableMediaProcessForTesting(true);
  const AudioParameters uniform = PortableCaptureParameters(mono_44k);
  EXPECT_EQ(uniform.format(), AudioParameters::AUDIO_PCM_LOW_LATENCY);
  EXPECT_EQ(uniform.sample_rate(), kPortableCaptureSampleRate);
  EXPECT_EQ(uniform.sample_rate(), 48000);
  EXPECT_EQ(uniform.channel_layout(), CHANNEL_LAYOUT_STEREO);
  EXPECT_EQ(uniform.channels(), kPortableCaptureChannels);
  EXPECT_EQ(uniform.frames_per_buffer(), 480);
  EXPECT_EQ(uniform.effects(), AudioParameters::NO_EFFECTS);
  EXPECT_FALSE(uniform.hardware_capabilities().has_value());
  EXPECT_TRUE(PortableCaptureParameters(stereo_96k).Equals(uniform));

  // No device: still no device, and no effects.
  AudioParameters unavailable;
  unavailable.set_effects(AudioParameters::ECHO_CANCELLER);
  EXPECT_FALSE(PortableCaptureParameters(unavailable).IsValid());
  EXPECT_EQ(PortableCaptureParameters(unavailable).effects(),
            AudioParameters::NO_EFFECTS);
}

// Windows: a portable process's stream never takes the communications
// category without raw mode (more than 8 channels); others keep Chromium's.
TEST_F(MoltPortableMediaTest, CommunicationsCategoryOnlyWithRawMode) {
  AudioParameters chromium(AudioParameters::AUDIO_PCM_LOW_LATENCY,
                           ChannelLayoutConfig::Stereo(), 48000, 480);
  AudioParameters portable = chromium;
  portable.set_effects(kPortableCaptureEffect);
  for (const int channels : {1, 2, 8}) {
    EXPECT_TRUE(PortableCaptureMayUseCommunicationsCategory(chromium,
                                                            channels));
    EXPECT_TRUE(PortableCaptureMayUseCommunicationsCategory(portable,
                                                            channels));
  }
  EXPECT_TRUE(PortableCaptureMayUseCommunicationsCategory(chromium, 16));
  EXPECT_FALSE(PortableCaptureMayUseCommunicationsCategory(portable, 9));
  EXPECT_FALSE(PortableCaptureMayUseCommunicationsCategory(portable, 16));
  EXPECT_FALSE(PortableCaptureMayUseCommunicationsCategory(portable, 0));
}

// The device of a portable process's stream opens with no platform effect,
// marked where the platform code reads the mark; on Android, where any effect
// asks for the OS's communication mode, with none at all.
TEST_F(MoltPortableMediaTest, PortableCaptureStreamEffects) {
  EXPECT_EQ(PortableCaptureStreamEffects() & ~kPortableCaptureEffect,
            AudioParameters::NO_EFFECTS);
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN)
  EXPECT_EQ(PortableCaptureStreamEffects(), kPortableCaptureEffect);
#else
  EXPECT_EQ(PortableCaptureStreamEffects(), AudioParameters::NO_EFFECTS);
#endif
}

TEST_F(MoltPortableMediaTest, PortableCaptureOutputFormat) {
  const AudioParameters mono_16k(AudioParameters::AUDIO_PCM_LOW_LATENCY,
                                 ChannelLayoutConfig::Mono(), 16000, 160);
  const AudioParameters mono = PortableCaptureOutputFormat(mono_16k, false);
  EXPECT_EQ(mono.sample_rate(), 48000);
  EXPECT_EQ(mono.channel_layout(), CHANNEL_LAYOUT_MONO);
  EXPECT_EQ(mono.frames_per_buffer(), 480);
  // Stereo when asked for, from a mono device too.
  const AudioParameters stereo = PortableCaptureOutputFormat(mono_16k, true);
  EXPECT_EQ(stereo.channel_layout(), CHANNEL_LAYOUT_STEREO);
  EXPECT_EQ(stereo.sample_rate(), 48000);
  EXPECT_EQ(stereo.frames_per_buffer(), 480);
  EXPECT_EQ(stereo.format(), AudioParameters::AUDIO_PCM_LOW_LATENCY);
}

// BEGIN DOWNMIX SIGNAL (the standalone harness extracts this block)
// Fills `bus` with the next frames of a fixed signal: every sample from a
// 32-bit LCG, in [-0.5, 0.5) with all 24 bits of the mantissa in use, so that
// a fused and a separate multiply-add round apart.
void FillDownmixSignal(uint32_t& state, AudioBus& bus) {
  for (int f = 0; f < bus.frames(); ++f) {
    for (int c = 0; c < bus.channels(); ++c) {
      state = state * 1664525u + 1013904223u;
      bus.channel(c)[f] = static_cast<int32_t>(state) / 4294967296.0f;
    }
  }
}

// FNV-1a over the bits of every sample of `bus`, channel by channel.
void FnvDownmix(uint64_t& hash, const AudioBus& bus) {
  for (int c = 0; c < bus.channels(); ++c) {
    for (float sample : bus.channel(c)) {
      uint32_t bits;
      std::memcpy(&bits, &sample, sizeof(bits));
      for (int i = 0; i < 4; ++i) {
        hash ^= (bits >> (8 * i)) & 0xff;
        hash *= 1099511628211ull;
      }
    }
  }
}

// The digest of `buffers` buffers of `frames` frames of the signal on
// `channels` channels as `mix` (const AudioBus& -> const AudioBus&) mixes
// them.
template <typename Mix>
uint64_t DigestDownmix(Mix&& mix, int channels, int frames, int buffers) {
  uint32_t state = 2026;
  uint64_t hash = 1469598103934665603ull;
  auto input = AudioBus::Create(channels, frames);
  for (int b = 0; b < buffers; ++b) {
    FillDownmixSignal(state, *input);
    FnvDownmix(hash, mix(*input));
  }
  return hash;
}

// The layouts of the golden digests below: Windows' QUAD and 5.1 microphone
// arrays, at 44.1 kHz in 10 ms buffers, down-mixed to one and two channels.
struct DownmixGolden {
  ChannelLayout layout;
  int channels;
  uint64_t digest;
};
// END DOWNMIX SIGNAL

// Computed with Chromium 156's ChannelMixer and MoltBrowser's vector_math.cc
// built standalone (x86-64 natively, with AVX2, FMA and AVX-512, and under
// qemu on Core 2, Nehalem, Haswell and EPYC models; arm64 under qemu on
// Cortex-A53 and the max CPU), all of which agree, also from a thread left in
// a flush mode. Chromium's own dispatch of the multiply-adds gives other
// digests on the CPUs with AVX2 and FMA.
constexpr DownmixGolden kDownmixGoldens[] = {
    {CHANNEL_LAYOUT_QUAD, 1, 0x75deaebc8f7a7033ull},
    {CHANNEL_LAYOUT_QUAD, 2, 0x4f1655f56506a6d9ull},
    {CHANNEL_LAYOUT_5_1, 1, 0x172ebd2bea51f2acull},
    {CHANNEL_LAYOUT_5_1, 2, 0x17611a79a9527093ull},
};

// A device of another layout than mono, stereo and discrete is down-mixed;
// those three aren't.
TEST_F(MoltPortableMediaTest, PortableCaptureNeedsDownmix) {
  EXPECT_FALSE(PortableCaptureNeedsDownmix(CHANNEL_LAYOUT_MONO));
  EXPECT_FALSE(PortableCaptureNeedsDownmix(CHANNEL_LAYOUT_STEREO));
  EXPECT_FALSE(PortableCaptureNeedsDownmix(CHANNEL_LAYOUT_DISCRETE));
  EXPECT_FALSE(PortableCaptureNeedsDownmix(CHANNEL_LAYOUT_NONE));
  EXPECT_FALSE(PortableCaptureNeedsDownmix(CHANNEL_LAYOUT_UNSUPPORTED));
  EXPECT_FALSE(PortableCaptureNeedsDownmix(CHANNEL_LAYOUT_BITSTREAM));
  EXPECT_TRUE(PortableCaptureNeedsDownmix(CHANNEL_LAYOUT_QUAD));
  EXPECT_TRUE(PortableCaptureNeedsDownmix(CHANNEL_LAYOUT_5_1));
  EXPECT_TRUE(PortableCaptureNeedsDownmix(CHANNEL_LAYOUT_7_1));
  EXPECT_TRUE(PortableCaptureNeedsDownmix(CHANNEL_LAYOUT_2_1));
  EXPECT_TRUE(PortableCaptureNeedsDownmix(CHANNEL_LAYOUT_STEREO_DOWNMIX));
}

// The down-mix computes the same samples on every CPU: from a thread outside
// any portable scope, and from one left in a flush mode, it gives the golden
// digests; and it gives what media's ChannelMixer gives in a portable scope.
TEST_F(MoltPortableMediaTest, DownmixGoldenDigests) {
  for (const DownmixGolden& golden : kDownmixGoldens) {
    const AudioParameters device(
        AudioParameters::AUDIO_PCM_LOW_LATENCY,
        ChannelLayoutConfig::FromLayout(golden.layout), 44100, 441);
    PortableCaptureDownmix downmix(device, golden.channels);
    EXPECT_EQ(downmix.channels(), golden.channels);
    const auto portable = [&downmix](const AudioBus& in) -> const AudioBus& {
      return downmix.Downmix(in);
    };
    ASSERT_FALSE(IsPortableAudioThread());
    const uint64_t digest =
        DigestDownmix(portable, device.channels(), 441, 200);
    EXPECT_EQ(digest, golden.digest)
        << ChannelLayoutToString(golden.layout) << " to "
        << golden.channels << ": " << std::hex << digest;
    {
      ScopedFlushModeForTesting flush;
      EXPECT_EQ(DigestDownmix(portable, device.channels(), 441, 200),
                digest);
    }

    ChannelMixer mixer(device.channel_layout_config(),
                       golden.channels == 1 ? ChannelLayoutConfig::Mono()
                                            : ChannelLayoutConfig::Stereo());
    auto output = AudioBus::Create(golden.channels, 441);
    const auto reference = [&](const AudioBus& in) -> const AudioBus& {
      ScopedPortableAudioThread portable_code(true);
      mixer.Transform(&in, output.get());
      return *output;
    };
    EXPECT_EQ(DigestDownmix(reference, device.channels(), 441, 200), digest);
  }
}

}  // namespace
}  // namespace media
