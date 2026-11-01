// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_gesture.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include "chrome/browser/molt_ai/voice/confirmation_words_es.h"
#include "chrome/browser/molt_ai/voice/phonetic_key.h"
#include "chrome/browser/molt_ai/voice/phonetic_key_es.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

namespace {

// Chosen with the phonetic key (no two within its bound plus a close sound
// of each other, none within its bound of an answer word) and measured with
// the speech helper's recognizer on the voice bench's synthetic voices
// (TOUCHLESS_BROWSING.md §7.3): of 46 candidates, the six it missed most
// (mitten, cupcake, ketchup, oatmeal, parrot, teapot) were left out.
constexpr auto kWords = std::to_array<std::string_view>({
    "harbor",   "maple",    "tiger",      "lemon",   "rocket",    "garden",
    "basket",   "dragon",   "planet",     "magnet",  "pepper",    "cactus",
    "falcon",   "hammer",   "lantern",    "napkin",  "orange",    "umbrella",
    "bamboo",   "cobalt",   "eagle",      "helmet",  "turtle",    "banjo",
    "fossil",   "kayak",    "lobster",    "mango",   "yogurt",    "compass",
    "guitar",   "lettuce",  "whistle",    "canyon",  "butterfly", "gorilla",
    "macaroni", "sandwich", "strawberry", "tornado",
});

// The answers voice mode takes to its own prompts and dialogs, the words
// around them, and the numbers a list is chosen from.
constexpr auto kAnswerWords = std::to_array<std::string_view>({
    "yes",    "no",     "ok",      "okay",     "stop",   "cancel",  "block",
    "allow",  "deny",   "leave",   "stay",     "keep",   "discard", "save",
    "never",  "repeat", "help",    "molt",     "hey",    "one",     "two",
    "three",  "four",   "five",    "six",      "seven",  "eight",   "nine",
    "ten",    "eleven", "twelve",  "next",     "back",   "answer",  "send",
    "yeah",   "yep",    "nope",    "sure",     "play",   "pause",   "open",
    "close",  "reload", "please",  "again",    "what",   "sorry",   "wait",
    "delete", "remove", "dismiss", "later",    "ignore", "confirm", "fine",
    "thanks", "thank",  "not",     "now",      "type",   "dont",    "reply",
    "page",   "here",   "got",     "password", "it",     "is",      "my",
});

// Said around the word: "yes, harbor", "say harbor", "the harbor",
// "harbor please".
constexpr auto kLeading = std::to_array<std::string_view>(
    {"yes", "ok", "okay", "say", "its", "the", "a"});
constexpr std::array<std::string_view, 2> kTrailing = {"please", "thanks"};

using Words = std::span<const std::string_view>;

// What a language hears a confirmation word with.
struct WordTable {
  Words words;
  Words answers;
  Words leading;
  // Each one word or two ("por favor").
  Words trailing;
  Language language;
};

WordTable TableFor(Language language) {
  switch (language) {
    case Language::kEnglish:
      break;
    case Language::kSpanish:
      return WordTable{SpanishConfirmationWords(), SpanishAnswerWords(),
                       SpanishConfirmationLeading(),
                       SpanishConfirmationTrailing(), Language::kSpanish};
  }
  return WordTable{kWords, kAnswerWords, kLeading, kTrailing,
                   Language::kEnglish};
}

bool OneOf(Words words, std::string_view word) {
  return std::find(words.begin(), words.end(), word) != words.end();
}

// How many of |words|' last ones before |end| (and after |begin|) are one
// of |trailing|: 0, 1 or 2.
size_t TrailingAt(Words trailing,
                  const std::vector<std::string>& words,
                  size_t begin,
                  size_t end) {
  for (std::string_view phrase : trailing) {
    const size_t space = phrase.find(' ');
    if (space == std::string_view::npos) {
      if (end > begin && words[end - 1] == phrase) {
        return 1;
      }
    } else if (end > begin + 1 && words[end - 2] == phrase.substr(0, space) &&
               words[end - 1] == phrase.substr(space + 1)) {
      return 2;
    }
  }
  return 0;
}

std::vector<std::string> SplitFolded(std::string_view heard,
                                     size_t max_bytes = kMaxHeardBytes,
                                     size_t max_words = kMaxHeardWords) {
  const std::string folded =
      FoldForComparison(heard.substr(0, std::min(heard.size(), max_bytes)));
  std::vector<std::string> words;
  size_t start = 0;
  while (start < folded.size() && words.size() <= max_words) {
    size_t space = folded.find(' ', start);
    if (space == std::string::npos) {
      space = folded.size();
    }
    if (space > start) {
      words.push_back(folded.substr(start, space - start));
    }
    start = space + 1;
  }
  return words;
}

HeardWord Word(size_t index) {
  HeardWord word;
  word.kind = HeardWord::Kind::kWord;
  word.index = index;
  return word;
}

HeardWord Unsure() {
  HeardWord word;
  word.kind = HeardWord::Kind::kUnsure;
  return word;
}

// The confirmation word |key| (a phonetic key) sounds like: one within its
// bound, for sure when no other is within the bound plus
// kSureSoundAlikeMargin ("pardon" is garden's, but almost falcon's too:
// unsure). The test looks at every word of the list alike, whichever the
// prompt's is.
HeardWord SoundsLike(const WordTable& table, const std::string& key) {
  if (key.empty()) {
    return HeardWord();
  }
  std::optional<size_t> found;
  int near = 0;
  for (size_t i = 0; i < table.words.size(); ++i) {
    const std::string word_key = PhoneticKeyFor(table.language, table.words[i]);
    const int bound = PhoneticBound(std::max(key.size(), word_key.size()));
    const int distance =
        PhoneticDistance(key, word_key, bound + kSureSoundAlikeMargin);
    if (distance <= bound + kSureSoundAlikeMargin) {
      ++near;
    }
    if (distance <= bound) {
      if (found) {
        return Unsure();  // two of them
      }
      found = i;
    }
  }
  if (!found) {
    return HeardWord();
  }
  return near == 1 ? Word(*found) : Unsure();
}

std::optional<size_t> Exactly(Words words, std::string_view word) {
  for (size_t i = 0; i < words.size(); ++i) {
    if (words[i] == word) {
      return i;
    }
  }
  return std::nullopt;
}

// A recognizer's plural of a word of |table|'s ("eagles"; in Spanish also
// "es": "camiones").
std::optional<size_t> Plural(const WordTable& table, std::string_view word) {
  if (word.size() > 3 && word.back() == 's') {
    if (std::optional<size_t> exact =
            Exactly(table.words, word.substr(0, word.size() - 1))) {
      return exact;
    }
  }
  if (table.language == Language::kSpanish && word.size() > 4 &&
      word.substr(word.size() - 2) == "es") {
    return Exactly(table.words, word.substr(0, word.size() - 2));
  }
  return std::nullopt;
}

HeardWord HearIn(const WordTable& table, std::string_view heard) {
  std::vector<std::string> words = SplitFolded(heard);
  if (words.empty() || words.size() > kMaxHeardWords) {
    return HeardWord();
  }
  size_t first = 0;
  while (first < words.size() && first < 2 && words.size() - first > 1 &&
         OneOf(table.leading, words[first])) {
    ++first;
  }
  size_t last = words.size();
  while (const size_t trailing =
             TrailingAt(table.trailing, words, first + 1, last)) {
    last -= trailing;
  }
  if (last - first == 1) {
    if (std::optional<size_t> exact = Exactly(table.words, words[first])) {
      return Word(*exact);
    }
    if (std::optional<size_t> plural = Plural(table, words[first])) {
      return Word(*plural);
    }
    // An answer word is never a confirmation word ("no" is no).
    if (OneOf(table.answers, words[first])) {
      return HeardWord();
    }
    return SoundsLike(table, PhoneticKeyFor(table.language, words[first]));
  }
  if (last - first == 2) {
    // A recognizer that split the word in two ("sail boat").
    if (std::optional<size_t> exact =
            Exactly(table.words, words[first] + words[first + 1])) {
      return Word(*exact);
    }
    if (OneOf(table.answers, words[first]) ||
        OneOf(table.answers, words[first + 1])) {
      return HeardWord();
    }
    const std::string a = PhoneticKeyFor(table.language, words[first]);
    const std::string b = PhoneticKeyFor(table.language, words[first + 1]);
    if (!a.empty() && !b.empty()) {
      return SoundsLike(table, JoinPhoneticKeys(a, b));
    }
  }
  return HeardWord();
}

}  // namespace

std::span<const std::string_view> ConfirmationWords() {
  return kWords;
}

std::span<const std::string_view> ConfirmationWords(Language language) {
  return TableFor(language).words;
}

std::span<const std::string_view> AnswerWords() {
  return kAnswerWords;
}

std::span<const std::string_view> AnswerWords(Language language) {
  return TableFor(language).answers;
}

bool WordsConfusable(std::string_view a, std::string_view b, int margin) {
  const std::string key_a = PhoneticKey(a);
  const std::string key_b = PhoneticKey(b);
  if (key_a.empty() || key_b.empty()) {
    return a == b;
  }
  if (key_a == key_b) {
    return true;
  }
  const int bound =
      PhoneticBound(std::max(key_a.size(), key_b.size())) + margin;
  return PhoneticDistance(key_a, key_b, bound) <= bound;
}

std::optional<size_t> MatchConfirmationWord(std::string_view heard) {
  return MatchConfirmationWord(heard, Language::kEnglish);
}

std::optional<size_t> MatchConfirmationWord(std::string_view heard,
                                            Language language) {
  const HeardWord word = HearConfirmationWord(heard, language);
  if (word.kind != HeardWord::Kind::kWord) {
    return std::nullopt;
  }
  return word.index;
}

HeardWord HearConfirmationWord(std::string_view heard) {
  return HearConfirmationWord(heard, Language::kEnglish);
}

HeardWord HearConfirmationWord(std::string_view heard, Language language) {
  return HearIn(TableFor(language), heard);
}

bool HoldsConfirmationWord(std::string_view heard,
                           std::string_view word,
                           Language language) {
  const WordTable table = TableFor(language);
  const std::string word_key = PhoneticKeyFor(language, word);
  if (word.empty() || word_key.empty()) {
    return false;
  }
  // Within the bound plus the margin: what would be the word, or unsure
  // between it and another.
  const auto sounds_like = [&](const std::string& key) {
    if (key.empty()) {
      return false;
    }
    const int bound = PhoneticBound(std::max(key.size(), word_key.size())) +
                      kSureSoundAlikeMargin;
    return PhoneticDistance(key, word_key, bound) <= bound;
  };
  // Every word (a byte cap, not a word cap: the word may come late).
  const std::vector<std::string> words =
      SplitFolded(heard, kMaxHeldWordScanBytes, kMaxHeldWordScanBytes);
  for (size_t i = 0; i < words.size(); ++i) {
    const std::string& one = words[i];
    if (one == word) {
      return true;
    }
    if (std::optional<size_t> plural = Plural(table, one);
        plural && table.words[*plural] == word) {
      return true;
    }
    if (sounds_like(PhoneticKeyFor(language, one))) {
      return true;
    }
    if (i + 1 < words.size()) {
      const std::string& next = words[i + 1];
      if (one + next == word) {
        return true;
      }
      const std::string a = PhoneticKeyFor(language, one);
      const std::string b = PhoneticKeyFor(language, next);
      if (!a.empty() && !b.empty() && sounds_like(JoinPhoneticKeys(a, b))) {
        return true;
      }
    }
  }
  return false;
}

std::string_view GestureActionName(GestureAction action) {
  switch (action) {
    case GestureAction::kAllowPermission:
      return "allow permission";
    case GestureAction::kKeepDangerousDownload:
      return "keep dangerous download";
    case GestureAction::kConfirmDraftSites:
      return "confirm draft sites";
  }
  return "unknown";
}

std::string_view HearResultName(HearResult result) {
  switch (result) {
    case HearResult::kMinted:
      return "minted";
    case HearResult::kNotClean:
      return "not clean";
    case HearResult::kWrongWord:
      return "wrong word";
    case HearResult::kUnsure:
      return "unsure";
    case HearResult::kNotAWord:
      return "not a word";
    case HearResult::kNoPrompt:
      return "no prompt";
    case HearResult::kLockedOut:
      return "locked out";
    case HearResult::kRetired:
      return "retired word";
  }
  return "unknown";
}

GestureBinding DraftSitesBinding(std::string_view workflow_id,
                                 std::vector<std::string> sites) {
  std::sort(sites.begin(), sites.end());
  sites.erase(std::unique(sites.begin(), sites.end()), sites.end());
  GestureBinding binding;
  binding.action = GestureAction::kConfirmDraftSites;
  // A space can't be in a workflow id or a host.
  binding.subject = std::string(workflow_id);
  for (const std::string& site : sites) {
    binding.subject += ' ';
    binding.subject += site;
  }
  return binding;
}

// ---- GestureBook -----------------------------------------------------------

namespace {
// Prompts waiting at once, at most (one per tab is the rule; this bounds a
// caller that forgets to cancel).
constexpr size_t kMaxWaitingPrompts = 8;
}  // namespace

GesturePrompt::GesturePrompt() = default;
GesturePrompt::~GesturePrompt() = default;
GesturePrompt::GesturePrompt(const GesturePrompt&) = default;
GesturePrompt& GesturePrompt::operator=(const GesturePrompt&) = default;
GesturePrompt::GesturePrompt(GesturePrompt&&) = default;
GesturePrompt& GesturePrompt::operator=(GesturePrompt&&) = default;

GestureBook::GestureBook(std::function<uint64_t()> random)
    : random_(std::move(random)) {}

GestureBook::~GestureBook() = default;

std::string GestureBook::PickWord(const std::vector<std::string>& retired,
                                  Language language) {
  std::vector<std::string_view> eligible;
  for (std::string_view word : ConfirmationWords(language)) {
    if (std::find(recent_words_.begin(), recent_words_.end(), word) ==
            recent_words_.end() &&
        std::find(retired.begin(), retired.end(), word) == retired.end()) {
      eligible.push_back(word);
    }
  }
  const uint64_t n = eligible.size();
  // Uniform: numbers at or above the largest multiple of n are drawn again
  // (a broken source that only gives those still ends).
  const uint64_t limit = std::numeric_limits<uint64_t>::max() -
                         std::numeric_limits<uint64_t>::max() % n;
  uint64_t r = 0;
  for (int attempt = 0; attempt < 64; ++attempt) {
    r = random_ ? random_() : 0;
    if (r < limit) {
      break;
    }
  }
  return std::string(eligible[static_cast<size_t>(r % n)]);
}

void GestureBook::Expire(int64_t now_ms) {
  std::erase_if(prompts_, [&](const GesturePrompt& prompt) {
    return prompt.expires_ms <= now_ms;
  });
  std::erase_if(live_,
                [&](const Live& live) { return live.expires_ms < now_ms; });
  std::erase_if(wrong_ms_, [&](int64_t at) {
    return at + kWrongWordWindowMs <= now_ms || at > now_ms;
  });
}

bool GestureBook::LockedOut(int64_t now_ms) const {
  return locked_until_ms_ > now_ms;
}

std::optional<GesturePrompt> GestureBook::Begin(GestureBinding binding,
                                                int64_t now_ms) {
  return Begin(std::move(binding), Language::kEnglish, now_ms);
}

std::optional<GesturePrompt> GestureBook::Begin(GestureBinding binding,
                                                Language language,
                                                int64_t now_ms) {
  Expire(now_ms);
  if (LockedOut(now_ms)) {
    return std::nullopt;
  }
  GesturePrompt prompt;
  // The same prompt begun again (its word retired): what it had before
  // stays known as its own, so hearing it is no miss.
  for (const GesturePrompt& waiting : prompts_) {
    if (waiting.binding == binding) {
      prompt.retired = waiting.retired;
      prompt.retired.push_back(waiting.word);
    }
  }
  if (prompt.retired.size() > kRecentWordsAvoided) {
    prompt.retired.erase(prompt.retired.begin(),
                         prompt.retired.end() - kRecentWordsAvoided);
  }
  std::erase_if(prompts_, [&](const GesturePrompt& waiting) {
    return waiting.binding == binding;
  });
  if (prompts_.size() >= kMaxWaitingPrompts) {
    prompts_.erase(prompts_.begin());
  }
  prompt.id = ++next_id_;
  prompt.word = PickWord(prompt.retired, language);
  prompt.language = language;
  prompt.binding = std::move(binding);
  prompt.expires_ms = now_ms + kGesturePromptWindowMs;
  recent_words_.push_back(prompt.word);
  if (recent_words_.size() > kRecentWordsAvoided) {
    recent_words_.erase(recent_words_.begin());
  }
  prompts_.push_back(prompt);
  return prompt;
}

const GesturePrompt* GestureBook::Find(int id, int64_t now_ms) const {
  for (const GesturePrompt& prompt : prompts_) {
    if (prompt.id == id && prompt.expires_ms > now_ms) {
      return &prompt;
    }
  }
  return nullptr;
}

void GestureBook::Cancel(int id) {
  std::erase_if(prompts_,
                [&](const GesturePrompt& prompt) { return prompt.id == id; });
}

HearResult GestureBook::Hear(int id,
                             std::string_view heard,
                             const HeardEvidence& evidence,
                             int64_t now_ms,
                             VoiceGesture* gesture) {
  Expire(now_ms);
  auto it = std::find_if(prompts_.begin(), prompts_.end(),
                         [&](const GesturePrompt& p) { return p.id == id; });
  if (it == prompts_.end()) {
    return HearResult::kNoPrompt;
  }
  if (LockedOut(now_ms)) {
    prompts_.erase(it);
    return HearResult::kLockedOut;
  }
  const HeardWord word = HearConfirmationWord(heard, it->language);
  const std::span<const std::string_view> words =
      ConfirmationWords(it->language);
  if (word.kind == HeardWord::Kind::kNone) {
    return HearResult::kNotAWord;
  }
  // Like a word of the list, but not for sure which: neither the word nor a
  // miss. Asked again, within the prompt's window.
  if (word.kind == HeardWord::Kind::kUnsure) {
    return HearResult::kUnsure;
  }
  const size_t match = word.index;
  // A word the prompt had before it was retired: neither the word nor a
  // miss (the user may have caught the old one). It tells a guesser
  // nothing: a retired word is never drawn again for its prompt.
  if (std::find(it->retired.begin(), it->retired.end(), words[match]) !=
      it->retired.end()) {
    return HearResult::kRetired;
  }
  // Only what the microphone heard cleanly answers (or refuses) a prompt:
  // typed text, or what may have been another source, changes nothing.
  if (!evidence.from_microphone || !evidence.clean) {
    return HearResult::kNotClean;
  }
  if (words[match] != it->word) {
    prompts_.erase(it);
    wrong_ms_.push_back(now_ms);
    if (wrong_ms_.size() >= static_cast<size_t>(kMaxWrongWords)) {
      locked_until_ms_ = now_ms + kLockoutMs;
      wrong_ms_.clear();
      prompts_.clear();
    }
    return HearResult::kWrongWord;
  }
  VoiceGesture minted;
  minted.prompt_id = it->id;
  minted.binding = it->binding;
  minted.minted_ms = now_ms;
  minted.serial = ++next_serial_;
  live_.push_back(
      Live{minted.serial, minted.binding, now_ms, now_ms + kGestureLifetimeMs});
  prompts_.erase(it);
  if (gesture) {
    *gesture = std::move(minted);
  }
  return HearResult::kMinted;
}

bool GestureBook::Consume(const VoiceGesture& gesture,
                          const GestureBinding& binding,
                          int64_t now_ms) {
  auto it = std::find_if(live_.begin(), live_.end(), [&](const Live& live) {
    return live.serial == gesture.serial;
  });
  if (it == live_.end() || gesture.serial == 0) {
    return false;
  }
  // Used once, whatever the outcome; checked against what it was minted
  // for, not against the caller's copy.
  Live live = std::move(*it);
  live_.erase(it);
  return live.binding == binding && gesture.prompt_id != 0 &&
         now_ms >= live.minted_ms && now_ms <= live.expires_ms;
}

}  // namespace voice
}  // namespace molt_ai
