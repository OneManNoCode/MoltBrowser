// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryIndex — in-RAM flat vector index optimized for sub-millisecond
// top-k cosine search over up to |capacity| chunks (kHotIndexCap by
// default).
//
// Layout: one contiguous std::vector<float> of up to
// (capacity * kEmbeddingDim) floats used as a ring buffer. Each row is one
// chunk's pre-L2-normalized embedding; row -> chunk_id / doc_id lookups are
// parallel arrays. The contiguous layout lets the dot-product loop hit
// prefetcher-friendly memory, so a 50K x 256 brute-force scan finishes in
// well under a millisecond on Apple-Silicon-class CPUs.
//
// Eviction: once the index holds |capacity| rows, each Add() overwrites the
// OLDEST row in O(1) (the previous implementation erased the front of the
// vectors — an O(n) memmove of ~50 MB on every insert once full). Evicted
// chunks are NOT deleted from MemoryStorage: they stay on disk (listed,
// deletable, subject to retention) but are no longer returned by semantic
// search. evicted_count() lets the UI say so honestly.
//
// Thread safety: not internally locked. The owning MemoryService confines
// every call to its worker sequence.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_INDEX_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_INDEX_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "chrome/browser/molt_ai/memory/memory_types.h"

namespace molt_ai {
namespace memory {

class MemoryIndex {
 public:
  explicit MemoryIndex(size_t capacity = kHotIndexCap);
  ~MemoryIndex();

  MemoryIndex(const MemoryIndex&) = delete;
  MemoryIndex& operator=(const MemoryIndex&) = delete;

  // Add one chunk to the hot tier. |embedding| must be length
  // kEmbeddingDim and already L2-normalized. If the index is at capacity,
  // the oldest entry is evicted (FIFO) in O(1).
  void Add(int64_t chunk_id, int64_t doc_id,
           const std::vector<float>& embedding);

  // Drop all entries belonging to |doc_id| (e.g. on document delete).
  // O(n); preserves the insertion order of the surviving rows so later
  // evictions still remove the true oldest entry.
  void RemoveDoc(int64_t doc_id);

  // Drop every entry whose doc_id satisfies |pred|. Same ordering
  // guarantee as RemoveDoc. Returns the number of rows removed.
  size_t RemoveDocsIf(const std::function<bool(int64_t doc_id)>& pred);

  // Drop everything (also resets evicted_count()).
  void Clear();

  // Returns the top-k chunks by cosine similarity to |query|, sorted
  // best-first. |query| must be length kEmbeddingDim and normalized.
  std::vector<QueryHit> Query(const std::vector<float>& query,
                              int top_k) const;

  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  // Number of chunks pushed out of the hot tier by FIFO eviction since
  // construction / the last Clear().
  size_t evicted_count() const { return evicted_; }

  // Chunk ids in insertion order (oldest first). Test/debug helper.
  std::vector<int64_t> ChunkIdsOldestFirst() const;

 private:
  // Physical row of the |logical|-th oldest entry.
  size_t PhysicalRow(size_t logical) const {
    return (head_ + logical) % capacity_;
  }

  // Parallel arrays, all |min(size_, capacity_)| rows long once grown.
  // Embeddings are packed contiguously so a single linear sweep computes
  // every score with good cache behavior. Storage grows on demand instead
  // of reserving capacity * kEmbeddingDim floats (~51 MB) up front.
  std::vector<float> embeddings_;
  std::vector<int64_t> chunk_ids_;
  std::vector<int64_t> doc_ids_;

  size_t capacity_;
  size_t head_ = 0;  // physical row of the oldest entry
  size_t size_ = 0;
  size_t evicted_ = 0;
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_INDEX_H_
