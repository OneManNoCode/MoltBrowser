// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_helper/voice_phonemes.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_helper/voice_lts.h"

namespace molt_voice {

namespace {

// Longest word and phones kept from the lexicon, in bytes.
constexpr size_t kMaxLexiconWordBytes = 64;
constexpr size_t kMaxLexiconPhonesBytes = 256;
// Words of at most this many letters may split into three parts.
constexpr size_t kMaxThreePartLetters = 20;
// Shortest part of a joined word.
constexpr size_t kMinPartLetters = 3;

bool IsLower(char c) {
  return c >= 'a' && c <= 'z';
}
bool IsUpperAscii(char c) {
  return c >= 'A' && c <= 'Z';
}
char Lower(char c) {
  return IsUpperAscii(c) ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string LowerLetters(std::string_view word) {
  std::string out;
  out.reserve(word.size());
  for (char c : word) {
    if (IsLower(c) || IsUpperAscii(c) || c == '\'') {
      out.push_back(Lower(c));
    }
  }
  return out;
}

// Decodes UTF-8 strictly: false on anything malformed.
bool DecodeUtf8(std::string_view s, std::vector<uint32_t>* out) {
  size_t i = 0;
  while (i < s.size()) {
    const auto b0 = static_cast<uint8_t>(s[i]);
    size_t length = 0;
    uint32_t cp = 0;
    uint32_t min = 0;
    if (b0 < 0x80) {
      length = 1;
      cp = b0;
    } else if (b0 >= 0xC2 && b0 <= 0xDF) {
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
      return false;
    }
    if (i + length > s.size()) {
      return false;
    }
    for (size_t k = 1; k < length; ++k) {
      const auto b = static_cast<uint8_t>(s[i + k]);
      if ((b & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    if (out) {
      out->push_back(cp);
    }
    i += length;
  }
  return true;
}

// The last sound of |phones|, ignoring stress and length marks.
uint32_t LastSound(std::string_view phones) {
  std::vector<uint32_t> cps;
  if (!DecodeUtf8(phones, &cps)) {
    return 0;
  }
  for (size_t i = cps.size(); i-- > 0;) {
    if (cps[i] != 0x02C8 && cps[i] != 0x02CC && cps[i] != 0x02D0) {
      return cps[i];
    }
  }
  return 0;
}

bool IsSibilant(uint32_t cp) {
  return cp == 's' || cp == 'z' || cp == 0x0283 /* ʃ */ || cp == 0x0292 /* ʒ */;
}
bool IsVoiceless(uint32_t cp) {
  return cp == 'p' || cp == 't' || cp == 'k' || cp == 'f' ||
         cp == 0x03B8 /* θ */;
}

// "-s": "ɪz" after a sibilant, "s" after a voiceless sound, else "z".
std::string PluralSound(std::string_view base) {
  const uint32_t last = LastSound(base);
  if (IsSibilant(last)) {
    return "ɪz";
  }
  return IsVoiceless(last) ? "s" : "z";
}

// "-ed": "ᵻd" after t or d, "t" after a voiceless sound, else "d".
std::string PastSound(std::string_view base) {
  const uint32_t last = LastSound(base);
  if (last == 't' || last == 'd' || last == 0x027E /* ɾ */) {
    return "ᵻd";
  }
  return IsVoiceless(last) || IsSibilant(last) ? "t" : "d";
}

// Function words as said in running text, where the lexicon lacks them or
// has the letter name ("a").
struct Override {
  const char* word;
  const char* phones;
};
constexpr Override kOverrides[] = {
    {"a", "ɐ"},          {"an", "ɐn"},           {"or", "ɔːɹ"},
    {"her", "hɜː"},      {"o'clock", "əklˈɑːk"}, {"ok", "ˌoʊkˈeɪ"},
    {"okay", "ˌoʊkˈeɪ"}, {"molt", "mˈoʊlt"},
};

// Letter names, each stressed.
constexpr const char* kLetterNames[26] = {
    "ˈeɪ",   "bˈiː",      "sˈiː",  "dˈiː", "ˈiː", "ˈɛf",  "dʒˈiː",
    "ˈeɪtʃ", "ˈaɪ",       "dʒˈeɪ", "kˈeɪ", "ˈɛl", "ˈɛm",  "ˈɛn",
    "ˈoʊ",   "pˈiː",      "kjˈuː", "ˈɑːɹ", "ˈɛs", "tˈiː", "jˈuː",
    "vˈiː",  "dˈʌbəljuː", "ˈɛks",  "wˈaɪ", "zˈiː"};

// All-capital words of at most three letters that are words, not
// acronyms ("GET THE BEST", "HOW TO").
constexpr const char* kCapitalWords[] = {
    "a",   "i",   "an",  "and", "the", "for", "you", "all", "new", "top",
    "big", "war", "off", "out", "now", "how", "why", "buy", "get", "one",
    "two", "my",  "me",  "we",  "he",  "she", "it",  "is",  "in",  "on",
    "of",  "to",  "or",  "at",  "by",  "do",  "go",  "no",  "so",  "up",
    "be",  "not", "our", "its", "his", "her", "are", "was", "has", "had",
    "can", "may", "day", "run", "see", "say", "set", "hot", "wow", "omg",
};

bool IsCapitalWord(std::string_view lower) {
  for (const char* word : kCapitalWords) {
    if (lower == word) {
      return true;
    }
  }
  return false;
}

bool HasVowelLetter(std::string_view lower) {
  for (char c : lower) {
    if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y') {
      return true;
    }
  }
  return false;
}

bool AllCapitals(std::string_view word) {
  size_t letters = 0;
  for (char c : word) {
    if (IsLower(c)) {
      return false;
    }
    letters += IsUpperAscii(c) ? 1 : 0;
  }
  return letters > 0;
}

}  // namespace

// ---- Lexicon ----------------------------------------------------------------

Lexicon::Lexicon() = default;
Lexicon::Lexicon(Lexicon&&) = default;
Lexicon& Lexicon::operator=(Lexicon&&) = default;
Lexicon::~Lexicon() = default;

std::string_view Lexicon::WordOf(const Entry& entry) const {
  return std::string_view(data_).substr(entry.word_offset, entry.word_size);
}

bool Lexicon::Load(std::string data, std::string* error) {
  entries_.clear();
  if (data.size() > kMaxLexiconBytes) {
    *error =
        "the lexicon is over " + std::to_string(kMaxLexiconBytes) + " bytes";
    return false;
  }
  if (!DecodeUtf8(data, nullptr)) {
    *error = "the lexicon isn't UTF-8";
    return false;
  }
  data_ = std::move(data);
  const std::string_view all(data_);
  size_t line_start = 0;
  size_t line_number = 0;
  while (line_start < all.size()) {
    size_t line_end = all.find('\n', line_start);
    if (line_end == std::string_view::npos) {
      line_end = all.size();
    }
    ++line_number;
    std::string_view line = all.substr(line_start, line_end - line_start);
    const size_t offset = line_start;
    line_start = line_end + 1;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const size_t bar = line.find(" || ");
    if (bar == std::string_view::npos || bar == 0) {
      *error = "lexicon line " + std::to_string(line_number) +
               " isn't \"word || phones\"";
      data_.clear();
      return false;
    }
    const std::string_view word = line.substr(0, bar);
    const std::string_view phones = line.substr(bar + 4);
    bool usable = word.size() <= kMaxLexiconWordBytes &&
                  phones.size() <= kMaxLexiconPhonesBytes && !phones.empty();
    for (char c : word) {
      usable = usable && (IsLower(c) || c == '\'');
    }
    for (char c : phones) {
      // No control characters; spaces separate the phones.
      usable = usable && static_cast<uint8_t>(c) >= 0x20 && c != 0x7F;
    }
    if (!usable) {
      continue;
    }
    Entry entry;
    entry.word_offset = static_cast<uint32_t>(offset);
    entry.word_size = static_cast<uint32_t>(word.size());
    entry.phones_offset = static_cast<uint32_t>(offset + bar + 4);
    entry.phones_size = static_cast<uint32_t>(phones.size());
    entries_.push_back(entry);
  }
  std::stable_sort(
      entries_.begin(), entries_.end(),
      [this](const Entry& a, const Entry& b) { return WordOf(a) < WordOf(b); });
  // The first of several entries for a word wins.
  entries_.erase(std::unique(entries_.begin(), entries_.end(),
                             [this](const Entry& a, const Entry& b) {
                               return WordOf(a) == WordOf(b);
                             }),
                 entries_.end());
  return true;
}

const Lexicon::Entry* Lexicon::Lookup(std::string_view word) const {
  auto it = std::lower_bound(entries_.begin(), entries_.end(), word,
                             [this](const Entry& entry, std::string_view w) {
                               return WordOf(entry) < w;
                             });
  if (it == entries_.end() || WordOf(*it) != word) {
    return nullptr;
  }
  return &*it;
}

bool Lexicon::Has(std::string_view word) const {
  return Lookup(word) != nullptr;
}

std::string Lexicon::Find(std::string_view word) const {
  const Entry* entry = Lookup(word);
  if (!entry) {
    return std::string();
  }
  std::string out;
  for (char c : std::string_view(data_).substr(entry->phones_offset,
                                               entry->phones_size)) {
    if (c != ' ') {
      out.push_back(c);
    }
  }
  return out;
}

// ---- Tokens -----------------------------------------------------------------

bool LoadVoiceTokens(std::string_view text,
                     std::vector<uint32_t>* tokens,
                     std::string* error) {
  tokens->clear();
  if (text.size() > (64 << 10)) {
    *error = "tokens.txt is too big";
    return false;
  }
  size_t start = 0;
  size_t line_number = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view line = text.substr(start, end - start);
    start = end + 1;
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.empty()) {
      continue;
    }
    // "<symbol> <id>", or " <id>" for the space.
    const size_t space = line.rfind(' ');
    if (space == std::string_view::npos || space + 1 >= line.size()) {
      *error = "tokens.txt line " + std::to_string(line_number) +
               " isn't \"symbol id\"";
      return false;
    }
    for (char c : line.substr(space + 1)) {
      if (c < '0' || c > '9') {
        *error =
            "tokens.txt line " + std::to_string(line_number) + " has a bad id";
        return false;
      }
    }
    const std::string_view symbol = line.substr(0, space);
    std::vector<uint32_t> cps;
    if (symbol.empty()) {
      cps.push_back(' ');
    } else if (!DecodeUtf8(symbol, &cps) || cps.size() != 1) {
      *error = "tokens.txt line " + std::to_string(line_number) +
               " isn't one code point";
      return false;
    }
    tokens->push_back(cps[0]);
  }
  std::sort(tokens->begin(), tokens->end());
  tokens->erase(std::unique(tokens->begin(), tokens->end()), tokens->end());
  if (tokens->empty()) {
    *error = "tokens.txt is empty";
    return false;
  }
  return true;
}

// ---- Phonemizer
// ---------------------------------------------------------------

Phonemizer::Phonemizer(Lexicon lexicon, std::vector<uint32_t> tokens)
    : lexicon_(std::move(lexicon)), tokens_(std::move(tokens)) {
  std::sort(tokens_.begin(), tokens_.end());
}

Phonemizer::~Phonemizer() = default;

std::string Phonemizer::Spell(std::string_view letters) const {
  std::string out;
  for (char c : letters) {
    const char lower = Lower(c);
    if (!IsLower(lower)) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(' ');
    }
    out.append(kLetterNames[lower - 'a']);
  }
  return out;
}

std::string Phonemizer::Word(std::string_view word,
                             bool all_caps_sentence) const {
  return WordAt(word, all_caps_sentence, 0);
}

std::string Phonemizer::Forms(std::string_view w) const {
  const auto found = [this](std::string_view base) {
    return base.size() >= 2 ? lexicon_.Find(base) : std::string();
  };
  const auto ends = [&w](std::string_view suffix) {
    return w.size() > suffix.size() + 1 &&
           w.substr(w.size() - suffix.size()) == suffix;
  };
  const auto stem = [&w](size_t cut) {
    return std::string(w.substr(0, w.size() - cut));
  };
  // A base for a suffix that may have dropped an "e" or doubled the last
  // consonant ("making", "stopped").
  const auto base_of = [&](size_t cut) -> std::string {
    const std::string s = stem(cut);
    std::string phones = found(s);
    if (!phones.empty()) {
      return phones;
    }
    phones = found(s + "e");
    if (!phones.empty()) {
      return phones;
    }
    if (s.size() >= 3 && s[s.size() - 1] == s[s.size() - 2]) {
      return found(s.substr(0, s.size() - 1));
    }
    return std::string();
  };
  std::string base;
  // Possessives.
  if (ends("'s")) {
    base = found(stem(2));
    if (!base.empty()) {
      return base + PluralSound(base);
    }
  }
  if (ends("s'")) {
    base = found(stem(1));
    if (!base.empty()) {
      return base;
    }
  }
  if (ends("ies") && !(base = found(stem(3) + "y")).empty()) {
    return base + "z";
  }
  if (ends("es") && !(base = found(stem(2))).empty() &&
      IsSibilant(LastSound(base))) {
    return base + "ɪz";
  }
  if (ends("s") && !ends("ss") && !(base = found(stem(1))).empty()) {
    return base + PluralSound(base);
  }
  if (ends("ied") && !(base = found(stem(3) + "y")).empty()) {
    return base + "d";
  }
  if (ends("ed") && !(base = base_of(2)).empty()) {
    return base + PastSound(base);
  }
  if (ends("ing") && !(base = base_of(3)).empty()) {
    return base + "ɪŋ";
  }
  if (ends("ers") && !(base = base_of(3)).empty()) {
    return base + "ɚz";
  }
  if (ends("er") && !(base = base_of(2)).empty()) {
    return base + "ɚ";
  }
  if (ends("est") && !(base = base_of(3)).empty()) {
    return base + "ɪst";
  }
  if (ends("ily") && !(base = found(stem(3) + "y")).empty()) {
    return base + "li";
  }
  if (ends("ly") && !(base = found(stem(2))).empty()) {
    return base + "li";
  }
  static constexpr std::pair<const char*, const char*> kSuffixes[] = {
      {"ness", "nəs"}, {"ments", "mənts"}, {"ment", "mənt"}, {"ful", "fəl"},
      {"less", "ləs"}, {"able", "əbəl"},   {"ism", "ɪzəm"},  {"ists", "ɪsts"},
      {"ist", "ɪst"},  {"ish", "ɪʃ"},      {"ship", "ʃɪp"},  {"hood", "hʊd"},
  };
  for (const auto& [suffix, sound] : kSuffixes) {
    if (ends(suffix) &&
        !(base = base_of(std::string_view(suffix).size())).empty()) {
      return base + sound;
    }
  }
  static constexpr std::pair<const char*, const char*> kPrefixes[] = {
      {"un", "ʌn"},       {"re", "ɹiː"},         {"non", "nɑːn"},
      {"pre", "pɹiː"},    {"dis", "dɪs"},        {"mis", "mɪs"},
      {"anti", "ˌæntaɪ"}, {"multi", "ˌmʌlti"},   {"super", "ˌsuːpɚ"},
      {"inter", "ˌɪntɚ"}, {"micro", "ˌmaɪkɹoʊ"}, {"auto", "ˌɔːɾoʊ"},
  };
  for (const auto& [prefix, sound] : kPrefixes) {
    const std::string_view p(prefix);
    if (w.size() >= p.size() + kMinPartLetters && w.substr(0, p.size()) == p) {
      // The rest from the lexicon, or a form of a word it has
      // ("unplayed").
      const std::string_view rest = w.substr(p.size());
      base = found(rest);
      if (base.empty()) {
        base = Forms(rest);
      }
      if (!base.empty()) {
        return std::string(sound) + base;
      }
    }
  }
  return std::string();
}

std::string Phonemizer::Known(std::string_view lower, int depth) const {
  for (const Override& o : kOverrides) {
    if (lower == o.word) {
      return o.phones;
    }
  }
  std::string phones = lexicon_.Find(lower);
  if (!phones.empty()) {
    return phones;
  }
  if (lower.find('\'') != std::string_view::npos) {
    // "rock'n'roll", "Wayne's" (Forms() has the possessive).
    phones = Forms(lower);
    if (!phones.empty()) {
      return phones;
    }
    std::string without;
    for (char c : lower) {
      if (c != '\'') {
        without.push_back(c);
      }
    }
    phones = lexicon_.Find(without);
    if (!phones.empty()) {
      return phones;
    }
    return std::string();
  }
  phones = Forms(lower);
  if (!phones.empty()) {
    return phones;
  }
  return Compound(lower, depth);
}

std::string Phonemizer::Compound(std::string_view w, int depth) const {
  if (depth >= 2 || w.size() < 2 * kMinPartLetters) {
    return std::string();
  }
  std::string best;
  size_t best_score = 0;
  for (size_t cut = kMinPartLetters; cut + kMinPartLetters <= w.size(); ++cut) {
    const std::string_view first = w.substr(0, cut);
    const std::string_view second = w.substr(cut);
    const size_t score = std::min(first.size(), second.size());
    if (score <= best_score) {
      continue;
    }
    std::string a = lexicon_.Find(first);
    if (a.empty()) {
      continue;
    }
    std::string b = lexicon_.Find(second);
    if (b.empty()) {
      b = Forms(second);
    }
    if (b.empty() && depth == 0 && w.size() <= kMaxThreePartLetters) {
      b = Compound(second, depth + 1);
    }
    if (b.empty()) {
      continue;
    }
    // The second part's primary stress becomes secondary.
    std::string joined = a;
    for (size_t i = 0; i < b.size();) {
      if (b.compare(i, 2, "ˈ") == 0) {
        joined.append("ˌ");
        i += 2;
      } else {
        joined.push_back(b[i]);
        ++i;
      }
    }
    best = std::move(joined);
    best_score = score;
  }
  return best;
}

std::string Phonemizer::WordAt(std::string_view word,
                               bool all_caps_sentence,
                               int depth) const {
  const std::string lower = LowerLetters(word.substr(0, kMaxWordLetters));
  if (lower.empty()) {
    return std::string();
  }
  size_t letters = 0;
  for (char c : lower) {
    letters += IsLower(c) ? 1 : 0;
  }
  // Acronyms: short words in capitals ("GPU", "HTML", "US") are spelled,
  // unless they are words written in capitals.
  if (AllCapitals(word) && letters >= 2 && !all_caps_sentence) {
    if (letters <= 3 && !IsCapitalWord(lower)) {
      return Spell(lower);
    }
    if (letters <= 5 && !lexicon_.Has(lower)) {
      return Spell(lower);
    }
  }
  std::string phones = Known(lower, depth);
  if (!phones.empty()) {
    return phones;
  }
  // Joined words written in camel case: "MoltBrowser", "iPhone".
  if (depth < 2) {
    std::string joined;
    size_t start = 0;
    bool split = false;
    for (size_t i = 1; i <= word.size(); ++i) {
      if (i < word.size() && !(IsUpperAscii(word[i]) && IsLower(word[i - 1]))) {
        continue;
      }
      if (i == word.size() && !split) {
        break;
      }
      const std::string part =
          WordAt(word.substr(start, i - start), all_caps_sentence, depth + 1);
      if (!part.empty()) {
        if (!joined.empty()) {
          joined.push_back(' ');
        }
        joined.append(part);
      }
      start = i;
      split = true;
    }
    if (split && !joined.empty()) {
      return joined;
    }
  }
  if (!HasVowelLetter(lower) || letters <= 2) {
    return Spell(lower);
  }
  return LetterToSound(lower);
}

void Phonemizer::AppendFiltered(std::string_view phones,
                                std::vector<int32_t>* out) const {
  std::vector<uint32_t> cps;
  if (!DecodeUtf8(phones, &cps)) {
    return;
  }
  for (uint32_t cp : cps) {
    if (tokens_.empty() ||
        std::binary_search(tokens_.begin(), tokens_.end(), cp)) {
      out->push_back(static_cast<int32_t>(cp));
    }
  }
}

std::vector<int32_t> Phonemizer::Sentence(
    const SpokenSentence& sentence) const {
  // A title in capitals ("OFFICIAL MUSIC VIDEO"): its words are words.
  size_t words = 0;
  size_t capitals = 0;
  for (const SpokenWord& w : sentence.words) {
    if (w.kind != WordKind::kWord || w.text.size() < 2) {
      continue;
    }
    ++words;
    capitals += AllCapitals(w.text) ? 1 : 0;
  }
  const bool all_caps = words >= 3 && capitals * 3 >= words * 2;

  std::vector<int32_t> out;
  for (const SpokenWord& w : sentence.words) {
    if (w.kind == WordKind::kPause) {
      if (!out.empty() && out.back() != ',') {
        AppendFiltered(",", &out);
      }
      continue;
    }
    const std::string phones =
        w.kind == WordKind::kSpell ? Spell(w.text) : Word(w.text, all_caps);
    if (phones.empty()) {
      continue;
    }
    std::vector<int32_t> word_cps;
    AppendFiltered(phones, &word_cps);
    if (word_cps.empty()) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(' ');
    }
    out.insert(out.end(), word_cps.begin(), word_cps.end());
  }
  if (!out.empty()) {
    if (out.back() == ',') {
      out.pop_back();
    }
    std::string mark(1, sentence.end);
    AppendFiltered(mark, &out);
  }
  return out;
}

// static
std::vector<std::vector<int32_t>> Phonemizer::Units(
    const std::vector<int32_t>& phonemes) {
  std::vector<std::vector<int32_t>> units;
  size_t start = 0;
  while (start < phonemes.size()) {
    if (phonemes.size() - start <= kMaxUnitCodePoints) {
      units.emplace_back(phonemes.begin() + static_cast<ptrdiff_t>(start),
                         phonemes.end());
      break;
    }
    const size_t limit = start + kMaxUnitCodePoints;
    size_t cut = 0;
    // After a pause in the second half, else at the last space.
    for (size_t i = limit; i > start + kMaxUnitCodePoints / 2; --i) {
      if (phonemes[i - 1] == ',') {
        cut = i;
        break;
      }
    }
    if (!cut) {
      for (size_t i = limit; i > start + 1; --i) {
        if (phonemes[i - 1] == ' ') {
          cut = i - 1;
          break;
        }
      }
    }
    if (!cut) {
      cut = limit;
    }
    std::vector<int32_t> unit(phonemes.begin() + static_cast<ptrdiff_t>(start),
                              phonemes.begin() + static_cast<ptrdiff_t>(cut));
    while (!unit.empty() && unit.back() == ' ') {
      unit.pop_back();
    }
    if (!unit.empty() && unit.back() != ',') {
      unit.push_back(',');
    }
    if (!unit.empty()) {
      units.push_back(std::move(unit));
    }
    start = cut;
    while (start < phonemes.size() && phonemes[start] == ' ') {
      ++start;
    }
  }
  return units;
}

}  // namespace molt_voice
