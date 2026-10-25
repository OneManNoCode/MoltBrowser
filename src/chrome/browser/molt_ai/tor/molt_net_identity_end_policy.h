// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// When the end of a MoltNet identity (New Identity, a setting the live
// check refuses) counts as done, and in which order it happens. Std-only,
// so molt_ai_unittests tests it; molt_net_routing.cc
// (MoltNetRouting::WhenIdentityRefuses()) and molt_net_window.cc
// (EndIdentityAndCloseWindows()) apply it.
//
// The refusal reaches the network service on a pipe of its own (the
// profile's ProxyConfigClient, from ProxyConfigMonitor), which nothing
// orders against the pipes pages send their requests on. Until it is
// applied there, the identity's network contexts still route through Tor.
// So the end is made synchronous with them:
//   1. The identity is marked ended: its network contexts are sent the
//      refusal (MoltNetRouting::RefuseAllProxyConfig()), and the browser
//      refuses its navigations from now on (MoltNetEndedIdentityThrottle).
//   2. Its renderer processes are ended at once, so its pages, frames and
//      workers can't make another request.
//   3. Every network context of the identity is asked how it would route a
//      request (LookUpProxyForURL, on the context's own pipe), again every
//      kMoltNetEndProbeIntervalMs, until each answers with the refusal.
//   4. Only then are its connections and host cache dropped (on the same
//      pipe as the answer, so after the refusal: nothing a request opened
//      before it stays), its DevTools sessions detached, its windows closed,
//      and the next step of New Identity taken.
// If no answer confirms the refusal within kMoltNetEndProbeDeadlineMs (the
// network service is gone or hung), the end carries on anyway: its pages
// are stopped already, a network context the service makes again gets the
// refusal from the start, and New Identity still waits for the profile, and
// with it every network context of the identity, to be destroyed.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_IDENTITY_END_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_IDENTITY_END_POLICY_H_

namespace molt_ai {
namespace tor {

// How often the end asks again, and for how long (milliseconds). The
// refusal normally arrives within one or two network-service tasks.
inline constexpr int kMoltNetEndProbeIntervalMs = 10;
inline constexpr int kMoltNetEndProbeDeadlineMs = 5000;
static_assert(kMoltNetEndProbeIntervalMs > 0 &&
              kMoltNetEndProbeIntervalMs < kMoltNetEndProbeDeadlineMs);

// Whether a network context's answer to LookUpProxyForURL is the refusal:
// net::OK (`lookup_ok`) with a proxy list (`has_proxy_info`) that has no
// entry, not even DIRECT (`proxy_list_empty`). Its answer before the
// refusal is applied (SOCKS5 to Tor), an error, or a lost context, isn't.
constexpr bool MoltNetContextRefuses(bool lookup_ok,
                                     bool has_proxy_info,
                                     bool proxy_list_empty) {
  return lookup_ok && has_proxy_info && proxy_list_empty;
}

enum class MoltNetEndProbeStep {
  kRefuses,   // Every context refuses: go on with the end.
  kAskAgain,  // Ask every context again after kMoltNetEndProbeIntervalMs.
  kGiveUp,    // The deadline passed: go on without the confirmation.
};

// After a round in which `refusing` of the identity's `contexts` network
// contexts answered with the refusal. An identity with no network context
// can send nothing.
constexpr MoltNetEndProbeStep MoltNetEndProbeNext(int contexts,
                                                  int refusing,
                                                  bool deadline_passed) {
  if (refusing >= contexts) {
    return MoltNetEndProbeStep::kRefuses;
  }
  return deadline_passed ? MoltNetEndProbeStep::kGiveUp
                         : MoltNetEndProbeStep::kAskAgain;
}

// Whether a navigation of a profile may go on: none of an ended MoltNet
// identity's does (`identity_ended`), whatever stage it is at.
constexpr bool MoltNetNavigationMayProceed(bool is_moltnet_profile,
                                           bool identity_ended) {
  return !(is_moltnet_profile && identity_ended);
}

// Whether the browser may still send a request it holds for a page of a
// profile: a keepalive request, beacon, ping or fetchLater it starts,
// retries or follows a redirect for (the page may be gone), a download or a
// Save Page As resource, a Background Fetch request. None of an ended
// MoltNet identity's, from the moment it ends (step 1 below), before its
// renderers are stopped: a fetchLater the browser would send because its
// page went away is dropped instead. What already reached the network
// service is stopped there when the refusal is applied
// (services/network/molt_ended_identity_policy.h).
constexpr bool MoltNetBrowserMaySendForPage(bool is_moltnet_profile,
                                            bool identity_ended) {
  return !(is_moltnet_profile && identity_ended);
}

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_IDENTITY_END_POLICY_H_
