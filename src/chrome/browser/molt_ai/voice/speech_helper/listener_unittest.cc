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
  // Where each wake phrase starts, if given: the spotter fires for it only
  // if it was given the audio from there since it was last reset.
  std::vector<int64_t> wake_starts;
  std::vector<Word> words;
  // How much of a word the recognizer needs before it writes it.
  int64_t word_lag = 200 * kSampleRate / 1000;
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
    if (spotter_first_ < 0) {
      spotter_first_ = from;
    }
    for (size_t i = 0; i < scene_->wakes.size(); ++i) {
      const int64_t at = scene_->wakes[i];
      const bool whole = i >= scene_->wake_starts.size() ||
                         spotter_first_ <= scene_->wake_starts[i];
      if (at >= from && at < to && at >= spotter_from_ && whole) {
        *words = 2;
        return true;
      }
    }
    return false;
  }
  void ResetSpotter() override {
    ++log_->spotter_resets;
    spotter_from_ = position_;
    spotter_first_ = -1;
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
      // A word is heard once |word_lag| of it went in.
      if (word.at >= first_ && word.at + scene_->word_lag <= last_) {
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
  // The first sample given to the spotter since its reset, or -1.
  int64_t spotter_first_ = -1;
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
  // With "hey" said short, under the same label (WakeKeywordLines()).
  EXPECT_EQ(log_->keyword_sets[0], WS "HE Y " WS "MO L T #0.15 @HEY_MOLT\n" WS
                                   "HE " WS "MO L T #0.15 @HEY_MOLT\n");
  EXPECT_EQ(log_->vad_silences, std::vector<int>{500});
  EXPECT_TRUE(events_.empty());
}

// "Hey Molt" (1.0 to 1.65 s), the spotter firing 0.28 s later, the command
// right after the phrase.
void WakePhrase(Scene* scene) {
  scene->words = {{"Hey", Ms(1000)}, {"Molt", Ms(1250)}};
  scene->wakes = {Ms(1930)};
}

// The wake phrase as the spotter hears it, which the recognizer doesn't
// write as the phrase: only the spotter wakes.
void SpottedOnly(Scene* scene) {
  scene->words = {{"Hello", Ms(1000)}, {"there", Ms(1250)}};
  scene->wakes = {Ms(1930)};
}

// What the events with |type| carry as audio_ms.
std::vector<int64_t> AudioMs(const std::vector<std::string>& events,
                             const std::string& type) {
  std::vector<int64_t> out;
  for (const std::string& line : events) {
    if (line.find("\"type\":\"" + type + "\"") == std::string::npos) {
      continue;
    }
    const size_t at = line.find("\"audio_ms\":");
    if (at == std::string::npos) {
      continue;
    }
    int64_t ms = 0;
    for (size_t i = at + 11; i < line.size() && line[i] >= '0' && line[i] <= '9';
         ++i) {
      ms = ms * 10 + (line[i] - '0');
    }
    out.push_back(ms);
  }
  return out;
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
  SpottedOnly(scene_.get());
  scene_->wakes = {Ms(2300)};
  scene_->speech = {{Ms(1000), Ms(2000)}};
  scene_->words.push_back({"stop", Ms(1700)});
  FeedUntil(Ms(3500));
  // "stop" starts before the cut, but three words outnumber the phrase.
  EXPECT_EQ(Types(), (std::vector<std::string>{"wake", "speech_start",
                                               "speech_end",
                                               "final:Hello there stop"}));
}

TEST_F(SpeechListenerTest, CountsAPhraseRunTogetherAsOneWord) {
  // The spotter fires after "stop" began (before the cut), and before the
  // recognizer wrote it; the recognizer runs the phrase together.
  scene_->word_lag = Ms(600);
  scene_->words = {{"Himalt.", Ms(1000)}, {"stop", Ms(1400)}};
  scene_->wakes = {Ms(1750)};
  scene_->speech = {{Ms(1000), Ms(1700)}};
  FeedUntil(Ms(3500));
  EXPECT_EQ(Types(), (std::vector<std::string>{"wake", "speech_start",
                                               "speech_end",
                                               "final:Himalt. stop"}));
}

TEST_F(SpeechListenerTest, CountsAPhraseWhoseStartWasLost) {
  // As the spotter heard it; the recognizer lost "hey" ("Emold"): "stop"
  // is still the command.
  scene_->word_lag = Ms(600);
  scene_->words = {{"Emold.", Ms(1000)}, {"stop", Ms(1400)}};
  scene_->wakes = {Ms(1750)};
  scene_->speech = {{Ms(1000), Ms(1700)}};
  FeedUntil(Ms(3500));
  EXPECT_EQ(Types(), (std::vector<std::string>{"wake", "speech_start",
                                               "speech_end",
                                               "final:Emold. stop"}));
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
  // after the wake phrase's own speech (which the recognizer heard while
  // the spotter listened, then again from before the phrase).
  ASSERT_EQ(log_->utterance_starts.size(), 3u);
  EXPECT_LE(log_->utterance_starts[2], Ms(3000));
  EXPECT_GE(log_->utterance_starts[2], Ms(2400));
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
  // Heard, for a wake phrase the spotter might have missed; nothing is
  // written.
  EXPECT_TRUE(events_.empty());
  EXPECT_EQ(log_->utterance_starts.size(), 1u);
}

TEST_F(SpeechListenerTest, HearsAWakePhraseTheSpotterMissed) {
  // The spotter never fires; the recognizer writes the phrase.
  scene_->words = {{"Hey,", Ms(1000)}, {"Malt,", Ms(1250)},
                   {"open", Ms(1700)}, {"YouTube", Ms(2050)}};
  scene_->speech = {{Ms(1000), Ms(2700)}};
  FeedUntil(Ms(4000));
  EXPECT_EQ(Types(), (std::vector<std::string>{
                         "wake", "speech_start", "partial:Hey, Malt, open",
                         "partial:Hey, Malt, open YouTube", "speech_end",
                         "final:Hey, Malt, open YouTube"}));
  // Where the phrase ended (the next word started), for the browser's
  // checks of what else was heard then; the command's audio from before
  // the phrase.
  EXPECT_EQ(AudioMs(events_, "wake"), std::vector<int64_t>{1700});
  EXPECT_EQ(AudioMs(events_, "speech_start"), std::vector<int64_t>{1700});
  ASSERT_EQ(log_->utterance_starts.size(), 1u);
  EXPECT_LE(log_->utterance_starts[0], Ms(1000));
}

TEST_F(SpeechListenerTest, HearsTheWakePhraseRunTogether) {
  scene_->words = {{"Himalt.", Ms(1000)}, {"Pause.", Ms(1500)}};
  scene_->speech = {{Ms(1000), Ms(1900)}};
  FeedUntil(Ms(3500));
  EXPECT_EQ(Types(), (std::vector<std::string>{"wake", "speech_start",
                                               "partial:Himalt. Pause.",
                                               "speech_end",
                                               "final:Himalt. Pause."}));
}

TEST_F(SpeechListenerTest, WaitsForTheCommandAfterAPhraseTheSpotterMissed) {
  scene_->words = {{"Hey", Ms(1000)}, {"mold.", Ms(1250)},
                   {"open", Ms(3050)}, {"YouTube", Ms(3350)}};
  scene_->speech = {{Ms(1000), Ms(1650)}, {Ms(3000), Ms(3800)}};
  FeedUntil(Ms(5000));
  EXPECT_EQ(Types(), (std::vector<std::string>{
                         "wake", "speech_start", "partial:open",
                         "partial:open YouTube", "speech_end",
                         "final:open YouTube"}));
  // The wake when the detector ended the phrase's speech (0.5 s on, at the
  // end of the 20 ms frame it was in).
  EXPECT_EQ(AudioMs(events_, "wake"), std::vector<int64_t>{2160});
}

TEST_F(SpeechListenerTest, GivesUpOnSpeechThatDoesntStartWithThePhrase) {
  scene_->words = {{"hey", Ms(1000)},   {"Walt,", Ms(1200)},
                   {"are", Ms(1500)},   {"you", Ms(1700)},
                   {"coming", Ms(1900)}, {"hey", Ms(2300)},
                   {"Molt", Ms(2500)},  {"stop", Ms(2800)}};
  scene_->speech = {{Ms(1000), Ms(3200)}};
  FeedUntil(Ms(5000));
  // Its first words aren't the phrase: the recognizer stops (and the
  // phrase later in the same breath isn't a wake phrase).
  EXPECT_TRUE(events_.empty());
  EXPECT_EQ(log_->abandoned, 1);
  ASSERT_EQ(log_->utterance_starts.size(), 1u);
  // The next stretch of speech is heard again.
  scene_->speech.push_back({Ms(6000), Ms(6900)});
  scene_->words.push_back({"Hey", Ms(6000)});
  scene_->words.push_back({"Molt,", Ms(6250)});
  scene_->words.push_back({"pause", Ms(6600)});
  FeedUntil(Ms(8500));
  EXPECT_EQ(Types().front(), "wake");
  EXPECT_EQ(Types().back(), "final:Hey Molt, pause");
}

TEST_F(SpeechListenerTest, AWindowOpeningMidSpeechCountsIt) {
  scene_->words = {{"next", Ms(1000)}, {"page", Ms(1300)}};
  scene_->speech = {{Ms(1000), Ms(1800)}};
  FeedUntil(Ms(1250));
  listener_->ApplyConfig(Config(R"({"wake_required":false})"), nullptr);
  FeedUntil(Ms(3000));
  EXPECT_EQ(Types(), (std::vector<std::string>{
                         "speech_start", "partial:next", "partial:next page",
                         "speech_end", "final:next page"}));
}

TEST_F(SpeechListenerTest, ASpottedWakeIsUnchanged) {
  // The recognizer doesn't write the phrase; the spotter fires.
  SpottedOnly(scene_.get());
  scene_->speech = {{Ms(1000), Ms(2700)}};
  scene_->words.push_back({"open", Ms(2000)});
  FeedUntil(Ms(4000));
  EXPECT_EQ(Types().front(), "wake");
  EXPECT_EQ(AudioMs(events_, "wake"), std::vector<int64_t>{1940});
  EXPECT_EQ(Types().back(), "final:Hello there open");
  // Heard from the speech's start, then again from before the phrase.
  EXPECT_EQ(log_->abandoned, 1);
  EXPECT_EQ(log_->utterance_starts.size(), 2u);
}

TEST_F(SpeechListenerTest, SpotsAPhraseBegunAsTheWindowCloses) {
  listener_->ApplyConfig(Config(R"({"wake_required":false})"), nullptr);
  SpottedOnly(scene_.get());
  scene_->wake_starts = {Ms(1000)};
  scene_->speech = {{Ms(1000), Ms(2300)}};
  scene_->words.push_back({"pause", Ms(2000)});
  // The browser asks for the wake phrase again 40 ms into it, before the
  // detector noticed the speech: the spotter starts on the audio before.
  FeedUntil(Ms(1040));
  listener_->ApplyConfig(Config(R"({"wake_required":true})"), nullptr);
  FeedUntil(Ms(4000));
  ASSERT_FALSE(Types().empty());
  EXPECT_EQ(Types().front(), "wake");
  EXPECT_EQ(Types().back(), "final:Hello there pause");
}

// The spoken suite's file run: "no, I said YouTube" begun as the follow-up
// window closed, before the detector noticed it, was heard for a wake
// phrase and dropped. Begun while every utterance counted, it counts.
TEST_F(SpeechListenerTest, CountsSpeechBegunAsTheWindowCloses) {
  listener_->ApplyConfig(Config(R"({"wake_required":false})"), nullptr);
  scene_->speech = {{Ms(1000), Ms(2300)}};
  scene_->words = {{"no,", Ms(1000)}, {"I", Ms(1300)}, {"said", Ms(1450)},
                   {"YouTube", Ms(1750)}};
  // The browser asks for the wake phrase again 40 ms into it.
  FeedUntil(Ms(1040));
  listener_->ApplyConfig(Config(R"({"wake_required":true})"), nullptr);
  FeedUntil(Ms(4000));
  EXPECT_EQ(Types(),
            (std::vector<std::string>{"speech_start", "speech_end",
                                      "final:no, I said YouTube"}));
  // When it began, for the browser's window.
  ASSERT_EQ(AudioMs(events_, "speech_start").size(), 1u);
  EXPECT_LE(AudioMs(events_, "speech_start")[0], 1200);
  ASSERT_EQ(log_->utterance_starts.size(), 1u);
  EXPECT_LE(log_->utterance_starts[0], Ms(1000));
  // Speech begun later needs the phrase again.
  scene_->speech.push_back({Ms(5000), Ms(5800)});
  scene_->words.push_back({"next", Ms(5000)});
  scene_->words.push_back({"page", Ms(5300)});
  const size_t before = events_.size();
  FeedUntil(Ms(7000));
  EXPECT_EQ(events_.size(), before);
}

TEST_F(SpeechListenerTest, SpeechBegunAfterTheWindowClosedNeedsThePhrase) {
  listener_->ApplyConfig(Config(R"({"wake_required":false})"), nullptr);
  scene_->speech = {{Ms(1500), Ms(2300)}};
  scene_->words = {{"next", Ms(1500)}, {"page", Ms(1800)}};
  FeedUntil(Ms(1000));
  listener_->ApplyConfig(Config(R"({"wake_required":true})"), nullptr);
  FeedUntil(Ms(4000));
  EXPECT_TRUE(events_.empty());
}

// The spoken suite's runs: "Hey Molt, select [pause] by letter" ended at
// the pause, and "by letter" came as an utterance of its own, heard from
// its middle ("I looked by letter"). A pause after one word is waited out.
TEST_F(SpeechListenerTest, WaitsOutAPauseAfterOneWord) {
  listener_->ApplyConfig(Config(R"({"wake_required":false})"), nullptr);
  scene_->speech = {{Ms(1000), Ms(1500)}, {Ms(2100), Ms(2800)}};
  scene_->words = {{"select", Ms(1000)}, {"by", Ms(2150)},
                   {"letter", Ms(2400)}};
  FeedUntil(Ms(5000));
  const std::vector<std::string> types = Types();
  EXPECT_EQ(types.back(), "final:select by letter");
  EXPECT_EQ(std::count(types.begin(), types.end(), "speech_start"), 1);
  ASSERT_EQ(log_->utterance_starts.size(), 1u);
}

TEST_F(SpeechListenerTest, WaitsOutAPauseAfterTheWakePhraseAndOneWord) {
  scene_->speech = {{Ms(1000), Ms(1900)}, {Ms(2500), Ms(3300)}};
  scene_->words = {{"Hey", Ms(1000)},     {"Molt,", Ms(1250)},
                   {"skip", Ms(1600)},    {"ahead", Ms(2550)},
                   {"one", Ms(2850)},     {"minute", Ms(3050)}};
  FeedUntil(Ms(6000));
  const std::vector<std::string> types = Types();
  EXPECT_EQ(types.front(), "wake");
  EXPECT_EQ(types.back(), "final:Hey Molt, skip ahead one minute");
  EXPECT_EQ(std::count(types.begin(), types.end(), "speech_start"), 1);
}

TEST_F(SpeechListenerTest, ALongerCommandEndsAtItsPause) {
  listener_->ApplyConfig(Config(R"({"wake_required":false})"), nullptr);
  scene_->speech = {{Ms(1000), Ms(1800)}, {Ms(2400), Ms(2900)}};
  scene_->words = {{"scroll", Ms(1000)}, {"down", Ms(1400)},
                   {"stop", Ms(2450)}};
  FeedUntil(Ms(5000));
  EXPECT_EQ(Types(), (std::vector<std::string>{
                         "speech_start", "partial:scroll",
                         "partial:scroll down", "speech_end",
                         "final:scroll down", "speech_start", "partial:stop",
                         "speech_end", "final:stop"}));
}

TEST_F(SpeechListenerTest, KeepsTheDetectorWhenItsSilenceDoesntChange) {
  // Every CONFIG carries the silence; a new detector forgets the speech it
  // was following.
  for (int i = 0; i < 3; ++i) {
    listener_->ApplyConfig(
        Config(i % 2 ? R"({"vad_silence_ms":500,"wake_required":true})"
                     : R"({"vad_silence_ms":500,"wake_required":false})"),
        nullptr);
  }
  EXPECT_EQ(log_->vad_silences, std::vector<int>{500});
  listener_->ApplyConfig(Config(R"({"vad_silence_ms":700})"), nullptr);
  listener_->ApplyConfig(Config(R"({"vad_silence_ms":700})"), nullptr);
  EXPECT_EQ(log_->vad_silences, (std::vector<int>{500, 700}));
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
  // A one-word command waits kShortCommandHangoverSamples longer.
  FeedUntil(Ms(2900));
  EXPECT_EQ(Types().back(), "final:pause");
  listener_->ApplyConfig(Config(R"({"wake_required":true})"), nullptr);
  // Begun well after (not within kLateOpenSamples of) the switch.
  scene_->speech.push_back({Ms(3500), Ms(4100)});
  scene_->words.push_back({"play", Ms(3500)});
  FeedUntil(Ms(5500));
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
               "MO L T #0.15 @HEY_MOLT\n" WS "HE " WS
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

TEST_F(SpeechListenerTest, KeepsTheSpotterWhenTheWakePhrasesDontChange) {
  // Every CONFIG the browser sends carries the wake phrases (the follow-up
  // window opens and closes): the spotter is built at start only.
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(listener_->ApplyConfig(
                  Config(i % 2 ? R"({"wake_phrases":["hey molt"],)"
                                 R"("wake_required":true})"
                               : R"({"wake_phrases":["Hey  Molt"],)"
                                 R"("wake_required":false})"),
                  nullptr),
              Listener::ConfigResult::kOk);
  }
  EXPECT_EQ(log_->keyword_sets.size(), 1u);
  // A wake phrase heard across such a CONFIG still wakes it.
  scene_->speech = {{Ms(1000), Ms(1600)}};
  scene_->wakes = {Ms(1500)};
  FeedUntil(Ms(1200));
  EXPECT_EQ(listener_->ApplyConfig(Config(R"({"wake_phrases":["hey molt"]})"),
                                   nullptr),
            Listener::ConfigResult::kOk);
  FeedUntil(Ms(3000));
  ASSERT_FALSE(Types().empty());
  EXPECT_EQ(Types().front(), "wake");
  EXPECT_EQ(log_->keyword_sets.size(), 1u);
  // New phrases rebuild it; the same ones again don't.
  listener_->ApplyConfig(Config(R"({"wake_phrases":["hello molt"]})"),
                         nullptr);
  listener_->ApplyConfig(Config(R"({"wake_phrases":["hello molt"]})"),
                         nullptr);
  EXPECT_EQ(log_->keyword_sets.size(), 2u);
  // After a failed rebuild, the next CONFIG tries again.
  log_->fail_keywords = true;
  EXPECT_EQ(listener_->ApplyConfig(Config(R"({"wake_phrases":["hey molt"]})"),
                                   nullptr),
            Listener::ConfigResult::kEngineError);
  log_->fail_keywords = false;
  EXPECT_EQ(listener_->ApplyConfig(Config(R"({"wake_phrases":["hey molt"]})"),
                                   nullptr),
            Listener::ConfigResult::kOk);
  EXPECT_EQ(log_->keyword_sets.size(), 4u);
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
