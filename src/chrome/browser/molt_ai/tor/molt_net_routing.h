// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNetRouting — the single choke point for how MoltNet routes traffic.
//
// Design (Tor-Browser/Brave-style identity isolation):
//   * MoltNet browsing happens ONLY in a dedicated, non-primary
//     off-the-record profile (molt_net_profile_id.h). It has its own
//     in-memory cookies/cache/storage and its own NetworkContext.
//   * The Tor SOCKS proxy and the leak-prevention prefs are applied ONLY to
//     that profile's (in-memory) prefs. The user's regular profile, their
//     primary incognito profile and local state (the system network context)
//     are never proxied — so "turning MoltNet on" can no longer make a
//     logged-in regular session egress from a Tor exit, and closing MoltNet
//     can't strand regular browsing behind a dead proxy.
//   * Fail-closed: the MoltNet profile's proxy is a fixed SOCKS5 server with
//     no DIRECT fallback. If Tor is down, requests fail with a proxy error;
//     they never go direct. A window is only opened after the profile's own
//     NetworkContext confirms (LookUpProxyForURL) that it routes via Tor.
//
// Leak controls applied to the MoltNet profile (rationale in the .cc):
//   proxy = socks5://127.0.0.1:<auto port> (remote DNS, fail-closed),
//   WebRTC = disable_non_proxied_udp, network prediction = off,
//   HTTPS-Only mode = on, Safe Browsing = off.
//
// Lives in the low-level tor target so the WebUI handlers, the toolbar and
// the chat handler share one implementation.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ROUTING_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ROUTING_H_

#include <stdint.h>

#include <string>

#include "base/functional/callback.h"

class Profile;
class PrefService;

namespace molt_ai {
namespace tor {

class MoltNetRouting {
 public:
  using DoneCallback = base::OnceCallback<void(bool ok, std::string error)>;

  enum class RoutingState {
    // A regular / incognito profile. MoltNet never proxies these.
    kNotRouted,
    // The MoltNet profile, proxied through the running Tor.
    kRoutedViaTor,
    // The MoltNet profile while Tor is down: requests fail (proxy error),
    // they never go direct.
    kBlockedTorDown,
    // The MoltNet profile, but a policy / extension / command-line flag
    // controls its proxy or a leak-prevention pref, so MoltNet can't
    // guarantee routing.
    kProxyOverridden,
  };

  struct Status {
    RoutingState state = RoutingState::kNotRouted;
    // True if the proxy pref of the profile shown is controlled by a policy,
    // an extension or the command line (informational for regular profiles).
    bool proxy_externally_controlled = false;
  };

  // The current MoltNet profile of |profile|'s original profile, or null.
  static Profile* GetMoltNetProfile(Profile* profile);

  // Returns the MoltNet profile (creating a fresh one if needed) with routing
  // applied for |socks_port|. Returns null and sets |error| if routing can't
  // be guaranteed (e.g. a policy/extension controls the proxy).
  static Profile* GetOrCreateMoltNetProfile(Profile* profile,
                                            uint16_t socks_port,
                                            std::string* error);

  // (Re)applies the proxy + leak-prevention prefs to a MoltNet profile.
  // Also used when Tor restarts on a different SOCKS port.
  static bool ApplyRoutingPrefs(Profile* moltnet_profile,
                                uint16_t socks_port,
                                std::string* error);

  // Asks the MoltNet profile's own NetworkContext how it would route a
  // request. |done(true)| only if the answer is exactly SOCKS5
  // 127.0.0.1:|socks_port| with no fallback.
  static void VerifyRouting(Profile* moltnet_profile,
                            uint16_t socks_port,
                            DoneCallback done);

  // Closes every socket and clears the host cache of the MoltNet profile's
  // NetworkContext (used by New Identity before the profile is destroyed).
  static void FlushNetworkState(Profile* moltnet_profile);

  // Forgets |moltnet_profile| as the current MoltNet profile so the next
  // GetOrCreateMoltNetProfile() builds a brand-new one (fresh cookies, cache
  // and storage). Does not destroy it.
  static void ForgetMoltNetProfile(Profile* moltnet_profile);

  // Makes |moltnet_profile| the current MoltNet profile of its original
  // profile again, unless that has a current one already (used when New
  // Identity gives up because a window of the old identity stayed open).
  static void AdoptMoltNetProfile(Profile* moltnet_profile);

  // Routing of the profile a UI surface is shown in.
  static Status GetStatus(Profile* profile);

  // Undoes what pre-isolation MoltNet builds wrote to the REGULAR profile and
  // local state (socks5://127.0.0.1:9050 proxy, WebRTC/prediction/DoH
  // overrides). Only exact matches of the legacy values are cleared, and
  // only when the legacy Tor proxy is still present, so user choices are
  // untouched. Idempotent; call once per original profile at startup.
  static void MigrateLegacyRoutingPrefs(Profile* profile,
                                        PrefService* local_state);

  // Keeps the MoltNet profile pointed at Tor's current SOCKS port when Tor
  // (re)starts. Idempotent.
  static void EnsureTorStateObserver();
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ROUTING_H_
