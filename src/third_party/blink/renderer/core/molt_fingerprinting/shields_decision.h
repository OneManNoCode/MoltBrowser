// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the browser's Shields decision, per
// document and per worker ("Shields off" = MoltShield turned off by the user
// for a site; docs/FINGERPRINTING.md, "Shields off").
//
// The browser decides, for every document it commits and for every service
// or shared worker it starts, whether MoltShield is off for the context's
// top-level site, and sends only that bit, only to the renderer that gets
// the document or the worker. No renderer holds the profile's list of
// Shields-off sites. Where each context's decision comes from:
//
//   documents the browser commits
//     The browser sends molt_ai.fingerprinting.mojom.ShieldsDecision to the
//     committing frame from ReadyToCommitNavigation, ordered before the
//     commit on the same channel; the embedder (chrome/renderer/molt_ai/
//     fingerprinting/molt_shields_decision_agent.cc) hands it to Blink when
//     the document is created, before any of its script can run
//     (WebMoltFingerprinting::SetDocumentShieldsDecision ->
//     SetWindowShieldsDecision).
//   documents the browser doesn't commit
//     The initial empty document of a new frame or window, and synchronous
//     about:blank, javascript: URL and XSLT documents in a frame that never
//     had a decision: the parent frame's decision, or else the opener's
//     (InheritWindowShieldsDecision).
//   dedicated workers (nested too)
//     Their creator's, handed over when they are created
//     (ProvideShieldsDecisionToDedicatedWorker).
//   worklets
//     Their creator's farbling key, or none (ProvideToWorklet in
//     molt_fingerprinting.h already folds the decision in).
//   service workers
//     Decided by the browser when it starts the worker, sent with the
//     storage key in InitializeGlobalScope (SetServiceWorkerStorageKey in
//     molt_fingerprinting.h -> SetWorkerShieldsDecision).
//   shared workers
//     Decided by the browser when it starts the worker, sent in the
//     worker's start metadata (ProvideToSharedWorker in
//     shared_worker_key.h -> ProvideShieldsDecisionToWorker).
//
// Rules:
//   * Fail safe: a context without a decision is farbled. Farbling can break
//     a page; it never gives a fingerprint away.
//   * A window's decision counts only for the top-level site it was made
//     for: the site of the top frame of the window's frame tree (the
//     precursor's, for a sandboxed top-level document). A decision that
//     reaches a document of another site (a race between a commit and a
//     document the renderer creates on its own, or anything unexpected)
//     farbles it.
//   * Only in kFarble mode. MoltNet (kUniform) farbles where Shields are
//     off: there the point is to look like every other MoltNet user.
//   * A decision is fixed for the life of its context: turning Shields off
//     or on applies from the next document (MoltShield reloads the page) and
//     the next worker start.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_SHIELDS_DECISION_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_SHIELDS_DECISION_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class ExecutionContext;
class LocalDOMWindow;
class WorkerClients;

namespace molt_fingerprinting {

// --- Documents (main thread) ------------------------------------------------

// Records what the browser decided for the document `window` now shows:
// whether MoltShield is off (`shields_down`) for `top_level_site`, as
// net::SchemefulSite serializes it ("https://example.com"). Replaces any
// earlier decision of `window` (a reused initial empty document's window).
CORE_EXPORT void SetWindowShieldsDecision(LocalDOMWindow& window,
                                          bool shields_down,
                                          const String& top_level_site);

// For a document the browser didn't commit: copies the decision of the
// parent frame's document, or else of the opener's, if that frame is in this
// process; otherwise `window` has no decision (farbled).
CORE_EXPORT void InheritWindowShieldsDecision(LocalDOMWindow& window);

// The top-level site a decision for `window` must have been made for (see
// above), or a null string when there is none (detached, opaque without a
// precursor).
CORE_EXPORT String ShieldsDecisionSiteOf(const LocalDOMWindow& window);

// --- Workers ----------------------------------------------------------------

// On the creator's thread, when a dedicated worker is created and before its
// thread starts: the worker inherits `creator`'s decision.
CORE_EXPORT void ProvideShieldsDecisionToDedicatedWorker(
    ExecutionContext& creator,
    WorkerClients* worker_clients);

// The same hand-over with a decision the browser made (shared workers).
// Nothing is recorded unless `shields_down`.
CORE_EXPORT void ProvideShieldsDecisionToWorker(bool shields_down,
                                                WorkerClients* worker_clients);

// On a service worker's thread, before its script runs: the browser's
// decision for the top-level site of its storage key.
CORE_EXPORT void SetWorkerShieldsDecision(ExecutionContext& worker,
                                          bool shields_down);

// --- Reading ----------------------------------------------------------------

// True if the browser decided that MoltShield is off for `context` and the
// process farbles (kFarble). False for contexts without a decision, worklets
// (their handed-over key carries their creator's decision) and in kOff and
// kUniform mode. Main thread or the context's thread.
CORE_EXPORT bool AreShieldsDown(ExecutionContext& context);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_SHIELDS_DECISION_H_
