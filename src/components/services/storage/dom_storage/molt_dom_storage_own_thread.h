// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the localStorage and sessionStorage of a partition that keeps
// DOM storage per site in memory (MoltNet's) on a thread of its own, with
// every message that names a storage key handled in its site's turn.
//
// At Chromium 156 the storage service (a utility process on desktop, the
// browser's IO thread on Android and with --single-process) makes every
// partition's LocalStorageImpl and SessionStorageImpl on its one sequence
// (StorageServiceImpl::BindLocalStorageControl(), BindSessionStorageControl()),
// so every profile's and every partition's DOM storage shared it, one task
// for one: a regular window's page flooding localStorage delayed MoltNet's
// storage, and MoltNet's pages delayed the regular window's, a timing
// channel between identities. MoltNet's sites took turns among themselves
// (molt_dom_storage_fair_scheduler.h), not with other partitions.
//
// Here the browser binds such a partition's controls through
// StorageService.MoltBindPerSiteLocalStorageControl() and
// MoltBindPerSiteSessionStorageControl() (DOMStorageContextWrapper), and
// the storage service gives each one a thread of its own
// (MoltDomStorageOwnThreads): a dedicated thread from the thread pool, which
// runs nothing else, so no other partition's tasks queue with it and a busy
// thread pool doesn't hold it up. On it, MoltPerSiteLocalStorage and
// MoltPerSiteSessionStorage own the partition's LocalStorageImpl or
// SessionStorageImpl and its fair scheduler, and receive the browser's
// control messages in a turn of their own (kMoltDomStorageControlGroup):
//  * a message that names a storage key (BindStorageArea(),
//    DeleteStorage()) is queued in its site's turn and handled there, so
//    a page that makes the browser send many (opening frames, binding areas
//    in a loop) takes its own site's turns, never another's;
//  * the others (the namespaces' lifecycle, usage, flushes) are handled in
//    the control's turn, in the order they came; they come from the
//    browser, for the whole partition;
//  * each sessionStorage namespace's pipes (a tab's renderers' Clone()
//    calls) take a turn of their own (MoltDomStorageNamespaceGroup()).
// A message that names a storage key is handled after the control messages
// that came before it (they ran in the control's turn when it was queued),
// and in order with its site's other messages; between two such messages of
// different sites the order may change, as between two sites' pipes.
//
// What a page can still observe: the work of the task in service on
// MoltNet's thread (one message of another site, or one control message),
// and the machine (the processor and the storage service's IO thread, which
// reads every pipe's messages for all partitions), as any page can.

#ifndef COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_OWN_THREAD_H_
#define COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_OWN_THREAD_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/threading/sequence_bound.h"
#include "components/services/storage/dom_storage/molt_dom_storage_fair_scheduler.h"
#include "components/services/storage/public/mojom/local_storage_control.mojom.h"
#include "components/services/storage/public/mojom/session_storage_control.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/dom_storage/session_storage_namespace.mojom.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom.h"

namespace storage {

class LocalStorageImpl;
class SessionStorageImpl;

// A partition's localStorage, kept per site, on the thread it is made on.
class MoltPerSiteLocalStorage : public mojom::LocalStorageControl {
 public:
  // `on_disconnect` runs when the browser closes `receiver`.
  MoltPerSiteLocalStorage(
      mojo::PendingReceiver<mojom::LocalStorageControl> receiver,
      base::OnceClosure on_disconnect);
  MoltPerSiteLocalStorage(const MoltPerSiteLocalStorage&) = delete;
  MoltPerSiteLocalStorage& operator=(const MoltPerSiteLocalStorage&) = delete;
  ~MoltPerSiteLocalStorage() override;

  // mojom::LocalStorageControl:
  void BindStorageArea(
      const blink::StorageKey& storage_key,
      mojo::PendingReceiver<blink::mojom::StorageArea> receiver) override;
  void GetUsage(GetUsageCallback callback) override;
  void DeleteStorage(const blink::StorageKey& storage_key,
                     DeleteStorageCallback callback) override;
  void CleanUpStorage(CleanUpStorageCallback callback) override;
  void Flush() override;
  void PurgeMemory() override;
  void ApplyPolicyUpdates(
      std::vector<mojom::StoragePolicyUpdatePtr> policy_updates) override;
  void ForceKeepSessionState() override;
  void MoltKeepCachesPerSite() override;
  void MoltSetGrowthObserver(
      mojo::PendingRemote<mojom::MoltDomStorageGrowthObserver> observer)
      override;

  LocalStorageImpl* storage_for_testing() { return impl_.get(); }

 private:
  void BindStorageAreaInTurn(
      const blink::StorageKey& storage_key,
      mojo::PendingReceiver<blink::mojom::StorageArea> receiver);
  void DeleteStorageInTurn(const blink::StorageKey& storage_key,
                           DeleteStorageCallback callback);

  const scoped_refptr<MoltDomStorageFairScheduler> scheduler_;
  // The implementation's own control pipe, kept open: its messages come
  // through this object instead.
  mojo::Remote<mojom::LocalStorageControl> impl_pipe_;
  std::unique_ptr<LocalStorageImpl> impl_;
  mojo::Receiver<mojom::LocalStorageControl> receiver_{this};
  base::WeakPtrFactory<MoltPerSiteLocalStorage> weak_factory_{this};
};

// A partition's sessionStorage, kept per site, on the thread it is made on.
class MoltPerSiteSessionStorage : public mojom::SessionStorageControl {
 public:
  MoltPerSiteSessionStorage(
      mojo::PendingReceiver<mojom::SessionStorageControl> receiver,
      base::OnceClosure on_disconnect);
  MoltPerSiteSessionStorage(const MoltPerSiteSessionStorage&) = delete;
  MoltPerSiteSessionStorage& operator=(const MoltPerSiteSessionStorage&) =
      delete;
  ~MoltPerSiteSessionStorage() override;

  // mojom::SessionStorageControl:
  void BindNamespace(
      const std::string& namespace_id,
      mojo::PendingReceiver<blink::mojom::SessionStorageNamespace> receiver)
      override;
  void BindStorageArea(
      const blink::StorageKey& storage_key,
      const std::string& namespace_id,
      mojo::PendingReceiver<blink::mojom::StorageArea> receiver) override;
  void GetUsage(GetUsageCallback callback) override;
  void DeleteStorage(const blink::StorageKey& storage_key,
                     const std::string& namespace_id,
                     DeleteStorageCallback callback) override;
  void CleanUpStorage(CleanUpStorageCallback callback) override;
  void ScavengeUnusedNamespaces() override;
  void Flush() override;
  void PurgeMemory() override;
  void MoltKeepCachesPerSite() override;
  void MoltSetGrowthObserver(
      mojo::PendingRemote<mojom::MoltDomStorageGrowthObserver> observer)
      override;
  void CreateNamespace(const std::string& namespace_id) override;
  void CloneNamespace(const std::string& namespace_id_to_clone,
                      const std::string& clone_namespace_id,
                      mojom::SessionStorageCloneType clone_type) override;
  void DeleteNamespace(const std::string& namespace_id,
                       bool should_persist) override;

  SessionStorageImpl* storage_for_testing() { return impl_.get(); }

 private:
  void BindStorageAreaInTurn(
      const blink::StorageKey& storage_key,
      const std::string& namespace_id,
      mojo::PendingReceiver<blink::mojom::StorageArea> receiver);
  void DeleteStorageInTurn(const blink::StorageKey& storage_key,
                           const std::string& namespace_id,
                           DeleteStorageCallback callback);

  const scoped_refptr<MoltDomStorageFairScheduler> scheduler_;
  mojo::Remote<mojom::SessionStorageControl> impl_pipe_;
  std::unique_ptr<SessionStorageImpl> impl_;
  mojo::Receiver<mojom::SessionStorageControl> receiver_{this};
  base::WeakPtrFactory<MoltPerSiteSessionStorage> weak_factory_{this};
};

// The storage service's per-site partitions, each on a thread of its own.
// Lives on the storage service's sequence (StorageServiceImpl's member).
class MoltDomStorageOwnThreads {
 public:
  MoltDomStorageOwnThreads();
  MoltDomStorageOwnThreads(const MoltDomStorageOwnThreads&) = delete;
  MoltDomStorageOwnThreads& operator=(const MoltDomStorageOwnThreads&) =
      delete;
  ~MoltDomStorageOwnThreads();

  void BindLocalStorageControl(
      mojo::PendingReceiver<mojom::LocalStorageControl> receiver);
  void BindSessionStorageControl(
      mojo::PendingReceiver<mojom::SessionStorageControl> receiver);

  size_t CountForTesting() const { return locals_.size() + sessions_.size(); }

 private:
  // The browser closed control `id`'s pipe: it goes, on its thread.
  void Forget(uint64_t id);

  std::map<uint64_t, base::SequenceBound<MoltPerSiteLocalStorage>> locals_;
  std::map<uint64_t, base::SequenceBound<MoltPerSiteSessionStorage>>
      sessions_;
  uint64_t next_id_ = 0;
  base::WeakPtrFactory<MoltDomStorageOwnThreads> weak_factory_{this};
};

}  // namespace storage

#endif  // COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_OWN_THREAD_H_
