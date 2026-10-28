// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "components/services/storage/dom_storage/molt_dom_storage_own_thread.h"

#include <deque>
#include <set>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/single_thread_task_runner_thread_mode.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "components/services/storage/dom_storage/local_storage_impl.h"
#include "components/services/storage/dom_storage/molt_dom_storage_site.h"
#include "components/services/storage/dom_storage/molt_dom_storage_site_policy.h"
#include "components/services/storage/dom_storage/session_storage_impl.h"

namespace storage {

namespace {

// A thread of the partition's own: dedicated, so that nothing else runs on
// it and it never waits for a worker of the thread pool. USER_BLOCKING: a
// page's first read of an area is synchronous.
scoped_refptr<base::SingleThreadTaskRunner> MakeOwnThread() {
  return base::ThreadPool::CreateSingleThreadTaskRunner(
      {base::TaskPriority::USER_BLOCKING,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::SingleThreadTaskRunnerThreadMode::DEDICATED);
}

scoped_refptr<MoltDomStorageFairScheduler> MakeScheduler() {
  return base::MakeRefCounted<MoltDomStorageFairScheduler>(
      base::SequencedTaskRunner::GetCurrentDefault());
}

}  // namespace

// ---- localStorage ----

MoltPerSiteLocalStorage::MoltPerSiteLocalStorage(
    mojo::PendingReceiver<mojom::LocalStorageControl> receiver,
    base::OnceClosure on_disconnect)
    : scheduler_(MakeScheduler()),
      // In memory (no directory), with no database once per site
      // (molt_dom_storage_site_policy.h). It isn't destroyed by its own
      // pipe closing: this object owns it.
      impl_(std::make_unique<LocalStorageImpl>(
          base::FilePath(),
          base::DoNothing(),
          impl_pipe_.BindNewPipeAndPassReceiver())) {
  impl_->MoltKeepCachesPerSiteOn(scheduler_);
  receiver_.Bind(std::move(receiver),
                 scheduler_->RunnerFor(kMoltDomStorageControlGroup));
  receiver_.set_disconnect_handler(std::move(on_disconnect));
}

MoltPerSiteLocalStorage::~MoltPerSiteLocalStorage() = default;

void MoltPerSiteLocalStorage::BindStorageArea(
    const blink::StorageKey& storage_key,
    mojo::PendingReceiver<blink::mojom::StorageArea> receiver) {
  // In the site's turn (molt_dom_storage_own_thread.h).
  scheduler_->Post(
      MoltDomStorageSiteOf(storage_key),
      base::BindOnce(&MoltPerSiteLocalStorage::BindStorageAreaInTurn,
                     weak_factory_.GetWeakPtr(), storage_key,
                     std::move(receiver)));
}

void MoltPerSiteLocalStorage::BindStorageAreaInTurn(
    const blink::StorageKey& storage_key,
    mojo::PendingReceiver<blink::mojom::StorageArea> receiver) {
  impl_->BindStorageArea(storage_key, std::move(receiver));
}

void MoltPerSiteLocalStorage::GetUsage(GetUsageCallback callback) {
  impl_->GetUsage(std::move(callback));
}

void MoltPerSiteLocalStorage::DeleteStorage(
    const blink::StorageKey& storage_key,
    DeleteStorageCallback callback) {
  scheduler_->Post(MoltDomStorageSiteOf(storage_key),
                   base::BindOnce(&MoltPerSiteLocalStorage::DeleteStorageInTurn,
                                  weak_factory_.GetWeakPtr(), storage_key,
                                  std::move(callback)));
}

void MoltPerSiteLocalStorage::DeleteStorageInTurn(
    const blink::StorageKey& storage_key,
    DeleteStorageCallback callback) {
  impl_->DeleteStorage(storage_key, std::move(callback));
}

void MoltPerSiteLocalStorage::CleanUpStorage(CleanUpStorageCallback callback) {
  impl_->CleanUpStorage(std::move(callback));
}

void MoltPerSiteLocalStorage::Flush() {
  impl_->Flush();
}

void MoltPerSiteLocalStorage::PurgeMemory() {
  impl_->PurgeMemory();
}

void MoltPerSiteLocalStorage::ApplyPolicyUpdates(
    std::vector<mojom::StoragePolicyUpdatePtr> policy_updates) {
  impl_->ApplyPolicyUpdates(std::move(policy_updates));
}

void MoltPerSiteLocalStorage::ForceKeepSessionState() {
  impl_->ForceKeepSessionState();
}

void MoltPerSiteLocalStorage::MoltKeepCachesPerSite() {
  // Kept per site from the start.
}

void MoltPerSiteLocalStorage::MoltSetGrowthObserver(
    mojo::PendingRemote<mojom::MoltDomStorageGrowthObserver> observer) {
  impl_->MoltSetGrowthObserver(std::move(observer));
}

void MoltPerSiteLocalStorage::MoltDropNonce(
    const base::UnguessableToken& nonce,
    bool closed) {
  // In the nonce's group's turn (LocalStorageImpl::MoltDropNonce()).
  impl_->MoltDropNonce(nonce, closed);
}

void MoltPerSiteLocalStorage::MoltGetUsageForStorageKey(
    const blink::StorageKey& storage_key,
    MoltGetUsageForStorageKeyCallback callback) {
  // In the key's site's turn (LocalStorageImpl::MoltGetUsageForStorageKey()).
  impl_->MoltGetUsageForStorageKey(storage_key, std::move(callback));
}

// ---- sessionStorage ----

MoltPerSiteSessionStorage::Barrier::Barrier() = default;
MoltPerSiteSessionStorage::Barrier::Barrier(Barrier&&) = default;
MoltPerSiteSessionStorage::Barrier&
MoltPerSiteSessionStorage::Barrier::operator=(Barrier&&) = default;
MoltPerSiteSessionStorage::Barrier::~Barrier() = default;

MoltPerSiteSessionStorage::HeldMessage::HeldMessage(std::string a,
                                                    std::string b,
                                                    base::OnceClosure message)
    : a(std::move(a)), b(std::move(b)), message(std::move(message)) {}
MoltPerSiteSessionStorage::HeldMessage::HeldMessage(HeldMessage&&) = default;
MoltPerSiteSessionStorage::HeldMessage&
MoltPerSiteSessionStorage::HeldMessage::operator=(HeldMessage&&) = default;
MoltPerSiteSessionStorage::HeldMessage::~HeldMessage() = default;

MoltPerSiteSessionStorage::MoltPerSiteSessionStorage(
    mojo::PendingReceiver<mojom::SessionStorageControl> receiver,
    base::OnceClosure on_disconnect)
    : scheduler_(MakeScheduler()),
      impl_(std::make_unique<SessionStorageImpl>(
          base::FilePath(),
          SessionStorageImpl::BackingMode::kNoDisk,
          base::DoNothing(),
          impl_pipe_.BindNewPipeAndPassReceiver())) {
  impl_->MoltKeepCachesPerSiteOn(scheduler_);
  // The messages that waited for a namespace's copy or deletion, made a
  // chunk a task, run once it ends.
  impl_->MoltSetOnNamespaceJobDone(
      base::BindRepeating(&MoltPerSiteSessionStorage::ReplayHeld,
                          weak_factory_.GetWeakPtr()));
  receiver_.Bind(std::move(receiver),
                 scheduler_->RunnerFor(kMoltDomStorageControlGroup));
  receiver_.set_disconnect_handler(std::move(on_disconnect));
}

MoltPerSiteSessionStorage::~MoltPerSiteSessionStorage() = default;

void MoltPerSiteSessionStorage::BindNamespace(
    const std::string& namespace_id,
    mojo::PendingReceiver<blink::mojom::SessionStorageNamespace> receiver) {
  if (IsHeld(namespace_id, namespace_id)) {
    Hold(namespace_id, namespace_id,
         base::BindOnce(&MoltPerSiteSessionStorage::BindNamespace,
                        weak_factory_.GetWeakPtr(), namespace_id,
                        std::move(receiver)));
    return;
  }
  // The namespace's pipe is then bound in its own turn
  // (SessionStorageImpl::MoltNamespaceTaskRunner()).
  impl_->BindNamespace(namespace_id, std::move(receiver));
}

void MoltPerSiteSessionStorage::BindStorageArea(
    const blink::StorageKey& storage_key,
    const std::string& namespace_id,
    mojo::PendingReceiver<blink::mojom::StorageArea> receiver) {
  if (IsHeld(namespace_id, namespace_id)) {
    Hold(namespace_id, namespace_id,
         base::BindOnce(&MoltPerSiteSessionStorage::BindStorageArea,
                        weak_factory_.GetWeakPtr(), storage_key, namespace_id,
                        std::move(receiver)));
    return;
  }
  // In the site's turn: after the namespace's creation or clone, which came
  // before it and ran in the control's turn.
  const std::string group = MoltDomStorageSiteOf(storage_key);
  Queued(namespace_id, group);
  scheduler_->Post(
      group, base::BindOnce(&MoltPerSiteSessionStorage::BindStorageAreaInTurn,
                            weak_factory_.GetWeakPtr(), storage_key,
                            namespace_id, std::move(receiver)));
}

void MoltPerSiteSessionStorage::BindStorageAreaInTurn(
    const blink::StorageKey& storage_key,
    const std::string& namespace_id,
    mojo::PendingReceiver<blink::mojom::StorageArea> receiver) {
  Dequeued(namespace_id, MoltDomStorageSiteOf(storage_key));
  impl_->BindStorageArea(storage_key, namespace_id, std::move(receiver));
}

void MoltPerSiteSessionStorage::GetUsage(GetUsageCallback callback) {
  impl_->GetUsage(std::move(callback));
}

void MoltPerSiteSessionStorage::DeleteStorage(
    const blink::StorageKey& storage_key,
    const std::string& namespace_id,
    DeleteStorageCallback callback) {
  if (IsHeld(namespace_id, namespace_id)) {
    Hold(namespace_id, namespace_id,
         base::BindOnce(&MoltPerSiteSessionStorage::DeleteStorage,
                        weak_factory_.GetWeakPtr(), storage_key, namespace_id,
                        std::move(callback)));
    return;
  }
  const std::string group = MoltDomStorageSiteOf(storage_key);
  Queued(namespace_id, group);
  scheduler_->Post(
      group, base::BindOnce(&MoltPerSiteSessionStorage::DeleteStorageInTurn,
                            weak_factory_.GetWeakPtr(), storage_key,
                            namespace_id, std::move(callback)));
}

void MoltPerSiteSessionStorage::DeleteStorageInTurn(
    const blink::StorageKey& storage_key,
    const std::string& namespace_id,
    DeleteStorageCallback callback) {
  Dequeued(namespace_id, MoltDomStorageSiteOf(storage_key));
  impl_->DeleteStorage(storage_key, namespace_id, std::move(callback));
}

void MoltPerSiteSessionStorage::CleanUpStorage(
    CleanUpStorageCallback callback) {
  impl_->CleanUpStorage(std::move(callback));
}

void MoltPerSiteSessionStorage::ScavengeUnusedNamespaces() {
  impl_->ScavengeUnusedNamespaces();
}

void MoltPerSiteSessionStorage::Flush() {
  impl_->Flush();
}

void MoltPerSiteSessionStorage::PurgeMemory() {
  impl_->PurgeMemory();
}

void MoltPerSiteSessionStorage::MoltKeepCachesPerSite() {
  // Kept per site from the start.
}

void MoltPerSiteSessionStorage::MoltSetGrowthObserver(
    mojo::PendingRemote<mojom::MoltDomStorageGrowthObserver> observer) {
  impl_->MoltSetGrowthObserver(std::move(observer));
}

void MoltPerSiteSessionStorage::MoltDropNonce(
    const base::UnguessableToken& nonce,
    bool closed) {
  // In the nonce's group's turn (SessionStorageImpl::MoltDropNonce()).
  impl_->MoltDropNonce(nonce, closed);
}

void MoltPerSiteSessionStorage::MoltGetUsageForStorageKey(
    const blink::StorageKey& storage_key,
    MoltGetUsageForStorageKeyCallback callback) {
  // In the key's site's turn
  // (SessionStorageImpl::MoltGetUsageForStorageKey()).
  impl_->MoltGetUsageForStorageKey(storage_key, std::move(callback));
}

void MoltPerSiteSessionStorage::CreateNamespace(
    const std::string& namespace_id) {
  if (IsHeld(namespace_id, namespace_id)) {
    Hold(namespace_id, namespace_id,
         base::BindOnce(&MoltPerSiteSessionStorage::CreateNamespace,
                        weak_factory_.GetWeakPtr(), namespace_id));
    return;
  }
  impl_->CreateNamespace(namespace_id);
}

void MoltPerSiteSessionStorage::CloneNamespace(
    const std::string& namespace_id_to_clone,
    const std::string& clone_namespace_id,
    mojom::SessionStorageCloneType clone_type) {
  if (IsHeld(namespace_id_to_clone, clone_namespace_id)) {
    Hold(namespace_id_to_clone, clone_namespace_id,
         base::BindOnce(&MoltPerSiteSessionStorage::CloneNamespace,
                        weak_factory_.GetWeakPtr(), namespace_id_to_clone,
                        clone_namespace_id, clone_type));
    return;
  }
  // A copy made at once holds what the source's sites queued before it
  // (molt_dom_storage_own_thread.h). A copy the renderer makes later is
  // ordered by the renderer.
  if (clone_type == mojom::SessionStorageCloneType::kImmediate) {
    AfterTurnsOf(
        namespace_id_to_clone, {namespace_id_to_clone, clone_namespace_id},
        base::BindOnce(&SessionStorageImpl::CloneNamespace,
                       base::Unretained(impl_.get()), namespace_id_to_clone,
                       clone_namespace_id, clone_type));
    return;
  }
  impl_->CloneNamespace(namespace_id_to_clone, clone_namespace_id, clone_type);
}

void MoltPerSiteSessionStorage::DeleteNamespace(const std::string& namespace_id,
                                                bool should_persist) {
  if (IsHeld(namespace_id, namespace_id)) {
    Hold(namespace_id, namespace_id,
         base::BindOnce(&MoltPerSiteSessionStorage::DeleteNamespace,
                        weak_factory_.GetWeakPtr(), namespace_id,
                        should_persist));
    return;
  }
  // A namespace going while tabs it opened wait for their copy makes the
  // copies now: they hold what its sites queued before.
  if (impl_->MoltHasClonesWaiting(namespace_id)) {
    AfterTurnsOf(namespace_id, {namespace_id},
                 base::BindOnce(&SessionStorageImpl::DeleteNamespace,
                                base::Unretained(impl_.get()), namespace_id,
                                should_persist));
    return;
  }
  impl_->DeleteNamespace(namespace_id, should_persist);
}

bool MoltPerSiteSessionStorage::IsHeld(const std::string& a,
                                       const std::string& b) const {
  return held_namespaces_.contains(a) || held_namespaces_.contains(b) ||
         impl_->MoltIsNamespaceBusy(a) || impl_->MoltIsNamespaceBusy(b);
}

void MoltPerSiteSessionStorage::Hold(const std::string& a,
                                     const std::string& b,
                                     base::OnceClosure message) {
  held_.push_back({a, b, std::move(message)});
}

void MoltPerSiteSessionStorage::ReplayHeld() {
  // Once at a time: a message it runs may end a hold, which asks again.
  if (replaying_) {
    replay_again_ = true;
    return;
  }
  replaying_ = true;
  do {
    replay_again_ = false;
    // The messages held meanwhile, in the order they came. One that still
    // names a held namespace is held again, in the same order, and so is
    // any later one that names a namespace of a message held again.
    std::deque<HeldMessage> held;
    held.swap(held_);
    std::set<std::string> blocked;
    for (HeldMessage& message : held) {
      if (IsHeld(message.a, message.b) || blocked.contains(message.a) ||
          blocked.contains(message.b)) {
        blocked.insert(message.a);
        blocked.insert(message.b);
        held_.push_back(std::move(message));
        continue;
      }
      std::move(message.message).Run();
    }
  } while (replay_again_ && !held_.empty());
  replaying_ = false;
}

void MoltPerSiteSessionStorage::Queued(const std::string& namespace_id,
                                       const std::string& group) {
  queued_[namespace_id].insert(group);
}

void MoltPerSiteSessionStorage::Dequeued(const std::string& namespace_id,
                                         const std::string& group) {
  auto it = queued_.find(namespace_id);
  if (it == queued_.end()) {
    return;
  }
  if (auto one = it->second.find(group); one != it->second.end()) {
    it->second.erase(one);
  }
  if (it->second.empty()) {
    queued_.erase(it);
  }
}

void MoltPerSiteSessionStorage::AfterTurnsOf(const std::string& namespace_id,
                                             std::vector<std::string> hold,
                                             base::OnceClosure operation) {
  const uint64_t id = next_barrier_++;
  Barrier& barrier = barriers_[id];
  barrier.operation = std::move(operation);
  for (const std::string& held : hold) {
    held_namespaces_.insert(held);
  }
  barrier.hold = std::move(hold);
  // The namespace's sites, gathered a chunk of areas a task
  // (molt_dom_storage_split_work.h); its messages wait meanwhile.
  impl_->MoltCollectGroups(
      namespace_id,
      base::BindOnce(&MoltPerSiteSessionStorage::PostTurns,
                     weak_factory_.GetWeakPtr(), id, namespace_id));
}

void MoltPerSiteSessionStorage::PostTurns(uint64_t barrier,
                                          const std::string& namespace_id,
                                          std::set<std::string> groups) {
  auto it = barriers_.find(barrier);
  if (it == barriers_.end()) {
    return;
  }
  // And the sites with a bind or a deletion for it still queued.
  if (auto queued = queued_.find(namespace_id); queued != queued_.end()) {
    groups.insert(queued->second.begin(), queued->second.end());
  }
  if (groups.empty()) {
    RunBarrier(barrier);
    return;
  }
  it->second.turns_left = groups.size();
  for (const std::string& group : groups) {
    scheduler_->Post(group,
                     base::BindOnce(&MoltPerSiteSessionStorage::TurnTaken,
                                    weak_factory_.GetWeakPtr(), barrier));
  }
}

void MoltPerSiteSessionStorage::TurnTaken(uint64_t barrier) {
  auto it = barriers_.find(barrier);
  if (it == barriers_.end() || --it->second.turns_left > 0) {
    return;
  }
  // The operation itself in the control's turn.
  scheduler_->Post(kMoltDomStorageControlGroup,
                   base::BindOnce(&MoltPerSiteSessionStorage::RunBarrier,
                                  weak_factory_.GetWeakPtr(), barrier));
}

void MoltPerSiteSessionStorage::RunBarrier(uint64_t barrier) {
  auto it = barriers_.find(barrier);
  if (it == barriers_.end()) {
    return;
  }
  Barrier done = std::move(it->second);
  barriers_.erase(it);
  std::move(done.operation).Run();
  for (const std::string& held : done.hold) {
    if (auto one = held_namespaces_.find(held);
        one != held_namespaces_.end()) {
      held_namespaces_.erase(one);
    }
  }
  ReplayHeld();
}

// ---- The threads ----

MoltDomStorageOwnThreads::MoltDomStorageOwnThreads() = default;
MoltDomStorageOwnThreads::~MoltDomStorageOwnThreads() = default;

void MoltDomStorageOwnThreads::BindLocalStorageControl(
    mojo::PendingReceiver<mojom::LocalStorageControl> receiver) {
  const uint64_t id = next_id_++;
  locals_.emplace(
      id, base::SequenceBound<MoltPerSiteLocalStorage>(
              MakeOwnThread(), std::move(receiver),
              base::BindPostTaskToCurrentDefault(
                  base::BindOnce(&MoltDomStorageOwnThreads::Forget,
                                 weak_factory_.GetWeakPtr(), id))));
}

void MoltDomStorageOwnThreads::BindSessionStorageControl(
    mojo::PendingReceiver<mojom::SessionStorageControl> receiver) {
  const uint64_t id = next_id_++;
  sessions_.emplace(
      id, base::SequenceBound<MoltPerSiteSessionStorage>(
              MakeOwnThread(), std::move(receiver),
              base::BindPostTaskToCurrentDefault(
                  base::BindOnce(&MoltDomStorageOwnThreads::Forget,
                                 weak_factory_.GetWeakPtr(), id))));
}

void MoltDomStorageOwnThreads::Forget(uint64_t id) {
  // Destroyed on its own thread; the thread goes with its last task.
  locals_.erase(id);
  sessions_.erase(id);
}

}  // namespace storage
