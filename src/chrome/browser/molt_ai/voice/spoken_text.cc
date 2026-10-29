// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/spoken_text.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace voice {

namespace {

constexpr uint32_t kReplacementCharacter = 0xFFFD;

// Decodes the code point at |text[at]|. Returns its length in bytes and
// sets |cp|; returns 0 for an invalid sequence (the caller skips one byte).
size_t DecodeAt(std::string_view text, size_t at, uint32_t* cp) {
  const auto byte = [&](size_t i) -> uint32_t {
    return static_cast<uint8_t>(text[i]);
  };
  const uint32_t b0 = byte(at);
  if (b0 < 0x80) {
    *cp = b0;
    return 1;
  }
  size_t length = 0;
  uint32_t lo = 0x80;
  uint32_t hi = 0xBF;
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    length = 2;
  } else if (b0 >= 0xE0 && b0 <= 0xEF) {
    length = 3;
    if (b0 == 0xE0) lo = 0xA0;  // no overlong forms
    if (b0 == 0xED) hi = 0x9F;  // no surrogates
  } else if (b0 >= 0xF0 && b0 <= 0xF4) {
    length = 4;
    if (b0 == 0xF0) lo = 0x90;  // no overlong forms
    if (b0 == 0xF4) hi = 0x8F;  // nothing past U+10FFFF
  } else {
    return 0;
  }
  if (text.size() - at < length) return 0;
  const uint32_t b1 = byte(at + 1);
  if (b1 < lo || b1 > hi) return 0;
  uint32_t value = (b0 & (0x7F >> length)) << 6 | (b1 & 0x3F);
  for (size_t i = 2; i < length; ++i) {
    const uint32_t b = byte(at + i);
    if (b < 0x80 || b > 0xBF) return 0;
    value = value << 6 | (b & 0x3F);
  }
  *cp = value;
  return length;
}

void AppendUtf8(uint32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

bool IsAsciiAlnum(uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') ||
         (cp >= 'A' && cp <= 'Z');
}

// Characters that render as nothing: dropped without breaking a word, and
// removed from text that is shown or passed on.
bool IsInvisibleFormat(uint32_t cp) {
  return cp == 0xAD || cp == 0x34F || cp == 0x61C || cp == 0x180E ||
         (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
         (cp >= 0x2060 && cp <= 0x206F) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
         cp == 0xFEFF || (cp >= 0xE0000 && cp <= 0xE007F) ||
         (cp >= 0xE0100 && cp <= 0xE01EF);
}

bool IsSpace(uint32_t cp) {
  return cp == ' ' || (cp >= 0x09 && cp <= 0x0D) || cp == 0x85 ||
         cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
         cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
         cp == 0x3000;
}

enum class CharClass {
  kWord,        // part of a word
  kIgnorable,   // dropped, the word goes on
  kApostrophe,  // dropped inside a word, a break elsewhere
  kBreak,       // ends a word
};

CharClass Classify(uint32_t cp) {
  if (cp < 0x80) {
    if (IsAsciiAlnum(cp)) return CharClass::kWord;
    return cp == '\'' ? CharClass::kApostrophe : CharClass::kBreak;
  }
  if (IsInvisibleFormat(cp)) return CharClass::kIgnorable;
  if (cp <= 0x9F) return CharClass::kBreak;  // C1 controls
  if (cp < 0xC0) {
    // Latin-1 punctuation and symbols, but for the ordinal indicators and
    // the micro sign.
    return (cp == 0xAA || cp == 0xB5 || cp == 0xBA) ? CharClass::kWord
                                                    : CharClass::kBreak;
  }
  if (cp == 0xD7 || cp == 0xF7) return CharClass::kBreak;  // x and divide
  if (cp <= 0x2AF) return CharClass::kWord;  // Latin extended, IPA
  if (cp == 0x2BC) return CharClass::kApostrophe;
  if (cp < 0x300) return CharClass::kBreak;  // spacing modifiers
  if (cp <= 0x36F) return CharClass::kIgnorable;  // combining accents
  if (cp == 0x37E || cp == 0x387) return CharClass::kBreak;  // Greek ; and .
  if (cp >= 0x483 && cp <= 0x489) return CharClass::kIgnorable;
  if (cp == 0x589 || cp == 0x5BE || cp == 0x5C0 || cp == 0x5C3 ||
      cp == 0x5F3 || cp == 0x5F4 || cp == 0x60C || cp == 0x60D ||
      cp == 0x61B || cp == 0x61F || cp == 0x66A || cp == 0x66B ||
      cp == 0x66C || cp == 0x6D4 || cp == 0x964 || cp == 0x965) {
    return CharClass::kBreak;  // Armenian, Hebrew, Arabic, Devanagari marks
  }
  if (cp == 0x640) return CharClass::kIgnorable;  // Arabic tatweel
  if (cp >= 0x2000 && cp <= 0x206F) {
    return (cp == 0x2018 || cp == 0x2019) ? CharClass::kApostrophe
                                          : CharClass::kBreak;
  }
  if (cp >= 0x2070 && cp <= 0x2BFF) return CharClass::kBreak;  // symbols
  if (cp >= 0x2E00 && cp <= 0x2E7F) return CharClass::kBreak;
  if (cp >= 0x3000 && cp <= 0x303F) return CharClass::kBreak;  // CJK marks
  if (cp >= 0xD800 && cp <= 0xDFFF) return CharClass::kBreak;
  if (cp >= 0xE000 && cp <= 0xF8FF) return CharClass::kBreak;  // private use
  if (cp >= 0xFE10 && cp <= 0xFE6F) return CharClass::kBreak;
  if (cp >= 0xFF00 && cp <= 0xFFEF) {
    if (cp == 0xFF07) return CharClass::kApostrophe;
    if ((cp >= 0xFF10 && cp <= 0xFF19) || (cp >= 0xFF21 && cp <= 0xFF3A) ||
        (cp >= 0xFF41 && cp <= 0xFF5A) || (cp >= 0xFF66 && cp <= 0xFFDC)) {
      return CharClass::kWord;
    }
    return CharClass::kBreak;
  }
  if (cp >= 0xFFF0 && cp <= 0xFFFF) return CharClass::kBreak;  // specials
  if (cp >= 0x1F000 && cp <= 0x1FAFF) return CharClass::kBreak;  // emoji
  if (cp >= 0xF0000) return CharClass::kBreak;  // private use planes
  return CharClass::kWord;
}

// U+00C0..U+00FF folded. "" for the two symbols in the block.
constexpr std::string_view kLatin1Fold[64] = {
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e",  "e", "i",
    "i", "i", "i", "d", "n", "o", "o",  "o", "o", "o", "",   "o", "u",
    "u", "u", "u", "y", "th", "ss", "a", "a", "a", "a", "a", "a", "ae",
    "c", "e", "e", "e", "e", "i", "i",  "i", "i", "d", "n", "o", "o",
    "o", "o", "o", "",  "o", "u", "u", "u", "u", "y", "th", "y"};

// U+0100..U+017F folded, one base letter each; '1' is "ij", '2' is "oe".
constexpr std::string_view kLatinExtendedAFold =
    "aaaaaa"
    "cccccccc"
    "dddd"
    "eeeeeeeeee"
    "gggggggg"
    "hhhh"
    "iiiiiiiiii"
    "11"
    "jj"
    "kkk"
    "llllllllll"
    "nnnnnnnnn"
    "oooooo"
    "22"
    "rrrrrr"
    "ssssssss"
    "tttttt"
    "uuuuuuuuuuuu"
    "ww"
    "yyy"
    "zzzzzz"
    "s";
static_assert(kLatinExtendedAFold.size() == 0x80);

// U+01CD..U+01DC: the pinyin vowels with a caron or a diaeresis.
constexpr std::string_view kPinyinFold = "aaiioouuuuuuuu";
static_assert(kPinyinFold.size() == 0x1DD - 0x1CD - 2);

// Appends the folded form of word character |cp| to |out|.
void Fold(uint32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(
        (cp >= 'A' && cp <= 'Z') ? cp - 'A' + 'a' : cp));
    return;
  }
  if (cp == 0xAA) return out->push_back('a');
  if (cp == 0xB5) return out->push_back('u');
  if (cp == 0xBA) return out->push_back('o');
  if (cp >= 0xC0 && cp <= 0xFF) {
    out->append(kLatin1Fold[cp - 0xC0]);
    return;
  }
  if (cp >= 0x100 && cp <= 0x17F) {
    const char c = kLatinExtendedAFold[cp - 0x100];
    if (c == '1') {
      out->append("ij");
    } else if (c == '2') {
      out->append("oe");
    } else {
      out->push_back(c);
    }
    return;
  }
  if (cp >= 0x1CD && cp < 0x1CD + kPinyinFold.size()) {
    out->push_back(kPinyinFold[cp - 0x1CD]);
    return;
  }
  if (cp == 0x218 || cp == 0x219) return out->push_back('s');  // Romanian
  if (cp == 0x21A || cp == 0x21B) return out->push_back('t');
  if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return AppendUtf8(cp + 0x20, out);
  if (cp == 0x3C2) return AppendUtf8(0x3C3, out);  // final sigma
  if (cp >= 0x410 && cp <= 0x42F) return AppendUtf8(cp + 0x20, out);
  if (cp >= 0x400 && cp <= 0x40F) return AppendUtf8(cp + 0x50, out);
  if (cp >= 0xFF10 && cp <= 0xFF19) return out->push_back(static_cast<char>('0' + cp - 0xFF10));
  if (cp >= 0xFF21 && cp <= 0xFF3A) return out->push_back(static_cast<char>('a' + cp - 0xFF21));
  if (cp >= 0xFF41 && cp <= 0xFF5A) return out->push_back(static_cast<char>('a' + cp - 0xFF41));
  AppendUtf8(cp, out);
}

bool IsJoiner(char c) {
  return c == '.' || c == '/' || c == '-' || c == '_' || c == ':' || c == '@';
}

bool IsAsciiPunct(char c) {
  const auto u = static_cast<unsigned char>(c);
  return u >= 0x21 && u <= 0x7E && !IsAsciiAlnum(u);
}

bool IsAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

}  // namespace

NormalizedText::NormalizedText() = default;
NormalizedText::~NormalizedText() = default;
NormalizedText::NormalizedText(const NormalizedText&) = default;
NormalizedText& NormalizedText::operator=(const NormalizedText&) = default;
NormalizedText::NormalizedText(NormalizedText&&) = default;
NormalizedText& NormalizedText::operator=(NormalizedText&&) = default;

std::optional<Language> LanguageFromTag(std::string_view tag) {
  std::string primary;
  for (char c : tag) {
    if (c == '-' || c == '_') break;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    primary.push_back(c);
  }
  if (primary == "en") return Language::kEnglish;
  return std::nullopt;
}

bool IsValidUtf8(std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    uint32_t cp = 0;
    const size_t length = DecodeAt(text, i, &cp);
    if (!length) return false;
    i += length;
  }
  return true;
}

NormalizedText NormalizeSpokenText(std::string_view text,
                                   const NormalizeOptions& options) {
  NormalizedText out;
  // Offsets are 32-bit; anything longer is read up to 1 GiB, far past any
  // cap voice mode applies first.
  if (text.size() > (1u << 30)) text = text.substr(0, 1u << 30);
  out.folded.reserve(text.size() + 16);

  bool in_word = false;
  SpokenWord word;
  const auto finish_word = [&] {
    if (!in_word) return;
    in_word = false;
    word.size = static_cast<uint32_t>(out.folded.size()) - word.begin;
    if (word.size == 0) return;  // only invisible characters
    const std::string_view folded =
        std::string_view(out.folded).substr(word.begin, word.size);
    word.numeric = std::all_of(folded.begin(), folded.end(), IsAsciiDigit);
    word.ascii = std::all_of(folded.begin(), folded.end(), [](char c) {
      return IsAsciiAlnum(static_cast<unsigned char>(c));
    });
    if (!out.words.empty()) {
      const SpokenWord& previous = out.words.back();
      if (word.source_begin == previous.source_end + 1 &&
          IsJoiner(text[previous.source_end])) {
        word.joiner = text[previous.source_end];
      }
    }
    if (word.source_end < text.size() && IsAsciiPunct(text[word.source_end])) {
      word.trailing = text[word.source_end];
    }
    out.words.push_back(word);
  };
  const auto add_made_up_word = [&](std::string_view made_up, size_t begin,
                                    size_t end) {
    if (out.words.size() >= options.max_words) {
      out.truncated = true;
      return;
    }
    SpokenWord made;
    made.begin = static_cast<uint32_t>(out.folded.size());
    made.size = static_cast<uint32_t>(made_up.size());
    made.source_begin = static_cast<uint32_t>(begin);
    made.source_end = static_cast<uint32_t>(end);
    made.ascii = true;
    out.folded.append(made_up);
    out.words.push_back(made);
  };

  size_t i = 0;
  while (i < text.size() && !out.truncated) {
    uint32_t cp = 0;
    size_t length = DecodeAt(text, i, &cp);
    if (!length) {
      out.had_invalid_utf8 = true;
      finish_word();
      ++i;
      continue;
    }
    const CharClass kind = Classify(cp);
    if (kind == CharClass::kWord) {
      if (!in_word) {
        if (out.words.size() >= options.max_words) {
          out.truncated = true;
          break;
        }
        in_word = true;
        word = SpokenWord();
        word.begin = static_cast<uint32_t>(out.folded.size());
        word.source_begin = static_cast<uint32_t>(i);
      }
      Fold(cp, &out.folded);
      i += length;
      word.source_end = static_cast<uint32_t>(i);
      continue;
    }
    if (kind == CharClass::kIgnorable) {
      i += length;
      continue;
    }
    // An apostrophe between two word characters is dropped ("what's").
    if (kind == CharClass::kApostrophe && in_word && i + length < text.size()) {
      uint32_t next = 0;
      if (DecodeAt(text, i + length, &next) && Classify(next) == CharClass::kWord) {
        i += length;
        continue;
      }
    }
    // A digit group: "1,000" is one number (exactly three digits follow).
    if (cp == ',' && in_word && i > 0 && IsAsciiDigit(text[i - 1]) &&
        text.size() - i > 3 && IsAsciiDigit(text[i + 1]) &&
        IsAsciiDigit(text[i + 2]) && IsAsciiDigit(text[i + 3]) &&
        (text.size() - i == 4 || !IsAsciiDigit(text[i + 4]))) {
      ++i;
      continue;
    }
    finish_word();
    if (options.marks) {
      if (cp == '%' && !out.words.empty() && out.words.back().numeric) {
        add_made_up_word("percent", i, i + 1);
      } else if (cp == '#' && i + 1 < text.size() && IsAsciiDigit(text[i + 1])) {
        add_made_up_word("number", i, i + 1);
      }
    }
    i += length;
  }
  finish_word();

  // "No. 4" and "no.4" say "number 4".
  if (options.marks) {
    for (size_t w = 0; w + 1 < out.words.size(); ++w) {
      if (out.Word(w) == "no" && out.words[w].trailing == '.' &&
          out.words[w + 1].numeric) {
        out.words[w].begin = static_cast<uint32_t>(out.folded.size());
        out.words[w].size = 6;
        out.folded.append("number");
      }
    }
  }
  return out;
}

std::string CleanSpokenText(std::string_view text, size_t max_bytes) {
  std::string out;
  out.reserve(std::min(text.size(), max_bytes));
  bool pending_space = false;
  size_t i = 0;
  while (i < text.size()) {
    uint32_t cp = 0;
    size_t length = DecodeAt(text, i, &cp);
    if (!length) {
      cp = kReplacementCharacter;
      length = 1;
    }
    i += length;
    if (IsSpace(cp)) {
      pending_space = !out.empty();
      continue;
    }
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) ||
        IsInvisibleFormat(cp)) {
      continue;
    }
    std::string encoded;
    if (pending_space) encoded.push_back(' ');
    AppendUtf8(cp, &encoded);
    if (out.size() + encoded.size() > max_bytes) break;
    out.append(encoded);
    pending_space = false;
  }
  return out;
}

std::string SourceText(std::string_view source,
                       const NormalizedText& text,
                       size_t first,
                       size_t last,
                       const std::vector<size_t>& skip) {
  std::string joined;
  if (first > last || last >= text.words.size()) return joined;
  size_t skip_at = 0;
  bool any = false;
  size_t previous = 0;
  for (size_t w = first; w <= last; ++w) {
    while (skip_at < skip.size() && skip[skip_at] < w) ++skip_at;
    if (skip_at < skip.size() && skip[skip_at] == w) continue;
    const SpokenWord& word = text.words[w];
    if (word.source_end > source.size() || word.source_begin > word.source_end) {
      continue;
    }
    if (any) {
      const SpokenWord& before = text.words[previous];
      if (previous + 1 == w && before.source_end <= word.source_begin) {
        joined.append(source.substr(before.source_end,
                                    word.source_begin - before.source_end));
      } else {
        joined.push_back(' ');
      }
    }
    joined.append(source.substr(word.source_begin,
                                word.source_end - word.source_begin));
    any = true;
    previous = w;
  }
  return CleanSpokenText(joined, joined.size());
}

std::string FoldForComparison(std::string_view text) {
  const NormalizedText normalized = NormalizeSpokenText(text, {});
  std::string out;
  for (size_t i = 0; i < normalized.words.size(); ++i) {
    if (i) out.push_back(' ');
    out.append(normalized.Word(i));
  }
  return out;
}

}  // namespace voice
}  // namespace molt_ai
