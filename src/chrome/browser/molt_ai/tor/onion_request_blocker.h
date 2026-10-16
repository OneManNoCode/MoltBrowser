// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Keeps .onion names away from the network outside MoltNet for everything
// that isn't a navigation: subresources, worker and service-worker loads,
// prefetches, downloads and WebSockets.
//
// The host resolver refuses every .onion name (net/dns), but when a regular
// or incognito window uses a proxy server (system settings, a policy or an
// extension), the PROXY resolves host names, so a page there could still
// make MoltBrowser send "<name>.onion" to that proxy by loading an image,
// script or WebSocket from it. Navigations are already stopped by
// OnionNavigationThrottle; this covers the rest, in the browser process:
//
//   * OnionBlockingURLLoaderFactory is appended, from
//     ContentBrowserClient::WillCreateURLLoaderFactory, to every
//     non-navigation URLLoaderFactory of every profile except MoltNet's
//     (document and worker subresources, worker and service-worker scripts,
//     prefetch, early hints, downloads, DevTools). A request whose URL host
//     is in .onion fails with ERR_BLOCKED_BY_CLIENT before it reaches the
//     network service; a redirect to a .onion host is cancelled before
//     anyone follows it, and so is a FollowRedirect() that substitutes a
//     .onion URL.
//   * WebSockets don't use URLLoaderFactory. ShouldInterceptWebSocket-
//     ForOnion() makes content hand WebSocket creation from non-MoltNet
//     frames to ChromeContentBrowserClient::CreateWebSocket(), where
//     IsOnionWebSocketBlocked() refuses a .onion URL (the dropped handshake
//     is a failed connection to the page). WebSockets don't follow
//     redirects, so the initial URL is the only one.
//
// Deliberately independent of MoltShield: its proxy honours the per-site
// Shields switch and its own kill switches; this doesn't. It never blocks
// anything in a MoltNet profile, where onion names go to Tor inside the
// SOCKS5 request.
//
// Known gap: content gives the embedder no frame (and so no profile) for
// WebSockets opened by shared or service workers, so those are not
// intercepted; with a proxy configured, such a WebSocket to a .onion host
// would reach the proxy. (Dedicated workers report their frame.)

#ifndef CHROME_BROWSER_MOLT_AI_TOR_ONION_REQUEST_BLOCKER_H_
#define CHROME_BROWSER_MOLT_AI_TOR_ONION_REQUEST_BLOCKER_H_

#include <memory>
#include <set>

#include "base/containers/unique_ptr_adapters.h"
#include "base/functional/callback.h"
#include "content/public/browser/content_browser_client.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"

class GURL;

namespace content {
class BrowserContext;
class RenderFrameHost;
}  // namespace content

namespace molt_ai {
namespace tor {

// A request is blocked iff IsOnionRequestURL() (onion_location.h) is true
// for its URL or a redirect target.
class OnionBlockingURLLoaderFactory : public network::mojom::URLLoaderFactory {
 public:
  using DisconnectCallback =
      base::OnceCallback<void(OnionBlockingURLLoaderFactory*)>;

  // UI thread. Appends a blocking proxy to |factory_builder| unless
  // |browser_context| is a MoltNet profile or |type| is a navigation
  // factory (OnionNavigationThrottle handles those).
  static void MaybeProxy(
      content::BrowserContext* browser_context,
      content::ContentBrowserClient::URLLoaderFactoryType type,
      network::URLLoaderFactoryBuilder& factory_builder);

  OnionBlockingURLLoaderFactory(
      network::URLLoaderFactoryBuilder& factory_builder,
      DisconnectCallback on_disconnect);
  OnionBlockingURLLoaderFactory(const OnionBlockingURLLoaderFactory&) =
      delete;
  OnionBlockingURLLoaderFactory& operator=(
      const OnionBlockingURLLoaderFactory&) = delete;
  ~OnionBlockingURLLoaderFactory() override;

  // network::mojom::URLLoaderFactory:
  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader_receiver,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override;
  void Clone(mojo::PendingReceiver<network::mojom::URLLoaderFactory>
                 loader_receiver) override;

 private:
  class InProgressRequest;

  void OnTargetFactoryError();
  void OnProxyBindingError();
  void RemoveRequest(InProgressRequest* request);
  void MaybeDestroySelf();

  mojo::ReceiverSet<network::mojom::URLLoaderFactory> proxy_receivers_;
  std::set<std::unique_ptr<InProgressRequest>, base::UniquePtrComparator>
      requests_;
  mojo::Remote<network::mojom::URLLoaderFactory> target_factory_;
  DisconnectCallback on_disconnect_;
};

// UI thread. True iff WebSockets from |frame| must be vetted for .onion
// URLs: |frame|'s profile isn't MoltNet. False for a null |frame| (see the
// known gap above).
bool ShouldInterceptWebSocketForOnion(content::RenderFrameHost* frame);

// UI thread. True iff the WebSocket |url| from |frame| must not be opened:
// ShouldInterceptWebSocketForOnion(frame) and |url| is a .onion URL.
bool IsOnionWebSocketBlocked(content::RenderFrameHost* frame, const GURL& url);

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_ONION_REQUEST_BLOCKER_H_
