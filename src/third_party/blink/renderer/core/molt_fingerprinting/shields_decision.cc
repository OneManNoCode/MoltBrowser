// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/shields_decision.h"

#include <string>

#include "base/memory/scoped_refptr.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_molt_fingerprinting.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/execution_context/security_context.h"
#include "third_party/blink/renderer/core/frame/frame.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/page/frame_tree.h"
#include "third_party/blink/renderer/core/workers/worker_clients.h"
#include "third_party/blink/renderer/core/workers/worker_global_scope.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/network/blink_schemeful_site.h"
#include "third_party/blink/renderer/platform/supplementable.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"

namespace blink {

namespace {

// The window `web_frame` shows now: at DidCreateNewDocument, the new one.
LocalDOMWindow* CurrentWindowOfWebFrame(WebLocalFrame* web_frame) {
  if (!web_frame) {
    return nullptr;
  }
  LocalFrame* frame = To<WebLocalFrameImpl>(web_frame)->GetFrame();
  return frame ? frame->DomWindow() : nullptr;
}

}  // namespace

// static
void WebMoltFingerprinting::SetDocumentShieldsDecision(
    WebLocalFrame* frame,
    bool shields_down,
    const std::string& top_level_site) {
  if (LocalDOMWindow* window = CurrentWindowOfWebFrame(frame)) {
    molt_fingerprinting::SetWindowShieldsDecision(
        *window, shields_down, String::FromUtf8(top_level_site));
  }
}

// static
void WebMoltFingerprinting::InheritDocumentShieldsDecision(
    WebLocalFrame* frame) {
  if (LocalDOMWindow* window = CurrentWindowOfWebFrame(frame)) {
    molt_fingerprinting::InheritWindowShieldsDecision(*window);
  }
}

namespace molt_fingerprinting {

namespace {

// The decision of a window (main thread) or of a service worker (its
// thread).
class MoltShieldsDecision final
    : public GarbageCollected<MoltShieldsDecision>,
      public Supplement<ExecutionContext> {
 public:
  static const char kSupplementName[];

  static MoltShieldsDecision& Ensure(ExecutionContext& context) {
    MoltShieldsDecision* decision =
        Supplement<ExecutionContext>::From<MoltShieldsDecision>(context);
    if (!decision) {
      decision = MakeGarbageCollected<MoltShieldsDecision>(context);
      Supplement<ExecutionContext>::ProvideTo(context, decision);
    }
    return *decision;
  }

  explicit MoltShieldsDecision(ExecutionContext& context)
      : Supplement<ExecutionContext>(context) {}

  void Trace(Visitor* visitor) const override {
    Supplement<ExecutionContext>::Trace(visitor);
  }

  // What the browser decided, or what the window inherited, and the
  // top-level site it was decided for (null for service workers): frames
  // that inherit it check the site again against their own.
  bool decided_down = false;
  String decided_for;
  // `decided_down`, if it was decided for this context's top-level site.
  bool shields_down = false;
};

const char MoltShieldsDecision::kSupplementName[] = "MoltShieldsDecision";

// Present in a dedicated or shared worker's WorkerClients when Shields are
// down for it. Made on the creator's (or the browser-started worker's main)
// thread before the worker thread starts, then only read on the worker's
// thread (the usual WorkerClients pattern). Absent: farbled.
class MoltShieldsDownForWorker final
    : public GarbageCollected<MoltShieldsDownForWorker>,
      public Supplement<WorkerClients> {
 public:
  static const char kSupplementName[];

  explicit MoltShieldsDownForWorker(WorkerClients& clients)
      : Supplement<WorkerClients>(clients) {}

  void Trace(Visitor* visitor) const override {
    Supplement<WorkerClients>::Trace(visitor);
  }
};

const char MoltShieldsDownForWorker::kSupplementName[] =
    "MoltShieldsDownForWorker";

}  // namespace

String ShieldsDecisionSiteOf(const LocalDOMWindow& window) {
  const LocalFrame* frame = window.GetFrame();
  if (!frame) {
    return String();
  }
  // The top frame of the window's frame tree, local or remote (remote frames
  // carry their replicated origin). A fenced frame is its own tree's top, as
  // the browser's RenderFrameHost::GetMainFrame() sees it.
  const SecurityContext* top = frame->Tree().Top().GetSecurityContext();
  const SecurityOrigin* origin = top ? top->GetSecurityOrigin() : nullptr;
  if (!origin) {
    return String();
  }
  // A sandboxed top-level document is still its site's page, as for the
  // browser (url::Origin::GetTupleOrPrecursorTupleIfOpaque).
  const SecurityOrigin* site_origin =
      origin->GetOriginOrPrecursorOriginIfOpaque();
  if (!site_origin || site_origin->IsOpaque()) {
    return String();
  }
  return BlinkSchemefulSite(scoped_refptr<const SecurityOrigin>(site_origin))
      .Serialize();
}

void SetWindowShieldsDecision(LocalDOMWindow& window,
                              bool shields_down,
                              const String& top_level_site) {
  MoltShieldsDecision& decision = MoltShieldsDecision::Ensure(window);
  decision.decided_down = shields_down;
  decision.decided_for = top_level_site;
  const String site = ShieldsDecisionSiteOf(window);
  decision.shields_down =
      shields_down && !site.IsNull() && site == top_level_site;
}

void InheritWindowShieldsDecision(LocalDOMWindow& window) {
  bool shields_down = false;
  String decided_for;
  if (const LocalFrame* frame = window.GetFrame()) {
    // The parent: same page, so same top-level site. A main frame (a popup's
    // initial empty document): its opener, whose document's origin it took.
    const Frame* source = frame->Tree().Parent();
    if (!source) {
      source = frame->Opener();
    }
    const auto* local_source = DynamicTo<LocalFrame>(source);
    const LocalDOMWindow* source_window =
        local_source ? local_source->DomWindow() : nullptr;
    if (source_window && source_window != &window) {
      if (const MoltShieldsDecision* source_decision =
              Supplement<ExecutionContext>::From<MoltShieldsDecision>(
                  *source_window)) {
        shields_down = source_decision->decided_down;
        decided_for = source_decision->decided_for;
      }
    }
  }
  SetWindowShieldsDecision(window, shields_down, decided_for);
}

void ProvideShieldsDecisionToDedicatedWorker(ExecutionContext& creator,
                                             WorkerClients* worker_clients) {
  ProvideShieldsDecisionToWorker(AreShieldsDown(creator), worker_clients);
}

void ProvideShieldsDecisionToWorker(bool shields_down,
                                    WorkerClients* worker_clients) {
  if (!shields_down || !worker_clients ||
      Supplement<WorkerClients>::From<MoltShieldsDownForWorker>(
          *worker_clients)) {
    return;
  }
  Supplement<WorkerClients>::ProvideTo(
      *worker_clients,
      MakeGarbageCollected<MoltShieldsDownForWorker>(*worker_clients));
}

void SetWorkerShieldsDecision(ExecutionContext& worker, bool shields_down) {
  MoltShieldsDecision& decision = MoltShieldsDecision::Ensure(worker);
  decision.decided_down = shields_down;
  decision.decided_for = String();
  decision.shields_down = shields_down;
}

bool AreShieldsDown(ExecutionContext& context) {
  // MoltNet keeps farbling where Shields are off; kOff farbles nothing.
  if (GetMode() != Mode::kFarble) {
    return false;
  }
  // Windows and service workers.
  if (const MoltShieldsDecision* decision =
          Supplement<ExecutionContext>::From<MoltShieldsDecision>(context)) {
    return decision->shields_down;
  }
  // Dedicated and shared workers.
  if (const auto* worker = DynamicTo<WorkerGlobalScope>(context)) {
    const WorkerClients* clients = worker->Clients();
    return clients &&
           Supplement<WorkerClients>::From<MoltShieldsDownForWorker>(*clients);
  }
  return false;
}

}  // namespace molt_fingerprinting
}  // namespace blink
