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

}  // namespace
}  // namespace voice
}  // namespace molt_ai
