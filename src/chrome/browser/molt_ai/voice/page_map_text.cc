// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/page_map_text.h"

#include <algorithm>
#include <array>
#include <vector>

#include "chrome/browser/molt_ai/agents/hidden_chars.h"

namespace molt_ai {
namespace voice {

namespace {

// A longer text is cut at its last space when that space is at most this
// many code points before the cut.
constexpr size_t kWordBoundarySlack = 16;

// The parsers below read at most this much of their input: a field is
// short, and anything longer is not one.
constexpr size_t kMaxFieldScanBytes = 256;

constexpr int kMaxDurationSeconds = 1000 * 3600;

bool IsContinuation(unsigned char c) {
  return (c & 0xC0) == 0x80;
}

// Decodes the code point at |s|[i]: its length in bytes (1 to 4), or 0 if
// the bytes there aren't valid UTF-8 (overlong forms, surrogates and values
// past U+10FFFF included).
size_t DecodeUtf8(std::string_view s, size_t i, uint32_t* cp) {
  auto at = [&](size_t k) -> unsigned char {
    return i + k < s.size() ? static_cast<unsigned char>(s[i + k]) : 0;
  };
  const unsigned char b0 = at(0);
  if (b0 < 0x80) {
    *cp = b0;
    return 1;
  }
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    if (!IsContinuation(at(1))) {
      return 0;
    }
    *cp = ((b0 & 0x1Fu) << 6) | (at(1) & 0x3Fu);
    return 2;
  }
  if (b0 >= 0xE0 && b0 <= 0xEF) {
    const unsigned char b1 = at(1);
    const unsigned char lo = b0 == 0xE0 ? 0xA0 : 0x80;
    const unsigned char hi = b0 == 0xED ? 0x9F : 0xBF;
    if (b1 < lo || b1 > hi || !IsContinuation(at(2))) {
      return 0;
    }
    *cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (at(2) & 0x3Fu);
    return 3;
  }
  if (b0 >= 0xF0 && b0 <= 0xF4) {
    const unsigned char b1 = at(1);
    const unsigned char lo = b0 == 0xF0 ? 0x90 : 0x80;
    const unsigned char hi = b0 == 0xF4 ? 0x8F : 0xBF;
    if (b1 < lo || b1 > hi || !IsContinuation(at(2)) ||
        !IsContinuation(at(3))) {
      return 0;
    }
    *cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) | ((at(2) & 0x3Fu) << 6) |
          (at(3) & 0x3Fu);
    return 4;
  }
  return 0;
}

bool IsSpaceLike(uint32_t cp) {
  return cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D ||
         cp == 0x20 || cp == 0xA0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
         cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

// Characters dropped beyond hidden_chars' set: the remaining C0 controls
// and DEL, the combining grapheme joiner, private-use characters (icon
// fonts), noncharacters, the interlinear annotation and object replacement
// characters, and U+FFFD.
bool IsDropped(uint32_t cp) {
  if (cp < 0x20 || cp == 0x7F) {
    return true;
  }
  if (cp == 0x034F) {
    return true;
  }
  if (cp >= 0xE000 && cp <= 0xF8FF) {
    return true;
  }
  if (cp >= 0xF0000) {
    return true;  // planes 15 and 16 are private use
  }
  if (cp >= 0xFDD0 && cp <= 0xFDEF) {
    return true;
  }
  if ((cp & 0xFFFE) == 0xFFFE) {
    return true;  // U+xFFFE, U+xFFFF in every plane
  }
  if (cp >= 0xFFF9 && cp <= 0xFFFD) {
    return true;
  }
  return false;
}

bool IsAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

bool IsAsciiAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

char ToLowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string_view TrimSpaces(std::string_view s) {
  while (!s.empty() && s.front() == ' ') {
    s.remove_prefix(1);
  }
  while (!s.empty() && s.back() == ' ') {
    s.remove_suffix(1);
  }
  return s;
}

// Lower-case ASCII of the first kMaxFieldScanBytes bytes, trimmed.
std::string LowerField(std::string_view text) {
  std::string out(TrimSpaces(text.substr(0, kMaxFieldScanBytes)));
  for (char& c : out) {
    c = ToLowerAscii(c);
  }
  return out;
}

// Splits |s| at spaces and at |extra| separators; empty pieces are left
// out. At most |max_tokens| + 1 tokens (so a caller can tell "too many").
std::vector<std::string_view> Tokens(std::string_view s,
                                     std::string_view extra,
                                     size_t max_tokens) {
  std::vector<std::string_view> out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    const bool sep = i == s.size() || s[i] == ' ' ||
                     extra.find(s[i]) != std::string_view::npos;
    if (!sep) {
      continue;
    }
    if (i > start) {
      out.push_back(s.substr(start, i - start));
      if (out.size() > max_tokens) {
        return out;
      }
    }
    start = i + 1;
  }
  return out;
}

bool AllDigits(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), IsAsciiDigit);
}

// The value of a run of at most 9 digits; -1 otherwise.
int SmallNumber(std::string_view s) {
  if (!AllDigits(s) || s.size() > 9) {
    return -1;
  }
  int v = 0;
  for (char c : s) {
    v = v * 10 + (c - '0');
  }
  return v;
}

bool OneOf(std::string_view s, std::initializer_list<std::string_view> set) {
  return std::find(set.begin(), set.end(), s) != set.end();
}

// Seconds in one of a duration's units, or 0.
int DurationUnitSeconds(std::string_view unit) {
  if (OneOf(unit, {"h", "hr", "hrs", "hour", "hours"})) {
    return 3600;
  }
  if (OneOf(unit, {"m", "min", "mins", "minute", "minutes"})) {
    return 60;
  }
  if (OneOf(unit, {"s", "sec", "secs", "second", "seconds"})) {
    return 1;
  }
  return 0;
}

bool IsTimeUnit(std::string_view unit) {
  return DurationUnitSeconds(unit) != 0 ||
         OneOf(unit, {"day", "days", "week", "weeks", "wk", "wks", "month",
                      "months", "mo", "mos", "year", "years", "yr", "yrs"});
}

std::optional<int> ParseClockDuration(std::string_view t) {
  // 1 to 4 digits, then one or two ":NN" groups.
  std::vector<std::string_view> groups = Tokens(t, ":", 3);
  if (groups.size() < 2 || groups.size() > 3 ||
      std::count(t.begin(), t.end(), ':') !=
          static_cast<std::ptrdiff_t>(groups.size() - 1) ||
      t.front() == ':' || t.back() == ':') {
    return std::nullopt;
  }
  if (groups[0].size() > 4) {
    return std::nullopt;
  }
  int64_t total = 0;
  for (size_t g = 0; g < groups.size(); ++g) {
    const int v = SmallNumber(groups[g]);
    if (v < 0 || (g > 0 && (groups[g].size() != 2 || v >= 60))) {
      return std::nullopt;
    }
    total = total * 60 + v;
  }
  if (total > kMaxDurationSeconds) {
    return std::nullopt;
  }
  return static_cast<int>(total);
}

std::optional<int> ParseIsoDuration(std::string_view t) {
  // "pt" then number-unit pairs among h, m, s, in that order.
  if (t.size() < 4 || t.substr(0, 2) != "pt") {
    return std::nullopt;
  }
  int64_t total = 0;
  int last_unit = 3601;
  size_t i = 2;
  bool any = false;
  while (i < t.size()) {
    size_t j = i;
    while (j < t.size() && IsAsciiDigit(t[j])) {
      ++j;
    }
    if (j == i || j - i > 6 || j >= t.size()) {
      return std::nullopt;
    }
    const int unit = DurationUnitSeconds(t.substr(j, 1));
    if (unit == 0 || unit >= last_unit) {
      return std::nullopt;
    }
    total += static_cast<int64_t>(SmallNumber(t.substr(i, j - i))) * unit;
    last_unit = unit;
    any = true;
    i = j + 1;
  }
  if (!any || total > kMaxDurationSeconds) {
    return std::nullopt;
  }
  return static_cast<int>(total);
}

// Seconds in one of a Spanish duration's units, or 0.
int SpanishDurationUnitSeconds(std::string_view unit) {
  if (OneOf(unit, {"h", "hora", "horas"})) {
    return 3600;
  }
  if (OneOf(unit, {"m", "min", "mins", "minuto", "minutos"})) {
    return 60;
  }
  if (OneOf(unit, {"s", "seg", "segs", "segundo", "segundos"})) {
    return 1;
  }
  return 0;
}

bool IsSpanishTimeUnit(std::string_view unit) {
  return SpanishDurationUnitSeconds(unit) != 0 ||
         OneOf(unit, {"d\xC3\xAD"
                      "a",
                      "d\xC3\xAD"
                      "as",
                      "dia", "dias", "semana", "semanas", "sem", "mes", "meses",
                      "a\xC3\xB1o", "a\xC3\xB1os"});
}

std::optional<int> ParseWordDuration(std::string_view t,
                                     FieldLanguage language) {
  // Number-unit pairs ("6 minutes, 7 seconds", "1 hr 2 min", "1h 2m"),
  // separated by spaces, commas or "and" (Spanish: "y"); each unit once,
  // larger first.
  const bool spanish = language == FieldLanguage::kSpanish;
  const std::string_view and_word = spanish ? "y" : "and";
  std::vector<std::string_view> tokens = Tokens(t, ",", 12);
  if (tokens.empty() || tokens.size() > 12) {
    return std::nullopt;
  }
  int64_t total = 0;
  int last_unit = 3601;
  bool any = false;
  size_t i = 0;
  while (i < tokens.size()) {
    if (tokens[i] == and_word && any) {
      ++i;
      continue;
    }
    std::string_view number = tokens[i];
    std::string_view unit;
    size_t digits = 0;
    while (digits < number.size() && IsAsciiDigit(number[digits])) {
      ++digits;
    }
    if (digits == 0 || digits > 6) {
      return std::nullopt;
    }
    if (digits < number.size()) {
      unit = number.substr(digits);  // "6min"
      number = number.substr(0, digits);
      ++i;
    } else {
      if (i + 1 >= tokens.size()) {
        return std::nullopt;
      }
      unit = tokens[i + 1];
      i += 2;
    }
    const int seconds =
        spanish ? SpanishDurationUnitSeconds(unit) : DurationUnitSeconds(unit);
    if (seconds == 0 || seconds >= last_unit) {
      return std::nullopt;
    }
    total += static_cast<int64_t>(SmallNumber(number)) * seconds;
    last_unit = seconds;
    any = true;
  }
  if (!any || total > kMaxDurationSeconds) {
    return std::nullopt;
  }
  return static_cast<int>(total);
}

constexpr std::array<std::string_view, 12> kMonths = {
    "january", "february", "march",     "april",   "may",      "june",
    "july",    "august",   "september", "october", "november", "december"};

bool IsMonthName(std::string_view token) {
  if (!token.empty() && token.back() == '.') {
    token.remove_suffix(1);
  }
  if (token.size() < 3) {
    return false;
  }
  if (token == "sept") {
    return true;
  }
  for (std::string_view month : kMonths) {
    if (token == month || (token.size() == 3 && month.substr(0, 3) == token)) {
      return true;
    }
  }
  return false;
}

// A day of the month: "5", "05", "5th", "21st".
bool IsDayNumber(std::string_view token) {
  size_t digits = 0;
  while (digits < token.size() && IsAsciiDigit(token[digits])) {
    ++digits;
  }
  if (digits == 0 || digits > 2) {
    return false;
  }
  const std::string_view rest = token.substr(digits);
  if (!rest.empty() && !OneOf(rest, {"st", "nd", "rd", "th"})) {
    return false;
  }
  const int day = SmallNumber(token.substr(0, digits));
  return day >= 1 && day <= 31;
}

bool IsYear(std::string_view token) {
  const int year = token.size() == 4 ? SmallNumber(token) : -1;
  return year >= 1000 && year <= 2999;
}

bool LooksLikeNumericDate(std::string_view t, char sep) {
  std::vector<std::string_view> parts = Tokens(t, std::string_view(&sep, 1), 3);
  if (parts.size() != 3 || std::count(t.begin(), t.end(), sep) != 2) {
    return false;
  }
  for (std::string_view part : parts) {
    if (!AllDigits(part) || part.size() > 4) {
      return false;
    }
  }
  if (sep == '-') {
    return IsYear(parts[0]) && parts[1].size() <= 2 && parts[2].size() <= 2;
  }
  return (IsYear(parts[0]) && parts[1].size() <= 2 && parts[2].size() <= 2) ||
         (IsYear(parts[2]) && parts[0].size() <= 2 && parts[1].size() <= 2);
}

// ---- Spanish fields -------------------------------------------------------

constexpr std::array<std::string_view, 12> kSpanishMonths = {
    "enero", "febrero", "marzo",      "abril",   "mayo",      "junio",
    "julio", "agosto",  "septiembre", "octubre", "noviembre", "diciembre"};

// "enero", "ene", "ene.", "sept", "setiembre".
bool IsSpanishMonthName(std::string_view token) {
  if (!token.empty() && token.back() == '.') {
    token.remove_suffix(1);
  }
  if (token.size() < 3) {
    return false;
  }
  if (token == "sept" || token == "setiembre") {
    return true;
  }
  for (std::string_view month : kSpanishMonths) {
    if (token == month || (token.size() == 3 && month.substr(0, 3) == token)) {
      return true;
    }
  }
  return false;
}

// A unit's word in Spanish: letters, accented ones included (any byte of a
// multi-byte character), at most 30 bytes.
bool IsSpanishUnitWord(std::string_view word) {
  return !word.empty() && word.size() <= 30 &&
         std::all_of(word.begin(), word.end(), [](char c) {
           return IsAsciiAlpha(c) || static_cast<unsigned char>(c) >= 0x80;
         });
}

// The next word of |rest| (after any spaces), and what follows it.
std::string_view NextWord(std::string_view rest, std::string_view* after) {
  rest = rest.substr(std::min(rest.size(), rest.find_first_not_of(' ')));
  const size_t end = std::min(rest.size(), rest.find(' '));
  *after = rest.substr(end);
  return rest.substr(0, end);
}

// A Spanish count (see ParseCount()). |t| is LowerField()'s.
std::optional<ParsedCount> ParseSpanishCount(std::string_view t) {
  ParsedCount count;
  std::string_view rest = t;
  if (rest.substr(0, 4) == "sin ") {
    rest.remove_prefix(3);  // "Sin visualizaciones": none, then the unit
  } else {
    // Digits, "." or "," before each group of three, and a decimal part
    // after a "," or "." that one or two digits follow (Spain writes
    // "1,2 M" and "1.234", Latin America "1.2 M" and "1,234").
    int64_t whole = 0;
    int digits = 0;
    int64_t fraction = 0;
    int64_t fraction_scale = 1;
    size_t i = 0;
    while (i < rest.size()) {
      const char c = rest[i];
      if (IsAsciiDigit(c)) {
        if (++digits > 15) {
          return std::nullopt;
        }
        whole = whole * 10 + (c - '0');
        ++i;
        continue;
      }
      if ((c != '.' && c != ',') || digits == 0) {
        break;
      }
      size_t j = i + 1;
      while (j < rest.size() && IsAsciiDigit(rest[j])) {
        ++j;
      }
      const size_t run = j - i - 1;
      if (run == 3) {
        ++i;  // a group separator: three digits follow
        continue;
      }
      if (run == 0) {
        break;  // the number ends ("12, ...")
      }
      if (run > 3) {
        return std::nullopt;
      }
      // A decimal part, of one or two digits.
      for (size_t k = i + 1; k < j; ++k) {
        fraction = fraction * 10 + (rest[k] - '0');
        fraction_scale *= 10;
      }
      i = j;
      break;
    }
    if (digits == 0) {
      return std::nullopt;
    }
    rest.remove_prefix(i);
    // A multiplier, attached ("12k", "1,2M") or as a word: "k", "mil",
    // "M", "mill.", "millones", "mil M", "mil millones".
    int64_t multiplier = 1;
    std::string_view after;
    const std::string_view word = NextWord(rest, &after);
    auto is_million = [](std::string_view w) {
      return OneOf(w, {"m", "mill", "mill.", "mill\xC3\xB3n", "millones"});
    };
    if (word == "k") {
      multiplier = 1000;
    } else if (is_million(word)) {
      multiplier = 1000000;
    } else if (word == "mil") {
      multiplier = 1000;
      std::string_view after_millions;
      if (is_million(NextWord(after, &after_millions))) {
        multiplier = 1000000000;
        after = after_millions;
      }
    }
    if (multiplier > 1) {
      rest = after;
      // "1,2 M de visualizaciones", "3 millones de vistas".
      std::string_view after_de;
      if (NextWord(rest, &after_de) == "de") {
        rest = after_de;
      }
    }
    constexpr int64_t kMaxCount = 1000000000000000;
    if (whole > kMaxCount / multiplier) {
      return std::nullopt;
    }
    count.value = whole * multiplier + fraction * multiplier / fraction_scale;
  }
  // The unit: one or two words, not a unit of time.
  if (rest.empty() || rest.front() != ' ') {
    return std::nullopt;
  }
  std::vector<std::string_view> words = Tokens(rest, "", 2);
  if (words.empty() || words.size() > 2) {
    return std::nullopt;
  }
  for (std::string_view word : words) {
    if (!IsSpanishUnitWord(word) || IsSpanishTimeUnit(word) ||
        IsTimeUnit(word) || word == "hace" || word == "de") {
      return std::nullopt;
    }
  }
  count.unit = std::string(words[0]);
  if (words.size() == 2) {
    count.unit += ' ';
    count.unit += words[1];
  }
  return count;
}

// A Spanish date (see LooksLikeDate()). |t| is LowerField()'s.
bool LooksLikeSpanishDate(std::string_view t) {
  if (OneOf(t, {"hoy", "ayer", "anteayer", "ahora", "justo ahora",
                "ahora mismo"})) {
    return true;
  }
  std::vector<std::string_view> tokens = Tokens(t, ",", 8);
  if (tokens.empty() || tokens.size() > 7) {
    return false;
  }
  // "Emitido hace 2 años", "Publicado el 5 de enero de 2024".
  if (OneOf(tokens[0], {"emitido", "transmitido", "publicado", "actualizado",
                        "subido", "estrenado", "editado"})) {
    tokens.erase(tokens.begin());
    if (!tokens.empty() && OneOf(tokens[0], {"el", "en"})) {
      tokens.erase(tokens.begin());
    }
  }
  if (tokens.empty()) {
    return false;
  }
  // "hace 3 días", "hace un año", "hace una hora".
  if (tokens.size() == 3 && tokens[0] == "hace" &&
      IsSpanishTimeUnit(tokens[2]) &&
      (AllDigits(tokens[1]) || tokens[1] == "un" || tokens[1] == "una")) {
    return true;
  }
  // "la semana pasada", "el mes pasado", "el año pasado".
  if (tokens.size() == 3 &&
      ((tokens[0] == "la" && tokens[1] == "semana" && tokens[2] == "pasada") ||
       (tokens[0] == "el" && OneOf(tokens[1], {"mes", "a\xC3\xB1o"}) &&
        tokens[2] == "pasado"))) {
    return true;
  }
  if (tokens.size() == 1) {
    if (LooksLikeNumericDate(tokens[0], '-') ||
        LooksLikeNumericDate(tokens[0], '/')) {
      return true;
    }
  }
  // Month names, "de" between: "5 ene 2024", "5 de enero de 2024", "enero
  // de 2024".
  int months = 0;
  int days = 0;
  int years = 0;
  int words = 0;
  for (std::string_view token : tokens) {
    if (token == "de") {
      continue;
    }
    ++words;
    if (IsSpanishMonthName(token)) {
      ++months;
    } else if (IsYear(token)) {
      ++years;
    } else if (IsDayNumber(token)) {
      ++days;
    } else {
      return false;
    }
  }
  return months == 1 && days <= 1 && years <= 1 && days + years >= 1 &&
         words <= 3;
}

}  // namespace

FieldLanguage FieldLanguageOf(std::string_view language_tag) {
  if (language_tag.size() >= 2 && ToLowerAscii(language_tag[0]) == 'e' &&
      ToLowerAscii(language_tag[1]) == 's' &&
      (language_tag.size() == 2 || language_tag[2] == '-' ||
       language_tag[2] == '_')) {
    return FieldLanguage::kSpanish;
  }
  return FieldLanguage::kEnglish;
}

size_t CountCodePoints(std::string_view utf8) {
  size_t n = 0;
  for (char c : utf8) {
    if (!IsContinuation(static_cast<unsigned char>(c))) {
      ++n;
    }
  }
  return n;
}

std::string SpeechSafeText(std::string_view text, size_t max_chars) {
  const std::string_view s = text.substr(0, kMaxSpeechScanBytes);
  std::string out;
  out.reserve(std::min(s.size(), max_chars * 4));
  size_t chars = 0;
  bool pending_space = false;
  bool cut = false;
  // Where the last space written is, in bytes and in code points.
  size_t last_space_byte = std::string::npos;
  size_t chars_at_last_space = 0;

  size_t i = 0;
  while (i < s.size()) {
    if (const size_t hidden = hidden_chars::HiddenCharAt(s, i)) {
      // U+2028 and U+2029 separate lines: a space, not nothing.
      if (hidden == 3 && static_cast<unsigned char>(s[i]) == 0xE2 &&
          static_cast<unsigned char>(s[i + 1]) == 0x80 &&
          (static_cast<unsigned char>(s[i + 2]) == 0xA8 ||
           static_cast<unsigned char>(s[i + 2]) == 0xA9)) {
        pending_space = true;
      }
      i += hidden;
      continue;
    }
    uint32_t cp = 0;
    const size_t n = DecodeUtf8(s, i, &cp);
    if (n == 0) {
      pending_space = true;
      ++i;
      continue;
    }
    const size_t start = i;
    i += n;
    if (IsSpaceLike(cp)) {
      pending_space = true;
      continue;
    }
    if (IsDropped(cp)) {
      continue;
    }
    const bool space = pending_space && !out.empty();
    if (chars + (space ? 1 : 0) + 1 > max_chars) {
      cut = true;
      break;
    }
    if (space) {
      last_space_byte = out.size();
      chars_at_last_space = chars;
      out.push_back(' ');
      ++chars;
    }
    pending_space = false;
    out.append(s.substr(start, n));
    ++chars;
  }
  if (cut && last_space_byte != std::string::npos &&
      chars - chars_at_last_space <= kWordBoundarySlack) {
    out.resize(last_space_byte);
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

std::optional<std::string> TypedText(std::string_view text,
                                     bool multiline,
                                     size_t max_bytes) {
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    if (out.size() > max_bytes) {
      return std::nullopt;
    }
    const unsigned char b = static_cast<unsigned char>(text[i]);
    if (b == '\r' || b == '\n') {
      out.push_back(multiline ? '\n' : ' ');
      i += (b == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ? 2 : 1;
      continue;
    }
    if (const size_t hidden = hidden_chars::HiddenCharAt(text, i)) {
      // U+2028 and U+2029 are line breaks.
      if (hidden == 3 && b == 0xE2 &&
          static_cast<unsigned char>(text[i + 1]) == 0x80 &&
          (static_cast<unsigned char>(text[i + 2]) == 0xA8 ||
           static_cast<unsigned char>(text[i + 2]) == 0xA9)) {
        out.push_back(multiline ? '\n' : ' ');
      }
      i += hidden;
      continue;
    }
    uint32_t cp = 0;
    const size_t n = DecodeUtf8(text, i, &cp);
    if (n == 0) {
      ++i;
      continue;
    }
    if (cp == '\t') {
      out.push_back(' ');
    } else if (!(cp < 0x20 || cp == 0x7F)) {
      out.append(text.substr(i, n));
    }
    i += n;
  }
  if (out.size() > max_bytes) {
    return std::nullopt;
  }
  return out;
}

std::optional<int> ParseDuration(std::string_view text,
                                 FieldLanguage language) {
  const std::string t = LowerField(text);
  if (t.empty() || text.size() > kMaxFieldScanBytes) {
    return std::nullopt;
  }
  if (t.find(':') != std::string::npos) {
    return ParseClockDuration(t);
  }
  if (t.size() >= 2 && t[0] == 'p' && t[1] == 't') {
    return ParseIsoDuration(t);
  }
  return ParseWordDuration(t, language);
}

std::optional<ParsedCount> ParseCount(std::string_view text,
                                      FieldLanguage language) {
  const std::string t = LowerField(text);
  if (t.empty() || text.size() > kMaxFieldScanBytes) {
    return std::nullopt;
  }
  if (language == FieldLanguage::kSpanish) {
    return ParseSpanishCount(t);
  }
  ParsedCount count;
  std::string_view rest = t;
  if (rest.substr(0, 3) == "no ") {
    rest.remove_prefix(2);  // "No views": zero, then the unit
  } else {
    // Digits with "," groups of three, and an optional decimal part.
    int64_t whole = 0;
    int digits = 0;
    size_t i = 0;
    while (i < rest.size()) {
      if (IsAsciiDigit(rest[i])) {
        if (++digits > 15) {
          return std::nullopt;
        }
        whole = whole * 10 + (rest[i] - '0');
        ++i;
      } else if (rest[i] == ',' && digits > 0 && i + 3 < rest.size() &&
                 AllDigits(rest.substr(i + 1, 3)) &&
                 (i + 4 == rest.size() || !IsAsciiDigit(rest[i + 4]))) {
        ++i;  // a group separator: three digits follow
      } else {
        break;
      }
    }
    if (digits == 0) {
      return std::nullopt;
    }
    int64_t fraction = 0;
    int64_t fraction_scale = 1;
    if (i < rest.size() && rest[i] == '.' && i + 1 < rest.size() &&
        IsAsciiDigit(rest[i + 1])) {
      ++i;
      while (i < rest.size() && IsAsciiDigit(rest[i])) {
        if (fraction_scale < 1000) {
          fraction = fraction * 10 + (rest[i] - '0');
          fraction_scale *= 10;
        }
        ++i;
      }
    }
    rest.remove_prefix(i);
    int64_t multiplier = 1;
    auto take_multiplier = [&](std::string_view word, int64_t value) {
      if (rest.substr(0, word.size()) == word &&
          (rest.size() == word.size() || rest[word.size()] == ' ')) {
        rest.remove_prefix(word.size());
        multiplier = value;
        return true;
      }
      return false;
    };
    // Attached ("2.1b") or as a word ("2.1 billion").
    if (!take_multiplier("k", 1000) && !take_multiplier("m", 1000000) &&
        !take_multiplier("b", 1000000000)) {
      std::string_view after = TrimSpaces(rest);
      std::string_view saved = rest;
      rest = after;
      if (!take_multiplier("thousand", 1000) &&
          !take_multiplier("million", 1000000) &&
          !take_multiplier("billion", 1000000000)) {
        rest = saved;
      }
    }
    // At most 10^15 (no overflow: the fraction adds less than 10^12).
    constexpr int64_t kMaxCount = 1000000000000000;
    if (whole > kMaxCount / multiplier) {
      return std::nullopt;
    }
    count.value = whole * multiplier + fraction * multiplier / fraction_scale;
  }
  // The unit: one or two words of letters, not a unit of time.
  if (rest.empty() || rest.front() != ' ') {
    return std::nullopt;
  }
  std::vector<std::string_view> words = Tokens(rest, "", 2);
  if (words.empty() || words.size() > 2) {
    return std::nullopt;
  }
  for (std::string_view word : words) {
    if (word.size() > 20 ||
        !std::all_of(word.begin(), word.end(), IsAsciiAlpha) ||
        IsTimeUnit(word) || word == "ago") {
      return std::nullopt;
    }
  }
  count.unit = std::string(words[0]);
  if (words.size() == 2) {
    count.unit += ' ';
    count.unit += words[1];
  }
  return count;
}

bool LooksLikeDate(std::string_view text, FieldLanguage language) {
  const std::string lower = LowerField(text);
  if (lower.empty() || text.size() > kMaxFieldScanBytes) {
    return false;
  }
  if (language == FieldLanguage::kSpanish) {
    return LooksLikeSpanishDate(lower);
  }
  std::string_view t = lower;
  if (OneOf(t, {"today", "yesterday", "just now"})) {
    return true;
  }
  std::vector<std::string_view> tokens = Tokens(t, ",", 6);
  if (tokens.empty() || tokens.size() > 5) {
    return false;
  }
  if (OneOf(tokens[0], {"streamed", "premiered", "updated", "posted",
                        "uploaded", "published", "edited", "live"})) {
    tokens.erase(tokens.begin());
    if (!tokens.empty() && tokens[0] == "on") {
      tokens.erase(tokens.begin());
    }
  }
  if (tokens.empty()) {
    return false;
  }
  // "3 days ago", "an hour ago".
  if (tokens.size() == 3 && tokens[2] == "ago" && IsTimeUnit(tokens[1]) &&
      (AllDigits(tokens[0]) || tokens[0] == "a" || tokens[0] == "an")) {
    return true;
  }
  if (tokens.size() == 2 && tokens[0] == "last" &&
      OneOf(tokens[1], {"week", "month", "year"})) {
    return true;
  }
  if (tokens.size() == 1) {
    if (LooksLikeNumericDate(tokens[0], '-') ||
        LooksLikeNumericDate(tokens[0], '/')) {
      return true;
    }
  }
  // Month names: "jan 5 2024", "5 january 2024", "march 2024", "jan 5".
  int months = 0;
  int days = 0;
  int years = 0;
  for (std::string_view token : tokens) {
    if (IsMonthName(token)) {
      ++months;
    } else if (IsYear(token)) {
      ++years;
    } else if (IsDayNumber(token)) {
      ++days;
    } else {
      return false;
    }
  }
  return months == 1 && days <= 1 && years <= 1 && days + years >= 1 &&
         tokens.size() <= 3;
}

bool LooksLikeAddress(std::string_view text) {
  const std::string t = LowerField(text);
  if (t.empty()) {
    return false;
  }
  if (t.rfind("http://", 0) == 0 || t.rfind("https://", 0) == 0 ||
      t.rfind("www.", 0) == 0) {
    return true;
  }
  // A breadcrumb: "en.wikipedia.org › wiki".
  if (t.find("\xE2\x80\xBA") != std::string::npos) {
    return true;
  }
  // A bare host name: labels of letters, digits and '-', the last one
  // letters only, no space.
  if (t.find(' ') != std::string::npos) {
    return false;
  }
  std::vector<std::string_view> labels = Tokens(t, ".", 16);
  if (labels.size() < 2 || labels.size() > 16 ||
      std::count(t.begin(), t.end(), '.') !=
          static_cast<std::ptrdiff_t>(labels.size() - 1) ||
      t.front() == '.' || t.back() == '.') {
    return false;
  }
  for (std::string_view label : labels) {
    for (char c : label) {
      if (!IsAsciiAlpha(c) && !IsAsciiDigit(c) && c != '-') {
        return false;
      }
    }
  }
  const std::string_view tld = labels.back();
  return tld.size() >= 2 && std::all_of(tld.begin(), tld.end(), IsAsciiAlpha);
}

bool HasNoWords(std::string_view text) {
  const std::string_view s = text.substr(0, kMaxSpeechScanBytes);
  size_t i = 0;
  while (i < s.size()) {
    uint32_t cp = 0;
    const size_t n = DecodeUtf8(s, i, &cp);
    if (n == 0) {
      ++i;
      continue;
    }
    i += n;
    if (cp < 0x80) {
      const char c = static_cast<char>(cp);
      if (IsAsciiAlpha(c) || IsAsciiDigit(c)) {
        return false;
      }
      continue;
    }
    // Common separators and symbols; any other character may be a letter.
    if (!(cp == 0xA0 || cp == 0xAB || cp == 0xB7 || cp == 0xBB ||
          (cp >= 0x2010 && cp <= 0x2027) || (cp >= 0x2030 && cp <= 0x205E) ||
          (cp >= 0x2190 && cp <= 0x21FF) || (cp >= 0x25A0 && cp <= 0x25FF))) {
      return false;
    }
  }
  return true;
}

std::string FoldForCompare(std::string_view text) {
  std::string out;
  const std::string_view s = text.substr(0, kMaxSpeechScanBytes);
  out.reserve(s.size());
  bool space = false;
  for (char c : s) {
    if (c == ' ') {
      space = !out.empty();
      continue;
    }
    if (space) {
      out.push_back(' ');
      space = false;
    }
    out.push_back(ToLowerAscii(c));
  }
  return out;
}

}  // namespace voice
}  // namespace molt_ai
