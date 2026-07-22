// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNetRouting — the single choke point for every network-side change
// MoltNet makes when the user turns on private routing. Centralizing it here
// keeps the popover, the settings page, and startup reconciliation from
// drifting into inconsistent half-routed states, and gives a security auditor
// one file to reason about.
//
// Lives in the low-level tor target (not the WebUI layer) so both the WebUI
// handlers and the toolbar can call it without a GN dependency cycle.
//
// Security model (rationale for each measure lives in the .cc):
//   * Fixed SOCKS5 proxy at 127.0.0.1:9050  -> FAIL-CLOSED. If the Tor
//     daemon is down, connections error out (ERR_PROXY_CONNECTION_FAILED)
//     instead of silently falling back to the direct connection and leaking.
//   * The socks5:// scheme                  -> Chromium hands the hostname to
//     the proxy for resolution, so DNS (including .onion) is resolved by Tor,
//     never by the local system resolver. No DNS leak.
//   * WebRTC "disable_non_proxied_udp"       -> STUN/TURN cannot open the
//     direct UDP sockets that would reveal the real IP behind the proxy.
//   * QUIC disabled                          -> no HTTP/3 UDP that would try
//     to bypass the TCP-only SOCKS proxy.
//
// Applied to BOTH the original profile and its primary OTR profile, so a
// normal window and an incognito window opened from it are equally covered.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ROUTING_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ROUTING_H_

class Profile;

namespace molt_ai {
namespace tor {

class MoltNetRouting {
 public:
  // The local Tor SOCKS5 endpoint TorManager launches (SocksPort in the
  // managed torrc). Kept in sync with tor_manager.cc.
  static constexpr char kSocksProxy[] = "socks5://127.0.0.1:9050";

  // Route this profile's traffic through Tor (leak-safe, fail-closed). Call
  // only once the Tor control port is reachable (TorManager::Launch's
  // callback) so we don't strand early connections behind a not-yet-ready
  // daemon.
  static void Enable(Profile* profile);

  // Restore direct networking on this profile.
  static void Disable(Profile* profile);

  // True iff this profile's proxy pref currently points at our Tor SOCKS
  // endpoint (i.e. routing is applied).
  static bool IsEnabled(Profile* profile);

  // Startup fail-safe. The proxy pref is persisted, so a crash/quit while
  // routing was on would otherwise leave the profile pointed at a dead Tor
  // proxy on the next launch (every request failing). If routing prefs are
  // set but Tor isn't running, clear them. MoltNet is session-scoped by
  // design — the user re-enables it each session, matching the ephemeral
  // Tor-window model. Call once per original profile at startup.
  static void ReconcileOnStartup(Profile* profile);
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ROUTING_H_
