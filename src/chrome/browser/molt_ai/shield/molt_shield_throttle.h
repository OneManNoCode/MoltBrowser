// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShield URL Loader Throttle — intercepts all network requests
// and blocks ads/trackers using ABP filter list matching.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_THROTTLE_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_THROTTLE_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/molt_ai/shield/molt_shield.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"
#include "url/gurl.h"

namespace molt_ai {

// URLLoaderThrottle that intercepts network requests and blocks
// ads/trackers based on MoltShield filter rules.
class MoltShieldURLLoaderThrottle : public blink::URLLoaderThrottle {
 public:
  // Creates a throttle. |shield| must outlive this throttle.
  // |first_party_url| is the top-level document URL for third-party checks.
  MoltShieldURLLoaderThrottle(MoltShield* shield,
                               const GURL& first_party_url);

  ~MoltShieldURLLoaderThrottle() override;

  MoltShieldURLLoaderThrottle(const MoltShieldURLLoaderThrottle&) = delete;
  MoltShieldURLLoaderThrottle& operator=(const MoltShieldURLLoaderThrottle&) =
      delete;

  // Creates a throttle if MoltShield is available for the given profile.
  // Returns nullptr if shield is not available.
  static std::unique_ptr<MoltShieldURLLoaderThrottle> MaybeCreate(
      MoltShield* shield,
      const GURL& first_party_url);

  // blink::URLLoaderThrottle:
  void WillStartRequest(network::ResourceRequest* request,
                         bool* defer) override;
  void WillRedirectRequest(
      net::RedirectInfo* redirect_info,
      const network::mojom::URLResponseHead& response_head,
      bool* defer,
      std::vector<std::string>* to_be_removed_request_headers,
      net::HttpRequestHeaders* modified_request_headers,
      net::HttpRequestHeaders* modified_cors_exempt_request_headers) override;
  void DetachFromCurrentSequence() override;

 private:
  // Checks if |url| should be blocked by MoltShield.
  void CheckURL(const GURL& url,
                const GURL& first_party,
                ResourceType resource_type);

  raw_ptr<MoltShield> shield_;  // Not owned.
  GURL first_party_url_;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_THROTTLE_H_
