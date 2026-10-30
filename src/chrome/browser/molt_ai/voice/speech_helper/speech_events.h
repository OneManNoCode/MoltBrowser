// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The JSON lines molt-speech writes to stdout
// (docs/design/SPEECH_HELPER_PROTOCOL.md §3). Each function returns one
// line with its '\n', at most kMaxEventLineBytes; text fields are
// CleanEventText()ed to at most kMaxEventTextBytes. Std-only.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_SPEECH_EVENTS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_SPEECH_EVENTS_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

namespace molt_speech {

inline constexpr int kProtocolVersion = 1;
inline constexpr size_t kMaxEventLineBytes = 4096;
inline constexpr size_t kMaxEventTextBytes = 1000;
inline constexpr size_t kMaxErrorMessageBytes = 200;

// When an event happened: how many RESET frames the helper had processed,
// and how much audio it had received (ms).
struct EventTime {
  int64_t epoch = 0;
  int64_t audio_ms = 0;
};

std::string ReadyEvent(std::string_view engine,
                       const std::vector<std::string>& models);
std::string WakeEvent(const EventTime& time);
std::string SpeechStartEvent(const EventTime& time);
std::string PartialEvent(const EventTime& time, std::string_view text);
std::string SpeechEndEvent(const EventTime& time);
// |confidence| is clamped to [0, 1] (NaN as 0) and written with two
// decimals.
std::string FinalEvent(const EventTime& time,
                       std::string_view text,
                       double confidence);
// |code| and |message| keep only printable ASCII; the message is cut to
// kMaxErrorMessageBytes.
std::string ErrorEvent(std::string_view code, std::string_view message);

// |text| as a JSON string literal, with its quotes.
std::string JsonQuote(std::string_view text);

}  // namespace molt_speech

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_SPEECH_EVENTS_H_
