// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryEmbedder — turns a chunk of text into a kEmbeddingDim-float
// dense vector suitable for cosine similarity.
//
// Two implementations:
//
//   1. HashingEmbedder (the fallback, sub-millisecond, synchronous)
//      Uses the signed feature-hashing trick over case-folded words +
//      character bigrams (UTF-8 aware, see memory_embedder.cc).
//      Deterministic, no model dependency, ~30µs for a
//      typical 600-char chunk. Quality is approximately "fuzzy keyword
//      overlap" — much better than substring search, much worse than
//      a real transformer embedding.
//
//   2. LlamaEmbedder (llama_embedder.h, asynchronous)
//      A small on-device embedding model run by the sandboxed inference
//      service (docs/design/SEMANTIC_SEARCH.md): real "search by
//      meaning". Used once the user has downloaded the (pinned) model;
//      HashingEmbedder stays the fallback.
//
// Vectors of different embedders live in different spaces and are never
// compared, stored or indexed together: each embedder names its space
// (EmbedderSpace), MemoryStorage records the space of its vectors and
// re-embeds from the encrypted chunk text when the space changes.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_EMBEDDER_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_EMBEDDER_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace molt_ai {
namespace memory {

// The vector space an embedder produces: its model (with whatever version
// changes its output) and the vector length.
struct EmbedderSpace {
  // "hashing-v3", "qwen3-embedding-0.6b@1a2b3c4d5e6f", ...
  std::string model;
  size_t dimensions = 0;

  // "<model>/<dimensions>", as MemoryStorage records it.
  std::string Key() const;
  bool operator==(const EmbedderSpace& other) const = default;
};

// What a text is embedded as (some models embed queries with an
// instruction and documents without one).
enum class EmbedPurpose { kDocument, kQuery };

// How an EmbedBatch() call ended.
enum class EmbedOutcome {
  kOk,
  // Not now (busy, short of memory, backing off after a crash): retry
  // later; the texts are not to blame.
  kRetryLater,
  // The call failed (the model process crashed, misbehaved or timed out):
  // retry later; a text that keeps failing alone may be to blame.
  kFailed,
  // The model can't run (missing or invalid file, unsupported machine):
  // don't use this embedder any more.
  kUnavailable,
};

struct EmbedBatchResult {
  EmbedOutcome outcome = EmbedOutcome::kFailed;
  // With kOk: one vector of space().dimensions per text, in order.
  std::vector<std::vector<float>> vectors;
};

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
  // Runs on the sequence that called EmbedBatch().
  using BatchCallback = std::function<void(EmbedBatchResult)>;

  virtual ~MemoryEmbedder() = default;

  virtual EmbedderSpace space() const = 0;

  // In-process embedders answer Embed() synchronously; the on-device model
  // only answers EmbedBatch(), asynchronously.
  virtual bool is_synchronous() const { return true; }

  // Returns a length-kEmbeddingDim L2-normalized float vector. Always
  // succeeds (worst case returns a zero-norm vector for empty input).
  // Synchronous embedders only; others return an empty vector.
  virtual std::vector<float> Embed(const std::string& text) const = 0;

  // One vector per text. Synchronous embedders run |done| before
  // returning; the default implementation calls Embed() for each text.
  virtual void EmbedBatch(std::vector<std::string> texts,
                          EmbedPurpose purpose,
                          BatchCallback done) const;

  // Most texts one EmbedBatch() call should carry.
  virtual size_t max_batch_size() const { return 64; }
};

// The fast deterministic fallback. Safe to instantiate freely; holds no
// state and reads no global resources. Queries and documents embed alike.
class HashingEmbedder : public MemoryEmbedder {
 public:
  HashingEmbedder() = default;
  ~HashingEmbedder() override = default;

  EmbedderSpace space() const override;
  std::vector<float> Embed(const std::string& text) const override;
};

// HashingEmbedder's space at kHashingEmbedderVersion: "hashing-v3/256".
EmbedderSpace HashingEmbedderSpace();
// The HashingEmbedder version of a "hashing-v<N>/<dims>" key; 0 for any
// other key.
int HashingVersionOfSpaceKey(const std::string& key);

// Convenience factory — every caller goes through this so the active
// embedder can be swapped in one place.
std::unique_ptr<MemoryEmbedder> CreateDefaultEmbedder();

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_EMBEDDER_H_
