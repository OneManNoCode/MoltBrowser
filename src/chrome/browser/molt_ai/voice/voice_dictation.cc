// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_dictation.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace voice {

namespace {

// How a mark sits between the words.
enum class Sits : uint8_t {
  kAfterWord,   // sticks to the word before: ", . ? ! : ; )"
  kBeforeWord,  // sticks to the word after: "( ¿ ¡" and opening quotes
  kNewLine,     // ends the line: no space either side
  kJoins,       // joins the words: "-"
  kSpaced,      // a space either side: " - "
};

struct Mark {
  std::string_view words;  // folded, one space between
  std::string_view mark;
  Sits sits = Sits::kAfterWord;
  // A sentence ends with it: the next word starts with a capital.
  bool ends_sentence = false;
};

constexpr Mark kEnglishMarks[] = {
    {"comma", ",", Sits::kAfterWord},
    {"period", ".", Sits::kAfterWord, true},
    {"full stop", ".", Sits::kAfterWord, true},
    {"question mark", "?", Sits::kAfterWord, true},
    {"exclamation mark", "!", Sits::kAfterWord, true},
    {"exclamation point", "!", Sits::kAfterWord, true},
    {"colon", ":", Sits::kAfterWord},
    {"semicolon", ";", Sits::kAfterWord},
    {"semi colon", ";", Sits::kAfterWord},
    {"ellipsis", "...", Sits::kAfterWord},
    {"dot dot dot", "...", Sits::kAfterWord},
    {"close quote", "\"", Sits::kAfterWord},
    {"end quote", "\"", Sits::kAfterWord},
    {"unquote", "\"", Sits::kAfterWord},
    {"close parenthesis", ")", Sits::kAfterWord},
    {"close paren", ")", Sits::kAfterWord},
    {"close bracket", ")", Sits::kAfterWord},
    {"apostrophe", "'", Sits::kJoins},
    {"open quote", "\"", Sits::kBeforeWord},
    {"begin quote", "\"", Sits::kBeforeWord},
    {"open parenthesis", "(", Sits::kBeforeWord},
    {"open paren", "(", Sits::kBeforeWord},
    {"open bracket", "(", Sits::kBeforeWord},
    {"new line", "\n", Sits::kNewLine, true},
    {"newline", "\n", Sits::kNewLine, true},
    {"new paragraph", "\n\n", Sits::kNewLine, true},
    {"hyphen", "-", Sits::kJoins},
    {"dash", "-", Sits::kSpaced},
};

constexpr Mark kSpanishMarks[] = {
    {"coma", ",", Sits::kAfterWord},
    {"punto y coma", ";", Sits::kAfterWord},
    {"dos puntos", ":", Sits::kAfterWord},
    {"puntos suspensivos", "...", Sits::kAfterWord},
    {"punto y aparte", ".\n", Sits::kNewLine, true},
    {"punto y seguido", ".", Sits::kAfterWord, true},
    {"punto final", ".", Sits::kAfterWord, true},
    {"punto", ".", Sits::kAfterWord, true},
    {"cierra interrogacion", "?", Sits::kAfterWord, true},
    {"signo de interrogacion", "?", Sits::kAfterWord, true},
    {"cierra exclamacion", "!", Sits::kAfterWord, true},
    {"signo de exclamacion", "!", Sits::kAfterWord, true},
    {"cierra comillas", "\"", Sits::kAfterWord},
    {"cierra parentesis", ")", Sits::kAfterWord},
    {"abre interrogacion", "\xC2\xBF", Sits::kBeforeWord},
    {"abre exclamacion", "\xC2\xA1", Sits::kBeforeWord},
    {"abre comillas", "\"", Sits::kBeforeWord},
    {"abre parentesis", "(", Sits::kBeforeWord},
    {"nueva linea", "\n", Sits::kNewLine, true},
    {"salto de linea", "\n", Sits::kNewLine, true},
    {"nuevo parrafo", "\n\n", Sits::kNewLine, true},
    {"guion", "-", Sits::kJoins},
    {"raya", "-", Sits::kSpaced},
};

// "The word comma": the next word as it is.
constexpr std::string_view kEnglishLiteral = "the word";
constexpr std::string_view kSpanishLiteral = "la palabra";

std::vector<std::string_view> SplitOnSpaces(std::string_view text) {
  std::vector<std::string_view> tokens;
  size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) {
      ++start;
    }
    size_t end = start;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t') {
      ++end;
    }
    if (end > start) {
      tokens.push_back(text.substr(start, end - start));
    }
    start = end;
  }
  return tokens;
}

// How many tokens from |at| spell |words| (folded, one space between); 0
// when they don't.
size_t PhraseAt(const std::vector<std::string>& folded,
                size_t at,
                std::string_view words) {
  size_t used = 0;
  size_t start = 0;
  while (start <= words.size()) {
    size_t space = words.find(' ', start);
    if (space == std::string_view::npos) {
      space = words.size();
    }
    const std::string_view word = words.substr(start, space - start);
    if (at + used >= folded.size() || folded[at + used] != word) {
      return 0;
    }
    ++used;
    start = space + 1;
  }
  return used;
}

bool EndsWithSpaceOrLine(const std::string& out) {
  return out.empty() || out.back() == ' ' || out.back() == '\n';
}

void TrimSpaces(std::string& out) {
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
}

std::string Capitalized(std::string_view word) {
  std::string out(word);
  if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') {
    out[0] = static_cast<char>(out[0] - 'a' + 'A');
  }
  return out;
}

// Whether |out| ends a sentence: its last mark, before any space, is
// ". ? !" or a new line.
bool EndsSentence(std::string_view out) {
  size_t i = out.size();
  while (i > 0 && out[i - 1] == ' ') {
    --i;
  }
  if (i == 0) {
    return false;
  }
  const char last = out[i - 1];
  return last == '.' || last == '?' || last == '!' || last == '\n';
}

}  // namespace

std::string ApplySpokenPunctuation(std::string_view text, Language language) {
  const std::vector<std::string_view> tokens = SplitOnSpaces(text);
  std::vector<std::string> folded;
  folded.reserve(tokens.size());
  for (std::string_view token : tokens) {
    folded.push_back(FoldForComparison(token));
  }
  const bool spanish = language == Language::kSpanish;
  const std::span<const Mark> marks =
      spanish ? std::span<const Mark>(kSpanishMarks)
              : std::span<const Mark>(kEnglishMarks);
  const std::string_view literal = spanish ? kSpanishLiteral : kEnglishLiteral;

  std::string out;
  // The next word sticks to what is before it (after an opening mark, a
  // hyphen or a new line), and starts a sentence.
  bool glue = true;
  bool capital = false;
  const auto add_word = [&](std::string_view word) {
    if (!glue && !EndsWithSpaceOrLine(out)) {
      out += ' ';
    }
    out += capital ? Capitalized(word) : std::string(word);
    glue = false;
    capital = false;
  };
  for (size_t i = 0; i < tokens.size();) {
    // "the word comma": the word itself.
    if (const size_t n = PhraseAt(folded, i, literal);
        n && i + n < tokens.size()) {
      add_word(tokens[i + n]);
      i += n + 1;
      continue;
    }
    // The longest mark said here (tables list longer phrases first where
    // they share a start).
    const Mark* found = nullptr;
    size_t found_length = 0;
    for (const Mark& mark : marks) {
      const size_t n = PhraseAt(folded, i, mark.words);
      if (n > found_length) {
        found = &mark;
        found_length = n;
      }
    }
    if (!found) {
      // "See you" as the recognizer writes it, in texting's "CU" (the
      // spoken suite's runs), or as "C you" at a sentence's start (build
      // 48's): the words said.
      const bool c_you = !spanish && tokens[i] == "C" &&
                         i + 1 < tokens.size() && folded[i + 1] == "you" &&
                         (out.empty() || EndsSentence(out));
      add_word(!spanish && tokens[i] == "CU" ? std::string_view("see you")
               : c_you                       ? std::string_view("see")
                                             : tokens[i]);
      ++i;
      continue;
    }
    switch (found->sits) {
      case Sits::kAfterWord:
        TrimSpaces(out);
        out += found->mark;
        glue = false;
        break;
      case Sits::kBeforeWord:
        if (!glue && !EndsWithSpaceOrLine(out)) {
          out += ' ';
        }
        out += found->mark;
        glue = true;
        break;
      case Sits::kNewLine:
        TrimSpaces(out);
        out += found->mark;
        glue = true;
        break;
      case Sits::kJoins:
        TrimSpaces(out);
        out += found->mark;
        glue = true;
        break;
      case Sits::kSpaced:
        TrimSpaces(out);
        if (!out.empty()) {
          out += ' ';
        }
        out += found->mark;
        out += ' ';
        glue = true;
        break;
    }
    if (found->ends_sentence) {
      capital = true;
    }
    i += found_length;
  }
  TrimSpaces(out);
  return out;
}

std::string JoinDictation(std::string_view value, std::string_view typed) {
  if (typed.empty()) {
    return std::string(value);
  }
  std::string next(typed);
  if (EndsSentence(value) && next[0] >= 'a' && next[0] <= 'z') {
    next[0] = static_cast<char>(next[0] - 'a' + 'A');
  }
  if (value.empty()) {
    return next;
  }
  const char first = next[0];
  const bool sticks = first == ',' || first == '.' || first == '?' ||
                      first == '!' || first == ':' || first == ';' ||
                      first == ')' || first == '\n';
  std::string out(value);
  if (sticks) {
    TrimSpaces(out);
    return out + next;
  }
  if (EndsWithSpaceOrLine(out)) {
    return out + next;
  }
  return out + " " + next;
}

}  // namespace voice
}  // namespace molt_ai
