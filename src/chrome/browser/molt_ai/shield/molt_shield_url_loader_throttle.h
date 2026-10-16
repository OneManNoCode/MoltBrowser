// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShieldURLLoaderThrottle — browser-side throttle for requests the
// browser loads on a renderer's behalf outside the subresource factory:
// keepalive requests (navigator.sendBeacon, fetch({keepalive: true}),
// fetchLater) that outlive their document, registered from
// ContentBrowserClient::CreateURLLoaderThrottlesForKeepAlive.
//
// The check is asynchronous: the request is deferred while MoltShield's
// engine sequence answers, then resumed or cancelled with
// ERR_BLOCKED_BY_CLIENT. Redirects are re-checked the same way. Requests
// for documents/frames are ignored (navigations are handled by
// MoltShieldNavigationThrottle; top-level documents are never blocked).

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_URL_LOADER_THROTTLE_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_URL_LOADER_THROTTLE_H_

#include <memory>
#include <optional>
#include <string>

#include "base/memory/weak_ptr.h"
#include "chrome/browser/molt_ai/shield/molt_shield_types.h"
#include "content/public/browser/frame_tree_node_id.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"
#include "url/origin.h"

namespace content {
class BrowserContext;
}  // namespace content

namespace molt_ai::shield {

class MoltShieldURLLoaderThrottle : public blink::URLLoaderThrottle {
 public:
  // UI thread. Null when MoltShield is off globally or for the top-level
  // site of `frame_tree_node_id`'s tab.
  static std::unique_ptr<MoltShieldURLLoaderThrottle> MaybeCreateForKeepAlive(
      content::BrowserContext* browser_context,
      content::FrameTreeNodeId frame_tree_node_id);

  explicit MoltShieldURLLoaderThrottle(
      content::FrameTreeNodeId frame_tree_node_id);
  MoltShieldURLLoaderThrottle(const MoltShieldURLLoaderThrottle&) = delete;
  MoltShieldURLLoaderThrottle& operator=(const MoltShieldURLLoaderThrottle&) =
      delete;
  ~MoltShieldURLLoaderThrottle() override;

  // blink::URLLoaderThrottle:
  void DetachFromCurrentSequence() override;
  void WillStartRequest(network::ResourceRequest* request,
                        bool* defer) override;
  void WillRedirectRequest(
      net::RedirectInfo* redirect_info,
      const network::mojom::URLResponseHead& response_head,
      bool* defer,
      network::HttpRequestHeadersUpdateParams* headers_update_params) override;

 private:
  // Defers and starts a check of `url`; returns false if `url` isn't
  // filtered (the load continues undeferred).
  bool MaybeCheck(const GURL& url, bool* defer);
  void OnChecked(CheckResult result);

  const content::FrameTreeNodeId frame_tree_node_id_;
  std::optional<url::Origin> initiator_;
  std::string request_type_;
  std::string method_;
  base::WeakPtrFactory<MoltShieldURLLoaderThrottle> weak_factory_{this};
};

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_URL_LOADER_THROTTLE_H_
