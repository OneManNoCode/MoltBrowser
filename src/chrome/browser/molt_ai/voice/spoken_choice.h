// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The spoken-choice matcher (docs/design/TOUCHLESS_BROWSING.md section
// 6.4): which item on screen the user meant. "The fourth one", "number
// 12", "the next one", "the live one by Queen", "the sign-in button".
//
// The browser reads a list aloud ("1: Bohemian Rhapsody, official video,
// Queen, 6 minutes. 2: ...") or shows numbers on the page; the page map
// (page_map.h) gives the items as ChoiceCandidates; the grammar
// (voice_command.h) or ParseChoiceRef() turns what the user said into a
// ChoiceRef; MatchSpokenChoice() picks the item. No language model, a
// few milliseconds for a page of results.
//
// The outcome is unique (act on it), ambiguous (the browser reads the
// close ones back and asks: "Did you mean 2, the live one at Live Aid, or
// 3, live at Wembley?") or no match (the browser says so, or hands the
// request to the planner, which ranks the candidates itself). Every
// outcome carries the ranked candidates and their scores.
//
// How a description is scored (MatchSpokenChoice()):
//   - Words are folded as spoken_text.h folds them (case, punctuation, a
//     small Latin diacritics fold). Stopwords ("the", "one", "of") and
//     role words ("video", "button") are not description words; number
//     words become digits ("nineteen eighty five" -> 1985).
//   - Each description word is compared with every word of each item's
//     title and fields: exactly (1.0), as a plural (0.95), or by sound
//     (phonetic_key.h: at most 0.9, less with each edit). Words are also
//     compared joined across two or three words on either side, so a
//     recognizer that split or merged words still matches ("Bohemian rap
//     city" -> "Bohemian Rhapsody", "signin" -> "Sign in").
//   - When some item has a word exactly as spelled, other items' sound-
//     alikes of it count less: the recognizer is biased toward the words
//     on screen, so "AirPods" means the AirPods, not the earbuds.
//   - Words that set items apart weigh more than words they share. A word
//     every item has ("official" on every result) counts little; one that
//     matches nothing counts a fixed, moderate amount, so a stray word
//     lowers confidence without ruling anything out. Short particles ("in",
//     "with") count only when an item has them ("Log in" / "Log out").
//   - "by X" is matched against the author fields (channel, artist,
//     seller), and against the title at a discount; "from X" against the
//     source, date and other fields and the title. A cue nothing matches
//     is read as plain description words ("Stand by me").
//   - An item's score is the weighted mean of its words' matches, in
//     [0, 1]. Below kMinChoiceScore nothing matches; a lead of at least
//     kChoiceMargin over the next item makes the best one unique;
//     otherwise the items within the margin are ambiguous.
//
// Not settled here: descriptions about things the items don't say ("the
// shortest one", "the newest"). Those score low (no match) or tie
// (ambiguous); the browser then asks the user or the planner.
//
// Std-only (no //base): unit-tested standalone and fuzzed
// (molt_spoken_choice_fuzzer).

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_CHOICE_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_CHOICE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

// ---------------------------------------------------------------------------
// The items on screen
// ---------------------------------------------------------------------------

// What an item is, as the page map reports it. Coarse on purpose: the
// matcher only uses it for role words ("the first video", "the last link",
// "the sign-in button"). A role word rules out items of other roles only
// when at least one item has a role it names; otherwise it is ignored
// ("the third song" on a list of videos is the third video).
//
// From the accessibility tree: link -> kLink; button, toggle button, menu
// button, pop-up button -> kButton; text field, search box, text area,
// editable combo box -> kTextField; check box, switch -> kCheckBox; radio
// button -> kRadioButton; list box, select, combo box without text entry
// -> kListBox; option, menu item -> kOption; tab -> kTab; image -> kImage;
// heading -> kHeading; article -> kArticle. An item of a repeated list the
// page map detected (a search result, a card, a row) is kVideo, kAudio,
// kArticle or kProduct when the page map can tell what it is, kListItem
// otherwise. Browser tabs, offered for "switch to the YouTube tab", are
// kTab.
enum class ChoiceRole : uint8_t {
  kOther = 0,  // none of these: never chosen or ruled out by a role word
  kListItem,
  kVideo,
  kAudio,  // a song, track, podcast episode
  kLink,
  kButton,
  kTextField,
  kCheckBox,
  kRadioButton,
  kListBox,
  kOption,
  kTab,
  kImage,
  kHeading,
  kArticle,  // an article, story or post
  kProduct,
};

// A set of roles, for role words: bit (1 << role).
using ChoiceRoleMask = uint32_t;
constexpr ChoiceRoleMask RoleBit(ChoiceRole role) {
  return ChoiceRoleMask{1} << static_cast<int>(role);
}

// What a field of an item says.
enum class ChoiceFieldKind : uint8_t {
  kAuthor,    // channel, artist, author, uploader, seller, brand: "by X"
  kSource,    // the site, publisher or news outlet: "from X" (and "by X")
  kDate,      // "3 years ago", "12 March 2024", "1985": "from X"
  kDuration,  // "6:01", "6 minutes"
  kCount,     // "1.2B views", "4.5 stars", "120 comments"
  kPrice,     // "$12.99"
  kDetail,    // anything else read with the item: a snippet, a badge
              // ("Live", "4K", "Official"), a subtitle
};

struct ChoiceField {
  ChoiceField();
  ChoiceField(ChoiceFieldKind kind, std::string text);
  ~ChoiceField();
  ChoiceField(const ChoiceField&);
  ChoiceField& operator=(const ChoiceField&);
  ChoiceField(ChoiceField&&);
  ChoiceField& operator=(ChoiceField&&);

  ChoiceFieldKind kind = ChoiceFieldKind::kDetail;
  // As shown on screen (page text: any bytes; invalid UTF-8, control and
  // bidi characters are read around).
  std::string text;
};

// One item the user can choose. The page map fills these in reading order
// for the list the browser read or numbered; the browser's own lists (tabs,
// a permission prompt's buttons) use the same shape.
struct ChoiceCandidate {
  ChoiceCandidate();
  ~ChoiceCandidate();
  ChoiceCandidate(const ChoiceCandidate&);
  ChoiceCandidate& operator=(const ChoiceCandidate&);
  ChoiceCandidate(ChoiceCandidate&&);
  ChoiceCandidate& operator=(ChoiceCandidate&&);

  // 1-based position in the list as read aloud ("the fourth one"). Unique
  // within a list; items need not be passed in order.
  int position = 0;
  // The number shown on the item by the numbers overlay ("number 12",
  // "click 7"); 0 when it has none.
  int screen_number = 0;
  ChoiceRole role = ChoiceRole::kOther;
  // The item's name as read aloud: a result's title, a link's or button's
  // accessible name. Page text.
  std::string title;
  // The parts read with it that tell items apart, at most
  // kMaxChoiceFields of them (the rest are not read).
  std::vector<ChoiceField> fields;
};

// ---------------------------------------------------------------------------
// What the user said
// ---------------------------------------------------------------------------

struct ChoiceRef {
  ChoiceRef();
  ~ChoiceRef();
  ChoiceRef(const ChoiceRef&);
  ChoiceRef& operator=(const ChoiceRef&);
  ChoiceRef(ChoiceRef&&);
  ChoiceRef& operator=(ChoiceRef&&);

  enum class Kind : uint8_t {
    kNone,
    // A position in the list: "the fourth one", "the last one", "the
    // second to last video". |ordinal| (1-based) counts from the end when
    // |from_end|.
    kPosition,
    // A number: "number 12", "click 7", "four" (right after a list was
    // read). The item showing |number| on screen; when no item shows
    // numbers, the item at that position.
    kNumber,
    // Relative to the current item (MatchOptions::current_position):
    // "this one", "it", "the next one", "the one above".
    kRelative,
    // Words describing the item: "the live one by Queen". With an
    // |ordinal| too ("the second live one"), the ordinal-th item the
    // description matches, in list order.
    kDescription,
  };
  enum class Relative : uint8_t { kCurrent, kNext, kPrevious };

  Kind kind = Kind::kNone;
  int ordinal = 0;
  bool from_end = false;
  int number = 0;
  Relative relative = Relative::kCurrent;
  // Roles named by a role word ("video", "link", "button"); 0 for none.
  ChoiceRoleMask roles = 0;
  // kDescription: folded words (spoken_text.h), stopwords and role words
  // removed, number words as digits. |by_words| follow "by" ("by Queen"),
  // |from_words| follow "from" ("from the BBC").
  std::vector<std::string> words;
  std::vector<std::string> by_words;
  std::vector<std::string> from_words;
};

// Reads |spoken| (one reference, without a verb: "the fourth one", "number
// 12", "the live one by Queen") as a ChoiceRef. A bare number ("four",
// "12") is read only with |bare_numbers| (right after the browser read a
// list or showed numbers). nullopt for text that names nothing ("the
// one"), text over kMaxChoiceRefBytes or invalid UTF-8.
std::optional<ChoiceRef> ParseChoiceRef(std::string_view spoken,
                                        Language language,
                                        bool bare_numbers);

// The same, from folded words (NormalizeSpokenText() with |marks|): the
// grammar's entry point. All of |words| must be the reference.
std::optional<ChoiceRef> ParseChoiceRefWords(const WordList& words,
                                             Language language,
                                             bool bare_numbers);

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

// The lowest score that counts as a match. A description whose words all
// match an item's words exactly scores 1; one where half the weight
// matches scores about 0.5. Below 0.5, more of what the user said fails to
// match the item than matches it, so the browser asks rather than acts.
inline constexpr double kMinChoiceScore = 0.5;
// The lead the best item needs over the next to be unique. Two items
// within 0.15 differ by less than one distinguishing word in about seven
// (a weight-1 word among weights summing to ~7), which is within what one
// misrecognized or extra word changes: too close to call.
inline constexpr double kChoiceMargin = 0.15;
// At most this many ambiguous items are offered back ("Did you mean 1, 2
// or 3?"): more than five is too many to hear.
inline constexpr size_t kMaxCloseChoices = 5;
inline constexpr size_t kMaxRankedChoices = 10;

// Caps. Items past kMaxChoiceCandidates are not considered; a title or
// field is read up to its byte cap (cut at a word); a reference over its
// cap matches nothing.
inline constexpr size_t kMaxChoiceCandidates = 1000;
inline constexpr size_t kMaxChoiceFields = 12;
inline constexpr size_t kMaxChoiceTitleBytes = 1024;
inline constexpr size_t kMaxChoiceFieldBytes = 256;
inline constexpr size_t kMaxChoiceRefBytes = 1000;
// Description words compared, all cues together; the rest are ignored.
inline constexpr size_t kMaxChoiceRefWords = 12;

struct MatchOptions {
  // The language the reference was spoken in.
  Language language = Language::kEnglish;
  // The item last chosen, read or focused, for "this one", "the next
  // one", "the one above"; 0 when there is none.
  int current_position = 0;
  // Overridable for tests; the defaults are the documented thresholds.
  double min_score = kMinChoiceScore;
  double margin = kChoiceMargin;
  size_t max_ranked = kMaxRankedChoices;
};

enum class ChoiceOutcome : uint8_t {
  kUnique,
  kAmbiguous,
  kNoMatch,
};

enum class NoMatchReason : uint8_t {
  kNone,
  kNoCandidates,
  kEmptyReference,  // nothing to match ("the one")
  kOutOfRange,      // no item at that position or number
  kNoCurrentItem,   // "the next one" with no current item
  kNothingMatched,  // no item scored kMinChoiceScore
};

struct RankedChoice {
  // Index into the candidates passed to MatchSpokenChoice().
  size_t index = 0;
  // [0, 1]. Positions, numbers and relative references score 1.
  double score = 0;
};

struct ChoiceMatch {
  ChoiceMatch();
  ~ChoiceMatch();
  ChoiceMatch(const ChoiceMatch&);
  ChoiceMatch& operator=(const ChoiceMatch&);
  ChoiceMatch(ChoiceMatch&&);
  ChoiceMatch& operator=(ChoiceMatch&&);

  ChoiceOutcome outcome = ChoiceOutcome::kNoMatch;
  // kUnique: the item (index into the candidates).
  size_t index = 0;
  // kAmbiguous: the items within the margin of the best, best first, at
  // most kMaxCloseChoices.
  std::vector<size_t> close;
  // The best items, best first (ties in list order), at most
  // MatchOptions::max_ranked; only items that scored above 0.
  std::vector<RankedChoice> ranked;
  NoMatchReason reason = NoMatchReason::kNone;
};

ChoiceMatch MatchSpokenChoice(const ChoiceRef& ref,
                              const std::vector<ChoiceCandidate>& candidates,
                              const MatchOptions& options);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_CHOICE_H_
