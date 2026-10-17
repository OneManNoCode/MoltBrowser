// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the key shared workers farble with.
//
// A shared worker farbles with the key of its storage key's top-level site
// (plus the storage key's nonce, if any), exactly like documents and service
// workers (docs/FINGERPRINTING.md, "Shared workers"). A third-party shared
// worker under a.example therefore gets a.example's key, the same as the
// pages it serves, and keeps it across restarts.
//
// Blink is only told a shared worker's constructor *origin*, so the storage
// key travels separately, all of it computed by the browser:
//
//   SharedWorkerHost::Start
//     -> SharedWorkerFactory.CreateSharedWorker(constructor_key, ...)
//        (the creator's storage key, which also partitions the worker)
//   EmbeddedSharedWorkerStub (content)
//     -> WebSharedWorker::CreateAndStart(..., constructor_key)
//   WebSharedWorkerImpl::StartWorkerContext (main thread)
//     -> ProvideToSharedWorker(constructor_key, worker_clients)
//   worker thread: molt_fingerprinting.cc's ResolveSite reads it back
//     through SharedWorkerStorageKey(worker->Clients()).
//
// Only the storage key is handed over; the farbling key is derived the first
// time the worker needs one, with the usual rules (molt_fingerprinting.h):
// Shields-off and non-http(s) top-level sites are not farbled, an opaque
// top-level site gets a random key, and a key needed before the session seed
// arrived stays random for the worker's lifetime.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_SHARED_WORKER_KEY_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_SHARED_WORKER_KEY_H_

#include "third_party/blink/renderer/core/core_export.h"

namespace blink {

class StorageKey;
class WorkerClients;

namespace molt_fingerprinting {

// Main thread, when a shared worker starts and before its thread exists
// (WebSharedWorkerImpl::StartWorkerContext): records the storage key of the
// context that constructed it, as the browser sent it, in the worker's
// `worker_clients`. Does nothing when protection is off or `worker_clients`
// is null.
CORE_EXPORT void ProvideToSharedWorker(const StorageKey& constructor_key,
                                       WorkerClients* worker_clients);

// The storage key ProvideToSharedWorker recorded in `worker_clients`, or null
// (other workers and worklets, protection off, `worker_clients` null). Call it
// on the thread `worker_clients` is attached to: the worker's thread once the
// worker runs.
CORE_EXPORT const StorageKey* SharedWorkerStorageKey(
    WorkerClients* worker_clients);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_SHARED_WORKER_KEY_H_
