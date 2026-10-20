// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What web content in a MoltNet (Tor) window may use of WebRTC's networking:
// nothing, as in Tor Browser (media.peerconnection.enabled = false).
//
// MoltNet's WebRTC policy (disable_non_proxied_udp) leaves WebRTC only TCP
// to relay (TURN) servers through the SOCKS5 proxy, but the host names a
// page hands WebRTC (a TURN or STUN server URL, a remote ICE candidate) are
// looked up by the network service's P2P socket manager with the network
// context's own resolver (services/network/p2p/socket_manager.cc,
// P2PSocketManager::GetHostAddress), outside Tor: a page could make the
// user's DNS resolver look up "<unique>.attacker.example" and learn which
// resolver serves this MoltNet user. The policy is also only enforced by
// the renderer; the P2P socket manager gives any renderer that asks the
// machine's network interface list and plain UDP and TCP sockets.
//
// The only input is whether the profile is MoltNet's: not fingerprinting
// protection or its kill switch, not a setting, not anything a page sends.
// Standard library only, so molt_ai_unittests (and a standalone harness)
// test it. Used by the browser (chrome/browser/molt_ai/tor/
// molt_net_webrtc.h) and the renderer (chrome/renderer/molt_ai/tor/).

#ifndef CHROME_COMMON_MOLT_AI_TOR_MOLT_NET_WEBRTC_POLICY_H_
#define CHROME_COMMON_MOLT_AI_TOR_MOLT_NET_WEBRTC_POLICY_H_

namespace molt_ai::tor {

namespace switches {
// On the command line of every renderer process that hosts a MoltNet
// profile, and of no other process. Not a secret: the process's proxy and
// fingerprinting mode say the same. Never copied from the browser's own
// command line, so only the browser's per-process decision sets it.
inline constexpr char kMoltNetRenderer[] = "molt-net-renderer";
}  // namespace switches

// WebRTC networking available to the web content of one profile.
struct WebRtcNetworkAccess {
  // `new RTCPeerConnection()` succeeds (renderer side).
  bool peer_connections = true;
  // Frames reach the network service's P2P socket manager: UDP and TCP
  // sockets, host-name lookups with the network context's resolver, and
  // the list of the machine's network interfaces (browser side).
  bool p2p_socket_manager = true;
  // Frames reach the network service's mDNS responder, which announces
  // ".local" names for host candidates by multicast on the local network
  // (browser side).
  bool mdns_responder = true;

  friend bool operator==(const WebRtcNetworkAccess&,
                         const WebRtcNetworkAccess&) = default;
};

// The browser's decision for a profile. MoltNet: nothing. Every other
// profile (regular, Incognito, Guest): Chromium's behaviour, unchanged.
WebRtcNetworkAccess WebRtcNetworkAccessForProfile(bool is_moltnet_profile);

// The same decision, as a renderer process reads it from its own command
// line: `has_moltnet_renderer_switch` is whether switches::kMoltNetRenderer
// is present. The browser puts it there exactly for MoltNet profiles, so
// this matches WebRtcNetworkAccessForProfile() for the process's profile.
WebRtcNetworkAccess WebRtcNetworkAccessForRenderer(
    bool has_moltnet_renderer_switch);

// Whether the browser puts switches::kMoltNetRenderer on the command line
// of a renderer process of this profile.
bool ShouldMarkMoltNetRenderer(bool is_moltnet_profile);

}  // namespace molt_ai::tor

#endif  // CHROME_COMMON_MOLT_AI_TOR_MOLT_NET_WEBRTC_POLICY_H_
