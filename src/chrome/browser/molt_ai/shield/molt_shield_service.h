// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShieldService — browser-wide owner of MoltShield (ad/tracker blocker).
//
// Threading model:
//  * The adblock-rust engine lives on a dedicated thread-pool sequence
//    (`engine_`, a base::SequenceBound<MoltShieldEngine>). Check(),
//    GetCosmetic() and GetHiddenClassIdCss() post there and reply on the
//    caller's sequence, so matching never runs on the UI or IO thread. They
//    are safe to call from any sequence.
//  * The first task on the engine sequence loads the cached engine, so
//    requests made during startup wait for it rather than going unfiltered.
//  * List updates (FilterListManager, UI thread) trigger a rebuild on a
//    separate thread-pool task; the result is swapped in on the engine
//    sequence between two checks.
//  * Settings and list state are owned on the UI thread; IsEnabled() etc.
//    read atomic mirrors and work from any sequence.
//
// Lists are browser-wide (like Brave); the per-site on/off switch is per
// profile (molt_shield_site_settings.h).

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SERVICE_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SERVICE_H_

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/sequence_bound.h"
#include "chrome/browser/molt_ai/shield/molt_shield_engine.h"
#include "chrome/browser/molt_ai/shield/molt_shield_types.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "url/gurl.h"

class Profile;

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace molt_ai::shield {

class FilterListManager;

class MoltShieldService : public ProfileObserver {
 public:
  // UI thread. Creates and starts the service on first use.
  static MoltShieldService* Get();

  // Null until Get() has run once; any sequence. For hooks that may run off
  // the UI thread and must not create the service there.
  static MoltShieldService* GetIfExists();

  MoltShieldService(const MoltShieldService&) = delete;
  MoltShieldService& operator=(const MoltShieldService&) = delete;

  // ---- Any sequence -------------------------------------------------------

  // Global switches (atomic mirrors of the persisted settings).
  bool IsEnabled() const { return enabled_.load(std::memory_order_relaxed); }
  bool IsCosmeticEnabled() const {
    return IsEnabled() && cosmetic_enabled_.load(std::memory_order_relaxed);
  }
  bool IsConsentKillerEnabled() const {
    return IsEnabled() &&
           consent_killer_enabled_.load(std::memory_order_relaxed);
  }

  // Engine lookups; `reply` runs on the calling sequence.
  void Check(CheckInput input, base::OnceCallback<void(CheckResult)> reply);
  void GetCosmetic(const GURL& document_url,
                   base::OnceCallback<void(CosmeticData)> reply);
  void GetHiddenClassIdCss(const GURL& document_url,
                           std::vector<std::string> classes,
                           std::vector<std::string> ids,
                           base::OnceCallback<void(std::string)> reply);

  // Counters.
  void RecordCheck(const CheckResult& result, std::string_view request_type);
  void RecordCosmetic(const CosmeticData& data);
  ShieldStats GetStats() const;

  // ---- UI thread ----------------------------------------------------------

  void SetEnabled(bool enabled);
  void SetCosmeticEnabled(bool enabled);
  void SetConsentKillerEnabled(bool enabled);

  // Offers a profile whose network context list updates may use. The first
  // live profile is kept until it is destroyed. OTR profiles map to their
  // original profile; updates never use a separate or system context.
  void RegisterProfile(Profile* profile);

  // Persisted switches (as stored, not combined with `enabled`).
  const ShieldSettings& settings() const;

  std::vector<ListStatus> GetListStatus() const;
  void SetListEnabled(const std::string& id, bool enabled);
  void UpdateListsNow();
  bool IsUpdating() const;
  bool IsReady() const { return engine_ready_; }
  base::Time last_update_check() const;

  // "adblock-rust X / molt-N" for reports.
  std::string EngineVersion() const;

  // ProfileObserver:
  void OnProfileWillBeDestroyed(Profile* profile) override;

 private:
  friend class base::NoDestructor<MoltShieldService>;

  MoltShieldService();
  ~MoltShieldService() override;

  void Initialize();
  void OnStartupLoaded(MoltShieldEngine::StartupResult result);
  void OnListPassDone(bool lists_changed);
  void Rebuild();
  void OnRebuilt(std::unique_ptr<MoltShieldEngine::Compiled> compiled);
  void OnEngineSwapped(MoltShieldEngine::Summary summary);
  void ApplySettingsToAtomics();
  scoped_refptr<network::SharedURLLoaderFactory> GetLoaderFactory();

  base::FilePath dir_;
  scoped_refptr<base::SequencedTaskRunner> engine_task_runner_;
  scoped_refptr<base::SequencedTaskRunner> file_task_runner_;
  base::SequenceBound<MoltShieldEngine> engine_;
  std::unique_ptr<FilterListManager> lists_;

  bool engine_ready_ = false;
  bool rebuild_in_flight_ = false;
  bool rebuild_pending_ = false;

  raw_ptr<Profile> update_profile_ = nullptr;
  base::ScopedObservation<Profile, ProfileObserver> profile_observation_{
      this};

  std::atomic<bool> enabled_{true};
  std::atomic<bool> cosmetic_enabled_{true};
  std::atomic<bool> consent_killer_enabled_{true};

  std::atomic<int64_t> total_requests_checked_{0};
  std::atomic<int64_t> trackers_blocked_{0};
  std::atomic<int64_t> ads_blocked_{0};
  std::atomic<int64_t> redirects_served_{0};
  std::atomic<int64_t> websockets_blocked_{0};
  std::atomic<int64_t> cosmetic_rules_applied_{0};
  std::atomic<int64_t> scriptlets_injected_{0};

  base::WeakPtrFactory<MoltShieldService> weak_factory_{this};
};

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SERVICE_H_
