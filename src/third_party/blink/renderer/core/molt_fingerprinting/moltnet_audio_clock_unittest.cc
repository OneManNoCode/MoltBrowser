// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_audio_clock.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media_values.h"

namespace blink::molt_fingerprinting::moltnet {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// Pushes `frames` frames of `value` into both channels of `ring`.
int PushValue(OutputRing& ring, int frames, float value) {
  std::vector<float> left(static_cast<size_t>(std::max(0, frames)), value);
  std::vector<float> right(left.size(), -value);
  const std::array<std::span<const float>, 2> channels = {left, right};
  return ring.Push(channels, frames);
}

TEST(MoltNetAudioClockTest, ClockRate) {
  EXPECT_EQ(ClockSampleRate(48000), 48000);
  EXPECT_EQ(ClockSampleRate(44100), 44100);
  // Whole frames per second, as Chromium opens the stream.
  EXPECT_EQ(ClockSampleRate(44100.9), 44100);
  EXPECT_EQ(ClockSampleRate(3000), 3000);
  EXPECT_EQ(ClockSampleRate(768000), 768000);
  EXPECT_EQ(ClockSampleRate(2999.9), 0);
  EXPECT_EQ(ClockSampleRate(768000.5), 0);
  EXPECT_EQ(ClockSampleRate(kNaN), 0);
  EXPECT_EQ(ClockSampleRate(kInf), 0);
  EXPECT_EQ(ClockSampleRate(-48000), 0);
}

TEST(MoltNetAudioClockTest, TheUniformDevicesTicks) {
  // The clock ticks with the uniform device's callback: 480 frames every
  // 10 ms at 48 kHz, whatever the real device.
  const int frames = AudioCallbackFrames(AudioLatency::kInteractive, 0, 48000);
  EXPECT_EQ(frames, 480);
  EXPECT_DOUBLE_EQ(frames / static_cast<double>(ClockSampleRate(48000)), 0.01);
  EXPECT_EQ(AudioCallbackFrames(AudioLatency::kInteractive, 0, 44100), 441);
  EXPECT_DOUBLE_EQ(441 / static_cast<double>(ClockSampleRate(44100)), 0.01);
}

TEST(MoltNetAudioClockTest, RingPushPop) {
  OutputRing ring(2, 8);
  EXPECT_EQ(ring.channels(), 2);
  EXPECT_EQ(ring.capacity(), 8);
  EXPECT_EQ(ring.level(), 0);
  const std::array<float, 5> left = {1, 2, 3, 4, 5};
  const std::array<float, 5> right = {-1, -2, -3, -4, -5};
  const std::array<std::span<const float>, 2> in = {left, right};
  EXPECT_EQ(ring.Push(in, 5), 0);
  EXPECT_EQ(ring.level(), 5);

  std::array<float, 3> out_left = {};
  std::array<float, 3> out_right = {};
  const std::array<std::span<float>, 2> out = {out_left, out_right};
  EXPECT_EQ(ring.Pop(out, 3), 3);
  EXPECT_EQ(out_left, (std::array<float, 3>{1, 2, 3}));
  EXPECT_EQ(out_right, (std::array<float, 3>{-1, -2, -3}));
  EXPECT_EQ(ring.level(), 2);

  // Wraps around the end.
  EXPECT_EQ(ring.Push(in, 5), 0);
  EXPECT_EQ(ring.level(), 7);
  std::array<float, 7> all_left = {};
  std::array<float, 7> all_right = {};
  const std::array<std::span<float>, 2> all = {all_left, all_right};
  EXPECT_EQ(ring.Pop(all, 100), 7);
  EXPECT_EQ(all_left, (std::array<float, 7>{4, 5, 1, 2, 3, 4, 5}));
  EXPECT_EQ(all_right, (std::array<float, 7>{-4, -5, -1, -2, -3, -4, -5}));
  EXPECT_EQ(ring.level(), 0);
  EXPECT_EQ(ring.Pop(all, 3), 0);
}

TEST(MoltNetAudioClockTest, RingOverflowDropsTheOldest) {
  OutputRing ring(1, 4);
  const std::array<float, 3> a = {1, 2, 3};
  const std::array<float, 3> b = {4, 5, 6};
  const std::array<std::span<const float>, 1> in_a = {a};
  const std::array<std::span<const float>, 1> in_b = {b};
  EXPECT_EQ(ring.Push(in_a, 3), 0);
  EXPECT_EQ(ring.Push(in_b, 3), 2);
  EXPECT_EQ(ring.level(), 4);
  std::array<float, 4> out = {};
  const std::array<std::span<float>, 1> dest = {out};
  EXPECT_EQ(ring.Pop(dest, 4), 4);
  EXPECT_EQ(out, (std::array<float, 4>{3, 4, 5, 6}));

  // A push larger than the ring keeps its newest frames.
  const std::array<float, 6> big = {1, 2, 3, 4, 5, 6};
  const std::array<std::span<const float>, 1> in_big = {big};
  EXPECT_EQ(ring.Push(in_a, 3), 0);
  EXPECT_EQ(ring.Push(in_big, 6), 3 + 2);
  EXPECT_EQ(ring.Pop(dest, 4), 4);
  EXPECT_EQ(out, (std::array<float, 4>{3, 4, 5, 6}));
}

TEST(MoltNetAudioClockTest, RingMissingAndShortChannels) {
  OutputRing ring(3, 8);
  const std::array<float, 4> only = {1, 2, 3, 4};
  const std::array<float, 2> short_one = {7, 8};
  // Channel 1 is short, channel 2 is missing: both read as silence.
  const std::array<std::span<const float>, 2> in = {only, short_one};
  EXPECT_EQ(ring.Push(in, 4), 0);
  std::array<float, 4> c0 = {};
  std::array<float, 4> c1 = {9, 9, 9, 9};
  std::array<float, 4> c2 = {9, 9, 9, 9};
  const std::array<std::span<float>, 3> out = {c0, c1, c2};
  EXPECT_EQ(ring.Pop(out, 4), 4);
  EXPECT_EQ(c0, (std::array<float, 4>{1, 2, 3, 4}));
  EXPECT_EQ(c1, (std::array<float, 4>{7, 8, 0, 0}));
  EXPECT_EQ(c2, (std::array<float, 4>{0, 0, 0, 0}));

  // A short destination limits the pop; the ring keeps the rest.
  EXPECT_EQ(ring.Push(in, 4), 0);
  std::array<float, 2> small = {};
  const std::array<std::span<float>, 3> limited = {c0, small, c2};
  EXPECT_EQ(ring.Pop(limited, 4), 2);
  EXPECT_EQ(ring.level(), 2);
}

TEST(MoltNetAudioClockTest, RingDiscardAndSilence) {
  OutputRing ring(2, 16);
  PushValue(ring, 5, 0.0f);
  PushValue(ring, 3, 0.25f);
  EXPECT_EQ(ring.LeadingSilence(100), 5);
  EXPECT_EQ(ring.LeadingSilence(4), 4);
  EXPECT_EQ(ring.LeadingSilence(-1), 0);
  EXPECT_EQ(ring.Discard(2), 2);
  EXPECT_EQ(ring.LeadingSilence(100), 3);
  EXPECT_EQ(ring.Discard(100), 6);
  EXPECT_EQ(ring.level(), 0);
  EXPECT_EQ(ring.LeadingSilence(100), 0);
  EXPECT_EQ(ring.Discard(-3), 0);

  // Silence means every channel: a non-zero right channel counts.
  std::vector<float> zeros(4, 0.0f);
  std::vector<float> right = {0, 0, 0.5f, 0};
  const std::array<std::span<const float>, 2> in = {zeros, right};
  ring.Push(in, 4);
  EXPECT_EQ(ring.LeadingSilence(4), 2);
  ring.Clear();
  EXPECT_EQ(ring.level(), 0);
}

TEST(MoltNetAudioClockTest, RingRejectsNonsense) {
  for (const auto& [channels, capacity] :
       std::vector<std::pair<int, int>>{{0, 8},
                                        {33, 8},
                                        {-1, 8},
                                        {2, 0},
                                        {2, -5},
                                        {32, std::numeric_limits<int>::max()}}) {
    OutputRing ring(channels, capacity);
    EXPECT_EQ(ring.capacity(), 0);
    EXPECT_EQ(PushValue(ring, 4, 1.0f), 0);
    EXPECT_EQ(ring.level(), 0);
    std::array<float, 4> out = {};
    const std::array<std::span<float>, 1> dest = {out};
    EXPECT_EQ(ring.Pop(dest, 4), 0);
    EXPECT_EQ(ring.Discard(4), 0);
    EXPECT_EQ(ring.LeadingSilence(4), 0);
  }
  OutputRing ring(1, 4);
  EXPECT_EQ(PushValue(ring, 0, 1.0f), 0);
  EXPECT_EQ(PushValue(ring, -3, 1.0f), 0);
  EXPECT_EQ(ring.level(), 0);
}

TEST(MoltNetAudioClockTest, RingSize) {
  // A 480-frame tick and device, 128-frame reads at 48 kHz: three of the
  // device's buffers, two ticks, two reads and 145 ms.
  EXPECT_EQ(OutputRingFrames(480, 480, 128, 48000),
            3 * 480 + 960 + 256 + 6960);
  // The largest device buffer Web Audio opens (8192) fits.
  EXPECT_GT(OutputRingFrames(480, 8192, 128, 48000), 8192 + 960);
  EXPECT_EQ(OutputRingFrames(480, 480, 128, kNaN), 0);
  EXPECT_EQ(OutputRingFrames(480, 480, 128, 1e9), 0);
  EXPECT_GT(OutputRingFrames(-1, -1, -1, 48000), 0);
}

TEST(MoltNetAudioClockTest, VirtualLevelTakesOutTheClocksSawtooth) {
  DriftControl control(480, 480, 48000);
  // Right after a tick, its 480 frames aren't due yet.
  EXPECT_DOUBLE_EQ(control.VirtualLevel(1000, 0), 520);
  // 5 ms later, half of them are.
  EXPECT_DOUBLE_EQ(control.VirtualLevel(1000, 0.005), 760);
  // Once the next tick is due, the level is the level.
  EXPECT_DOUBLE_EQ(control.VirtualLevel(1000, 0.010), 1000);
  EXPECT_DOUBLE_EQ(control.VirtualLevel(1000, 0.5), 1000);
  // Nonsense times count as "just ticked".
  EXPECT_DOUBLE_EQ(control.VirtualLevel(1000, -1), 520);
  EXPECT_DOUBLE_EQ(control.VirtualLevel(1000, kNaN), 520);
  EXPECT_DOUBLE_EQ(control.VirtualLevel(1000, kInf), 520);
  EXPECT_DOUBLE_EQ(control.VirtualLevel(-5, 1), 0);
}

TEST(MoltNetAudioClockTest, PrimesBeforePlaying) {
  // A 128-frame device reading 128 frames at a time waits for two of its
  // callbacks (one and a cushion) plus 3 ms of margin plus 5 ms: 640.
  DriftControl control(480, 128, 48000);
  EXPECT_FALSE(control.primed());
  ReadPlan plan = control.BeforeRead(639, 1, 128);
  EXPECT_FALSE(plan.play);
  EXPECT_FALSE(control.primed());
  plan = control.BeforeRead(640, 1, 128);
  EXPECT_TRUE(plan.play);
  EXPECT_TRUE(control.primed());
  EXPECT_EQ(plan.drop, 0);
  EXPECT_EQ(plan.drop_if_silent, 0);
  // Once primed it plays whatever the level.
  EXPECT_TRUE(control.BeforeRead(0, 1, 128).play);
  control.Reset();
  EXPECT_FALSE(control.BeforeRead(300, 1, 128).play);

  // A device that takes 2048 frames per callback (in 128-frame reads, back
  // to back) waits for a whole callback and a 20 ms cushion.
  DriftControl big(480, 2048, 48000);
  EXPECT_FALSE(big.BeforeRead(2048 + 960 + 383, 1, 128).play);
  EXPECT_TRUE(big.BeforeRead(2048 + 960 + 384, 1, 128).play);
  // Measured on the clock's continuous schedule.
  EXPECT_FALSE(DriftControl(480, 128, 48000).BeforeRead(640, 0, 128).play);
}

// One read of a whole window (half a second of audio) at a constant virtual
// headroom of `headroom` frames.
constexpr int kWindowFrames = 24000;
ReadPlan ReadWindow(DriftControl& control, double headroom) {
  return control.BeforeRead(static_cast<int>(kWindowFrames + headroom), 1,
                            kWindowFrames);
}

TEST(MoltNetAudioClockTest, ReadRateFollowsTheExcess) {
  DriftControl control(480, 480, 48000);
  EXPECT_DOUBLE_EQ(control.read_rate(), 1);
  // Primes, and starts the first window.
  EXPECT_TRUE(ReadWindow(control, 144 + 72 + 240).play);
  // 1.5 ms above the 3 ms margin: 72 frames / (4 s * 48000).
  ReadWindow(control, 144 + 72);
  ReadWindow(control, 144 + 72);
  EXPECT_NEAR(control.read_rate(), 1 + 72.0 / (4 * 48000), 1e-12);
  // Below the margin it reads slower, at once.
  ReadWindow(control, 144 - 96);
  EXPECT_NEAR(control.read_rate(), 1 - 96.0 / (4 * 48000), 1e-12);
  // More headroom only counts once the last 1.5 s all had it.
  ReadWindow(control, 144 + 2000);
  ReadWindow(control, 144 + 2000);
  EXPECT_NEAR(control.read_rate(), 1 - 96.0 / (4 * 48000), 1e-12);
  ReadWindow(control, 144 + 2000);
  // Never by more than 0.2%.
  EXPECT_DOUBLE_EQ(control.read_rate(), 1.002);
  ReadWindow(control, -5000);
  EXPECT_DOUBLE_EQ(control.read_rate(), 0.998);
}

TEST(MoltNetAudioClockTest, DropsSilentExcess) {
  DriftControl control(480, 480, 48000);
  // 20 ms of excess: offered as a silent drop, and the rate leans in.
  ReadWindow(control, 144 + 960);
  EXPECT_GT(control.read_rate(), 1);
  const ReadPlan plan = control.BeforeRead(128 + 144 + 960, 1, 128);
  EXPECT_TRUE(plan.play);
  EXPECT_EQ(plan.drop, 0);
  EXPECT_EQ(plan.drop_if_silent, 960);
  // Offered once.
  EXPECT_EQ(control.BeforeRead(128 + 144, 1, 128).drop_if_silent, 0);
  // Under a millisecond isn't worth it.
  DriftControl small(480, 480, 48000);
  ReadWindow(small, 144 + 40);
  EXPECT_EQ(small.BeforeRead(128 + 144, 1, 128).drop_if_silent, 0);
}

TEST(MoltNetAudioClockTest, DropsALargeExcessAtOnce) {
  DriftControl control(480, 480, 48000);
  // Primes with 5 ms of excess.
  EXPECT_TRUE(ReadWindow(control, 144 + 240).play);
  // 100 ms more (a device that stalled), once the last 1.5 s all had it:
  // dropped down to the margin.
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(ReadWindow(control, 144 + 4800).drop, 0);
  }
  EXPECT_DOUBLE_EQ(control.read_rate(), 1);
  const ReadPlan plan = control.BeforeRead(128 + 144 + 4800, 1, 128);
  EXPECT_EQ(plan.drop, 4800);
  EXPECT_EQ(plan.drop_if_silent, 0);
  // Never more than the ring holds.
  DriftControl again(480, 480, 48000);
  ReadWindow(again, 144 + 240);
  for (int i = 0; i < 3; ++i) {
    ReadWindow(again, 144 + 4800);
  }
  EXPECT_EQ(again.BeforeRead(1000, 1, 128).drop, 1000);
}

TEST(MoltNetAudioClockTest, KeepsWhatTheDeviceStartedWith) {
  // A device that took 100 ms to start: the audio rendered meanwhile is
  // played late rather than skipped (silence would go at once), and the
  // read rate works the excess off.
  DriftControl control(480, 480, 48000);
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(ReadWindow(control, 144 + 4800 - 48 * i).drop, 0);
  }
  EXPECT_DOUBLE_EQ(control.read_rate(), 1.002);
  // A stall on top of it is dropped.
  for (int i = 0; i < 3; ++i) {
    ReadWindow(control, 144 + 4800 + 2600);
  }
  EXPECT_EQ(control.BeforeRead(20000, 1, 128).drop, 4800 + 2600);
}

TEST(MoltNetAudioClockTest, AnUnderrunRefillsAndWidensTheMargin) {
  DriftControl control(480, 64, 48000);
  EXPECT_DOUBLE_EQ(control.margin_frames(), 144);
  ASSERT_TRUE(control.BeforeRead(10000, 1, 128).play);
  control.OnShortRead();
  EXPECT_FALSE(control.primed());
  // At least 2 ms more.
  EXPECT_DOUBLE_EQ(control.margin_frames(), 144 + 96);
  // It now waits for 128 + 240 + 240 frames.
  EXPECT_FALSE(control.BeforeRead(607, 1, 128).play);
  EXPECT_TRUE(control.BeforeRead(608, 1, 128).play);
  // Up to 20 ms.
  for (int i = 0; i < 20; ++i) {
    control.OnShortRead();
  }
  EXPECT_DOUBLE_EQ(control.margin_frames(), 960);
  // A restart keeps what it learned.
  control.Reset();
  EXPECT_DOUBLE_EQ(control.margin_frames(), 960);

  // A device with a longer callback: a whole callback more, up to a
  // callback.
  DriftControl big(480, 2048, 48000);
  big.OnShortRead();
  EXPECT_DOUBLE_EQ(big.margin_frames(), 2048);
}

TEST(MoltNetAudioClockTest, NonsenseInputsAreSafe) {
  DriftControl bad(-5, -5, kNaN);
  EXPECT_DOUBLE_EQ(bad.read_rate(), 1);
  EXPECT_FALSE(bad.BeforeRead(-100, kNaN, -7).play);
  EXPECT_TRUE(bad.BeforeRead(std::numeric_limits<int>::max(), kInf, 128).play);
  for (int i = 0; i < 100000; ++i) {
    const ReadPlan plan =
        bad.BeforeRead(i % 3 ? std::numeric_limits<int>::max() : -1,
                       i % 2 ? kInf : -kInf,
                       i % 5 ? std::numeric_limits<int>::max() : -3);
    EXPECT_GE(plan.drop, 0);
    EXPECT_GE(plan.drop_if_silent, 0);
    if (i % 7 == 0) {
      bad.OnShortRead();
    }
  }
  EXPECT_GE(bad.read_rate(), 0.998);
  EXPECT_LE(bad.read_rate(), 1.002);
}

// A clock and a device running side by side, the device reading through a
// resampler model (it takes `frames * ratio` input frames per output frame,
// in 128-frame chunks, as media::MultiChannelResampler does), with the
// device's clock off by `drift` and the clock's ticks up to `jitter`
// seconds late. Returns what the device side saw.
struct SimResult {
  int short_reads = 0;
  int hard_drops = 0;
  // Over the second half: frames in the ring before each read, and the
  // read rate.
  double mean_level = 0;
  int max_level = 0;
  double mean_rate = 0;
  double min_rate = 2;
  double max_rate = 0;
};

struct SimDevice {
  int frames;          // per callback, at the device's rate
  double sample_rate;  // the device's rate (the stream's)
  int burst;           // callbacks that arrive back to back
};

SimResult Simulate(const SimDevice& device,
                   double drift,
                   double jitter,
                   double seconds) {
  constexpr double kRate = 48000;
  constexpr int kTick = 480;
  constexpr int kRead = 128;
  const int device_frames = static_cast<int>(std::ceil(
      device.frames * device.burst * kRate / device.sample_rate));
  OutputRing ring(2, OutputRingFrames(kTick, device_frames, kRead, kRate));
  DriftControl control(kTick, device_frames, kRate);
  std::vector<float> chunk_left(kRead);
  std::vector<float> chunk_right(kRead);
  const std::array<std::span<float>, 2> chunk = {chunk_left, chunk_right};
  uint32_t seed = 12345;
  auto random = [&seed] {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) / static_cast<double>(1 << 24);
  };

  SimResult result;
  double level_sum = 0;
  double rate_sum = 0;
  int samples = 0;
  double next_tick_due = 0;
  double next_tick_at = 0;
  double last_tick_due = -1;
  const double device_period =
      device.frames / device.sample_rate * (1 + drift) * device.burst;
  // The device starts a little after the clock, as a stream does.
  double next_device = 0.03;
  double buffered = 0;  // input frames the resampler holds
  const double nominal = kRate / device.sample_rate;
  while (std::min(next_tick_at, next_device) < seconds) {
    if (next_tick_at <= next_device) {
      PushValue(ring, kTick, 0.5f);
      last_tick_due = next_tick_due;
      next_tick_due += kTick / kRate;
      next_tick_at = next_tick_due + jitter * random();
      continue;
    }
    const double now = next_device;
    for (int b = 0; b < device.burst; ++b) {
      const double need = device.frames * nominal * control.read_rate();
      while (buffered < need) {
        const int level_before = ring.level();
        const ReadPlan plan = control.BeforeRead(
            ring.level(), last_tick_due < 0 ? 0 : now - last_tick_due, kRead);
        if (plan.drop > 0) {
          ++result.hard_drops;
          ring.Discard(plan.drop);
        }
        ring.Discard(ring.LeadingSilence(plan.drop_if_silent));
        if (plan.play) {
          if (now > seconds / 2) {
            level_sum += level_before;
            rate_sum += control.read_rate();
            ++samples;
            result.max_level = std::max(result.max_level, level_before);
            result.min_rate = std::min(result.min_rate, control.read_rate());
            result.max_rate = std::max(result.max_rate, control.read_rate());
          }
          if (ring.Pop(chunk, kRead) < kRead) {
            ++result.short_reads;
            control.OnShortRead();
          }
        }
        buffered += kRead;
      }
      buffered -= need;
    }
    next_device += device_period;
  }
  result.mean_level = samples ? level_sum / samples : 0;
  result.mean_rate = samples ? rate_sum / samples : 0;
  return result;
}

constexpr SimDevice kDevices[] = {
    {128, 48000, 1},  {256, 48000, 1},  {441, 44100, 1},  {480, 48000, 1},
    {512, 48000, 1},  {1024, 48000, 1}, {2048, 48000, 1}, {8192, 48000, 1},
    // The audio service converting another rate: two of the renderer's
    // buffers in a row.
    {512, 48000, 2},
};

TEST(MoltNetAudioClockTest, KeepsUpWithEveryDevice) {
  for (const SimDevice& device : kDevices) {
    for (double drift : {0.0, 2e-4, -2e-4}) {
      SCOPED_TRACE(testing::Message()
                   << device.frames << "@" << device.sample_rate << " x"
                   << device.burst << " drift " << drift);
      const SimResult result = Simulate(device, drift, 0.001, 120);
      // Never runs dry once it has filled, never needs to drop audio.
      EXPECT_EQ(result.short_reads, 0);
      EXPECT_EQ(result.hard_drops, 0);
      // Follows the device's clock, wavering by less than 0.1%.
      EXPECT_NEAR(result.mean_rate, 1 + drift, 2e-5);
      EXPECT_GT(result.min_rate, 0.999);
      EXPECT_LT(result.max_rate, 1.001);
      // And holds little more than the device's own buffer and a tick.
      const double device_frames =
          device.frames * device.burst * 48000 / device.sample_rate;
      EXPECT_LT(result.mean_level, device_frames / 2 + 960 + 48 * 3);
      EXPECT_LT(result.max_level, device_frames + 960 + 48 * 12);
    }
  }
}

TEST(MoltNetAudioClockTest, SurvivesALateClock) {
  // Ticks up to 4 ms late: the windows see it and keep more headroom.
  for (const SimDevice& device : kDevices) {
    SCOPED_TRACE(testing::Message() << device.frames << " x" << device.burst);
    const SimResult late = Simulate(device, 1e-4, 0.004, 120);
    EXPECT_EQ(late.short_reads, 0);
    EXPECT_EQ(late.hard_drops, 0);
  }
}

}  // namespace
}  // namespace blink::molt_fingerprinting::moltnet
