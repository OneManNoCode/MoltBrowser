// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/renderer/molt_ai/fingerprinting/molt_shields_decision_agent.h"

#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_switches.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_molt_fingerprinting.h"

namespace molt_ai::fingerprinting {

namespace {

// Shields decisions only matter in farble mode: MoltNet (uniform) farbles
// where Shields are off, and with protection off nothing is farbled.
bool IsFarbleRenderer() {
  static const bool farble =
      ModeFromSwitchValue(
          base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
              switches::kMode)) == RendererMode::kFarble;
  return farble;
}

}  // namespace

// static
void ShieldsDecisionAgent::MaybeCreate(content::RenderFrame* render_frame) {
  if (render_frame && IsFarbleRenderer()) {
    new ShieldsDecisionAgent(render_frame);  // Deletes itself in OnDestruct.
  }
}

ShieldsDecisionAgent::ShieldsDecisionAgent(content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {
  render_frame->GetAssociatedInterfaceRegistry()
      ->AddInterface<mojom::ShieldsDecision>(base::BindRepeating(
          &ShieldsDecisionAgent::Bind, base::Unretained(this)));
  // The initial empty document was created with the frame, before any
  // observer existed: it takes its parent's or opener's decision.
  blink::WebMoltFingerprinting::InheritDocumentShieldsDecision(
      render_frame->GetWebFrame());
}

ShieldsDecisionAgent::~ShieldsDecisionAgent() = default;

void ShieldsDecisionAgent::DidCreateNewDocument() {
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (latest_) {
    // The document the browser committed after sending `latest_`, or one
    // the renderer made in its place later (javascript: URL, XSLT): Blink
    // keeps the decision only if it was made for the document's own
    // top-level site.
    blink::WebMoltFingerprinting::SetDocumentShieldsDecision(
        frame, latest_->shields_down, latest_->top_level_site);
  } else {
    // A frame the browser never committed into: a synchronous about:blank,
    // javascript: URL or XSLT document replacing its initial empty one.
    blink::WebMoltFingerprinting::InheritDocumentShieldsDecision(frame);
  }
}

void ShieldsDecisionAgent::OnDestruct() {
  delete this;
}

void ShieldsDecisionAgent::SetForNextDocument(
    bool shields_down,
    const std::string& top_level_site) {
  latest_ = Decision{shields_down, top_level_site};
}

void ShieldsDecisionAgent::Bind(
    mojo::PendingAssociatedReceiver<mojom::ShieldsDecision> receiver) {
  // Like every channel-associated receiver of a frame. The channel itself
  // dispatches this frame's associated messages in the order they were sent
  // (ChannelAssociatedGroupController posts them all to the IPC task
  // runner), which is what puts the decision before the commit.
  receivers_.Add(this, std::move(receiver),
                 render_frame()->GetTaskRunner(
                     blink::TaskType::kInternalNavigationAssociated));
}

}  // namespace molt_ai::fingerprinting
