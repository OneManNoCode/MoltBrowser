// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/speech_helper/speech_text.h"

#include <stdint.h>

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace molt_speech {

namespace {

constexpr size_t kMaxPieceBytes = 64;
constexpr size_t kMaxPieces = 100000;
constexpr size_t kMaxWordBytes = 64;

bool IsAsciiLetter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

bool IsSpaceOrTab(char c) {
  return c == ' ' || c == '\t';
}

// [-+]?[0-9]+(\.[0-9]+)? as a double, without the C library (no locale).
bool ParseDecimal(std::string_view text, double* out) {
  size_t i = 0;
  bool negative = false;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
    negative = text[i] == '-';
    ++i;
  }
  const size_t digits_start = i;
  double value = 0;
  while (i < text.size() && IsAsciiDigit(text[i])) {
    value = value * 10 + (text[i] - '0');
    ++i;
  }
  if (i == digits_start || i - digits_start > 12) {
    return false;
  }
  if (i < text.size() && text[i] == '.') {
    ++i;
    const size_t fraction_start = i;
    double scale = 0.1;
    while (i < text.size() && IsAsciiDigit(text[i])) {
      value += (text[i] - '0') * scale;
      scale /= 10;
      ++i;
    }
    if (i == fraction_start || i - fraction_start > 17) {
      return false;
    }
  }
  if (i != text.size()) {
    return false;
  }
  *out = negative ? -value : value;
  return true;
}

// Splits |text| into lines ('\n', an optional '\r' before it), calling
// |line_fn| on each until it returns false.
template <typename Fn>
bool ForEachLine(std::string_view text, Fn line_fn) {
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view line = text.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (!line_fn(line)) {
      return false;
    }
    pos = end + 1;
  }
  return true;
}

// The length of the well-formed UTF-8 sequence at the start of |text|, or
// 0 (Unicode's table 3-7).
size_t Utf8Length(std::string_view text, uint32_t* code_point) {
  const auto byte = [&text](size_t i) -> unsigned char {
    return i < text.size() ? static_cast<unsigned char>(text[i]) : 0;
  };
  const unsigned char b0 = byte(0);
  if (b0 < 0x80) {
    *code_point = b0;
    return text.empty() ? 0 : 1;
  }
  const auto in = [](unsigned char b, int lo, int hi) {
    return b >= lo && b <= hi;
  };
  if (b0 >= 0xC2 && b0 <= 0xDF && in(byte(1), 0x80, 0xBF)) {
    *code_point = ((b0 & 0x1Fu) << 6) | (byte(1) & 0x3Fu);
    return 2;
  }
  if (b0 >= 0xE0 && b0 <= 0xEF) {
    const int lo = b0 == 0xE0 ? 0xA0 : 0x80;
    const int hi = b0 == 0xED ? 0x9F : 0xBF;
    if (in(byte(1), lo, hi) && in(byte(2), 0x80, 0xBF)) {
      *code_point =
          ((b0 & 0x0Fu) << 12) | ((byte(1) & 0x3Fu) << 6) | (byte(2) & 0x3Fu);
      return 3;
    }
    return 0;
  }
  if (b0 >= 0xF0 && b0 <= 0xF4) {
    const int lo = b0 == 0xF0 ? 0x90 : 0x80;
    const int hi = b0 == 0xF4 ? 0x8F : 0xBF;
    if (in(byte(1), lo, hi) && in(byte(2), 0x80, 0xBF) &&
        in(byte(3), 0x80, 0xBF)) {
      *code_point = ((b0 & 0x07u) << 18) | ((byte(1) & 0x3Fu) << 12) |
                    ((byte(2) & 0x3Fu) << 6) | (byte(3) & 0x3Fu);
      return 4;
    }
  }
  return 0;
}

bool IsControl(uint32_t c) {
  return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029;
}

bool IsWhiteSpace(uint32_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B ||
         c == 0x0C || c == 0x85 || c == 0xA0 || c == 0x2028 || c == 0x2029;
}

// A cleaned hotword word may be used if sherpa-onnx can spell it: the
// word-start mark and each character are pieces (and tokens), so its
// split never needs <unk> and every piece maps to a token.
bool CanSpellHotword(std::string_view word,
                     const PieceVocab& vocab,
                     const TokenSet& tokens) {
  const auto known = [&vocab, &tokens](std::string_view piece) {
    return vocab.Contains(piece) && tokens.count(std::string(piece)) > 0;
  };
  if (!known(kWordStart)) {
    return false;
  }
  for (const char c : word) {
    if (!known(std::string_view(&c, 1))) {
      return false;
    }
  }
  std::vector<std::string> pieces;
  if (!vocab.Split(word, &pieces)) {
    return false;
  }
  for (const std::string& piece : pieces) {
    if (!known(piece)) {
      return false;
    }
  }
  return true;
}

}  // namespace

PieceVocab::PieceVocab() = default;
PieceVocab::PieceVocab(const PieceVocab&) = default;
PieceVocab& PieceVocab::operator=(const PieceVocab&) = default;
PieceVocab::~PieceVocab() = default;

bool PieceVocab::Load(std::string_view text, std::string* error) {
  scores_.clear();
  longest_ = 0;
  size_t line_number = 0;
  std::string problem;
  const bool ok = ForEachLine(text, [&](std::string_view line) {
    ++line_number;
    if (line.empty()) {
      return true;
    }
    size_t split = 0;
    while (split < line.size() && !IsSpaceOrTab(line[split])) {
      ++split;
    }
    std::string_view piece = line.substr(0, split);
    size_t score_start = split;
    while (score_start < line.size() && IsSpaceOrTab(line[score_start])) {
      ++score_start;
    }
    double score = 0;
    if (piece.empty() || piece.size() > kMaxPieceBytes ||
        score_start == split ||
        !ParseDecimal(line.substr(score_start), &score)) {
      problem = "malformed vocabulary line " + std::to_string(line_number);
      return false;
    }
    if (scores_.size() == kMaxPieces) {
      problem = "vocabulary has too many pieces";
      return false;
    }
    if (!scores_.emplace(std::string(piece), score).second) {
      problem = "vocabulary repeats a piece on line " +
                std::to_string(line_number);
      return false;
    }
    longest_ = std::max(longest_, piece.size());
    return true;
  });
  if (!ok) {
    scores_.clear();
    longest_ = 0;
    if (error) {
      *error = problem;
    }
    return false;
  }
  return true;
}

bool PieceVocab::Contains(std::string_view piece) const {
  return scores_.count(std::string(piece)) > 0;
}

bool PieceVocab::Split(std::string_view word,
                       std::vector<std::string>* pieces) const {
  pieces->clear();
  if (word.empty() || word.size() > kMaxWordBytes) {
    return false;
  }
  const std::string s = std::string(kWordStart) + std::string(word);
  const size_t n = s.size();
  // best[i]: the highest total score of a split of s[0, i), and where its
  // last piece starts. Starts are tried from the farthest, keeping the
  // first strictly better total, so ties go to the longer piece.
  constexpr double kNone = -std::numeric_limits<double>::infinity();
  std::vector<double> best(n + 1, kNone);
  std::vector<size_t> from(n + 1, 0);
  best[0] = 0;
  for (size_t i = 1; i <= n; ++i) {
    const size_t first = i > longest_ ? i - longest_ : 0;
    for (size_t j = first; j < i; ++j) {
      if (best[j] == kNone) {
        continue;
      }
      const auto it = scores_.find(s.substr(j, i - j));
      if (it == scores_.end()) {
        continue;
      }
      const double total = best[j] + it->second;
      if (best[i] == kNone || total > best[i]) {
        best[i] = total;
        from[i] = j;
      }
    }
  }
  if (best[n] == kNone) {
    return false;
  }
  std::vector<std::string> reversed;
  for (size_t i = n; i > 0; i = from[i]) {
    reversed.push_back(s.substr(from[i], i - from[i]));
  }
  pieces->assign(reversed.rbegin(), reversed.rend());
  return true;
}

bool LoadTokenSet(std::string_view text, TokenSet* tokens, std::string* error) {
  tokens->clear();
  size_t line_number = 0;
  std::string problem;
  const bool ok = ForEachLine(text, [&](std::string_view line) {
    ++line_number;
    if (line.empty()) {
      return true;
    }
    const size_t space = line.rfind(' ');
    if (space == std::string_view::npos || space == 0 ||
        space + 1 == line.size() || line.size() > 256) {
      problem = "malformed tokens line " + std::to_string(line_number);
      return false;
    }
    for (const char c : line.substr(space + 1)) {
      if (!IsAsciiDigit(c)) {
        problem = "malformed tokens line " + std::to_string(line_number);
        return false;
      }
    }
    if (tokens->size() == kMaxPieces) {
      problem = "too many tokens";
      return false;
    }
    tokens->insert(std::string(line.substr(0, space)));
    return true;
  });
  if (!ok) {
    tokens->clear();
    if (error) {
      *error = problem;
    }
  }
  return ok;
}

bool WakeKeywordLine(std::string_view phrase,
                     const PieceVocab& vocab,
                     const TokenSet& tokens,
                     std::string* line) {
  std::string out;
  std::string label;
  size_t pos = 0;
  while (pos < phrase.size()) {
    size_t end = phrase.find(' ', pos);
    if (end == std::string_view::npos) {
      end = phrase.size();
    }
    std::string word(phrase.substr(pos, end - pos));
    pos = end + 1;
    if (word.empty()) {
      continue;
    }
    for (char& c : word) {
      if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
      } else if (!IsAsciiLetter(c) && c != '\'') {
        return false;
      }
    }
    std::vector<std::string> pieces;
    if (!vocab.Split(word, &pieces)) {
      return false;
    }
    for (const std::string& piece : pieces) {
      if (tokens.count(piece) == 0) {
        return false;
      }
      if (!out.empty()) {
        out.push_back(' ');
      }
      out += piece;
    }
    if (!label.empty()) {
      label.push_back('_');
    }
    label += word;
  }
  if (out.empty()) {
    return false;
  }
  *line = out + " #" + std::string(kWakeThreshold) + " @" + label;
  return true;
}

std::string HotwordsArgument(const std::vector<std::string>& phrases,
                             const PieceVocab& vocab,
                             const TokenSet& tokens) {
  std::string result;
  std::set<std::string> seen;
  size_t count = 0;
  for (const std::string& raw : phrases) {
    if (count == kMaxHotwordPhrases) {
      break;
    }
    std::string phrase;
    size_t words = 0;
    size_t pos = 0;
    while (pos < raw.size() && words < kMaxHotwordWords) {
      // A word: a run of ASCII letters, digits and apostrophes.
      while (pos < raw.size() && !IsAsciiLetter(raw[pos]) &&
             !IsAsciiDigit(raw[pos]) && raw[pos] != '\'') {
        ++pos;
      }
      const size_t start = pos;
      while (pos < raw.size() && (IsAsciiLetter(raw[pos]) ||
                                  IsAsciiDigit(raw[pos]) || raw[pos] == '\'')) {
        ++pos;
      }
      const std::string_view word =
          std::string_view(raw).substr(start, pos - start);
      if (word.empty() ||
          std::none_of(word.begin(), word.end(),
                       [](char c) { return IsAsciiLetter(c) || IsAsciiDigit(c); }) ||
          !CanSpellHotword(word, vocab, tokens)) {
        continue;
      }
      const size_t grown = phrase.size() + (phrase.empty() ? 0 : 1) + word.size();
      if (grown > kMaxHotwordPhraseBytes) {
        break;
      }
      if (!phrase.empty()) {
        phrase.push_back(' ');
      }
      phrase.append(word);
      ++words;
    }
    if (phrase.empty() || !seen.insert(phrase).second) {
      continue;
    }
    const size_t grown = result.size() + (result.empty() ? 0 : 1) + phrase.size();
    if (grown > kMaxHotwordsBytes) {
      break;
    }
    if (!result.empty()) {
      result.push_back('/');
    }
    result += phrase;
    ++count;
  }
  return result;
}

std::string CleanEventText(std::string_view text, size_t max_bytes) {
  std::string out;
  bool pending_space = false;
  size_t pos = 0;
  while (pos < text.size()) {
    uint32_t c = 0;
    const size_t length = Utf8Length(text.substr(pos), &c);
    if (length == 0) {
      ++pos;  // Not well-formed UTF-8: dropped.
      continue;
    }
    const std::string_view sequence = text.substr(pos, length);
    pos += length;
    if (IsWhiteSpace(c)) {
      pending_space = !out.empty();
      continue;
    }
    if (IsControl(c)) {
      continue;
    }
    const size_t needed = sequence.size() + (pending_space ? 1 : 0);
    if (out.size() + needed > max_bytes) {
      // Cut at the last space in the final 64 bytes, if there is one.
      const size_t space = out.rfind(' ');
      if (space != std::string::npos && out.size() - space <= 64) {
        out.resize(space);
      }
      return out;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.append(sequence);
  }
  return out;
}

}  // namespace molt_speech
