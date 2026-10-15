// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_tab_helper.h"

#include <algorithm>
#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "chrome/browser/molt_ai/shield/molt_shield_navigation_throttle.h"
#include "chrome/browser/molt_ai/shield/molt_shield_service.h"
#include "chrome/browser/molt_ai/shield/molt_shield_site_settings.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"

namespace molt_ai::shield {

namespace {

// Browser-side bounds on what one renderer request may submit (the Rust
// side bounds again).
constexpr size_t kMaxTokens = 5000;

std::set<MoltShieldTabHelper*>& AllHelpers() {
  static base::NoDestructor<std::set<MoltShieldTabHelper*>> helpers;
  return *helpers;
}

}  // namespace

MoltShieldTabHelper::MoltShieldTabHelper(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<MoltShieldTabHelper>(*web_contents) {
  AllHelpers().insert(this);
}

MoltShieldTabHelper::~MoltShieldTabHelper() {
  AllHelpers().erase(this);
}

// static
std::vector<MoltShieldTabHelper*> MoltShieldTabHelper::GetAll() {
  return std::vector<MoltShieldTabHelper*>(AllHelpers().begin(),
                                           AllHelpers().end());
}

int64_t MoltShieldTabHelper::GetBlockedCount() {
  return GetBlockedCountForPage(web_contents()->GetPrimaryPage());
}

void MoltShieldTabHelper::ReadyToCommitNavigation(
    content::NavigationHandle* navigation_handle) {
  ShieldNavigationData* stash =
      ShieldNavigationData::GetForNavigationHandle(*navigation_handle);
  if (!stash || stash->url.GetWithoutRef() !=
                    navigation_handle->GetURL().GetWithoutRef()) {
    return;
  }
  content::RenderFrameHost* frame = navigation_handle->GetRenderFrameHost();
  if (!frame) {
    return;
  }

  mojo::PendingRemote<molt_shield::mojom::CosmeticFiltersHost> host;
  if (!stash->data.generichide) {
    host_receivers_.Add(this, host.InitWithNewPipeAndPassReceiver(),
                        HostContext{stash->url, frame->GetGlobalId()});
  }

  // Channel-associated: delivered before the CommitNavigation IPC that
  // follows ReadyToCommitNavigation on the same channel.
  mojo::AssociatedRemote<molt_shield::mojom::CosmeticFiltersAgent> agent;
  frame->GetRemoteAssociatedInterfaces()->GetInterface(&agent);
  agent->ApplyDocumentResources(
      stash->url,
      molt_shield::mojom::CosmeticResources::New(
          stash->data.hide_css, stash->data.scriptlet_js,
          stash->data.generichide, stash->data.procedural_json,
          stash->data.procedural_styles),
      std::move(host));
  MoltShieldService::Get()->RecordCosmetic(stash->data);
}

void MoltShieldTabHelper::HiddenClassIdSelectors(
    const std::vector<std::string>& classes,
    const std::vector<std::string>& ids,
    HiddenClassIdSelectorsCallback callback) {
  const HostContext& context = host_receivers_.current_context();
  content::RenderFrameHost* frame =
      content::RenderFrameHost::FromID(context.frame_id);
  MoltShieldService* service = MoltShieldService::Get();
  // Only answer for the document this endpoint was handed to.
  if (!frame || !service->IsCosmeticEnabled() ||
      frame->GetLastCommittedURL().GetWithoutRef() !=
          context.document_url.GetWithoutRef()) {
    std::move(callback).Run(std::string());
    return;
  }
  std::vector<std::string> bounded_classes(
      classes.begin(), classes.begin() + std::min(classes.size(), kMaxTokens));
  std::vector<std::string> bounded_ids(
      ids.begin(), ids.begin() + std::min(ids.size(), kMaxTokens));
  service->GetHiddenClassIdCss(
      context.document_url, std::move(bounded_classes), std::move(bounded_ids),
      base::BindOnce(
          [](HiddenClassIdSelectorsCallback callback, std::string css) {
            std::move(callback).Run(css);
          },
          std::move(callback)));
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(MoltShieldTabHelper);

}  // namespace molt_ai::shield
