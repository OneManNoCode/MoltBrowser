// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Renderer half of fingerprinting protection's per-document Shields decision
// (docs/FINGERPRINTING.md, "Shields off"). One per frame, in renderers whose
// mode is farble:
//
//   * the browser sends ShieldsDecision.SetForNextDocument from
//     ReadyToCommitNavigation (chrome/browser/molt_ai/fingerprinting/
//     molt_shields_decision.h). The receiver is bound to the frame's
//     associated interfaces, whose messages ride the renderer's channel in
//     order with the NavigationClient's CommitNavigation that follows it, so
//     it is always here before the commit;
//   * DidCreateNewDocument (from RenderFrameImpl::DidCommitNavigation and
//     DidCommitDocumentReplacementNavigation, after Blink installed the new
//     document and before its parser or any of its script runs) hands Blink
//     the frame's latest decision, or, in a frame that never got one, has
//     Blink inherit the parent's or opener's;
//   * the initial empty document exists before this agent, so it inherits
//     when the agent is created (RenderFrameCreated).

#ifndef CHROME_RENDERER_MOLT_AI_FINGERPRINTING_MOLT_SHIELDS_DECISION_AGENT_H_
#define CHROME_RENDERER_MOLT_AI_FINGERPRINTING_MOLT_SHIELDS_DECISION_AGENT_H_

#include <optional>
#include <string>

#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting.mojom.h"
#include "content/public/renderer/render_frame_observer.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"

namespace content {
class RenderFrame;
}  // namespace content

namespace molt_ai::fingerprinting {

class ShieldsDecisionAgent : public content::RenderFrameObserver,
                             public mojom::ShieldsDecision {
 public:
  // Called from ChromeContentRendererClient::RenderFrameCreated. Creates the
  // agent (owned by the frame) when this renderer's mode is farble.
  static void MaybeCreate(content::RenderFrame* render_frame);

  ShieldsDecisionAgent(const ShieldsDecisionAgent&) = delete;
  ShieldsDecisionAgent& operator=(const ShieldsDecisionAgent&) = delete;

 private:
  explicit ShieldsDecisionAgent(content::RenderFrame* render_frame);
  ~ShieldsDecisionAgent() override;

  // content::RenderFrameObserver:
  void DidCreateNewDocument() override;
  void OnDestruct() override;

  // mojom::ShieldsDecision:
  void SetForNextDocument(bool shields_down,
                          const std::string& top_level_site) override;

  void Bind(mojo::PendingAssociatedReceiver<mojom::ShieldsDecision> receiver);

  struct Decision {
    bool shields_down = false;
    std::string top_level_site;
  };
  // The latest decision the browser sent for this frame.
  std::optional<Decision> latest_;
  // The browser opens a new endpoint per decision.
  mojo::AssociatedReceiverSet<mojom::ShieldsDecision> receivers_;
};

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_RENDERER_MOLT_AI_FINGERPRINTING_MOLT_SHIELDS_DECISION_AGENT_H_
