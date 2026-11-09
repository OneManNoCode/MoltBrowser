// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_audio_samples.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <set>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

TEST(VoiceAudioSamplesTest, FloatToS16) {
  EXPECT_EQ(FloatToS16(0.0f), 0);
  EXPECT_EQ(FloatToS16(1.0f), 32767);
  EXPECT_EQ(FloatToS16(-1.0f), -32767);
  EXPECT_EQ(FloatToS16(0.5f), 16384);
  EXPECT_EQ(FloatToS16(7.0f), 32767);
  EXPECT_EQ(FloatToS16(-7.0f), -32767);
  EXPECT_EQ(FloatToS16(std::numeric_limits<float>::infinity()), 32767);
  EXPECT_EQ(FloatToS16(-std::numeric_limits<float>::infinity()), -32767);
  EXPECT_EQ(FloatToS16(std::numeric_limits<float>::quiet_NaN()), 0);
}

TEST(VoiceAudioSamplesTest, ChunkerMakesWholeFrames) {
  AudioFrameChunker chunker(4);
  std::vector<std::vector<int16_t>> frames;
  const std::vector<float> first = {0.0f, 1.0f, -1.0f};
  chunker.Add(first, &frames);
  EXPECT_TRUE(frames.empty());
  const std::vector<float> second = {0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  chunker.Add(second, &frames);
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[0], (std::vector<int16_t>{0, 32767, -32767, 16384}));
  EXPECT_EQ(frames[1], (std::vector<int16_t>{0, 0, 0, 0}));
  // One sample (1.0) is waiting; a reset drops it.
  chunker.Reset();
  frames.clear();
  chunker.Add(std::vector<float>(4, 0.25f), &frames);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0], std::vector<int16_t>(4, 8192));
}

TEST(VoiceAudioSamplesTest, DefaultFrameIsFiftyMilliseconds) {
  AudioFrameChunker chunker;
  std::vector<std::vector<int16_t>> frames;
  chunker.Add(std::vector<float>(16000, 0.0f), &frames);
  EXPECT_EQ(frames.size(), 20u);
  EXPECT_EQ(frames[0].size(), kAudioFrameSamples);
}

// The rate of sign changes over [begin, end), as a frequency.
double ZeroCrossingHz(const std::vector<int16_t>& samples,
                      size_t begin,
                      size_t end) {
  int crossings = 0;
  for (size_t i = begin + 1; i < end; ++i) {
    if ((samples[i - 1] < 0) != (samples[i] < 0)) {
      ++crossings;
    }
  }
  return crossings / 2.0 * kEarconSampleRate / static_cast<double>(end - begin);
}

TEST(VoiceAudioSamplesTest, Earcons) {
  std::set<std::vector<int16_t>> distinct;
  for (EarconKind kind :
       {EarconKind::kOff, EarconKind::kWaiting, EarconKind::kListening,
        EarconKind::kThinking, EarconKind::kSpeaking}) {
    const std::vector<int16_t> earcon = MakeEarcon(kind);
    ASSERT_GT(earcon.size(), static_cast<size_t>(kEarconSampleRate / 50));
    EXPECT_LE(earcon.size(), kMaxEarconSamples);
    int peak = 0;
    for (int16_t sample : earcon) {
      peak = std::max(peak, std::abs(static_cast<int>(sample)));
    }
    EXPECT_LE(peak, kMaxEarconLevel);
    EXPECT_GT(peak, 1000);
    // Smooth edges: no click at either end.
    EXPECT_LE(std::abs(earcon.front()), 50);
    EXPECT_LE(std::abs(earcon.back()), 300);
    EXPECT_TRUE(distinct.insert(earcon).second);
  }
  // Listening rises, waiting falls (the zero-crossing rate of the first
  // and last 50 ms).
  const std::vector<int16_t> up = MakeEarcon(EarconKind::kListening);
  const size_t span = kEarconSampleRate / 20;
  EXPECT_LT(ZeroCrossingHz(up, 0, span),
            ZeroCrossingHz(up, up.size() - span, up.size()));
  const std::vector<int16_t> down = MakeEarcon(EarconKind::kWaiting);
  EXPECT_GT(ZeroCrossingHz(down, 0, span),
            ZeroCrossingHz(down, down.size() - span, down.size()));
  // Speaking's blip is the quietest and shortest.
  EXPECT_LT(MakeEarcon(EarconKind::kSpeaking).size(),
            MakeEarcon(EarconKind::kThinking).size());
}

// What the microphone gave lately, slot by slot of capture time.
TEST(VoiceAudioSamplesTest, LevelHistoryHoldsTheLoudestOfEachSlot) {
  MicrophoneLevelHistory history;
  EXPECT_FALSE(history.LoudestBetween(0, 1'000'000).has_value());
  // One second of 10 ms buffers, quiet but for 300 to 500 ms.
  for (int64_t at = 0; at < 1'000'000; at += 10'000) {
    const bool loud = at >= 300'000 && at < 500'000;
    history.Note(at, 10'000, loud ? 0.5f : 0.001f);
  }
  EXPECT_FLOAT_EQ(*history.LoudestBetween(0, 999'999), 0.5f);
  EXPECT_FLOAT_EQ(*history.LoudestBetween(350'000, 400'000), 0.5f);
  EXPECT_FLOAT_EQ(*history.LoudestBetween(0, 249'999), 0.001f);
  EXPECT_FLOAT_EQ(*history.LoudestBetween(550'000, 999'999), 0.001f);
  // Later than what was captured: only what was.
  EXPECT_FLOAT_EQ(*history.LoudestBetween(900'000, 5'000'000), 0.001f);
  EXPECT_FALSE(history.LoudestBetween(2'000'000, 3'000'000).has_value());
  // An empty or backwards span, or one before the clock's start.
  EXPECT_FALSE(history.LoudestBetween(500'000, 400'000).has_value());
  EXPECT_FALSE(history.LoudestBetween(-10, 400'000).has_value());
}

TEST(VoiceAudioSamplesTest, LevelHistoryForgetsWhatIsTooOld) {
  MicrophoneLevelHistory history;
  history.Note(0, 50'000, 0.9f);
  const int64_t held = static_cast<int64_t>(kLevelSlots) * kLevelSlotUs;
  // Quiet since, up to the whole history's length later.
  for (int64_t at = 50'000; at < held + 50'000; at += 50'000) {
    history.Note(at, 50'000, 0.01f);
  }
  // The loud slot was written over: a span reaching back to it isn't known.
  EXPECT_FALSE(history.LoudestBetween(0, held).has_value());
  EXPECT_FLOAT_EQ(*history.LoudestBetween(held - 100'000, held), 0.01f);
  // A buffer noted out of order changes nothing.
  history.Note(held - 100'000, 10'000, 1.0f);
  EXPECT_FLOAT_EQ(*history.LoudestBetween(held - 100'000, held), 0.01f);
  // Nor does a bad one.
  history.Note(held + 50'000, -1, 1.0f);
  history.Note(held + 50'000, 10'000, std::numeric_limits<float>::quiet_NaN());
  EXPECT_FLOAT_EQ(*history.LoudestBetween(held - 100'000, held + 100'000),
                  0.01f);
}

TEST(VoiceAudioSamplesTest, LevelHistoryBuffersSpanningSlots) {
  MicrophoneLevelHistory history;
  // A 120 ms buffer covers three slots; a peak above 1 is 1.
  history.Note(1'000'000, 120'000, 2.0f);
  EXPECT_FLOAT_EQ(*history.LoudestBetween(1'100'000, 1'110'000), 1.0f);
  // A gap in the capture: nothing there.
  history.Note(2'000'000, 10'000, 0.2f);
  EXPECT_FALSE(history.LoudestBetween(1'500'000, 1'600'000).has_value());
  EXPECT_FLOAT_EQ(*history.LoudestBetween(1'500'000, 2'000'000), 0.2f);
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
