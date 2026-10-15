// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_url_loader_throttle.h"

#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/molt_ai/shield/molt_shield_request_utils.h"
#include "chrome/browser/molt_ai/shield/molt_shield_service.h"
#include "chrome/browser/molt_ai/shield/molt_shield_site_settings.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/resource_request.h"

namespace molt_ai::shield {

namespace {

void RecordBlockedOnUI(content::FrameTreeNodeId frame_tree_node_id) {
  content::WebContents* web_contents =
      content::WebContents::FromFrameTreeNodeId(frame_tree_node_id);
  if (web_contents) {
    RecordBlockedForFrame(web_contents->GetPrimaryMainFrame());
  }
}

}  // namespace

// static
std::unique_ptr<MoltShieldURLLoaderThrottle>
MoltShieldURLLoaderThrottle::MaybeCreateForKeepAlive(
    content::BrowserContext* browser_context,
    content::FrameTreeNodeId frame_tree_node_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  MoltShieldService* service = MoltShieldService::Get();
  if (browser_context) {
    service->RegisterProfile(Profile::FromBrowserContext(browser_context));
  }
  if (!service->IsEnabled()) {
    return nullptr;
  }
  // Workers have no frame; filter them (the per-site switch is keyed on the
  // tab's top-level site, which is all the UI exposes).
  if (content::WebContents* web_contents =
          content::WebContents::FromFrameTreeNodeId(frame_tree_node_id)) {
    if (!AreShieldsUpForSite(browser_context,
                             web_contents->GetLastCommittedURL())) {
      return nullptr;
    }
  }
  return std::make_unique<MoltShieldURLLoaderThrottle>(frame_tree_node_id);
}

MoltShieldURLLoaderThrottle::MoltShieldURLLoaderThrottle(
    content::FrameTreeNodeId frame_tree_node_id)
    : frame_tree_node_id_(frame_tree_node_id) {}

MoltShieldURLLoaderThrottle::~MoltShieldURLLoaderThrottle() = default;

void MoltShieldURLLoaderThrottle::DetachFromCurrentSequence() {
  // Pending replies are bound to the old sequence; a throttle is only
  // detached before it starts, when none are outstanding.
  weak_factory_.InvalidateWeakPtrs();
}

void MoltShieldURLLoaderThrottle::WillStartRequest(
    network::ResourceRequest* request,
    bool* defer) {
  initiator_ = request->request_initiator;
  request_type_ = std::string(
      RequestTypeForDestination(request->destination, request->keepalive));
  method_ = request->method;
  MaybeCheck(request->url, defer);
}

void MoltShieldURLLoaderThrottle::WillRedirectRequest(
    net::RedirectInfo* redirect_info,
    const network::mojom::URLResponseHead& response_head,
    bool* defer,
    std::vector<std::string>* to_be_removed_request_headers,
    net::HttpRequestHeaders* modified_request_headers,
    net::HttpRequestHeaders* modified_cors_exempt_request_headers) {
  method_ = redirect_info->new_method;
  MaybeCheck(redirect_info->new_url, defer);
}

bool MoltShieldURLLoaderThrottle::MaybeCheck(const GURL& url, bool* defer) {
  if (request_type_ == kTypeDocument || request_type_ == kTypeSubdocument) {
    return false;
  }
  MoltShieldService* service = MoltShieldService::GetIfExists();
  if (!service || !service->IsEnabled()) {
    return false;
  }
  std::optional<CheckInput> input =
      MakeCheckInput(url, initiator_, request_type_, method_);
  if (!input) {
    return false;
  }
  *defer = true;
  // Replies on this throttle's sequence.
  service->Check(std::move(*input),
                 base::BindOnce(&MoltShieldURLLoaderThrottle::OnChecked,
                                weak_factory_.GetWeakPtr()));
  return true;
}

void MoltShieldURLLoaderThrottle::OnChecked(CheckResult result) {
  if (MoltShieldService* service = MoltShieldService::GetIfExists()) {
    service->RecordCheck(result, request_type_);
  }
  if (!result.should_block) {
    delegate_->Resume();
    return;
  }
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(&RecordBlockedOnUI, frame_tree_node_id_));
  // Keepalive requests can't take a surrogate body; cancel.
  delegate_->CancelWithError(net::ERR_BLOCKED_BY_CLIENT, "MoltShield");
}

}  // namespace molt_ai::shield
