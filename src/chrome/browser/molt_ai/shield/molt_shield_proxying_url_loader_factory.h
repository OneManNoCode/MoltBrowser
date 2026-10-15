// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShieldProxyingURLLoaderFactory — puts MoltShield between renderers /
// workers and the network service for every subresource request: scripts,
// images, stylesheets, fonts, media, XHR/fetch, beacons and keepalive
// fetches, worker and service-worker scripts and their fetches, prefetch and
// early-hints preloads. (The old browser-side throttle only ever saw
// navigations, workers and prefetch.)
//
// Installed from ContentBrowserClient::WillCreateURLLoaderFactory via
// network::URLLoaderFactoryBuilder, following the pattern of
// signin::ProxyingURLLoaderFactory / extensions'
// WebRequestProxyingURLLoaderFactory: one proxy per factory pipe, one
// InProgressRequest per request that proxies both URLLoader and
// URLLoaderClient so redirects are re-checked before the renderer follows
// them.
//
// Lives on the UI thread. Each check is posted to MoltShield's engine
// sequence and the request is started (or failed with
// ERR_BLOCKED_BY_CLIENT, or answered with a surrogate for `$redirect`
// rules) when the verdict comes back — the UI thread never matches.
//
// Navigations (documents and frames) are not handled here; see
// molt_shield_navigation_throttle.h. Top-level documents are never blocked.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_PROXYING_URL_LOADER_FACTORY_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_PROXYING_URL_LOADER_FACTORY_H_

#include <memory>
#include <optional>
#include <set>

#include "base/containers/unique_ptr_adapters.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/global_routing_id.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/isolation_info.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "url/origin.h"

namespace content {
class BrowserContext;
class RenderFrameHost;
}  // namespace content

namespace molt_ai::shield {

class MoltShieldProxyingURLLoaderFactory
    : public network::mojom::URLLoaderFactory {
 public:
  using DisconnectCallback =
      base::OnceCallback<void(MoltShieldProxyingURLLoaderFactory*)>;

  // UI thread. Appends a proxy to `factory_builder` unless MoltShield is off
  // or the factory serves requests it never filters (navigations,
  // downloads, DevTools).
  static void MaybeProxy(
      content::BrowserContext* browser_context,
      content::RenderFrameHost* frame,
      content::ContentBrowserClient::URLLoaderFactoryType type,
      const url::Origin& request_initiator,
      const net::IsolationInfo& isolation_info,
      network::URLLoaderFactoryBuilder& factory_builder);

  MoltShieldProxyingURLLoaderFactory(
      content::BrowserContext* browser_context,
      content::GlobalRenderFrameHostId frame_id,
      std::optional<url::Origin> top_frame_origin,
      network::URLLoaderFactoryBuilder& factory_builder,
      DisconnectCallback on_disconnect);
  MoltShieldProxyingURLLoaderFactory(
      const MoltShieldProxyingURLLoaderFactory&) = delete;
  MoltShieldProxyingURLLoaderFactory& operator=(
      const MoltShieldProxyingURLLoaderFactory&) = delete;
  ~MoltShieldProxyingURLLoaderFactory() override;

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

  // Valid for the proxy's lifetime: proxies are owned by per-profile user
  // data and destroyed with the profile.
  const raw_ptr<content::BrowserContext> browser_context_;
  // Frame the requests are attributed to (per-page blocked counter); null
  // for service workers and shared workers.
  const content::GlobalRenderFrameHostId frame_id_;
  // Top-level site of the page/worker, for the per-site Shields switch.
  const std::optional<url::Origin> top_frame_origin_;

  mojo::ReceiverSet<network::mojom::URLLoaderFactory> proxy_receivers_;
  std::set<std::unique_ptr<InProgressRequest>, base::UniquePtrComparator>
      requests_;
  mojo::Remote<network::mojom::URLLoaderFactory> target_factory_;
  DisconnectCallback on_disconnect_;
};

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_PROXYING_URL_LOADER_FACTORY_H_
