// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_live_guard.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/tor/molt_net_routing.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "components/safe_browsing/core/common/safe_browsing_prefs.h"

namespace molt_ai::tor {

namespace {

// Every setting MoltNetRouting::SettingsError() reads. A change to any of
// them, in the MoltNet profile or in the profile it was made from, runs
// the check again.
const char* const kWatchedPrefs[] = {
    // The proxy and the rules applied on top of it (ProxySettings,
    // ProxyMode, ProxyServer, ProxyPacUrl, ProxyBypassList and
    // ProxyOverrideRules policies; --proxy-server and friends can't change
    // while the browser runs).
    proxy_config::prefs::kProxy,
    proxy_config::prefs::kProxyOverrideRules,
    // The leak controls (WebRtcIPHandling, NetworkPredictionOptions,
    // HttpsOnlyMode, SafeBrowsingEnabled / SafeBrowsingProtectionLevel).
    prefs::kWebRTCIPHandlingPolicy,
    prefs::kNetworkPredictionOptions,
    prefs::kHttpsOnlyModeEnabled,
    prefs::kSafeBrowsingEnabled,
    // Blob URL partitioning (PartitionedBlobUrlUsage).
    prefs::kPartitionedBlobUrlUsage,
};

class Guard : public ProfileObserver {
 public:
  static Guard& Get() {
    static base::NoDestructor<Guard> guard;
    return *guard;
  }

  Guard() = default;
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
  ~Guard() override = default;

  void Watch(Profile* moltnet, MoltNetLiveGuard::RefusedCallback on_refused) {
    if (!IsMoltNetProfile(moltnet) || Find(moltnet)) {
      return;
    }
    Profile* original = moltnet->GetOriginalProfile();
    auto watch = std::make_unique<Watched>();
    watch->moltnet = moltnet;
    watch->original = original;
    watch->on_refused = std::move(on_refused);
    // base::Unretained: the registrars are members of the watch, which is
    // dropped before either profile's settings go away
    // (OnProfileWillBeDestroyed()); `moltnet` is only a key, looked up
    // again in OnSettingChanged().
    const base::RepeatingClosure changed = base::BindRepeating(
        &Guard::OnSettingChanged, base::Unretained(this),
        base::Unretained(moltnet));
    watch->moltnet_prefs.Init(moltnet->GetPrefs());
    if (original != moltnet) {
      watch->original_prefs.Init(original->GetPrefs());
    }
    for (const char* name : kWatchedPrefs) {
      // Only a registered setting can be watched; every one of these is,
      // for every profile.
      if (moltnet->GetPrefs()->FindPreference(name)) {
        watch->moltnet_prefs.Add(name, changed);
      } else {
        LOG(ERROR) << "[MoltNet] can't watch unregistered setting " << name;
      }
      if (original != moltnet &&
          original->GetPrefs()->FindPreference(name)) {
        watch->original_prefs.Add(name, changed);
      }
    }
    Observe(moltnet);
    Observe(original);
    watches_.push_back(std::move(watch));
  }

  bool IsWatching(Profile* moltnet) { return Find(moltnet) != nullptr; }

  // ProfileObserver:
  void OnProfileWillBeDestroyed(Profile* profile) override {
    // Drop every watch of `profile`, or made from it, while its settings
    // still exist (PrefChangeRegistrar removes its observers when it goes).
    std::vector<Profile*> others;
    std::erase_if(watches_, [profile, &others](const auto& watch) {
      if (watch->moltnet != profile && watch->original != profile) {
        return false;
      }
      others.push_back(watch->moltnet == profile ? watch->original.get()
                                                 : watch->moltnet.get());
      return true;
    });
    Unobserve(profile);
    for (Profile* other : others) {
      if (!InUse(other)) {
        Unobserve(other);
      }
    }
  }

 private:
  struct Watched {
    raw_ptr<Profile> moltnet = nullptr;
    raw_ptr<Profile> original = nullptr;
    MoltNetLiveGuard::RefusedCallback on_refused;
    PrefChangeRegistrar moltnet_prefs;
    PrefChangeRegistrar original_prefs;
    // The check refused once; the identity is ending.
    bool ended = false;
  };

  Watched* Find(Profile* moltnet) {
    for (const std::unique_ptr<Watched>& watch : watches_) {
      if (watch->moltnet == moltnet) {
        return watch.get();
      }
    }
    return nullptr;
  }

  bool InUse(Profile* profile) {
    for (const std::unique_ptr<Watched>& watch : watches_) {
      if (watch->moltnet == profile || watch->original == profile) {
        return true;
      }
    }
    return false;
  }

  void Observe(Profile* profile) {
    if (observed_.insert(profile).second) {
      profile->AddObserver(this);
    }
  }

  void Unobserve(Profile* profile) {
    if (observed_.erase(profile)) {
      profile->RemoveObserver(this);
    }
  }

  void OnSettingChanged(Profile* moltnet) {
    Watched* watch = Find(moltnet);
    if (!watch || watch->ended) {
      return;
    }
    // The open check, on MoltNet's effective values; Tor's current port
    // doesn't count (Tor stopping is not a settings change, and its pages
    // then fail with a proxy error anyway).
    const std::string error =
        MoltNetRouting::SettingsError(moltnet, std::nullopt);
    if (error.empty()) {
      return;
    }
    watch->ended = true;
    LOG(WARNING) << "[MoltNet] a setting changed while MoltNet was open: "
                 << error << "; ending this MoltNet identity";
    // A copy: the callback must not, but could, end the watch.
    MoltNetLiveGuard::RefusedCallback on_refused = watch->on_refused;
    on_refused.Run(moltnet, error);
  }

  std::vector<std::unique_ptr<Watched>> watches_;
  std::set<raw_ptr<Profile>> observed_;
};

}  // namespace

// static
void MoltNetLiveGuard::Watch(Profile* moltnet_profile,
                             RefusedCallback on_refused) {
  Guard::Get().Watch(moltnet_profile, std::move(on_refused));
}

// static
bool MoltNetLiveGuard::IsWatchingForTesting(Profile* moltnet_profile) {
  return Guard::Get().IsWatching(moltnet_profile);
}

}  // namespace molt_ai::tor
