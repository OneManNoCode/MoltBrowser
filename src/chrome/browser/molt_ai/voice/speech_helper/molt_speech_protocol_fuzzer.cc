// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-speech's stdin on any bytes (docs/design/SPEECH_HELPER_PROTOCOL.md
// §2): the frames, CONFIG's JSON, the hotwords and wake phrases it carries,
// RESET, and the listening states fed by deterministic fake engines that
// react to the audio. The first byte picks how the stream is cut into
// reads; the rest is the stream.
//
// Checks: frames come out whole and within the limits, an error ends the
// stream for good; a CONFIG that parses has in-range fields and parses the
// same again; the hotwords argument holds only letters, digits,
// apostrophes, single spaces and '/' between phrases, within its caps; a
// keyword line is pieces, the threshold and a label only; event lines are
// at most 4096 bytes with one newline and no control bytes, their text
// well-formed UTF-8; the events follow §4's sequence: speech_start, then
// speech_end and final before anything else starts, a final for every
// wake, nothing for an utterance after RESET, epoch and audio_ms never go
// back.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "chrome/browser/molt_ai/voice/speech_helper/frame_reader.h"
#include "chrome/browser/molt_ai/voice/speech_helper/listener.h"
#include "chrome/browser/molt_ai/voice/speech_helper/speech_config.h"
#include "chrome/browser/molt_ai/voice/speech_helper/speech_events.h"
#include "chrome/browser/molt_ai/voice/speech_helper/speech_text.h"

namespace molt_speech {
namespace {

#define WS "\xE2\x96\x81"

// Well-formed UTF-8 without control characters, decoded independently.
void CheckCleanUtf8(std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    const auto b0 = static_cast<uint8_t>(text[i]);
    size_t n = 1;
    uint32_t c = b0;
    if (b0 >= 0xF0) {
      n = 4;
      c = b0 & 0x07;
    } else if (b0 >= 0xE0) {
      n = 3;
      c = b0 & 0x0F;
    } else if (b0 >= 0xC0) {
      n = 2;
      c = b0 & 0x1F;
    } else {
      CHECK_LT(b0, 0x80);
    }
    CHECK_LE(i + n, text.size());
    for (size_t k = 1; k < n; ++k) {
      const auto b = static_cast<uint8_t>(text[i + k]);
      CHECK_EQ(b & 0xC0, 0x80);
      c = (c << 6) | (b & 0x3F);
    }
    const uint32_t min[] = {0, 0, 0x80, 0x800, 0x10000};
    CHECK_GE(c, min[n]);
    CHECK_LE(c, 0x10FFFFu);
    CHECK(c < 0xD800 || c > 0xDFFF);
    CHECK(c >= 0x20 && !(c >= 0x7F && c <= 0x9F) && c != 0x2028 &&
          c != 0x2029);
    i += n;
  }
}

// An event line: bounded, one trailing newline, no raw control bytes, and
// any "text" well-formed.
void CheckLine(const std::string& line) {
  CHECK_LE(line.size(), kMaxEventLineBytes);
  CHECK(!line.empty() && line.back() == '\n');
  for (size_t i = 0; i + 1 < line.size(); ++i) {
    CHECK_GE(static_cast<uint8_t>(line[i]), 0x20);
  }
  const size_t text = line.find("\"text\":\"");
  if (text != std::string::npos) {
    std::string value;
    for (size_t i = text + 8; i < line.size(); ++i) {
      if (line[i] == '\\') {
        value.push_back(line[++i]);
      } else if (line[i] == '"') {
        break;
      } else {
        value.push_back(line[i]);
      }
    }
    CHECK_LE(value.size(), kMaxEventTextBytes);
    CheckCleanUtf8(value);
  }
}

Vocabularies FuzzVocabularies() {
  Vocabularies v;
  std::string error;
  // Single letters come from the loop below.
  std::string kws = WS " -4\n" WS "HE -7.9\n" WS "MO -9.1\n";
  std::string kws_tokens = WS " 1\n" WS "HE 2\n" WS "MO 4\n";
  std::string asr = WS " -2\n" WS "the -1.8\n";
  std::string asr_tokens = WS " 0\n" WS "the 1\n";
  int id = 10;
  for (int c = 0x21; c < 0x7F; ++c) {
    const std::string piece(1, static_cast<char>(c));
    if (c >= 'A' && c <= 'Z') {
      kws += piece + " -6\n";
      kws_tokens += piece + " " + std::to_string(id) + "\n";
    }
    asr += piece + " -5\n";
    asr_tokens += piece + " " + std::to_string(id++) + "\n";
  }
  CHECK(v.kws_pieces.Load(kws, &error));
  CHECK(LoadTokenSet(kws_tokens, &v.kws_tokens, &error));
  CHECK(v.asr_pieces.Load(asr, &error));
  CHECK(LoadTokenSet(asr_tokens, &v.asr_tokens, &error));
  return v;
}

const Vocabularies& TheVocabularies() {
  static const Vocabularies* const vocabularies =
      new Vocabularies(FuzzVocabularies());
  return *vocabularies;
}

// Engines that react to the audio: speech is any step with a sample past a
// quarter of full scale, the spotter fires on the sample value 12345, and
// the recognizer hears one word per loud sample run.
class AudioDrivenEngines : public SpeechEngines {
 public:
  bool SetKeywords(const std::vector<std::string>& lines) override {
    for (const std::string& line : lines) {
      CHECK_EQ(line.find('/'), std::string::npos);
      CHECK_EQ(line.find(':'), std::string::npos);
      CHECK_EQ(line.find('\n'), std::string::npos);
    }
    return true;
  }
  bool Spot(const std::vector<float>& samples, size_t* words) override {
    for (const float s : samples) {
      if (s == 12345.0f / 32768.0f) {
        *words = 2;
        return true;
      }
    }
    return false;
  }
  void ResetSpotter() override {}
  void SetVadSilenceMs(int ms) override {
    CHECK_GE(ms, kMinVadSilenceMs);
    CHECK_LE(ms, kMaxVadSilenceMs);
  }
  VadUpdate Detect(const std::vector<float>& samples) override {
    bool loud = false;
    for (const float s : samples) {
      loud = loud || s > 0.25f || s < -0.25f;
    }
    VadUpdate update;
    update.in_speech = loud;
    update.segment_ended = was_loud_ && !loud;
    was_loud_ = loud;
    return update;
  }
  void ResetVad() override { was_loud_ = false; }
  void BeginUtterance(const std::string& hotwords) override {
    CHECK_LE(hotwords.size(), kMaxHotwordsBytes);
    words_.clear();
    in_utterance_ = true;
    fed_ = 0;
  }
  void Recognize(const std::vector<float>& samples) override {
    CHECK(in_utterance_);
    for (const float s : samples) {
      const bool loud = s > 0.5f;
      if (loud && !word_) {
        words_.push_back(static_cast<float>(fed_) / kSampleRate);
      }
      word_ = loud;
      ++fed_;
    }
  }
  RecognizerResult Partial() override {
    RecognizerResult result;
    for (size_t i = 0; i < words_.size() && i < 600; ++i) {
      result.text += i ? " w\x01\xC3\xA9" : "w";
    }
    result.word_seconds = words_;
    return result;
  }
  RecognizerResult Finish() override {
    CHECK(in_utterance_);
    RecognizerResult result = Partial();
    result.confidence = 0.5;
    in_utterance_ = false;
    return result;
  }
  void AbandonUtterance() override { in_utterance_ = false; }

 private:
  bool was_loud_ = false;
  bool in_utterance_ = false;
  bool word_ = false;
  int64_t fed_ = 0;
  std::vector<float> words_;
};

// Follows the event sequence of §4.
class SequenceChecker {
 public:
  void OnLine(const std::string& line) {
    CheckLine(line);
    const size_t type_at = line.find("\"type\":\"") + 8;
    const std::string type =
        line.substr(type_at, line.find('"', type_at) - type_at);
    const int64_t epoch = Number(line, "\"epoch\":");
    const int64_t audio_ms = Number(line, "\"audio_ms\":");
    CHECK_GE(epoch, epoch_);
    CHECK_GE(audio_ms, audio_ms_);
    if (epoch > epoch_) {
      // A RESET dropped whatever was in progress.
      in_utterance_ = ended_ = wake_open_ = false;
    }
    epoch_ = epoch;
    audio_ms_ = audio_ms;
    if (type == "wake") {
      CHECK(!in_utterance_ && !wake_open_);
      wake_open_ = true;
    } else if (type == "speech_start") {
      CHECK(!in_utterance_);
      in_utterance_ = true;
      ended_ = false;
    } else if (type == "partial") {
      CHECK(in_utterance_ && !ended_);
    } else if (type == "speech_end") {
      CHECK(in_utterance_ && !ended_);
      ended_ = true;
    } else if (type == "final") {
      CHECK((in_utterance_ && ended_) || (!in_utterance_ && wake_open_));
      in_utterance_ = ended_ = wake_open_ = false;
    } else {
      CHECK(false) << "unexpected event " << type;
    }
  }
  void OnReset() { ++resets_; }

 private:
  static int64_t Number(const std::string& line, const char* key) {
    const size_t at = line.find(key);
    CHECK_NE(at, std::string::npos);
    int64_t value = 0;
    for (size_t i = at + std::string(key).size();
         i < line.size() && line[i] >= '0' && line[i] <= '9'; ++i) {
      value = value * 10 + (line[i] - '0');
    }
    return value;
  }

  int64_t epoch_ = 0;
  int64_t audio_ms_ = 0;
  int resets_ = 0;
  bool in_utterance_ = false;
  bool ended_ = false;
  bool wake_open_ = false;
};

void CheckHotwordsArgument(const std::string& arg) {
  CHECK_LE(arg.size(), kMaxHotwordsBytes);
  if (arg.empty()) {
    return;
  }
  CHECK_NE(arg.front(), '/');
  CHECK_NE(arg.back(), '/');
  size_t phrases = 0;
  size_t start = 0;
  while (start <= arg.size()) {
    size_t end = arg.find('/', start);
    if (end == std::string::npos) {
      end = arg.size();
    }
    const std::string phrase = arg.substr(start, end - start);
    ++phrases;
    CHECK(!phrase.empty());
    CHECK_LE(phrase.size(), kMaxHotwordPhraseBytes);
    CHECK_NE(phrase.front(), ' ');
    CHECK_NE(phrase.back(), ' ');
    CHECK_EQ(phrase.find("  "), std::string::npos);
    size_t words = 1;
    for (const char c : phrase) {
      words += c == ' ';
      CHECK((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '\'' || c == ' ');
    }
    CHECK_LE(words, kMaxHotwordWords);
    start = end + 1;
  }
  CHECK_LE(phrases, kMaxHotwordPhrases);
}

void FuzzOne(const uint8_t* data, size_t size) {
  if (size < 1) {
    return;
  }
  // Reads of 1, 2, 3, 5, 8, 64, 1024 bytes or everything at once.
  const size_t read_sizes[] = {1, 2, 3, 5, 8, 64, 1024, kMaxReadBytes};
  const size_t read_size = read_sizes[data[0] % 8];
  ++data;
  --size;

  SequenceChecker checker;
  Listener listener(std::make_unique<AudioDrivenEngines>(), TheVocabularies(),
                    [&checker](const std::string& line) {
                      checker.OnLine(line);
                    });
  std::string error;
  CHECK(listener.Start(&error));

  FrameReader reader;
  Frame frame;
  bool failed = false;
  for (size_t pos = 0; pos < size && !failed; pos += read_size) {
    const size_t n = std::min(read_size, size - pos);
    CHECK(reader.Append(data + pos, n));
    while (true) {
      const FrameReader::Status status = reader.Next(&frame, &error);
      if (status == FrameReader::Status::kNeedMore) {
        break;
      }
      if (status == FrameReader::Status::kError) {
        CHECK(!error.empty());
        CHECK_EQ(reader.Next(&frame, &error), FrameReader::Status::kError);
        failed = true;
        break;
      }
      CHECK_LE(frame.payload.size(), kMaxFramePayloadBytes);
      switch (frame.type) {
        case FrameType::kAudio: {
          CHECK(!frame.payload.empty() && frame.payload.size() % 2 == 0);
          std::vector<int16_t> samples(frame.payload.size() / 2);
          for (size_t i = 0; i < samples.size(); ++i) {
            samples[i] = static_cast<int16_t>(
                static_cast<uint8_t>(frame.payload[2 * i]) |
                (static_cast<uint8_t>(frame.payload[2 * i + 1]) << 8));
          }
          listener.Feed(samples.data(), samples.size());
          break;
        }
        case FrameType::kConfig: {
          ConfigUpdate update;
          std::string config_error;
          const ConfigStatus status =
              ParseConfigFrame(frame.payload, &update, &config_error);
          ConfigUpdate again;
          std::string again_error;
          CHECK(ParseConfigFrame(frame.payload, &again, &again_error) ==
                status);
          CHECK_EQ(config_error, again_error);
          if (status != ConfigStatus::kOk) {
            CHECK(!config_error.empty());
            failed = true;
            break;
          }
          const SpeechConfig& v = update.values;
          if (update.has_wake_phrases) {
            CHECK(!v.wake_phrases.empty());
            CHECK_LE(v.wake_phrases.size(), kMaxWakePhrases);
            for (const std::string& phrase : v.wake_phrases) {
              std::string again_phrase;
              CHECK(NormalizeWakePhrase(phrase, &again_phrase));
              CHECK_EQ(again_phrase, phrase);
              std::string line;
              if (WakeKeywordLine(phrase, TheVocabularies().kws_pieces,
                                  TheVocabularies().kws_tokens, &line)) {
                CHECK_EQ(line.find('/'), std::string::npos);
                CHECK_NE(line.find(" #0.15 @"), std::string::npos);
              }
            }
          }
          if (update.has_hotwords) {
            CHECK_LE(v.hotwords.size(), kMaxConfigHotwords);
            CheckHotwordsArgument(HotwordsArgument(
                v.hotwords, TheVocabularies().asr_pieces,
                TheVocabularies().asr_tokens));
          }
          if (update.has_vad_silence_ms) {
            CHECK_GE(v.vad_silence_ms, kMinVadSilenceMs);
            CHECK_LE(v.vad_silence_ms, kMaxVadSilenceMs);
          }
          const Listener::ConfigResult applied =
              listener.ApplyConfig(update, &config_error);
          if (applied != Listener::ConfigResult::kOk) {
            failed = true;
          }
          break;
        }
        case FrameType::kReset:
          checker.OnReset();
          listener.Reset();
          break;
        case FrameType::kShutdown:
          failed = true;  // The helper exits.
          break;
      }
      if (failed) {
        break;
      }
    }
  }

  // The event writers on the stream's bytes as text.
  const std::string_view text(reinterpret_cast<const char*>(data),
                              std::min<size_t>(size, 8192));
  EventTime time;
  CheckLine(PartialEvent(time, text));
  CheckLine(FinalEvent(time, text, 0.5));
  CheckLine(ErrorEvent(text, text));
  const std::string clean = CleanEventText(text, kMaxEventTextBytes);
  CHECK_LE(clean.size(), kMaxEventTextBytes);
  CheckCleanUtf8(clean);
  CHECK_EQ(CleanEventText(clean, kMaxEventTextBytes), clean);
}

}  // namespace
}  // namespace molt_speech

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  molt_speech::FuzzOne(data, size);
  return 0;
}
