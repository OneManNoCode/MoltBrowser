// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/shared_worker_key.h"

#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/workers/worker_clients.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/supplementable.h"

namespace blink::molt_fingerprinting {

namespace {

// The storage key a shared worker was constructed with. Made on the main
// thread before the worker thread starts, then only read on the worker's
// thread (the usual WorkerClients pattern). A StorageKey is a plain value
// (url::Origin, net::SchemefulSite, an optional nonce: no Blink strings or
// ref counts), and copying it here generates the lazily created nonces of
// any opaque origin in it (url::Origin::Nonce), so the worker thread never
// writes to it.
class MoltSharedWorkerStorageKey final
    : public GarbageCollected<MoltSharedWorkerStorageKey>,
      public Supplement<WorkerClients> {
 public:
  static const char kSupplementName[];

  MoltSharedWorkerStorageKey(WorkerClients& clients,
                             const StorageKey& storage_key)
      : Supplement<WorkerClients>(clients), storage_key_(storage_key) {}

  const StorageKey& storage_key() const { return storage_key_; }

  void Trace(Visitor* visitor) const override {
    Supplement<WorkerClients>::Trace(visitor);
  }

 private:
  const StorageKey storage_key_;
};

const char MoltSharedWorkerStorageKey::kSupplementName[] =
    "MoltSharedWorkerStorageKey";

}  // namespace

void ProvideToSharedWorker(const StorageKey& constructor_key,
                           WorkerClients* worker_clients) {
  if (!worker_clients || GetMode() == Mode::kOff) {
    return;
  }
  Supplement<WorkerClients>::ProvideTo(
      *worker_clients, MakeGarbageCollected<MoltSharedWorkerStorageKey>(
                           *worker_clients, constructor_key));
}

const StorageKey* SharedWorkerStorageKey(WorkerClients* worker_clients) {
  if (!worker_clients) {
    return nullptr;
  }
  const MoltSharedWorkerStorageKey* handed_over =
      Supplement<WorkerClients>::From<MoltSharedWorkerStorageKey>(
          *worker_clients);
  return handed_over ? &handed_over->storage_key() : nullptr;
}

}  // namespace blink::molt_fingerprinting
