// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The voice gesture (docs/design/TOUCHLESS_BROWSING.md §7.3): in voice mode,
// what stands in for the physical click some decisions need (a permission
// prompt's "Allow", keeping a dangerous download, confirming a workflow
// draft's sites).
//
// The browser says and shows a prompt with a confirmation word chosen at
// random for it ("example.com wants to use your camera. To allow it, say
// harbor."). Hearing that word, cleanly, from the listening session mints a
// gesture: a one-time capability bound to that prompt (its id and what it
// allows), which expires kGestureLifetimeMs after it was minted and is used
// once.
//
//   - The words: ConfirmationWords(), short everyday nouns that no two of
//     which the phonetic key (phonetic_key.h) takes for each other or for
//     the answers and numbers voice mode understands ("no", "block",
//     "stay", "four"...). Each language has its own
//     (ConfirmationWords(Language)): Spanish's are
//     confirmation_words_es.h's, heard with Spanish's phonetic key
//     (phonetic_key_es.h), its answers and the words said around them. A
//     prompt is heard in the language its word was drawn in.
//   - The choice: uniform over the list (the caller's random source; the
//     browser's is base::RandUint64()), never one of the last
//     kRecentWordsAvoided words, so a recording of an earlier answer never
//     answers a later prompt.
//   - Who mints: only the browser's voice loop, from the listening
//     session's final text, with HeardEvidence that says it was heard
//     cleanly (voice_session_logic.h: it began after the browser stopped
//     speaking, no other source spoke meanwhile, no tab played sound
//     without an echo canceller). Typed text never mints one; pages can't
//     reach this at all.
//   - One try: hearing another word of the list, cleanly and for sure,
//     refuses the prompt, and kMaxWrongWords such misses within
//     kWrongWordWindowMs turn the gesture off for kLockoutMs (prompts are
//     then answered with the mouse or keyboard, or refused by voice). What
//     sounds like a word of the list but not for sure (HearConfirmationWord()
//     says kUnsure) neither answers nor refuses: the user is asked to say
//     the word again, within the prompt's window. Whether a hearing is sure
//     never depends on the prompt's word, so it tells a guesser nothing.
//   - The word isn't given to the speech helper as a hotword (see §7.3 for
//     the measurements): a helper that someone took over would otherwise
//     know it.
//
// Std-only, so molt_ai_unittests and a standalone harness build the same
// code. The browser's side (prompts, dialogs, downloads) is
// voice_browser_prompts.h.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_GESTURE_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_GESTURE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

// How long a prompt waits for its word (the loop's other confirmations wait
// as long, kConfirmationTimeoutMs).
inline constexpr int64_t kGesturePromptWindowMs = 30000;
// How long a minted gesture can be used.
inline constexpr int64_t kGestureLifetimeMs = 10000;
// A new prompt never gets one of the words the last this many prompts had.
inline constexpr size_t kRecentWordsAvoided = 8;
// Wrong words of the list heard within kWrongWordWindowMs that turn the
// gesture off, and for how long.
inline constexpr int kMaxWrongWords = 2;
inline constexpr int64_t kWrongWordWindowMs = 10 * 60 * 1000;
inline constexpr int64_t kLockoutMs = 10 * 60 * 1000;
// What is heard is compared only up to this many bytes and words.
inline constexpr size_t kMaxHeardBytes = 200;
inline constexpr size_t kMaxHeardWords = 6;

// ---------------------------------------------------------------------------
// The words
// ---------------------------------------------------------------------------

// The confirmation words, lower-case ASCII.
std::span<const std::string_view> ConfirmationWords();
// |language|'s: ConfirmationWords() in English, SpanishConfirmationWords()
// (folded: "platano") in Spanish.
std::span<const std::string_view> ConfirmationWords(Language language);

// The words voice mode's answers and numbers use ("yes", "no", "block",
// "leave", "one" ... "twelve"): no confirmation word may sound like one.
std::span<const std::string_view> AnswerWords();
// |language|'s ("si", "no", "bloquear", "salir", "uno" ... "doce" in
// Spanish).
std::span<const std::string_view> AnswerWords(Language language);

// Whether the phonetic key could take |a| for |b|: their keys are equal or
// within PhoneticBound() of each other, plus |margin| half-edits (the list's
// own words keep a margin of 1 between them).
bool WordsConfusable(std::string_view a, std::string_view b, int margin);

// What a sound-alike must keep from every other confirmation word to stand
// for one for sure: no other within the phonetic key's bound plus this many
// half-edits (the margin the list's own words keep from each other, so a
// word of the list itself is always sure).
inline constexpr int kSureSoundAlikeMargin = 1;

// Which confirmation word |heard| (an utterance, the wake phrase already
// removed) is: its folded words, with "yes", "ok", "okay" or "say" before
// and "please" after allowed, are one word that is a confirmation word (or
// its plural, or it split in two), or sounds like one of them (the phonetic
// key, within its bound).
struct HeardWord {
  enum class Kind : uint8_t {
    kNone,    // not a confirmation word
    kWord,    // this one, for sure
    kUnsure,  // it sounds like one, but also like another
  };
  Kind kind = Kind::kNone;
  // kWord: the word's index in ConfirmationWords().
  size_t index = 0;
};
HeardWord HearConfirmationWord(std::string_view heard);
// The same in |language|: its words, heard with its phonetic key
// (PhoneticKeyFor()), the words said around them and its answers. In
// Spanish, "si", "vale", "ok", "di" or an article before the word ("la
// manzana") and "por favor", "porfa" or "gracias" after it; a plural adds
// "s" or "es" ("camiones").
HeardWord HearConfirmationWord(std::string_view heard, Language language);

// Whether |heard| holds the confirmation word |word| of |language|
// anywhere in it, whatever else it says ("the word is harbor", "i said
// harbour i think"): one of its words is |word|, its plural or it split in
// two, or sounds like it within the phonetic key's bound plus
// kSureSoundAlikeMargin. What a recording of the utterance could answer
// the prompt with. Reads at most kMaxHeldWordScanBytes of |heard|.
inline constexpr size_t kMaxHeldWordScanBytes = 1024;
bool HoldsConfirmationWord(std::string_view heard,
                           std::string_view word,
                           Language language);

// The index of the confirmation word |heard| is for sure (kWord), or
// nullopt.
std::optional<size_t> MatchConfirmationWord(std::string_view heard);
std::optional<size_t> MatchConfirmationWord(std::string_view heard,
                                            Language language);

// ---------------------------------------------------------------------------
// Prompts and gestures
// ---------------------------------------------------------------------------

// What a gesture allows.
enum class GestureAction : uint8_t {
  kAllowPermission,        // a permission prompt's requests
  kKeepDangerousDownload,  // a download the browser warned about
  kConfirmDraftSites,      // a workflow draft's sites (automation_security.h)
};

std::string_view GestureActionName(GestureAction action);

// The prompt a gesture is bound to: what it allows, and the subject the
// caller checks again when it uses the gesture (the requesting origin and
// what it asked for; the download's id; the workflow and its sites).
struct GestureBinding {
  GestureAction action = GestureAction::kAllowPermission;
  std::string subject;

  friend bool operator==(const GestureBinding&,
                         const GestureBinding&) = default;
};

// The binding for confirming the sites of workflow draft |workflow_id|:
// |sites| in any order (they are sorted here), so a draft whose sites
// changed after the prompt isn't confirmed by it.
GestureBinding DraftSitesBinding(std::string_view workflow_id,
                                 std::vector<std::string> sites);

// A prompt that waits for its word.
struct GesturePrompt {
  GesturePrompt();
  ~GesturePrompt();
  GesturePrompt(const GesturePrompt&);
  GesturePrompt& operator=(const GesturePrompt&);
  GesturePrompt(GesturePrompt&&);
  GesturePrompt& operator=(GesturePrompt&&);

  int id = 0;
  std::string word;
  // The language |word| was drawn in, and what is heard is heard in.
  Language language = Language::kEnglish;
  GestureBinding binding;
  int64_t expires_ms = 0;
  // The words this prompt had before (Begin() for the same binding while
  // it waited replaced them: a word retired, voice_gesture_channel.h).
  // Hearing one is neither the word nor a miss.
  std::vector<std::string> retired;
};

// Where what was heard came from (voice_session_logic.h fills it; typed
// text leaves it all false).
struct HeardEvidence {
  // The listening session's final text, not typed.
  bool from_microphone = false;
  // It began after the browser's own speech ended (and its echo), no other
  // source spoke while it was said, and no tab played sound without an
  // echo canceller; with a tab playing sound, it was heard with the
  // confidence an utterance without the wake phrase needs then.
  bool clean = false;
  // The helper's wake detector heard the wake phrase begin it (or say it
  // alone just before it): SessionActions::woke.
  bool woke = false;
};

// The capability: minted by GestureBook::Hear(), used by Consume().
struct VoiceGesture {
  int prompt_id = 0;
  GestureBinding binding;
  int64_t minted_ms = 0;
  uint64_t serial = 0;
};

enum class HearResult : uint8_t {
  kMinted,     // the prompt's word, heard cleanly
  kNotClean,   // the prompt's word, but not heard cleanly: say it again
  kWrongWord,  // another confirmation word: the prompt is refused
  kUnsure,     // a confirmation word, but not for sure: say it again
  kNotAWord,   // no confirmation word: the prompt still waits
  kNoPrompt,   // no such prompt waits (answered, expired, cancelled)
  kLockedOut,  // the gesture is off for now
  kRetired,    // a word this prompt had before: it changed, say the new one
};

std::string_view HearResultName(HearResult result);

// The prompts of one listening session, their words and the gestures they
// minted. Every time is the same monotonic clock, in milliseconds.
class GestureBook {
 public:
  // |random| returns uniformly random 64-bit numbers.
  explicit GestureBook(std::function<uint64_t()> random);
  ~GestureBook();
  GestureBook(const GestureBook&) = delete;
  GestureBook& operator=(const GestureBook&) = delete;

  // A new prompt for |binding|, with a fresh word; nullopt while the gesture
  // is locked out. A prompt with the same binding that still waits is
  // replaced, and its words are the new one's retired words (a word retired
  // because something may have heard or seen it: the prompt is begun again
  // with a new word).
  std::optional<GesturePrompt> Begin(GestureBinding binding, int64_t now_ms);
  // The same with a word of |language|'s list, which Hear() then hears in
  // |language|.
  std::optional<GesturePrompt> Begin(GestureBinding binding,
                                     Language language,
                                     int64_t now_ms);

  // |heard| while prompt |id| waits. kMinted fills |gesture| and closes the
  // prompt; kWrongWord (another word, heard cleanly and for sure) closes it
  // too and counts toward the lockout. kUnsure, kNotClean, kRetired (one of
  // its retired words, for sure) and kNotAWord leave it waiting.
  HearResult Hear(int id,
                  std::string_view heard,
                  const HeardEvidence& evidence,
                  int64_t now_ms,
                  VoiceGesture* gesture);

  // Uses |gesture| for |binding|: true once, for the binding it was minted
  // for, within kGestureLifetimeMs of its minting.
  bool Consume(const VoiceGesture& gesture,
               const GestureBinding& binding,
               int64_t now_ms);

  // Prompt |id| no longer waits (answered otherwise, timed out, gone).
  void Cancel(int id);

  // The prompt |id|, if it still waits at |now_ms|.
  const GesturePrompt* Find(int id, int64_t now_ms) const;

  bool LockedOut(int64_t now_ms) const;

 private:
  // A word of |language|'s list: never a recent word, nor one of
  // |retired|.
  std::string PickWord(const std::vector<std::string>& retired,
                       Language language);
  void Expire(int64_t now_ms);

  std::function<uint64_t()> random_;
  std::vector<GesturePrompt> prompts_;
  // The words the last kRecentWordsAvoided prompts had, oldest first.
  std::vector<std::string> recent_words_;
  // When wrong words were heard (within kWrongWordWindowMs).
  std::vector<int64_t> wrong_ms_;
  int64_t locked_until_ms_ = -1;
  int next_id_ = 0;
  uint64_t next_serial_ = 0;
  // Gestures minted and not yet used or expired: what they were minted
  // for, and until when.
  struct Live {
    uint64_t serial = 0;
    GestureBinding binding;
    int64_t minted_ms = 0;
    int64_t expires_ms = 0;
  };
  std::vector<Live> live_;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_GESTURE_H_
