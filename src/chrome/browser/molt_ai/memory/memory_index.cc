// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_index.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace molt_ai {
namespace memory {

MemoryIndex::MemoryIndex(size_t capacity)
    : capacity_(std::max<size_t>(capacity, 1)) {}

MemoryIndex::~MemoryIndex() = default;

void MemoryIndex::Add(int64_t chunk_id, int64_t doc_id,
                      const std::vector<float>& embedding) {
  if (embedding.size() != kEmbeddingDim) return;

  if (size_ < capacity_) {
    // Still growing: append a row. head_ stays 0 until the first wrap.
    const size_t old = embeddings_.size();
    embeddings_.resize(old + kEmbeddingDim);
    std::memcpy(embeddings_.data() + old, embedding.data(),
                kEmbeddingDim * sizeof(float));
    chunk_ids_.push_back(chunk_id);
    doc_ids_.push_back(doc_id);
    ++size_;
    return;
  }

  // Full: overwrite the oldest row in place and advance the head. O(1).
  const size_t row = head_;
  std::memcpy(embeddings_.data() + row * kEmbeddingDim, embedding.data(),
              kEmbeddingDim * sizeof(float));
  chunk_ids_[row] = chunk_id;
  doc_ids_[row] = doc_id;
  head_ = (head_ + 1) % capacity_;
  ++evicted_;
}

void MemoryIndex::RemoveDoc(int64_t doc_id) {
  RemoveDocsIf([doc_id](int64_t d) { return d == doc_id; });
}

size_t MemoryIndex::RemoveDocsIf(
    const std::function<bool(int64_t doc_id)>& pred) {
  // Slow path (document delete / retention / domain forget): rebuild the
  // rows in logical (oldest-first) order without the removed docs, which
  // also un-rotates the ring so head_ returns to 0.
  std::vector<float> emb;
  std::vector<int64_t> cids;
  std::vector<int64_t> dids;
  emb.reserve(embeddings_.size());
  cids.reserve(size_);
  dids.reserve(size_);
  size_t removed = 0;
  for (size_t i = 0; i < size_; ++i) {
    const size_t row = PhysicalRow(i);
    if (pred(doc_ids_[row])) {
      ++removed;
      continue;
    }
    emb.insert(emb.end(), embeddings_.begin() + row * kEmbeddingDim,
               embeddings_.begin() + (row + 1) * kEmbeddingDim);
    cids.push_back(chunk_ids_[row]);
    dids.push_back(doc_ids_[row]);
  }
  if (removed == 0) return 0;
  embeddings_ = std::move(emb);
  chunk_ids_ = std::move(cids);
  doc_ids_ = std::move(dids);
  size_ = chunk_ids_.size();
  head_ = 0;
  return removed;
}

void MemoryIndex::Clear() {
  embeddings_.clear();
  embeddings_.shrink_to_fit();
  chunk_ids_.clear();
  chunk_ids_.shrink_to_fit();
  doc_ids_.clear();
  doc_ids_.shrink_to_fit();
  head_ = 0;
  size_ = 0;
  evicted_ = 0;
}

std::vector<int64_t> MemoryIndex::ChunkIdsOldestFirst() const {
  std::vector<int64_t> out;
  out.reserve(size_);
  for (size_t i = 0; i < size_; ++i) {
    out.push_back(chunk_ids_[PhysicalRow(i)]);
  }
  return out;
}

std::vector<QueryHit> MemoryIndex::Query(const std::vector<float>& query,
                                         int top_k) const {
  std::vector<QueryHit> out;
  if (query.size() != kEmbeddingDim || top_k <= 0 || size_ == 0)
    return out;

  // Brute-force dot product over the physical rows (order is irrelevant
  // for scoring). The compiler auto-vectorizes this on every supported
  // platform; for 50K rows x 256 dims that's 12.8M FMAs, well under a
  // millisecond on M-series and modern x86.
  const float* base = embeddings_.data();
  const float* q = query.data();
  const size_t n = size_;

  // Keep a small top-k min-heap. For typical top_k = 5..20 this beats
  // computing all scores then sorting.
  auto min_heap_cmp = [](const std::pair<float, size_t>& a,
                         const std::pair<float, size_t>& b) {
    return a.first > b.first;
  };
  std::vector<std::pair<float, size_t>> heap;  // (score, row)
  heap.reserve(static_cast<size_t>(top_k) + 1);

  for (size_t row = 0; row < n; ++row) {
    const float* v = base + row * kEmbeddingDim;
    float s = 0.0f;
    for (size_t d = 0; d < kEmbeddingDim; ++d) {
      s += v[d] * q[d];
    }
    if (heap.size() < static_cast<size_t>(top_k)) {
      heap.emplace_back(s, row);
      std::push_heap(heap.begin(), heap.end(), min_heap_cmp);
    } else if (s > heap.front().first) {
      std::pop_heap(heap.begin(), heap.end(), min_heap_cmp);
      heap.back() = {s, row};
      std::push_heap(heap.begin(), heap.end(), min_heap_cmp);
    }
  }

  // Drain heap into a sorted best-first vector.
  std::sort(heap.begin(), heap.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
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
