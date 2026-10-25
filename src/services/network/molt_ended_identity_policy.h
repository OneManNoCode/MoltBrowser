// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: what a MoltNet network context does when its identity ends.
//
// The browser ends a MoltNet identity (New Identity, a setting the live
// check refuses, the memory valve) by sending its network contexts a proxy
// configuration that refuses every request: a proxy list with no proxy and
// no DIRECT (chrome/browser/molt_ai/tor/molt_net_routing.cc:
// RefuseAllProxyConfig()). That message comes on the context's
// ProxyConfigClient pipe. Pages and the browser send requests on other
// pipes (URLLoaderFactory, the context's own for WebSockets, WebTransport
// and preconnects), and nothing orders messages on different pipes: a
// request sent before the end may be handled after it, and one sent after
// it before. What was already under way also carries on unless stopped: a
// request whose proxy was resolved before, an open WebSocket, frames an
// HTTP/2 session has queued.
//
// So a context with NetworkContextParams::molt_stop_all_when_refusing
// (MoltNet's) ends with its identity, in the task in which the refusal is
// applied (ProxyConfigServiceMojo::OnProxyConfigUpdated(), after its
// observers, the proxy resolution service among them, have the new
// configuration): NetworkContext::MoltEndIdentity() takes kMoltEndSteps in
// order. From then on the context starts nothing (MoltEndedContextAdmits()):
// whatever pipe a request comes on, and whenever it was sent, if the
// context handles it after the end it is refused before anything is sent.
// And nothing is written to Tor any more: the context's SOCKS connections
// refuse every write and no new SOCKS handshake starts
// (net/socket/molt_identity_end.h), so neither a WebSocket's closing
// handshake, nor an HTTP/2 frame queued before the end, nor the rest of a
// request body leaves. A context made for an identity that has already
// ended (a storage partition loaded late) starts ended.
//
// The browser side (chrome/browser/molt_ai/tor/molt_net_identity_end_policy.h)
// waits until every network context of the identity has the end before
// anything else of the identity learns of it: its pages are stopped, its
// windows closed, Tor asked for new circuits only after that. Anything the
// browser itself holds for the identity's pages it refuses to send from the
// moment the identity ends (ContentBrowserClient::HasNetworkIdentityEnded()).
//
// Std-only, so molt_ai_unittests tests it.

#ifndef SERVICES_NETWORK_MOLT_ENDED_IDENTITY_POLICY_H_
#define SERVICES_NETWORK_MOLT_ENDED_IDENTITY_POLICY_H_

#include <array>

namespace network {

// Whether a proxy configuration refuses every request: a single proxy list
// (`is_single_proxy_list`) without any entry (`proxy_list_empty`), not even
// DIRECT, and no automatic settings (PAC or auto-detect) that could give
// one.
constexpr bool MoltIsRefuseAllProxyConfig(bool is_single_proxy_list,
                                          bool proxy_list_empty,
                                          bool has_automatic_settings) {
  return is_single_proxy_list && proxy_list_empty && !has_automatic_settings;
}

// Whether a context stops everything when a new proxy configuration is
// applied: only one that asked to (`stop_all_when_refusing`), only for a
// configuration that refuses every request, and only on a change (the
// first configuration a context gets is applied before it has anything to
// stop; such a context starts ended instead, MoltStartsEnded()).
constexpr bool MoltStopsEverythingOnConfig(bool stop_all_when_refusing,
                                           bool refuses_all,
                                           bool changed) {
  return stop_all_when_refusing && refuses_all && changed;
}

// Whether a context starts ended: one that would stop everything at the
// refusal, made with the refusal already in place.
constexpr bool MoltStartsEnded(bool stop_all_when_refusing,
                               bool initial_config_refuses_all) {
  return stop_all_when_refusing && initial_config_refuses_all;
}

// Where a network context starts something that may send for its identity.
enum class MoltNetworkEntry {
  // URLLoaderFactory::CreateLoaderAndStart(WithSyncClient)(): every request
  // of a page, a worker or the browser (CORS preflights, Background Fetch,
  // downloads, certificate fetches, Oblivious HTTP and prefetches go through
  // it too).
  kURLLoader,
  kWebSocket,       // NetworkContext::CreateWebSocket()
  kWebTransport,    // NetworkContext::CreateWebTransport()
  kPreconnect,      // NetworkContext::PreconnectSockets()
  kPrefetch,        // NetworkContext::Prefetch()
  kHostResolution,  // NetworkContext::ResolveHost(), CreateHostResolver()
  // NetworkContext::LookUpProxyForURL(): answers how a request would be
  // routed and sends nothing; the browser asks it after the end
  // (MoltNetRouting::WhenIdentityRefuses()).
  kProxyLookup,
  // SOCKSConnectJob: a new stream through Tor (the SOCKS5 CONNECT makes Tor
  // open one to the destination).
  kTorStream,
  // SOCKS5ClientSocket::Write(): bytes to Tor on a connection already open.
  kTorWrite,
};

// Whether a context whose identity has ended (`identity_ended`) still
// starts `entry`: only what sends nothing.
constexpr bool MoltEndedContextAdmits(MoltNetworkEntry entry,
                                      bool identity_ended) {
  return !identity_ended || entry == MoltNetworkEntry::kProxyLookup;
}

// What NetworkContext::MoltEndIdentity() does, in this order, in one task.
enum class MoltEndStep {
  // From here on MoltEndedContextAdmits() refuses everything new.
  kMarkEnded,
  // The context's SOCKS connections refuse every write, and no new SOCKS
  // handshake starts (HttpNetworkSession::MoltEndIdentity()). Before every
  // step below that could write: a WebSocket closed sends its closing
  // handshake, a cancelled HTTP/2 stream queues RST_STREAM.
  kRefuseTorWrites,
  // Every URLLoaderFactory binding is closed
  // (NetworkContext::ResetURLLoaderFactories()): requests still queued in
  // those pipes are discarded unread.
  kCloseURLLoaderFactories,
  // Every WebSocket, open or opening, is destroyed.
  kDropWebSockets,
  // Every WebTransport session is destroyed (none can go through a proxy,
  // so MoltNet's context normally has none).
  kDropWebTransports,
  // Every URL request of the context is cancelled.
  kCancelRequests,
  // Every connection, connection attempt and HTTP/2 session is closed.
  kCloseConnections,
};

inline constexpr std::array<MoltEndStep, 7> kMoltEndSteps = {
    MoltEndStep::kMarkEnded,
    MoltEndStep::kRefuseTorWrites,
    MoltEndStep::kCloseURLLoaderFactories,
    MoltEndStep::kDropWebSockets,
    MoltEndStep::kDropWebTransports,
    MoltEndStep::kCancelRequests,
    MoltEndStep::kCloseConnections,
};

// Whether a step may write to Tor as a side effect, at once or in a later
// task (so it must come after kRefuseTorWrites): closing a WebSocket (its
// closing handshake, at once), cancelling a request (an HTTP/2 RST_STREAM,
// queued), closing a connection (an HTTP/2 session drains what it had
// queued). A WebTransport session never goes through Tor (QUIC can't go
// through a SOCKS proxy).
constexpr bool MoltEndStepMayWrite(MoltEndStep step) {
  return step == MoltEndStep::kDropWebSockets ||
         step == MoltEndStep::kCancelRequests ||
         step == MoltEndStep::kCloseConnections;
}

// Whether `steps` mark the context ended first and refuse writes to Tor
// before any step that may write.
constexpr bool MoltEndStepsInOrder(
    const std::array<MoltEndStep, kMoltEndSteps.size()>& steps) {
  if (steps[0] != MoltEndStep::kMarkEnded) {
    return false;
  }
  bool refusing = false;
  for (MoltEndStep step : steps) {
    if (step == MoltEndStep::kRefuseTorWrites) {
      refusing = true;
    } else if (MoltEndStepMayWrite(step) && !refusing) {
      return false;
    }
  }
  return refusing;
}
static_assert(MoltEndStepsInOrder(kMoltEndSteps));

}  // namespace network

#endif  // SERVICES_NETWORK_MOLT_ENDED_IDENTITY_POLICY_H_
