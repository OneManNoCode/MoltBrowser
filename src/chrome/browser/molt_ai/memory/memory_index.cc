// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_index.h"

#include <algorithm>
#include <cstring>

namespace molt_ai {
namespace memory {

MemoryIndex::MemoryIndex() {
  // Reserve up front so the first kHotIndexCap inserts don't reallocate
  // the embedding pool (which would invalidate contiguity assumptions
  // briefly and slow the hot path).
  embeddings_.reserve(kHotIndexCap * kEmbeddingDim);
  chunk_ids_.reserve(kHotIndexCap);
  doc_ids_.reserve(kHotIndexCap);
}

MemoryIndex::~MemoryIndex() = default;

void MemoryIndex::Add(int64_t chunk_id, int64_t doc_id,
                       const std::vector<float>& embedding) {
  if (embedding.size() != kEmbeddingDim) return;

  // FIFO eviction when full. We shift the oldest entry out — for the
  // workload (a few thousand insertions per active session) this is
  // cheap relative to the dot-product scan.
  if (chunk_ids_.size() >= kHotIndexCap) {
    // Drop row 0.
    embeddings_.erase(embeddings_.begin(),
                       embeddings_.begin() + kEmbeddingDim);
    chunk_ids_.erase(chunk_ids_.begin());
    doc_ids_.erase(doc_ids_.begin());
  }

  size_t old = embeddings_.size();
  embeddings_.resize(old + kEmbeddingDim);
  std::memcpy(embeddings_.data() + old, embedding.data(),
              kEmbeddingDim * sizeof(float));
  chunk_ids_.push_back(chunk_id);
  doc_ids_.push_back(doc_id);
}

void MemoryIndex::RemoveDoc(int64_t doc_id) {
  // Compact-in-place. Slow path (called on document delete), so the
  // O(n) shuffle is fine.
  size_t read = 0;
  size_t write = 0;
  while (read < chunk_ids_.size()) {
    if (doc_ids_[read] != doc_id) {
      if (write != read) {
        chunk_ids_[write] = chunk_ids_[read];
        doc_ids_[write] = doc_ids_[read];
        std::memcpy(embeddings_.data() + write * kEmbeddingDim,
                    embeddings_.data() + read * kEmbeddingDim,
                    kEmbeddingDim * sizeof(float));
      }
      ++write;
    }
    ++read;
  }
  chunk_ids_.resize(write);
  doc_ids_.resize(write);
  embeddings_.resize(write * kEmbeddingDim);
}

void MemoryIndex::Clear() {
  embeddings_.clear();
  chunk_ids_.clear();
  doc_ids_.clear();
}

std::vector<QueryHit> MemoryIndex::Query(const std::vector<float>& query,
                                           int top_k) {
  std::vector<QueryHit> out;
  if (query.size() != kEmbeddingDim || top_k <= 0 || chunk_ids_.empty())
    return out;

  // Brute-force dot product. The compiler can auto-vectorize this on
  // every supported platform; with -O2 + NEON/SSE/AVX it saturates the
  // load-store unit. For 50K rows × 256 dims that's 12.8M FMAs, well
  // under a millisecond on M-series and modern x86.
  const float* base = embeddings_.data();
  const float* q = query.data();
  const size_t n = chunk_ids_.size();

  // Keep a small top-k heap. For typical top_k = 5..20 this beats
  // computing all scores then sorting.
  std::vector<std::pair<float, size_t>> heap;  // (score, row)
  heap.reserve(top_k + 1);

  for (size_t row = 0; row < n; ++row) {
    const float* v = base + row * kEmbeddingDim;
    float s = 0.0f;
    for (size_t d = 0; d < kEmbeddingDim; ++d) {
      s += v[d] * q[d];
    }
    if (static_cast<int>(heap.size()) < top_k) {
      heap.emplace_back(s, row);
      std::push_heap(heap.begin(), heap.end(),
                     [](const auto& a, const auto& b) {
                       return a.first > b.first;  // min-heap on score
                     });
    } else if (s > heap.front().first) {
      std::pop_heap(heap.begin(), heap.end(),
                    [](const auto& a, const auto& b) {
                      return a.first > b.first;
                    });
      heap.back() = {s, row};
      std::push_heap(heap.begin(), heap.end(),
                     [](const auto& a, const auto& b) {
                       return a.first > b.first;
                     });
    }
  }

  // Drain heap into a sorted best-first vector.
  std::sort(heap.begin(), heap.end(),
            [](const auto& a, const auto& b) {
              return a.first > b.first;
            });
  out.reserve(heap.size());
  for (const auto& [score, row] : heap) {
    QueryHit h;
    h.chunk_id = chunk_ids_[row];
    h.doc_id = doc_ids_[row];
    h.score = score;
    out.push_back(std::move(h));
  }
  return out;
}

}  // namespace memory
}  // namespace molt_ai
