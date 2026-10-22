// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_service.h"

#include <ctime>
#include <functional>
#include <memory>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/keys/molt_os_crypt.h"
#include "chrome/browser/molt_ai/memory/llama_embedder.h"
#include "chrome/browser/molt_ai/memory/memory_backend.h"
#include "chrome/browser/molt_ai/memory/memory_embedder.h"
#include "chrome/browser/molt_ai/memory/memory_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "url/gurl.h"

namespace molt_ai {
namespace memory {

namespace {

// How often the retention sweep runs while the browser stays open.
constexpr base::TimeDelta kRetentionSweepInterval = base::Hours(6);

// A once-only callback carried through the backend's copyable callbacks,
// answered on |runner| (the UI thread).
template <typename T>
std::function<void(T)> ReplyOn(scoped_refptr<base::SequencedTaskRunner> runner,
                               base::OnceCallback<void(T)> callback) {
  auto shared =
      std::make_shared<base::OnceCallback<void(T)>>(std::move(callback));
  return [runner, shared](T value) {
    if (!*shared) return;
    runner->PostTask(FROM_HERE,
                     base::BindOnce(std::move(*shared), std::move(value)));
  };
}

}  // namespace

MemoryService::MemoryService(Profile* profile)
    : profile_(profile),
      worker_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::TaskPriority::BEST_EFFORT, base::MayBlock(),
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {
  // Delayed work of the backend runs on the worker; it checks itself that
  // the backend still exists (MemoryBackend::Guarded).
  backend_ = std::make_unique<MemoryBackend>(
      [runner = worker_](std::function<void()> task, int64_t delay_ms) {
        runner->PostDelayedTask(
            FROM_HERE,
            base::BindOnce([](std::function<void()> t) { t(); },
                           std::move(task)),
            base::Milliseconds(delay_ms));
      });

  // Opening the DB needs the Encryptor (the URL key is stored encrypted), so
  // wait for molt_os_crypt; MoltOSCryptStartupFactory starts it with the
  // profile. Until then the backend isn't open: queries come back empty and
  // pages aren't recorded.
  molt_os_crypt::RunWhenReady(
      base::BindOnce(&MemoryService::StartBackend, weak_factory_.GetWeakPtr()));

  pref_registrar_.Init(profile_->GetPrefs());
  pref_registrar_.Add(prefs::kEnabled,
                      base::BindRepeating(&MemoryService::OnPrefsChanged,
                                          base::Unretained(this)));
  pref_registrar_.Add(prefs::kPaused,
                      base::BindRepeating(&MemoryService::OnPrefsChanged,
                                          base::Unretained(this)));
  pref_registrar_.Add(prefs::kRetentionDays,
                      base::BindRepeating(&MemoryService::OnPrefsChanged,
                                          base::Unretained(this)));
  retention_timer_.Start(FROM_HERE, kRetentionSweepInterval, this,
                         &MemoryService::EnforceRetention);

  // The on-device embedding model (shared by every profile).
  EmbeddingModelManager* models = EmbeddingModelManager::GetInstance();
  models->AddObserver(this);
  if (!models->IsKnown()) models->Refresh();
}

MemoryService::~MemoryService() = default;

void MemoryService::StartBackend() {
  if (!backend_) {
    return;  // shut down meanwhile
  }
  // Open (and, for a pre-v2 DB, migrate) storage and hydrate the index on
  // the worker. Later tasks queue behind this one on the same sequence.
  worker_->PostTask(
      FROM_HERE,
      base::BindOnce(&MemoryBackend::Start, base::Unretained(backend_.get()),
                     profile_->GetPath().AppendASCII("MoltMemory")));
  PushBackgroundAllowed();
  PushModelEmbedder();
  // The startup retention sweep, now that there is a DB to sweep.
  EnforceRetention();
}

void MemoryService::Shutdown() {
  retention_timer_.Stop();
  pref_registrar_.RemoveAll();
  EmbeddingModelManager::GetInstance()->RemoveObserver(this);
  // sql::Database has a sequence checker — it must be destroyed on the
  // sequence it was opened on — and the index/embedders are worker-confined
  // too. DeleteSoon queues the deletion behind every in-flight task that
  // still holds the raw backend pointer, so they finish first; replies and
  // delayed tasks arriving later find it gone and do nothing.
  if (backend_)
    worker_->DeleteSoon(FROM_HERE, std::move(backend_));
}

void MemoryService::OnEmbeddingModelChanged() {
  PushModelEmbedder();
}

void MemoryService::PushModelEmbedder() {
  EmbeddingModelManager* models = EmbeddingModelManager::GetInstance();
  // Until the model's state is known nothing may be re-embedded (at
  // startup that would throw the model's vectors away).
  if (!backend_ || !models->IsKnown()) return;
  std::unique_ptr<MemoryEmbedder> embedder;
  if (models->IsInstalled()) {
    embedder =
        std::make_unique<LlamaEmbedder>(models->spec(), models->ModelPath());
  }
  worker_->PostTask(FROM_HERE,
                    base::BindOnce(&MemoryBackend::SetModelEmbedder,
                                   base::Unretained(backend_.get()),
                                   std::move(embedder)));
}

void MemoryService::PushBackgroundAllowed() {
  if (!backend_) return;
  // Re-embedding in the background is memory work like recording: only
  // while memory is on and not paused.
  worker_->PostTask(FROM_HERE,
                    base::BindOnce(&MemoryBackend::SetBackgroundAllowed,
                                   base::Unretained(backend_.get()),
                                   IsEnabled() && !IsPaused()));
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
  PushBackgroundAllowed();
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
                    base::BindOnce(&MemoryBackend::EnforceRetention,
                                   base::Unretained(backend_.get()), cutoff));
}

void MemoryService::IngestPage(GURL url, std::string title,
                               std::string raw_text) {
  // Re-check: the pref may have flipped while the page's text was being
  // fetched from the renderer.
  if (!ShouldCapture(url)) return;
  worker_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](MemoryBackend* backend, std::string url, std::string title,
             std::string raw_text) {
            backend->Ingest(url, std::move(title), raw_text,
                            static_cast<int64_t>(std::time(nullptr)));
          },
          base::Unretained(backend_.get()), url.spec(), std::move(title),
          std::move(raw_text)));
}

void MemoryService::RunQuery(const std::string& query_text,
                             int top_k,
                             bool one_per_page,
                             base::OnceCallback<void(SearchResult)> on_results) {
  if (!backend_ || query_text.empty() || top_k <= 0) {
    std::move(on_results).Run(SearchResult());
    return;
  }
  worker_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](MemoryBackend* backend, std::string text, int top_k,
             bool one_per_page, std::function<void(SearchResult)> done) {
            backend->Query(text, top_k, one_per_page, std::move(done));
          },
          base::Unretained(backend_.get()), query_text, top_k, one_per_page,
          ReplyOn<SearchResult>(base::SequencedTaskRunner::GetCurrentDefault(),
                                std::move(on_results))));
}

void MemoryService::Query(
    const std::string& query_text, int top_k,
    base::OnceCallback<void(std::vector<QueryHit>)> on_results) {
  RunQuery(query_text, top_k, /*one_per_page=*/false,
           base::BindOnce(
               [](base::OnceCallback<void(std::vector<QueryHit>)> cb,
                  SearchResult result) {
                 std::move(cb).Run(std::move(result.hits));
               },
               std::move(on_results)));
}

void MemoryService::SearchByMeaning(
    const std::string& query_text,
    int top_k,
    base::OnceCallback<void(SearchResult)> on_results) {
  RunQuery(query_text, top_k, /*one_per_page=*/true, std::move(on_results));
}

void MemoryService::GetSemanticStatus(
    base::OnceCallback<void(SemanticIndexStatus)> on_done) {
  if (!backend_) {
    std::move(on_done).Run(SemanticIndexStatus());
    return;
  }
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&MemoryBackend::GetSemanticStatus,
                     base::Unretained(backend_.get())),
      std::move(on_done));
}

void MemoryService::GetStats(base::OnceCallback<void(Stats)> on_done) {
  if (!backend_) {
    std::move(on_done).Run(Stats());
    return;
  }
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&MemoryBackend::GetStats,
                     base::Unretained(backend_.get())),
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
      base::BindOnce(&MemoryBackend::ListRecent,
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
      base::BindOnce(&MemoryBackend::DeleteDocument,
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
      base::BindOnce(&MemoryBackend::DeleteByDomain,
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
      base::BindOnce(&MemoryBackend::ClearAll,
                     base::Unretained(backend_.get())),
      std::move(on_done));
}

}  // namespace memory
}  // namespace molt_ai
