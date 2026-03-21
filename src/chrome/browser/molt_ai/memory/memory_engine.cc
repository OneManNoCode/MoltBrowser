// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/values.h"

namespace molt_ai {

constexpr int kEmbeddingDimensions = 384;

struct MemoryEngine::Impl {
  std::string storage_path;
  bool initialized = false;
  std::vector<MemoryEntry> entries;
  int64_t next_id = 1;

  void SaveToFile() {
    if (storage_path.empty()) return;
    base::Value::List list;
    for (const auto& entry : entries) {
      base::Value::Dict d;
      d.Set("id", static_cast<int>(entry.id));
      d.Set("type", static_cast<int>(entry.type));
      d.Set("content", entry.content);
      d.Set("source_url", entry.source_url);
      d.Set("context", entry.context);
      d.Set("persona_id", entry.persona_id);
      d.Set("timestamp", static_cast<double>(entry.timestamp));
      list.Append(std::move(d));
    }
    std::string json;
    base::JSONWriter::WriteWithOptions(
        base::Value(std::move(list)),
        base::JSONWriter::OPTIONS_PRETTY_PRINT, &json);
    base::WriteFile(base::FilePath(storage_path + "/memory.json"), json);
  }

  void LoadFromFile() {
    base::FilePath path(storage_path + "/memory.json");
    std::string json;
    if (!base::ReadFileToString(path, &json)) return;
    auto parsed = base::JSONReader::Read(json);
    if (!parsed || !parsed->is_list()) return;
    entries.clear();
    for (const auto& item : parsed->GetList()) {
      if (!item.is_dict()) continue;
      const auto& d = item.GetDict();
      MemoryEntry entry;
      if (auto v = d.FindInt("id")) entry.id = *v;
      entry.type = static_cast<MemoryType>(d.FindInt("type").value_or(1));
      if (auto* v = d.FindString("content")) entry.content = *v;
      if (auto* v = d.FindString("source_url")) entry.source_url = *v;
      if (auto* v = d.FindString("context")) entry.context = *v;
      if (auto* v = d.FindString("persona_id")) entry.persona_id = *v;
      if (auto v = d.FindDouble("timestamp"))
        entry.timestamp = static_cast<int64_t>(*v);
      entry.relevance_score = 0.0f;
      if (entry.id >= next_id) next_id = entry.id + 1;
      entries.push_back(std::move(entry));
    }
    LOG(INFO) << "[MoltAI] Loaded " << entries.size() << " memory entries";
  }
};

MemoryEngine::MemoryEngine() : impl_(std::make_unique<Impl>()) {}
MemoryEngine::~MemoryEngine() { Shutdown(); }

bool MemoryEngine::Initialize(const std::string& storage_path) {
  if (impl_->initialized) return true;
  impl_->storage_path = storage_path;
  base::CreateDirectory(base::FilePath(storage_path));
  impl_->LoadFromFile();
  impl_->initialized = true;
  return true;
}

void MemoryEngine::Shutdown() {
  if (!impl_->initialized) return;
  impl_->SaveToFile();
  impl_->initialized = false;
}

int64_t MemoryEngine::Store(const std::string& content, MemoryType type,
                             const std::string& source_url,
                             const std::string& context,
                             const std::string& persona_id) {
  return StoreWithEmbedding(content, ComputeEmbedding(content), type, source_url);
}

int64_t MemoryEngine::StoreWithEmbedding(const std::string& content,
    const std::vector<float>& embedding, MemoryType type,
    const std::string& source_url) {
  MemoryEntry entry;
  entry.id = impl_->next_id++;
  entry.type = type;
  entry.content = content;
  entry.source_url = source_url;
  entry.embedding = embedding;
  entry.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  impl_->entries.push_back(entry);
  impl_->SaveToFile();
  return entry.id;
}

std::vector<MemoryEntry> MemoryEngine::Search(const MemoryQuery& query) const {
  auto qe = ComputeEmbedding(query.query_text);
  std::vector<MemoryEntry> results;
  for (const auto& e : impl_->entries) {
    if (e.type != query.type) continue;
    if (!query.persona_filter.empty() && e.persona_id != query.persona_filter) continue;
    if (query.time_after > 0 && e.timestamp < query.time_after) continue;
    if (query.time_before > 0 && e.timestamp > query.time_before) continue;
    float score = (!e.embedding.empty() && !qe.empty())
        ? CosineSimilarity(qe, e.embedding)
        : (e.content.find(query.query_text) != std::string::npos ? 0.7f : 0.0f);
    if (score >= query.min_relevance) {
      MemoryEntry r = e; r.relevance_score = score; results.push_back(r);
    }
  }
  std::sort(results.begin(), results.end(),
      [](const MemoryEntry& a, const MemoryEntry& b) { return a.relevance_score > b.relevance_score; });
  if (static_cast<int>(results.size()) > query.max_results) results.resize(query.max_results);
  return results;
}

std::vector<MemoryEntry> MemoryEngine::GetRecent(MemoryType type, int limit) const {
  std::vector<MemoryEntry> r;
  for (const auto& e : impl_->entries) if (e.type == type) r.push_back(e);
  std::sort(r.begin(), r.end(), [](const MemoryEntry& a, const MemoryEntry& b) { return a.timestamp > b.timestamp; });
  if (static_cast<int>(r.size()) > limit) r.resize(limit);
  return r;
}

MemoryEntry MemoryEngine::GetById(int64_t id) const {
  for (const auto& e : impl_->entries) if (e.id == id) return e;
  return {};
}

std::vector<MemoryEntry> MemoryEngine::GetByURL(const std::string& url) const {
  std::vector<MemoryEntry> r;
  for (const auto& e : impl_->entries) if (e.source_url == url) r.push_back(e);
  return r;
}

bool MemoryEngine::Delete(int64_t id) {
  auto it = std::find_if(impl_->entries.begin(), impl_->entries.end(),
      [id](const MemoryEntry& e) { return e.id == id; });
  if (it != impl_->entries.end()) { impl_->entries.erase(it); impl_->SaveToFile(); return true; }
  return false;
}

void MemoryEngine::ClearByType(MemoryType type) {
  impl_->entries.erase(std::remove_if(impl_->entries.begin(), impl_->entries.end(),
      [type](const MemoryEntry& e) { return e.type == type; }), impl_->entries.end());
  impl_->SaveToFile();
}

void MemoryEngine::ClearSessionMemory() { ClearByType(MemoryType::SHORT_TERM); }

int MemoryEngine::GetCount(MemoryType type) const {
  int c = 0; for (const auto& e : impl_->entries) if (e.type == type) c++;
  return c;
}

size_t MemoryEngine::GetStorageSize() const {
  size_t t = 0;
  for (const auto& e : impl_->entries)
    t += e.content.size() + e.source_url.size() + e.context.size() + e.embedding.size() * sizeof(float);
  return t;
}

std::vector<float> MemoryEngine::ComputeEmbedding(const std::string& text) const {
  std::vector<float> emb(kEmbeddingDimensions, 0.0f);
  if (text.empty()) return emb;
  std::string word;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == ' ' || text[i] == '\n' || text[i] == '\t') {
      if (!word.empty()) { emb[std::hash<std::string>{}(word) % kEmbeddingDimensions] += 1.0f; word.clear(); }
    } else { word += std::tolower(text[i]); }
  }
  float norm = 0.0f;
  for (float v : emb) norm += v * v;
  if (norm > 0.0f) { norm = std::sqrt(norm); for (float& v : emb) v /= norm; }
  return emb;
}

float MemoryEngine::CosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty()) return 0.0f;
  float dot = 0, na = 0, nb = 0;
  for (size_t i = 0; i < a.size(); ++i) { dot += a[i]*b[i]; na += a[i]*a[i]; nb += b[i]*b[i]; }
  return (na == 0 || nb == 0) ? 0.0f : dot / (std::sqrt(na) * std::sqrt(nb));
}

}  // namespace molt_ai
