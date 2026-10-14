// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryService — profile-scoped owner of Personal Vector Memory: the
// in-RAM MemoryIndex, the SQLite MemoryStorage, the MemoryEmbedder, and
// the MemoryPrivacy gate.
//
// Recording is OPT-IN (prefs::kEnabled, default false) and can be paused
// (prefs::kPaused). While enabled, captured pages older than
// prefs::kRetentionDays are deleted (checked at startup, on pref change and
// every few hours). Data captured before memory became opt-in is kept until
// the user clears it or opts in (at which point retention applies).
//
// Threading: public methods are called on the UI thread. Everything that
// touches the DB, the hot index or the embedder (text normalization,
// chunking, embedding, cosine search, encryption) runs on |worker_|; the
// UI thread only reads prefs and posts tasks.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_SERVICE_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_SERVICE_H_

#include <cstdint>  // for int64_t — required on Linux under -fmodules
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/memory/memory_privacy.h"
#include "chrome/browser/molt_ai/memory/memory_types.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/prefs/pref_change_registrar.h"
#include "url/gurl.h"

class Profile;

namespace molt_ai {
namespace memory {

class MemoryService : public KeyedService {
 public:
  struct Stats {
    int doc_count = 0;
    int chunk_count = 0;
    // Chunks currently in the hot (semantically searchable) index. Older
    // chunks beyond kHotIndexCap stay on disk but are not searched.
    int searchable_chunk_count = 0;
  };

  explicit MemoryService(Profile* profile);
  ~MemoryService() override;

  MemoryService(const MemoryService&) = delete;
  MemoryService& operator=(const MemoryService&) = delete;

  // Recording gate for the recorder: memory is enabled (opt-in), not
  // paused, and |url| passes the privacy policy. Cheap; UI thread.
  bool ShouldCapture(const GURL& url) const;

  // ---- Settings (UI thread; backed by profile prefs) ----
  bool IsEnabled() const;
  bool IsPaused() const;
  int GetRetentionDays() const;  // 0 = forever
  bool IsOptInNoticeDismissed() const;
  void SetEnabled(bool enabled);
  void SetPaused(bool paused);
  void SetRetentionDays(int days);  // sanitized to 30/90/365/0
  void DismissOptInNotice();

  // Top-level entry point from the recorder. Re-checks ShouldCapture, then
  // schedules normalize -> chunk -> embed -> encrypt -> write -> index on
  // the worker; returns immediately.
  void IngestPage(GURL url, std::string title, std::string raw_text);

  // Top-level entry point from the query UI / AI chat handler. Embeds the
  // query and runs cosine search against the hot index on the worker,
  // enriching hits with url/title/snippet from storage, then replies on
  // the calling (UI) sequence. Works whether or not recording is enabled —
  // callers that ground the AI must check IsEnabled() themselves.
  void Query(const std::string& query_text, int top_k,
             base::OnceCallback<void(std::vector<QueryHit>)> on_results);

  // UI helpers used by molt://memory/.
  void GetStats(base::OnceCallback<void(Stats)> on_done);
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
  // Worker-confined state (storage + index + embedder). Defined in the .cc.
  struct Backend;

  void OnPrefsChanged();
  void EnforceRetention();

  raw_ptr<Profile> profile_;
  MemoryPrivacy privacy_;
  scoped_refptr<base::SequencedTaskRunner> worker_;
  // Owned here, used and destroyed only on |worker_| (see Shutdown()).
  std::unique_ptr<Backend> backend_;

  PrefChangeRegistrar pref_registrar_;
  base::RepeatingTimer retention_timer_;

  base::WeakPtrFactory<MemoryService> weak_factory_{this};
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_SERVICE_H_
