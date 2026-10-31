// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/spoken_phrases.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace voice {

LanguagePhrases::LanguagePhrases() = default;
LanguagePhrases::~LanguagePhrases() = default;
LanguagePhrases::LanguagePhrases(const LanguagePhrases&) = default;
LanguagePhrases& LanguagePhrases::operator=(const LanguagePhrases&) = default;
LanguagePhrases::LanguagePhrases(LanguagePhrases&&) = default;
LanguagePhrases& LanguagePhrases::operator=(LanguagePhrases&&) = default;

const LanguagePhrases& PhrasesFor(Language language) {
  switch (language) {
    case Language::kEnglish:
      break;
    case Language::kSpanish: {
      // Built once, never destroyed (no exit-time destructor).
      static const LanguagePhrases* const spanish =
          new LanguagePhrases(MakeSpanishPhrases());
      return *spanish;
    }
  }
  // Built once, never destroyed (no exit-time destructor).
  static const LanguagePhrases* const english =
      new LanguagePhrases(MakeEnglishPhrases());
  return *english;
}

size_t PhraseLengthAt(std::string_view phrase,
                      const WordList& words,
                      size_t at) {
  size_t count = 0;
  while (!phrase.empty()) {
    const size_t space = phrase.find(' ');
    const std::string_view word = phrase.substr(0, space);
    if (at + count >= words.size() || words[at + count] != word)
      return 0;
    ++count;
    if (space == std::string_view::npos)
      break;
    phrase.remove_prefix(space + 1);
  }
  return count;
}

size_t LongestPhraseAt(const std::vector<std::string_view>& phrases,
                       const WordList& words,
                       size_t at) {
  size_t best = 0;
  for (std::string_view phrase : phrases) {
    best = std::max(best, PhraseLengthAt(phrase, words, at));
  }
  return best;
}

bool HasPhrase(const std::vector<std::string_view>& phrases,
               std::string_view phrase) {
  return std::find(phrases.begin(), phrases.end(), phrase) != phrases.end();
}

bool InSortedList(const std::vector<std::string_view>& sorted,
                  std::string_view word) {
  return std::binary_search(sorted.begin(), sorted.end(), word);
}

const NumberWord* FindNumberWord(const LanguagePhrases& phrases,
                                 std::string_view word) {
  const auto it =
      std::lower_bound(phrases.numbers.begin(), phrases.numbers.end(), word,
                       [](const NumberWord& entry, std::string_view w) {
                         return entry.word < w;
                       });
  return (it != phrases.numbers.end() && it->word == word) ? &*it : nullptr;
}

const RoleWord* FindRoleWord(const LanguagePhrases& phrases,
                             std::string_view word) {
  const auto it = std::lower_bound(
      phrases.role_words.begin(), phrases.role_words.end(), word,
      [](const RoleWord& entry, std::string_view w) { return entry.word < w; });
  return (it != phrases.role_words.end() && it->word == word) ? &*it : nullptr;
}

}  // namespace voice
}  // namespace molt_ai
