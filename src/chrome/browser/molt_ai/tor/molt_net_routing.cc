// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_routing.h"

#include <string>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"

namespace molt_ai {
namespace tor {

namespace {

// Value of blink::kWebRTCIPHandlingDisableNonProxiedUdp (a stable, web-exposed
// WebRTC policy value). Inlined so this leaf target needn't link blink/common.
// Forces WebRTC to route only through the configured proxy, closing the
// STUN/UDP real-IP leak.
constexpr char kWebRtcDisableNonProxiedUdp[] = "disable_non_proxied_udp";

// Value of SecureDnsConfig::kModeOff (the DoH-mode pref string). Disables
// Secure DNS on the routed profile so no DoH resolver fires out-of-band.
constexpr char kDnsModeOff[] = "off";

// preloading::NetworkPredictionOptions::kDisabled — turns off preconnect /
// prefetch / predictive DNS, which resolve via the host resolver directly and
// would otherwise leak page-adjacent hostnames outside the proxy.
constexpr int kNetworkPredictionDisabled = 2;

// Set (enable) or clear (disable) the proxy pref on one PrefService's store.
// Enabling installs the fixed Tor SOCKS5 server (fail-closed, remote DNS);
// disabling CLEARS the pref so the store reverts to its default (follow the
// system proxy) rather than being force-pinned to Direct — which would wipe a
// real OS/enterprise proxy the user relies on.
void SetProxyPref(PrefService* prefs, bool enable) {
  if (!prefs) {
    return;
  }
  if (enable) {
    base::Value proxy_dict(ProxyConfigDictionary::CreateFixedServers(
        MoltNetRouting::kSocksProxy, /*bypass_list=*/std::string()));
    prefs->Set(proxy_config::prefs::kProxy, proxy_dict);
  } else {
    prefs->ClearPref(proxy_config::prefs::kProxy);
  }
}

// Profile-side leak controls: WebRTC IP policy + network prediction. Both are
// profile prefs. (Secure DNS mode is a LOCAL-STATE pref, handled on the system
// store in SetSystemContextPrefs — setting it here would CHECK-fail.)
void SetProfileLeakPrefs(PrefService* prefs, bool enable) {
  if (!prefs) {
    return;
  }
  if (enable) {
    prefs->SetString(prefs::kWebRTCIPHandlingPolicy,
                     kWebRtcDisableNonProxiedUdp);
    // Kill speculative preconnect/prefetch, whose predictive DNS resolves via
    // the host resolver directly (outside the proxy).
    prefs->SetInteger(prefs::kNetworkPredictionOptions,
                      kNetworkPredictionDisabled);
  } else {
    prefs->ClearPref(prefs::kWebRTCIPHandlingPolicy);
    prefs->ClearPref(prefs::kNetworkPredictionOptions);
  }
}

// System (local-state) context: the proxy for all non-profile browser traffic,
// plus Secure DNS off so no DoH resolver fires an out-of-band lookup that would
// bypass the proxy. kDnsOverHttpsMode is a local-state pref (system host
// resolver), so it belongs here, not on the profile store.
void SetSystemContextPrefs(PrefService* local_state, bool enable) {
  if (!local_state) {
    return;
  }
  SetProxyPref(local_state, enable);
  if (enable) {
    local_state->SetString(prefs::kDnsOverHttpsMode, kDnsModeOff);
  } else {
    local_state->ClearPref(prefs::kDnsOverHttpsMode);
  }
}

void ApplyToProfile(Profile* profile, bool enable) {
  if (!profile) {
    return;
  }
  SetProxyPref(profile->GetPrefs(), enable);
  SetProfileLeakPrefs(profile->GetPrefs(), enable);
}

}  // namespace

// static
void MoltNetRouting::Enable(Profile* profile, PrefService* local_state) {
  LOG(INFO) << "[MoltNet] Enabling Tor routing (profile + system context)";
  if (profile) {
    Profile* original = profile->GetOriginalProfile();
    ApplyToProfile(original, /*enable=*/true);
    // Only touch the OTR profile if one already exists — creating it here just
    // to set a pref would spin up an incognito session the user never opened.
    // (A newly-opened incognito inherits the original's proxy pref anyway.)
    if (original->HasPrimaryOTRProfile()) {
      ApplyToProfile(original->GetPrimaryOTRProfile(/*create_if_needed=*/false),
                     /*enable=*/true);
    }
  }
  // The shared system/browser network context (component updater, metrics,
  // network-time, Safe Browsing, …) takes its proxy + DoH config from local
  // state — set it too, or that background traffic would keep egressing from
  // the real IP.
  SetSystemContextPrefs(local_state, /*enable=*/true);
}

// static
void MoltNetRouting::Disable(Profile* profile, PrefService* local_state) {
  LOG(INFO) << "[MoltNet] Disabling Tor routing (restoring default)";
  if (profile) {
    Profile* original = profile->GetOriginalProfile();
    ApplyToProfile(original, /*enable=*/false);
    if (original->HasPrimaryOTRProfile()) {
      ApplyToProfile(original->GetPrimaryOTRProfile(/*create_if_needed=*/false),
                     /*enable=*/false);
    }
  }
  SetSystemContextPrefs(local_state, /*enable=*/false);
}

// static
bool MoltNetRouting::IsEnabled(Profile* profile) {
  if (!profile) {
    return false;
  }
  const base::DictValue& proxy =
      profile->GetOriginalProfile()->GetPrefs()->GetDict(
          proxy_config::prefs::kProxy);
  ProxyConfigDictionary dict(proxy.Clone());
  std::string server;
  return dict.GetProxyServer(&server) &&
         server.find("127.0.0.1:9050") != std::string::npos;
}

// static
void MoltNetRouting::ReconcileOnStartup(Profile* profile,
                                        PrefService* local_state) {
  if (!profile || !IsEnabled(profile)) {
    return;
  }
  if (TorManager::Get()->IsRunning()) {
    // Routing prefs are set and Tor is up (e.g. a restored session that
    // already relaunched Tor) — leave it be.
    return;
  }
  LOG(WARNING) << "[MoltNet] Routing prefs persisted from a prior session but "
                  "Tor isn't running — clearing to avoid a dead proxy";
  Disable(profile, local_state);
}

}  // namespace tor
}  // namespace molt_ai
