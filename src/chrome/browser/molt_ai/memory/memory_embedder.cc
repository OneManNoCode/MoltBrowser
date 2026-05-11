// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_embedder.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>

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

// Treat ASCII letters and digits as in-token; everything else as a
// separator. Lowercases in-place. The non-ASCII path is conservative
// (we keep multi-byte UTF-8 bytes together as parts of one token rather
// than try to do proper Unicode segmentation in the hot path).
inline bool IsWordByte(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         (c >= 0x80);  // keep utf-8 continuation bytes together
}

inline char ToLowerAscii(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
  return c;
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

}  // namespace

std::vector<float> HashingEmbedder::Embed(const std::string& text) const {
  std::vector<float> v(kEmbeddingDim, 0.0f);
  if (text.empty()) return v;

  // Lowercase + token-split pass with no allocation: walk the string
  // once, find [start,end) runs of word bytes, hash them.
  size_t n = text.size();
  size_t i = 0;
  // Reuse a small local buffer for the lowercased token so we don't
  // touch the heap.
  char tok[64];
  while (i < n) {
    while (i < n && !IsWordByte(static_cast<unsigned char>(text[i]))) ++i;
    size_t start = i;
    size_t toklen = 0;
    while (i < n && IsWordByte(static_cast<unsigned char>(text[i]))) {
      if (toklen < sizeof(tok)) {
        tok[toklen++] = ToLowerAscii(static_cast<unsigned char>(text[i]));
      }
      ++i;
    }
    if (toklen == 0) continue;

    // Word-level feature.
    HashToken(tok, toklen, &v, /*salt=*/1);

    // Character-bigram features ride along with a different salt so
    // the model can rescue queries that miss the exact word form
    // ("recipe" vs "recipes" share several bigrams).
    if (toklen >= 2) {
      for (size_t b = 0; b + 1 < toklen; ++b) {
        HashToken(tok + b, 2, &v, /*salt=*/3);
      }
    }

    // Word-pair (bigram of words). Capture phrase context. We
    // approximate by hashing the prefix of the previous token + this
    // one — but the previous-token state would require carrying a
    // buffer. Skipped for v1; word + char-bigram already gives a
    // reasonable separability for short queries.
    (void)start;
  }

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

}  // namespace memory
}  // namespace molt_ai
