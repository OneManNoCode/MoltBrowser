// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_throttle.h"

#include "base/logging.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace molt_ai {

namespace {

// Converts network::mojom::RequestDestination to MoltShield ResourceType.
ResourceType ToMoltResourceType(
    network::mojom::RequestDestination destination) {
  switch (destination) {
    case network::mojom::RequestDestination::kDocument:
      return ResourceType::DOCUMENT;
    case network::mojom::RequestDestination::kIframe:
      return ResourceType::SUBDOCUMENT;
    case network::mojom::RequestDestination::kStyle:
      return ResourceType::STYLESHEET;
    case network::mojom::RequestDestination::kScript:
    case network::mojom::RequestDestination::kWorker:
    case network::mojom::RequestDestination::kSharedWorker:
    case network::mojom::RequestDestination::kServiceWorker:
      return ResourceType::SCRIPT;
    case network::mojom::RequestDestination::kImage:
      return ResourceType::IMAGE;
    case network::mojom::RequestDestination::kFont:
      return ResourceType::FONT;
    case network::mojom::RequestDestination::kObject:
    case network::mojom::RequestDestination::kEmbed:
      return ResourceType::OBJECT;
    case network::mojom::RequestDestination::kAudio:
    case network::mojom::RequestDestination::kVideo:
    case network::mojom::RequestDestination::kTrack:
      return ResourceType::MEDIA;
    default:
      return ResourceType::OTHER;
  }
}

}  // namespace

MoltShieldURLLoaderThrottle::MoltShieldURLLoaderThrottle(
    MoltShield* shield,
    const GURL& first_party_url)
    : shield_(shield), first_party_url_(first_party_url) {}

MoltShieldURLLoaderThrottle::~MoltShieldURLLoaderThrottle() = default;

// static
std::unique_ptr<MoltShieldURLLoaderThrottle>
MoltShieldURLLoaderThrottle::MaybeCreate(MoltShield* shield,
                                          const GURL& first_party_url) {
  if (!shield) {
    return nullptr;
  }
  return std::make_unique<MoltShieldURLLoaderThrottle>(shield,
                                                        first_party_url);
}

void MoltShieldURLLoaderThrottle::WillStartRequest(
    network::ResourceRequest* request,
    bool* defer) {
  if (!shield_) {
    return;
  }

  const GURL& url = request->url;

  // Skip chrome:// and chrome-extension:// URLs
  if (url.SchemeIs("chrome") || url.SchemeIs("chrome-extension") ||
      url.SchemeIs("chrome-untrusted") || url.SchemeIs("devtools") ||
      url.SchemeIs("data") || url.SchemeIs("blob")) {
    return;
  }

  ResourceType resource_type = ToMoltResourceType(request->destination);

  // Use the request's referrer as first-party if available
  GURL first_party = first_party_url_;
  if (request->referrer.is_valid()) {
    first_party = request->referrer;
  }

  CheckURL(url, first_party, resource_type);
}

void MoltShieldURLLoaderThrottle::WillRedirectRequest(
    net::RedirectInfo* redirect_info,
    const network::mojom::URLResponseHead& response_head,
    bool* defer,
    std::vector<std::string>* to_be_removed_request_headers,
    net::HttpRequestHeaders* modified_request_headers,
    net::HttpRequestHeaders* modified_cors_exempt_request_headers) {
  if (!shield_) {
    return;
  }

  const GURL& new_url = redirect_info->new_url;

  // Skip internal URLs
  if (new_url.SchemeIs("chrome") || new_url.SchemeIs("chrome-extension") ||
      new_url.SchemeIs("data") || new_url.SchemeIs("blob")) {
    return;
  }

  // Check the redirect URL against filters
  CheckURL(new_url, first_party_url_, ResourceType::OTHER);
}

void MoltShieldURLLoaderThrottle::DetachFromCurrentSequence() {
  // MoltShield is thread-safe for reads, so nothing special needed.
}

void MoltShieldURLLoaderThrottle::CheckURL(const GURL& url,
                                            const GURL& first_party,
                                            ResourceType resource_type) {
  BlockResult result = shield_->ShouldBlockRequest(
      url.spec(), first_party.spec(), resource_type);

  if (result.should_block) {
    VLOG(1) << "[MoltShield] Blocked: " << url.spec()
            << " (rule: " << result.matched_rule << ")";
    delegate_->CancelWithError(net::ERR_BLOCKED_BY_CLIENT,
                               "MoltShield: ad/tracker blocked");
    return;
  }

  if (result.should_redirect && !result.redirect_url.empty()) {
    VLOG(1) << "[MoltShield] Redirecting: " << url.spec()
            << " -> " << result.redirect_url;
    // TODO: Implement redirect via delegate when needed
  }
}

}  // namespace molt_ai
