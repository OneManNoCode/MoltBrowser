// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "components/services/storage/dom_storage/molt_dom_storage_own_thread.h"

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

// ---- sessionStorage ----

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
  receiver_.Bind(std::move(receiver),
                 scheduler_->RunnerFor(kMoltDomStorageControlGroup));
  receiver_.set_disconnect_handler(std::move(on_disconnect));
}

MoltPerSiteSessionStorage::~MoltPerSiteSessionStorage() = default;

void MoltPerSiteSessionStorage::BindNamespace(
    const std::string& namespace_id,
    mojo::PendingReceiver<blink::mojom::SessionStorageNamespace> receiver) {
  // The namespace's pipe is then bound in its own turn
  // (SessionStorageImpl::MoltNamespaceTaskRunner()).
  impl_->BindNamespace(namespace_id, std::move(receiver));
}

void MoltPerSiteSessionStorage::BindStorageArea(
    const blink::StorageKey& storage_key,
    const std::string& namespace_id,
    mojo::PendingReceiver<blink::mojom::StorageArea> receiver) {
  // In the site's turn: after the namespace's creation or clone, which came
  // before it and ran in the control's turn.
  scheduler_->Post(
      MoltDomStorageSiteOf(storage_key),
      base::BindOnce(&MoltPerSiteSessionStorage::BindStorageAreaInTurn,
                     weak_factory_.GetWeakPtr(), storage_key, namespace_id,
                     std::move(receiver)));
}

void MoltPerSiteSessionStorage::BindStorageAreaInTurn(
    const blink::StorageKey& storage_key,
    const std::string& namespace_id,
    mojo::PendingReceiver<blink::mojom::StorageArea> receiver) {
  impl_->BindStorageArea(storage_key, namespace_id, std::move(receiver));
}

void MoltPerSiteSessionStorage::GetUsage(GetUsageCallback callback) {
  impl_->GetUsage(std::move(callback));
}

void MoltPerSiteSessionStorage::DeleteStorage(
    const blink::StorageKey& storage_key,
    const std::string& namespace_id,
    DeleteStorageCallback callback) {
  scheduler_->Post(
      MoltDomStorageSiteOf(storage_key),
      base::BindOnce(&MoltPerSiteSessionStorage::DeleteStorageInTurn,
                     weak_factory_.GetWeakPtr(), storage_key, namespace_id,
                     std::move(callback)));
}

void MoltPerSiteSessionStorage::DeleteStorageInTurn(
    const blink::StorageKey& storage_key,
    const std::string& namespace_id,
    DeleteStorageCallback callback) {
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

void MoltPerSiteSessionStorage::CreateNamespace(
    const std::string& namespace_id) {
  impl_->CreateNamespace(namespace_id);
}

void MoltPerSiteSessionStorage::CloneNamespace(
    const std::string& namespace_id_to_clone,
    const std::string& clone_namespace_id,
    mojom::SessionStorageCloneType clone_type) {
  impl_->CloneNamespace(namespace_id_to_clone, clone_namespace_id, clone_type);
}

void MoltPerSiteSessionStorage::DeleteNamespace(const std::string& namespace_id,
                                                bool should_persist) {
  impl_->DeleteNamespace(namespace_id, should_persist);
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
