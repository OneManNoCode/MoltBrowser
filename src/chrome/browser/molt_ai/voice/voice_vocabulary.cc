// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_vocabulary.h"

#include <algorithm>
#include <utility>

#include "chrome/browser/molt_ai/voice/phonetic_key.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"

namespace molt_ai {
namespace voice {

namespace {

// A phrase is cleaned to this many bytes before it is measured.
constexpr size_t kMaxPhraseInputBytes = 1000;
// Utterances lined up for a correction: at most this many words each.
constexpr size_t kMaxAlignWords = 64;
// Sub-spans of a phrase checked for an answer word ("of course").
constexpr size_t kMaxAnswerSpanWords = 3;
// A date this far past "now" means the clock moved: forgotten.
constexpr int64_t kFutureSlackSeconds = 24 * 3600;

// Words that name a secret: a phrase holding one is never learned.
constexpr std::string_view kSecretWords[] = {
    "password", "passwords", "passcode", "passphrase", "pin",
    "pins",     "cvv",       "cvc",      "ssn",        "otp",
    "token",    "secret",    "login",    "username",   "iban",
};

NormalizedText Normalize(std::string_view text, size_t max_words) {
  NormalizeOptions options;
  options.max_words = max_words;
  return NormalizeSpokenText(text, options);
}

std::string JoinWords(const NormalizedText& text, size_t first, size_t end) {
  std::string out;
  for (size_t i = first; i < end; ++i) {
    if (!out.empty()) {
      out += ' ';
    }
    out += text.Word(i);
  }
  return out;
}

bool HasDigit(std::string_view word) {
  return std::any_of(word.begin(), word.end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}

bool HasNonDigit(std::string_view word) {
  return std::any_of(word.begin(), word.end(),
                     [](char c) { return c < '0' || c > '9'; });
}

// Whether |words| (folded, space-separated) answer a yes-or-no question.
bool IsAnswer(const std::string& words) {
  ParseContext context;
  context.expecting_yes_no = true;
  const VoiceCommandKind kind = ParseVoiceCommand(words, context).kind;
  return kind == VoiceCommandKind::kYes || kind == VoiceCommandKind::kNo;
}

// The joined phonetic keys of words [first, end) of |text|; "" if a word
// has none.
std::string SpanKey(const NormalizedText& text, size_t first, size_t end) {
  std::string key;
  for (size_t i = first; i < end; ++i) {
    const std::string word_key = PhoneticKey(text.Word(i));
    if (word_key.empty()) {
      return std::string();
    }
    key = key.empty() ? word_key : JoinPhoneticKeys(key, word_key);
  }
  return key;
}

// How far apart two keys may be and still be a mishearing: the matcher's
// bound and half an edit more ("Green" for "Queen", "hats" for "cats"; not
// "dogs" for "cats", nor "music" for "news").
int CorrectionBound(size_t longer_key_size) {
  return PhoneticBound(longer_key_size) + 1;
}

// Whether two keys sound alike enough for one to be misheard as the other.
bool KeysSoundAlike(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) {
    return false;
  }
  const int bound = CorrectionBound(std::max(a.size(), b.size()));
  return PhoneticDistance(a, b, bound) <= bound;
}

// The source bytes of words [first, end) of |text| in |source|.
std::pair<size_t, size_t> SourceSpan(const NormalizedText& text,
                                     size_t first,
                                     size_t end) {
  return {text.words[first].source_begin, text.words[end - 1].source_end};
}

}  // namespace

VocabularyEntry::VocabularyEntry() = default;
VocabularyEntry::~VocabularyEntry() = default;
VocabularyEntry::VocabularyEntry(const VocabularyEntry&) = default;
VocabularyEntry& VocabularyEntry::operator=(const VocabularyEntry&) = default;
VocabularyEntry::VocabularyEntry(VocabularyEntry&&) = default;
VocabularyEntry& VocabularyEntry::operator=(VocabularyEntry&&) = default;

AlignedCorrection::AlignedCorrection() = default;
AlignedCorrection::~AlignedCorrection() = default;
AlignedCorrection::AlignedCorrection(const AlignedCorrection&) = default;
AlignedCorrection& AlignedCorrection::operator=(const AlignedCorrection&) =
    default;
AlignedCorrection::AlignedCorrection(AlignedCorrection&&) = default;
AlignedCorrection& AlignedCorrection::operator=(AlignedCorrection&&) = default;

PersonalVocabulary::PersonalVocabulary() = default;
PersonalVocabulary::~PersonalVocabulary() = default;
PersonalVocabulary::PersonalVocabulary(const PersonalVocabulary&) = default;
PersonalVocabulary& PersonalVocabulary::operator=(const PersonalVocabulary&) =
    default;
PersonalVocabulary::PersonalVocabulary(PersonalVocabulary&&) = default;
PersonalVocabulary& PersonalVocabulary::operator=(PersonalVocabulary&&) =
    default;

bool LearnablePhrase(std::string_view phrase) {
  return !LearnedPhrase(phrase).empty();
}

std::string LearnedPhrase(std::string_view phrase) {
  if (phrase.size() > kMaxPhraseInputBytes) {
    return std::string();
  }
  std::string cleaned = CleanSpokenText(phrase, kMaxPhraseInputBytes);
  if (cleaned.empty() || cleaned.size() > kMaxVocabularyPhraseBytes ||
      !IsValidUtf8(phrase)) {
    return std::string();
  }
  const NormalizedText text =
      Normalize(cleaned, kMaxVocabularyPhraseWords + 1);
  const size_t n = text.words.size();
  if (n == 0 || n > kMaxVocabularyPhraseWords || text.truncated) {
    return std::string();
  }
  bool letter = false;
  for (size_t i = 0; i < n; ++i) {
    const std::string_view word = text.Word(i);
    const bool digit = HasDigit(word);
    // A code, a year-like number, a card's digits; a token like "a1b2".
    if ((digit && word.size() >= 4 && !HasNonDigit(word)) ||
        (digit && HasNonDigit(word))) {
      return std::string();
    }
    letter = letter || HasNonDigit(word);
    for (std::string_view secret : kSecretWords) {
      if (word == secret) {
        return std::string();
      }
    }
  }
  if (!letter) {
    return std::string();
  }
  // No answer to a question, in any part of it.
  for (size_t first = 0; first < n; ++first) {
    for (size_t end = first + 1;
         end <= n && end - first <= kMaxAnswerSpanWords; ++end) {
      if (IsAnswer(JoinWords(text, first, end))) {
        return std::string();
      }
    }
  }
  if (IsAnswer(JoinWords(text, 0, n))) {
    return std::string();
  }
  return cleaned;
}

VocabularyEntry* PersonalVocabulary::FindName(const std::string& key) {
  for (VocabularyEntry& entry : entries_) {
    if (!entry.correction() && FoldForComparison(entry.phrase) == key) {
      return &entry;
    }
  }
  return nullptr;
}

VocabularyEntry* PersonalVocabulary::FindCorrection(const std::string& heard) {
  for (VocabularyEntry& entry : entries_) {
    if (entry.correction() && entry.heard == heard) {
      return &entry;
    }
  }
  return nullptr;
}

void PersonalVocabulary::Evict() {
  while (entries_.size() > kMaxVocabularyEntries) {
    auto oldest = std::min_element(
        entries_.begin(), entries_.end(),
        [](const VocabularyEntry& a, const VocabularyEntry& b) {
          if (a.last_used != b.last_used) {
            return a.last_used < b.last_used;
          }
          return a.uses < b.uses;
        });
    entries_.erase(oldest);
  }
}

bool PersonalVocabulary::NoteName(std::string_view phrase, int64_t now) {
  std::string learned = LearnedPhrase(phrase);
  if (learned.empty()) {
    return false;
  }
  if (VocabularyEntry* entry = FindName(FoldForComparison(learned))) {
    entry->uses = std::min(entry->uses + 1, kMaxVocabularyUses);
    entry->last_used = now;
    entry->phrase = std::move(learned);
    return true;
  }
  VocabularyEntry entry;
  entry.phrase = std::move(learned);
  entry.uses = 1;
  entry.last_used = now;
  entries_.push_back(std::move(entry));
  Evict();
  return true;
}

bool PersonalVocabulary::NoteCorrection(std::string_view heard,
                                        std::string_view meant,
                                        int64_t now) {
  const std::string heard_key = FoldForComparison(LearnedPhrase(heard));
  std::string learned = LearnedPhrase(meant);
  if (heard_key.empty() || learned.empty() ||
      heard_key == FoldForComparison(learned)) {
    return false;
  }
  if (VocabularyEntry* entry = FindCorrection(heard_key)) {
    entry->phrase = std::move(learned);
    entry->uses = std::min(entry->uses + 1, kMaxVocabularyUses);
    entry->last_used = now;
    return true;
  }
  VocabularyEntry entry;
  entry.phrase = std::move(learned);
  entry.heard = heard_key;
  entry.uses = 1;
  entry.last_used = now;
  entries_.push_back(std::move(entry));
  Evict();
  return true;
}

void PersonalVocabulary::NoteCorrectionUsed(std::string_view heard,
                                            int64_t now) {
  if (VocabularyEntry* entry = FindCorrection(std::string(heard))) {
    entry->uses = std::min(entry->uses + 1, kMaxVocabularyUses);
    entry->last_used = now;
  }
}

size_t PersonalVocabulary::Forget(std::string_view phrase) {
  const std::string key = FoldForComparison(phrase.substr(
      0, std::min(phrase.size(), kMaxPhraseInputBytes)));
  if (key.empty()) {
    return 0;
  }
  const size_t before = entries_.size();
  std::erase_if(entries_, [&key](const VocabularyEntry& entry) {
    return FoldForComparison(entry.phrase) == key || entry.heard == key;
  });
  return before - entries_.size();
}

void PersonalVocabulary::Clear() {
  entries_.clear();
}

void PersonalVocabulary::Age(int64_t now) {
  std::erase_if(entries_, [now](const VocabularyEntry& entry) {
    return entry.last_used < now - kVocabularyLifetimeSeconds ||
           entry.last_used > now + kFutureSlackSeconds;
  });
  Evict();
}

std::vector<std::string> PersonalVocabulary::HotwordPhrases(size_t max) const {
  std::vector<const VocabularyEntry*> corrections;
  std::vector<const VocabularyEntry*> names;
  for (const VocabularyEntry& entry : entries_) {
    if (entry.correction()) {
      corrections.push_back(&entry);
    } else if (entry.uses >= kHotwordMinUses) {
      names.push_back(&entry);
    }
  }
  auto recent_first = [](const VocabularyEntry* a, const VocabularyEntry* b) {
    if (a->last_used != b->last_used) {
      return a->last_used > b->last_used;
    }
    return a->uses > b->uses;
  };
  std::stable_sort(corrections.begin(), corrections.end(), recent_first);
  std::stable_sort(names.begin(), names.end(), recent_first);
  std::vector<std::string> phrases;
  std::vector<std::string> keys;
  for (const auto* group : {&corrections, &names}) {
    for (const VocabularyEntry* entry : *group) {
      if (phrases.size() >= max) {
        return phrases;
      }
      std::string key = FoldForComparison(entry->phrase);
      if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
        continue;
      }
      keys.push_back(std::move(key));
      phrases.push_back(entry->phrase);
    }
  }
  return phrases;
}

std::optional<std::string> PersonalVocabulary::ApplyCorrections(
    std::string_view utterance,
    std::vector<std::string>* used) const {
  if (utterance.size() > kMaxUtteranceBytes || !IsValidUtf8(utterance)) {
    return std::nullopt;
  }
  struct Rule {
    std::vector<std::string> words;
    // Into |entries_|.
    size_t entry = 0;
  };
  std::vector<Rule> rules;
  for (size_t index = 0; index < entries_.size(); ++index) {
    const VocabularyEntry& entry = entries_[index];
    if (!entry.correction()) {
      continue;
    }
    Rule rule;
    rule.entry = index;
    size_t start = 0;
    while (start < entry.heard.size()) {
      size_t space = entry.heard.find(' ', start);
      if (space == std::string::npos) {
        space = entry.heard.size();
      }
      if (space > start) {
        rule.words.push_back(entry.heard.substr(start, space - start));
      }
      start = space + 1;
    }
    if (!rule.words.empty()) {
      rules.push_back(std::move(rule));
    }
  }
  if (rules.empty()) {
    return std::nullopt;
  }
  // The longest first.
  std::stable_sort(rules.begin(), rules.end(),
                   [](const Rule& a, const Rule& b) {
                     return a.words.size() > b.words.size();
                   });
  const NormalizedText text = Normalize(utterance, 512);
  const size_t n = text.words.size();
  std::string out;
  size_t copied = 0;
  bool changed = false;
  size_t i = 0;
  while (i < n) {
    const Rule* applied = nullptr;
    for (const Rule& rule : rules) {
      const size_t k = rule.words.size();
      if (i + k > n) {
        continue;
      }
      bool match = true;
      for (size_t j = 0; j < k && match; ++j) {
        match = text.Word(i + j) == rule.words[j];
      }
      if (match) {
        applied = &rule;
        break;
      }
    }
    if (!applied) {
      ++i;
      continue;
    }
    const auto [begin, end] = SourceSpan(text, i, i + applied->words.size());
    if (begin < copied || end > utterance.size() || begin > end) {
      ++i;
      continue;
    }
    out.append(utterance.substr(copied, begin - copied));
    out += entries_[applied->entry].phrase;
    copied = end;
    changed = true;
    if (used) {
      used->push_back(entries_[applied->entry].heard);
    }
    i += applied->words.size();
  }
  if (!changed) {
    return std::nullopt;
  }
  out.append(utterance.substr(copied));
  return CleanSpokenText(out, kMaxUtteranceBytes);
}

bool PersonalVocabulary::Restore(VocabularyEntry entry) {
  if (entries_.size() >= kMaxVocabularyEntries) {
    return false;
  }
  std::string learned = LearnedPhrase(entry.phrase);
  if (learned.empty() || learned != entry.phrase || entry.uses < 1 ||
      entry.uses > kMaxVocabularyUses || entry.last_used < 0) {
    return false;
  }
  if (entry.correction()) {
    // Stored folded, and learnable itself.
    if (FoldForComparison(LearnedPhrase(entry.heard)) != entry.heard ||
        entry.heard == FoldForComparison(entry.phrase) ||
        FindCorrection(entry.heard)) {
      return false;
    }
  } else if (FindName(FoldForComparison(entry.phrase))) {
    return false;
  }
  entries_.push_back(std::move(entry));
  return true;
}

bool ReadsPageText(VoiceCommandKind kind) {
  using K = VoiceCommandKind;
  switch (kind) {
    case K::kReadPage:
    case K::kReadFromHere:
    case K::kReadHeadings:
    case K::kReadLinks:
    case K::kReadResults:
    case K::kReadTitle:
    case K::kReadSelection:
    case K::kReadSection:
    case K::kDescribeScreen:
    case K::kNextHeading:
    case K::kPreviousHeading:
    case K::kNextLink:
    case K::kPreviousLink:
    case K::kNextItem:
    case K::kPreviousItem:
    case K::kNextParagraph:
    case K::kPreviousParagraph:
    case K::kReadBack:
    case K::kWhereAmI:
    case K::kRepeat:
    // "Continue reading" (and "continue" after "pause").
    case K::kResume:
    // A question about the page: the answer is made from its text.
    case K::kAskPage:
      return true;
    default:
      return false;
  }
}

std::optional<AlignedCorrection> AlignCorrection(std::string_view previous,
                                                 std::string_view said) {
  if (previous.size() > kMaxUtteranceBytes || said.size() > kMaxUtteranceBytes) {
    return std::nullopt;
  }
  const std::string prev_clean = CleanSpokenText(previous, kMaxUtteranceBytes);
  const std::string said_clean = CleanSpokenText(said, kMaxUtteranceBytes);
  const NormalizedText p = Normalize(prev_clean, kMaxAlignWords);
  const NormalizedText s = Normalize(said_clean, kMaxAlignWords);
  const size_t n = p.words.size();
  const size_t m = s.words.size();
  if (n == 0 || m == 0 || p.truncated || s.truncated) {
    return std::nullopt;
  }
  AlignedCorrection out;
  // A whole command: the same words at either end.
  size_t a = 0;
  while (a < n && a < m && p.Word(a) == s.Word(a)) {
    ++a;
  }
  size_t b = 0;
  while (b < n - a && b < m - a && p.Word(n - 1 - b) == s.Word(m - 1 - b)) {
    ++b;
  }
  if (a + b > 0) {
    if (a + b >= n || a + b >= m) {
      // Nothing changed, or words only added or dropped.
      return std::nullopt;
    }
    if (n - b - a > kMaxVocabularyPhraseWords ||
        m - b - a > kMaxVocabularyPhraseWords) {
      return std::nullopt;
    }
    out.corrected = said_clean;
    // Only a mishearing is learned: the two must sound alike ("no, I said
    // open the BBC" after "open YouTube" is another request).
    if (KeysSoundAlike(SpanKey(p, a, n - b), SpanKey(s, a, m - b))) {
      out.heard = JoinWords(p, a, n - b);
      out.meant = SourceText(said_clean, s, a, m - b - 1, {});
    }
    return out;
  }
  // The words meant: the span of |previous| that sounds most like them.
  if (m > kMaxVocabularyPhraseWords) {
    return std::nullopt;
  }
  const std::string said_key = SpanKey(s, 0, m);
  if (said_key.empty()) {
    return std::nullopt;
  }
  size_t best_first = 0;
  size_t best_end = 0;
  int best = -1;
  const size_t shortest = m > 1 ? m - 1 : 1;
  for (size_t len = shortest; len <= m + 1; ++len) {
    for (size_t first = 0; first + len <= n; ++first) {
      const std::string key = SpanKey(p, first, first + len);
      if (key.empty()) {
        continue;
      }
      const int bound =
          CorrectionBound(std::max(key.size(), said_key.size()));
      const int distance = PhoneticDistance(key, said_key, bound);
      // The best; on a tie the later span (names come last).
      if (distance <= bound && (best < 0 || distance <= best)) {
        best = distance;
        best_first = first;
        best_end = first + len;
      }
    }
  }
  if (best < 0) {
    return std::nullopt;
  }
  out.heard = JoinWords(p, best_first, best_end);
  out.meant = said_clean;
  if (out.heard == FoldForComparison(said_clean)) {
    return std::nullopt;
  }
  const auto [begin, end] = SourceSpan(p, best_first, best_end);
  if (begin > end || end > prev_clean.size()) {
    return std::nullopt;
  }
  out.corrected = CleanSpokenText(prev_clean.substr(0, begin) + said_clean +
                                      prev_clean.substr(end),
                                  kMaxUtteranceBytes);
  return out;
}

}  // namespace voice
}  // namespace molt_ai
