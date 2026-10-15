// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryEmbedder — turns a chunk of text into a kEmbeddingDim-float
// dense vector suitable for cosine similarity.
//
// Two implementations:
//
//   1. HashingEmbedder (default, sub-millisecond)
//      Uses the signed feature-hashing trick over case-folded words +
//      character bigrams (UTF-8 aware, see memory_embedder.cc).
//      Deterministic, no model dependency, ~30µs for a
//      typical 600-char chunk. Quality is approximately "fuzzy keyword
//      overlap" — much better than substring search, much worse than
//      a real transformer embedding, but the latency budget is the
//      hard constraint per user spec.
//
//   2. (Future) LLMEmbedder
//      Mean-pools the last hidden state of TinyLlama via
//      BrowserAIRuntime. Real semantic but ~100-300 ms per chunk on
//      CPU. Plug in later by inheriting from MemoryEmbedder and
//      swapping the factory in MemoryService.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_EMBEDDER_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_EMBEDDER_H_

#include <memory>
#include <string>
#include <vector>

namespace molt_ai {
namespace memory {

// Bumped whenever HashingEmbedder's output changes for the same input, so
// MemoryStorage knows stored vectors are stale and re-embeds them all from
// the (encrypted) chunk text when the DB is opened; vectors from two
// versions are never mixed in one index.
//   v2: uppercase ASCII is lowercased instead of being treated as a
//       separator.
//   v3: the tokenizer works on UTF-8 code points: Unicode spaces and
//       punctuation separate words, non-ASCII letters are case-folded,
//       character bigrams are taken over code points. ASCII-only text
//       embeds exactly as in v2.
inline constexpr int kHashingEmbedderVersion = 3;

class MemoryEmbedder {
 public:
  virtual ~MemoryEmbedder() = default;
  // Returns a length-kEmbeddingDim L2-normalized float vector. Always
  // succeeds (worst case returns a zero-norm vector for empty input).
  virtual std::vector<float> Embed(const std::string& text) const = 0;
};

// The fast deterministic default. Safe to instantiate freely; holds no
// state and reads no global resources.
class HashingEmbedder : public MemoryEmbedder {
 public:
  HashingEmbedder() = default;
  ~HashingEmbedder() override = default;

  std::vector<float> Embed(const std::string& text) const override;
};

// Convenience factory — every caller goes through this so the active
// embedder can be swapped in one place.
std::unique_ptr<MemoryEmbedder> CreateDefaultEmbedder();

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_EMBEDDER_H_
