// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_index.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "chrome/browser/molt_ai/memory/memory_types.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace memory {
namespace {

// Unit vector along axis |axis| (mod kEmbeddingDim).
std::vector<float> Axis(size_t axis) {
  std::vector<float> v(kEmbeddingDim, 0.0f);
  v[axis % kEmbeddingDim] = 1.0f;
  return v;
}

TEST(MemoryIndexTest, EmptyIndexReturnsNothing) {
  MemoryIndex index(4);
  EXPECT_TRUE(index.Query(Axis(0), 5).empty());
  EXPECT_EQ(index.size(), 0u);
}

TEST(MemoryIndexTest, RejectsWrongDimension) {
  MemoryIndex index(4);
  index.Add(1, 1, std::vector<float>(3, 1.0f));
  EXPECT_EQ(index.size(), 0u);
  EXPECT_TRUE(index.Query(std::vector<float>(3, 1.0f), 1).empty());
}

TEST(MemoryIndexTest, QueryReturnsBestFirst) {
  MemoryIndex index(8);
  index.Add(/*chunk_id=*/10, /*doc_id=*/1, Axis(0));
  index.Add(11, 2, Axis(1));
  std::vector<float> mixed(kEmbeddingDim, 0.0f);
  mixed[0] = 0.6f;
  mixed[1] = 0.8f;
  index.Add(12, 3, mixed);

  std::vector<QueryHit> hits = index.Query(Axis(1), 2);
  ASSERT_EQ(hits.size(), 2u);
  EXPECT_EQ(hits[0].chunk_id, 11);
  EXPECT_EQ(hits[0].doc_id, 2);
  EXPECT_FLOAT_EQ(hits[0].score, 1.0f);
  EXPECT_EQ(hits[1].chunk_id, 12);
  EXPECT_NEAR(hits[1].score, 0.8f, 1e-6f);
}

TEST(MemoryIndexTest, RingBufferEvictsOldestInOrder) {
  MemoryIndex index(3);
  for (int64_t i = 1; i <= 5; ++i) {
    index.Add(i, i, Axis(static_cast<size_t>(i)));
  }
  EXPECT_EQ(index.size(), 3u);
  EXPECT_EQ(index.evicted_count(), 2u);
  EXPECT_EQ(index.ChunkIdsOldestFirst(), (std::vector<int64_t>{3, 4, 5}));

  // Evicted chunks are no longer searchable; surviving ones are.
  EXPECT_LT(index.Query(Axis(1), 1)[0].score, 0.5f);
  EXPECT_EQ(index.Query(Axis(4), 1)[0].chunk_id, 4);
}

TEST(MemoryIndexTest, RemoveAfterWrapPreservesEvictionOrder) {
  MemoryIndex index(3);
  index.Add(1, 100, Axis(1));
  index.Add(2, 200, Axis(2));
  index.Add(3, 300, Axis(3));
  index.Add(4, 400, Axis(4));  // evicts 1; ring has wrapped
  ASSERT_EQ(index.ChunkIdsOldestFirst(), (std::vector<int64_t>{2, 3, 4}));

  index.RemoveDoc(300);
  EXPECT_EQ(index.ChunkIdsOldestFirst(), (std::vector<int64_t>{2, 4}));

  // Next two inserts: the first fills the hole, the second must evict the
  // true oldest survivor (2), not whatever sits at physical row 0.
  index.Add(5, 500, Axis(5));
  index.Add(6, 600, Axis(6));
  EXPECT_EQ(index.ChunkIdsOldestFirst(), (std::vector<int64_t>{4, 5, 6}));
}

TEST(MemoryIndexTest, RemoveDocDropsAllChunksOfThatDoc) {
  MemoryIndex index(10);
  index.Add(1, 7, Axis(1));
  index.Add(2, 8, Axis(2));
  index.Add(3, 7, Axis(3));
  index.RemoveDoc(7);
  EXPECT_EQ(index.ChunkIdsOldestFirst(), (std::vector<int64_t>{2}));
  for (const QueryHit& h : index.Query(Axis(3), 5)) {
    EXPECT_NE(h.doc_id, 7);
  }
}

TEST(MemoryIndexTest, RemoveDocsCountsRemovedRows) {
  MemoryIndex index(10);
  for (int64_t i = 1; i <= 6; ++i) {
    index.Add(i, i, Axis(static_cast<size_t>(i)));
  }
  EXPECT_EQ(index.RemoveDocs({2, 4, 6, 42}), 3u);
  EXPECT_EQ(index.ChunkIdsOldestFirst(), (std::vector<int64_t>{1, 3, 5}));
  EXPECT_EQ(index.RemoveDocs({}), 0u);
  EXPECT_EQ(index.RemoveDocs({99}), 0u);
}

TEST(MemoryIndexTest, ClearResetsEverything) {
  MemoryIndex index(2);
  index.Add(1, 1, Axis(1));
  index.Add(2, 2, Axis(2));
  index.Add(3, 3, Axis(3));
  index.Clear();
  EXPECT_EQ(index.size(), 0u);
  EXPECT_EQ(index.evicted_count(), 0u);
  index.Add(9, 9, Axis(9));
  EXPECT_EQ(index.ChunkIdsOldestFirst(), (std::vector<int64_t>{9}));
}

TEST(MemoryIndexTest, ManyInsertsPastCapacityStayBounded) {
  MemoryIndex index(64);
  for (int64_t i = 0; i < 10000; ++i) {
    index.Add(i, i / 10, Axis(static_cast<size_t>(i)));
  }
  EXPECT_EQ(index.size(), 64u);
  EXPECT_EQ(index.evicted_count(), 10000u - 64u);
  std::vector<int64_t> ids = index.ChunkIdsOldestFirst();
  EXPECT_EQ(ids.front(), 10000 - 64);
  EXPECT_EQ(ids.back(), 9999);
}

}  // namespace
}  // namespace memory
}  // namespace molt_ai
