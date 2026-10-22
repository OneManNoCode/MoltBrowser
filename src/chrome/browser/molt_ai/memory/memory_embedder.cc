// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_embedder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include "chrome/browser/molt_ai/memory/memory_types.h"

namespace molt_ai {
namespace memory {

namespace {

// 64-bit FNV-1a — fast, good distribution, no external dep. We pair
// each token with two independent hashes: one selects the dimension,
// the second's low bit selects the sign. This is the standard signed
// hashing trick from the vowpal-wabbit / scikit-learn HashingVectorizer.
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

inline uint64_t FnvHash(const char* data, size_t len, uint64_t salt) {
  uint64_t h = kFnvOffset ^ salt;
  for (size_t i = 0; i < len; ++i) {
    h ^= static_cast<uint8_t>(data[i]);
    h *= kFnvPrime;
  }
  return h;
}

// Hash a single token into the accumulator. We mix in two perturbations
// to spread word features (offset 1) vs. character-bigram features
// (offset 3) so they don't cancel each other.
inline void HashToken(const char* tok, size_t len, std::vector<float>* out,
                       uint64_t salt) {
  if (len == 0) return;
  uint64_t h1 = FnvHash(tok, len, salt);
  uint64_t h2 = FnvHash(tok, len, salt ^ 0x9E3779B97F4A7C15ULL);
  size_t dim = static_cast<size_t>(h1 % kEmbeddingDim);
  float sign = (h2 & 1) ? 1.0f : -1.0f;
  (*out)[dim] += sign;
}

// ---- Tokenizer -----------------------------------------------------
//
// Text is split into words on UTF-8 code points, not bytes:
//   - ASCII letters and digits are word characters, other ASCII is a
//     separator (as before, so ASCII-only text embeds exactly as in v2);
//   - non-ASCII letters, marks and digits of every script are word
//     characters; common Unicode spaces, punctuation and symbols (NBSP,
//     dashes, curly quotes, ellipsis, CJK punctuation, emoji, ...) are
//     separators, so "naïve—café" is two words like "naïve café";
//   - soft hyphens, ZWJ/ZWNJ, variation selectors and BOMs are dropped
//     without splitting the word;
//   - case is folded (ASCII, Latin-1, Latin Extended-A/B, Latin Extended
//     Additional, Greek, Cyrillic, Armenian; fullwidth Latin letters and
//     digits fold to ASCII) so "ÄRGER" matches "ärger";
//   - a malformed UTF-8 byte stays in the word as that raw byte rather
//     than being dropped.

constexpr char32_t kMalformed = 0xFFFFFFFF;

// Decodes the UTF-8 sequence starting at text[i] into *len bytes. Returns
// kMalformed (with *len == 1) for anything that isn't a well-formed,
// shortest-form, non-surrogate sequence.
char32_t DecodeUtf8(const std::string& text, size_t i, size_t* len) {
  const unsigned char c0 = static_cast<unsigned char>(text[i]);
  *len = 1;
  if (c0 < 0x80) return c0;
  size_t n;
  char32_t cp;
  char32_t min;
  if ((c0 & 0xE0) == 0xC0) {
    n = 2;
    cp = c0 & 0x1F;
    min = 0x80;
  } else if ((c0 & 0xF0) == 0xE0) {
    n = 3;
    cp = c0 & 0x0F;
    min = 0x800;
  } else if ((c0 & 0xF8) == 0xF0) {
    n = 4;
    cp = c0 & 0x07;
    min = 0x10000;
  } else {
    return kMalformed;
  }
  if (n > text.size() - i) return kMalformed;
  for (size_t k = 1; k < n; ++k) {
    const unsigned char ck = static_cast<unsigned char>(text[i + k]);
    if ((ck & 0xC0) != 0x80) return kMalformed;
    cp = (cp << 6) | (ck & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    return kMalformed;
  }
  *len = n;
  return cp;
}

// Appends |cp| (a valid scalar value) to |out| as UTF-8; returns the length.
size_t EncodeUtf8(char32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

enum class CharClass { kSeparator, kWord, kIgnorable };

bool InRange(char32_t c, char32_t lo, char32_t hi) {
  return c >= lo && c <= hi;
}

CharClass Classify(char32_t c) {
  if (c < 0x80) {
    return (InRange(c, 'a', 'z') || InRange(c, 'A', 'Z') ||
            InRange(c, '0', '9'))
               ? CharClass::kWord
               : CharClass::kSeparator;
  }
  if (c <= 0xBF) {
    // C1 controls, NBSP and Latin-1 punctuation/symbols, except the
    // letters ª µ º and the soft hyphen.
    if (c == 0xAA || c == 0xB5 || c == 0xBA) return CharClass::kWord;
    if (c == 0xAD) return CharClass::kIgnorable;
    return CharClass::kSeparator;
  }
  if (c == 0xD7 || c == 0xF7) return CharClass::kSeparator;  // × ÷
  if (c == 0x200C || c == 0x200D || c == 0xFEFF ||
      InRange(c, 0xFE00, 0xFE0F) || InRange(c, 0xE0000, 0xE007F)) {
    return CharClass::kIgnorable;  // ZWNJ, ZWJ, BOM, variation selectors
  }
  if (InRange(c, 0x2000, 0x206F) ||   // General Punctuation
      InRange(c, 0x20A0, 0x20CF) ||   // Currency Symbols
      InRange(c, 0x2190, 0x2BFF) ||   // arrows, math, technical, shapes
      InRange(c, 0x2E00, 0x2E7F) ||   // Supplemental Punctuation
      InRange(c, 0x3000, 0x303F) ||   // CJK Symbols and Punctuation
      InRange(c, 0xFE10, 0xFE1F) ||   // Vertical Forms
      InRange(c, 0xFE30, 0xFE6F) ||   // CJK Compatibility / Small Forms
      InRange(c, 0xFF01, 0xFF0F) || InRange(c, 0xFF1A, 0xFF20) ||
      InRange(c, 0xFF3B, 0xFF40) || InRange(c, 0xFF5B, 0xFF65) ||
      InRange(c, 0xFFF0, 0xFFFF) ||   // Specials (U+FFFD)
      InRange(c, 0x1F000, 0x1FAFF)) {  // emoji and other pictographs
    return CharClass::kSeparator;
  }
  return CharClass::kWord;
}

// Simple (one-to-one) lowercase mapping for the scripts listed above.
char32_t FoldCase(char32_t c) {
  if (c < 0x80) return InRange(c, 'A', 'Z') ? c + 0x20 : c;
  if (c < 0x100) {
    return (InRange(c, 0xC0, 0xDE) && c != 0xD7) ? c + 0x20 : c;
  }
  if (c <= 0x17F) {  // Latin Extended-A
    if (c == 0x130) return 'i';  // İ
    if (c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c;
    if (c == 0x178) return 0xFF;  // Ÿ
    if (InRange(c, 0x139, 0x148) || InRange(c, 0x179, 0x17E)) {
      return (c & 1) ? c + 1 : c;  // odd code points are capitals here
    }
    return (c & 1) ? c : c + 1;
  }
  if (c <= 0x24F) {  // Latin Extended-B: the regular pairs only
    if (InRange(c, 0x1CD, 0x1DC)) return (c & 1) ? c + 1 : c;
    if (InRange(c, 0x1DE, 0x1EF) || InRange(c, 0x1F8, 0x21F) ||
        InRange(c, 0x222, 0x233)) {
      return (c & 1) ? c : c + 1;
    }
    return c;
  }
  if (c <= 0x3FF) {  // Greek
    if (c == 0x386) return 0x3AC;
    if (InRange(c, 0x388, 0x38A)) return c + 0x25;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 0x3F;
    if (InRange(c, 0x391, 0x3AB) && c != 0x3A2) return c + 0x20;
    if (c == 0x3C2) return 0x3C3;  // final sigma matches sigma
    return c;
  }
  if (c <= 0x52F) {  // Cyrillic + Supplement
    if (InRange(c, 0x400, 0x40F)) return c + 0x50;
    if (InRange(c, 0x410, 0x42F)) return c + 0x20;
    if (c == 0x4C0) return 0x4CF;
    if (InRange(c, 0x4C1, 0x4CE)) return (c & 1) ? c + 1 : c;
    if (InRange(c, 0x460, 0x481) || InRange(c, 0x48A, 0x4BF) ||
        InRange(c, 0x4D0, 0x52F)) {
      return (c & 1) ? c : c + 1;
    }
    return c;
  }
  if (InRange(c, 0x531, 0x556)) return c + 0x30;  // Armenian
  if (InRange(c, 0x1E00, 0x1EFF)) {  // Latin Extended Additional
    if (c == 0x1E9E) return 0xDF;  // capital sharp s
    if (InRange(c, 0x1E96, 0x1E9F)) return c;
    return (c & 1) ? c : c + 1;
  }
  // Fullwidth forms fold to ASCII so "ＮＡＳＡ" matches "nasa".
  if (InRange(c, 0xFF10, 0xFF19)) return c - 0xFF10 + '0';
  if (InRange(c, 0xFF21, 0xFF3A)) return c - 0xFF21 + 'a';
  if (InRange(c, 0xFF41, 0xFF5A)) return c - 0xFF41 + 'a';
  return c;
}

// Longest token prefix that is hashed, in bytes (as in v2).
constexpr size_t kMaxTokenBytes = 64;

// Accumulates one word as folded UTF-8 and remembers where each code point
// starts, so character bigrams are taken over code points, not bytes.
class TokenBuilder {
 public:
  // Appends |bytes| (one code point, or one malformed byte) unless the
  // token is full; once one doesn't fit, the rest of the word is skipped
  // so the token stays a prefix of the word.
  void Append(const char* bytes, size_t n) {
    if (full_ || len_ + n > kMaxTokenBytes) {
      full_ = true;
      return;
    }
    starts_[count_++] = static_cast<uint8_t>(len_);
    for (size_t k = 0; k < n; ++k) buf_[len_++] = bytes[k];
  }

  void Flush(std::vector<float>* v) {
    if (len_ > 0) {
      // Word-level feature.
      HashToken(buf_, len_, v, /*salt=*/1);
      // Character-bigram features ride along with a different salt so
      // the model can rescue queries that miss the exact word form
      // ("recipe" vs "recipes" share several bigrams).
      for (size_t k = 0; k + 1 < count_; ++k) {
        const size_t begin = starts_[k];
        const size_t end = k + 2 < count_ ? starts_[k + 2] : len_;
        HashToken(buf_ + begin, end - begin, v, /*salt=*/3);
      }
    }
    len_ = 0;
    count_ = 0;
    full_ = false;
  }

 private:
  char buf_[kMaxTokenBytes];
  uint8_t starts_[kMaxTokenBytes];
  size_t len_ = 0;
  size_t count_ = 0;
  bool full_ = false;
};

}  // namespace

std::vector<float> HashingEmbedder::Embed(const std::string& text) const {
  std::vector<float> v(kEmbeddingDim, 0.0f);
  if (text.empty()) return v;

  // One pass, no allocation: decode code points, fold case, and hash each
  // run of word characters as it ends.
  TokenBuilder token;
  size_t i = 0;
  while (i < text.size()) {
    size_t len = 1;
    const char32_t cp = DecodeUtf8(text, i, &len);
    if (cp == kMalformed) {
      token.Append(&text[i], 1);
    } else {
      switch (Classify(cp)) {
        case CharClass::kSeparator:
          token.Flush(&v);
          break;
        case CharClass::kWord: {
          char utf8[4];
          const size_t n = EncodeUtf8(FoldCase(cp), utf8);
          token.Append(utf8, n);
          break;
        }
        case CharClass::kIgnorable:
          break;
      }
    }
    i += len;
  }
  token.Flush(&v);

  // L2 normalize so cosine == dot product.
  double sq = 0.0;
  for (float x : v) sq += static_cast<double>(x) * x;
  if (sq > 0) {
    float inv = static_cast<float>(1.0 / std::sqrt(sq));
    for (float& x : v) x *= inv;
  }
  return v;
}

std::unique_ptr<MemoryEmbedder> CreateDefaultEmbedder() {
  return std::make_unique<HashingEmbedder>();
}

std::string EmbedderSpace::Key() const {
  return model + "/" + std::to_string(dimensions);
}

void MemoryEmbedder::EmbedBatch(std::vector<std::string> texts,
                                EmbedPurpose purpose,
                                BatchCallback done) const {
  EmbedBatchResult result;
  result.outcome = EmbedOutcome::kOk;
  result.vectors.reserve(texts.size());
  for (const std::string& text : texts) {
    result.vectors.push_back(Embed(text));
  }
  if (done) done(std::move(result));
}

void MemoryEmbedder::EmbedQueryIfReady(std::string text,
                                       BatchCallback done) const {
  std::vector<std::string> texts;
  texts.push_back(std::move(text));
  EmbedBatch(std::move(texts), EmbedPurpose::kQuery, std::move(done));
}

std::vector<std::string> FoldedWords(const std::string& text,
                                     size_t max_words) {
  std::vector<std::string> words;
  std::string word;
  bool full = false;
  auto flush = [&]() {
    if (!word.empty() && words.size() < max_words) {
      words.push_back(std::move(word));
    }
    word.clear();
    full = false;
  };
  // Appends one code point's (or malformed byte's) bytes, keeping the word a
  // prefix of at most kMaxTokenBytes, as TokenBuilder does.
  auto append = [&](const char* bytes, size_t n) {
    if (full || word.size() + n > kMaxTokenBytes) {
      full = true;
      return;
    }
    word.append(bytes, n);
  };
  size_t i = 0;
  while (i < text.size() && words.size() < max_words) {
    size_t len = 1;
    const char32_t cp = DecodeUtf8(text, i, &len);
    if (cp == kMalformed) {
      append(&text[i], 1);
    } else {
      switch (Classify(cp)) {
        case CharClass::kSeparator:
          flush();
          break;
        case CharClass::kWord: {
          char utf8[4];
          const size_t n = EncodeUtf8(FoldCase(cp), utf8);
          append(utf8, n);
          break;
        }
        case CharClass::kIgnorable:
          break;
      }
    }
    i += len;
  }
  flush();
  return words;
}

EmbedderSpace HashingEmbedder::space() const {
  return HashingEmbedderSpace();
}

EmbedderSpace HashingEmbedderSpace() {
  EmbedderSpace space;
  space.model = "hashing-v" + std::to_string(kHashingEmbedderVersion);
  space.dimensions = kEmbeddingDim;
  return space;
}

int HashingVersionOfSpaceKey(const std::string& key) {
  static constexpr char kPrefix[] = "hashing-v";
  constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
  if (key.compare(0, kPrefixLen, kPrefix) != 0) return 0;
  int version = 0;
  size_t i = kPrefixLen;
  for (; i < key.size() && key[i] >= '0' && key[i] <= '9'; ++i) {
    if (version > 100000) return 0;
    version = version * 10 + (key[i] - '0');
  }
  // Digits, then "/<dimensions>".
  if (i == kPrefixLen || i >= key.size() || key[i] != '/') return 0;
  return version;
}

}  // namespace memory
}  // namespace molt_ai
