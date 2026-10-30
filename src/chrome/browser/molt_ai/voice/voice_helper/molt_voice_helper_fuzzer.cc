// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-voice's input on any bytes (docs/design/VOICE_HELPER_PROTOCOL.md §2,
// §4): page-derived text through the text front end (normalization, the
// lexicon, forms, joined words, spelling, letter-to-sound), and the frames
// a browser (or anything that takes its place) writes to the helper's
// stdin, each SPEAK said through a deterministic fake engine. The first
// byte picks the mode and how the stream is cut into reads.
//
// Checks: the sentences are never empty, never start or end with a pause
// or hold two in a row, and stay within kMaxSpokenWords words; a word is
// ASCII letters and inner apostrophes (spelled ones lower-case letters),
// at most kMaxWordLetters; every phoneme is one of the voice's tokens;
// units stay within kMaxUnitCodePoints (plus the "," a cut adds); the
// letter-to-sound output is well-formed UTF-8; frames come out whole and
// within the limits, an error ends the stream for good; each SPEAK gives
// AUDIO frames of its own id within the per-request limit, then one DONE
// with a known status and the audio it wrote.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "chrome/browser/molt_ai/voice/voice_helper/voice_frames.h"
#include "chrome/browser/molt_ai/voice/voice_helper/voice_lts.h"
#include "chrome/browser/molt_ai/voice/voice_helper/voice_phonemes.h"
#include "chrome/browser/molt_ai/voice/voice_helper/voice_synth.h"
#include "chrome/browser/molt_ai/voice/voice_helper/voice_text.h"

namespace molt_voice {
namespace {

// A small lexicon in the real file's format, and the voice's tokens.
constexpr char kLexicon[] =
    "# test lexicon\n"
    "a || ˈ e ɪ\n"
    "book || b ˈ ʊ k\n"
    "browser || b ɹ ˈ aʊ z ɚ\n"
    "hello || h ə lˈ o ʊ\n"
    "molt || m ˈ o ʊ l t\n"
    "open || ˈ o ʊ p ə n\n"
    "play || p lˈ e ɪ\n"
    "the || ð ə\n"
    "watch || w ˈ ɑː t ʃ\n"
    "youtube || jˈ uː t uː b\n";
// Piper's tokens.txt, less a few symbols (so filtering is exercised).
constexpr char kTokens[] =
    "_ 0\n^ 1\n$ 2\n  3\n! 4\n, 8\n. 10\n? 13\na 14\nb 15\nd 17\ne 18\n"
    "f 19\nh 20\ni 21\nj 22\nk 23\nl 24\nm 25\nn 26\no 27\np 28\ns 31\n"
    "t 32\nu 33\nv 34\nw 35\nz 38\næ 39\nð 41\nŋ 44\nɐ 50\nɑ 51\nɔ 54\n"
    "ə 59\nɚ 60\nɛ 61\nɜ 62\nɡ 66\nɪ 74\nɹ 88\nʃ 96\nʊ 100\nʌ 102\n"
    "ˈ 120\nˌ 121\nː 122\nθ 126\nᵻ 128\n";

const Phonemizer& TestPhonemizer() {
  static const Phonemizer* phonemizer = [] {
    Lexicon lexicon;
    std::string error;
    CHECK(lexicon.Load(kLexicon, &error));
    std::vector<uint32_t> tokens;
    CHECK(LoadVoiceTokens(kTokens, &tokens, &error));
    return new Phonemizer(std::move(lexicon), std::move(tokens));
  }();
  return *phonemizer;
}

const std::vector<uint32_t>& Tokens() {
  static const std::vector<uint32_t>* tokens = [] {
    auto* t = new std::vector<uint32_t>();
    std::string error;
    CHECK(LoadVoiceTokens(kTokens, t, &error));
    return t;
  }();
  return *tokens;
}

void CheckUtf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const auto b0 = static_cast<uint8_t>(s[i]);
    size_t n = b0 < 0x80 ? 1 : b0 >= 0xF0 ? 4 : b0 >= 0xE0 ? 3 : 2;
    CHECK(b0 < 0x80 || b0 >= 0xC2);
    CHECK_LE(i + n, s.size());
    for (size_t k = 1; k < n; ++k) {
      CHECK_EQ(static_cast<uint8_t>(s[i + k]) & 0xC0, 0x80);
    }
    i += n;
  }
}

void CheckSentences(const std::vector<SpokenSentence>& sentences) {
  size_t words = 0;
  for (const SpokenSentence& sentence : sentences) {
    CHECK(!sentence.words.empty());
    CHECK(sentence.end == '.' || sentence.end == '?' || sentence.end == '!');
    CHECK(sentence.words.front().kind != WordKind::kPause);
    CHECK(sentence.words.back().kind != WordKind::kPause);
    for (size_t i = 0; i < sentence.words.size(); ++i) {
      const SpokenWord& word = sentence.words[i];
      ++words;
      if (word.kind == WordKind::kPause) {
        CHECK(word.text.empty());
        CHECK(sentence.words[i - 1].kind != WordKind::kPause);
        continue;
      }
      CHECK(!word.text.empty());
      CHECK_LE(word.text.size(), kMaxWordLetters);
      for (size_t k = 0; k < word.text.size(); ++k) {
        const char c = word.text[k];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (word.kind == WordKind::kSpell) {
          CHECK(c >= 'a' && c <= 'z');
        } else if (c == '\'') {
          CHECK_GT(k, 0u);
        } else {
          CHECK(letter);
        }
      }
    }
  }
  CHECK_LE(words, kMaxSpokenWords);
}

void CheckPhonemes(const std::vector<int32_t>& phonemes) {
  const std::vector<uint32_t>& tokens = Tokens();
  for (int32_t cp : phonemes) {
    CHECK(std::binary_search(tokens.begin(), tokens.end(),
                             static_cast<uint32_t>(cp)));
  }
  for (const std::vector<int32_t>& unit : Phonemizer::Units(phonemes)) {
    CHECK(!unit.empty());
    CHECK_LE(unit.size(), kMaxUnitCodePoints + 1);
  }
  for (const std::vector<int32_t>& unit : FirstSentenceUnits(phonemes)) {
    CHECK(!unit.empty());
    CHECK_LE(unit.size(), kMaxUnitCodePoints + 1);
  }
}

void FuzzText(std::string_view text) {
  const std::vector<SpokenSentence> sentences = NormalizeForSpeech(text);
  CheckSentences(sentences);
  for (const SpokenSentence& sentence : sentences) {
    CheckPhonemes(TestPhonemizer().Sentence(sentence));
  }
  // The rules alone, on any bytes.
  CheckUtf8(LetterToSound(text.substr(0, kMaxWordLetters)));
}

// Deterministic: samples follow the phonemes.
class FakeEngine : public VoiceEngine {
 public:
  int sample_rate() const override { return 8000; }
  bool Synthesize(const std::vector<int32_t>& phonemes,
                  float length_scale,
                  const AudioCallback& on_audio) override {
    CHECK_GE(length_scale, 0.39f);
    CHECK_LE(length_scale, 2.01f);
    CHECK(!phonemes.empty());
    CheckPhonemes(phonemes);
    std::vector<float> samples;
    for (int32_t cp : phonemes) {
      for (int i = 0; i < 40; ++i) {
        samples.push_back(static_cast<float>((cp * 37 + i) % 300) / 100.0f -
                          1.5f);
      }
    }
    // Two calls, as an engine may make.
    const size_t half = samples.size() / 2;
    if (!on_audio(samples.data(), half)) {
      return true;
    }
    on_audio(samples.data() + half, samples.size() - half);
    return (phonemes.size() % 7) != 3;  // fails now and then
  }
};

class Hooks : public SynthHooks {
 public:
  explicit Hooks(uint32_t id) : id_(id) {}
  int64_t NowMs() override { return now_ += 3; }
  bool Cancelled() override {
    return cancel_after_ >= 0 && frames_ > cancel_after_;
  }
  void WaitUntil(int64_t until_ms) override {
    CHECK_GE(until_ms, now_ - 3);
    now_ = std::max(now_, until_ms);
  }
  void Emit(std::string frame) override {
    FrameReader reader(FrameReader::Direction::kFromHelper);
    reader.Append(reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
    Frame parsed;
    std::string error;
    CHECK(reader.Next(&parsed, &error) == FrameReader::Status::kFrame);
    CHECK_EQ(parsed.type, static_cast<uint32_t>(FromHelper::kAudio));
    uint32_t id = 0;
    std::vector<int16_t> samples;
    CHECK(ParseAudio(parsed.payload, &id, &samples));
    CHECK_EQ(id, id_);
    CHECK_LE(samples.size(), kAudioChunkSamples);
    samples_ += samples.size();
    ++frames_;
  }
  void set_cancel_after(int frames) { cancel_after_ = frames; }
  uint64_t samples() const { return samples_; }

 private:
  const uint32_t id_;
  int64_t now_ = 0;
  int cancel_after_ = -1;
  int frames_ = 0;
  uint64_t samples_ = 0;
};

void FuzzFrames(const uint8_t* data, size_t size, uint8_t mode) {
  FrameReader reader(FrameReader::Direction::kToHelper);
  FakeEngine engine;
  SynthLimits limits;
  limits.max_request_audio_ms = 20000;
  limits.max_unit_audio_ms = 5000;
  const size_t step = 1 + (mode % 13) * 7;
  bool failed = false;
  for (size_t at = 0; at < size && !failed; at += step) {
    CHECK(reader.Append(data + at, std::min(step, size - at)));
    while (true) {
      Frame frame;
      std::string error;
      const FrameReader::Status status = reader.Next(&frame, &error);
      if (status == FrameReader::Status::kNeedMore) {
        break;
      }
      if (status == FrameReader::Status::kError) {
        CHECK(!error.empty());
        // It stays in error.
        CHECK(reader.Next(&frame, &error) == FrameReader::Status::kError);
        failed = true;
        break;
      }
      CHECK_LE(frame.payload.size(), kMaxFramePayloadBytes);
      if (frame.type != static_cast<uint32_t>(ToHelper::kSpeak)) {
        CHECK(frame.payload.empty());
        continue;
      }
      SpeakRequest request;
      if (!ParseSpeak(frame.payload, &request, &error)) {
        continue;
      }
      CHECK_GE(request.rate_percent, kMinRatePercent);
      CHECK_LE(request.rate_percent, kMaxRatePercent);
      Hooks hooks(request.id);
      hooks.set_cancel_after(mode & 0x80 ? static_cast<int>(mode & 7) : -1);
      const DoneInfo done =
          SpeakRequestNow(request, TestPhonemizer(), engine, hooks, limits);
      CHECK_EQ(done.id, request.id);
      CHECK_LE(static_cast<uint32_t>(done.status), kMaxDoneStatus);
      CHECK_EQ(static_cast<uint64_t>(done.audio_ms), hooks.samples() / 8);
      CHECK_LE(hooks.samples(), 20000u * 8);
      DoneInfo parsed;
      CHECK(ParseDone(EncodeDone(done).substr(kFrameHeaderBytes), &parsed));
      CHECK_EQ(parsed.audio_ms, done.audio_ms);
    }
  }
}

}  // namespace
}  // namespace molt_voice

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1) {
    return 0;
  }
  const uint8_t mode = data[0];
  if (mode & 1) {
    molt_voice::FuzzText(
        std::string_view(reinterpret_cast<const char*>(data + 1), size - 1));
  } else {
    molt_voice::FuzzFrames(data + 1, size - 1, mode);
  }
  return 0;
}
