// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_helper/voice_lts.h"

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <utility>

namespace molt_voice {

namespace {

// One rule: |letters| at the current place, with |left| before them and
// |right| after them, sound as |sounds|. In the contexts:
//   ' '  the edge of the word
//   '#'  one or more vowels (a e i o u)
//   ':'  zero or more consonants
//   '^'  one consonant
//   '.'  one voiced consonant (b d g j l m n r v w z)
//   '+'  e, i or y
//   '%'  a suffix: ing, ely, er, es, ed or e (right context only)
//   '@'  t s r d l z n j, or th ch sh (they make "u" sound "oo")
//   '&'  s c g z x j, or ch sh (the sibilants: "es" sounds "iz")
// The sounds are espeak-ng's IPA for American English, which the voice's
// lexicon uses: "ɡ" is U+0261, "ɹ" the English r.
struct Rule {
  const char* left;
  const char* letters;
  const char* right;
  const char* sounds;
};

constexpr Rule kA[] = {
    {" ", "a", " ", "ɐ"},        {"", "a", " ", "ə"},
    {"", "are", " ", "ɑːɹ"},     {" ", "ar", "o", "ɚ"},
    {"", "ar", "#", "ɛɹ"},       {" ^", "as", "#", "eɪs"},
    {"", "a", "wa", "ə"},        {"", "aw", "", "ɔː"},
    {" :", "any", "", "ɛni"},    {"", "a", "^+#", "eɪ"},
    {"#:", "ally", "", "əli"},   {" ", "al", "#", "əl"},
    {"", "again", "", "əɡɛn"},   {"#:", "ag", "e", "ɪdʒ"},
    {"", "a", "^+:#", "æ"},      {" :", "a", "^+ ", "eɪ"},
    {"", "a", "^%", "eɪ"},       {" ", "arr", "", "əɹ"},
    {"", "arr", "", "æɹ"},       {" :", "ar", " ", "ɑːɹ"},
    {"", "ar", " ", "ɚ"},        {"", "ar", "", "ɑːɹ"},
    {"", "air", "", "ɛɹ"},       {"", "ai", "", "eɪ"},
    {"", "ay", "", "eɪ"},        {"", "au", "", "ɔː"},
    {"#:", "al", " ", "əl"},     {"#:", "als", " ", "əlz"},
    {"", "alk", "", "ɔːk"},      {"", "al", "^", "ɔːl"},
    {" :", "able", "", "eɪbəl"}, {"", "able", "", "əbəl"},
    {"", "ang", "+", "eɪndʒ"},   {"", "a", "", "æ"},
};

constexpr Rule kB[] = {
    {" ", "be", "^#", "bɪ"},    {"", "being", "", "biːɪŋ"},
    {" ", "both", " ", "boʊθ"}, {" ", "bus", "#", "bɪz"},
    {"", "buil", "", "bɪl"},    {"", "b", "", "b"},
};

constexpr Rule kC[] = {
    {" ", "ch", "^", "k"},    {"^e", "ch", "", "k"},   {"", "ch", "", "tʃ"},
    {" s", "ci", "#", "saɪ"}, {"", "ci", "a", "ʃ"},    {"", "ci", "o", "ʃ"},
    {"", "ci", "en", "ʃ"},    {"", "cc", "+", "ks"},   {"", "ck", "", "k"},
    {"", "cq", "", "k"},      {"", "com", "%", "kʌm"}, {"", "c", "+", "s"},
    {"", "c", "", "k"},
};

constexpr Rule kD[] = {
    {"#:", "ded", " ", "dɪd"},   {".e", "d", " ", "d"},
    {"#^:e", "d", " ", "t"},     {" ", "de", "^#", "dɪ"},
    {" ", "do", " ", "duː"},     {" ", "does", "", "dʌz"},
    {" ", "doing", "", "duːɪŋ"}, {" ", "dow", "", "daʊ"},
    {"", "du", "a", "dʒuː"},     {"", "dg", "+", "dʒ"},
    {"", "dj", "", "dʒ"},        {"", "d", "", "d"},
};

constexpr Rule kE[] = {
    {"#:", "e", " ", ""},     {" :", "e", " ", "iː"},
    {"#", "ed", " ", "d"},    {"#:", "e", "d ", ""},
    {"", "ev", "er", "ɛv"},   {"", "e", "^%", "iː"},
    {"", "eri", "#", "ɪɹi"},  {"", "eri", "", "ɛɹɪ"},
    {"#:", "er", "#", "ɚ"},   {"", "er", "#", "ɛɹ"},
    {"", "er", "", "ɚ"},      {" ", "even", "", "iːvən"},
    {"#:", "e", "w", ""},     {"@", "ew", "", "uː"},
    {"", "ew", "", "juː"},    {"", "e", "o", "iː"},
    {"#:&", "es", " ", "ɪz"}, {"#:", "e", "s ", ""},
    {"#:", "ely", " ", "li"}, {"#:", "ement", "", "mənt"},
    {"", "eful", "", "fʊl"},  {"", "ee", "", "iː"},
    {"", "earn", "", "ɜːn"},  {" ", "ear", "^", "ɜː"},
    {"", "ead", "", "ɛd"},    {"#:", "ea", " ", "iːə"},
    {"", "ea", "su", "ɛ"},    {"", "ea", "", "iː"},
    {"", "eigh", "", "eɪ"},   {"", "ei", "", "iː"},
    {" ", "eye", "", "aɪ"},   {"", "ey", "", "i"},
    {"", "eu", "", "juː"},    {"", "e", "", "ɛ"},
};

constexpr Rule kF[] = {
    {"", "ful", "", "fʊl"},
    {"", "f", "", "f"},
};

constexpr Rule kG[] = {
    {"", "giv", "", "ɡɪv"}, {" ", "g", "i^", "ɡ"},
    {"", "ge", "t", "ɡɛ"},  {"su", "gges", "", "ɡdʒɛs"},
    {"", "gg", "", "ɡ"},    {" b#", "g", "", "ɡ"},
    {"", "g", "+", "dʒ"},   {"", "great", "", "ɡɹeɪt"},
    {"#", "gh", "", ""},    {"", "g", "", "ɡ"},
};

constexpr Rule kH[] = {
    {" ", "hav", "", "hæv"}, {" ", "here", "", "hɪɹ"}, {" ", "hour", "", "aʊɚ"},
    {"", "how", "", "haʊ"},  {"", "h", "#", "h"},      {"", "h", "", ""},
};

constexpr Rule kI[] = {
    {" ", "in", "", "ɪn"},     {" ", "i", " ", "aɪ"},
    {"", "in", "d", "aɪn"},    {"", "ier", "", "iɚ"},
    {"#:r", "ied", "", "iːd"}, {"", "ied", " ", "aɪd"},
    {"", "ien", "", "iːɛn"},   {"", "ie", "t", "aɪɛ"},
    {" :", "i", "%", "aɪ"},    {"", "i", "%", "iː"},
    {"", "ie", "", "iː"},      {"", "i", "^+:#", "ɪ"},
    {"", "ir", "#", "aɪɹ"},    {"", "iz", "%", "aɪz"},
    {"", "is", "%", "aɪz"},    {"", "i", "d%", "aɪ"},
    {"+^", "i", "^+", "ɪ"},    {"", "i", "t%", "aɪ"},
    {"#^:", "i", "^+", "ɪ"},   {"", "i", "^+", "aɪ"},
    {"", "ir", "", "ɜː"},      {"", "igh", "", "aɪ"},
    {"", "ild", "", "aɪld"},   {"", "ign", " ", "aɪn"},
    {"", "ign", "^", "aɪn"},   {"", "ign", "%", "aɪn"},
    {"", "ible", "", "ɪbəl"},  {"", "ique", "", "iːk"},
    {"", "i", "", "ɪ"},
};

constexpr Rule kJ[] = {
    {"", "j", "", "dʒ"},
};

constexpr Rule kK[] = {
    {" ", "k", "n", ""},
    {"", "k", "", "k"},
};

constexpr Rule kL[] = {
    {"", "lo", "c#", "loʊ"},
    {"#^:", "l", "%", "əl"},
    {"", "lead", "", "liːd"},
    {"", "l", "", "l"},
};

constexpr Rule kM[] = {
    {"", "mov", "", "muːv"},
    {"", "m", "", "m"},
};

constexpr Rule kN[] = {
    {"e", "ng", "+", "ndʒ"},  {"", "ng", "r", "ŋɡ"}, {"", "ng", "#", "ŋɡ"},
    {"", "ngl", "%", "ŋɡəl"}, {"", "ng", "", "ŋ"},   {"", "nk", "", "ŋk"},
    {" ", "now", " ", "naʊ"}, {"", "n", "", "n"},
};

constexpr Rule kO[] = {
    {"", "of", " ", "əv"},     {"", "orough", "", "ɜːoʊ"},
    {"#:", "or", " ", "ɚ"},    {"#:", "ors", " ", "ɚz"},
    {"", "or", "", "ɔːɹ"},     {" ", "one", "", "wʌn"},
    {"", "ow", "", "oʊ"},      {" ", "over", "", "oʊvɚ"},
    {"", "ov", "", "ʌv"},      {"", "o", "^%", "oʊ"},
    {"", "o", "^en", "oʊ"},    {"", "o", "^i#", "oʊ"},
    {"", "ol", "d", "oʊl"},    {"", "ought", "", "ɔːt"},
    {"", "ough", "", "ʌf"},    {" ", "ou", "", "aʊ"},
    {"h", "ou", "s#", "aʊ"},   {"", "ous", "", "əs"},
    {"", "our", "", "ɔːɹ"},    {"", "ould", "", "ʊd"},
    {"^", "ou", "^l", "ʌ"},    {"", "oup", "", "uːp"},
    {"", "ou", "", "aʊ"},      {"", "oy", "", "ɔɪ"},
    {"", "oing", "", "oʊɪŋ"},  {"", "oi", "", "ɔɪ"},
    {"", "oor", "", "ɔːɹ"},    {"", "ook", "", "ʊk"},
    {"", "ood", "", "ʊd"},     {"", "oo", "", "uː"},
    {"", "o", "e", "oʊ"},      {"", "o", " ", "oʊ"},
    {"", "oa", "", "oʊ"},      {" ", "only", "", "oʊnli"},
    {" ", "once", "", "wʌns"}, {"c", "o", "n", "ɑː"},
    {"", "o", "ng", "ɔː"},     {" :^", "o", "n", "ʌ"},
    {"i", "on", "", "ən"},     {"#:", "on", " ", "ən"},
    {"#^", "on", "", "ən"},    {"", "o", "st ", "oʊ"},
    {"", "of", "^", "ɔːf"},    {"", "other", "", "ʌðɚ"},
    {"", "oss", " ", "ɔːs"},   {"#^:", "om", "", "ʌm"},
    {"", "o", "", "ɑː"},
};

constexpr Rule kP[] = {
    {"", "ph", "", "f"},     {"", "peop", "", "piːp"}, {"", "pow", "", "paʊ"},
    {"", "put", " ", "pʊt"}, {"", "p", "", "p"},
};

constexpr Rule kQ[] = {
    {"", "quar", "", "kwɔːɹ"},
    {"", "qu", "", "kw"},
    {"", "q", "", "k"},
};

constexpr Rule kR[] = {
    {" ", "re", "^#", "ɹiː"},
    {"", "r", "", "ɹ"},
};

constexpr Rule kS[] = {
    {"", "sh", "", "ʃ"},      {"#", "sion", "", "ʒən"}, {"", "some", "", "sʌm"},
    {"#", "sur", "#", "ʒɚ"},  {"", "sur", "#", "ʃɚ"},   {"#", "su", "#", "ʒuː"},
    {"#", "ssu", "#", "ʃuː"}, {"#", "sed", " ", "zd"},  {"#", "s", "#", "z"},
    {"", "said", "", "sɛd"},  {"^", "sion", "", "ʃən"}, {".", "s", " ", "z"},
    {"#:.e", "s", " ", "z"},  {"#^:##", "s", " ", "z"}, {"#^:#", "s", " ", "s"},
    {"u", "s", " ", "s"},     {" :#", "s", " ", "z"},   {" ", "sch", "", "sk"},
    {"", "s", "c+", ""},      {"#", "sm", "", "zəm"},   {"", "s", "", "s"},
};

constexpr Rule kT[] = {
    {" ", "the", " ", "ðə"},     {"", "to", " ", "tuː"},
    {"", "that", " ", "ðæt"},    {" ", "this", " ", "ðɪs"},
    {" ", "they", "", "ðeɪ"},    {" ", "there", "", "ðɛɹ"},
    {"", "ther", "", "ðɚ"},      {"", "their", "", "ðɛɹ"},
    {" ", "than", " ", "ðæn"},   {" ", "them", " ", "ðɛm"},
    {"", "these", " ", "ðiːz"},  {" ", "then", "", "ðɛn"},
    {"", "through", "", "θɹuː"}, {"", "those", "", "ðoʊz"},
    {"", "though", " ", "ðoʊ"},  {" ", "thus", "", "ðʌs"},
    {"", "th", "", "θ"},         {"#:", "ted", " ", "tɪd"},
    {"s", "ti", "#n", "tʃ"},     {"", "ti", "o", "ʃ"},
    {"", "ti", "a", "ʃ"},        {"", "tien", "", "ʃən"},
    {"", "tur", "#", "tʃɚ"},     {"", "tu", "a", "tʃuː"},
    {" ", "two", "", "tuː"},     {"", "t", "", "t"},
};

constexpr Rule kU[] = {
    {" ", "un", "i", "juːn"},   {" ", "un", "", "ʌn"},
    {" ", "upon", "", "əpɑːn"}, {"@", "ur", "#", "ʊɹ"},
    {"", "ur", "#", "jʊɹ"},     {"", "ur", "^", "ɜː"},
    {"", "u", "^ ", "ʌ"},       {"", "u", "^^", "ʌ"},
    {"", "uy", "", "aɪ"},       {" g", "u", "#", ""},
    {"g", "u", "%", ""},        {"g", "u", "#", "w"},
    {"#n", "u", "", "juː"},     {"@", "u", "", "uː"},
    {"", "u", "", "juː"},
};

constexpr Rule kV[] = {
    {"", "view", "", "vjuː"},
    {"", "v", "", "v"},
};

constexpr Rule kW[] = {
    {" ", "were", "", "wɜː"}, {"", "wa", "s", "wɑː"},  {"", "wa", "t", "wɑː"},
    {"", "where", "", "wɛɹ"}, {"", "what", "", "wʌt"}, {"", "whol", "", "hoʊl"},
    {"", "who", "", "huː"},   {"", "wh", "", "w"},     {"", "war", "", "wɔːɹ"},
    {"", "wor", "^", "wɜː"},  {"", "wr", "", "ɹ"},     {"", "w", "", "w"},
};

constexpr Rule kX[] = {
    {" ", "x", "", "z"},
    {"", "x", "", "ks"},
};

constexpr Rule kY[] = {
    {"", "young", "", "jʌŋ"}, {" ", "you", "", "juː"}, {" ", "yes", "", "jɛs"},
    {" ", "y", "", "j"},      {"#^:", "y", " ", "i"},  {"#^:", "y", "i", "i"},
    {" :", "y", " ", "aɪ"},   {" :", "y", "#", "aɪ"},  {" :", "y", "^+:#", "ɪ"},
    {" :", "y", "^#", "aɪ"},  {"", "y", "", "ɪ"},
};

constexpr Rule kZ[] = {
    {"", "z", "", "z"},
};

// The rules of a letter, and how many there are.
template <size_t N>
std::pair<const Rule*, size_t> Set(const Rule (&rules)[N]) {
  return {rules, N};
}

std::pair<const Rule*, size_t> RulesFor(char letter) {
  switch (letter) {
    case 'a':
      return Set(kA);
    case 'b':
      return Set(kB);
    case 'c':
      return Set(kC);
    case 'd':
      return Set(kD);
    case 'e':
      return Set(kE);
    case 'f':
      return Set(kF);
    case 'g':
      return Set(kG);
    case 'h':
      return Set(kH);
    case 'i':
      return Set(kI);
    case 'j':
      return Set(kJ);
    case 'k':
      return Set(kK);
    case 'l':
      return Set(kL);
    case 'm':
      return Set(kM);
    case 'n':
      return Set(kN);
    case 'o':
      return Set(kO);
    case 'p':
      return Set(kP);
    case 'q':
      return Set(kQ);
    case 'r':
      return Set(kR);
    case 's':
      return Set(kS);
    case 't':
      return Set(kT);
    case 'u':
      return Set(kU);
    case 'v':
      return Set(kV);
    case 'w':
      return Set(kW);
    case 'x':
      return Set(kX);
    case 'y':
      return Set(kY);
    default:
      return Set(kZ);
  }
}

bool IsVowelLetter(char c) {
  return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}
bool IsConsonantLetter(char c) {
  return c >= 'a' && c <= 'z' && !IsVowelLetter(c);
}
bool IsVoiced(char c) {
  return c == 'b' || c == 'd' || c == 'g' || c == 'j' || c == 'l' || c == 'm' ||
         c == 'n' || c == 'r' || c == 'v' || c == 'w' || c == 'z';
}
bool IsFront(char c) {
  return c == 'e' || c == 'i' || c == 'y';
}
bool IsOneOf(char c, std::string_view set) {
  return set.find(c) != std::string_view::npos;
}

// |w| is the word with a space at each end; positions are into it.
char At(const std::string& w, ptrdiff_t pos) {
  return pos >= 0 && static_cast<size_t>(pos) < w.size()
             ? w[static_cast<size_t>(pos)]
             : '\0';
}

// Whether |c| (at the place a context element looks at) is in the
// single-letter class |cls|.
bool InClass(char cls, char c) {
  switch (cls) {
    case '^':
      return IsConsonantLetter(c);
    case '.':
      return IsVoiced(c);
    case '+':
      return IsFront(c);
    default:
      return false;
  }
}

// The context before the letters: |pattern|'s elements from |p| - 1 down
// to 0, against the word from |pos| leftwards. "#" and ":" try every
// length, longest first (a rule like "#^:" needs ":" to leave a consonant
// for "^").
bool MatchLeftFrom(const std::string& w,
                   ptrdiff_t pos,
                   std::string_view pattern,
                   size_t p) {
  if (p == 0) {
    return true;
  }
  const char c = pattern[p - 1];
  const char here = At(w, pos);
  switch (c) {
    case ' ':
      return here == ' ' && MatchLeftFrom(w, pos - 1, pattern, p - 1);
    case '#':
    case ':': {
      const bool vowels = c == '#';
      ptrdiff_t end = pos;
      while (vowels ? IsVowelLetter(At(w, end))
                    : IsConsonantLetter(At(w, end))) {
        --end;
      }
      for (ptrdiff_t at = end; at <= pos - (vowels ? 1 : 0); ++at) {
        if (MatchLeftFrom(w, at, pattern, p - 1)) {
          return true;
        }
      }
      return false;
    }
    case '^':
    case '.':
    case '+':
      return InClass(c, here) && MatchLeftFrom(w, pos - 1, pattern, p - 1);
    case '@':
      if (here == 'h' && IsOneOf(At(w, pos - 1), "tcs")) {
        return MatchLeftFrom(w, pos - 2, pattern, p - 1);
      }
      return here != '\0' && IsOneOf(here, "tsrdlznj") &&
             MatchLeftFrom(w, pos - 1, pattern, p - 1);
    case '&':
      if (here == 'h' && IsOneOf(At(w, pos - 1), "cs")) {
        return MatchLeftFrom(w, pos - 2, pattern, p - 1);
      }
      return here != '\0' && IsOneOf(here, "scgzxj") &&
             MatchLeftFrom(w, pos - 1, pattern, p - 1);
    default:
      return here == c && MatchLeftFrom(w, pos - 1, pattern, p - 1);
  }
}

bool MatchLeft(const std::string& w, ptrdiff_t pos, std::string_view pattern) {
  return MatchLeftFrom(w, pos, pattern, pattern.size());
}

// The context after the letters: |pattern| from |p| on, against the word
// from |pos| rightwards; "#" and ":" try every length, longest first.
bool MatchRightFrom(const std::string& w,
                    ptrdiff_t pos,
                    std::string_view pattern,
                    size_t p) {
  if (p == pattern.size()) {
    return true;
  }
  const char c = pattern[p];
  const char here = At(w, pos);
  switch (c) {
    case ' ':
      return here == ' ' && MatchRightFrom(w, pos + 1, pattern, p + 1);
    case '#':
    case ':': {
      const bool vowels = c == '#';
      ptrdiff_t end = pos;
      while (vowels ? IsVowelLetter(At(w, end))
                    : IsConsonantLetter(At(w, end))) {
        ++end;
      }
      for (ptrdiff_t at = end; at >= pos + (vowels ? 1 : 0); --at) {
        if (MatchRightFrom(w, at, pattern, p + 1)) {
          return true;
        }
      }
      return false;
    }
    case '^':
    case '.':
    case '+':
      return InClass(c, here) && MatchRightFrom(w, pos + 1, pattern, p + 1);
    case '%': {
      static constexpr std::string_view kSuffixes[] = {"ing", "ely", "er",
                                                       "es",  "ed",  "e"};
      for (std::string_view suffix : kSuffixes) {
        if (pos >= 0 && static_cast<size_t>(pos) <= w.size() &&
            w.compare(static_cast<size_t>(pos), suffix.size(), suffix) == 0 &&
            MatchRightFrom(w, pos + static_cast<ptrdiff_t>(suffix.size()),
                           pattern, p + 1)) {
          return true;
        }
      }
      return false;
    }
    case '@':
      if (here != '\0' && IsOneOf(here, "tcs") && At(w, pos + 1) == 'h') {
        return MatchRightFrom(w, pos + 2, pattern, p + 1);
      }
      return here != '\0' && IsOneOf(here, "tsrdlznj") &&
             MatchRightFrom(w, pos + 1, pattern, p + 1);
    case '&':
      if (here != '\0' && IsOneOf(here, "cs") && At(w, pos + 1) == 'h') {
        return MatchRightFrom(w, pos + 2, pattern, p + 1);
      }
      return here != '\0' && IsOneOf(here, "scgzxj") &&
             MatchRightFrom(w, pos + 1, pattern, p + 1);
    default:
      return here == c && MatchRightFrom(w, pos + 1, pattern, p + 1);
  }
}

bool MatchRight(const std::string& w, ptrdiff_t pos, std::string_view pattern) {
  return MatchRightFrom(w, pos, pattern, 0);
}

// Decodes the code point at |s[*i]| (valid UTF-8 from the tables above, or
// the lexicon after its check); advances past it.
uint32_t NextCodePoint(std::string_view s, size_t* i) {
  const auto b0 = static_cast<uint8_t>(s[*i]);
  size_t length = b0 < 0x80 ? 1 : b0 < 0xE0 ? 2 : b0 < 0xF0 ? 3 : 4;
  if (*i + length > s.size()) {
    length = s.size() - *i;
  }
  uint32_t cp = length == 1   ? b0
                : length == 2 ? (b0 & 0x1Fu)
                : length == 3 ? (b0 & 0x0Fu)
                              : (b0 & 0x07u);
  for (size_t k = 1; k < length; ++k) {
    cp = (cp << 6) | (static_cast<uint8_t>(s[*i + k]) & 0x3Fu);
  }
  *i += length;
  return cp;
}

bool IsVowelCodePoint(uint32_t cp) {
  switch (cp) {
    case 'a':
    case 'e':
    case 'i':
    case 'o':
    case 'u':
    case 0x00E6:  // æ
    case 0x0250:  // ɐ
    case 0x0251:  // ɑ
    case 0x0254:  // ɔ
    case 0x0259:  // ə
    case 0x025A:  // ɚ
    case 0x025B:  // ɛ
    case 0x025C:  // ɜ
    case 0x026A:  // ɪ
    case 0x028A:  // ʊ
    case 0x028C:  // ʌ
    case 0x1D7B:  // ᵻ
      return true;
    default:
      return false;
  }
}

}  // namespace

bool StartsWithVowelSound(std::string_view phones) {
  size_t i = 0;
  while (i < phones.size()) {
    const uint32_t cp = NextCodePoint(phones, &i);
    if (cp == 0x02C8 || cp == 0x02CC) {
      continue;  // stress marks
    }
    return IsVowelCodePoint(cp);
  }
  return false;
}

std::string LetterToSound(std::string_view word) {
  std::string w = " ";
  for (char c : word) {
    if (c >= 'A' && c <= 'Z') {
      w.push_back(static_cast<char>(c - 'A' + 'a'));
    } else if (c >= 'a' && c <= 'z') {
      w.push_back(c);
    }
  }
  if (w.size() == 1) {
    return std::string();
  }
  w.push_back(' ');
  std::string out;
  size_t i = 1;
  while (i + 1 < w.size()) {
    const char c = w[i];
    // A doubled consonant is said once ("Sennheiser"); "cc" before e, i or
    // y has its own rule, which takes both letters.
    if (i > 1 && c == w[i - 1] && IsConsonantLetter(c)) {
      ++i;
      continue;
    }
    const auto [rules, count] = RulesFor(c);
    bool matched = false;
    for (size_t r = 0; r < count; ++r) {
      const Rule& rule = rules[r];
      const std::string_view letters(rule.letters);
      if (w.compare(i, letters.size(), letters) != 0 ||
          !MatchLeft(w, static_cast<ptrdiff_t>(i) - 1, rule.left) ||
          !MatchRight(w, static_cast<ptrdiff_t>(i + letters.size()),
                      rule.right)) {
        continue;
      }
      out.append(rule.sounds);
      i += letters.size();
      matched = true;
      break;
    }
    if (!matched) {
      ++i;  // every letter has a rule with no context, so never
    }
  }
  // Primary stress on the first vowel.
  size_t at = 0;
  while (at < out.size()) {
    size_t next = at;
    if (IsVowelCodePoint(NextCodePoint(out, &next))) {
      out.insert(at, "ˈ");
      break;
    }
    at = next;
  }
  return out;
}

}  // namespace molt_voice
