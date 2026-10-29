// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/phonetic_key.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace molt_ai {
namespace voice {

namespace {

// Keys longer than this aren't compared (three joined 40-letter words
// stay under it).
constexpr size_t kMaxComparedKey = 160;

bool IsVowelLetter(char c) {
  return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

// Whether |word[i]| is heard as a vowel: a e i o u, and y unless it
// starts the word before a vowel ("yes").
bool SoundsVowel(std::string_view word, size_t i) {
  const char c = word[i];
  if (IsVowelLetter(c)) return true;
  if (c != 'y') return false;
  return !(i == 0 && i + 1 < word.size() && IsVowelLetter(word[i + 1]));
}

class KeyWriter {
 public:
  void Emit(char sound) {
    if (key_.empty() || key_.back() != sound) key_.push_back(sound);
  }
  bool HasVowel() const { return key_.find('a') != std::string::npos; }
  std::string Take() { return std::move(key_); }

 private:
  std::string key_;
};

int IndelCost(char sound) {
  return (sound == 'a' || sound == 'W' || sound == 'Y' || sound == 'H') ? 1
                                                                        : 2;
}

bool CloseSounds(char x, char y) {
  if (x > y) std::swap(x, y);
  return (x == 'M' && y == 'N') || (x == 'L' && y == 'R') ||
         (x == 'S' && y == 'X') || (x == 'K' && y == 'X') ||
         (x == 'K' && y == 'S') ||
         (x == 'S' && y == 'T') || (x == 'P' && y == 'W');
}

// A far substitution costs more than one edit: a recognizer that heard
// the word at all rarely swaps a sound for an unrelated one.
int SubstitutionCost(char x, char y) {
  if (x == y) return 0;
  return CloseSounds(x, y) ? 1 : 3;
}

}  // namespace

std::string PhoneticKey(std::string_view w) {
  if (w.empty() || w.size() > kMaxPhoneticWordBytes) return std::string();
  for (char c : w) {
    if (c < 'a' || c > 'z') return std::string();
  }
  const size_t n = w.size();
  const auto at = [&](size_t i) -> char { return i < n ? w[i] : '\0'; };
  KeyWriter key;
  size_t i = 0;

  // Clusters at the start of a word.
  const std::string_view start = w.substr(0, 2);
  if (start == "kn" || start == "gn" || start == "pn" || start == "wr" ||
      start == "ps") {
    i = 1;  // the first letter is silent
  } else if (start == "wh") {
    key.Emit('W');
    i = 2;
  } else if (start == "rh") {
    key.Emit('R');
    i = 2;
  } else if (w[0] == 'x') {
    key.Emit('S');  // "xylophone"
    i = 1;
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
        // A final e after a consonant is silent when the word has another
        // vowel ("live", "time"; not "the", "me").
        if (c == 'e' && i + 1 == n && i > 0 && !SoundsVowel(w, i - 1) &&
            key.HasVowel()) {
          ++i;
          break;
        }
        key.Emit('a');
        ++i;
        break;
      case 'y':
        key.Emit(SoundsVowel(w, i) ? 'a' : 'Y');
        ++i;
        break;
      case 'b':
        if (!(i + 1 == n && i > 0 && w[i - 1] == 'm')) key.Emit('P');  // climb
        ++i;
        break;
      case 'p':
        key.Emit('P');
        i += next == 'h' ? 2 : 1;
        break;
      case 'f':
      case 'v':
        key.Emit('P');
        ++i;
        break;
      case 't':
        if (next == 'c' && after == 'h') {
          key.Emit('X');  // "watch"
          i += 3;
        } else if (next == 'h') {
          key.Emit('T');
          i += 2;
        } else if (i > 0 && next == 'i' && (after == 'o' || after == 'a')) {
          key.Emit('X');  // "station", "partial"
          i += 2;
        } else {
          key.Emit('T');
          ++i;
        }
        break;
      case 'd':
        if (next == 'g' && (after == 'e' || after == 'i' || after == 'y')) {
          key.Emit('X');  // "edge", "judging"
          i += 2;
        } else {
          key.Emit('T');
          ++i;
        }
        break;
      case 'c':
        if (next == 'e' || next == 'i' || next == 'y') {
          key.Emit('S');
          ++i;
        } else if (next == 'h') {
          key.Emit(i == 0 && after == 'r' ? 'K' : 'X');  // "chris", "church"
          i += 2;
        } else if (next == 'k' || next == 'q') {
          key.Emit('K');
          i += 2;
        } else {
          key.Emit('K');
          ++i;
        }
        break;
      case 'k':
        key.Emit('K');
        ++i;
        break;
      case 'q':
        key.Emit('K');
        if (next == 'u') {
          key.Emit('W');  // "queen"
          i += 2;
        } else {
          ++i;
        }
        break;
      case 'g':
        if (next == 'h') {
          if (i == 0) key.Emit('K');  // "ghost"; silent inside ("night")
          i += 2;
        } else if (next == 'n' && i + 2 == n) {
          ++i;  // "sign"
        } else if (i + 1 == n && i > 0 && w[i - 1] == 'n') {
          ++i;  // "song": a final ng is one sound
        } else if (next == 'e' && i + 2 == n) {
          key.Emit('X');  // "page"
          ++i;
        } else {
          key.Emit('K');
          ++i;
        }
        break;
      case 'j':
        key.Emit('X');
        ++i;
        break;
      case 's':
        if (next == 'c' && after == 'h') {
          key.Emit('S');  // "school"
          key.Emit('K');
          i += 3;
        } else if (next == 'h') {
          key.Emit('X');
          i += 2;
        } else if (i > 0 && next == 'i' && (after == 'o' || after == 'a')) {
          key.Emit('X');  // "version"
          i += 2;
        } else {
          key.Emit('S');
          ++i;
        }
        break;
      case 'z':
        key.Emit('S');
        ++i;
        break;
      case 'x':
        key.Emit('K');
        key.Emit('S');
        ++i;
        break;
      case 'h':
        // Heard at the start of a syllable, silent after a vowel ("oh").
        if ((i == 0 || !SoundsVowel(w, i - 1)) &&
            (IsVowelLetter(next) || next == 'y')) {
          key.Emit('H');
        }
        ++i;
        break;
      case 'w':
        // A consonant before a vowel; part of the vowel otherwise ("cow").
        if (IsVowelLetter(next) || next == 'y') key.Emit('W');
        ++i;
        break;
      case 'l':
        key.Emit('L');
        ++i;
        break;
      case 'r':
        key.Emit('R');
        ++i;
        break;
      case 'm':
        key.Emit('M');
        ++i;
        break;
      case 'n':
        key.Emit('N');
        ++i;
        break;
      default:
        ++i;
        break;
    }
  }
  return key.Take();
}

std::string JoinPhoneticKeys(std::string_view a, std::string_view b) {
  std::string joined(a);
  if (!joined.empty() && !b.empty() && joined.back() == b.front()) {
    b.remove_prefix(1);
  }
  joined.append(b);
  return joined;
}

int PhoneticDistance(std::string_view a, std::string_view b, int bound) {
  if (bound < 0) bound = 0;
  bound = std::min(bound, 2 * kMaxPhoneticBand);
  const int too_far = bound + 1;
  if (a == b) return 0;
  if (a.size() > kMaxComparedKey || b.size() > kMaxComparedKey) return too_far;
  const int n = static_cast<int>(a.size());
  const int m = static_cast<int>(b.size());
  // Every insertion or deletion costs at least 1, so cells further than
  // |bound| from the diagonal cost more than the bound.
  const int band = std::min(bound, kMaxPhoneticBand);
  if (std::abs(n - m) > band) return too_far;

  std::array<int, kMaxComparedKey + 2> previous;
  std::array<int, kMaxComparedKey + 2> current;
  previous.fill(too_far);
  current.fill(too_far);
  previous[0] = 0;
  for (int j = 1; j <= std::min(m, band); ++j) {
    previous[j] = std::min(too_far, previous[j - 1] + IndelCost(b[j - 1]));
  }
  for (int i = 1; i <= n; ++i) {
    const int lo = std::max(0, i - band);
    const int hi = std::min(m, i + band);
    int row_best = too_far;
    if (lo == 0) {
      current[0] = std::min(too_far, previous[0] + IndelCost(a[i - 1]));
      row_best = current[0];
    } else {
      current[lo - 1] = too_far;
    }
    for (int j = std::max(1, lo); j <= hi; ++j) {
      int cost = previous[j - 1] + SubstitutionCost(a[i - 1], b[j - 1]);
      cost = std::min(cost, previous[j] + IndelCost(a[i - 1]));
      cost = std::min(cost, current[j - 1] + IndelCost(b[j - 1]));
      current[j] = std::min(cost, too_far);
      row_best = std::min(row_best, current[j]);
    }
    if (hi + 1 <= m) current[hi + 1] = too_far;
    if (row_best >= too_far) return too_far;
    std::swap(previous, current);
  }
  return std::min(previous[m], too_far);
}

int PhoneticBound(size_t longer_key_size) {
  if (longer_key_size <= 2) return 0;
  return static_cast<int>(std::min<size_t>(5, longer_key_size / 2));
}

double PhoneticSimilarity(std::string_view a, std::string_view b) {
  if (a.empty() || b.empty()) return 0;
  const size_t longer = std::max(a.size(), b.size());
  const int bound = PhoneticBound(longer);
  const int distance = PhoneticDistance(a, b, bound);
  if (distance > bound) return 0;
  // Short keys collide more ("live", "life" and "love" are all "LaP"), so
  // they are worth less even when equal.
  const double cap = longer <= 3 ? 0.7 : longer == 4 ? 0.8 : 0.9;
  // One half-edit in a key of L letters costs 1/L: a close sound in a
  // seven-letter key keeps 6/7 of the cap (0.77), a whole consonant edit
  // 5/7 (0.64).
  return cap * (1.0 - static_cast<double>(distance) /
                          static_cast<double>(longer));
}

}  // namespace voice
}  // namespace molt_ai
