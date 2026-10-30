// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_helper/voice_synth.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_helper/voice_frames.h"
#include "chrome/browser/molt_ai/voice/voice_helper/voice_phonemes.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_voice {
namespace {

constexpr int kRate = 1000;  // 1 sample per ms: easy arithmetic

// Makes |samples_per_code_point| samples per phoneme, in one call, and
// takes |ms_per_unit| of the fake clock to do it.
class FakeEngine : public VoiceEngine {
 public:
  int sample_rate() const override { return kRate; }

  bool Synthesize(const std::vector<int32_t>& phonemes,
                  float length_scale,
                  const AudioCallback& on_audio) override {
    units.push_back(phonemes);
    scales.push_back(length_scale);
    if (now) {
      *now += ms_per_unit;
    }
    if (fail) {
      return false;
    }
    const std::vector<float> samples(phonemes.size() * samples_per_code_point,
                                     0.5f);
    on_audio(samples.data(), samples.size());
    return true;
  }

  std::vector<std::vector<int32_t>> units;
  std::vector<float> scales;
  size_t samples_per_code_point = 10;
  int64_t ms_per_unit = 5;
  bool fail = false;
  int64_t* now = nullptr;
};

class FakeHooks : public SynthHooks {
 public:
  int64_t NowMs() override { return now; }
  bool Cancelled() override {
    return cancel_after_frames >= 0 &&
           static_cast<int>(frames.size()) >= cancel_after_frames;
  }
  void WaitUntil(int64_t until_ms) override {
    waits.push_back(until_ms - now);
    now = std::max(now, until_ms);
  }
  void Emit(std::string frame) override { frames.push_back(std::move(frame)); }

  size_t Samples() const {
    size_t total = 0;
    for (const std::string& frame : frames) {
      total += (frame.size() - kFrameHeaderBytes - 4) / 2;
    }
    return total;
  }

  int64_t now = 1000;
  int cancel_after_frames = -1;
  std::vector<std::string> frames;
  std::vector<int64_t> waits;
};

Phonemizer MakePhonemizer() {
  Lexicon lexicon;
  std::string error;
  EXPECT_TRUE(lexicon.Load("open || ˈ o ʊ p ə n\nis || ɪ z\n", &error));
  return Phonemizer(std::move(lexicon), {});
}

SpeakRequest Request(std::string text, int rate = 100) {
  SpeakRequest request;
  request.id = 4;
  request.rate_percent = rate;
  request.text = std::move(text);
  return request;
}

TEST(VoiceSynthTest, SpeaksAndReports) {
  const Phonemizer phonemizer = MakePhonemizer();
  FakeEngine engine;
  FakeHooks hooks;
  engine.now = &hooks.now;
  const DoneInfo done = SpeakRequestNow(Request("It is open.", 125), phonemizer,
                                        engine, hooks, SynthLimits());
  EXPECT_EQ(4u, done.id);
  EXPECT_EQ(DoneStatus::kSpoken, done.status);
  ASSERT_EQ(1u, engine.units.size());
  EXPECT_FLOAT_EQ(0.8f, engine.scales[0]);
  EXPECT_EQ(engine.units[0].size() * 10, hooks.Samples());
  EXPECT_EQ(hooks.Samples(), done.audio_ms);
  EXPECT_EQ(5u, done.first_audio_ms);
  EXPECT_EQ(5u, done.synth_ms);
  // Each frame: request 4, at most kAudioChunkSamples.
  for (const std::string& frame : hooks.frames) {
    uint32_t id = 0;
    std::vector<int16_t> samples;
    ASSERT_TRUE(ParseAudio(frame.substr(kFrameHeaderBytes), &id, &samples));
    EXPECT_EQ(4u, id);
    EXPECT_LE(samples.size(), kAudioChunkSamples);
    EXPECT_EQ(ToPcm16(0.5f), samples[0]);
  }
}

TEST(VoiceSynthTest, NothingToSay) {
  const Phonemizer phonemizer = MakePhonemizer();
  FakeEngine engine;
  FakeHooks hooks;
  const DoneInfo done =
      SpeakRequestNow(Request("\xF0\x9F\x8E\x89 ..."), phonemizer, engine,
                      hooks, SynthLimits());
  EXPECT_EQ(DoneStatus::kNothingToSay, done.status);
  EXPECT_TRUE(hooks.frames.empty());
  EXPECT_TRUE(engine.units.empty());
}

TEST(VoiceSynthTest, FirstUnitIsShort) {
  const Phonemizer phonemizer = MakePhonemizer();
  FakeEngine engine;
  FakeHooks hooks;
  std::string text;
  for (int i = 0; i < 40; ++i) {
    text += "it is open ";
  }
  SpeakRequestNow(Request(text), phonemizer, engine, hooks, SynthLimits());
  ASSERT_GT(engine.units.size(), 1u);
  EXPECT_LE(engine.units[0].size(), kFirstUnitCodePoints + 1);
  for (const std::vector<int32_t>& unit : engine.units) {
    EXPECT_LE(unit.size(), kMaxUnitCodePoints + 1);
  }
}

TEST(VoiceSynthTest, PacedAheadOfRealTime) {
  const Phonemizer phonemizer = MakePhonemizer();
  FakeEngine engine;
  engine.samples_per_code_point = 100;  // long audio for each unit
  FakeHooks hooks;
  engine.now = &hooks.now;
  std::string text;
  for (int i = 0; i < 20; ++i) {
    text += "It is open. ";
  }
  SynthLimits limits;
  limits.lookahead_ms = 2000;
  const DoneInfo done =
      SpeakRequestNow(Request(text), phonemizer, engine, hooks, limits);
  EXPECT_EQ(DoneStatus::kSpoken, done.status);
  ASSERT_FALSE(hooks.waits.empty());
  // The written audio never ran more than the lookahead (plus one unit)
  // ahead of the clock.
  EXPECT_GE(hooks.now - 1000 + limits.lookahead_ms +
                static_cast<int64_t>(engine.units[0].size() * 100),
            static_cast<int64_t>(done.audio_ms));
}

TEST(VoiceSynthTest, Cancelled) {
  const Phonemizer phonemizer = MakePhonemizer();
  FakeEngine engine;
  FakeHooks hooks;
  hooks.cancel_after_frames = 0;
  DoneInfo done = SpeakRequestNow(Request("It is open."), phonemizer, engine,
                                  hooks, SynthLimits());
  EXPECT_EQ(DoneStatus::kCancelled, done.status);
  EXPECT_TRUE(hooks.frames.empty());

  // Cancelled after the first unit's audio: no more units.
  FakeEngine engine2;
  FakeHooks hooks2;
  hooks2.cancel_after_frames = 1;
  done = SpeakRequestNow(Request("It is open. It is open. It is open."),
                         phonemizer, engine2, hooks2, SynthLimits());
  EXPECT_EQ(DoneStatus::kCancelled, done.status);
  EXPECT_EQ(1u, engine2.units.size());
}

TEST(VoiceSynthTest, CutAtTheLimits) {
  const Phonemizer phonemizer = MakePhonemizer();
  FakeEngine engine;
  engine.samples_per_code_point = 1000;
  FakeHooks hooks;
  SynthLimits limits;
  limits.max_unit_audio_ms = 3000;
  const DoneInfo done = SpeakRequestNow(Request("It is open."), phonemizer,
                                        engine, hooks, limits);
  EXPECT_EQ(DoneStatus::kCut, done.status);
  EXPECT_EQ(3000u, hooks.Samples());
  EXPECT_EQ(3000u, done.audio_ms);

  FakeEngine engine2;
  engine2.samples_per_code_point = 100;
  FakeHooks hooks2;
  SynthLimits limits2;
  limits2.max_request_audio_ms = 1500;
  const DoneInfo done2 = SpeakRequestNow(
      Request("It is open. It is open. It is open. It is open."), phonemizer,
      engine2, hooks2, limits2);
  EXPECT_EQ(DoneStatus::kCut, done2.status);
  EXPECT_EQ(1500u, hooks2.Samples());
}

TEST(VoiceSynthTest, EngineFailure) {
  const Phonemizer phonemizer = MakePhonemizer();
  FakeEngine engine;
  engine.fail = true;
  FakeHooks hooks;
  const DoneInfo done = SpeakRequestNow(Request("It is open."), phonemizer,
                                        engine, hooks, SynthLimits());
  EXPECT_EQ(DoneStatus::kEngineFailed, done.status);
}

TEST(VoiceSynthTest, Pcm) {
  EXPECT_EQ(0, ToPcm16(0.0f));
  EXPECT_EQ(32767, ToPcm16(1.0f));
  EXPECT_EQ(32767, ToPcm16(7.0f));
  EXPECT_EQ(-32767, ToPcm16(-3.0f));
  EXPECT_EQ(0, ToPcm16(std::numeric_limits<float>::quiet_NaN()));
}

}  // namespace
}  // namespace molt_voice
