// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_service.h"

#include <algorithm>
#include <ctime>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/memory/memory_embedder.h"
#include "chrome/browser/molt_ai/memory/memory_extractor.h"
#include "chrome/browser/molt_ai/memory/memory_index.h"
#include "chrome/browser/molt_ai/memory/memory_prefs.h"
#include "chrome/browser/molt_ai/memory/memory_storage.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "crypto/sha2.h"
#include "url/gurl.h"

namespace molt_ai {
namespace memory {

namespace {

// How often the retention sweep runs while the browser stays open.
constexpr base::TimeDelta kRetentionSweepInterval = base::Hours(6);

// Build a single contiguous byte vector from the SHA-256 of a string.
std::vector<uint8_t> Sha256(const std::string& s) {
  std::string hash = crypto::SHA256HashString(s);
  return std::vector<uint8_t>(hash.begin(), hash.end());
}

}  // namespace

// Everything here runs on MemoryService::worker_ only.
struct MemoryService::Backend {
  Backend() : embedder(CreateDefaultEmbedder()) {}

  void Start(const base::FilePath& dir) {
    open = storage.Open(dir, *embedder);
    if (!open) return;
    // Hydrate the hot index with the newest kHotIndexCap vectors only — a
    // large DB is never fully materialized in RAM.
    std::vector<MemoryStorage::EmbeddingRow> rows;
    storage.LoadNewestEmbeddings(index.capacity(), &rows);
    for (auto& [chunk_id, doc_id, emb] : rows) {
      index.Add(chunk_id, doc_id, emb);
    }
    LOG(INFO) << "[MoltMemory] index ready with " << index.size()
              << " chunks";
  }

  void Ingest(const GURL& url, std::string title, std::string raw_text) {
    if (!open) return;
    std::string normalized = NormalizePageText(raw_text);
    if (normalized.empty()) return;

    // Hash the normalized text so a revisit to an unchanged page is a
    // no-op (no re-embedding, no duplicate rows).
    std::vector<uint8_t> hash = Sha256(normalized);
    const std::string url_str = url.spec();
    if (storage.FindUnchangedDoc(url_str, hash)) return;

    std::vector<std::string> pieces = ChunkText(normalized);
    std::vector<Chunk> chunks;
    chunks.reserve(pieces.size());
    for (size_t i = 0; i < pieces.size(); ++i) {
      Chunk c;
      c.chunk_idx = static_cast<int>(i);
      c.text = std::move(pieces[i]);
      c.embedding = embedder->Embed(c.text);
      chunks.push_back(std::move(c));
    }

    Document doc;
    doc.url = url_str;
    doc.title = std::move(title);
    doc.visited_at_unix = static_cast<int64_t>(std::time(nullptr));
    doc.word_count = CountWords(normalized);
    doc.content_hash = std::move(hash);
    if (!storage.InsertDocumentWithChunks(&doc, &chunks)) return;
    for (const Chunk& c : chunks) {
      index.Add(c.chunk_id, c.doc_id, c.embedding);
    }
  }

  std::vector<QueryHit> Query(const std::string& text, int top_k) {
    if (!open) return {};
    std::vector<QueryHit> hits = index.Query(embedder->Embed(text), top_k);
    for (auto& h : hits) {
      if (auto doc = storage.GetDocument(h.doc_id)) {
        h.url = doc->url;
        h.title = doc->title;
        h.visited_at_unix = doc->visited_at_unix;
      }
      if (auto chunk = storage.GetChunk(h.chunk_id)) {
        // Snippet: first ~240 chars of the matched chunk.
        h.snippet = chunk->text.substr(
            0, std::min<size_t>(240, chunk->text.size()));
      }
    }
    return hits;
  }

  Stats GetStats() {
    Stats s;
    if (!open) return s;
    s.doc_count = storage.CountDocuments();
    s.chunk_count = storage.CountChunks();
    s.searchable_chunk_count = static_cast<int>(index.size());
    return s;
  }

  bool DeleteDocument(int64_t doc_id) {
    index.RemoveDoc(doc_id);
    return open && storage.DeleteDocument(doc_id);
  }

  int DeleteByDomain(const std::string& domain) {
    if (!open) return 0;
    std::vector<int64_t> ids = storage.DeleteByDomain(domain);
    index.RemoveDocs(ids);
    return static_cast<int>(ids.size());
  }

  void EnforceRetention(int64_t cutoff_unix) {
    if (!open) return;
    std::vector<int64_t> ids =
        storage.DeleteDocumentsVisitedBefore(cutoff_unix);
    if (ids.empty()) return;
    index.RemoveDocs(ids);
    LOG(INFO) << "[MoltMemory] retention removed " << ids.size()
              << " pages";
  }

  bool ClearAll() {
    index.Clear();
    return open && storage.Clear();
  }

  std::unique_ptr<MemoryEmbedder> embedder;
  MemoryStorage storage;
  MemoryIndex index;
  bool open = false;
};

MemoryService::MemoryService(Profile* profile)
    : profile_(profile),
      worker_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::TaskPriority::BEST_EFFORT, base::MayBlock(),
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})),
      backend_(std::make_unique<Backend>()) {
  // Open (and, for a pre-v2 DB, migrate) storage and hydrate the index on
  // the worker. Later tasks queue behind this one on the same sequence, so
  // nothing needs to wait for a "ready" signal.
  worker_->PostTask(
      FROM_HERE,
      base::BindOnce(&Backend::Start, base::Unretained(backend_.get()),
                     profile_->GetPath().AppendASCII("MoltMemory")));

  pref_registrar_.Init(profile_->GetPrefs());
  pref_registrar_.Add(prefs::kEnabled,
                      base::BindRepeating(&MemoryService::OnPrefsChanged,
                                          base::Unretained(this)));
  pref_registrar_.Add(prefs::kRetentionDays,
                      base::BindRepeating(&MemoryService::OnPrefsChanged,
                                          base::Unretained(this)));
  EnforceRetention();
  retention_timer_.Start(FROM_HERE, kRetentionSweepInterval, this,
                         &MemoryService::EnforceRetention);
}

MemoryService::~MemoryService() = default;

void MemoryService::Shutdown() {
  retention_timer_.Stop();
  pref_registrar_.RemoveAll();
  // sql::Database has a sequence checker — it must be destroyed on the
  // sequence it was opened on — and the index/embedder are worker-confined
  // too. DeleteSoon queues the deletion behind every in-flight task that
  // still holds the raw Backend pointer, so they finish first.
  if (backend_)
    worker_->DeleteSoon(FROM_HERE, std::move(backend_));
}

bool MemoryService::IsEnabled() const {
  return profile_->GetPrefs()->GetBoolean(prefs::kEnabled);
}

bool MemoryService::IsPaused() const {
  return profile_->GetPrefs()->GetBoolean(prefs::kPaused);
}

int MemoryService::GetRetentionDays() const {
  return prefs::SanitizeRetentionDays(
      profile_->GetPrefs()->GetInteger(prefs::kRetentionDays));
}

bool MemoryService::IsOptInNoticeDismissed() const {
  return profile_->GetPrefs()->GetBoolean(prefs::kOptInNoticeDismissed);
}

void MemoryService::SetEnabled(bool enabled) {
  profile_->GetPrefs()->SetBoolean(prefs::kEnabled, enabled);
  if (enabled) {
    // Opting in answers the "memory is now opt-in" notice.
    DismissOptInNotice();
  }
}

void MemoryService::SetPaused(bool paused) {
  profile_->GetPrefs()->SetBoolean(prefs::kPaused, paused);
}

void MemoryService::SetRetentionDays(int days) {
  profile_->GetPrefs()->SetInteger(prefs::kRetentionDays,
                                   prefs::SanitizeRetentionDays(days));
}

void MemoryService::DismissOptInNotice() {
  profile_->GetPrefs()->SetBoolean(prefs::kOptInNoticeDismissed, true);
}

bool MemoryService::ShouldCapture(const GURL& url) const {
  if (!backend_) return false;  // shutting down
  if (!IsEnabled() || IsPaused()) return false;
  return privacy_.ShouldCapture(url);
}

void MemoryService::OnPrefsChanged() {
  EnforceRetention();
}

void MemoryService::EnforceRetention() {
  if (!backend_) return;
  // Retention applies to an opted-in memory. Pages captured before memory
  // became opt-in are kept until the user clears them or opts in.
  if (!IsEnabled()) return;
  const int days = GetRetentionDays();
  if (days <= 0) return;  // forever
  const int64_t cutoff = static_cast<int64_t>(
      (base::Time::Now() - base::Days(days)).InSecondsFSinceUnixEpoch());
  worker_->PostTask(FROM_HERE,
                    base::BindOnce(&Backend::EnforceRetention,
                                   base::Unretained(backend_.get()), cutoff));
}

void MemoryService::IngestPage(GURL url, std::string title,
                               std::string raw_text) {
  // Re-check: the pref may have flipped while the page's text was being
  // fetched from the renderer.
  if (!ShouldCapture(url)) return;
  worker_->PostTask(
      FROM_HERE,
      base::BindOnce(&Backend::Ingest, base::Unretained(backend_.get()),
                     std::move(url), std::move(title), std::move(raw_text)));
}

void MemoryService::Query(
    const std::string& query_text, int top_k,
    base::OnceCallback<void(std::vector<QueryHit>)> on_results) {
  if (!backend_ || query_text.empty() || top_k <= 0) {
    std::move(on_results).Run({});
    return;
  }
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&Backend::Query, base::Unretained(backend_.get()),
                     query_text, top_k),
      std::move(on_results));
}

void MemoryService::GetStats(base::OnceCallback<void(Stats)> on_done) {
  if (!backend_) {
    std::move(on_done).Run(Stats());
    return;
  }
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&Backend::GetStats, base::Unretained(backend_.get())),
      std::move(on_done));
}

void MemoryService::ListRecent(
    int limit,
    base::OnceCallback<void(std::vector<Document>)> on_done) {
  if (!backend_) {
    std::move(on_done).Run({});
    return;
  }
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](Backend* b, int n) {
            return b->open ? b->storage.ListRecentDocuments(n)
                           : std::vector<Document>();
          },
          base::Unretained(backend_.get()), limit),
      std::move(on_done));
}

void MemoryService::DeleteDocument(int64_t doc_id,
                                   base::OnceCallback<void(bool)> on_done) {
  if (!backend_) {
    std::move(on_done).Run(false);
    return;
  }
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&Backend::DeleteDocument,
                     base::Unretained(backend_.get()), doc_id),
      std::move(on_done));
}

void MemoryService::DeleteByDomain(const std::string& domain,
                                   base::OnceCallback<void(int)> on_done) {
  if (!backend_) {
    std::move(on_done).Run(0);
    return;
  }
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&Backend::DeleteByDomain,
                     base::Unretained(backend_.get()), domain),
      std::move(on_done));
}

void MemoryService::ClearAll(base::OnceCallback<void(bool)> on_done) {
  if (!backend_) {
    std::move(on_done).Run(false);
    return;
  }
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&Backend::ClearAll, base::Unretained(backend_.get())),
      std::move(on_done));
}

}  // namespace memory
}  // namespace molt_ai
