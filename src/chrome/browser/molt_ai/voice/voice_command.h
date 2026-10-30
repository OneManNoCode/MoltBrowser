// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The direct-command grammar (docs/design/TOUCHLESS_BROWSING.md sections
// 5.2 and 6.2, layer 1): what a recognized utterance asks for, without a
// language model, in well under a millisecond. "Open YouTube", "search
// for Bohemian Rhapsody", "play the fourth one", "louder", "skip ahead one
// minute", "read the headings", "no, I said ...".
//
// ParseVoiceCommand() cleans the utterance up, then tries the language's
// command patterns (voice_command_phrases.h) in order; the first that
// matches all of it wins. Everything else is kUnknown, for the planner,
// with the text as heard. The grammar decides what was asked, never
// whether it is allowed: a site name or address is only host-like text,
// which the browser resolves and checks with agent_safety
// (IsAllowedNavigationURL()) before going anywhere; a choice reference is
// resolved against the page map by MatchSpokenChoice(); every action
// still goes through the voice controller's confirmations.
//
// Clean-up, in order:
//   - Refused: more than kMaxUtteranceBytes, or invalid UTF-8. Never cut
//     down into something with a different meaning.
//   - Words are folded (spoken_text.h): case, punctuation, apostrophes,
//     accents. "No. 4", "#4" and "50%" become words.
//   - Hesitations go wherever they are ("um", "uh").
//   - A leading wake phrase goes ("Hey Molt", and sound-alikes such as
//     "hey mold"), then politeness and preambles at the start ("could you
//     please", "I want to", "okay") and at the end ("please", "thank you").
//
// Numbers in number slots are digits or words, cardinal or ordinal ("4th",
// "fourth", "number four", "No. 4"), and there sound-alikes count too
// ("number for", "click to"). Time takes the usual forms ("a minute and a
// half"). Linear time: no std::regex, no recursion, patterns are matched
// left to right without backtracking.
//
// Std-only (no //base): unit-tested standalone and fuzzed
// (molt_voice_command_fuzzer).

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_COMMAND_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_COMMAND_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "chrome/browser/molt_ai/voice/spoken_choice.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

// Longer utterances are refused (kRefused), not truncated.
inline constexpr size_t kMaxUtteranceBytes = 1000;
// Free text a command carries (a query, dictated text, a name), in bytes.
inline constexpr size_t kMaxCommandTextBytes = 500;
// A site name or address, in bytes (a host name's limit).
inline constexpr size_t kMaxSiteBytes = 253;
// A routine's name, in bytes.
inline constexpr size_t kMaxRoutineNameBytes = 100;

enum class VoiceCommandKind : uint8_t {
  // Not a direct command: the planner gets |heard|.
  kUnknown,
  // Over kMaxUtteranceBytes or not UTF-8: nothing is done (|refusal|).
  kRefused,

  // --- Navigation ---------------------------------------------------------
  kOpenSite,    // |site|, |site_is_address|, |new_tab|
  kSearch,      // |text| (the query), |scope|, |site| for kNamedSite
  kFindInPage,  // |text|
  kBack,
  kForward,
  kReload,
  kStopLoading,
  kHome,
  kNextPage,  // a result list's next page (or the next screenful)
  kNewTab,
  kCloseTab,   // |choice| names the tab; kNone for the current one
  kSwitchTab,  // |choice|

  // --- Viewing ------------------------------------------------------------
  kScrollDown,  // |scroll|, |count|
  kScrollUp,
  kScrollLeft,
  kScrollRight,
  kScrollToTop,
  kScrollToBottom,
  kZoomIn,
  kZoomOut,
  kZoomReset,

  // --- Media --------------------------------------------------------------
  kPlay,
  kPause,
  kResume,
  kVolumeUp,
  kVolumeDown,
  kSetVolume,  // |level|: 0..100
  kMute,
  kUnmute,
  kSkipForward,  // |seconds|; 0 for the player's default step
  kSkipBack,     // |seconds|; 0 for the player's default step
  kRestart,
  kNextTrack,
  kPreviousTrack,
  kFullScreen,
  kExitFullScreen,
  kCaptionsOn,
  kCaptionsOff,
  kSpeedUp,
  kSlowDown,
  kNormalSpeed,
  kSkipAd,  // the player's "skip ad" button

  // --- Reading ------------------------------------------------------------
  kReadPage,
  kReadFromHere,
  kReadHeadings,
  kReadLinks,
  kReadResults,
  kReadTitle,
  kReadSelection,
  kDescribeScreen,
  kNextHeading,
  kPreviousHeading,
  kNextLink,
  kPreviousLink,
  // The next or previous item of the list being read. A bare "next" /
  // "previous" is this too; the browser may take it as the next track
  // while media plays.
  kNextItem,
  kPreviousItem,
  // The next or previous paragraph of the text being read.
  kNextParagraph,
  kPreviousParagraph,
  kReadSection,  // |text|: the section's name ("history")

  // --- Choosing -----------------------------------------------------------
  kChoose,  // |verb|, |choice|
  kShowNumbers,
  kHideNumbers,

  // --- Dictation (in a focused text field) ------------------------------
  kType,  // |text|
  kNewLine,
  kDeleteLastWord,
  kClearField,
  kReadBack,
  kSubmit,

  // --- Conversation -------------------------------------------------------
  kYes,  // only when ParseContext::expecting_yes_no
  kNo,   // only when ParseContext::expecting_yes_no
  kCancel,
  kStop,  // stop speaking and whatever is running
  kStopSpeaking,
  kUndo,
  kRepeat,
  kHelp,
  kWhereAmI,
  kSpeakFaster,
  kSpeakSlower,
  kVerbosity,   // |verbosity|
  kCorrection,  // |text|: what the user meant, to parse again

  // --- Learning (section 6.8) ---------------------------------------------
  kRememberRoutine,  // |text|: the name
  kForgetRoutine,    // |text|: the name
  kRunRoutine,       // |text|: the name; the browser decides if it exists
  kListRoutines,     // "what are my routines"
  kForgetWord,       // |text|: a learned word or correction to forget
  kForgetLearning,   // forget everything learned (after a spoken "yes")

  // --- Voice mode (listening) ----------------------------------------------
  kVoiceModeOn,   // start listening (typed, or said while voice mode is on)
  kVoiceModeOff,  // stop listening (after a spoken "yes")
};

// The number of kinds, for tables and tests.
inline constexpr size_t kVoiceCommandKindCount =
    static_cast<size_t>(VoiceCommandKind::kVoiceModeOff) + 1;

// A stable name ("kOpenSite" -> "open_site"), for logs and the docs.
std::string_view VoiceCommandKindName(VoiceCommandKind kind);

enum class SearchScope : uint8_t {
  // "search for X": the current site's search box when it has one, else
  // the default search engine.
  kAuto,
  kCurrentSite,    // "search this site for X"
  kDefaultEngine,  // "search the web for X"
  kNamedSite,      // "search YouTube for X", "search for X on YouTube"
};

enum class ChoiceVerb : uint8_t {
  kDefault,  // "the fourth one": the item's own action (play a video,
             // follow a link, press a button, focus a field)
  kOpen,
  kPlay,
  kClick,
  kSelect,
  kRead,
  kDescribe,
};

enum class ScrollAmount : uint8_t {
  kDefault,   // the browser's step
  kPage,      // |count| pages
  kHalfPage,  // "a bit", "half a page"
};

enum class Verbosity : uint8_t {
  kBrief,
  kNormal,
  kFull,
  kMore,  // one step up
  kLess,  // one step down
};

enum class RefusalReason : uint8_t {
  kNone,
  kTooLong,
  kInvalidUtf8,
};

struct ParseContext {
  // The browser just asked a yes/no question: "yes" and "no" answer it.
  bool expecting_yes_no = false;
  // The browser just read a list or shows numbers: a bare number ("four")
  // or a bare description ("the live one") chooses from it.
  bool expecting_choice = false;
  // A text field has focus: the dictation commands ("new line", "clear",
  // "read it back", "submit") apply to it.
  bool text_field_focused = false;
  Language language = Language::kEnglish;
  // The wake phrase the user chose ("Hey Molt", "computer"; folded here);
  // empty for the language's default ("hey molt"). Removed from the start
  // of an utterance, with sound-alikes of its name, when the recognizer
  // wrote it.
  std::string_view wake_phrase;
};

struct VoiceCommand {
  VoiceCommand();
  ~VoiceCommand();
  VoiceCommand(const VoiceCommand&);
  VoiceCommand& operator=(const VoiceCommand&);
  VoiceCommand(VoiceCommand&&);
  VoiceCommand& operator=(VoiceCommand&&);

  VoiceCommandKind kind = VoiceCommandKind::kUnknown;
  // The utterance as heard, without the wake phrase: valid UTF-8, no
  // control, bidi or invisible characters, single spaces, at most
  // kMaxUtteranceBytes. For the caption bar ("Heard: ..."), the planner,
  // and any fallback (a choice nothing matched goes to the planner).
  // Empty for kRefused.
  std::string heard;
  // Words the command carries, as heard (case and punctuation kept, "AC/DC",
  // "node.js"), cleaned like |heard|, at most kMaxCommandTextBytes: the
  // query (kSearch, kFindInPage), the text (kType), the correction
  // (kCorrection), the routine's name (kRememberRoutine, kForgetRoutine,
  // kRunRoutine), the section's name (kReadSection), the word to forget
  // (kForgetWord).
  std::string text;
  // kOpenSite, and kSearch with kNamedSite: the site as host-like text,
  // lower case: "youtube", "new york times" -> "newyorktimes",
  // "example dot com" -> "example.com", "b b c dot co dot uk" ->
  // "bbc.co.uk". Letters, digits, '.', '-', '_' and '/' only. The browser
  // resolves a name (a known site, or a search) and checks every URL with
  // agent_safety; this is not a URL.
  std::string site;
  // The site as said, for readback ("the New York Times").
  std::string site_words;
  // A dot was spoken or written: |site| is an address, not a name.
  bool site_is_address = false;
  // kOpenSite: "... in a new tab".
  bool new_tab = false;
  // kSearch.
  SearchScope scope = SearchScope::kAuto;
  // kSearch "search for X on Y": the whole "X on Y", to search for when Y
  // is not a site the browser knows ("search for things to do on Sunday").
  std::string whole_query;
  // kChoose (and kCloseTab, kSwitchTab).
  ChoiceVerb verb = ChoiceVerb::kDefault;
  ChoiceRef choice;
  // kSkipForward, kSkipBack: seconds, 0 for the default step.
  int seconds = 0;
  // kSetVolume: 0..100.
  int level = 0;
  // kScroll*.
  ScrollAmount scroll = ScrollAmount::kDefault;
  int count = 1;
  // kVerbosity.
  Verbosity verbosity = Verbosity::kNormal;
  // kRefused.
  RefusalReason refusal = RefusalReason::kNone;
};

VoiceCommand ParseVoiceCommand(std::string_view utterance,
                               const ParseContext& context);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_COMMAND_H_
