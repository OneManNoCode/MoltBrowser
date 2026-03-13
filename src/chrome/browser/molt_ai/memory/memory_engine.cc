// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

// SQLite will be used for structured storage
// #include "third_party/sqlite/sqlite3.h"

namespace molt_ai {

// Embedding dimensions for MiniLM (all-MiniLM-L6-v2)
constexpr int kEmbeddingDimensions = 384;

struct MemoryEngine::Impl {
  std::string storage_path;
  bool initialized = false;

  // In-memory store (will be replaced with SQLite)
  std::vector<MemoryEntry> entries;
  int64_t next_id = 1;

  // SQLite handles (to be initialized)
  // sqlite3* db = nullptr;
};

MemoryEngine::MemoryEngine() : impl_(std::make_unique<Impl>()) {}

MemoryEngine::~MemoryEngine() {
  Shutdown();
}

bool MemoryEngine::Initialize(const std::string& storage_path) {
  if (impl_->initialized) return true;

  impl_->storage_path = storage_path;

  // TODO: Initialize SQLite database
  // sqlite3_open((storage_path + "/memory.db").c_str(), &impl_->db);
  //
  // Create tables:
  // CREATE TABLE IF NOT EXISTS memories (
  //   id INTEGER PRIMARY KEY AUTOINCREMENT,
  //   type INTEGER NOT NULL,
  //   content TEXT NOT NULL,
  //   source_url TEXT,
  //   context TEXT,
  //   persona_id TEXT,
  //   timestamp INTEGER NOT NULL,
  //   embedding BLOB
  // );
  //
  // CREATE INDEX IF NOT EXISTS idx_memories_type ON memories(type);
  // CREATE INDEX IF NOT EXISTS idx_memories_timestamp ON memories(timestamp);
  // CREATE INDEX IF NOT EXISTS idx_memories_url ON memories(source_url);

  impl_->initialized = true;
  return true;
}

void MemoryEngine::Shutdown() {
  if (!impl_->initialized) return;

  // TODO: Close SQLite
  // sqlite3_close(impl_->db);
  // impl_->db = nullptr;

  impl_->initialized = false;
}

int64_t MemoryEngine::Store(const std::string& content,
                             MemoryType type,
                             const std::string& source_url,
                             const std::string& context,
                             const std::string& persona_id) {
  // Compute embedding for semantic search
  auto embedding = ComputeEmbedding(content);
  return StoreWithEmbedding(content, embedding, type, source_url);
}

int64_t MemoryEngine::StoreWithEmbedding(
    const std::string& content,
    const std::vector<float>& embedding,
    MemoryType type,
    const std::string& source_url) {
  MemoryEntry entry;
  entry.id = impl_->next_id++;
  entry.type = type;
  entry.content = content;
  entry.source_url = source_url;
  entry.embedding = embedding;
  entry.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  // TODO: Insert into SQLite
  // For now, store in memory
  impl_->entries.push_back(entry);

  return entry.id;
}

std::vector<MemoryEntry> MemoryEngine::Search(
    const MemoryQuery& query) const {
  // Compute query embedding
  auto query_embedding = ComputeEmbedding(query.query_text);

  // Search by cosine similarity
  std::vector<MemoryEntry> results;

  for (const auto& entry : impl_->entries) {
    // Filter by type
    if (entry.type != query.type) continue;

    // Filter by persona
    if (!query.persona_filter.empty() &&
        entry.persona_id != query.persona_filter) {
      continue;
    }

    // Filter by time range
    if (query.time_after > 0 && entry.timestamp < query.time_after) continue;
    if (query.time_before > 0 && entry.timestamp > query.time_before) continue;

    // Compute relevance score
    float score = 0.0f;
    if (!entry.embedding.empty() && !query_embedding.empty()) {
      score = CosineSimilarity(query_embedding, entry.embedding);
    }

    if (score >= query.min_relevance) {
      MemoryEntry result = entry;
      result.relevance_score = score;
      results.push_back(result);
    }
  }

  // Sort by relevance (descending)
  std::sort(results.begin(), results.end(),
            [](const MemoryEntry& a, const MemoryEntry& b) {
              return a.relevance_score > b.relevance_score;
            });

  // Limit results
  if (static_cast<int>(results.size()) > query.max_results) {
    results.resize(query.max_results);
  }

  return results;
}

std::vector<MemoryEntry> MemoryEngine::GetRecent(MemoryType type,
                                                   int limit) const {
  std::vector<MemoryEntry> results;

  // Collect entries of the given type
  for (const auto& entry : impl_->entries) {
    if (entry.type == type) {
      results.push_back(entry);
    }
  }

  // Sort by timestamp (most recent first)
  std::sort(results.begin(), results.end(),
            [](const MemoryEntry& a, const MemoryEntry& b) {
              return a.timestamp > b.timestamp;
            });

  if (static_cast<int>(results.size()) > limit) {
    results.resize(limit);
  }

  return results;
}

MemoryEntry MemoryEngine::GetById(int64_t id) const {
  for (const auto& entry : impl_->entries) {
    if (entry.id == id) return entry;
  }
  return {};
}

std::vector<MemoryEntry> MemoryEngine::GetByURL(
    const std::string& url) const {
  std::vector<MemoryEntry> results;
  for (const auto& entry : impl_->entries) {
    if (entry.source_url == url) {
      results.push_back(entry);
    }
  }
  return results;
}

bool MemoryEngine::Delete(int64_t id) {
  auto it = std::find_if(impl_->entries.begin(), impl_->entries.end(),
                          [id](const MemoryEntry& e) { return e.id == id; });
  if (it != impl_->entries.end()) {
    impl_->entries.erase(it);
    return true;
  }
  return false;
}

void MemoryEngine::ClearByType(MemoryType type) {
  impl_->entries.erase(
      std::remove_if(impl_->entries.begin(), impl_->entries.end(),
                      [type](const MemoryEntry& e) { return e.type == type; }),
      impl_->entries.end());
}

void MemoryEngine::ClearSessionMemory() {
  ClearByType(MemoryType::SHORT_TERM);
}

int MemoryEngine::GetCount(MemoryType type) const {
  int count = 0;
  for (const auto& entry : impl_->entries) {
    if (entry.type == type) count++;
  }
  return count;
}

size_t MemoryEngine::GetStorageSize() const {
  size_t total = 0;
  for (const auto& entry : impl_->entries) {
    total += entry.content.size();
    total += entry.source_url.size();
    total += entry.context.size();
    total += entry.embedding.size() * sizeof(float);
  }
  return total;
}

std::vector<float> MemoryEngine::ComputeEmbedding(
    const std::string& text) const {
  // TODO: Integrate MiniLM (all-MiniLM-L6-v2) via ONNX Runtime
  // For now, return a placeholder zero vector
  return std::vector<float>(kEmbeddingDimensions, 0.0f);
}

float MemoryEngine::CosineSimilarity(const std::vector<float>& a,
                                      const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty()) return 0.0f;

  float dot_product = 0.0f;
  float norm_a = 0.0f;
  float norm_b = 0.0f;

  for (size_t i = 0; i < a.size(); ++i) {
    dot_product += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }

  if (norm_a == 0.0f || norm_b == 0.0f) return 0.0f;

  return dot_product / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

}  // namespace molt_ai
