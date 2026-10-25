// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/model_audio.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "chrome/services/molt_ai/inference/engine/audio_input.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

TEST(ModelAudioTest, FromPage) {
  std::vector<float> s(16000, 0.5f);
  std::optional<ModelAudio> a =
      ModelAudioFromPage(1, 16000, 16000, s.data(), s.size());
  ASSERT_TRUE(a);
  EXPECT_TRUE(a->IsValid());
  EXPECT_EQ(16000u, a->samples.size());
  EXPECT_EQ(1u, a->Seconds());
  // What Blink wasn't asked for, a frame count that isn't the data's.
  EXPECT_FALSE(ModelAudioFromPage(2, 16000, 8000, s.data(), s.size()));
  EXPECT_FALSE(ModelAudioFromPage(1, 44100, 16000, s.data(), s.size()));
  EXPECT_FALSE(ModelAudioFromPage(1, 16000, 15999, s.data(), s.size()));
}

TEST(ModelAudioTest, ValidAndSeconds) {
  EXPECT_FALSE(ModelAudio().IsValid());
  EXPECT_FALSE(ModelAudio(std::vector<int16_t>(kMinAudioSamples - 1)).IsValid());
  EXPECT_TRUE(ModelAudio(std::vector<int16_t>(kMinAudioSamples)).IsValid());
  EXPECT_TRUE(ModelAudio(std::vector<int16_t>(kMaxAudioSamples)).IsValid());
  EXPECT_FALSE(
      ModelAudio(std::vector<int16_t>(kMaxAudioSamples + 1)).IsValid());
  EXPECT_EQ(1u, ModelAudio(std::vector<int16_t>(kMinAudioSamples)).Seconds());
  EXPECT_EQ(30u, ModelAudio(std::vector<int16_t>(kMaxAudioSamples)).Seconds());
  EXPECT_EQ(2u, ModelAudio(std::vector<int16_t>(16001)).Seconds());
}

TEST(ModelAudioTest, PlanRequestAudio) {
  // The new message's clips first; more than the cap is refused.
  EXPECT_FALSE(PlanRequestAudio(kMaxAudioPerRequest + 1, {}));
  EXPECT_EQ(std::vector<size_t>{}, *PlanRequestAudio(kMaxAudioPerRequest, {}));
  // Then the newest turns' while there is room.
  EXPECT_EQ((std::vector<size_t>{0, 1, 2}),
            *PlanRequestAudio(1, {3, 1, 2}));
  EXPECT_EQ((std::vector<size_t>{1, 1, 2}), *PlanRequestAudio(0, {1, 1, 2}));
  EXPECT_EQ((std::vector<size_t>{0, 0, 4}), *PlanRequestAudio(0, {5, 5, 5}));
  EXPECT_EQ((std::vector<size_t>{0, 0, 0}), *PlanRequestAudio(4, {1, 1, 1}));
  // Every split of up to 9 clips over three turns stays within the cap and
  // takes the newest.
  for (size_t u = 0; u <= kMaxAudioPerRequest; ++u) {
    for (size_t a = 0; a <= 3; ++a) {
      for (size_t b = 0; b <= 3; ++b) {
        for (size_t c = 0; c <= 3; ++c) {
          const auto plan = PlanRequestAudio(u, {a, b, c});
          ASSERT_TRUE(plan);
          const size_t sent = (*plan)[0] + (*plan)[1] + (*plan)[2];
          EXPECT_LE(u + sent, kMaxAudioPerRequest);
          EXPECT_EQ(std::min(kMaxAudioPerRequest - u, a + b + c), sent);
          EXPECT_LE((*plan)[2], c);
          if ((*plan)[1] > 0) {
            EXPECT_EQ(c, (*plan)[2]);
          }
          if ((*plan)[0] > 0) {
            EXPECT_EQ(b, (*plan)[1]);
          }
        }
      }
    }
  }
}

}  // namespace
}  // namespace molt_ai
