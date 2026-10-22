// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Delivers fingerprinting protection's Shields decision (molt_shields_
// decision.h) to the renderer, per document and per worker
// (docs/FINGERPRINTING.md, "Shields off").
//
// Documents. ShieldsDecisionObserver, attached to every WebContents of a
// farbling profile (AttachUniversalWebContentsObservers), sends the decision
// to the committing frame in ReadyToCommitNavigation, through the
// RenderFrameHost's remote associated interfaces. At 152,
// NavigationRequest::CommitNavigation (and CommitErrorPage) call
// ReadyToCommitNavigation synchronously before RenderFrameHostImpl sends the
// commit over mojom::NavigationClient, and the NavigationClient is itself a
// frame-associated interface on the same channel (for browser-initiated
// navigations it comes from that very provider), so the decision is
// dispatched in the renderer before the commit it precedes, and the
// document is created with it. Same-document navigations create no
// document and are not sent; back/forward cache restores and prerender
// activations keep the documents, and the decisions, they were committed
// with. Documents the renderer creates on its own inherit (Blink,
// shields_decision.h).
//
// Service and shared workers. ContentBrowserClient::
// AreMoltShieldsDownForWorker, asked by content when it starts one, answers
// with AreShieldsDownForWorker() below; the answer travels with the
// worker's storage key (ServiceWorker.InitializeGlobalScope,
// SharedWorkerInfo), before any of its script runs. Dedicated workers and
// worklets inherit their creator's in the renderer.
//
// A decision is fixed for the life of its document or worker: turning
// Shields off or on applies from the next navigation (MoltShield's toggle
// reloads the page) and the next worker start.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_SHIELDS_DELIVERY_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_SHIELDS_DELIVERY_H_

#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

namespace blink {
class StorageKey;
}  // namespace blink

namespace content {
class BrowserContext;
class NavigationHandle;
class WebContents;
}  // namespace content

namespace url {
class Origin;
}  // namespace url

namespace molt_ai::fingerprinting {

class ShieldsDecisionObserver
    : public content::WebContentsObserver,
      public content::WebContentsUserData<ShieldsDecisionObserver> {
 public:
  // Called for every WebContents (AttachUniversalWebContentsObservers).
  // Attaches an observer if its profile's renderers farble (not MoltNet, not
  // with the kill switch off).
  static void MaybeCreateForWebContents(content::WebContents* web_contents);

  ShieldsDecisionObserver(const ShieldsDecisionObserver&) = delete;
  ShieldsDecisionObserver& operator=(const ShieldsDecisionObserver&) = delete;
  ~ShieldsDecisionObserver() override;

  // The origin of the top frame of the frame tree `handle` commits in, as
  // Blink's FrameTree::Top() will see it: for a main frame (outermost, a
  // fenced frame root or a prerendered page's), the origin it commits;
  // otherwise its tree's main frame's committed origin. Valid from
  // ReadyToCommitNavigation on.
  static url::Origin TopFrameOriginForCommit(content::NavigationHandle& handle);

 private:
  friend class content::WebContentsUserData<ShieldsDecisionObserver>;

  explicit ShieldsDecisionObserver(content::WebContents* web_contents);

  // content::WebContentsObserver:
  void ReadyToCommitNavigation(content::NavigationHandle* handle) override;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

// ContentBrowserClient::AreMoltShieldsDownForWorker: the decision for a
// service or shared worker of `context` with `storage_key`. UI thread.
bool AreShieldsDownForWorker(content::BrowserContext* context,
                             const blink::StorageKey& storage_key);

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_SHIELDS_DELIVERY_H_
