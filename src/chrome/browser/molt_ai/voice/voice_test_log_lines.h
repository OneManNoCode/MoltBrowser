// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The lines of voice mode's test log (voice_test_log.h): one JSON object
// per line, for the end-to-end spoken test kit (tools/voice_e2e/,
// docs/VOICE_E2E.md) to read what voice mode heard, understood and said.
//
//   {"t":1790000000123,"w":1,"e":"start","kinds":["unknown",...]}
//   {"t":...,"w":1,"e":"heard","text":"open YouTube","source":"mic",
//    "clean":true,"intent":"open_site"}
//   {"t":...,"w":1,"e":"said","text":"YouTube is open."}
//   {"t":...,"w":1,"e":"activity","activity":"listening","mic":true,
//    "waiting":false}
//   {"t":...,"w":1,"e":"prompt","op":"open","id":3,"kind":"permission"}
//   {"t":...,"w":1,"e":"prompt","op":"close","id":3}
//   {"t":...,"w":1,"e":"confirm","op":"open","what":"press Buy now"}
//   {"t":...,"w":1,"e":"confirm","op":"close"}
//   {"t":...,"w":1,"e":"mic","peak_db":-23,"samples":80000,"dropped":0,
//    "echo":"chrome-wide"}
//   {"t":...,"w":1,"e":"dropped","text":"open YouTube",
//    "reason":"no wake phrase"}
//   {"t":...,"w":1,"e":"map","items":12,"lists":1,"media":1,
//    "selection":false,"selected_text":false,"labels":["Search",...]}
//
// "t" is the wall clock in milliseconds since the Unix epoch (the kit
// compares it with its own clock), "w" the window's number in this run of
// the browser. "start" lists the grammar's command names
// (VoiceCommandKindName()), so the kit can tell a command this build
// doesn't have yet from one that failed.
//
// No line ever holds a confirmation word of the voice gesture
// (voice_gesture.h): what is said with the word reaches the log as its
// twin without it (VoiceController::Observer gets only the twin), and every
// text is masked again here: each word (or two words run together) that is
// a confirmation word of English or Spanish, or the waiting prompt's word,
// becomes "[word]": the word itself (folded: case and accents aside), its
// plural, or a word of six letters or more heard one letter off
// ("harbour"). The gesture's own sound-alike rule (HoldsConfirmationWord())
// is broader than a log can be: it would take "cart" for "garden". Every
// text is cleaned (CleanSpokenText()) and capped first.
//
// Std-only, so molt_ai_unittests and the standalone harness build the same
// code.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_TEST_LOG_LINES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_TEST_LOG_LINES_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace molt_ai {
namespace voice {

// A text is logged up to this many bytes (cut at a character).
inline constexpr size_t kMaxTestLogTextBytes = 2048;
// A "map" line's labels: at most this many, each up to this many bytes.
inline constexpr size_t kMaxTestLogLabels = 40;
inline constexpr size_t kMaxTestLogLabelBytes = 60;

// What stands for a masked confirmation word.
inline constexpr std::string_view kTestLogMaskedWord = "[word]";

// |text| with every word that is a confirmation word (either language's
// list, or |pending_word|, as above) replaced by kTestLogMaskedWord. |text|
// is cleaned and capped first.
std::string MaskConfirmationWords(std::string_view text,
                                  std::string_view pending_word);

// |text| as the body of a JSON string (without the quotes): '"', '\\' and
// every control character escaped. |text| must be valid UTF-8 (cleaned).
std::string TestLogJsonEscape(std::string_view text);

// What the log keeps of the loop at a moment; a line is written for each
// part that changed.
struct TestLogState {
  // "off", "listening", "thinking" or "speaking" (VoiceActivity).
  std::string_view activity = "off";
  // The microphone is on; only the wake phrase counts now.
  bool mic = false;
  bool waiting = false;
  // The browser's prompt or a page's dialog waiting for its answer (0 for
  // none), and its kind (PromptKindName()).
  int prompt_id = 0;
  std::string_view prompt_kind;
  // The loop's own yes-or-no question ("Say yes to press Buy now"), and
  // what it asks about.
  bool confirming = false;
  std::string confirm_what;
};

// The lines of one window's log. Each method returns whole lines, each
// ending in '\n' ("" when there is nothing to write).
class TestLogLines {
 public:
  explicit TestLogLines(int window);
  TestLogLines(const TestLogLines&) = delete;
  TestLogLines& operator=(const TestLogLines&) = delete;
  ~TestLogLines();

  // The window's first line: the grammar's command names.
  std::string Start(int64_t ms, std::span<const std::string_view> kinds) const;

  // An utterance reached the loop: as typed or heard ("typed" or "mic"),
  // whether it was heard cleanly, and the command it was understood as.
  std::string Heard(int64_t ms,
                    std::string_view text,
                    bool from_microphone,
                    bool clean,
                    std::string_view intent,
                    std::string_view pending_word) const;

  // What voice mode said, as it starts saying it.
  std::string Said(int64_t ms, std::string_view text) const;

  // What the page map held when an utterance was planned: its items'
  // number and labels (masked, each capped at kMaxTestLogLabelBytes, at
  // most kMaxTestLogLabels), its lists and media, and whether the page's
  // tree had a selection and its text could be read.
  struct MapSummary {
    size_t items = 0;
    size_t lists = 0;
    size_t media = 0;
    bool selection = false;
    bool selected_text = false;
    std::span<const std::string> labels;
  };
  std::string Map(int64_t ms,
                  const MapSummary& map,
                  std::string_view pending_word) const;

  // What the microphone gave over the last few seconds: its loudest
  // sample in dBFS (-120 for silence), how many samples, and the echo
  // cancellation it has ("chrome-wide", "platform" or "none"; "off"
  // before it starts).
  std::string Microphone(int64_t ms,
                         float peak,
                         int64_t samples,
                         int64_t dropped,
                         std::string_view echo) const;

  // What the listening session set aside and why ("no wake phrase"),
  // masked as every text is.
  std::string Dropped(int64_t ms,
                      std::string_view text,
                      std::string_view reason,
                      std::string_view pending_word) const;

  // A speech helper event the listening session saw ("wake",
  // "speech_start", "speech_end"), how far its audio was behind the
  // microphone (-1 unknown) and, for a wake, what became of it ("taken",
  // or why it was set aside): where a command went missing.
  std::string Helper(int64_t ms,
                     std::string_view event,
                     int64_t lag_ms,
                     std::string_view note) const;

  // An earcon voice mode played ("listening", "waiting", ...).
  std::string Earcon(int64_t ms, std::string_view kind) const;

  // Other tabs' media lowered (|on|) or restored while a command is said,
  // in |tabs| tabs (0: none had a media session to lower).
  std::string Duck(int64_t ms, bool on, int tabs) const;

  // The lines for what changed since the last call (the first call compares
  // with a TestLogState{}).
  std::string Changed(int64_t ms, const TestLogState& now);

 private:
  std::string Head(int64_t ms, std::string_view event) const;

  const int window_;
  // What was last written.
  std::string activity_ = "off";
  bool mic_ = false;
  bool waiting_ = false;
  int prompt_id_ = 0;
  bool confirming_ = false;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_TEST_LOG_LINES_H_
