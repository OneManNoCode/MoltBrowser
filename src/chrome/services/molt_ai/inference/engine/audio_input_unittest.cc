// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/audio_input.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

TEST(AudioInputTest, Bounds) {
  EXPECT_EQ(16000u, kAudioSampleRate);
  EXPECT_EQ(1600u, kMinAudioSamples);
  EXPECT_EQ(480000u, kMaxAudioSamples);
  EXPECT_EQ(AudioCheck::kOk, CheckModelAudio(kMinAudioSamples * 2));
  EXPECT_EQ(AudioCheck::kOk, CheckModelAudio(kMaxAudioSamples * 2));
  EXPECT_EQ(AudioCheck::kTooShort, CheckModelAudio(kMinAudioSamples * 2 - 2));
  EXPECT_EQ(AudioCheck::kTooShort, CheckModelAudio(0));
  EXPECT_EQ(AudioCheck::kTooLong, CheckModelAudio(kMaxAudioSamples * 2 + 2));
  EXPECT_EQ(AudioCheck::kBadByteLength, CheckModelAudio(kMinAudioSamples * 2 + 1));
  EXPECT_EQ(AudioCheck::kBadByteLength, CheckModelAudio(1));
  EXPECT_EQ(AudioCheck::kTooLong,
            CheckModelAudio(std::numeric_limits<size_t>::max() - 1));
  EXPECT_EQ(AudioCheck::kBadByteLength,
            CheckModelAudio(std::numeric_limits<size_t>::max()));
}

TEST(AudioInputTest, PrepareTakesOnlyMono16k) {
  std::vector<float> s(2000, 0.25f);
  EXPECT_TRUE(PrepareModelAudio(1, 16000, 2000, s.data(), s.size()));
  // Another rate or channel count: Blink resamples and mixes down to what
  // the session asked for, so anything else is a bad renderer.
  EXPECT_FALSE(PrepareModelAudio(2, 16000, 1000, s.data(), s.size()));
  EXPECT_FALSE(PrepareModelAudio(0, 16000, 2000, s.data(), s.size()));
  EXPECT_FALSE(PrepareModelAudio(-1, 16000, 2000, s.data(), s.size()));
  EXPECT_FALSE(PrepareModelAudio(1, 48000, 2000, s.data(), s.size()));
  EXPECT_FALSE(PrepareModelAudio(1, 0, 2000, s.data(), s.size()));
  // The frame count must be the data's.
  EXPECT_FALSE(PrepareModelAudio(1, 16000, 1999, s.data(), s.size()));
  EXPECT_FALSE(PrepareModelAudio(1, 16000, 2001, s.data(), s.size()));
  EXPECT_FALSE(PrepareModelAudio(1, 16000, -2000, s.data(), s.size()));
  EXPECT_FALSE(PrepareModelAudio(1, 16000, 2000, nullptr, s.size()));
  // Empty.
  EXPECT_FALSE(PrepareModelAudio(1, 16000, 0, s.data(), 0));
}

TEST(AudioInputTest, PrepareRefusesLongClips) {
  std::vector<float> s(kMaxAudioSamples + 1, 0.0f);
  EXPECT_FALSE(PrepareModelAudio(1, 16000, static_cast<int64_t>(s.size()),
                                 s.data(), s.size()));
  s.pop_back();
  const auto out = PrepareModelAudio(1, 16000, static_cast<int64_t>(s.size()),
                                     s.data(), s.size());
  ASSERT_TRUE(out);
  EXPECT_EQ(kMaxAudioSamples, out->size());
  EXPECT_EQ(AudioCheck::kOk, CheckModelAudio(out->size() * 2));
  EXPECT_FALSE(PrepareModelAudio(1, 16000, INT64_MAX, s.data(), s.size()));
}

TEST(AudioInputTest, PreparePadsShortClipsWithSilence) {
  const float one = 0.5f;
  const auto out = PrepareModelAudio(1, 16000, 1, &one, 1);
  ASSERT_TRUE(out);
  ASSERT_EQ(kMinAudioSamples, out->size());
  EXPECT_EQ(16384, (*out)[0]);  // round(0.5 * 32767)
  for (size_t i = 1; i < out->size(); ++i) EXPECT_EQ(0, (*out)[i]);
  EXPECT_EQ(AudioCheck::kOk, CheckModelAudio(out->size() * 2));
}

TEST(AudioInputTest, PrepareClampsAndSilencesOddValues) {
  const float in[] = {1.0f,
                      -1.0f,
                      2.0f,
                      -7.5f,
                      std::numeric_limits<float>::quiet_NaN(),
                      std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::denorm_min(),
                      0.0f,
                      -0.0f,
                      1e-5f,
                      0.999999f};
  const size_t n = sizeof(in) / sizeof(in[0]);
  const auto out = PrepareModelAudio(1, 16000, n, in, n);
  ASSERT_TRUE(out);
  const int16_t want[] = {32767, -32767, 32767, -32767, 0, 0, 0, 0, 0, 0, 0,
                          32767};
  for (size_t i = 0; i < n; ++i) EXPECT_EQ(want[i], (*out)[i]) << i;
}

TEST(AudioInputTest, FloatsForTheEncoder) {
  const int16_t s[] = {0, 32767, -32768, -32767, 16384};
  const std::vector<float> f = AudioSamplesToFloat(s, 5);
  ASSERT_EQ(5u, f.size());
  EXPECT_EQ(0.0f, f[0]);
  EXPECT_FLOAT_EQ(32767.0f / 32768.0f, f[1]);
  EXPECT_EQ(-1.0f, f[2]);
  EXPECT_FLOAT_EQ(-32767.0f / 32768.0f, f[3]);
  EXPECT_FLOAT_EQ(0.5f, f[4]);
  for (float v : f) EXPECT_TRUE(v >= -1.0f && v <= 1.0f);
  EXPECT_TRUE(AudioSamplesToFloat(nullptr, 0).empty());
}

TEST(AudioInputTest, RoundTripIsClose) {
  // A sine through the browser's preparation and the engine's conversion
  // comes back within two steps (rounding, and 32767 against 32768).
  std::vector<float> in(16000);
  for (size_t i = 0; i < in.size(); ++i) {
    in[i] = 0.8f * static_cast<float>(std::sin(i * 0.05));
  }
  const auto pcm = PrepareModelAudio(1, 16000, in.size(), in.data(), in.size());
  ASSERT_TRUE(pcm);
  const std::vector<float> back = AudioSamplesToFloat(pcm->data(), pcm->size());
  for (size_t i = 0; i < in.size(); ++i) {
    EXPECT_NEAR(in[i], back[i], 2.0 / 32767.0) << i;
  }
}

}  // namespace
}  // namespace molt_ai
