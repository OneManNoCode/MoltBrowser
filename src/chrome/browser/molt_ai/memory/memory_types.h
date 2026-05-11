// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Personal Vector Memory — shared types used by the recorder, storage,
// embedder, index, and query path.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_TYPES_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace molt_ai {
namespace memory {

// Embedding dimensionality. Keep small so the hot in-RAM index stays
// cheap to brute-force-scan with SIMD. 256 floats = 1 KB per chunk.
// At 256, 50K chunks fit in 50 MB and brute-force cosine over the
// whole set takes well under 1 ms on Apple-Silicon-class CPUs.
constexpr size_t kEmbeddingDim = 256;

// Maximum number of in-memory chunks for the hot index. Older chunks
// stay in SQLite and are paged in only when explicitly searched.
constexpr size_t kHotIndexCap = 50000;

// Target chunk size in characters. Tuned for human-visible paragraphs
// rather than token boundaries — we don't have a real tokenizer in the
// hot path and a character cap is good enough at ~125-200 tokens.
constexpr size_t kChunkTargetChars = 600;
// Overlap between adjacent chunks (chars). Lets a query that lands on
// the boundary still hit both sides.
constexpr size_t kChunkOverlapChars = 100;

// One captured page.
struct Document {
  int64_t doc_id = 0;          // primary key from storage, 0 = not yet saved
  std::string url;             // full URL (plaintext; needed for indexing)
  std::string title;
  int64_t visited_at_unix = 0;
  int word_count = 0;
  // SHA-256 of the cleaned page text. Used to skip re-embedding when a
  // user revisits an unchanged page.
  std::vector<uint8_t> content_hash;
};

// One embedded slice of a document.
struct Chunk {
  int64_t chunk_id = 0;
  int64_t doc_id = 0;
  int chunk_idx = 0;           // 0-based position within the document
  std::string text;            // plaintext after extraction
  std::vector<float> embedding;  // length == kEmbeddingDim, L2-normalized
};

// Search hit returned by MemoryIndex::Query.
struct QueryHit {
  int64_t chunk_id = 0;
  int64_t doc_id = 0;
  float score = 0.0f;          // cosine similarity in [-1, 1]
  // Populated by the service after vector-search by looking up the
  // owning document so callers get one ready-to-render result struct.
  std::string url;
  std::string title;
  std::string snippet;
  int64_t visited_at_unix = 0;
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_TYPES_H_
