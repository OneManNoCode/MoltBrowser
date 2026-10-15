// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// CosmeticFilterAgent — renderer half of MoltShield cosmetic filtering.
//
// The browser sends each document's resources (hide CSS + scriptlets) over a
// channel-associated interface at ReadyToCommitNavigation, so they arrive
// before the commit. When the new document element is created — before any
// page script runs, the same point extension document_start scripts use —
// the agent:
//   * inserts the hide CSS as a *user* stylesheet (not in the DOM, so pages
//     can't remove it; user !important beats author !important), and
//   * runs the scriptlets in the page's MAIN world via
//     WebLocalFrame::ExecuteScript (not a <script> element, so the page's CSP
//     can't block it and nothing is left in the DOM).
// Then, unless the page is `$generichide`, it collects the classes and ids
// in the document at DOMContentLoaded, at load, and once shortly after, and
// asks the browser which generic `##.class` / `###id` rules apply.

#ifndef CHROME_RENDERER_MOLT_AI_SHIELD_COSMETIC_FILTER_AGENT_H_
#define CHROME_RENDERER_MOLT_AI_SHIELD_COSMETIC_FILTER_AGENT_H_

#include <set>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "chrome/common/molt_ai/shield/cosmetic_filters.mojom.h"
#include "content/public/renderer/render_frame_observer.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "url/gurl.h"

namespace molt_ai::shield {

class CosmeticFilterAgent : public content::RenderFrameObserver,
                            public molt_shield::mojom::CosmeticFiltersAgent {
 public:
  // Creates the agent for `render_frame` unless the kMoltShieldCosmetics kill
  // switch (chrome://flags/#molt-shield-cosmetics) is off or
  // --disable-molt-shield is set, in which case nothing is attached to the
  // frame.
  static void MaybeCreate(content::RenderFrame* render_frame);

  // Self-owned: deleted when the frame goes away.
  explicit CosmeticFilterAgent(content::RenderFrame* render_frame);
  CosmeticFilterAgent(const CosmeticFilterAgent&) = delete;
  CosmeticFilterAgent& operator=(const CosmeticFilterAgent&) = delete;
  ~CosmeticFilterAgent() override;

  // content::RenderFrameObserver:
  void DidCreateDocumentElement() override;
  void DidDispatchDOMContentLoadedEvent() override;
  void DidFinishLoad() override;
  void OnDestruct() override;

  // molt_shield::mojom::CosmeticFiltersAgent:
  void ApplyDocumentResources(
      const GURL& document_url,
      molt_shield::mojom::CosmeticResourcesPtr resources,
      mojo::PendingRemote<molt_shield::mojom::CosmeticFiltersHost> host)
      override;

 private:
  void BindReceiver(
      mojo::PendingAssociatedReceiver<molt_shield::mojom::CosmeticFiltersAgent>
          receiver);
  void ResetDocumentState();
  void ScanClassesAndIds();
  void OnHiddenCss(uint64_t document_sequence, const std::string& css);
  void InsertUserStyleSheet(const std::string& css);

  mojo::AssociatedReceiverSet<molt_shield::mojom::CosmeticFiltersAgent>
      receivers_;

  // Resources for the document about to commit.
  GURL pending_url_;
  molt_shield::mojom::CosmeticResourcesPtr pending_;
  mojo::PendingRemote<molt_shield::mojom::CosmeticFiltersHost> pending_host_;

  // Current document.
  uint64_t document_sequence_ = 0;
  bool generic_pass_ = false;
  mojo::Remote<molt_shield::mojom::CosmeticFiltersHost> host_;
  std::set<std::string> sent_classes_;
  std::set<std::string> sent_ids_;
  int scans_ = 0;
  base::OneShotTimer late_scan_timer_;

  base::WeakPtrFactory<CosmeticFilterAgent> weak_factory_{this};
};

}  // namespace molt_ai::shield

#endif  // CHROME_RENDERER_MOLT_AI_SHIELD_COSMETIC_FILTER_AGENT_H_
