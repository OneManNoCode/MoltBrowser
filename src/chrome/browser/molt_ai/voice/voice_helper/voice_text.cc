// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_helper/voice_text.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace molt_voice {

namespace {

// Private bytes FoldToAscii() writes for signs that are read after a number
// ("5 euros"); the real control characters became spaces before.
constexpr char kEuro = 0x01;
constexpr char kPound = 0x02;
constexpr char kYen = 0x03;
constexpr char kRupee = 0x04;
constexpr char kCent = 0x05;
constexpr char kDegree = 0x06;

bool IsAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}
bool IsAlnum(char c) {
  return IsAlpha(c) || IsDigit(c);
}
bool IsUpper(char c) {
  return c >= 'A' && c <= 'Z';
}
char Lower(char c) {
  return IsUpper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}
std::string LowerAscii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = Lower(c);
  }
  return out;
}
bool IsCurrency(char c) {
  return c == '$' || c == kEuro || c == kPound || c == kYen || c == kRupee;
}

// ---- Unicode to ASCII ------------------------------------------------------

// Decodes one code point at |*i|; advances past it. Invalid sequences give
// 0xFFFD and advance by one byte.
uint32_t DecodeOne(std::string_view s, size_t* i) {
  const auto byte = [&s](size_t at) { return static_cast<uint8_t>(s[at]); };
  const uint8_t b0 = byte(*i);
  if (b0 < 0x80) {
    ++*i;
    return b0;
  }
  int length = 0;
  uint32_t cp = 0;
  uint32_t min = 0;
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    length = 2;
    cp = b0 & 0x1F;
    min = 0x80;
  } else if (b0 >= 0xE0 && b0 <= 0xEF) {
    length = 3;
    cp = b0 & 0x0F;
    min = 0x800;
  } else if (b0 >= 0xF0 && b0 <= 0xF4) {
    length = 4;
    cp = b0 & 0x07;
    min = 0x10000;
  } else {
    ++*i;
    return 0xFFFD;
  }
  if (*i + static_cast<size_t>(length) > s.size()) {
    ++*i;
    return 0xFFFD;
  }
  for (int k = 1; k < length; ++k) {
    const uint8_t b = byte(*i + static_cast<size_t>(k));
    if ((b & 0xC0) != 0x80) {
      ++*i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    ++*i;
    return 0xFFFD;
  }
  *i += static_cast<size_t>(length);
  return cp;
}

// Latin-1 letters U+00C0..U+00FF as ASCII ("" for the two signs).
constexpr const char* kLatin1Letters[64] = {
    "A", "A", "A",  "A", "A", "A", "AE", "C",  "E", "E", "E", "E",
    "I", "I", "I",  "I", "D", "N", "O",  "O",  "O", "O", "O", "",  // U+00D7 x
    "O", "U", "U",  "U", "U", "Y", "TH", "ss", "a", "a", "a", "a",
    "a", "a", "ae", "c", "e", "e", "e",  "e",  "i", "i", "i", "i",
    "d", "n", "o",  "o", "o", "o", "o",  "",  // U+00F7
    "o", "u", "u",  "u", "u", "y", "th", "y"};

// Latin Extended-A, U+0100..U+017F, one base letter each (IJ, OE: the
// first letter only).
constexpr char kLatinExtendedA[] =
    "AaAaAa"        // 0100 A with macron, breve, ogonek
    "CcCcCcCc"      // 0106
    "DdDd"          // 010E
    "EeEeEeEeEe"    // 0112
    "GgGgGgGg"      // 011C
    "HhHh"          // 0124
    "IiIiIiIiIi"    // 0128
    "Ii"            // 0132 IJ
    "Jj"            // 0134
    "Kkk"           // 0136
    "LlLlLlLlLl"    // 0139
    "NnNnNnnNn"     // 0143
    "OoOoOo"        // 014C
    "Oo"            // 0152 OE
    "RrRrRr"        // 0154
    "SsSsSsSs"      // 015A
    "TtTtTt"        // 0162
    "UuUuUuUuUuUu"  // 0168
    "Ww"            // 0174
    "YyY"           // 0176
    "ZzZzZz"        // 0179
    "s";            // 017F long s
static_assert(sizeof(kLatinExtendedA) == 0x80 + 1,
              "one letter per code point of U+0100..U+017F");

// Characters that vanish without separating anything: format characters
// (zero-width, bidirectional controls, the BOM, tags, variation selectors),
// the soft hyphen and combining marks.
bool Vanishes(uint32_t cp) {
  return cp == 0xAD || (cp >= 0x0300 && cp <= 0x036F) ||
         (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
         (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
         (cp >= 0x2060 && cp <= 0x206F) || (cp >= 0x20D0 && cp <= 0x20FF) ||
         (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
         cp == 0xFEFF || (cp >= 0xFFF9 && cp <= 0xFFFB) ||
         (cp >= 0xE0000 && cp <= 0xE007F);
}

// What a non-ASCII code point becomes; " " separates words.
std::string_view FoldOne(uint32_t cp) {
  if (cp >= 0xC0 && cp <= 0xFF) {
    std::string_view letters = kLatin1Letters[cp - 0xC0];
    if (!letters.empty()) {
      return letters;
    }
    return cp == 0xD7 ? std::string_view(" times ")
                      : std::string_view(" divided by ");
  }
  if (cp >= 0x100 && cp <= 0x17F) {
    return std::string_view(&kLatinExtendedA[cp - 0x100], 1);
  }
  if (cp >= 0xFF01 && cp <= 0xFF5E) {
    // Fullwidth ASCII: handled by the caller (it isn't a fixed string).
    return std::string_view();
  }
  switch (cp) {
    case 0xA2:
      return std::string_view(&kCent, 1);
    case 0xA3:
    case 0x20A4:
      return std::string_view(&kPound, 1);
    case 0xA5:
      return std::string_view(&kYen, 1);
    case 0x20AC:
      return std::string_view(&kEuro, 1);
    case 0x20B9:
      return std::string_view(&kRupee, 1);
    case 0xB0:
      return std::string_view(&kDegree, 1);
    case 0xA7:
      return " section ";
    case 0xB7:
    case 0x2022:
    case 0x2023:
    case 0x2043:
    case 0x25CF:
    case 0x25AA:
      return " , ";
    case 0xBC:
      return " one quarter ";
    case 0xBD:
      return " one half ";
    case 0xBE:
      return " three quarters ";
    case 0xAB:
    case 0xBB:
    case 0x201C:
    case 0x201D:
    case 0x201E:
    case 0x201F:
    case 0x2033:
    case 0x2039:
    case 0x203A:
      return "\"";
    case 0x2018:
    case 0x2019:
    case 0x201A:
    case 0x201B:
    case 0x2032:
      return "'";
    case 0x2010:
    case 0x2011:
    case 0x2012:
    case 0x2013:
    case 0x2014:
    case 0x2015:
    case 0x2212:
      return "-";
    case 0x2024:
      return ".";
    case 0x2025:
    case 0x2026:
      return "...";
    case 0x203C:
      return "!";
    case 0x2047:
    case 0x2048:
    case 0x2049:
      return "?";
    case 0x2044:
    case 0x2215:
      return "/";
    case 0x2116:
      return " number ";
    default:
      return " ";
  }
}

}  // namespace

std::string FoldToAscii(std::string_view text) {
  if (text.size() > kMaxSpeakTextBytes) {
    text = text.substr(0, kMaxSpeakTextBytes);
  }
  std::string out;
  out.reserve(text.size() + 16);
  size_t i = 0;
  while (i < text.size()) {
    const uint32_t cp = DecodeOne(text, &i);
    if (cp >= 0x20 && cp < 0x7F) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0xA0) {
      out.push_back(' ');  // controls, DEL, C1 controls
    } else if (Vanishes(cp)) {
      continue;
    } else if (cp >= 0xFF01 && cp <= 0xFF5E) {
      out.push_back(static_cast<char>(cp - 0xFEE0));
    } else {
      out.append(FoldOne(cp));
    }
  }
  return out;
}

// ---- Numbers as words
// --------------------------------------------------------

namespace {

constexpr const char* kOnes[20] = {
    "zero",    "one",     "two",       "three",    "four",
    "five",    "six",     "seven",     "eight",    "nine",
    "ten",     "eleven",  "twelve",    "thirteen", "fourteen",
    "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
constexpr const char* kTens[10] = {"",       "",      "twenty", "thirty",
                                   "forty",  "fifty", "sixty",  "seventy",
                                   "eighty", "ninety"};
constexpr const char* kScales[5] = {"", "thousand", "million", "billion",
                                    "trillion"};
constexpr const char* kMonths[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};

void Append(std::string* out, std::string_view word) {
  if (word.empty()) {
    return;
  }
  if (!out->empty()) {
    out->push_back(' ');
  }
  out->append(word);
}

// 1..999.
void Hundreds(uint32_t n, std::string* out) {
  if (n >= 100) {
    Append(out, kOnes[n / 100]);
    Append(out, "hundred");
    n %= 100;
  }
  if (n >= 20) {
    Append(out, kTens[n / 10]);
    n %= 10;
    if (n) {
      Append(out, kOnes[n]);
    }
  } else if (n > 0) {
    Append(out, kOnes[n]);
  }
}

std::string DigitWords(std::string_view digits) {
  std::string out;
  for (char c : digits.substr(0, kMaxDigitsRead)) {
    if (IsDigit(c)) {
      Append(&out, kOnes[c - '0']);
    }
  }
  return out;
}

// The last word of a cardinal as an ordinal.
std::string OrdinalOfWord(std::string_view word) {
  static constexpr std::pair<const char*, const char*> kIrregular[] = {
      {"one", "first"},     {"two", "second"},   {"three", "third"},
      {"five", "fifth"},    {"eight", "eighth"}, {"nine", "ninth"},
      {"twelve", "twelfth"}};
  for (const auto& [cardinal, ordinal] : kIrregular) {
    if (word == cardinal) {
      return ordinal;
    }
  }
  if (!word.empty() && word.back() == 'y') {
    return std::string(word.substr(0, word.size() - 1)) + "ieth";
  }
  return std::string(word) + "th";
}

}  // namespace

std::string CardinalWords(uint64_t n) {
  if (n == 0) {
    return "zero";
  }
  if (n >= 1000000000000000ULL) {
    return DigitWords(std::to_string(n));
  }
  uint32_t groups[5] = {};
  for (int g = 0; g < 5; ++g) {
    groups[g] = static_cast<uint32_t>(n % 1000);
    n /= 1000;
  }
  std::string out;
  for (int g = 4; g >= 0; --g) {
    if (!groups[g]) {
      continue;
    }
    Hundreds(groups[g], &out);
    Append(&out, kScales[g]);
  }
  return out;
}

std::string OrdinalWords(uint64_t n) {
  std::string words = CardinalWords(n);
  const size_t space = words.rfind(' ');
  const size_t start = space == std::string::npos ? 0 : space + 1;
  return words.substr(0, start) + OrdinalOfWord(words.substr(start));
}

std::string YearWords(int year) {
  if (year <= 0 || year > 9999) {
    return CardinalWords(year < 0 ? 0 : static_cast<uint64_t>(year));
  }
  const int high = year / 100;
  const int low = year % 100;
  // 2000..2009, and 1000, 3000...: "two thousand five".
  if ((year >= 2000 && year <= 2009) || (low == 0 && high % 10 == 0)) {
    return CardinalWords(static_cast<uint64_t>(year));
  }
  if (year < 1000) {
    return CardinalWords(static_cast<uint64_t>(year));
  }
  std::string out = CardinalWords(static_cast<uint64_t>(high));
  if (low == 0) {
    Append(&out, "hundred");
  } else if (low < 10) {
    Append(&out, "oh");
    Append(&out, kOnes[low]);
  } else {
    Append(&out, CardinalWords(static_cast<uint64_t>(low)));
  }
  return out;
}

SpokenSentence::SpokenSentence() = default;
SpokenSentence::SpokenSentence(const SpokenSentence&) = default;
SpokenSentence& SpokenSentence::operator=(const SpokenSentence&) = default;
SpokenSentence::SpokenSentence(SpokenSentence&&) = default;
SpokenSentence& SpokenSentence::operator=(SpokenSentence&&) = default;
SpokenSentence::~SpokenSentence() = default;

namespace {

// ---- The sentences being built ----------------------------------------------

class Builder {
 public:
  bool full() const { return count_ >= kMaxSpokenWords; }

  void Word(std::string_view word) {
    if (word.empty() || full()) {
      return;
    }
    Add(WordKind::kWord, std::string(word.substr(0, kMaxWordLetters)));
  }

  // Several words, separated by single spaces.
  void Words(std::string_view words) {
    size_t start = 0;
    while (start < words.size()) {
      size_t end = words.find(' ', start);
      if (end == std::string_view::npos) {
        end = words.size();
      }
      Word(words.substr(start, end - start));
      start = end + 1;
    }
  }

  void Spell(std::string_view letters) {
    std::string lower;
    for (char c : letters) {
      if (IsAlpha(c) && lower.size() < kMaxWordLetters) {
        lower.push_back(Lower(c));
      }
    }
    if (lower.empty() || full()) {
      return;
    }
    Add(WordKind::kSpell, std::move(lower));
  }

  void Pause() {
    if (current_.words.empty() || full() ||
        current_.words.back().kind == WordKind::kPause) {
      return;
    }
    Add(WordKind::kPause, std::string());
  }

  void End(char mark) {
    if (!current_.words.empty() &&
        current_.words.back().kind == WordKind::kPause) {
      current_.words.pop_back();
      --count_;
    }
    if (current_.words.empty()) {
      return;
    }
    current_.end = mark;
    sentences_.push_back(std::move(current_));
    current_ = SpokenSentence();
  }

  std::vector<SpokenSentence> Take() {
    End('.');
    return std::move(sentences_);
  }

 private:
  void Add(WordKind kind, std::string text) {
    SpokenWord word;
    word.kind = kind;
    word.text = std::move(text);
    current_.words.push_back(std::move(word));
    ++count_;
  }

  std::vector<SpokenSentence> sentences_;
  SpokenSentence current_;
  size_t count_ = 0;
};

// ---- Tables
// ------------------------------------------------------------------

struct Expansion {
  const char* written;  // lower case
  const char* said;
};

// Abbreviations read with their period ("Mr." "mister"). The period then
// doesn't end the sentence.
constexpr Expansion kWithPeriod[] = {
    {"mr", "mister"},
    {"mrs", "missus"},
    {"ms", "miz"},
    {"dr", "doctor"},
    {"st", "saint"},
    {"jr", "junior"},
    {"sr", "senior"},
    {"vs", "versus"},
    {"etc", "et cetera"},
    {"approx", "approximately"},
    {"dept", "department"},
    {"inc", "incorporated"},
    {"ltd", "limited"},
    {"corp", "corporation"},
    {"jan", "January"},
    {"feb", "February"},
    {"apr", "April"},
    {"jun", "June"},
    {"jul", "July"},
    {"aug", "August"},
    {"sep", "September"},
    {"sept", "September"},
    {"oct", "October"},
    {"nov", "November"},
    {"dec", "December"},
    {"vol", "volume"},
    {"fig", "figure"},
    {"govt", "government"},
    {"intl", "international"},
    {"univ", "university"},
    {"ave", "avenue"},
    {"blvd", "boulevard"},
    {"mt", "mount"},
    {"hrs", "hours"},
    {"mins", "minutes"},
    {"secs", "seconds"},
};

// Words read differently without a period too.
constexpr Expansion kAlways[] = {
    {"vs", "versus"},
    {"etc", "et cetera"},
    {"approx", "approximately"},
    {"govt", "government"},
    {"intl", "international"},
    {"hrs", "hours"},
    {"mins", "minutes"},
    {"secs", "seconds"},
    {"lbs", "pounds"},
    {"btw", "by the way"},
    {"faq", "f a q"},
};

// Dotted initialisms read as words; others are spelled.
constexpr Expansion kInitialisms[] = {
    {"eg", "for example"},
    {"ie", "that is"},
    {"aka", "also known as"},
};

// Units read after a number: written (case-sensitive), singular, plural.
struct Unit {
  const char* written;
  const char* one;
  const char* many;
};
constexpr Unit kUnits[] = {
    {"GB", "gigabyte", "gigabytes"},
    {"MB", "megabyte", "megabytes"},
    {"KB", "kilobyte", "kilobytes"},
    {"TB", "terabyte", "terabytes"},
    {"kB", "kilobyte", "kilobytes"},
    {"GHz", "gigahertz", "gigahertz"},
    {"MHz", "megahertz", "megahertz"},
    {"kHz", "kilohertz", "kilohertz"},
    {"Hz", "hertz", "hertz"},
    {"km", "kilometer", "kilometers"},
    {"cm", "centimeter", "centimeters"},
    {"mm", "millimeter", "millimeters"},
    {"kg", "kilogram", "kilograms"},
    {"mg", "milligram", "milligrams"},
    {"lb", "pound", "pounds"},
    {"lbs", "pound", "pounds"},
    {"oz", "ounce", "ounces"},
    {"mph", "mile per hour", "miles per hour"},
    {"ms", "millisecond", "milliseconds"},
    {"fps", "frame per second", "frames per second"},
    {"kWh", "kilowatt hour", "kilowatt hours"},
    {"W", "watt", "watts"},
    {"kW", "kilowatt", "kilowatts"},
    {"mAh", "milliamp hour", "milliamp hours"},
};

// Top-level domains that make "name.tld" (without a scheme) an address.
constexpr const char* kTlds[] = {
    "com",  "org", "net", "edu", "gov",    "mil",  "int",  "io",    "ai",
    "app",  "dev", "co",  "uk",  "us",     "de",   "fr",   "jp",    "cn",
    "ru",   "in",  "br",  "au",  "ca",     "es",   "it",   "nl",    "se",
    "no",   "fi",  "dk",  "pl",  "ch",     "at",   "be",   "eu",    "info",
    "biz",  "me",  "tv",  "xyz", "online", "site", "tech", "store", "blog",
    "news", "gg",  "ly",  "fm",  "to",     "ie",   "nz",   "mx",    "kr"};

const char* Find(const Expansion* table, size_t size, std::string_view lower) {
  for (size_t i = 0; i < size; ++i) {
    if (lower == table[i].written) {
      return table[i].said;
    }
  }
  return nullptr;
}

template <size_t N>
const char* Find(const Expansion (&table)[N], std::string_view lower) {
  return Find(table, N, lower);
}

const Unit* FindUnit(std::string_view written) {
  for (const Unit& unit : kUnits) {
    if (written == unit.written) {
      return &unit;
    }
  }
  return nullptr;
}

bool IsTld(std::string_view lower) {
  for (const char* tld : kTlds) {
    if (lower == tld) {
      return true;
    }
  }
  return false;
}

std::string_view CurrencyName(char sign, bool one) {
  switch (sign) {
    case '$':
      return one ? "dollar" : "dollars";
    case kEuro:
      return one ? "euro" : "euros";
    case kPound:
      return one ? "pound" : "pounds";
    case kYen:
      return "yen";
    case kRupee:
      return one ? "rupee" : "rupees";
  }
  return std::string_view();
}

std::string_view SubunitName(char sign, bool one) {
  switch (sign) {
    case '$':
    case kEuro:
      return one ? "cent" : "cents";
    case kPound:
      return one ? "penny" : "pence";
    case kRupee:
      return one ? "paisa" : "paise";
  }
  return std::string_view();
}

// ---- The reader
// ----------------------------------------------------------------

// Characters that may open or close a chunk around its body.
bool IsOpening(char c) {
  return c == '(' || c == '[' || c == '{' || c == '"' || c == '\'' ||
         c == '`' || c == '<' || c == '*' || c == '_';
}
bool IsClosing(char c) {
  return c == ')' || c == ']' || c == '}' || c == '"' || c == '\'' ||
         c == '`' || c == '>' || c == ',' || c == ';' || c == ':' || c == '.' ||
         c == '!' || c == '?' || c == '*' || c == '_';
}

size_t DigitsAt(std::string_view s, size_t at) {
  size_t end = at;
  while (end < s.size() && IsDigit(s[end])) {
    ++end;
  }
  return end - at;
}

uint64_t ParseDigits(std::string_view digits) {
  uint64_t value = 0;
  for (char c : digits) {
    if (IsDigit(c)) {
      value = value * 10 + static_cast<uint64_t>(c - '0');
    }
  }
  return value;
}

class Reader {
 public:
  explicit Reader(std::string_view ascii) : s_(ascii) {}

  std::vector<SpokenSentence> Read() {
    size_t i = 0;
    while (i < s_.size() && !out_.full()) {
      if (s_[i] == ' ') {
        ++i;
        continue;
      }
      size_t end = i;
      while (end < s_.size() && s_[end] != ' ') {
        ++end;
      }
      size_t next = end;
      while (next < s_.size() && s_[next] == ' ') {
        ++next;
      }
      next_starts_with_digit_ = next < s_.size() && IsDigit(s_[next]);
      Chunk(s_.substr(i, end - i));
      i = end;
    }
    return out_.Take();
  }

 private:
  // One run of non-spaces: its opening punctuation, its body and its
  // closing punctuation.
  void Chunk(std::string_view chunk) {
    size_t a = 0;
    size_t b = chunk.size();
    while (a < b && IsOpening(chunk[a])) {
      if (chunk[a] == '(' || chunk[a] == '[' || chunk[a] == '{') {
        out_.Pause();
      }
      ++a;
    }
    size_t e = b;
    while (e > a && IsClosing(chunk[e - 1])) {
      --e;
    }
    std::string_view body = chunk.substr(a, e - a);
    std::string_view tail = chunk.substr(e);
    // A period that belongs to the body: "Mr.", "e.g.", "U.S.", "No. 5".
    if (!tail.empty() && tail[0] == '.' && !body.empty() &&
        AbbreviationTakesPeriod(body, tail)) {
      body = chunk.substr(a, e - a + 1);
      tail = tail.substr(1);
    }
    Body(body);
    Tail(tail);
  }

  bool AbbreviationTakesPeriod(std::string_view body, std::string_view tail) {
    if (tail.size() >= 2 && tail[1] == '.') {
      return false;  // an ellipsis
    }
    const std::string lower = LowerAscii(body);
    if (Find(kWithPeriod, lower)) {
      return true;
    }
    if (lower == "no" && next_starts_with_digit_) {
      return true;
    }
    // "e.g", "U.S", "a.m": single letters between periods.
    if (body.size() >= 3 && body.size() <= 15) {
      for (size_t k = 0; k < body.size(); ++k) {
        const bool letter_place = k % 2 == 0;
        if (letter_place ? !IsAlpha(body[k]) : body[k] != '.') {
          return false;
        }
      }
      return body.size() % 2 == 1;
    }
    return false;
  }

  void Tail(std::string_view tail) {
    size_t k = 0;
    while (k < tail.size()) {
      const char c = tail[k];
      if (c == '.' && k + 2 < tail.size() && tail[k + 1] == '.' &&
          tail[k + 2] == '.') {
        out_.Pause();
        k += 3;
        while (k < tail.size() && tail[k] == '.') {
          ++k;
        }
        continue;
      }
      if (c == '.' || c == '!' || c == '?') {
        out_.End(c);
      } else if (c == ',' || c == ';' || c == ':' || c == ')' || c == ']' ||
                 c == '}') {
        out_.Pause();
      }
      ++k;
    }
  }

  void Body(std::string_view body) {
    if (body.empty()) {
      return;
    }
    const size_t at = body.find('@');
    if (at != std::string_view::npos && at > 0 &&
        IsHost(body.substr(at + 1), /*need_tld=*/true)) {
      Email(body.substr(0, at), body.substr(at + 1));
      return;
    }
    std::string_view rest = body;
    if (StripScheme(&rest)) {
      Address(rest);
      return;
    }
    const size_t host_end = rest.find_first_of("/?#");
    const std::string_view host = rest.substr(0, host_end);
    if (IsHost(host, /*need_tld=*/true)) {
      Address(rest);
      return;
    }
    Pieces(body);
  }

  // Removes "https://", "www." and the like; true if there was one.
  static bool StripScheme(std::string_view* s) {
    static constexpr const char* kSchemes[] = {"https://", "http://", "ftp://",
                                               "www."};
    bool stripped = false;
    for (const char* scheme : kSchemes) {
      const std::string_view prefix(scheme);
      if (s->size() > prefix.size() &&
          LowerAscii(s->substr(0, prefix.size())) == prefix) {
        s->remove_prefix(prefix.size());
        stripped = true;
      }
    }
    return stripped;
  }

  // "example.com", "mail.example.co.uk" (with a known last label when
  // |need_tld|).
  static bool IsHost(std::string_view host, bool need_tld) {
    if (host.empty() || host.size() > 253) {
      return false;
    }
    size_t labels = 0;
    size_t start = 0;
    std::string_view last;
    while (start <= host.size()) {
      size_t end = host.find('.', start);
      if (end == std::string_view::npos) {
        end = host.size();
      }
      const std::string_view label = host.substr(start, end - start);
      if (label.empty() || label.size() > 63 || label.front() == '-' ||
          label.back() == '-') {
        return false;
      }
      for (char c : label) {
        if (!IsAlnum(c) && c != '-') {
          return false;
        }
      }
      ++labels;
      last = label;
      start = end + 1;
      if (end == host.size()) {
        break;
      }
    }
    if (labels < 2) {
      return false;
    }
    for (char c : last) {
      if (!IsAlpha(c)) {
        return false;
      }
    }
    return !need_tld || IsTld(LowerAscii(last));
  }

  // A host and whatever follows it (path, query): the host only.
  void Address(std::string_view rest) {
    const size_t end = rest.find_first_of("/?#:");
    Host(rest.substr(0, end));
  }

  void Host(std::string_view host) {
    size_t start = 0;
    bool first = true;
    while (start < host.size() && !out_.full()) {
      size_t end = host.find('.', start);
      if (end == std::string_view::npos) {
        end = host.size();
      }
      const std::string_view label = host.substr(start, end - start);
      if (!first) {
        out_.Word("dot");
      }
      first = false;
      const bool last = end == host.size();
      bool letters = !label.empty();
      for (char c : label) {
        letters = letters && IsAlpha(c);
      }
      if (last && letters && label.size() <= 2) {
        out_.Spell(label);  // "uk", "io"
      } else {
        Label(label);
      }
      start = end + 1;
    }
  }

  // A part of an address: letters as words, digits one by one, "-" said.
  void Label(std::string_view label) {
    size_t k = 0;
    while (k < label.size()) {
      if (IsAlpha(label[k])) {
        size_t end = k;
        while (end < label.size() && IsAlpha(label[end])) {
          ++end;
        }
        out_.Word(label.substr(k, end - k));
        k = end;
      } else if (IsDigit(label[k])) {
        const size_t n = DigitsAt(label, k);
        out_.Words(DigitWords(label.substr(k, n)));
        k += n;
      } else {
        if (label[k] == '-') {
          out_.Word("dash");
        } else if (label[k] == '_') {
          out_.Word("underscore");
        } else if (label[k] == '.') {
          out_.Word("dot");
        } else if (label[k] == '+') {
          out_.Word("plus");
        }
        ++k;
      }
    }
  }

  void Email(std::string_view local, std::string_view host) {
    Label(local);
    out_.Word("at");
    Host(host);
  }

  // Words, numbers and signs inside one chunk.
  void Pieces(std::string_view b) {
    size_t k = 0;
    while (k < b.size() && !out_.full()) {
      const char c = b[k];
      const bool digit_next = k + 1 < b.size() && IsDigit(b[k + 1]);
      if (IsAlpha(c)) {
        k = Letters(b, k);
      } else if (IsDigit(c) || (IsCurrency(c) && digit_next) ||
                 (c == '#' && digit_next) ||
                 ((c == '-' || c == '+') && k == 0 && digit_next) ||
                 (c == '.' && digit_next && (k == 0 || !IsAlnum(b[k - 1])))) {
        k = Number(b, k);
      } else {
        k = Sign(b, k);
      }
    }
  }

  size_t Letters(std::string_view b, size_t k) {
    // Dotted initials: "U.S.A.", "e.g.", "a.m.".
    if (k + 2 < b.size() && b[k + 1] == '.' && IsAlpha(b[k + 2]) &&
        (k + 3 == b.size() || !IsAlpha(b[k + 3]))) {
      std::string letters;
      size_t end = k;
      while (end < b.size() && IsAlpha(b[end]) &&
             (end + 1 == b.size() || !IsAlpha(b[end + 1]))) {
        letters.push_back(Lower(b[end]));
        ++end;
        if (end < b.size() && b[end] == '.') {
          ++end;
        } else {
          break;
        }
      }
      if (const char* said = Find(kInitialisms, letters)) {
        out_.Words(said);
      } else {
        out_.Spell(letters);
      }
      return end;
    }
    size_t end = k;
    while (end < b.size() &&
           (IsAlpha(b[end]) || (b[end] == '\'' && end + 1 < b.size() &&
                                IsAlpha(b[end + 1]) && end > k))) {
      ++end;
    }
    const std::string_view word = b.substr(k, end - k);
    const std::string lower = LowerAscii(word);
    // "Mr." inside the chunk, "vs", "etc".
    if (end < b.size() && b[end] == '.' && Find(kWithPeriod, lower)) {
      out_.Words(Find(kWithPeriod, lower));
      return end + 1;
    }
    if (lower == "no" && end + 1 == b.size() && b[end] == '.') {
      out_.Word("number");
      return end + 1;
    }
    if (const char* said = Find(kAlways, lower)) {
      out_.Words(said);
      return end;
    }
    // "w/" with, "w/o" without.
    if (lower == "w" && end < b.size() && b[end] == '/') {
      if (end + 1 < b.size() && Lower(b[end + 1]) == 'o' &&
          (end + 2 == b.size() || !IsAlpha(b[end + 2]))) {
        out_.Word("without");
        return end + 2;
      }
      out_.Word("with");
      return end + 1;
    }
    // "C#", "F#": the note, or the language.
    if (word.size() == 1 && end < b.size() && b[end] == '#') {
      out_.Spell(word);
      out_.Word("sharp");
      return end + 1;
    }
    out_.Word(word);
    return end;
  }

  // A sign between words.
  size_t Sign(std::string_view b, size_t k) {
    const char c = b[k];
    const bool alnum_before = k > 0 && IsAlnum(b[k - 1]);
    const bool alpha_after = k + 1 < b.size() && IsAlpha(b[k + 1]);
    switch (c) {
      case '&':
        out_.Word("and");
        break;
      case '+':
        out_.Word("plus");
        break;
      case '=':
        out_.Word("equals");
        break;
      case '@':
        out_.Word("at");
        break;
      case '%':
        out_.Word("percent");
        break;
      case kDegree:
        out_.Word("degrees");
        break;
      case '.':
        if (k + 2 < b.size() && b[k + 1] == '.' && b[k + 2] == '.') {
          out_.Pause();
          while (k + 1 < b.size() && b[k + 1] == '.') {
            ++k;
          }
        } else if (alpha_after && (k == 0 || alnum_before)) {
          out_.Word("dot");  // "Node.js", ".NET"
        } else {
          out_.End('.');
        }
        break;
      case '!':
      case '?':
        out_.End(c);
        break;
      case ',':
      case ';':
      case ':':
      case '|':
      case '(':
      case ')':
      case '[':
      case ']':
      case '{':
      case '}':
        out_.Pause();
        break;
      case '-':
        // Between words it only separates them ("state-of-the-art");
        // alone or doubled it is a dash.
        if (!(alnum_before && k + 1 < b.size() && IsAlnum(b[k + 1]))) {
          out_.Pause();
        }
        break;
      default:
        // Quotes, slashes and the rest separate words and are not read.
        break;
    }
    return k + 1;
  }

  // A number and what goes with it, from |k| (a digit, or a sign before
  // one). Returns where it ends.
  size_t Number(std::string_view b, size_t k) {
    if (b[k] == '-') {
      out_.Word("minus");
      ++k;
    } else if (b[k] == '+') {
      out_.Word("plus");
      ++k;
    } else if (b[k] == '#') {
      out_.Word("number");
      ++k;
    }
    char currency = 0;
    if (k < b.size() && IsCurrency(b[k])) {
      currency = b[k];
      ++k;
    }
    if (k >= b.size() || (!IsDigit(b[k]) && b[k] != '.')) {
      return k;
    }
    const size_t start = k;
    // The integer part, with thousands separators ("1,234,567").
    std::string digits;
    bool grouped = false;
    size_t first = DigitsAt(b, k);
    digits.append(b.substr(k, first));
    k += first;
    if (first >= 1 && first <= 3) {
      while (k + 3 < b.size() && b[k] == ',' && DigitsAt(b, k + 1) == 3) {
        digits.append(b.substr(k + 1, 3));
        k += 4;
        grouped = true;
      }
    }
    if (!grouped && currency == 0 && !digits.empty()) {
      // Clock times and durations, dates.
      if (k + 2 < b.size() && b[k] == ':' && DigitsAt(b, k + 1) == 2 &&
          digits.size() <= 2) {
        return Clock(b, digits, k);
      }
      if (digits.size() <= 2 && k < b.size() && b[k] == '/') {
        const size_t day = DigitsAt(b, k + 1);
        const size_t after_day = k + 1 + day;
        if (day >= 1 && day <= 2 && after_day < b.size() &&
            b[after_day] == '/' && DigitsAt(b, after_day + 1) == 4) {
          const uint64_t month = ParseDigits(digits);
          const uint64_t d = ParseDigits(b.substr(k + 1, day));
          const uint64_t y = ParseDigits(b.substr(after_day + 1, 4));
          if (Date(y, month, d)) {
            return after_day + 5;
          }
        }
      }
      if (digits.size() == 4 && k + 5 < b.size() && b[k] == '-' &&
          DigitsAt(b, k + 1) == 2 && k + 3 < b.size() && b[k + 3] == '-' &&
          DigitsAt(b, k + 4) == 2) {
        const uint64_t y = ParseDigits(digits);
        const uint64_t month = ParseDigits(b.substr(k + 1, 2));
        const uint64_t d = ParseDigits(b.substr(k + 4, 2));
        if (Date(y, month, d)) {
          return k + 6;
        }
      }
    }
    // The fraction, or a version ("1.2.3").
    std::string fraction;
    if (k + 1 < b.size() && b[k] == '.' && IsDigit(b[k + 1])) {
      const size_t n = DigitsAt(b, k + 1);
      const size_t after = k + 1 + n;
      if (after + 1 < b.size() && b[after] == '.' && IsDigit(b[after + 1])) {
        return Version(b, start);
      }
      fraction.assign(b.substr(k + 1, std::min(n, kMaxDigitsRead)));
      k = after;
    }
    // What follows: a suffix, a unit, a sign.
    size_t suffix_end = k;
    while (suffix_end < b.size() && IsAlpha(b[suffix_end])) {
      ++suffix_end;
    }
    const std::string_view suffix = b.substr(k, suffix_end - k);
    const std::string lower_suffix = LowerAscii(suffix);
    const uint64_t value = ParseDigits(digits.substr(0, kMaxDigitsAsNumber));
    const bool plain_integer = fraction.empty() && !digits.empty() &&
                               digits.size() <= kMaxDigitsAsNumber &&
                               !(digits.size() > 1 && digits[0] == '0');
    // Ordinals: "1st", "22nd".
    if (plain_integer && currency == 0 &&
        (lower_suffix == "st" || lower_suffix == "nd" || lower_suffix == "rd" ||
         lower_suffix == "th")) {
      out_.Words(OrdinalWords(value));
      return suffix_end;
    }
    // Decades: "1980s", "90s".
    if (plain_integer && currency == 0 && lower_suffix == "s" &&
        value % 10 == 0 && (digits.size() == 4 || digits.size() == 2)) {
      std::string words = digits.size() == 4
                              ? YearWords(static_cast<int>(value))
                              : CardinalWords(value);
      const size_t space = words.rfind(' ');
      const size_t last = space == std::string::npos ? 0 : space + 1;
      std::string tail = words.substr(last);
      // "eighty" -> "eighties"; "hundred" -> "hundreds".
      if (!tail.empty() && tail.back() == 'y') {
        tail = tail.substr(0, tail.size() - 1) + "ies";
      } else {
        tail += "s";
      }
      out_.Words(words.substr(0, last) + tail);
      return suffix_end;
    }
    // Counts as pages show them: "1.2M", "15K", "3B".
    std::string_view scale;
    if (suffix == "K" || suffix == "k") {
      scale = "thousand";
    } else if (suffix == "M") {
      scale = "million";
    } else if (suffix == "B" || suffix == "bn") {
      scale = "billion";
    } else if (suffix == "T") {
      scale = "trillion";
    }
    SayNumber(digits, fraction, grouped,
              /*as_year=*/currency == 0 && scale.empty() && suffix.empty() &&
                  (suffix_end >= b.size() || b[suffix_end] != '%'));
    k = suffix_end;
    if (!scale.empty()) {
      out_.Word(scale);
    } else if (!suffix.empty()) {
      const bool one = value == 1 && fraction.empty();
      if (const Unit* unit = FindUnit(suffix)) {
        out_.Words(one ? unit->one : unit->many);
      } else if ((lower_suffix == "am" || lower_suffix == "pm") &&
                 fraction.empty()) {
        out_.Spell(lower_suffix);
      } else if (lower_suffix == "x") {
        out_.Word(suffix_end < b.size() && IsDigit(b[suffix_end]) ? "by"
                                                                  : "times");
      } else {
        k = suffix_end - suffix.size();  // read as a word
      }
    }
    if (currency) {
      out_.Words(CurrencyName(currency,
                              value == 1 && fraction.empty() && scale.empty()));
    }
    if (k < b.size()) {
      if (b[k] == '%') {
        out_.Word("percent");
        ++k;
      } else if (b[k] == kCent) {
        out_.Word(value == 1 ? "cent" : "cents");
        ++k;
      } else if (b[k] == kDegree) {
        out_.Word("degrees");
        ++k;
        if (k < b.size() && (b[k] == 'C' || b[k] == 'F') &&
            (k + 1 == b.size() || !IsAlpha(b[k + 1]))) {
          out_.Word(b[k] == 'C' ? "Celsius" : "Fahrenheit");
          ++k;
        }
      } else if (b[k] == '-' && k + 1 < b.size() && IsDigit(b[k + 1]) &&
                 digits.size() <= 4 && fraction.empty() &&
                 DigitsAt(b, k + 1) <= 4 &&
                 !(digits.size() == 3 && DigitsAt(b, k + 1) == 4)) {
        out_.Word("to");  // a range: "5-10", "1990-2000"
        ++k;
      } else if (b[k] == '/' && k + 1 < b.size() && IsDigit(b[k + 1])) {
        out_.Words("out of");  // "4/5"
        ++k;
      }
    }
    return k;
  }

  // The integer and fraction read aloud. |as_year|: a plain four-digit
  // number from 1100 to 2099 is read as a year ("in 1985").
  void SayNumber(const std::string& digits,
                 const std::string& fraction,
                 bool grouped,
                 bool as_year) {
    if (digits.empty()) {
      out_.Word("zero");
    } else if ((digits.size() > 1 && digits[0] == '0' && !grouped) ||
               digits.size() > kMaxDigitsAsNumber) {
      out_.Words(DigitWords(digits));
    } else {
      const uint64_t value = ParseDigits(digits);
      if (as_year && !grouped && fraction.empty() && digits.size() == 4 &&
          value >= 1100 && value <= 2099) {
        out_.Words(YearWords(static_cast<int>(value)));
      } else {
        out_.Words(CardinalWords(value));
      }
    }
    if (!fraction.empty()) {
      out_.Word("point");
      out_.Words(DigitWords(fraction));
    }
  }

  // "6:07", "10:30", "1:02:03", "0:45".
  size_t Clock(std::string_view b, const std::string& hours_digits, size_t k) {
    const uint64_t first = ParseDigits(hours_digits);
    const uint64_t second = ParseDigits(b.substr(k + 1, 2));
    k += 3;
    std::optional<uint64_t> third;
    if (k + 2 < b.size() && b[k] == ':' && DigitsAt(b, k + 1) == 2) {
      third = ParseDigits(b.substr(k + 1, 2));
      k += 3;
    }
    if (third) {
      Duration(first, second, *third);
      return k;
    }
    // "am"/"pm" right after.
    size_t suffix_end = k;
    while (suffix_end < b.size() && IsAlpha(b[suffix_end])) {
      ++suffix_end;
    }
    const std::string suffix = LowerAscii(b.substr(k, suffix_end - k));
    const bool meridiem = suffix == "am" || suffix == "pm";
    if (!meridiem && (first == 0 || first > 24)) {
      Duration(0, first, second);  // minutes and seconds
      return k;
    }
    out_.Words(CardinalWords(first));
    if (second == 0) {
      if (!meridiem) {
        out_.Word("o'clock");
      }
    } else if (second < 10) {
      out_.Word("oh");
      out_.Words(CardinalWords(second));
    } else {
      out_.Words(CardinalWords(second));
    }
    if (meridiem) {
      out_.Spell(suffix);
      return suffix_end;
    }
    return k;
  }

  void Duration(uint64_t hours, uint64_t minutes, uint64_t seconds) {
    const auto unit = [this](uint64_t n, const char* one, const char* many) {
      if (n == 0) {
        return;
      }
      out_.Words(CardinalWords(n));
      out_.Word(n == 1 ? one : many);
    };
    unit(hours, "hour", "hours");
    unit(minutes, "minute", "minutes");
    unit(seconds, "second", "seconds");
    if (hours == 0 && minutes == 0 && seconds == 0) {
      out_.Words("zero seconds");
    }
  }

  bool Date(uint64_t year, uint64_t month, uint64_t day) {
    if (month < 1 || month > 12 || day < 1 || day > 31 || year < 1000 ||
        year > 2999) {
      return false;
    }
    out_.Word(kMonths[month - 1]);
    out_.Words(OrdinalWords(day));
    out_.Pause();
    out_.Words(YearWords(static_cast<int>(year)));
    return true;
  }

  // "1.2.3": each part, with "point" between.
  size_t Version(std::string_view b, size_t k) {
    bool first = true;
    while (k < b.size() && IsDigit(b[k])) {
      const size_t n = DigitsAt(b, k);
      if (!first) {
        out_.Word("point");
      }
      first = false;
      const std::string_view part = b.substr(k, n);
      if (part.size() > 1 && part[0] == '0') {
        out_.Words(DigitWords(part));
      } else {
        out_.Words(CardinalWords(ParseDigits(part.substr(0, 6))));
      }
      k += n;
      if (k + 1 < b.size() && b[k] == '.' && IsDigit(b[k + 1])) {
        ++k;
      } else {
        break;
      }
    }
    return k;
  }

  const std::string_view s_;
  Builder out_;
  bool next_starts_with_digit_ = false;
};

// Money with a two-digit fraction ("$4.99", "5.50 euros" written with the sign)
// is said "four dollars and ninety nine cents": rewritten as those words before
// the reader sees it (which would say "four point nine nine dollars").
std::string RewriteMoney(const std::string& ascii) {
  std::string out;
  out.reserve(ascii.size() + 32);
  size_t i = 0;
  while (i < ascii.size()) {
    const char c = ascii[i];
    const bool at_start = i == 0 || !IsAlnum(ascii[i - 1]);
    if (IsCurrency(c) && at_start && !SubunitName(c, true).empty() &&
        i + 1 < ascii.size() && IsDigit(ascii[i + 1])) {
      const size_t k = i + 1;
      const size_t first = DigitsAt(ascii, k);
      size_t end = k + first;
      std::string whole = ascii.substr(k, first);
      if (first <= 3) {
        while (end + 3 < ascii.size() && ascii[end] == ',' &&
               DigitsAt(ascii, end + 1) == 3) {
          whole.append(ascii, end + 1, 3);
          end += 4;
        }
      }
      if (end + 2 < ascii.size() && ascii[end] == '.' &&
          DigitsAt(ascii, end + 1) == 2 &&
          (end + 3 == ascii.size() || !IsAlnum(ascii[end + 3])) &&
          whole.size() <= kMaxDigitsAsNumber &&
          !(whole.size() > 1 && whole[0] == '0')) {
        const uint64_t units = ParseDigits(whole);
        const uint64_t cents = ParseDigits(ascii.substr(end + 1, 2));
        out.push_back(' ');
        if (units > 0 || cents == 0) {
          out.append(CardinalWords(units));
          out.push_back(' ');
          out.append(CurrencyName(c, units == 1));
          if (cents > 0) {
            out.append(" and ");
          }
        }
        if (cents > 0) {
          out.append(CardinalWords(cents));
          out.push_back(' ');
          out.append(SubunitName(c, cents == 1));
        }
        out.push_back(' ');
        i = end + 3;
        continue;
      }
    }
    out.push_back(c);
    ++i;
  }
  return out;
}

}  // namespace

std::vector<SpokenSentence> NormalizeForSpeech(std::string_view text) {
  const std::string ascii = RewriteMoney(FoldToAscii(text));
  Reader reader(ascii);
  return reader.Read();
}

}  // namespace molt_voice
