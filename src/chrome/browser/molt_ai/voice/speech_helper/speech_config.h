// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-speech's settings and the CONFIG frame that changes them
// (docs/design/SPEECH_HELPER_PROTOCOL.md §2.2): a JSON object read by a
// small strict parser (RFC 8259, no extensions), with limits on nesting and
// string length. Std-only, no exceptions: the helper, molt_ai_unittests and
// molt_speech_protocol_fuzzer build the same code.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_SPEECH_CONFIG_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_SPEECH_CONFIG_H_

#include <stddef.h>

#include <string>
#include <string_view>
#include <vector>

namespace molt_speech {

inline constexpr size_t kMaxWakePhrases = 8;
inline constexpr size_t kMaxWakePhraseBytes = 64;
inline constexpr size_t kMaxWakePhraseWords = 4;
inline constexpr size_t kMaxConfigHotwords = 1000;
inline constexpr int kMinVadSilenceMs = 100;
inline constexpr int kMaxVadSilenceMs = 3000;
inline constexpr int kDefaultVadSilenceMs = 500;
// JSON limits: deeper nesting, or a longer string (after unescaping), is a
// protocol error.
inline constexpr int kMaxJsonDepth = 8;
inline constexpr size_t kMaxJsonStringBytes = 1024;

struct SpeechConfig {
  SpeechConfig();
  SpeechConfig(const SpeechConfig&);
  SpeechConfig& operator=(const SpeechConfig&);
  ~SpeechConfig();

  // Normalized (NormalizeWakePhrase()): lower case, single spaces.
  std::vector<std::string> wake_phrases;
  bool wake_required = true;
  // As the browser sent them; HotwordsArgument() cleans them.
  std::vector<std::string> hotwords;
  int vad_silence_ms = kDefaultVadSilenceMs;
};

// The fields one CONFIG frame sets; the others keep their values.
struct ConfigUpdate {
  ConfigUpdate();
  ConfigUpdate(const ConfigUpdate&);
  ConfigUpdate& operator=(const ConfigUpdate&);
  ~ConfigUpdate();

  bool has_wake_phrases = false;
  bool has_wake_required = false;
  bool has_hotwords = false;
  bool has_vad_silence_ms = false;
  SpeechConfig values;
};

enum class ConfigStatus {
  kOk,
  // Not JSON, not an object, nesting or a string past the limits, or a
  // duplicate field: error code "protocol".
  kProtocolError,
  // A known field of the wrong type or out of range, or a wake phrase that
  // breaks the rules: error code "config".
  kConfigError,
};

// The defaults before the first CONFIG: wake phrase "hey molt", required.
SpeechConfig DefaultSpeechConfig();

// Parses a CONFIG frame's payload. On an error, |error| says what, without
// echoing the payload. Fields this version doesn't know are skipped.
ConfigStatus ParseConfigFrame(std::string_view json,
                              ConfigUpdate* update,
                              std::string* error);

// Copies the fields |update| sets into |config|.
void ApplyConfigUpdate(const ConfigUpdate& update, SpeechConfig* config);

// A wake phrase as CONFIG may give it: 1 to kMaxWakePhraseBytes of ASCII
// letters, apostrophes and spaces, 1 to kMaxWakePhraseWords words, each with
// a letter. |out| is lower case with single spaces. Whether the spotter's
// vocabulary can spell it is checked later (WakeKeywordLine()).
bool NormalizeWakePhrase(std::string_view phrase, std::string* out);

}  // namespace molt_speech

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_SPEECH_CONFIG_H_
