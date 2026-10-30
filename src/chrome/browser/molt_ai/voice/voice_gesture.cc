// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_gesture.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include "chrome/browser/molt_ai/voice/phonetic_key.h"
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

template <size_t N>
bool OneOf(const std::array<std::string_view, N>& words,
           std::string_view word) {
  return std::find(words.begin(), words.end(), word) != words.end();
}

std::vector<std::string> SplitFolded(std::string_view heard) {
  const std::string folded = FoldForComparison(
      heard.substr(0, std::min(heard.size(), kMaxHeardBytes)));
  std::vector<std::string> words;
  size_t start = 0;
  while (start < folded.size() && words.size() <= kMaxHeardWords) {
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

// The confirmation word |key| (a phonetic key) sounds like: exactly one
// within its bound.
std::optional<size_t> SoundsLike(const std::string& key) {
  if (key.empty()) {
    return std::nullopt;
  }
  std::optional<size_t> found;
  for (size_t i = 0; i < kWords.size(); ++i) {
    const std::string word_key = PhoneticKey(kWords[i]);
    const int bound = PhoneticBound(std::max(key.size(), word_key.size()));
    if (PhoneticDistance(key, word_key, bound) <= bound) {
      if (found) {
        return std::nullopt;  // two of them: neither
      }
      found = i;
    }
  }
  return found;
}

std::optional<size_t> Exactly(std::string_view word) {
  for (size_t i = 0; i < kWords.size(); ++i) {
    if (kWords[i] == word) {
      return i;
    }
  }
  return std::nullopt;
}

}  // namespace

std::span<const std::string_view> ConfirmationWords() {
  return kWords;
}

std::span<const std::string_view> AnswerWords() {
  return kAnswerWords;
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
  std::vector<std::string> words = SplitFolded(heard);
  if (words.empty() || words.size() > kMaxHeardWords) {
    return std::nullopt;
  }
  size_t first = 0;
  while (first < words.size() && first < 2 && words.size() - first > 1 &&
         OneOf(kLeading, words[first])) {
    ++first;
  }
  size_t last = words.size();
  while (last > first + 1 && OneOf(kTrailing, words[last - 1])) {
    --last;
  }
  if (last - first == 1) {
    if (std::optional<size_t> exact = Exactly(words[first])) {
      return exact;
    }
    // A recognizer's plural ("eagles").
    const std::string& word = words[first];
    if (word.size() > 3 && word.back() == 's') {
      if (std::optional<size_t> exact =
              Exactly(std::string_view(word).substr(0, word.size() - 1))) {
        return exact;
      }
    }
    // An answer word is never a confirmation word ("no" is no).
    if (OneOf(kAnswerWords, words[first])) {
      return std::nullopt;
    }
    return SoundsLike(PhoneticKey(words[first]));
  }
  if (last - first == 2) {
    // A recognizer that split the word in two ("sail boat").
    if (std::optional<size_t> exact =
            Exactly(words[first] + words[first + 1])) {
      return exact;
    }
    if (OneOf(kAnswerWords, words[first]) ||
        OneOf(kAnswerWords, words[first + 1])) {
      return std::nullopt;
    }
    const std::string a = PhoneticKey(words[first]);
    const std::string b = PhoneticKey(words[first + 1]);
    if (!a.empty() && !b.empty()) {
      return SoundsLike(JoinPhoneticKeys(a, b));
    }
  }
  return std::nullopt;
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
    case HearResult::kNotAWord:
      return "not a word";
    case HearResult::kNoPrompt:
      return "no prompt";
    case HearResult::kLockedOut:
      return "locked out";
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

GestureBook::GestureBook(std::function<uint64_t()> random)
    : random_(std::move(random)) {}

GestureBook::~GestureBook() = default;

std::string GestureBook::PickWord() {
  std::vector<std::string_view> eligible;
  for (std::string_view word : kWords) {
    if (std::find(recent_words_.begin(), recent_words_.end(), word) ==
        recent_words_.end()) {
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
  Expire(now_ms);
  if (LockedOut(now_ms)) {
    return std::nullopt;
  }
  std::erase_if(prompts_, [&](const GesturePrompt& prompt) {
    return prompt.binding == binding;
  });
  if (prompts_.size() >= kMaxWaitingPrompts) {
    prompts_.erase(prompts_.begin());
  }
  GesturePrompt prompt;
  prompt.id = ++next_id_;
  prompt.word = PickWord();
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
  const std::optional<size_t> match = MatchConfirmationWord(heard);
  if (!match) {
    return HearResult::kNotAWord;
  }
  // Only what the microphone heard cleanly answers (or refuses) a prompt:
  // typed text, or what may have been another source, changes nothing.
  if (!evidence.from_microphone || !evidence.clean) {
    return HearResult::kNotClean;
  }
  if (kWords[*match] != it->word) {
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
