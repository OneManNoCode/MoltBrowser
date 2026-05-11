// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryService — profile-scoped owner of the in-RAM MemoryIndex, the
// SQLite MemoryStorage, the MemoryEmbedder, and the MemoryPrivacy
// gate. All public methods are UI-thread safe; storage I/O is bounced
// to a worker sequence.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_SERVICE_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_SERVICE_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/molt_ai/memory/memory_embedder.h"
#include "chrome/browser/molt_ai/memory/memory_index.h"
#include "chrome/browser/molt_ai/memory/memory_privacy.h"
#include "chrome/browser/molt_ai/memory/memory_storage.h"
#include "chrome/browser/molt_ai/memory/memory_types.h"
#include "components/keyed_service/core/keyed_service.h"
#include "url/gurl.h"

class Profile;

namespace molt_ai {
namespace memory {

class MemoryService : public KeyedService {
 public:
  explicit MemoryService(Profile* profile);
  ~MemoryService() override;

  MemoryService(const MemoryService&) = delete;
  MemoryService& operator=(const MemoryService&) = delete;

  // Privacy gate exposed for the recorder.
  bool ShouldCapture(const GURL& url) const {
    return privacy_.ShouldCapture(url);
  }

  // Top-level entry point from the recorder. Schedules the heavy work
  // (text normalize → chunk → embed → encrypt → write) off the UI
  // thread; returns immediately.
  void IngestPage(GURL url, std::string title, std::string raw_text);

  // Top-level entry point from the query UI / AI chat handler. Embeds
  // the query (sub-millisecond on the hashing embedder), runs cosine
  // search against the in-RAM index, then bounces back to the UI
  // thread with hits enriched by url/title/snippet from storage.
  void Query(const std::string& query_text, int top_k,
             base::OnceCallback<void(std::vector<QueryHit>)> on_results);

  // UI helpers used by molt://memory/.
  void GetStats(
      base::OnceCallback<void(int doc_count, int chunk_count)> on_done);
  void ListRecent(
      int limit,
      base::OnceCallback<void(std::vector<Document>)> on_done);
  void DeleteDocument(int64_t doc_id, base::OnceCallback<void(bool)> on_done);
  void DeleteByDomain(const std::string& domain,
                      base::OnceCallback<void(int)> on_done);
  void ClearAll(base::OnceCallback<void(bool)> on_done);

  // KeyedService.
  void Shutdown() override;

 private:
  void FinishStartOnUI();
  void OnEmbeddingsLoaded(
      std::vector<std::tuple<int64_t, int64_t, std::vector<float>>> rows);
  void OnPageIngested(int64_t doc_id,
                       std::vector<std::tuple<int64_t, int64_t,
                                              std::vector<float>>> new_rows);

  raw_ptr<Profile> profile_;
  std::unique_ptr<MemoryEmbedder> embedder_;
  std::unique_ptr<MemoryIndex> index_;
  std::unique_ptr<MemoryStorage> storage_;
  MemoryPrivacy privacy_;

  scoped_refptr<base::SequencedTaskRunner> worker_;
  bool ready_ = false;

  base::WeakPtrFactory<MemoryService> weak_factory_{this};
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_SERVICE_H_
