// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryBackend — everything of AI memory that runs on MemoryService's
// worker sequence: the encrypted SQLite store, the in-RAM index and the
// embedders (docs/design/SEMANTIC_SEARCH.md).
//
// Embedders. HashingEmbedder is always there (synchronous, in-process).
// The on-device model (an asynchronous MemoryEmbedder: its texts go to the
// sandboxed inference service) is set once the user has installed it, and
// replaces hashing once every stored chunk has a vector in its space:
//   - Switching to the model builds its vectors in the background from the
//     encrypted chunk text, a few chunks per call, while searches keep
//     using the committed (hashing) vectors; then the store switches in one
//     transaction and the index is rebuilt. Vectors of two spaces are never
//     in the index, or compared, together.
//   - Back to hashing (model removed or unusable): re-embedded at once.
//   - With the model committed, a new page is stored at once and its
//     chunks are embedded in the background (searchable when done).
//   - Background embedding only runs while memory is on and not paused.
//     A call that fails is retried with a growing delay; a text that makes
//     the model fail on its own twice gets an all-zero vector (never a
//     match) so one hostile page can't stall the index.
//
// Not thread-safe: every method runs on the one sequence; delayed work and
// embedder replies come back through |post_delayed| / the embedder, and are
// dropped once the backend is destroyed.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_BACKEND_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_BACKEND_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "chrome/browser/molt_ai/memory/memory_embedder.h"
#include "chrome/browser/molt_ai/memory/memory_index.h"
#include "chrome/browser/molt_ai/memory/memory_storage.h"
#include "chrome/browser/molt_ai/memory/memory_types.h"

namespace molt_ai {
namespace memory {

class MemoryBackend {
 public:
  // Runs |task| on the backend's sequence after |delay_ms|.
  using PostDelayedTask =
      std::function<void(std::function<void()> task, int64_t delay_ms)>;
  using SearchCallback = std::function<void(SearchResult)>;

  explicit MemoryBackend(PostDelayedTask post_delayed);
  ~MemoryBackend();
  MemoryBackend(const MemoryBackend&) = delete;
  MemoryBackend& operator=(const MemoryBackend&) = delete;

  // Opens (and migrates) the store in |dir|, hydrates the index and settles
  // the vector space once the model's availability is known.
  void Start(const base::FilePath& dir);
  bool open() const { return open_; }

  // The on-device model's embedder, or nullptr when it isn't installed or
  // can't be used. Until the first call nothing is re-embedded either way
  // (the model's state isn't known yet at startup).
  void SetModelEmbedder(std::unique_ptr<MemoryEmbedder> embedder);
  // Memory is on and not paused.
  void SetBackgroundAllowed(bool allowed);

  void Ingest(const std::string& url,
              std::string title,
              const std::string& raw_text,
              int64_t visited_at_unix);

  // Top hits for |text|, best first, enriched with url, title, date and a
  // snippet; with |one_per_page|, the best chunk of each page only.
  // |done| runs on this sequence (before returning with hashing).
  void Query(const std::string& text,
             int top_k,
             bool one_per_page,
             SearchCallback done);

  MemoryStats GetStats();
  SemanticIndexStatus GetSemanticStatus();
  std::vector<Document> ListRecent(int limit);
  bool DeleteDocument(int64_t doc_id);
  int DeleteByDomain(const std::string& domain);
  void EnforceRetention(int64_t cutoff_unix);
  bool ClearAll();

  MemoryStorage& storage_for_testing() { return storage_; }
  const MemoryIndex& index_for_testing() const { return index_; }

 private:
  // |f| with a check that this backend still exists.
  template <typename F>
  auto Guarded(F f);
  void Schedule(int64_t delay_ms, std::function<void()> task);

  // The embedder of the committed space, or nullptr if not available.
  MemoryEmbedder* CommittedEmbedder();
  // Moves the store towards the desired space (see the header comment).
  void Reconcile();
  void ReloadIndex();
  void SchedulePump(int64_t delay_ms);
  void Pump();
  void EmbedInBackground(MemoryEmbedder* embedder,
                         std::vector<MemoryStorage::ChunkText> chunks,
                         bool migration);
  void OnBackgroundEmbedded(std::vector<int64_t> chunk_ids,
                            std::vector<int64_t> doc_ids,
                            const std::string& space,
                            bool migration,
                            EmbedBatchResult result);
  // Stores vectors for the pending chunks / the migration; returns whether
  // the space still matched.
  bool StoreVectors(const std::vector<MemoryStorage::EmbeddingRow>& rows,
                    const std::string& space,
                    bool migration);
  std::vector<QueryHit> Search(const std::vector<float>& query_vector,
                               const std::string& query_text,
                               int top_k,
                               bool one_per_page);

  PostDelayedTask post_delayed_;
  std::unique_ptr<MemoryEmbedder> hashing_;
  std::unique_ptr<MemoryEmbedder> model_;
  bool model_known_ = false;
  bool model_failed_ = false;
  bool background_allowed_ = false;

  MemoryStorage storage_;
  MemoryIndex index_;
  bool open_ = false;

  // Background embedding.
  bool busy_ = false;
  bool pump_scheduled_ = false;
  int64_t pending_cursor_ = 0;
  int64_t migration_cursor_ = 0;
  // After a failed batch, chunks up to this id go one per call, to find
  // the one to blame.
  int64_t single_until_ = 0;
  int64_t retry_ms_ = 0;
  // Chunk id -> failed one-text calls.
  std::map<int64_t, int> failures_;

  // Expires when the backend is destroyed (checked by Guarded()).
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_BACKEND_H_
