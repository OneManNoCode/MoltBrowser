// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/spoken_answers.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace voice {

namespace {

std::vector<SpokenAnswer> Sorted(std::vector<SpokenAnswer> answers) {
  std::sort(answers.begin(), answers.end(),
            [](const SpokenAnswer& a, const SpokenAnswer& b) {
              return a.english < b.english;
            });
  return answers;
}

}  // namespace

const std::vector<SpokenAnswer>& SpokenAnswers(Language language) {
  switch (language) {
    case Language::kEnglish:
      break;
    case Language::kSpanish: {
      // Built once, never destroyed (no exit-time destructor).
      static const std::vector<SpokenAnswer>* const spanish =
          new std::vector<SpokenAnswer>(Sorted(MakeSpanishSpokenAnswers()));
      return *spanish;
    }
  }
  static const std::vector<SpokenAnswer>* const english =
      new std::vector<SpokenAnswer>();
  return *english;
}

std::string_view Translated(Language language, std::string_view english) {
  if (language == Language::kEnglish) {
    return english;
  }
  const std::vector<SpokenAnswer>& answers = SpokenAnswers(language);
  const auto it =
      std::lower_bound(answers.begin(), answers.end(), english,
                       [](const SpokenAnswer& a, std::string_view key) {
                         return a.english < key;
                       });
  if (it == answers.end() || it->english != english) {
    return english;
  }
  return it->text;
}

std::string Localized(Language language, std::string_view text) {
  return std::string(Translated(language, text));
}

std::string Said(Language language,
                 std::string_view english,
                 std::initializer_list<std::string_view> args) {
  const std::string_view sentence = Translated(language, english);
  std::string out;
  out.reserve(sentence.size());
  for (size_t i = 0; i < sentence.size(); ++i) {
    const char c = sentence[i];
    if (c == '{' && i + 1 < sentence.size() && sentence[i + 1] == '{') {
      out.push_back('{');
      ++i;
      continue;
    }
    if (c == '{' && i + 2 < sentence.size() && sentence[i + 1] >= '0' &&
        sentence[i + 1] <= '9' && sentence[i + 2] == '}') {
      const size_t index = static_cast<size_t>(sentence[i + 1] - '0');
      size_t at = 0;
      for (std::string_view arg : args) {
        if (at++ == index) {
          out.append(arg);
          break;
        }
      }
      i += 2;
      continue;
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace voice
}  // namespace molt_ai
