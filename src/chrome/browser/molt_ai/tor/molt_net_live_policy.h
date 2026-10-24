// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet fails closed while it is open, not only when it opens.
//
// Before a MoltNet window opens, MoltNet checks the MoltNet profile's
// settings (MoltNetRouting::ApplyRoutingPrefs()): the proxy is MoltNet's own
// Tor proxy and nothing outranks it or applies on top of it (a policy, a
// command-line flag, proxy override rules: molt_net_proxy_policy.h), the
// leak controls have MoltNet's values, and blob URLs are partitioned by
// site. Most of those settings can change while MoltNet is open. A policy
// pushed later (a cloud policy refresh, a changed GPO, plist or policy file)
// reaches MoltNet's profile at once: its settings store shares the regular
// profile's policy store (molt_net_pref_isolation.h). Chromium then hands the
// policy's proxy configuration to MoltNet's network context
// (PrefProxyConfigTrackerImpl -> ProxyConfigMonitor -> the network service),
// where it outranks MoltNet's own Tor proxy, and override rules apply on top
// of whatever proxy is in effect.
//
// Two decisions, both made here:
//  * MoltNet's network context only ever gets MoltNet's own proxy
//    configuration: Tor's SOCKS5 port on 127.0.0.1, nothing bypassed
//    (MoltNetAcceptsProxyConfig(), asked by ProxyConfigMonitor through
//    MoltNetRouting::MayApplyProxyConfig()). It never gets any other one,
//    so its requests keep going to Tor, or fail, and never go direct, to
//    another proxy or where an override rule says. A network context that
//    hasn't had MoltNet's configuration yet has none at all: its requests
//    wait.
//  * When a setting changes so that the check before opening would refuse
//    (CheckMoltNetSettings()), MoltNet ends that identity the way New
//    Identity does (its windows close, its connections are dropped, its
//    profile is destroyed) and tells the user why, with the open check's
//    message (MoltNetSettingsError(); molt_net_live_guard.h watches the
//    settings).
//
// The process-wide partitioning switches (molt_net_partitioning_policy.h)
// are base::Features, fixed once the browser has started; they can't change
// while MoltNet is open. Blob URL partitioning can: the
// PartitionedBlobUrlUsage policy turns it off per profile, at any time.
//
// Header-only and std-only, so molt_ai_unittests tests it.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_LIVE_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_LIVE_POLICY_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "chrome/browser/molt_ai/tor/molt_net_proxy_policy.h"

namespace molt_ai::tor {

// ---- MoltNet's own proxy setting -------------------------------------------

// What MoltNet writes into its profile's "proxy" setting
// (MoltNetRouting::ApplyRoutingPrefs()): fixed servers,
// MoltNetSocksProxyUri(<Tor's SOCKS port>), and this bypass list. A
// socks5:// proxy resolves host names remotely (in Tor), there is no
// DIRECT fallback, and "<-loopback>" removes Chromium's implicit bypass of
// localhost, loopback and link-local addresses, so those go to Tor too,
// which refuses them.
inline constexpr std::string_view kMoltNetSocksProxyPrefix =
    "socks5://127.0.0.1:";
inline constexpr std::string_view kMoltNetProxyBypassList = "<-loopback>";

inline std::string MoltNetSocksProxyUri(uint16_t port) {
  return std::string(kMoltNetSocksProxyPrefix) + std::to_string(port);
}

// The SOCKS port of a "proxy" setting if it is exactly what MoltNet writes:
// fixed servers, `server` is MoltNetSocksProxyUri(port) for a port from 1 to
// 65535 (decimal, no sign, no leading zero, nothing after it), and
// `bypass_list` is kMoltNetProxyBypassList. Otherwise nullopt.
constexpr std::optional<uint16_t> MoltNetSocksPort(bool fixed_servers,
                                                   std::string_view server,
                                                   std::string_view bypass_list) {
  if (!fixed_servers || bypass_list != kMoltNetProxyBypassList ||
      !server.starts_with(kMoltNetSocksProxyPrefix)) {
    return std::nullopt;
  }
  const std::string_view digits = server.substr(kMoltNetSocksProxyPrefix.size());
  if (digits.empty() || digits.size() > 5 || digits.front() == '0') {
    return std::nullopt;
  }
  uint32_t port = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    port = port * 10 + static_cast<uint32_t>(c - '0');
  }
  if (port > 65535) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(port);
}

// ---- The settings check ----------------------------------------------------

// What the check reads from a MoltNet profile's settings. Every field
// defaults to the refusing value, so a field nobody filled in refuses.
struct MoltNetProfileSettings {
  // "proxy" and "proxy_override_rules", as molt_net_proxy_policy.h reads
  // them.
  ProfileProxyPrefs proxy = {.proxy_is_default = false,
                             .proxy_is_user_controlled = false,
                             .override_rule_count = 0};
  // The effective "proxy" value is MoltNet's own (MoltNetSocksPort()
  // accepts it). Before a window opens, the caller also requires the port
  // to be the running Tor's; while MoltNet is open it doesn't, because Tor
  // stopping is not a settings change (MoltNet's pages then fail with a
  // proxy error, and MoltNet re-points its proxy when Tor restarts).
  bool proxy_is_moltnets_own = false;
  // WebRTC IP handling, network prediction, HTTPS-Only mode and Safe
  // Browsing have MoltNet's values, from MoltNet's own store (no policy
  // outranks them).
  bool leak_controls_in_place = false;
  // Blob URLs are partitioned by top-level site: the profile's
  // "PartitionedBlobUrlUsage" setting is true. Only a policy can make it
  // false. Without it a third party's blob URL created on one site can be
  // fetched or opened by the same third party on another site, which links
  // the two visits within one MoltNet identity (Chromium reads it in
  // ChromeContentBrowserClient::IsBlobUrlPartitioningEnabled() and for each
  // new renderer process).
  bool blob_urls_partitioned = false;
};

enum class MoltNetSettingsCheck {
  kOk,
  // Something other than MoltNet decides the proxy (a policy, a
  // command-line flag), override rules apply on top of it, or the
  // effective value isn't MoltNet's Tor proxy.
  kProxyNotMoltNets,
  // A policy outranks one of the leak controls.
  kLeakControlsOverridden,
  // A policy turned blob URL partitioning off.
  kBlobUrlsUnpartitioned,
};

constexpr MoltNetSettingsCheck CheckMoltNetSettings(
    const MoltNetProfileSettings& settings) {
  if (ProxyPrefsOverrideMoltNet(settings.proxy) ||
      !settings.proxy_is_moltnets_own) {
    return MoltNetSettingsCheck::kProxyNotMoltNets;
  }
  if (!settings.leak_controls_in_place) {
    return MoltNetSettingsCheck::kLeakControlsOverridden;
  }
  if (!settings.blob_urls_partitioned) {
    return MoltNetSettingsCheck::kBlobUrlsUnpartitioned;
  }
  return MoltNetSettingsCheck::kOk;
}

// The message shown instead of opening a MoltNet window, and when MoltNet
// ends because a setting changed while it was open. Empty for kOk.
inline std::string MoltNetSettingsError(MoltNetSettingsCheck check) {
  switch (check) {
    case MoltNetSettingsCheck::kOk:
      return std::string();
    case MoltNetSettingsCheck::kProxyNotMoltNets:
      // Extension-controlled settings never reach MoltNet's profile
      // (molt_net_isolation.h: CreateMoltNetExtensionPrefStore).
      return "A policy or command-line flag controls the proxy, so MoltNet "
             "can't guarantee Tor routing";
    case MoltNetSettingsCheck::kLeakControlsOverridden:
      return "A policy overrides MoltNet's WebRTC / prediction / HTTPS-Only "
             "/ Safe Browsing settings";
    case MoltNetSettingsCheck::kBlobUrlsUnpartitioned:
      return "A policy turns off the partitioning of blob URLs by site "
             "(PartitionedBlobUrlUsage), so MoltNet can't keep sites apart";
  }
  return "MoltNet can't check its settings";
}

// The title of the dialog that tells the user why MoltNet ended.
inline constexpr char kMoltNetEndedTitle[] = "MoltNet windows closed";

// ---- What MoltNet's network context may be given ---------------------------

// Whether a profile's network context may be given a proxy configuration
// (ProxyConfigMonitor: the first one, with the network context's
// parameters, and every update after it). Every profile but MoltNet's gets
// whatever Chromium computed. A MoltNet profile's network context only gets
// a configuration that is available (`available`: not unset, which the
// monitor would turn into DIRECT) and is exactly the one MoltNet's own
// "proxy" setting describes (`is_moltnets_own`: equal, override rules and
// all, to the configuration made from a setting MoltNetSocksPort()
// accepts). Anything else is held back: the network context keeps the
// configuration it has, MoltNet's, or, before it has had MoltNet's, none,
// and then its requests wait.
constexpr bool MoltNetAcceptsProxyConfig(bool is_moltnet_profile,
                                         bool available,
                                         bool is_moltnets_own) {
  return !is_moltnet_profile || (available && is_moltnets_own);
}

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_LIVE_POLICY_H_
