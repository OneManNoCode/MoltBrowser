// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/speech_helper/listener.h"

#include <stdint.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_speech {
namespace {

#define WS "\xE2\x96\x81"

constexpr int64_t Ms(int64_t ms) {
  return ms * kSampleRate / 1000;
}

// The audio the tests feed carries its own position: sample i has the
// value i / 16 (so a fake engine knows where any sample it is given came
// from, to the millisecond, up to 32 s).
int64_t PositionOf(float sample) {
  return static_cast<int64_t>(sample * 32768.0f + 0.5f) * 16;
}

struct Word {
  std::string text;
  int64_t at;  // Sample where it starts.
};

// What the fake world sounds like.
struct Scene {
  // Stretches of speech [start, end), in samples.
  std::vector<std::pair<int64_t, int64_t>> speech;
  // Where the spotter fires.
  std::vector<int64_t> wakes;
  std::vector<Word> words;
};

// What the listener asked of the engines.
struct EngineLog {
  std::vector<std::string> keyword_sets;
  std::vector<int> vad_silences;
  std::vector<std::string> utterance_hotwords;
  // Where each utterance's audio started.
  std::vector<int64_t> utterance_starts;
  int spotter_resets = 0;
  int vad_resets = 0;
  int abandoned = 0;
  bool fail_keywords = false;
};

class FakeEngines : public SpeechEngines {
 public:
  FakeEngines(std::shared_ptr<const Scene> scene, std::shared_ptr<EngineLog> log)
      : scene_(std::move(scene)), log_(std::move(log)) {}

  bool SetKeywords(const std::vector<std::string>& lines) override {
    std::string all;
    for (const std::string& line : lines) {
      all += line + "\n";
    }
    log_->keyword_sets.push_back(all);
    return !log_->fail_keywords;
  }
  bool Spot(const std::vector<float>& samples, size_t* words) override {
    const int64_t from = PositionOf(samples.front());
    const int64_t to = from + static_cast<int64_t>(samples.size());
    for (int64_t at : scene_->wakes) {
      if (at >= from && at < to && at >= spotter_from_) {
        *words = 2;
        return true;
      }
    }
    return false;
  }
  void ResetSpotter() override {
    ++log_->spotter_resets;
    spotter_from_ = position_;
  }
  void SetVadSilenceMs(int ms) override {
    log_->vad_silences.push_back(ms);
    silence_ = Ms(ms);
    // A new detector: it doesn't hear speech that ended before it.
    vad_from_ = position_;
  }
  VadUpdate Detect(const std::vector<float>& samples) override {
    const int64_t from = PositionOf(samples.front());
    const int64_t to = from + static_cast<int64_t>(samples.size());
    position_ = to;
    VadUpdate update;
    for (const auto& [start, end] : scene_->speech) {
      if (end <= vad_from_) {
        continue;
      }
      // The detector notices speech 100 ms in, and ends it |silence_|
      // after.
      if (to > start + Ms(100) && to <= end + silence_) {
        update.in_speech = true;
      }
      const int64_t ends = end + silence_;
      if (ends >= from && ends < to && ends > vad_from_) {
        update.segment_ended = true;
      }
    }
    return update;
  }
  void ResetVad() override {
    ++log_->vad_resets;
    vad_from_ = position_;
  }
  void BeginUtterance(const std::string& hotwords) override {
    log_->utterance_hotwords.push_back(hotwords);
    utterance_ = true;
    first_ = -1;
    last_ = -1;
  }
  void Recognize(const std::vector<float>& samples) override {
    if (!utterance_ || samples.empty()) {
      return;
    }
    if (first_ < 0) {
      first_ = PositionOf(samples.front());
      log_->utterance_starts.push_back(first_);
    }
    last_ = PositionOf(samples.back());
  }
  RecognizerResult Partial() override { return Heard(); }
  RecognizerResult Finish() override {
    RecognizerResult result = Heard();
    result.confidence = 0.9;
    utterance_ = false;
    return result;
  }
  void AbandonUtterance() override {
    if (utterance_) {
      ++log_->abandoned;
    }
    utterance_ = false;
  }

 private:
  RecognizerResult Heard() const {
    RecognizerResult result;
    if (!utterance_ || first_ < 0) {
      return result;
    }
    for (const Word& word : scene_->words) {
      // A word is heard once 200 ms of it went in.
      if (word.at >= first_ && word.at + Ms(200) <= last_) {
        result.text += (result.text.empty() ? "" : " ") + word.text;
        result.word_seconds.push_back(static_cast<float>(word.at - first_) /
                                      kSampleRate);
      }
    }
    return result;
  }

  const std::shared_ptr<const Scene> scene_;
  const std::shared_ptr<EngineLog> log_;
  int64_t silence_ = Ms(500);
  int64_t position_ = 0;
  int64_t spotter_from_ = 0;
  int64_t vad_from_ = -1;
  bool utterance_ = false;
  int64_t first_ = -1;
  int64_t last_ = -1;
};

Vocabularies TestVocabularies() {
  Vocabularies v;
  std::string error;
  EXPECT_TRUE(v.kws_pieces.Load(WS "HE -7.9\nY -6.6\n" WS "MO -9.1\nL -6\n"
                                "T -3.6\n" WS "HELLO -9\n",
                                &error));
  EXPECT_TRUE(LoadTokenSet(WS "HE 1\nY 2\n" WS "MO 3\nL 4\nT 5\n" WS
                           "HELLO 6\n",
                           &v.kws_tokens, &error));
  std::string asr = WS " -2\n";
  std::string tokens = WS " 0\n";
  for (char c = 'A'; c <= 'z'; ++c) {
    if (c > 'Z' && c < 'a') {
      continue;
    }
    asr += std::string(1, c) + " -5\n";
    tokens += std::string(1, c) + " " + std::to_string(c) + "\n";
  }
  EXPECT_TRUE(v.asr_pieces.Load(asr, &error));
  EXPECT_TRUE(LoadTokenSet(tokens, &v.asr_tokens, &error));
  return v;
}

class SpeechListenerTest : public testing::Test {
 protected:
  void SetUp() override {
    auto engines = std::make_unique<FakeEngines>(scene_, log_);
    listener_ = std::make_unique<Listener>(
        std::move(engines), TestVocabularies(),
        [this](const std::string& line) { events_.push_back(line); });
    std::string error;
    ASSERT_TRUE(listener_->Start(&error)) << error;
  }

  // Feeds audio up to |until| (samples), in 20 ms frames.
  void FeedUntil(int64_t until) {
    std::vector<int16_t> frame;
    while (fed_ < until) {
      frame.clear();
      for (int i = 0; i < Ms(20) && fed_ < until; ++i, ++fed_) {
        frame.push_back(static_cast<int16_t>(fed_ / 16));
      }
      listener_->Feed(frame.data(), frame.size());
    }
  }

  // The events' types, with the text of partial and final ones.
  std::vector<std::string> Types() const {
    std::vector<std::string> out;
    for (const std::string& line : events_) {
      const size_t type_at = line.find("\"type\":\"") + 8;
      std::string type = line.substr(type_at, line.find('"', type_at) - type_at);
      const size_t text_at = line.find("\"text\":\"");
      if (text_at != std::string::npos) {
        const size_t from = text_at + 8;
        type += ":" + line.substr(from, line.find('"', from) - from);
      }
      out.push_back(type);
    }
    return out;
  }

  ConfigUpdate Config(const std::string& json) {
    ConfigUpdate update;
    std::string error;
    EXPECT_EQ(ParseConfigFrame(json, &update, &error), ConfigStatus::kOk);
    return update;
  }

  const std::shared_ptr<Scene> scene_ = std::make_shared<Scene>();
  const std::shared_ptr<EngineLog> log_ = std::make_shared<EngineLog>();
  std::vector<std::string> events_;
  std::unique_ptr<Listener> listener_;
  int64_t fed_ = 0;
};

TEST_F(SpeechListenerTest, StartsWithTheDefaultWakePhrase) {
  ASSERT_EQ(log_->keyword_sets.size(), 1u);
  EXPECT_EQ(log_->keyword_sets[0], WS "HE Y " WS "MO L T #0.15 @HEY_MOLT\n");
  EXPECT_EQ(log_->vad_silences, std::vector<int>{500});
  EXPECT_TRUE(events_.empty());
}

// "Hey Molt" (1.0 to 1.65 s), the spotter firing 0.28 s later, the command
// right after the phrase.
void WakePhrase(Scene* scene) {
  scene->words = {{"Hey", Ms(1000)}, {"Molt", Ms(1250)}};
  scene->wakes = {Ms(1930)};
}

TEST_F(SpeechListenerTest, HearsAWakePhraseAndACommandInOneBreath) {
  WakePhrase(scene_.get());
  scene_->speech = {{Ms(1000), Ms(2700)}};
  scene_->words.push_back({"open", Ms(1700)});
  scene_->words.push_back({"YouTube", Ms(2050)});
  FeedUntil(Ms(4000));
  EXPECT_EQ(Types(), (std::vector<std::string>{
                         "wake", "speech_start", "partial:Hey Molt open",
                         "partial:Hey Molt open YouTube", "speech_end",
                         "final:Hey Molt open YouTube"}));
  // The recognizer heard the wake phrase too (the grammar drops it), but
  // not the silence long before it.
  ASSERT_EQ(log_->utterance_starts.size(), 1u);
  EXPECT_LE(log_->utterance_starts[0], Ms(1000));
  EXPECT_GE(log_->utterance_starts[0], Ms(1000) - Listener::kPrerollSamples);
  EXPECT_NE(events_.back().find("\"confidence\":0.90"), std::string::npos);
  // speech_end is written when the detector ends the speech (0.5 s after).
  EXPECT_NE(events_[4].find("\"audio_ms\":3220"), std::string::npos);
}

TEST_F(SpeechListenerTest, HearsAShortCommandInTheSameBreath) {
  WakePhrase(scene_.get());
  scene_->speech = {{Ms(1000), Ms(2000)}};
  scene_->words.push_back({"stop", Ms(1700)});
  FeedUntil(Ms(3500));
  EXPECT_EQ(Types(), (std::vector<std::string>{
                         "wake", "speech_start", "partial:Hey Molt stop",
                         "speech_end", "final:Hey Molt stop"}));
}

TEST_F(SpeechListenerTest, CountsWordsWhenTheSpotterIsLate) {
  WakePhrase(scene_.get());
  scene_->wakes = {Ms(2300)};
  scene_->speech = {{Ms(1000), Ms(2000)}};
  scene_->words.push_back({"stop", Ms(1700)});
  FeedUntil(Ms(3500));
  // "stop" starts before the cut, but three words outnumber the phrase.
  EXPECT_EQ(Types(), (std::vector<std::string>{"wake", "speech_start",
                                               "speech_end",
                                               "final:Hey Molt stop"}));
}

TEST_F(SpeechListenerTest, WaitsForTheCommandAfterAPause) {
  WakePhrase(scene_.get());
  scene_->speech = {{Ms(1000), Ms(1650)}, {Ms(3000), Ms(3800)}};
  scene_->words.push_back({"open", Ms(3050)});
  scene_->words.push_back({"YouTube", Ms(3350)});
  FeedUntil(Ms(5000));
  EXPECT_EQ(Types(), (std::vector<std::string>{
                         "wake", "speech_start", "partial:open",
                         "partial:open YouTube", "speech_end",
                         "final:open YouTube"}));
  // The command's audio starts a little before the detector noticed it,
  // after the wake phrase's own speech.
  ASSERT_EQ(log_->utterance_starts.size(), 2u);
  EXPECT_LE(log_->utterance_starts[1], Ms(3000));
  EXPECT_GE(log_->utterance_starts[1], Ms(2400));
}

TEST_F(SpeechListenerTest, AnswersAWakeWithNothingAfterIt) {
  WakePhrase(scene_.get());
  scene_->speech = {{Ms(1000), Ms(1650)}};
  FeedUntil(Ms(9000));
  EXPECT_EQ(Types(), (std::vector<std::string>{"wake", "final:"}));
  EXPECT_NE(events_[1].find("\"confidence\":0.00"), std::string::npos);
  // Six seconds after the wake.
  EXPECT_NE(events_[1].find("\"audio_ms\":7940"), std::string::npos);
  // Then it waits for the wake phrase again: speech alone does nothing.
  scene_->speech.push_back({Ms(9500), Ms(10500)});
  scene_->words.push_back({"hello", Ms(9500)});
  FeedUntil(Ms(12000));
  EXPECT_EQ(events_.size(), 2u);
}

TEST_F(SpeechListenerTest, IgnoresSpeechWithoutTheWakePhrase) {
  scene_->speech = {{Ms(500), Ms(2000)}};
  scene_->words = {{"hello", Ms(500)}};
  FeedUntil(Ms(4000));
  EXPECT_TRUE(events_.empty());
  EXPECT_TRUE(log_->utterance_starts.empty());
}

TEST_F(SpeechListenerTest, CountsEveryUtteranceWhenNoWakeIsRequired) {
  listener_->ApplyConfig(Config(R"({"wake_required":false})"), nullptr);
  scene_->speech = {{Ms(1000), Ms(1800)}, {Ms(3000), Ms(3600)}};
  scene_->words = {{"next", Ms(1050)}, {"page", Ms(1400)}, {"stop", Ms(3020)}};
  FeedUntil(Ms(5000));
  EXPECT_EQ(Types(), (std::vector<std::string>{
                         "speech_start", "partial:next", "partial:next page",
                         "speech_end", "final:next page", "speech_start",
                         "partial:stop", "speech_end", "final:stop"}));
  // Each utterance starts before its first word (the pre-roll), and never
  // reaches back into the previous one.
  ASSERT_EQ(log_->utterance_starts.size(), 2u);
  EXPECT_LE(log_->utterance_starts[0], Ms(1050));
  EXPECT_LE(log_->utterance_starts[1], Ms(3020));
  EXPECT_GE(log_->utterance_starts[1], Ms(2300));
}

TEST_F(SpeechListenerTest, ResetDropsTheUtterance) {
  listener_->ApplyConfig(Config(R"({"wake_required":false})"), nullptr);
  scene_->speech = {{Ms(1000), Ms(3000)}, {Ms(5000), Ms(5600)}};
  scene_->words = {{"type", Ms(1000)}, {"hello", Ms(1500)}, {"yes", Ms(5000)}};
  FeedUntil(Ms(2000));
  listener_->Reset();
  EXPECT_EQ(listener_->epoch(), 1);
  EXPECT_EQ(log_->abandoned, 1);
  EXPECT_EQ(log_->vad_resets, 1);
  FeedUntil(Ms(7000));
  // No speech_end or final for the dropped utterance; the next ones carry
  // epoch 1.
  const std::vector<std::string> types = Types();
  ASSERT_GE(types.size(), 2u);
  EXPECT_EQ(std::count(types.begin(), types.end(), "speech_end"),
            std::count(types.begin(), types.end(), "speech_start") - 1);
  EXPECT_NE(events_.back().find("\"epoch\":1"), std::string::npos);
  EXPECT_EQ(Types().back(), "final:yes");
}

TEST_F(SpeechListenerTest, ResetForgetsAWake) {
  WakePhrase(scene_.get());
  scene_->speech = {{Ms(1000), Ms(1650)}};
  FeedUntil(Ms(2000));
  listener_->Reset();
  FeedUntil(Ms(10000));
  // No final answers the dropped wake.
  EXPECT_EQ(Types(), std::vector<std::string>{"wake"});
}

TEST_F(SpeechListenerTest, AppliesHotwordsAndSilenceFromTheNextUtterance) {
  listener_->ApplyConfig(Config(R"({"wake_required":false,
                                    "hotwords":["Queen", "AC/DC:9"]})"),
                         nullptr);
  scene_->speech = {{Ms(1000), Ms(2000)}, {Ms(4000), Ms(4500)}};
  scene_->words = {{"play", Ms(1000)}, {"Queen", Ms(4000)}};
  FeedUntil(Ms(1500));
  // Mid-utterance: neither applies yet.
  EXPECT_EQ(listener_->ApplyConfig(
                Config(R"({"hotwords":["Bohemian Rhapsody"],
                           "vad_silence_ms":900})"),
                nullptr),
            Listener::ConfigResult::kOk);
  EXPECT_EQ(log_->vad_silences, std::vector<int>{500});
  FeedUntil(Ms(6000));
  EXPECT_EQ(log_->vad_silences, (std::vector<int>{500, 900}));
  ASSERT_EQ(log_->utterance_hotwords.size(), 2u);
  // The test vocabulary has no digits: "9" is dropped.
  EXPECT_EQ(log_->utterance_hotwords[0], "Queen/AC DC");
  EXPECT_EQ(log_->utterance_hotwords[1], "Bohemian Rhapsody");
}

TEST_F(SpeechListenerTest, SwitchesWakeModesBetweenUtterances) {
  scene_->speech = {{Ms(1000), Ms(1600)}};
  scene_->words = {{"pause", Ms(1000)}};
  FeedUntil(Ms(500));
  listener_->ApplyConfig(Config(R"({"wake_required":false})"), nullptr);
  FeedUntil(Ms(2500));
  EXPECT_EQ(Types().back(), "final:pause");
  listener_->ApplyConfig(Config(R"({"wake_required":true})"), nullptr);
  scene_->speech.push_back({Ms(3000), Ms(3600)});
  scene_->words.push_back({"play", Ms(3000)});
  FeedUntil(Ms(5000));
  EXPECT_EQ(Types().back(), "final:pause");
}

TEST_F(SpeechListenerTest, ChangesWakePhrases) {
  EXPECT_EQ(listener_->ApplyConfig(
                Config(R"({"wake_phrases":["hello molt","hey molt"]})"),
                nullptr),
            Listener::ConfigResult::kOk);
  ASSERT_EQ(log_->keyword_sets.size(), 2u);
  EXPECT_EQ(log_->keyword_sets[1],
            WS "HELLO " WS "MO L T #0.15 @HELLO_MOLT\n" WS "HE Y " WS
               "MO L T #0.15 @HEY_MOLT\n");
  std::string error;
  EXPECT_EQ(listener_->ApplyConfig(Config(R"({"wake_phrases":["hi molt"]})"),
                                   &error),
            Listener::ConfigResult::kConfigError);
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(log_->keyword_sets.size(), 2u);
  log_->fail_keywords = true;
  EXPECT_EQ(listener_->ApplyConfig(Config(R"({"wake_phrases":["hey molt"]})"),
                                   &error),
            Listener::ConfigResult::kEngineError);
}

TEST_F(SpeechListenerTest, EndsAnUtteranceAtTwentySeconds) {
  listener_->ApplyConfig(Config(R"({"wake_required":false})"), nullptr);
  scene_->speech = {{Ms(1000), Ms(30000)}};
  scene_->words = {{"blah", Ms(1000)}};
  FeedUntil(Ms(25000));
  const std::vector<std::string> types = Types();
  ASSERT_GE(types.size(), 4u);
  EXPECT_EQ(types[0], "speech_start");
  const auto end = std::find(types.begin(), types.end(), "speech_end");
  ASSERT_NE(end, types.end());
  const size_t i = static_cast<size_t>(end - types.begin());
  // 20 s after its first sample (the pre-roll included).
  EXPECT_NE(events_[i].find("\"audio_ms\":20"), std::string::npos);
  // The speech goes on: a new utterance starts right away.
  EXPECT_EQ(types[i + 2], "speech_start");
}

}  // namespace
}  // namespace molt_speech
