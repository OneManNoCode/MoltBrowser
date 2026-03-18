// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "sql/transaction.h"

namespace molt_ai {

// Embedding dimensions for MiniLM (all-MiniLM-L6-v2)
constexpr int kEmbeddingDimensions = 384;

struct MemoryEngine::Impl {
  std::string storage_path;
  bool initialized = false;

  // SQLite database for persistent storage
  std::unique_ptr<sql::Database> db;

  // In-memory cache for fast access (synced with SQLite)
  std::vector<MemoryEntry> cache;
  int64_t next_id = 1;

  bool InitDatabase(const std::string& db_path) {
    db = std::make_unique<sql::Database>(sql::DatabaseOptions{
        .exclusive_locking = false,
        .page_size = 4096,
        .cache_size = 128,
    });

    base::FilePath path(db_path);
    if (!db->Open(path)) {
      LOG(ERROR) << "[MoltAI] Failed to open memory database: " << db_path;
      return false;
    }

    // Create tables
    if (!db->Execute(
            "CREATE TABLE IF NOT EXISTS memories ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  type INTEGER NOT NULL,"
            "  content TEXT NOT NULL,"
            "  source_url TEXT DEFAULT '',"
            "  context TEXT DEFAULT '',"
            "  persona_id TEXT DEFAULT '',"
            "  timestamp INTEGER NOT NULL,"
            "  embedding BLOB"
            ")")) {
      LOG(ERROR) << "[MoltAI] Failed to create memories table";
      return false;
    }

    // Create indexes for efficient queries
    db->Execute(
        "CREATE INDEX IF NOT EXISTS idx_memories_type ON memories(type)");
    db->Execute(
        "CREATE INDEX IF NOT EXISTS idx_memories_timestamp "
        "ON memories(timestamp)");
    db->Execute(
        "CREATE INDEX IF NOT EXISTS idx_memories_url "
        "ON memories(source_url)");

    // Load existing entries into cache
    LoadFromDatabase();

    LOG(INFO) << "[MoltAI] Memory engine initialized with "
              << cache.size() << " entries";
    return true;
  }

  void LoadFromDatabase() {
    if (!db)
      return;

    sql::Statement stmt(db->GetUniqueStatement(
        "SELECT id, type, content, source_url, context, persona_id, "
        "timestamp, embedding FROM memories ORDER BY timestamp DESC"));

    cache.clear();
    while (stmt.Step()) {
      MemoryEntry entry;
      entry.id = stmt.ColumnInt64(0);
      entry.type = static_cast<MemoryType>(stmt.ColumnInt(1));
      entry.content = stmt.ColumnString(2);
      entry.source_url = stmt.ColumnString(3);
      entry.context = stmt.ColumnString(4);
      entry.persona_id = stmt.ColumnString(5);
      entry.timestamp = stmt.ColumnInt64(6);
      entry.relevance_score = 0.0f;

      // Deserialize embedding blob
      if (stmt.ColumnByteLength(7) > 0) {
        int blob_size = stmt.ColumnByteLength(7);
        int num_floats = blob_size / sizeof(float);
        entry.embedding.resize(num_floats);
        memcpy(entry.embedding.data(), stmt.ColumnBlob(7), blob_size);
      }

      if (entry.id >= next_id) {
        next_id = entry.id + 1;
      }

      cache.push_back(std::move(entry));
    }
  }

  int64_t InsertIntoDatabase(const MemoryEntry& entry) {
    if (!db)
      return -1;

    sql::Statement stmt(db->GetUniqueStatement(
        "INSERT INTO memories (type, content, source_url, context, "
        "persona_id, timestamp, embedding) VALUES (?, ?, ?, ?, ?, ?, ?)"));

    stmt.BindInt(0, static_cast<int>(entry.type));
    stmt.BindString(1, entry.content);
    stmt.BindString(2, entry.source_url);
    stmt.BindString(3, entry.context);
    stmt.BindString(4, entry.persona_id);
    stmt.BindInt64(5, entry.timestamp);

    if (!entry.embedding.empty()) {
      stmt.BindBlob(6, base::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(entry.embedding.data()),
          entry.embedding.size() * sizeof(float)));
    } else {
      stmt.BindNull(6);
    }

    if (!stmt.Run()) {
      LOG(ERROR) << "[MoltAI] Failed to insert memory entry";
      return -1;
    }

    return db->GetLastInsertRowId();
  }

  bool DeleteFromDatabase(int64_t id) {
    if (!db)
      return false;

    sql::Statement stmt(
        db->GetUniqueStatement("DELETE FROM memories WHERE id = ?"));
    stmt.BindInt64(0, id);
    return stmt.Run();
  }

  void DeleteByTypeFromDatabase(MemoryType type) {
    if (!db)
      return;

    sql::Statement stmt(
        db->GetUniqueStatement("DELETE FROM memories WHERE type = ?"));
    stmt.BindInt(0, static_cast<int>(type));
    stmt.Run();
  }
};

MemoryEngine::MemoryEngine() : impl_(std::make_unique<Impl>()) {}

MemoryEngine::~MemoryEngine() {
  Shutdown();
}

bool MemoryEngine::Initialize(const std::string& storage_path) {
  if (impl_->initialized)
    return true;

  impl_->storage_path = storage_path;

  // Create storage directory if needed
  base::CreateDirectory(base::FilePath(storage_path));

  // Initialize SQLite database
  std::string db_path = storage_path + "/memory.db";
  if (!impl_->InitDatabase(db_path)) {
    LOG(WARNING) << "[MoltAI] SQLite init failed — using in-memory only";
  }

  impl_->initialized = true;
  return true;
}

void MemoryEngine::Shutdown() {
  if (!impl_->initialized)
    return;

  if (impl_->db) {
    impl_->db->Close();
    impl_->db.reset();
  }

  impl_->initialized = false;
}

int64_t MemoryEngine::Store(const std::string& content,
                             MemoryType type,
                             const std::string& source_url,
                             const std::string& context,
                             const std::string& persona_id) {
  auto embedding = ComputeEmbedding(content);
  return StoreWithEmbedding(content, embedding, type, source_url);
}

int64_t MemoryEngine::StoreWithEmbedding(
    const std::string& content,
    const std::vector<float>& embedding,
    MemoryType type,
    const std::string& source_url) {
  MemoryEntry entry;
  entry.type = type;
  entry.content = content;
  entry.source_url = source_url;
  entry.embedding = embedding;
  entry.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  // Persist to SQLite
  int64_t db_id = impl_->InsertIntoDatabase(entry);
  if (db_id > 0) {
    entry.id = db_id;
  } else {
    entry.id = impl_->next_id++;
  }

  impl_->cache.push_back(entry);

  return entry.id;
}

std::vector<MemoryEntry> MemoryEngine::Search(
    const MemoryQuery& query) const {
  auto query_embedding = ComputeEmbedding(query.query_text);

  std::vector<MemoryEntry> results;

  for (const auto& entry : impl_->cache) {
    if (entry.type != query.type)
      continue;
    if (!query.persona_filter.empty() &&
        entry.persona_id != query.persona_filter)
      continue;
    if (query.time_after > 0 && entry.timestamp < query.time_after)
      continue;
    if (query.time_before > 0 && entry.timestamp > query.time_before)
      continue;

    float score = 0.0f;
    if (!entry.embedding.empty() && !query_embedding.empty()) {
      score = CosineSimilarity(query_embedding, entry.embedding);
    } else {
      // Fallback: keyword match when embeddings unavailable
      if (entry.content.find(query.query_text) != std::string::npos) {
        score = 0.7f;
      }
    }

    if (score >= query.min_relevance) {
      MemoryEntry result = entry;
      result.relevance_score = score;
      results.push_back(result);
    }
  }

  std::sort(results.begin(), results.end(),
            [](const MemoryEntry& a, const MemoryEntry& b) {
              return a.relevance_score > b.relevance_score;
            });

  if (static_cast<int>(results.size()) > query.max_results) {
    results.resize(query.max_results);
  }

  return results;
}

std::vector<MemoryEntry> MemoryEngine::GetRecent(MemoryType type,
                                                   int limit) const {
  std::vector<MemoryEntry> results;
  for (const auto& entry : impl_->cache) {
    if (entry.type == type) {
      results.push_back(entry);
    }
  }

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
  for (const auto& entry : impl_->cache) {
    if (entry.id == id)
      return entry;
  }
  return {};
}

std::vector<MemoryEntry> MemoryEngine::GetByURL(
    const std::string& url) const {
  std::vector<MemoryEntry> results;
  for (const auto& entry : impl_->cache) {
    if (entry.source_url == url) {
      results.push_back(entry);
    }
  }
  return results;
}

bool MemoryEngine::Delete(int64_t id) {
  impl_->DeleteFromDatabase(id);

  auto it = std::find_if(impl_->cache.begin(), impl_->cache.end(),
                          [id](const MemoryEntry& e) { return e.id == id; });
  if (it != impl_->cache.end()) {
    impl_->cache.erase(it);
    return true;
  }
  return false;
}

void MemoryEngine::ClearByType(MemoryType type) {
  impl_->DeleteByTypeFromDatabase(type);

  impl_->cache.erase(
      std::remove_if(impl_->cache.begin(), impl_->cache.end(),
                      [type](const MemoryEntry& e) { return e.type == type; }),
      impl_->cache.end());
}

void MemoryEngine::ClearSessionMemory() {
  ClearByType(MemoryType::SHORT_TERM);
}

int MemoryEngine::GetCount(MemoryType type) const {
  int count = 0;
  for (const auto& entry : impl_->cache) {
    if (entry.type == type)
      count++;
  }
  return count;
}

size_t MemoryEngine::GetStorageSize() const {
  size_t total = 0;
  for (const auto& entry : impl_->cache) {
    total += entry.content.size();
    total += entry.source_url.size();
    total += entry.context.size();
    total += entry.embedding.size() * sizeof(float);
  }
  return total;
}

std::vector<float> MemoryEngine::ComputeEmbedding(
    const std::string& text) const {
  // Hash-based embedding as a functional placeholder.
  // For true semantic search, integrate MiniLM (all-MiniLM-L6-v2) via
  // ONNX Runtime. This hash approach provides basic keyword matching
  // capability that persists across sessions via SQLite.
  std::vector<float> embedding(kEmbeddingDimensions, 0.0f);

  if (text.empty())
    return embedding;

  // Hash each word into a dimension and accumulate
  std::string word;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == ' ' || text[i] == '\n' ||
        text[i] == '\t') {
      if (!word.empty()) {
        size_t hash = std::hash<std::string>{}(word);
        int dim = hash % kEmbeddingDimensions;
        embedding[dim] += 1.0f;
        word.clear();
      }
    } else {
      word += std::tolower(text[i]);
    }
  }

  // L2 normalize
  float norm = 0.0f;
  for (float v : embedding)
    norm += v * v;
  if (norm > 0.0f) {
    norm = std::sqrt(norm);
    for (float& v : embedding)
      v /= norm;
  }

  return embedding;
}

float MemoryEngine::CosineSimilarity(const std::vector<float>& a,
                                      const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty())
    return 0.0f;

  float dot_product = 0.0f;
  float norm_a = 0.0f;
  float norm_b = 0.0f;

  for (size_t i = 0; i < a.size(); ++i) {
    dot_product += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }

  if (norm_a == 0.0f || norm_b == 0.0f)
    return 0.0f;

  return dot_product / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

}  // namespace molt_ai
