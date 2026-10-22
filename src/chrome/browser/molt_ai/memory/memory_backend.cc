// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_backend.h"

#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

#include "base/logging.h"
#include "chrome/browser/molt_ai/memory/embedding_space.h"
#include "chrome/browser/molt_ai/memory/memory_extractor.h"
#include "chrome/browser/molt_ai/memory/memory_snippet.h"
#include "crypto/sha2.h"

namespace molt_ai {
namespace memory {

namespace {

// Between background calls, so the worker (and the model process) aren't
// monopolized.
constexpr int64_t kPumpDelayMs = 50;
// Retry delays after a failed or refused call.
constexpr int64_t kFirstRetryMs = 30 * 1000;
constexpr int64_t kMaxRetryMs = 10 * 60 * 1000;
// One-text calls a chunk may fail before it gets an all-zero vector.
constexpr int kFailuresToGiveUp = 2;
constexpr size_t kSnippetBytes = 240;
// Most hits looked at to find |top_k| distinct pages.
constexpr int kMaxCandidates = 200;

std::vector<uint8_t> Sha256(const std::string& s) {
  const std::string hash = crypto::SHA256HashString(s);
  return std::vector<uint8_t>(hash.begin(), hash.end());
}

const char* OutcomeName(EmbedOutcome outcome) {
  switch (outcome) {
    case EmbedOutcome::kOk:
      return "ok";
    case EmbedOutcome::kRetryLater:
      return "not now";
    case EmbedOutcome::kFailed:
      return "failed";
    case EmbedOutcome::kUnavailable:
      return "unavailable";
  }
  return "unknown";
}

}  // namespace

MemoryBackend::MemoryBackend(PostDelayedTask post_delayed)
    : post_delayed_(std::move(post_delayed)),
      hashing_(CreateDefaultEmbedder()) {}

MemoryBackend::~MemoryBackend() = default;

template <typename F>
auto MemoryBackend::Guarded(F f) {
  std::weak_ptr<bool> alive = alive_;
  return [alive, f = std::move(f)](auto&&... args) mutable {
    if (alive.expired()) return;
    f(std::forward<decltype(args)>(args)...);
  };
}

void MemoryBackend::Schedule(int64_t delay_ms, std::function<void()> task) {
  if (post_delayed_) post_delayed_(Guarded(std::move(task)), delay_ms);
}

void MemoryBackend::Start(const base::FilePath& dir) {
  open_ = storage_.Open(dir, *hashing_);
  if (!open_) return;
  ReloadIndex();
  LOG(INFO) << "[MoltMemory] index ready with " << index_.size()
            << " chunks";
  Reconcile();
}

void MemoryBackend::SetModelEmbedder(std::unique_ptr<MemoryEmbedder> embedder) {
  const std::string before = model_ ? model_->space().Key() : std::string();
  const std::string after = embedder ? embedder->space().Key() : std::string();
  model_ = std::move(embedder);
  model_known_ = true;
  if (before != after) {
    model_failed_ = false;
    retry_ms_ = 0;
  }
  Reconcile();
}

void MemoryBackend::SetBackgroundAllowed(bool allowed) {
  background_allowed_ = allowed;
  if (allowed) SchedulePump(0);
}

MemoryEmbedder* MemoryBackend::CommittedEmbedder() {
  const std::string& committed = storage_.committed_space();
  if (committed == hashing_->space().Key()) return hashing_.get();
  if (model_ && committed == model_->space().Key()) return model_.get();
  return nullptr;
}

void MemoryBackend::ReloadIndex() {
  index_.Clear();
  // The newest kHotIndexCap vectors only — a large DB is never fully
  // materialized in RAM. Chunks still waiting for a vector are skipped.
  std::vector<MemoryStorage::EmbeddingRow> rows;
  storage_.LoadNewestEmbeddings(index_.capacity(), &rows);
  for (auto& [chunk_id, doc_id, emb] : rows) {
    index_.Add(chunk_id, doc_id, emb);
  }
}

void MemoryBackend::Reconcile() {
  if (!open_ || !model_known_) return;
  const MemoryEmbedder& desired = model_ ? *model_ : *hashing_;
  switch (PlanEmbedderSpace(storage_.committed_space(), storage_.next_space(),
                            desired.space(), desired.is_synchronous())) {
    case SpaceAction::kNone:
    case SpaceAction::kContinueMigration:
      break;
    case SpaceAction::kAbortMigration:
      storage_.AbortMigration();
      migration_cursor_ = 0;
      break;
    case SpaceAction::kSwitchNow:
      if (storage_.ReembedAllAndCommit(desired)) {
        ReloadIndex();
        pending_cursor_ = migration_cursor_ = single_until_ = 0;
        failures_.clear();
        LOG(INFO) << "[MoltMemory] index re-embedded for "
                  << desired.space().Key();
      } else {
        LOG(ERROR) << "[MoltMemory] re-embedding for "
                   << desired.space().Key() << " failed";
      }
      break;
    case SpaceAction::kMigrate:
      if (storage_.BeginMigration(desired.space().Key())) {
        migration_cursor_ = single_until_ = 0;
        failures_.clear();
        LOG(INFO) << "[MoltMemory] building the index for "
                  << desired.space().Key() << " in the background";
      }
      break;
  }
  SchedulePump(0);
}

void MemoryBackend::SchedulePump(int64_t delay_ms) {
  if (pump_scheduled_) return;
  pump_scheduled_ = true;
  Schedule(delay_ms, [this]() {
    pump_scheduled_ = false;
    Pump();
  });
}

void MemoryBackend::Pump() {
  if (!open_ || busy_ || !background_allowed_) return;

  auto batch_size = [this](const MemoryEmbedder& e, int64_t cursor) {
    return cursor < single_until_ ? size_t{1}
                                  : std::max<size_t>(1, e.max_batch_size());
  };

  // 1. Chunks saved while the (asynchronous) committed embedder was busy.
  MemoryEmbedder* committed = CommittedEmbedder();
  if (committed && !committed->is_synchronous()) {
    std::vector<MemoryStorage::ChunkText> chunks;
    storage_.PendingChunks(pending_cursor_,
                           batch_size(*committed, pending_cursor_), &chunks);
    if (chunks.empty() && pending_cursor_ > 0) {
      pending_cursor_ = 0;  // a final pass for any skipped earlier
      storage_.PendingChunks(0, batch_size(*committed, 0), &chunks);
    }
    if (!chunks.empty()) {
      EmbedInBackground(committed, std::move(chunks), /*migration=*/false);
      return;
    }
  }

  // 2. The model's space, being built.
  if (model_ && !storage_.next_space().empty() &&
      storage_.next_space() == model_->space().Key()) {
    std::vector<MemoryStorage::ChunkText> chunks;
    storage_.ChunksMissingNextVector(
        migration_cursor_, batch_size(*model_, migration_cursor_), &chunks);
    if (chunks.empty() && migration_cursor_ > 0) {
      migration_cursor_ = 0;
      storage_.ChunksMissingNextVector(0, batch_size(*model_, 0), &chunks);
    }
    if (!chunks.empty()) {
      EmbedInBackground(model_.get(), std::move(chunks), /*migration=*/true);
      return;
    }
    // Every chunk has a vector in the model's space: switch to it.
    if (storage_.CommitMigration()) {
      ReloadIndex();
      failures_.clear();
      single_until_ = 0;
      LOG(INFO) << "[MoltMemory] searching with " << model_->space().Key();
    } else {
      LOG(ERROR) << "[MoltMemory] couldn't switch to the new index";
      retry_ms_ = std::min(kMaxRetryMs, std::max(kFirstRetryMs, retry_ms_ * 2));
      SchedulePump(retry_ms_);
    }
  }
}

void MemoryBackend::EmbedInBackground(
    MemoryEmbedder* embedder,
    std::vector<MemoryStorage::ChunkText> chunks,
    bool migration) {
  busy_ = true;
  std::vector<std::string> texts;
  std::vector<int64_t> chunk_ids;
  std::vector<int64_t> doc_ids;
  for (MemoryStorage::ChunkText& c : chunks) {
    texts.push_back(std::move(c.text));
    chunk_ids.push_back(c.chunk_id);
    doc_ids.push_back(c.doc_id);
  }
  const std::string space = embedder->space().Key();
  embedder->EmbedBatch(
      std::move(texts), EmbedPurpose::kDocument,
      Guarded([this, chunk_ids, doc_ids, space,
               migration](EmbedBatchResult result) {
        OnBackgroundEmbedded(chunk_ids, doc_ids, space, migration,
                             std::move(result));
      }));
}

bool MemoryBackend::StoreVectors(
    const std::vector<MemoryStorage::EmbeddingRow>& rows,
    const std::string& space,
    bool migration) {
  if (migration) {
    if (storage_.next_space() != space) return false;
    storage_.StoreNextVectors(rows);
    return true;
  }
  if (storage_.committed_space() != space) return false;
  for (const auto& [chunk_id, doc_id, v] : storage_.StorePendingVectors(rows)) {
    index_.Add(chunk_id, doc_id, v);
  }
  return true;
}

void MemoryBackend::OnBackgroundEmbedded(std::vector<int64_t> chunk_ids,
                                         std::vector<int64_t> doc_ids,
                                         const std::string& space,
                                         bool migration,
                                         EmbedBatchResult result) {
  busy_ = false;
  if (chunk_ids.empty()) return;
  int64_t& cursor = migration ? migration_cursor_ : pending_cursor_;

  bool ok = result.outcome == EmbedOutcome::kOk &&
            result.vectors.size() == chunk_ids.size();
  for (size_t i = 0; ok && i < result.vectors.size(); ++i) {
    ok = result.vectors[i].size() == kEmbeddingDim;
  }
  if (ok) {
    std::vector<MemoryStorage::EmbeddingRow> rows;
    for (size_t i = 0; i < chunk_ids.size(); ++i) {
      rows.emplace_back(chunk_ids[i], doc_ids[i],
                        std::move(result.vectors[i]));
      failures_.erase(chunk_ids[i]);
    }
    // Vectors for a space that is no longer wanted are dropped.
    if (StoreVectors(rows, space, migration)) cursor = chunk_ids.back();
    retry_ms_ = 0;
    SchedulePump(kPumpDelayMs);
    return;
  }

  if (result.outcome == EmbedOutcome::kUnavailable) {
    // The model can't run (file gone or invalid, unsupported machine, the
    // sandbox can't start): back to keyword hashing for this session.
    LOG(WARNING) << "[MoltMemory] the on-device embedding model is "
                    "unavailable; using keyword matching";
    model_.reset();
    model_failed_ = true;
    Reconcile();
    return;
  }

  if (result.outcome == EmbedOutcome::kFailed) {
    if (chunk_ids.size() > 1) {
      // Find the text to blame, one per call (the model process's own
      // crash back-off still applies).
      single_until_ = std::max(single_until_, chunk_ids.back());
      SchedulePump(kPumpDelayMs);
      return;
    }
    if (++failures_[chunk_ids[0]] >= kFailuresToGiveUp) {
      // It keeps failing on its own: an all-zero vector (never a match)
      // lets the rest of the index go on. Never logs the text.
      LOG(WARNING) << "[MoltMemory] a passage couldn't be embedded; it "
                      "won't be found by meaning";
      failures_.erase(chunk_ids[0]);
      std::vector<MemoryStorage::EmbeddingRow> rows;
      rows.emplace_back(chunk_ids[0], doc_ids[0],
                        std::vector<float>(kEmbeddingDim, 0.0f));
      if (StoreVectors(rows, space, migration)) cursor = chunk_ids[0];
      SchedulePump(kPumpDelayMs);
      return;
    }
  }
  retry_ms_ = std::min(kMaxRetryMs, std::max(kFirstRetryMs, retry_ms_ * 2));
  VLOG(1) << "[MoltMemory] background embedding: "
          << OutcomeName(result.outcome) << "; retrying in "
          << retry_ms_ / 1000 << " s";
  SchedulePump(retry_ms_);
}

void MemoryBackend::Ingest(const std::string& url,
                           std::string title,
                           const std::string& raw_text,
                           int64_t visited_at_unix) {
  if (!open_) return;
  const std::string normalized = NormalizePageText(raw_text);
  if (normalized.empty()) return;

  // Hash the normalized text so a revisit to an unchanged page is a no-op
  // (no re-embedding, no duplicate rows).
  std::vector<uint8_t> hash = Sha256(normalized);
  if (storage_.FindUnchangedDoc(url, hash)) return;

  // In-process embedders embed now; with the model committed the chunks
  // are stored pending and embedded in the background (the page text is
  // never held in RAM waiting for the model process).
  MemoryEmbedder* committed = CommittedEmbedder();
  const bool now = committed && committed->is_synchronous();
  std::vector<std::string> pieces = ChunkText(normalized);
  std::vector<Chunk> chunks;
  chunks.reserve(pieces.size());
  for (size_t i = 0; i < pieces.size(); ++i) {
    Chunk c;
    c.chunk_idx = static_cast<int>(i);
    c.text = std::move(pieces[i]);
    if (now) c.embedding = committed->Embed(c.text);
    chunks.push_back(std::move(c));
  }

  Document doc;
  doc.url = url;
  doc.title = std::move(title);
  doc.visited_at_unix = visited_at_unix;
  doc.word_count = CountWords(normalized);
  doc.content_hash = std::move(hash);
  if (!storage_.InsertDocumentWithChunks(&doc, &chunks)) return;
  if (now) {
    for (const Chunk& c : chunks) index_.Add(c.chunk_id, c.doc_id, c.embedding);
  } else {
    SchedulePump(0);
  }
}

void MemoryBackend::Query(const std::string& text,
                          int top_k,
                          bool one_per_page,
                          SearchCallback done) {
  if (!done) return;
  MemoryEmbedder* committed = open_ ? CommittedEmbedder() : nullptr;
  if (!committed || text.empty() || top_k <= 0) {
    SearchResult r;
    r.ok = open_ && committed;
    done(std::move(r));
    return;
  }
  const std::string space = committed->space().Key();
  const bool model = !committed->is_synchronous();
  committed->EmbedBatch(
      {text}, EmbedPurpose::kQuery,
      Guarded([this, text, top_k, one_per_page, space, model,
               done](EmbedBatchResult r) {
        SearchResult out;
        out.model = model;
        // The space may have changed while the model was busy: a vector
        // from another space is never compared with the index.
        if (r.outcome != EmbedOutcome::kOk || r.vectors.size() != 1 ||
            storage_.committed_space() != space) {
          out.ok = false;
          done(std::move(out));
          return;
        }
        out.hits = Search(r.vectors[0], text, top_k, one_per_page);
        done(std::move(out));
      }));
}

std::vector<QueryHit> MemoryBackend::Search(
    const std::vector<float>& query_vector,
    const std::string& query_text,
    int top_k,
    bool one_per_page) {
  const int want = one_per_page ? std::min(kMaxCandidates, top_k * 4) : top_k;
  std::vector<QueryHit> hits = index_.Query(query_vector, want);
  if (one_per_page) {
    std::set<int64_t> seen;
    std::vector<QueryHit> unique;
    for (QueryHit& h : hits) {
      if (!seen.insert(h.doc_id).second) continue;
      unique.push_back(std::move(h));
      if (static_cast<int>(unique.size()) >= top_k) break;
    }
    hits = std::move(unique);
  }
  for (auto& h : hits) {
    if (auto doc = storage_.GetDocument(h.doc_id)) {
      h.url = doc->url;
      h.title = doc->title;
      h.visited_at_unix = doc->visited_at_unix;
    }
    if (auto chunk = storage_.GetChunk(h.chunk_id)) {
      h.snippet = BestSnippet(chunk->text, query_text, kSnippetBytes);
    }
  }
  return hits;
}

MemoryStats MemoryBackend::GetStats() {
  MemoryStats s;
  if (!open_) return s;
  s.doc_count = storage_.CountDocuments();
  s.chunk_count = storage_.CountChunks();
  s.searchable_chunk_count = static_cast<int>(index_.size());
  return s;
}

SemanticIndexStatus MemoryBackend::GetSemanticStatus() {
  SemanticIndexStatus s;
  s.background_allowed = background_allowed_;
  s.model_failed = model_failed_;
  if (!open_) return s;
  s.space = storage_.committed_space();
  s.model_active = model_ && s.space == model_->space().Key();
  s.migrating = !storage_.next_space().empty();
  if (s.migrating) {
    s.migration_total = storage_.CountChunks();
    const int missing = storage_.CountChunksMissingNextVector();
    s.migration_done = missing >= 0 ? std::max(0, s.migration_total - missing)
                                    : 0;
  }
  if (s.space != hashing_->space().Key()) {
    s.pending = std::max(0, storage_.CountPendingChunks());
  }
  return s;
}

std::vector<Document> MemoryBackend::ListRecent(int limit) {
  return open_ ? storage_.ListRecentDocuments(limit) : std::vector<Document>();
}

bool MemoryBackend::DeleteDocument(int64_t doc_id) {
  index_.RemoveDoc(doc_id);
  return open_ && storage_.DeleteDocument(doc_id);
}

int MemoryBackend::DeleteByDomain(const std::string& domain) {
  if (!open_) return 0;
  std::vector<int64_t> ids = storage_.DeleteByDomain(domain);
  index_.RemoveDocs(ids);
  return static_cast<int>(ids.size());
}

void MemoryBackend::EnforceRetention(int64_t cutoff_unix) {
  if (!open_) return;
  std::vector<int64_t> ids = storage_.DeleteDocumentsVisitedBefore(cutoff_unix);
  if (ids.empty()) return;
  index_.RemoveDocs(ids);
  LOG(INFO) << "[MoltMemory] retention removed " << ids.size() << " pages";
}

bool MemoryBackend::ClearAll() {
  index_.Clear();
  pending_cursor_ = migration_cursor_ = single_until_ = 0;
  failures_.clear();
  return open_ && storage_.Clear();
}

}  // namespace memory
}  // namespace molt_ai
