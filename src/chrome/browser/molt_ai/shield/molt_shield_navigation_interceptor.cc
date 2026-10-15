// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_navigation_interceptor.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/strcat.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/shield/molt_shield_request_utils.h"
#include "chrome/browser/molt_ai/shield/molt_shield_service.h"
#include "chrome/browser/molt_ai/shield/molt_shield_site_settings.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/molt_ai/shield/molt_shield_features.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/http/http_util.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/origin.h"

namespace molt_ai::shield {

// static
std::unique_ptr<MoltShieldNavigationInterceptor>
MoltShieldNavigationInterceptor::MaybeCreate(
    content::FrameTreeNodeId frame_tree_node_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  // Kill switch (chrome://flags/#molt-shield-removeparam or
  // --disable-molt-shield): no interceptor at all.
  if (!molt_shield::IsRemoveParamEnabled()) {
    return nullptr;
  }
  return std::make_unique<MoltShieldNavigationInterceptor>(frame_tree_node_id);
}

MoltShieldNavigationInterceptor::MoltShieldNavigationInterceptor(
    content::FrameTreeNodeId frame_tree_node_id)
    : frame_tree_node_id_(frame_tree_node_id) {}

MoltShieldNavigationInterceptor::~MoltShieldNavigationInterceptor() = default;

std::optional<CheckInput> MoltShieldNavigationInterceptor::MakeInput(
    const network::ResourceRequest& request,
    content::BrowserContext* browser_context) const {
  if (strips_ >= kMaxParamStripsPerNavigation || !browser_context ||
      !CanStripParamsFromNavigation(request.method, request.url)) {
    return std::nullopt;
  }
  MoltShieldService* service = MoltShieldService::Get();
  service->RegisterProfile(Profile::FromBrowserContext(browser_context));
  if (!service->IsEnabled()) {
    return std::nullopt;
  }

  // Per-site Shields are keyed on the top-level site. For a main-frame
  // navigation that is where this leg is going.
  const bool main_frame =
      request.destination == network::mojom::RequestDestination::kDocument;
  GURL top_frame_url = request.url;
  if (!main_frame) {
    std::optional<url::Origin> top;
    if (request.trusted_params) {
      top = request.trusted_params->isolation_info.top_frame_origin();
    }
    if (top) {
      top_frame_url = top->GetURL();
    } else if (content::WebContents* web_contents =
                   content::WebContents::FromFrameTreeNodeId(
                       frame_tree_node_id_)) {
      top_frame_url = web_contents->GetLastCommittedURL();
    } else {
      return std::nullopt;
    }
  }
  if (!AreShieldsUpForSite(browser_context, top_frame_url)) {
    return std::nullopt;
  }

  // A document is checked in its own context, so `domain=` and party
  // options of removeparam filters refer to the site being visited; a
  // frame in its embedder's.
  std::optional<url::Origin> context =
      main_frame ? std::make_optional(url::Origin::Create(request.url))
                 : request.request_initiator;
  return MakeCheckInput(request.url, context,
                        main_frame ? kTypeDocument : kTypeSubdocument,
                        request.method);
}

void MoltShieldNavigationInterceptor::MaybeCreateLoader(
    const network::ResourceRequest& tentative_resource_request,
    content::BrowserContext* browser_context,
    LoaderCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  std::optional<CheckInput> input =
      MakeInput(tentative_resource_request, browser_context);
  if (!input) {
    std::move(callback).Run({});
    return;
  }
  // Matching runs on MoltShield's engine sequence; the leg waits for it
  // (one lookup, microseconds once the engine is loaded).
  MoltShieldService::Get()->Check(
      std::move(*input),
      base::BindOnce(&MoltShieldNavigationInterceptor::OnChecked,
                     weak_factory_.GetWeakPtr(),
                     tentative_resource_request.url, std::move(callback)));
}

void MoltShieldNavigationInterceptor::OnChecked(GURL url,
                                                LoaderCallback callback,
                                                CheckResult result) {
  // The block verdict is deliberately ignored: navigations are only
  // stripped here, never blocked.
  GURL stripped(result.rewritten_url);
  if (result.rewritten_url.empty() || !IsSafeParamStrip(url, stripped)) {
    std::move(callback).Run({});
    return;
  }
  if (!redirected_to_.insert(stripped).second) {
    // The server sent this navigation back to parameters it was already
    // stripped of once: it insists on them. Load the URL as is (and stop
    // stripping) rather than bounce until ERR_TOO_MANY_REDIRECTS.
    strips_ = kMaxParamStripsPerNavigation;
    std::move(callback).Run({});
    return;
  }
  ++strips_;
  MoltShieldService::Get()->RecordParamsStripped();
  std::move(callback).Run(
      base::BindOnce(&MoltShieldNavigationInterceptor::ServeRedirect,
                     weak_factory_.GetWeakPtr(), std::move(stripped)));
}

void MoltShieldNavigationInterceptor::ServeRedirect(
    const GURL& new_url,
    const network::ResourceRequest& request,
    mojo::PendingReceiver<network::mojom::URLLoader> receiver,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client) {
  // A redirect served on an earlier leg may still be bound.
  receiver_.reset();
  client_.reset();
  receiver_.Bind(std::move(receiver));
  receiver_.set_disconnect_handler(
      base::BindOnce(&MoltShieldNavigationInterceptor::OnConnectionClosed,
                     base::Unretained(this)));
  client_.Bind(std::move(client));

  // Same shape as HttpsUpgradesInterceptor's: a 307 keeps the method, and
  // the redirect info is computed from the request as a server redirect's
  // would be (referrer policy, site-for-cookies update for main frames).
  auto head = network::mojom::URLResponseHead::New();
  head->encoded_data_length = 0;
  head->request_start = base::TimeTicks::Now();
  head->response_start = head->request_start;
  head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
      net::HttpUtil::AssembleRawHeaders(
          base::StrCat({"HTTP/1.1 307 Internal Redirect\nLocation: ",
                        new_url.spec(),
                        "\nNon-Authoritative-Reason: MoltShield\n"})));
  net::RedirectInfo redirect_info = net::RedirectInfo::ComputeRedirectInfo(
      request.method, request.url, request.site_for_cookies,
      request.update_first_party_url_on_redirect
          ? net::RedirectInfo::FirstPartyURLPolicy::UPDATE_URL_ON_REDIRECT
          : net::RedirectInfo::FirstPartyURLPolicy::NEVER_CHANGE_URL,
      request.referrer_policy, request.referrer.spec(),
      request.request_initiator, net::HTTP_TEMPORARY_REDIRECT, new_url,
      /*referrer_policy_header=*/std::nullopt,
      /*insecure_scheme_was_upgraded=*/false);
  client_->OnReceiveRedirect(redirect_info, std::move(head));
}

void MoltShieldNavigationInterceptor::OnConnectionClosed() {
  // The navigation followed the redirect (its loader restarts) or was
  // cancelled.
  receiver_.reset();
  client_.reset();
}

}  // namespace molt_ai::shield
