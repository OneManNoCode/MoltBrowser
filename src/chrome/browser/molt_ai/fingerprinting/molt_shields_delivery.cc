// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_shields_delivery.h"

#include <optional>

#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/molt_ai/fingerprinting/molt_fingerprinting_browser.h"
#include "chrome/browser/molt_ai/fingerprinting/molt_shields_decision.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting.mojom.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "url/origin.h"

namespace molt_ai::fingerprinting {

// static
void ShieldsDecisionObserver::MaybeCreateForWebContents(
    content::WebContents* web_contents) {
  if (!web_contents) {
    return;
  }
  if (ModeForBrowserContext(web_contents->GetBrowserContext()) ==
      RendererMode::kFarble) {
    CreateForWebContents(web_contents);
  }
}

ShieldsDecisionObserver::ShieldsDecisionObserver(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<ShieldsDecisionObserver>(*web_contents) {}

ShieldsDecisionObserver::~ShieldsDecisionObserver() = default;

// static
url::Origin ShieldsDecisionObserver::TopFrameOriginForCommit(
    content::NavigationHandle& handle) {
  if (handle.IsInMainFrame()) {
    return handle.GetOriginToCommit().value_or(url::Origin());
  }
  // GetParentFrame() and GetMainFrame() stay inside a fenced frame's tree,
  // like Blink's FrameTree.
  content::RenderFrameHost* parent = handle.GetParentFrame();
  return parent ? parent->GetMainFrame()->GetLastCommittedOrigin()
                : url::Origin();
}

void ShieldsDecisionObserver::ReadyToCommitNavigation(
    content::NavigationHandle* handle) {
  // Same-document navigations keep their document. Back/forward cache
  // restores and prerender activations keep their documents and the
  // decisions those were committed with (they already ran with them).
  if (handle->IsSameDocument() || handle->IsPageActivation()) {
    return;
  }
  content::RenderFrameHost* frame = handle->GetRenderFrameHost();
  if (!frame) {
    return;
  }
  content::BrowserContext* context = frame->GetBrowserContext();
  const RendererMode mode = ModeForBrowserContext(context);
  if (mode != RendererMode::kFarble) {
    return;  // Such renderers have no receiver.
  }
  // Decided now, from the settings as they are now: toggling Shields applies
  // from the next commit.
  const ShieldsDecision decision = DecideForTopFrameOrigin(
      mode, HostContentSettingsMapFactory::GetForProfile(context),
      TopFrameOriginForCommit(*handle));
  // The frame's own associated interfaces: ordered with the commit that
  // follows on the same channel.
  mojo::AssociatedRemote<mojom::ShieldsDecision> remote;
  frame->GetRemoteAssociatedInterfaces()->GetInterface(&remote);
  remote->SetForNextDocument(decision.shields_down, decision.top_level_site);
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(ShieldsDecisionObserver);

bool AreShieldsDownForWorker(content::BrowserContext* context,
                             const blink::StorageKey& storage_key) {
  if (!context) {
    return false;
  }
  return AreShieldsDownForWorker(
      ModeForBrowserContext(context),
      HostContentSettingsMapFactory::GetForProfile(context), storage_key);
}

}  // namespace molt_ai::fingerprinting
