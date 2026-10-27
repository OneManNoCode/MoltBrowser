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
// A control message that reads every site's areas waits for what those sites
// queued before it:
//  * GetUsage() (what Clear-Site-Data and the settings read before they
//    delete) lists each site's areas in that site's turn
//    (molt_dom_storage_split_work.h), so a clear that follows deletes what
//    the site wrote before it;
//  * a sessionStorage namespace copied at once (CloneNamespace() with
//    kImmediate: a tab duplicated) waits for a turn of each site with areas
//    in it, or with a bind or a deletion for it queued, so the copy holds
//    every write those sites queued before it; and a namespace that goes
//    while tabs it opened wait for their copy (DeleteNamespace()) does the
//    same. Meanwhile, every other control message that names either
//    namespace waits too, in the order it came (a bind of the copy, the
//    source's deletion), and runs after it; messages for other namespaces
//    don't wait. A renderer's own copy (window.open(), SessionStorageNamespace
//    Clone()) is ordered after its writes by the renderer itself (it pauses
//    the namespace's pipe until each area's pipe is flushed).
//
// What a page can still observe: the work of the task in service on
// MoltNet's thread (one message of another site, or one control message),
// and the machine (the processor and the storage service's IO thread, which
// reads every pipe's messages for all partitions), as any page can.

#ifndef COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_OWN_THREAD_H_
#define COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_OWN_THREAD_H_

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/threading/sequence_bound.h"
#include "base/unguessable_token.h"
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
  void MoltDropNonce(const base::UnguessableToken& nonce,
                     bool closed) override;

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
  void MoltDropNonce(const base::UnguessableToken& nonce,
                     bool closed) override;
  void CreateNamespace(const std::string& namespace_id) override;
  void CloneNamespace(const std::string& namespace_id_to_clone,
                      const std::string& clone_namespace_id,
                      mojom::SessionStorageCloneType clone_type) override;
  void DeleteNamespace(const std::string& namespace_id,
                       bool should_persist) override;

  SessionStorageImpl* storage_for_testing() { return impl_.get(); }

  // How many control messages wait for a copy (for testing).
  size_t held_for_testing() const { return held_.size(); }

 private:
  void BindStorageAreaInTurn(
      const blink::StorageKey& storage_key,
      const std::string& namespace_id,
      mojo::PendingReceiver<blink::mojom::StorageArea> receiver);
  void DeleteStorageInTurn(const blink::StorageKey& storage_key,
                           const std::string& namespace_id,
                           DeleteStorageCallback callback);

  // Whether a control message naming `a` (and `b`) waits for a copy; such a
  // message is kept (Hold()) to run after it, in the order they came.
  bool IsHeld(const std::string& a, const std::string& b) const;
  void Hold(const std::string& a,
            const std::string& b,
            base::OnceClosure message);
  // Runs the held messages that no longer wait, in the order they came.
  void ReplayHeld();
  // A site's message for `namespace_id` queued in its turn, and its end.
  void Queued(const std::string& namespace_id, const std::string& group);
  void Dequeued(const std::string& namespace_id, const std::string& group);
  // Runs `operation` in the control's turn once each site with areas in
  // `namespace_id`, or a message for it queued, has had a turn; the
  // namespaces in `hold` hold their control messages until then.
  void AfterTurnsOf(const std::string& namespace_id,
                    std::vector<std::string> hold,
                    base::OnceClosure operation);
  void TurnTaken(uint64_t barrier);
  void RunBarrier(uint64_t barrier);

  const scoped_refptr<MoltDomStorageFairScheduler> scheduler_;
  mojo::Remote<mojom::SessionStorageControl> impl_pipe_;
  std::unique_ptr<SessionStorageImpl> impl_;
  // The groups of each namespace's messages queued in their sites' turns
  // (BindStorageArea(), DeleteStorage()).
  std::map<std::string, std::multiset<std::string>> queued_;
  // Copies (and deletions) waiting for their sites' turns: the turns still
  // to come, the operation, and the namespaces held; the held namespaces
  // and their control messages, in the order they came.
  struct Barrier {
    Barrier();
    Barrier(Barrier&&);
    Barrier& operator=(Barrier&&);
    ~Barrier();
    size_t turns_left = 0;
    base::OnceClosure operation;
    std::vector<std::string> hold;
  };
  std::map<uint64_t, Barrier> barriers_;
  uint64_t next_barrier_ = 0;
  std::multiset<std::string> held_namespaces_;
  struct HeldMessage {
    HeldMessage(std::string a, std::string b, base::OnceClosure message);
    HeldMessage(HeldMessage&&);
    HeldMessage& operator=(HeldMessage&&);
    ~HeldMessage();
    // The namespaces it names (the same twice if one).
    std::string a;
    std::string b;
    base::OnceClosure message;
  };
  std::deque<HeldMessage> held_;
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
