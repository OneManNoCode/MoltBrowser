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

// Apply (or revert) the full set of leak-safe routing prefs on one profile's
// PrefService. Setting proxy_config::prefs::kProxy pushes a live ProxyConfig
// into that profile's network context via PrefProxyConfigTrackerImpl (the same
// path chrome://settings' proxy UI uses), so this takes effect immediately for
// new connections.
void ApplyToPrefs(Profile* profile, bool enable) {
  if (!profile) {
    return;
  }
  PrefService* prefs = profile->GetPrefs();
  if (enable) {
    // Fixed SOCKS5 server -> fail-closed + remote DNS (see header).
    base::Value proxy_dict(ProxyConfigDictionary::CreateFixedServers(
        MoltNetRouting::kSocksProxy, /*bypass_list=*/std::string()));
    prefs->Set(proxy_config::prefs::kProxy, proxy_dict);
    // Close the WebRTC UDP IP-leak vector.
    prefs->SetString(prefs::kWebRTCIPHandlingPolicy,
                     kWebRtcDisableNonProxiedUdp);
    // No HTTP/3 UDP that would sidestep the TCP-only SOCKS proxy.
    prefs->SetBoolean(prefs::kQuicAllowed, false);
  } else {
    base::Value direct(ProxyConfigDictionary::CreateDirect());
    prefs->Set(proxy_config::prefs::kProxy, direct);
    prefs->ClearPref(prefs::kWebRTCIPHandlingPolicy);
    prefs->ClearPref(prefs::kQuicAllowed);
  }
}

// Cover both the regular profile and its already-created incognito profile.
void ApplyToProfileAndOtr(Profile* profile, bool enable) {
  if (!profile) {
    return;
  }
  Profile* original = profile->GetOriginalProfile();
  ApplyToPrefs(original, enable);
  // Only touch the OTR profile if one already exists — creating it here just
  // to set a pref would spin up an incognito session the user never opened.
  if (original->HasPrimaryOTRProfile()) {
    ApplyToPrefs(
        original->GetPrimaryOTRProfile(/*create_if_needed=*/false), enable);
  }
}

}  // namespace

// static
void MoltNetRouting::Enable(Profile* profile) {
  LOG(INFO) << "[MoltNet] Enabling whole-profile Tor routing";
  ApplyToProfileAndOtr(profile, /*enable=*/true);
}

// static
void MoltNetRouting::Disable(Profile* profile) {
  LOG(INFO) << "[MoltNet] Disabling Tor routing (restoring direct)";
  ApplyToProfileAndOtr(profile, /*enable=*/false);
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
void MoltNetRouting::ReconcileOnStartup(Profile* profile) {
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
  Disable(profile);
}

}  // namespace tor
}  // namespace molt_ai
