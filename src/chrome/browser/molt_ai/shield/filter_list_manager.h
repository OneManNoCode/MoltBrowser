// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// FilterListManager — keeps MoltShield's filter lists fresh.
//
//  * Daily check with ±2 h random jitter (first run: within seconds; after a
//    failure: retry in ~1 h). "Update now" in settings forces a pass.
//  * Fetched through a *profile* network context supplied by the service
//    (Profile::GetURLLoaderFactory), never a separate or system context. When
//    MoltNet is on, that context carries the fail-closed Tor SOCKS proxy, so
//    list updates go through Tor too — or fail — and never leave directly.
//  * Credentials omitted, HTTP cache bypassed, conditional requests with
//    the stored ETag / Last-Modified (304 = unchanged), 16 MB cap per list,
//    publisher URL then a mirror.
//  * Downloads stream to a temp file; a file-sequence task validates the
//    content (UTF-8, not HTML, enough filter lines), computes its sha256 and
//    only replaces the cached copy when it changed. The sha256 is recorded
//    in state.json and re-checked before every compile.
//  * Signed GenEye bundles: see list_signature_verifier.h (TODO slot).
//
// Lives on the UI thread; file work runs on `file_task_runner`.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_FILTER_LIST_MANAGER_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_FILTER_LIST_MANAGER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/shield/list_signature_verifier.h"
#include "chrome/browser/molt_ai/shield/molt_shield_engine.h"
#include "chrome/browser/molt_ai/shield/molt_shield_store.h"
#include "chrome/browser/molt_ai/shield/molt_shield_types.h"

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace molt_ai::shield {

struct FilterListEntry;

class FilterListManager {
 public:
  // Returns the URL loader factory of a live profile network context, or
  // null if none is available yet (the pass is retried later).
  using LoaderFactoryGetter =
      base::RepeatingCallback<scoped_refptr<network::SharedURLLoaderFactory>()>;
  // Invoked after every update pass with whether any cached list changed
  // (the service then recompiles off-thread).
  using PassDoneCallback = base::RepeatingCallback<void(bool lists_changed)>;

  FilterListManager(const base::FilePath& dir,
                    scoped_refptr<base::SequencedTaskRunner> file_task_runner,
                    LoaderFactoryGetter loader_factory_getter,
                    PassDoneCallback pass_done);
  ~FilterListManager();
  FilterListManager(const FilterListManager&) = delete;
  FilterListManager& operator=(const FilterListManager&) = delete;

  // Takes the state loaded from disk and schedules the first pass.
  void Start(StoreState state);

  // Runs a pass now (no-op if one is in flight).
  void UpdateNow();
  bool update_in_progress() const { return pass_.has_value(); }

  const StoreState& state() const { return state_; }
  ShieldSettings& mutable_settings() { return state_.settings; }
  // Persists state.json on the file sequence.
  void SaveState();

  // Enabling a list that was never downloaded triggers a pass.
  void SetListEnabled(const std::string& id, bool enabled);

  // Lists to compile: enabled and downloaded at least once.
  std::vector<ListSpec> EnabledListSpecs() const;
  std::vector<ListStatus> GetListStatus() const;

  // Records filter-line counts reported by the engine after a compile.
  void SetRuleLines(const std::map<std::string, uint32_t>& rule_lines);

  // TODO(molt-update): install once signed GenEye bundles exist.
  void SetSignatureVerifier(std::unique_ptr<ListSignatureVerifier> verifier);

 private:
  struct Pass {
    Pass();
    ~Pass();
    std::vector<std::string> pending;  // List ids still to fetch.
    bool changed = false;
    bool any_success = false;
    bool force = false;
  };
  struct ProcessResult;

  void ScheduleNext(base::TimeDelta delay);
  void StartPass(bool force);
  void FetchNext();
  void Fetch(const FilterListEntry& entry, bool use_fallback);
  void OnDownloaded(std::string id,
                    bool used_fallback,
                    std::unique_ptr<network::SimpleURLLoader> loader,
                    base::FilePath temp_path);
  void OnProcessed(std::string id,
                   std::string etag,
                   std::string last_modified,
                   ProcessResult result);
  void RecordFailure(const std::string& id, std::string error);
  void FinishPass();

  static ProcessResult ProcessDownload(base::FilePath dir,
                                       std::string id,
                                       base::FilePath temp_path,
                                       std::string previous_sha256);

  const base::FilePath dir_;
  const scoped_refptr<base::SequencedTaskRunner> file_task_runner_;
  const LoaderFactoryGetter loader_factory_getter_;
  const PassDoneCallback pass_done_;

  StoreState state_;
  std::optional<Pass> pass_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  std::unique_ptr<ListSignatureVerifier> signature_verifier_;
  base::OneShotTimer timer_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<FilterListManager> weak_factory_{this};
};

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_FILTER_LIST_MANAGER_H_
