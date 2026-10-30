// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser's side of the speech helper protocol, version 1
// (docs/design/SPEECH_HELPER_PROTOCOL.md): what voice mode writes to the
// resident `molt-speech` helper and how it reads what comes back
// (docs/design/TOUCHLESS_BROWSING.md §6.1).
//
//   Browser to helper, on its stdin: frames of a little-endian uint32 type,
//   a little-endian uint32 length and |length| bytes of payload (at most
//   kMaxFramePayloadBytes):
//     1 AUDIO     16 kHz mono signed 16-bit PCM, echo-cancelled
//     2 CONFIG    UTF-8 JSON: wake_phrases, wake_required, hotwords,
//                 vad_silence_ms
//     3 RESET     drop the current utterance
//     4 SHUTDOWN
//   Helper to browser, on its stdout: one JSON object per line, at most
//   kMaxHelperLineBytes, with a "type" of ready, wake, speech_start,
//   partial, final, speech_end or error.
//
// Everything the helper writes is untrusted: a helper a hostile input took
// over may write anything. ParseHelperLine() accepts only a flat JSON
// object of a known type with the fields that type has, each of the right
// kind and within its cap, in valid UTF-8, with no key twice; text comes
// out through SpeechSafeText(). HelperLineReader splits the stream into
// lines and refuses one past the cap; HelperLineBudget refuses a flood.
// Anything refused ends the helper (voice_listener.h).
//
// Hotwords (§6.1, "recognition biased toward the screen") are page text
// too: HotwordsFromPageMap() reduces the page map's list titles and
// channels to letters, digits, spaces and apostrophes (accented Latin
// letters folded, numbers as words), so nothing reaches sherpa-onnx's
// hotword parser that it reads as syntax (VOICE_BENCH.md §5.3).
//
// Std-only (with the std-only page map): molt_ai_unittests, a standalone
// harness and molt_voice_listener_fuzzer build the same code.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LISTENER_PROTOCOL_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LISTENER_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace voice {

struct PageMap;

// ---------------------------------------------------------------------------
// Browser to helper
// ---------------------------------------------------------------------------

inline constexpr uint32_t kSpeechProtocolVersion = 1;

enum class HelperFrameType : uint32_t {
  kAudio = 1,
  kConfig = 2,
  kReset = 3,
  kShutdown = 4,
};

// The largest payload of one frame, and its header.
inline constexpr size_t kMaxFramePayloadBytes = 65536;
inline constexpr size_t kFrameHeaderBytes = 8;

// The audio the helper takes.
inline constexpr int kHelperSampleRate = 16000;

// The frame |type| with |payload|; empty if |payload| is over
// kMaxFramePayloadBytes.
std::string EncodeHelperFrame(HelperFrameType type, std::string_view payload);

// An AUDIO frame of |samples| (little-endian, whatever the host); empty
// if there are more than kMaxFramePayloadBytes / 2.
std::string EncodeAudioFrame(std::span<const int16_t> samples);

// What CONFIG carries.
struct HelperConfig {
  HelperConfig();
  ~HelperConfig();
  HelperConfig(const HelperConfig&);
  HelperConfig& operator=(const HelperConfig&);
  HelperConfig(HelperConfig&&);
  HelperConfig& operator=(HelperConfig&&);

  // Folded phrases ("hey molt"): ReduceSpeechPhrase() output.
  std::vector<std::string> wake_phrases;
  // False: every utterance counts, without the wake phrase.
  bool wake_required = true;
  // ReduceSpeechPhrase() output, most important first.
  std::vector<std::string> hotwords;
  // Silence that ends an utterance.
  int vad_silence_ms = 500;

  friend bool operator==(const HelperConfig&, const HelperConfig&) = default;
};

// Bounds on CONFIG.
inline constexpr size_t kMaxWakePhrases = 4;
inline constexpr size_t kMaxHotwords = 64;
inline constexpr size_t kMaxPhraseBytes = 64;
inline constexpr size_t kMaxPhraseWords = 6;
inline constexpr int kMinVadSilenceMs = 200;
inline constexpr int kMaxVadSilenceMs = 3000;
inline constexpr int kDefaultVadSilenceMs = 500;

// |config| as CONFIG's JSON. Phrases are reduced again (a phrase that
// reduces to nothing is left out), at most kMaxWakePhrases and
// kMaxHotwords are kept, and |vad_silence_ms| is clamped. Always well
// under kMaxFramePayloadBytes.
std::string EncodeConfigJson(const HelperConfig& config);

// |text| reduced to what a hotword or wake phrase may hold: ASCII letters
// and digits, apostrophes inside words, single spaces. The case of ASCII
// words is kept (Kroko's vocabulary is cased, as the screen is); Latin
// letters with accents are folded to their lower-case base letters
// ("Beyoncé" -> "beyonce"); a run of digits becomes words ("1985" ->
// "nineteen eighty five", "4" -> "four"); everything else separates words.
// At most kMaxPhraseWords words and kMaxPhraseBytes bytes (whole words
// only).
std::string ReduceSpeechPhrase(std::string_view text);

// "4" -> "four", "1985" -> "nineteen eighty five", "2022" -> "twenty
// twenty two", "2005" -> "two thousand five", "120" -> "one hundred
// twenty". Digits only, at most 9 of them; empty otherwise.
std::string NumberAsWords(std::string_view digits);

// Hotwords for the page in |map|: each list entry's title, split at its
// separators (" - ", " | ", ": ", brackets, '!', ',', '?'), and its
// channel, from the most prominent list down; reduced, de-duplicated
// whatever their case, at most |max| phrases. A phrase the grammar reads as
// a yes ("Yes", "OK", "Sure") is left out: page text never biases what the
// user answers to a question.
std::vector<std::string> HotwordsFromPageMap(const PageMap& map,
                                             size_t max = kMaxHotwords);

// ---------------------------------------------------------------------------
// Helper to browser
// ---------------------------------------------------------------------------

// A line's cap (without its '\n'), and a text field's (bytes, before
// SpeechSafeText()).
inline constexpr size_t kMaxHelperLineBytes = 4096;
inline constexpr size_t kMaxHelperTextBytes = 1000;
// Other string fields: the engine, a model name, an error code.
inline constexpr size_t kMaxHelperNameBytes = 128;
inline constexpr size_t kMaxHelperModels = 8;

enum class HelperEventType : uint8_t {
  kReady,
  kWake,
  kSpeechStart,
  kPartial,
  kFinal,
  kSpeechEnd,
  kError,
};

std::string_view HelperEventTypeName(HelperEventType type);

struct HelperEvent {
  HelperEvent();
  ~HelperEvent();
  HelperEvent(const HelperEvent&);
  HelperEvent& operator=(const HelperEvent&);
  HelperEvent(HelperEvent&&);
  HelperEvent& operator=(HelperEvent&&);

  HelperEventType type = HelperEventType::kReady;
  // kPartial, kFinal: what was heard, through SpeechSafeText().
  std::string text;
  // kFinal: 0..1.
  double confidence = 0;
  // kReady.
  std::string engine;
  std::vector<std::string> models;
  // kError: a short code ("protocol") and a message, both made safe.
  std::string code;
  std::string message;
  // Every event but ready and error: how many RESET frames the helper had
  // read when it wrote it, and how much audio it had (milliseconds); -1
  // when the line has none.
  int64_t epoch = -1;
  int64_t audio_ms = -1;
};

// Why a line was refused.
enum class HelperLineError : uint8_t {
  kNone,
  kTooLong,
  kInvalidUtf8,
  kNotJson,
  kNotObject,
  kDuplicateKey,
  kUnknownType,
  kMissingField,
  kBadField,
  kFieldTooLong,
};

std::string_view HelperLineErrorName(HelperLineError error);

struct ParsedHelperLine {
  ParsedHelperLine();
  ~ParsedHelperLine();
  ParsedHelperLine(const ParsedHelperLine&);
  ParsedHelperLine& operator=(const ParsedHelperLine&);
  ParsedHelperLine(ParsedHelperLine&&);
  ParsedHelperLine& operator=(ParsedHelperLine&&);

  HelperLineError error = HelperLineError::kNone;
  HelperEvent event;  // when |error| is kNone
};

// One line (without its '\n'; a '\r' before it is allowed). The line must
// be one JSON object (RFC 8259, white space around tokens allowed) whose
// values are strings, numbers, true, false, null or arrays of strings, no
// key twice, with "type" one of the types above and:
//   ready        "engine" (string), optional "models" (string or array of
//                at most kMaxHelperModels strings), optional "protocol"
//                (number: 1)
//   partial      "text" (string, at most kMaxHelperTextBytes)
//   final        "text", "confidence" (number in 0..1)
//   error        "code" (string), optional "message" (string)
// and, optional on every event but ready and error, "epoch" and "audio_ms"
// (whole numbers from 0 to 2^53).
// Other keys are ignored, as long as their values are of those kinds.
// Linear in the line's length.
ParsedHelperLine ParseHelperLine(std::string_view line);

// Splits the helper's stdout into lines.
class HelperLineReader {
 public:
  HelperLineReader();
  ~HelperLineReader();
  HelperLineReader(const HelperLineReader&) = delete;
  HelperLineReader& operator=(const HelperLineReader&) = delete;

  // Takes the next bytes and appends the complete lines in them (without
  // '\n') to |lines|. False once a line is longer than kMaxHelperLineBytes
  // (|lines| then holds the lines before it); the reader stays failed.
  bool Feed(std::string_view bytes, std::vector<std::string>* lines);

  bool failed() const { return failed_; }

 private:
  std::string partial_;
  bool failed_ = false;
};

// How many lines the helper may write: a burst of kLineBurst, then
// kLinesPerSecond on average. The real helper writes a few partials a
// second.
class HelperLineBudget {
 public:
  static constexpr int kLineBurst = 200;
  static constexpr int kLinesPerSecond = 50;

  HelperLineBudget();

  // A line at |now_ms| (monotonic). False once the helper is over its
  // budget.
  bool Take(int64_t now_ms);

 private:
  double tokens_ = kLineBurst;
  int64_t last_ms_ = -1;
};

// The order of the helper's events (SPEECH_HELPER_PROTOCOL.md §4): `ready`
// first, once; after it each `speech_start` is followed by `partial`s,
// `speech_end` and then `final` before anything else starts, and each
// `wake` by exactly one `final` (alone when no command came). Every event
// but `ready` and `error` carries `epoch` and `audio_ms`, which never go
// back; `epoch` never passes the RESETs sent, and a higher one (the helper
// read a RESET) drops whatever was in progress. `error` may come at any
// time. A helper that breaks the order is stopped.
class HelperEventSequence {
 public:
  HelperEventSequence();

  // Whether |event| may come next, |resets_sent| RESET frames having been
  // sent to the helper so far.
  bool Accept(const HelperEvent& event, int64_t resets_sent);

  bool ready() const { return ready_; }

 private:
  bool ready_ = false;
  bool wake_open_ = false;     // a `wake` its `final` hasn't answered yet
  bool in_utterance_ = false;  // from `speech_start` to `final`
  bool ended_ = false;         // `speech_end` came
  int64_t epoch_ = 0;
  int64_t audio_ms_ = 0;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LISTENER_PROTOCOL_H_
