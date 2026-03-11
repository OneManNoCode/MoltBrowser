// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef MOLT_AI_MEMORY_MEMORY_ENGINE_H_
#define MOLT_AI_MEMORY_MEMORY_ENGINE_H_

#include <memory>
#include <string>
#include <vector>

namespace molt_ai {

// Memory entry types
enum class MemoryType {
  SHORT_TERM,   // Session-scoped, cleared on browser close
  LONG_TERM,    // Persistent across sessions
  TASK_MEMORY   // Associated with a specific agent task
};

// Single memory entry
struct MemoryEntry {
  int64_t id;
  MemoryType type;
  std::string content;
  std::string source_url;
  std::string context;       // Additional context (e.g., page title)
  std::string persona_id;    // Associated persona
  int64_t timestamp;
  float relevance_score;     // Computed during retrieval
  std::vector<float> embedding;  // Vector embedding for semantic search
};

// Memory query options
struct MemoryQuery {
  std::string query_text;
  MemoryType type = MemoryType::LONG_TERM;
  int max_results = 10;
  float min_relevance = 0.5f;
  std::string persona_filter;  // Filter by persona
  int64_t time_after = 0;     // Filter by time range
  int64_t time_before = 0;
};

// ============================================================
// MemoryEngine — Persistent browsing intelligence
// ============================================================
// Memory layers (from V4 spec):
//   - Short-term session memory (cleared on close)
//   - Long-term user memory (persists across sessions)
//   - Task memory (associated with agent tasks)
//
// Storage architecture:
//   - SQLite for structured data
//   - Local vector database for embeddings
//   - Embedding model: MiniLM (all-MiniLM-L6-v2)
//
// Memory enables:
//   - Contextual browsing
//   - Personalized AI behavior
//   - Task continuation across sessions
// ============================================================
class MemoryEngine {
 public:
  MemoryEngine();
  ~MemoryEngine();

  // Initialize the memory engine with storage path
  bool Initialize(const std::string& storage_path);

  // Shutdown and flush pending writes
  void Shutdown();

  // ---- Store ----

  // Store a new memory entry
  int64_t Store(const std::string& content,
                MemoryType type,
                const std::string& source_url = "",
                const std::string& context = "",
                const std::string& persona_id = "");

  // Store with pre-computed embedding
  int64_t StoreWithEmbedding(const std::string& content,
                              const std::vector<float>& embedding,
                              MemoryType type,
                              const std::string& source_url = "");

  // ---- Retrieve ----

  // Semantic search: find memories similar to query
  std::vector<MemoryEntry> Search(const MemoryQuery& query) const;

  // Get recent memories (chronological)
  std::vector<MemoryEntry> GetRecent(MemoryType type, int limit = 10) const;

  // Get memory by ID
  MemoryEntry GetById(int64_t id) const;

  // Get all memories for a URL
  std::vector<MemoryEntry> GetByURL(const std::string& url) const;

  // ---- Manage ----

  // Delete a specific memory
  bool Delete(int64_t id);

  // Clear all memories of a given type
  void ClearByType(MemoryType type);

  // Clear all short-term memories (called on session end)
  void ClearSessionMemory();

  // Get total memory count
  int GetCount(MemoryType type = MemoryType::LONG_TERM) const;

  // Get total storage size in bytes
  size_t GetStorageSize() const;

  // ---- Embedding ----

  // Compute embedding for text using MiniLM
  std::vector<float> ComputeEmbedding(const std::string& text) const;

  // Compute cosine similarity between two embeddings
  static float CosineSimilarity(const std::vector<float>& a,
                                 const std::vector<float>& b);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace molt_ai

#endif  // MOLT_AI_MEMORY_MEMORY_ENGINE_H_
