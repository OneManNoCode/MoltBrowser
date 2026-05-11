// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryIndex — in-RAM flat vector index optimized for sub-millisecond
// top-k cosine search over up to kHotIndexCap chunks.
//
// Layout: one contiguous std::vector<float> of length
// (kHotIndexCap * kEmbeddingDim). Each row is one chunk's pre-L2-
// normalized embedding. Lookup by row index → chunk_id is maintained
// in parallel arrays. The contiguous layout is critical: it lets the
// dot-product loop hit prefetcher-friendly memory and saturate cache
// bandwidth, which on Apple Silicon means a 50K×256 brute-force scan
// finishes in well under a millisecond.
//
// Thread safety: not internally locked. The owning MemoryService
// serializes Add / Remove / Query on its own task runner.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_INDEX_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_INDEX_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "chrome/browser/molt_ai/memory/memory_types.h"

namespace molt_ai {
namespace memory {

class MemoryIndex {
 public:
  MemoryIndex();
  ~MemoryIndex();

  MemoryIndex(const MemoryIndex&) = delete;
  MemoryIndex& operator=(const MemoryIndex&) = delete;

  // Add one chunk to the hot tier. |embedding| must be length
  // kEmbeddingDim and already L2-normalized. If the index is at
  // kHotIndexCap, the oldest entry is evicted (FIFO).
  void Add(int64_t chunk_id, int64_t doc_id,
           const std::vector<float>& embedding);

  // Drop all entries belonging to |doc_id| (e.g. on document delete).
  void RemoveDoc(int64_t doc_id);

  // Drop everything.
  void Clear();

  // Returns the top-k chunks by cosine similarity to |query|, sorted
  // best-first. |query| must be length kEmbeddingDim and normalized.
  std::vector<QueryHit> Query(const std::vector<float>& query, int top_k);

  size_t size() const { return chunk_ids_.size(); }

 private:
  // Parallel arrays, all the same length. Embeddings are packed
  // contiguously so a single linear sweep computes every score with
  // good cache behavior.
  std::vector<float> embeddings_;
  std::vector<int64_t> chunk_ids_;
  std::vector<int64_t> doc_ids_;
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_INDEX_H_
