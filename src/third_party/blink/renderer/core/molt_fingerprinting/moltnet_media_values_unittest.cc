// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media_values.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace blink::molt_fingerprinting::moltnet {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

constexpr AudioLatency kAllLatencies[] = {
    AudioLatency::kInteractive, AudioLatency::kBalanced,
    AudioLatency::kPlayback, AudioLatency::kExact};

TEST(MoltNetMediaValuesTest, OneStereoDeviceAt48kHz) {
  EXPECT_EQ(kAudioSampleRate, 48000.0f);
  EXPECT_EQ(kAudioMaxChannelCount, 2u);
  // The device's own buffer is 10 ms.
  EXPECT_EQ(kAudioDeviceFramesPerBuffer * 100, kAudioDeviceSampleRate);
  EXPECT_EQ(static_cast<double>(kAudioSampleRate), kAudioDeviceSampleRate);
  EXPECT_EQ(kAudioHardwareRenderQuantum, 128u);
}

TEST(MoltNetMediaValuesTest, CallbackBuffersAt48kHz) {
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kInteractive, 0, 48000), 480);
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kBalanced, 0, 48000), 480);
  // 20 ms (960 frames) rounded up to a power of two.
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kPlayback, 0, 48000), 1024);
  // The categories' answers don't depend on the exact value passed with
  // them.
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kInteractive, 5, 48000), 480);
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kPlayback, kNaN, 48000), 1024);
}

TEST(MoltNetMediaValuesTest, ExactLatencyHints) {
  struct {
    double seconds;
    int frames;
  } kCases[] = {
      {0, 128},          // as short as possible
      {0.001, 128},      // 48 frames
      {0.0027, 480},     // 129.6 frames: more than the minimum, so the 10 ms
      {0.01, 480},       // exactly the device's buffer
      {0.0101, 512},     // 484.8 frames, rounded up to 128 frames
      {0.02, 1024},      // 960 frames
      {0.1, 4864},       // 4800 frames
      {0.17, 8192},      // 8160 frames: rounded up to 8192
      {0.5, 8192},       // capped
      {-1, 128},         // nonsense is the minimum
      {kNaN, 128},       //
      {kInf, 8192},      //
      {1e300, 8192},     //
      {-kInf, 128},      //
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(AudioCallbackFrames(AudioLatency::kExact, c.seconds, 48000),
              c.frames)
        << c.seconds;
  }
}

TEST(MoltNetMediaValuesTest, CallbackBuffersAtThePagesSampleRate) {
  // A page that asks for a sample rate gets the device's 10 ms at that rate.
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kInteractive, 0, 44100), 441);
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kPlayback, 0, 44100), 1024);
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kInteractive, 0, 96000), 960);
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kPlayback, 0, 96000), 2048);
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kInteractive, 0, 22050.5), 221);
  // The limits of a valid AudioContext rate.
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kInteractive, 0, 3000), 128);
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kPlayback, 0, 3000), 128);
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kInteractive, 0, 768000), 7680);
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kPlayback, 0, 768000), 16384);
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kExact, 1, 768000), 8192);
}

TEST(MoltNetMediaValuesTest, InvalidRatesGiveNothing) {
  for (const double rate : {0.0, -48000.0, 2999.0, 768001.0, kNaN, kInf}) {
    for (const AudioLatency latency : kAllLatencies) {
      EXPECT_EQ(AudioCallbackFrames(latency, 0.01, rate), 0) << rate;
      EXPECT_EQ(AudioBaseLatency(latency, 0.01, 128, rate), 0) << rate;
    }
  }
}

TEST(MoltNetMediaValuesTest, EveryAnswerIsInWebAudiosRange) {
  for (double rate = 3000; rate <= 768000; rate += 997.5) {
    for (const AudioLatency latency : kAllLatencies) {
      for (const double seconds : {-1.0, 0.0, 0.003, 0.01, 0.05, 0.2, 5.0}) {
        const int frames = AudioCallbackFrames(latency, seconds, rate);
        EXPECT_GE(frames, kAudioMinCallbackFrames) << rate;
        EXPECT_LE(frames, latency == AudioLatency::kPlayback
                              ? 2 * kAudioMaxCallbackFrames
                              : kAudioMaxCallbackFrames)
            << rate;
        // Never shorter than the device's 10 ms unless an exact hint asked
        // for less than 128 frames.
        if (latency != AudioLatency::kExact ||
            std::lround(seconds * rate) > 128) {
          EXPECT_GE(frames, AudioCallbackFrames(AudioLatency::kInteractive,
                                                0, rate))
              << rate;
        }
      }
    }
  }
}

TEST(MoltNetMediaValuesTest, BaseLatency) {
  // 480 frames at 48 kHz.
  EXPECT_DOUBLE_EQ(AudioBaseLatency(AudioLatency::kInteractive, 0, 128, 48000),
                   0.01);
  EXPECT_DOUBLE_EQ(AudioBaseLatency(AudioLatency::kBalanced, 0, 128, 48000),
                   0.01);
  EXPECT_DOUBLE_EQ(AudioBaseLatency(AudioLatency::kPlayback, 0, 128, 48000),
                   1024.0 / 48000);
  EXPECT_DOUBLE_EQ(AudioBaseLatency(AudioLatency::kExact, 0, 128, 48000),
                   128.0 / 48000);
  // A render quantum longer than the callback buffer sets the latency, so a
  // page that picks one reads back only what it picked.
  EXPECT_DOUBLE_EQ(AudioBaseLatency(AudioLatency::kInteractive, 0, 1024, 48000),
                   1024.0 / 48000);
  EXPECT_DOUBLE_EQ(AudioBaseLatency(AudioLatency::kInteractive, 0, 256, 48000),
                   0.01);
  EXPECT_DOUBLE_EQ(AudioBaseLatency(AudioLatency::kInteractive, 0, 128, 44100),
                   0.01);
}

TEST(MoltNetMediaValuesTest, OutputLatencyIsOnChromiumsGrid) {
  // Chromium rounds outputLatency to 8 ms without microphone permission and
  // to 1 ms with it; the fixed value is on both grids, so it reads the same
  // either way.
  const double eighths = kAudioOutputLatencySeconds / 0.008;
  EXPECT_DOUBLE_EQ(eighths, std::round(eighths));
  const double ms = kAudioOutputLatencySeconds / 0.001;
  EXPECT_DOUBLE_EQ(ms, std::round(ms));
  // More than the base latency of an interactive context: the device's
  // buffer plays out after it is rendered.
  EXPECT_GT(kAudioOutputLatencySeconds,
            AudioBaseLatency(AudioLatency::kInteractive, 0, 128, 48000));
}

TEST(MoltNetMediaValuesTest, ScriptProcessorDefaultBufferSize) {
  // What Chromium picks for these callback buffers.
  EXPECT_EQ(ScriptProcessorBufferSize(480), 2048u);
  EXPECT_EQ(ScriptProcessorBufferSize(441), 2048u);
  EXPECT_EQ(ScriptProcessorBufferSize(512), 2048u);
  EXPECT_EQ(ScriptProcessorBufferSize(256), 1024u);
  EXPECT_EQ(ScriptProcessorBufferSize(1024), 4096u);
  EXPECT_EQ(ScriptProcessorBufferSize(128), 512u);
  EXPECT_EQ(ScriptProcessorBufferSize(1), 256u);
  EXPECT_EQ(ScriptProcessorBufferSize(0), 256u);
  EXPECT_EQ(ScriptProcessorBufferSize(16384), 16384u);
  EXPECT_EQ(ScriptProcessorBufferSize(UINT32_MAX), 16384u);
  // MoltNet's answer for a default context.
  EXPECT_EQ(ScriptProcessorBufferSize(static_cast<uint32_t>(
                AudioCallbackFrames(AudioLatency::kInteractive, 0, 48000))),
            2048u);
}

TEST(MoltNetMediaValuesTest, OutputPosition) {
  const AudioOutputPosition before = AudioOutputPositionAt(0);
  EXPECT_EQ(before.context_time, 0);
  EXPECT_FALSE(before.playing);
  const AudioOutputPosition starting = AudioOutputPositionAt(0.01);
  EXPECT_EQ(starting.context_time, 0);
  EXPECT_TRUE(starting.playing);
  const AudioOutputPosition later = AudioOutputPositionAt(2.5);
  EXPECT_DOUBLE_EQ(later.context_time, 2.5 - kAudioOutputLatencySeconds);
  EXPECT_TRUE(later.playing);
  for (const double t : {-1.0, kNaN, -kInf}) {
    const AudioOutputPosition p = AudioOutputPositionAt(t);
    EXPECT_EQ(p.context_time, 0);
    EXPECT_FALSE(p.playing);
  }
}

TEST(MoltNetMediaValuesTest, NoKeySystemsWithChromiumsMessage) {
  EXPECT_EQ(std::string(kNoKeySystemMessage),
            "Unsupported keySystem or supportedConfigurations.");
}

TEST(MoltNetMediaValuesTest, MediaCapabilitiesTable) {
  const MediaCapabilitiesAnswer built_in = VideoDecodingInfo(true);
  EXPECT_TRUE(built_in.supported);
  EXPECT_TRUE(built_in.smooth);
  EXPECT_FALSE(built_in.power_efficient);
  const MediaCapabilitiesAnswer platform_only = VideoDecodingInfo(false);
  EXPECT_FALSE(platform_only.supported);
  EXPECT_FALSE(platform_only.smooth);
  EXPECT_FALSE(platform_only.power_efficient);
  EXPECT_FALSE(kWebrtcCodingInfo.supported);
  EXPECT_FALSE(kWebrtcCodingInfo.smooth);
  EXPECT_FALSE(kWebrtcCodingInfo.power_efficient);
}

TEST(MoltNetMediaValuesTest, OneDeviceOfEachKindIsListed) {
  // A machine without a device of a kind lists one, as every other machine
  // does before the page may use that kind.
  EXPECT_TRUE(ListsUniformDevice(0, false));
  // A machine with one already lists it (one entry, whatever the count).
  EXPECT_FALSE(ListsUniformDevice(1, false));
  // With the permission the page sees the real devices, or none.
  EXPECT_FALSE(ListsUniformDevice(0, true));
  EXPECT_FALSE(ListsUniformDevice(3, true));
}

TEST(MoltNetMediaValuesTest, UserMediaFailsOnlyAfterAsking) {
  using A = PermissionAnswer;
  using V = UserMediaVerdict;
  // Granted (now or before): the page may know the devices.
  EXPECT_EQ(UserMediaFailureVerdict(std::array{A::kGranted}), V::kRealFailure);
  EXPECT_EQ(UserMediaFailureVerdict(std::array{A::kGranted, A::kGranted}),
            V::kRealFailure);
  // Denied, now or before: as a request a device could meet.
  EXPECT_EQ(UserMediaFailureVerdict(std::array{A::kDenied}),
            V::kPermissionDenied);
  EXPECT_EQ(UserMediaFailureVerdict(std::array{A::kGranted, A::kDenied}),
            V::kPermissionDenied);
  EXPECT_EQ(UserMediaFailureVerdict(std::array{A::kAsk, A::kDenied}),
            V::kPermissionDenied);
  // Dismissed.
  EXPECT_EQ(UserMediaFailureVerdict(std::array{A::kAsk}),
            V::kPermissionDismissed);
  EXPECT_EQ(UserMediaFailureVerdict(std::array{A::kGranted, A::kAsk}),
            V::kPermissionDismissed);
  // Nothing asked: never the real failure.
  EXPECT_EQ(UserMediaFailureVerdict({}), V::kPermissionDenied);
}

}  // namespace
}  // namespace blink::molt_fingerprinting::moltnet
