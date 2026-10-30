// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What the speech helper writes on its stdout (voice_listener_protocol.h),
// as a helper a hostile input took over may write it, and what voice mode
// does with it (voice_session_logic.h); plus page text made into hotwords.
// The first byte picks:
//   0  the bytes are the helper's stdout, read in chunks the next bytes
//      size: every line HelperLineReader gives is within the cap, every
//      event ParseHelperLine() accepts has clean text within its caps and a
//      confidence in 0..1, and a line is refused the same way twice; the
//      events also go through HelperEventSequence, which accepts nothing
//      but an error before `ready`, and nothing after an error;
//   1  the bytes are page text: ReduceSpeechPhrase() gives only ASCII
//      letters and digits, apostrophes inside words and single spaces,
//      within the caps, and
//      CONFIG's JSON (EncodeConfigJson()) holds it without escapes;
//   2  the bytes are a session: helper events (with any lag behind the
//      microphone, or none that fits) and browser changes in any order and
//      at any time; the session never hands over an utterance while it
//      isn't listening, nor one the rules don't let count (while another
//      source speaks, or voice mode's own voice plays with no echo
//      canceller, only the commands that always count), nor one heard
//      cleanly whose final lagged past kMaxHeardLagMs;
//   3  the bytes are audio sent to the helper and audio_ms values it
//      reports: HelperAudioClock never maps one past what was sent or
//      before what it remembers, nor to a time after the audio it knows.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_listener_protocol.h"
#include "chrome/browser/molt_ai/voice/voice_session_logic.h"

namespace {

using molt_ai::voice::BrowserActivity;
using molt_ai::voice::EncodeConfigJson;
using molt_ai::voice::HelperAudioClock;
using molt_ai::voice::HelperConfig;
using molt_ai::voice::HelperEvent;
using molt_ai::voice::HelperEventSequence;
using molt_ai::voice::HelperEventType;
using molt_ai::voice::HelperLineError;
using molt_ai::voice::HelperLineReader;
using molt_ai::voice::IsAlwaysHeardCommand;
using molt_ai::voice::IsValidUtf8;
using molt_ai::voice::kMaxFramePayloadBytes;
using molt_ai::voice::kMaxHeardLagMs;
using molt_ai::voice::kMaxHelperLineBytes;
using molt_ai::voice::kMaxHelperModels;
using molt_ai::voice::kMaxHelperNameBytes;
using molt_ai::voice::kMaxHelperTextBytes;
using molt_ai::voice::kMaxPhraseBytes;
using molt_ai::voice::kMaxPhraseWords;
using molt_ai::voice::ListeningSettings;
using molt_ai::voice::ParsedHelperLine;
using molt_ai::voice::ParseHelperLine;
using molt_ai::voice::ReduceSpeechPhrase;
using molt_ai::voice::SessionActions;
using molt_ai::voice::SessionState;
using molt_ai::voice::VoiceSessionLogic;

// No C0 control, DEL, C1 control, bidi control or zero-width character, and
// no space at either end (decoded here independently of the code under
// test).
void CheckClean(std::string_view text, size_t cap) {
  CHECK_LE(text.size(), cap);
  CHECK(IsValidUtf8(text));
  if (text.empty()) {
    return;
  }
  CHECK_NE(text.front(), ' ');
  CHECK_NE(text.back(), ' ');
  size_t i = 0;
  while (i < text.size()) {
    const auto b0 = static_cast<uint8_t>(text[i]);
    uint32_t cp = b0;
    size_t n = 1;
    if (b0 >= 0xF0) {
      n = 4;
      cp = b0 & 0x07;
    } else if (b0 >= 0xE0) {
      n = 3;
      cp = b0 & 0x0F;
    } else if (b0 >= 0xC0) {
      n = 2;
      cp = b0 & 0x1F;
    }
    for (size_t k = 1; k < n; ++k) {
      cp = (cp << 6) | (static_cast<uint8_t>(text[i + k]) & 0x3F);
    }
    CHECK(cp >= 0x20);
    CHECK(cp < 0x7F || cp > 0x9F);
    CHECK(!(cp >= 0x202A && cp <= 0x202E));
    CHECK(!(cp >= 0x2066 && cp <= 0x2069));
    CHECK(cp != 0x200B && cp != 0x200E && cp != 0x200F && cp != 0xFEFF);
    i += n;
  }
}

void CheckEvent(const HelperEvent& event) {
  CheckClean(event.text, kMaxHelperTextBytes);
  CheckClean(event.engine, kMaxHelperNameBytes);
  CheckClean(event.code, kMaxHelperNameBytes);
  CheckClean(event.message, kMaxHelperTextBytes);
  CHECK_LE(event.models.size(), kMaxHelperModels);
  for (const std::string& model : event.models) {
    CheckClean(model, kMaxHelperNameBytes);
  }
  CHECK(event.confidence >= 0 && event.confidence <= 1);
  CHECK_GE(event.epoch, -1);
  CHECK_GE(event.audio_ms, -1);
  if (event.type != HelperEventType::kPartial &&
      event.type != HelperEventType::kFinal) {
    CHECK(event.text.empty());
  }
}

void FuzzStream(std::string_view data) {
  HelperLineReader reader;
  HelperEventSequence sequence;
  bool errored = false;
  int64_t resets = 0;
  size_t pos = 0;
  while (pos < data.size()) {
    // A chunk of 1..64 bytes, sized by the byte before it.
    const size_t size = 1 + static_cast<uint8_t>(data[pos]) % 64;
    ++pos;
    const std::string_view chunk = data.substr(pos, size);
    pos += chunk.size();
    std::vector<std::string> lines;
    const bool ok = reader.Feed(chunk, &lines);
    CHECK_EQ(ok, !reader.failed());
    for (const std::string& line : lines) {
      CHECK_LE(line.size(), kMaxHelperLineBytes);
      CHECK_EQ(line.find('\n'), std::string::npos);
      const ParsedHelperLine parsed = ParseHelperLine(line);
      const ParsedHelperLine again = ParseHelperLine(line);
      CHECK(parsed.error == again.error);
      if (parsed.error == HelperLineError::kNone) {
        CheckEvent(parsed.event);
        CHECK(parsed.event.text == again.event.text);
        const bool was_ready = sequence.ready();
        const bool accepted = sequence.Accept(parsed.event, resets);
        // Nothing follows an error: the helper exits after one.
        CHECK(!(accepted && errored));
        errored = errored || (accepted && parsed.event.type ==
                                              HelperEventType::kError);
        if (accepted && parsed.event.type != HelperEventType::kError) {
          CHECK(sequence.ready());
          CHECK(was_ready || parsed.event.type == HelperEventType::kReady);
        }
        // The browser sends a RESET now and then.
        resets += line.size() % 2;
      }
    }
    if (!ok) {
      return;
    }
  }
  // The whole input as one line, too.
  const ParsedHelperLine whole = ParseHelperLine(data);
  if (whole.error == HelperLineError::kNone) {
    CheckEvent(whole.event);
  }
}

void FuzzPhrase(std::string_view data) {
  const std::string phrase = ReduceSpeechPhrase(data);
  CHECK_LE(phrase.size(), kMaxPhraseBytes);
  size_t words = phrase.empty() ? 0 : 1;
  for (size_t i = 0; i < phrase.size(); ++i) {
    const char c = phrase[i];
    CHECK((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '\'' || c == ' ');
    if (c == ' ') {
      ++words;
      CHECK(i > 0 && i + 1 < phrase.size() && phrase[i + 1] != ' ');
    }
    if (c == '\'') {
      CHECK(i > 0 && i + 1 < phrase.size());
      CHECK_NE(phrase[i - 1], ' ');
      CHECK_NE(phrase[i + 1], ' ');
    }
  }
  CHECK_LE(words, kMaxPhraseWords);
  CHECK_EQ(ReduceSpeechPhrase(phrase), phrase);

  HelperConfig config;
  config.wake_phrases = {std::string(data)};
  config.hotwords = {std::string(data), phrase};
  const std::string json = EncodeConfigJson(config);
  CHECK_EQ(json.find('\\'), std::string::npos);
  CHECK(IsValidUtf8(json));
}

void FuzzSession(std::string_view data) {
  ListeningSettings settings;
  settings.wake_phrases = {"hey molt"};
  settings.always_listening = !data.empty() && (data[0] & 1);
  VoiceSessionLogic logic(settings);
  int64_t now = 0;
  size_t pos = 1;
  auto next = [&]() -> uint8_t {
    return pos < data.size() ? static_cast<uint8_t>(data[pos++]) : 0;
  };
  auto text = [&]() {
    const size_t size = next() % 24;
    std::string out(data.substr(std::min(pos, data.size()), size));
    pos += size;
    return out;
  };
  // An event's lag behind the microphone: any, negative too (which the
  // logic takes as none), or none that fits.
  auto lag = [&]() -> std::optional<int64_t> {
    const uint8_t byte = next();
    if (byte == 255) {
      return std::nullopt;
    }
    return int64_t{byte} * 40 - 400;
  };
  bool other_speaking = false;
  bool page_audible = false;
  bool echo_cancelled = true;
  bool own_voice_unfiltered = false;
  while (pos < data.size()) {
    const uint8_t op = next();
    now += next() * 37;
    const bool listening_before = logic.running() &&
                                  logic.state() != SessionState::kStarting &&
                                  logic.state() != SessionState::kOff;
    SessionActions actions;
    std::string said;
    std::optional<int64_t> final_lag;
    switch (op % 18) {
      case 0:
        actions = logic.Start(now);
        break;
      case 1:
        actions = logic.Stop(now);
        break;
      case 2:
        actions = logic.OnHelperReady(now);
        break;
      case 3:
        actions = logic.OnHelperLost(now);
        break;
      case 4:
        actions = logic.OnWake(now, lag());
        break;
      case 5:
        actions = logic.OnSpeechStart(now, lag());
        break;
      case 6:
        said = text();
        actions = logic.OnPartial(said, now, lag());
        break;
      case 7: {
        said = text();
        const double confidence = (next() % 101) / 100.0;
        final_lag = lag();
        actions = logic.OnFinal(said, confidence, now, final_lag);
        break;
      }
      case 8:
        actions = logic.OnSpeechEnd(now, lag());
        break;
      case 9:
        actions = logic.OnBrowserActivity(
            static_cast<BrowserActivity>(next() % 3), now);
        break;
      case 10:
        actions = logic.OnBrowserSaid(text(), now);
        break;
      case 11:
        actions = logic.OnAwaitingAnswer(next() & 1, now);
        break;
      case 12: {
        const uint8_t byte = next();
        if (byte & 2) {
          echo_cancelled = byte & 4;
          actions = logic.OnEchoCancellation(echo_cancelled, now);
        } else {
          page_audible = byte & 1;
          actions = logic.OnPageAudible(page_audible, now);
        }
        break;
      }
      case 13: {
        ListeningSettings changed = settings;
        changed.always_listening = next() & 1;
        actions = logic.OnSettingsChanged(changed, now);
        break;
      }
      case 14:
        other_speaking = next() & 1;
        actions = logic.OnOtherSpeech(other_speaking, now);
        break;
      case 15:
        own_voice_unfiltered = next() & 1;
        actions = logic.OnOwnVoiceUnfiltered(own_voice_unfiltered, now);
        break;
      case 16:
        actions = logic.OnOwnSpeechInReference(next() & 1, now);
        break;
      default:
        actions = logic.OnTick(now);
        break;
    }
    if (actions.utterance) {
      // Only what was heard, only while listening, never empty.
      CHECK(listening_before);
      CHECK_EQ(*actions.utterance, said);
      CHECK(!said.empty());
      if (other_speaking || own_voice_unfiltered ||
          (page_audible && !echo_cancelled)) {
        CHECK(IsAlwaysHeardCommand(said));
      }
    }
    if (actions.heard_cleanly) {
      // Never for a final that lagged, or while another source speaks.
      CHECK(actions.utterance.has_value());
      CHECK(final_lag.has_value());
      CHECK_LE(*final_lag, kMaxHeardLagMs);
      CHECK(!other_speaking && !own_voice_unfiltered);
    }
    if (actions.state) {
      CHECK(*actions.state == logic.state());
    }
    if (!logic.running()) {
      CHECK(logic.state() == SessionState::kOff);
      CHECK(!logic.NextDeadline().has_value());
      CHECK(!actions.duck.value_or(false));
    }
  }
}

void FuzzClock(std::string_view data) {
  HelperAudioClock clock;
  size_t pos = 0;
  auto next = [&]() -> uint8_t {
    return pos < data.size() ? static_cast<uint8_t>(data[pos++]) : 0;
  };
  auto wide = [&]() -> int64_t {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value = (value << 8) | next();
    }
    return static_cast<int64_t>(value);
  };
  int64_t sent = 0;
  int64_t captured = 1000;
  // The latest capture time sent since the clock was reset.
  int64_t latest = std::numeric_limits<int64_t>::min();
  int64_t now = 0;
  while (pos < data.size()) {
    const uint8_t op = next();
    now += next() * 13;
    switch (op % 5) {
      case 0: {
        const size_t samples = (size_t{next()} << 8 | next()) % 40000;
        // Capture times mostly go forward, sometimes back.
        captured += int64_t{next()} * 3 - 100;
        clock.OnSent(samples, captured);
        if (samples > 0 && samples <= kMaxFramePayloadBytes / 2) {
          sent += static_cast<int64_t>(samples);
          latest = std::max(latest, captured);
        }
        break;
      }
      case 1:
        clock.Reset();
        sent = 0;
        latest = std::numeric_limits<int64_t>::min();
        break;
      default: {
        const int64_t audio_ms =
            op % 5 == 2 ? wide() : (int64_t{next()} << 8 | next());
        const std::optional<int64_t> at = clock.CapturedAt(audio_ms);
        if (audio_ms < 0 || audio_ms > sent / HelperAudioClock::kSamplesPerMs) {
          CHECK(!at.has_value());
        }
        if (at) {
          CHECK_LE(*at, latest);
        }
        const std::optional<int64_t> lag = clock.LagMs(audio_ms, now);
        CHECK_EQ(lag.has_value(), at.has_value());
        if (lag) {
          CHECK_GE(*lag, 0);
        }
        break;
      }
    }
    CHECK_EQ(clock.sent_samples(), sent);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  // SAFETY: libFuzzer passes |size| valid bytes at |data|.
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const std::string_view rest = input.substr(1);
  switch (static_cast<uint8_t>(input[0]) % 4) {
    case 0:
      FuzzStream(rest);
      break;
    case 1:
      FuzzPhrase(rest);
      break;
    case 2:
      FuzzSession(rest);
      break;
    default:
      FuzzClock(rest);
      break;
  }
  return 0;
}
