// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_service.h"

#include <ctime>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/molt_ai/memory/memory_extractor.h"
#include "chrome/browser/profiles/profile.h"
#include "crypto/sha2.h"
#include "url/gurl.h"

namespace molt_ai {
namespace memory {

namespace {

// Build a single contiguous byte vector from the SHA-256 of a string.
std::vector<uint8_t> Sha256(const std::string& s) {
  std::string hash = crypto::SHA256HashString(s);
  return std::vector<uint8_t>(hash.begin(), hash.end());
}

}  // namespace

MemoryService::MemoryService(Profile* profile)
    : profile_(profile),
      embedder_(CreateDefaultEmbedder()),
      index_(std::make_unique<MemoryIndex>()),
      storage_(std::make_unique<MemoryStorage>()),
      worker_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::TaskPriority::BEST_EFFORT, base::MayBlock(),
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {
  base::FilePath memory_dir =
      profile_->GetPath().AppendASCII("MoltMemory");

  // Open storage off the UI thread, then hydrate the in-RAM index.
  worker_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(
          [](MemoryStorage* s, base::FilePath dir) {
            s->Open(dir);
          },
          storage_.get(), memory_dir),
      base::BindOnce(&MemoryService::FinishStartOnUI,
                     weak_factory_.GetWeakPtr()));
}

MemoryService::~MemoryService() = default;

void MemoryService::Shutdown() {
  // sql::Database has a sequence checker — it must be destroyed on
  // the same task runner it was opened on. The DB was opened in
  // worker_ via PostTaskAndReply (see ctor), so we hand storage_'s
  // ownership back to the worker via DeleteSoon. This posts a
  // deletion task at the tail of the worker queue, so any in-flight
  // PostTask jobs that still hold raw_storage finish first and the
  // deletion runs cleanly on the right sequence.
  //
  // After this move, storage_ is empty. The remaining ~MemoryService
  // destruction on the UI thread is a no-op for storage_ — index_
  // and embedder_ are UI-thread-only and tear down safely there.
  if (storage_)
    worker_->DeleteSoon(FROM_HERE, std::move(storage_));
}

void MemoryService::FinishStartOnUI() {
  // Hydrate the in-RAM index from disk in a single bulk load.
  auto* raw_storage = storage_.get();
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](MemoryStorage* s) {
            std::vector<std::tuple<int64_t, int64_t, std::vector<float>>> rows;
            s->LoadAllEmbeddings(&rows);
            return rows;
          },
          raw_storage),
      base::BindOnce(&MemoryService::OnEmbeddingsLoaded,
                     weak_factory_.GetWeakPtr()));
}

void MemoryService::OnEmbeddingsLoaded(
    std::vector<std::tuple<int64_t, int64_t, std::vector<float>>> rows) {
  for (auto& [chunk_id, doc_id, emb] : rows) {
    index_->Add(chunk_id, doc_id, emb);
  }
  ready_ = true;
  LOG(INFO) << "[MoltMemory] index ready with " << index_->size()
            << " chunks";
}

void MemoryService::IngestPage(GURL url, std::string title,
                                std::string raw_text) {
  if (!ready_) return;  // first-launch race; drop the page silently
  if (!ShouldCapture(url)) return;

  std::string normalized = NormalizePageText(raw_text);
  if (normalized.empty()) return;

  // Hash the normalized text so we can skip re-embedding on a revisit
  // to an unchanged page.
  std::vector<uint8_t> hash = Sha256(normalized);
  std::string url_str = url.spec();

  // The chunking + embedding + write is fully off-UI. We move the
  // owned data into the worker closure so no copies are made.
  auto chunker_strings = ChunkText(normalized);
  std::vector<Chunk> chunks;
  chunks.reserve(chunker_strings.size());
  for (size_t i = 0; i < chunker_strings.size(); ++i) {
    Chunk c;
    c.chunk_idx = static_cast<int>(i);
    c.text = std::move(chunker_strings[i]);
    c.embedding = embedder_->Embed(c.text);  // sub-millisecond
    chunks.push_back(std::move(c));
  }

  Document doc;
  doc.url = url_str;
  doc.title = std::move(title);
  doc.visited_at_unix = static_cast<int64_t>(std::time(nullptr));
  doc.word_count = CountWords(normalized);
  doc.content_hash = std::move(hash);

  // Save off-thread, then on reply update the hot index.
  auto* raw_storage = storage_.get();
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](MemoryStorage* s, Document d, std::vector<Chunk> chs) {
            // Idempotency: if URL + hash already exists, no-op.
            int64_t existing = s->FindUnchangedDoc(d.url, d.content_hash);
            if (existing) {
              return std::vector<std::tuple<int64_t, int64_t,
                                            std::vector<float>>>{};
            }
            if (!s->InsertDocumentWithChunks(&d, &chs)) {
              return std::vector<std::tuple<int64_t, int64_t,
                                            std::vector<float>>>{};
            }
            std::vector<std::tuple<int64_t, int64_t, std::vector<float>>>
                rows;
            rows.reserve(chs.size());
            for (auto& c : chs) {
              rows.emplace_back(c.chunk_id, c.doc_id, std::move(c.embedding));
            }
            return rows;
          },
          raw_storage, std::move(doc), std::move(chunks)),
      base::BindOnce(&MemoryService::OnPageIngested,
                     weak_factory_.GetWeakPtr(), /*doc_id=*/0));
}

void MemoryService::OnPageIngested(
    int64_t /*doc_id*/,
    std::vector<std::tuple<int64_t, int64_t, std::vector<float>>> new_rows) {
  for (auto& [chunk_id, doc_id, emb] : new_rows) {
    index_->Add(chunk_id, doc_id, emb);
  }
}

void MemoryService::Query(
    const std::string& query_text, int top_k,
    base::OnceCallback<void(std::vector<QueryHit>)> on_results) {
  if (!ready_ || query_text.empty() || top_k <= 0) {
    std::move(on_results).Run({});
    return;
  }
  std::vector<float> qv = embedder_->Embed(query_text);
  std::vector<QueryHit> hits = index_->Query(qv, top_k);
  if (hits.empty()) {
    std::move(on_results).Run({});
    return;
  }

  // Enrich hits with title/url/snippet from storage (off-UI).
  std::vector<int64_t> chunk_ids;
  std::vector<int64_t> doc_ids;
  for (const auto& h : hits) {
    chunk_ids.push_back(h.chunk_id);
    doc_ids.push_back(h.doc_id);
  }
  auto* raw_storage = storage_.get();
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](MemoryStorage* s, std::vector<QueryHit> hits) {
            for (auto& h : hits) {
              auto doc = s->GetDocument(h.doc_id);
              if (doc) {
                h.url = doc->url;
                h.title = doc->title;
                h.visited_at_unix = doc->visited_at_unix;
              }
              auto chunk = s->GetChunk(h.chunk_id);
              if (chunk) {
                // Snippet: first ~240 chars of the matched chunk.
                h.snippet = chunk->text.substr(
                    0, std::min<size_t>(240, chunk->text.size()));
              }
            }
            return hits;
          },
          raw_storage, std::move(hits)),
      std::move(on_results));
}

void MemoryService::GetStats(
    base::OnceCallback<void(int, int)> on_done) {
  auto* raw_storage = storage_.get();
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](MemoryStorage* s) {
            return std::make_pair(s->CountDocuments(), s->CountChunks());
          },
          raw_storage),
      base::BindOnce(
          [](base::OnceCallback<void(int, int)> cb,
             std::pair<int, int> r) {
            std::move(cb).Run(r.first, r.second);
          },
          std::move(on_done)));
}

void MemoryService::ListRecent(
    int limit,
    base::OnceCallback<void(std::vector<Document>)> on_done) {
  auto* raw_storage = storage_.get();
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](MemoryStorage* s, int n) { return s->ListRecentDocuments(n); },
          raw_storage, limit),
      std::move(on_done));
}

void MemoryService::DeleteDocument(int64_t doc_id,
                                    base::OnceCallback<void(bool)> on_done) {
  // Drop from hot index immediately so subsequent queries don't return
  // a soon-to-be-deleted chunk.
  index_->RemoveDoc(doc_id);
  auto* raw_storage = storage_.get();
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](MemoryStorage* s, int64_t id) { return s->DeleteDocument(id); },
          raw_storage, doc_id),
      std::move(on_done));
}

void MemoryService::DeleteByDomain(
    const std::string& domain,
    base::OnceCallback<void(int)> on_done) {
  auto* raw_storage = storage_.get();
  // Slow but correct: we'll resync the in-RAM index from storage on
  // reply rather than try to identify which doc_ids belong to the
  // domain on the UI thread.
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](MemoryStorage* s, std::string d) {
            return s->DeleteByDomain(d);
          },
          raw_storage, domain),
      base::BindOnce(
          [](base::WeakPtr<MemoryService> self,
             base::OnceCallback<void(int)> cb, int n) {
            if (self) {
              self->index_->Clear();
              self->FinishStartOnUI();
            }
            std::move(cb).Run(n);
          },
          weak_factory_.GetWeakPtr(), std::move(on_done)));
}

void MemoryService::ClearAll(base::OnceCallback<void(bool)> on_done) {
  index_->Clear();
  auto* raw_storage = storage_.get();
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce([](MemoryStorage* s) { return s->Clear(); },
                     raw_storage),
      std::move(on_done));
}

}  // namespace memory
}  // namespace molt_ai
