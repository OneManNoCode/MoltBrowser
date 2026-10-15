// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShieldNavigationInterceptor — strips tracking parameters
// (`$removeparam` filters and MoltShield's built-in list: utm_*, fbclid,
// gclid, ...) from top-level and frame navigations with an internal
// redirect, before the decorated URL reaches the network. Never blocks.
//
// Why an interceptor and not MoltShieldNavigationThrottle: a
// NavigationThrottle can proceed, defer, cancel or block, but cannot change
// the URL. A URLLoaderThrottle can, but only synchronously, and on a
// server-redirect leg only for the network fetch: the navigation would still
// commit the decorated URL (shown in the omnibox, readable by page script).
// A URLLoaderRequestInterceptor is asked before every leg of a navigation --
// the first request and each redirect -- may answer asynchronously, and can
// serve a redirect that the navigation follows like a server redirect
// (NavigationThrottles see it in WillRedirectRequest). HTTPS-Upgrades
// (HttpsUpgradesInterceptor) uses the same mechanism.
//
// So for a click through l.facebook.com/l.php?u=... -> 302 -> site/?fbclid=X:
// the network loader paused at the 302 is dropped without following it,
// MoltShield answers "307 Internal Redirect" to site/, and only site/ is
// fetched, committed and shown. (The navigation's redirect chain still lists
// the decorated URL, as for any redirect.)
//
// Rules:
//  * GET navigations to http(s) URLs with a query only (a 307 re-sends a
//    POST body).
//  * Global Shields on, Shields up for the top-level site, and the
//    MoltShieldRemoveParam kill switch on (chrome://flags/
//    #molt-shield-removeparam, --disable-molt-shield).
//  * Only the engine's rewrite is used, never its block verdict: documents
//    are never blocked here (ad frames are collapsed by
//    MoltShieldNavigationThrottle).
//  * Loop-safe: a navigation is redirected to a given stripped URL at most
//    once. If the server sends it back to a decorated URL whose stripped
//    form it was already redirected to, the server insists on the
//    parameters: that URL is loaded as is and stripping stops for this
//    navigation. At most kMaxParamStripsPerNavigation strips overall.
//
// Created per navigation by
// ChromeContentBrowserClient::WillCreateURLLoaderRequestInterceptors and
// kept for all its redirect legs. UI thread.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_NAVIGATION_INTERCEPTOR_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_NAVIGATION_INTERCEPTOR_H_

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "chrome/browser/molt_ai/shield/molt_shield_types.h"
#include "content/public/browser/frame_tree_node_id.h"
#include "content/public/browser/url_loader_request_interceptor.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "url/gurl.h"

namespace content {
class BrowserContext;
}  // namespace content

namespace network {
struct ResourceRequest;
}  // namespace network

namespace molt_ai::shield {

class MoltShieldNavigationInterceptor
    : public content::URLLoaderRequestInterceptor,
      public network::mojom::URLLoader {
 public:
  // UI thread. Null when the MoltShieldRemoveParam kill switch is off or
  // --disable-molt-shield is set. Shields state is checked per leg.
  static std::unique_ptr<MoltShieldNavigationInterceptor> MaybeCreate(
      content::FrameTreeNodeId frame_tree_node_id);

  explicit MoltShieldNavigationInterceptor(
      content::FrameTreeNodeId frame_tree_node_id);
  MoltShieldNavigationInterceptor(const MoltShieldNavigationInterceptor&) =
      delete;
  MoltShieldNavigationInterceptor& operator=(
      const MoltShieldNavigationInterceptor&) = delete;
  ~MoltShieldNavigationInterceptor() override;

  // content::URLLoaderRequestInterceptor:
  void MaybeCreateLoader(
      const network::ResourceRequest& tentative_resource_request,
      content::BrowserContext* browser_context,
      LoaderCallback callback) override;

 private:
  // network::mojom::URLLoader, for the synthesized redirect: following it
  // restarts the navigation's loader, so there is nothing to do here.
  void FollowRedirect(
      const std::vector<std::string>& removed_headers,
      const net::HttpRequestHeaders& modified_headers,
      const net::HttpRequestHeaders& modified_cors_exempt_headers,
      const std::optional<GURL>& new_url) override {}
  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override {}

  // The engine input for this leg, or nullopt if it can't or mustn't be
  // stripped.
  std::optional<CheckInput> MakeInput(
      const network::ResourceRequest& request,
      content::BrowserContext* browser_context) const;
  void OnChecked(GURL url, LoaderCallback callback, CheckResult result);
  void ServeRedirect(
      const GURL& new_url,
      const network::ResourceRequest& request,
      mojo::PendingReceiver<network::mojom::URLLoader> receiver,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client);
  void OnConnectionClosed();

  const content::FrameTreeNodeId frame_tree_node_id_;
  // Stripped URLs this navigation was redirected to (loop guard).
  std::set<GURL> redirected_to_;
  int strips_ = 0;

  mojo::Receiver<network::mojom::URLLoader> receiver_{this};
  mojo::Remote<network::mojom::URLLoaderClient> client_;

  base::WeakPtrFactory<MoltShieldNavigationInterceptor> weak_factory_{this};
};

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_NAVIGATION_INTERCEPTOR_H_
