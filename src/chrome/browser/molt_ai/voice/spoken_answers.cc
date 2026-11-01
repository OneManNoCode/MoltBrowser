// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/spoken_answers.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <span>
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

namespace {

// |sentence| with |args| in its placeholders (Said()'s rules).
std::string Filled(std::string_view sentence,
                   std::span<const std::string_view> args) {
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
      if (index < args.size()) {
        out.append(args[index]);
      }
      i += 2;
      continue;
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace

std::string Said(Language language,
                 std::string_view english,
                 std::initializer_list<std::string_view> args) {
  return Filled(Translated(language, english),
                std::span<const std::string_view>(args.begin(), args.size()));
}

namespace {

// A key matches only with this much of its own text (not "{0}."): the
// words it names are the browser's.
constexpr size_t kMinKeyLiteralBytes = 12;

// The words |text| has where |key| has its placeholders, when |text| is
// |key| with words in them: each placeholder takes the shortest text before
// the key's next literal part, the last one takes what is left before the
// key's end. Placeholders as Said() reads them; "{{" is "{". |literal| is
// how much of |key| is its own text.
bool MatchKey(std::string_view key,
              std::string_view text,
              std::array<std::string_view, 10>& args,
              size_t& arg_count,
              size_t& literal) {
  // The key's literal parts and the placeholders between them.
  std::array<std::string, 11> parts;
  std::array<int, 10> slots{};
  size_t slot_count = 0;
  for (size_t i = 0; i < key.size(); ++i) {
    const char c = key[i];
    if (c == '{' && i + 1 < key.size() && key[i + 1] == '{') {
      parts[slot_count].push_back('{');
      ++i;
      continue;
    }
    if (c == '{' && i + 2 < key.size() && key[i + 1] >= '0' &&
        key[i + 1] <= '9' && key[i + 2] == '}') {
      if (slot_count == slots.size()) {
        return false;
      }
      slots[slot_count++] = key[i + 1] - '0';
      i += 2;
      continue;
    }
    parts[slot_count].push_back(c);
  }
  if (slot_count == 0) {
    return false;
  }
  literal = 0;
  for (size_t i = 0; i <= slot_count; ++i) {
    literal += parts[i].size();
  }
  if (literal < kMinKeyLiteralBytes) {
    return false;
  }
  // Two placeholders with nothing between them can't be told apart.
  for (size_t i = 1; i < slot_count; ++i) {
    if (parts[i].empty()) {
      return false;
    }
  }
  if (text.substr(0, parts[0].size()) != parts[0]) {
    return false;
  }
  const std::string& last = parts[slot_count];
  if (text.size() < parts[0].size() + last.size() ||
      text.substr(text.size() - last.size()) != last) {
    return false;
  }
  const size_t end = text.size() - last.size();
  size_t at = parts[0].size();
  arg_count = 0;
  for (size_t i = 0; i < slot_count; ++i) {
    size_t next = end;
    if (i + 1 < slot_count) {
      next = text.find(parts[i + 1], at);
      if (next == std::string_view::npos || next > end) {
        return false;
      }
    }
    const std::string_view word = text.substr(at, next - at);
    const size_t index = static_cast<size_t>(slots[i]);
    if (index >= args.size()) {
      return false;
    }
    // A placeholder said twice holds the same words twice.
    if (index < arg_count && !args[index].empty() && args[index] != word) {
      return false;
    }
    args[index] = word;
    arg_count = std::max(arg_count, index + 1);
    at = next + (i + 1 < slot_count ? parts[i + 1].size() : 0);
  }
  return true;
}

}  // namespace

std::string Relocalized(Language language, std::string_view english) {
  if (language == Language::kEnglish ||
      english.size() > kMaxRelocalizedBytes) {
    return std::string(english);
  }
  const std::string_view whole = Translated(language, english);
  if (whole != english) {
    return std::string(whole);
  }
  // The key with the most text of its own that made it.
  const SpokenAnswer* best = nullptr;
  std::array<std::string_view, 10> best_args;
  size_t best_count = 0;
  size_t best_literal = 0;
  for (const SpokenAnswer& answer : SpokenAnswers(language)) {
    std::array<std::string_view, 10> args;
    size_t count = 0;
    size_t literal = 0;
    if (MatchKey(answer.english, english, args, count, literal) &&
        literal > best_literal) {
      best = &answer;
      best_args = args;
      best_count = count;
      best_literal = literal;
    }
  }
  if (!best) {
    return std::string(english);
  }
  // Said() with the words that filled the key.
  return Filled(best->text, std::span<const std::string_view>(
                                best_args.data(), best_count));
}

}  // namespace voice
}  // namespace molt_ai
