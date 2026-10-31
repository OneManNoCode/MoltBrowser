// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/phonetic_key_es.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "chrome/browser/molt_ai/voice/phonetic_key.h"

namespace molt_ai {
namespace voice {

namespace {

bool IsVowel(char c) {
  return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

bool FrontVowel(char c) {
  return c == 'e' || c == 'i';
}

}  // namespace

std::string SpanishPhoneticKey(std::string_view w) {
  if (w.empty() || w.size() > kMaxPhoneticWordBytes)
    return std::string();
  for (char c : w) {
    if (c < 'a' || c > 'z')
      return std::string();
  }
  const size_t n = w.size();
  const auto at = [&](size_t i) -> char { return i < n ? w[i] : '\0'; };
  std::string key;
  const auto emit = [&](char sound) {
    if (key.empty() || key.back() != sound)
      key.push_back(sound);
  };
  size_t i = 0;
  // At the start: "psicología" (silent p), "huevo" (w), "hielo" (y).
  if (w.substr(0, 2) == "ps") {
    i = 1;
  } else if (n >= 3 && w[0] == 'h' && w[1] == 'u' && IsVowel(w[2])) {
    emit('W');
    i = 2;
  } else if (n >= 3 && w[0] == 'h' && w[1] == 'i' && IsVowel(w[2])) {
    emit('Y');
    i = 2;
  }
  while (i < n) {
    const char c = w[i];
    const char next = at(i + 1);
    const char after = at(i + 2);
    switch (c) {
      case 'a':
      case 'e':
      case 'i':
      case 'o':
      case 'u':
        emit('a');
        ++i;
        break;
      case 'y':
        // A consonant before a vowel ("yo", "mayo"); a vowel otherwise
        // ("hoy", "y").
        emit(IsVowel(next) ? 'Y' : 'a');
        ++i;
        break;
      case 'b':
      case 'v':
      case 'p':
      case 'f':
        emit('P');
        ++i;
        break;
      case 'd':
      case 't':
        emit('T');
        ++i;
        break;
      case 'c':
        if (next == 'h') {
          emit('X');
          i += 2;
        } else if (FrontVowel(next)) {
          emit('S');
          ++i;
        } else {
          emit('K');
          ++i;
        }
        break;
      case 'q':
        emit('K');
        i += next == 'u' ? 2 : 1;  // "queso": the u is silent
        break;
      case 'g':
        emit('K');
        // "guerra", "guitarra": the u is silent before e or i.
        i += (next == 'u' && FrontVowel(after)) ? 2 : 1;
        break;
      case 'j':
      case 'k':
        emit('K');
        ++i;
        break;
      case 's':
      case 'z':
        emit('S');
        ++i;
        break;
      case 'x':
        if (i == 0) {
          emit('S');  // "xilófono"
        } else {
          emit('K');
          emit('S');
        }
        ++i;
        break;
      case 'h':
        ++i;  // silent ("hola", "ahora"); "ch" is read with the c
        break;
      case 'l':
        if (next == 'l') {
          emit('Y');  // "calle", "llave"
          i += 2;
        } else {
          emit('L');
          ++i;
        }
        break;
      case 'r':
        emit('R');
        ++i;
        break;
      case 'm':
        emit('M');
        ++i;
        break;
      case 'n':
        emit('N');
        ++i;
        break;
      case 'w':
        if (IsVowel(next))
          emit('W');
        ++i;
        break;
      default:
        ++i;
        break;
    }
  }
  return key;
}

std::string PhoneticKeyFor(Language language, std::string_view word) {
  switch (language) {
    case Language::kEnglish:
      break;
    case Language::kSpanish:
      return SpanishPhoneticKey(word);
  }
  return PhoneticKey(word);
}

}  // namespace voice
}  // namespace molt_ai
