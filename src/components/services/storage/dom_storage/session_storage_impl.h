// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_SESSION_STORAGE_IMPL_H_
#define COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_SESSION_STORAGE_IMPL_H_

#include <stdint.h>

#include <deque>  // MoltBrowser
#include <map>
#include <memory>
#include <optional>
#include <set>  // MoltBrowser
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"  // MoltBrowser
#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/threading/sequence_bound.h"
#include "base/trace_event/memory_allocator_dump.h"
#include "base/trace_event/memory_dump_provider.h"
#include "components/services/storage/dom_storage/async_dom_storage_database.h"
#include "components/services/storage/dom_storage/db_status.h"
#include "components/services/storage/dom_storage/dom_storage_database.h"
#include "base/unguessable_token.h"  // MoltBrowser
#include "components/services/storage/dom_storage/molt_dom_storage_fair_scheduler.h"  // MoltBrowser
#include "components/services/storage/dom_storage/molt_dom_storage_site_budget.h"  // MoltBrowser
#include "components/services/storage/dom_storage/session_storage_data_map.h"
#include "components/services/storage/dom_storage/session_storage_metadata.h"
#include "components/services/storage/dom_storage/session_storage_namespace_impl.h"
#include "components/services/storage/public/mojom/session_storage_control.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "third_party/blink/public/mojom/dom_storage/session_storage_namespace.mojom.h"

namespace blink {
class StorageKey;
}  // namespace blink

namespace storage {

class StorageServiceImpl;
// The Session Storage implementation. An instance of this class exists for each
// profile directory (within the user data directory) that is using Session
// Storage. It manages storage for all StorageKeys and namespaces within that
// partition.
class SessionStorageImpl : public base::trace_event::MemoryDumpProvider,
                           public mojom::SessionStorageControl,
                           public SessionStorageDataMap::Listener,
                           public SessionStorageNamespaceImpl::Delegate {
 public:
  enum class BackingMode {
    // Use an in-memory database to store our state.
    kNoDisk,
    // Use disk for the database, but clear its contents before we open
    // it. Always set on Android where session restore is never used. On
    // Desktop, this is set when the browser determines that all session
    // storage data is stale (e.g., fresh non-recovery startup with no
    // session restore needed).
    kClearDiskStateOnOpen,
    // Use disk for the database, restore all saved namespaces from
    // disk. This assumes that ScavengeUnusedNamespace will eventually be called
    // to clean up unused namespaces on disk.
    kRestoreDiskState
  };

  using DestructSessionStorageCallback =
      base::OnceCallback<void(SessionStorageImpl*)>;
  SessionStorageImpl(
      const base::FilePath& storage_partition_directory,
      BackingMode backing_option,
      DestructSessionStorageCallback destruct_callback,
      mojo::PendingReceiver<mojom::SessionStorageControl> receiver);

  ~SessionStorageImpl() override;

  // mojom::SessionStorageControl implementation:
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
  void MoltKeepCachesPerSite() override;  // MoltBrowser
  void MoltSetGrowthObserver(
      mojo::PendingRemote<mojom::MoltDomStorageGrowthObserver> observer)
      override;  // MoltBrowser
  void MoltDropNonce(const base::UnguessableToken& nonce,
                     bool closed) override;  // MoltBrowser
  // MoltBrowser: gives `callback` the groups (molt_dom_storage_site.h) of
  // namespace `namespace_id`'s areas, for a clone that waits for their
  // turns (molt_dom_storage_own_thread.h): a chunk of areas a task in the
  // control's turn, the first at once (molt_dom_storage_split_work.h).
  void MoltCollectGroups(
      const std::string& namespace_id,
      base::OnceCallback<void(std::set<std::string>)> callback);
  // MoltBrowser: whether a copy or a deletion of namespace `namespace_id`
  // (or one it is copied from) is under way a chunk a task, and `callback`
  // to be posted in the control's turn as each ends: the control messages
  // naming such a namespace wait for it (molt_dom_storage_split_work.h).
  bool MoltIsNamespaceBusy(const std::string& namespace_id) const;
  void MoltSetOnNamespaceJobDone(base::RepeatingClosure callback);
  // MoltBrowser: whether namespace `namespace_id` has namespaces waiting for
  // a clone of it.
  bool MoltHasClonesWaiting(const std::string& namespace_id);
  // MoltBrowser: as MoltKeepCachesPerSite(), taking the sequence in turn
  // through `scheduler` (one for the whole partition, which its host shares:
  // molt_dom_storage_own_thread.h), or a scheduler of its own if null.
  void MoltKeepCachesPerSiteOn(
      scoped_refptr<MoltDomStorageFairScheduler> scheduler);
  void CreateNamespace(const std::string& namespace_id) override;
  void CloneNamespace(const std::string& namespace_id_to_clone,
                      const std::string& clone_namespace_id,
                      mojom::SessionStorageCloneType clone_type) override;
  void DeleteNamespace(const std::string& namespace_id,
                       bool should_persist) override;

  // Clears unused storage areas, when thresholds are reached.
  void PurgeUnusedAreasIfNeeded();

  // base::trace_event::MemoryDumpProvider implementation:
  bool OnMemoryDump(const base::trace_event::MemoryDumpArgs& args,
                    base::trace_event::ProcessMemoryDump* pmd) override;

  const base::FilePath& GetStoragePartitionDirectory() const;

  void FlushAreaForTesting(const std::string& namespace_id,
                           const blink::StorageKey& storage_key);

  // Access the underlying `AsyncDomStorageDatabase`. May be null if the
  // database is not yet open.
  AsyncDomStorageDatabase* GetDatabaseForTesting() { return database_.get(); }

  const SessionStorageMetadata& GetMetadataForTesting() const {
    return metadata_;
  }

  SessionStorageNamespaceImpl* GetNamespaceForTesting(const std::string& id) {
    auto it = namespaces_.find(id);
    if (it == namespaces_.end())
      return nullptr;
    return it->second.get();
  }

  // Wait for the database to be opened, or for opening to fail. If the database
  // is already opened, |callback| is invoked immediately.
  void SetDatabaseOpenCallbackForTesting(base::OnceClosure callback);

 private:
  class MoltUsageWalk;  // MoltBrowser
  friend class DOMStorageBrowserTest;
  FRIEND_TEST_ALL_PREFIXES(SessionStorageImplTest,
                           ClearDiskStateOnOpenWithEmptyPathUsesInMemory);

  // SessionStorageAreaImpl::Listener implementation:
  void OnDataMapCreation(int64_t map_id, SessionStorageDataMap* map) override;
  void OnDataMapDestruction(int64_t map_id) override;
  void OnCommitResult(DbStatus status) override;
  // MoltBrowser: the per-site budget (molt_dom_storage_site_budget.h);
  // everything goes without caches per site.
  bool MoltMayGrowTo(int64_t map_id,
                     const blink::StorageKey& storage_key,
                     size_t new_storage_used) override;
  void MoltShrankTo(int64_t map_id,
                    const blink::StorageKey& storage_key,
                    size_t new_storage_used) override;
  base::TimeDelta MoltSiteCommitDelay(
      const blink::StorageKey& storage_key) override;
  void MoltSiteCommitted(const blink::StorageKey& storage_key,
                         size_t bytes) override;
  bool MoltMayWrite(const blink::StorageKey& storage_key,
                    size_t bytes) override;
  // MoltBrowser: the task runner of `storage_key`'s site, with caches per
  // site (molt_dom_storage_fair_scheduler.h); else null.
  scoped_refptr<base::SequencedTaskRunner> MoltTaskRunner(
      const blink::StorageKey& storage_key) override;
  // MoltBrowser: kept per site without a database, an area left unbound
  // goes in its site's turn if it holds nothing.
  void MoltAreaUnbound(const std::string& namespace_id,
                       const blink::StorageKey& storage_key) override;
  void MoltDropAreaIfUnused(const std::string& namespace_id,
                            const blink::StorageKey& storage_key);

  // SessionStorageNamespaceImpl::Delegate implementation:
  scoped_refptr<base::SequencedTaskRunner> MoltNamespaceTaskRunner(
      const std::string& namespace_id) override;  // MoltBrowser
  scoped_refptr<SessionStorageDataMap> MaybeGetExistingDataMapForId(
      int64_t map_id) override;
  void RegisterShallowClonedNamespace(
      const std::string& source_namespace_id,
      const std::string& new_namespace_id,
      const SessionStorageNamespaceImpl::StorageKeyAreas&
          clone_from_storage_keys) override;

  std::unique_ptr<SessionStorageNamespaceImpl>
  CreateSessionStorageNamespaceImpl(std::string namespace_id);

  // Removes the namespaces in `namespace_ids` from `metadata_` and `database_`.
  // Deletes map key/value pairs from `database_` for maps that no longer have
  // any references.
  void DeleteNamespacesFromMetadataAndDatabase(
      std::vector<std::string> namespace_ids);

  // Runs |callback| immediately if already connected to a database, otherwise
  // delays running |callback| untill after a connection has been established.
  // Initiates connecting to the database if no connection is in progress yet.
  void RunWhenConnected(base::OnceClosure callback);

  // Part of asynchronous database opening called from `RunWhenConnected()`. If
  // opening the database on disk fails twice, falls back to in memory. If
  // opening the database in memory fails, runs without a database.
  // For `BackingMode::kClearDiskStateOnOpen`, the on-disk database is
  // unconditionally destroyed on open.
  void InitiateConnection(bool in_memory_only = false,
                          bool destroy_existing_db_for_recovery = false);
  void OnDatabaseOpened(AsyncDomStorageDatabase::OpenOutcome outcome);
  void OnGotDatabaseMetadata(
      StatusOr<DomStorageDatabase::Metadata> all_metadata);
  void OnConnectionFinished();
  void PurgeAllNamespaceDataMaps();
  void DeleteAndRecreateDatabase(DomStorageRecoveryReason reason);

  void GetStatistics(size_t* total_cache_size, size_t* unused_areas_count);

  // MoltBrowser: with caches per site (molt_dom_storage_site_policy.h),
  // purges the unbound areas of `storage_key`'s top-level site, in every
  // namespace, if that site's maps pass the per-site limits. Without a
  // database, only areas whose maps hold no item.
  void MoltPurgeUnusedAreasOfSite(const blink::StorageKey& storage_key);
  bool molt_caches_per_site_ = false;
  // MoltBrowser: what each site's maps hold, its commit rate and its write
  // rate. A map is forgotten when its data leaves the database (or, without
  // one, the namespaces).
  MoltDomStorageSiteBudget molt_site_budget_;
  void MoltForgetMaps(const std::vector<DomStorageDatabase::MapLocator>& maps);
  // MoltBrowser: with caches per site, the storage sequence taken in turn
  // per site by the areas' pipes.
  scoped_refptr<MoltDomStorageFairScheduler> molt_scheduler_;
  // MoltBrowser: with caches per site, each group's maps
  // (molt_dom_storage_site.h) and each map's group, kept as maps are made
  // and go (declared before the maps' owners, which tell as they go); and
  // the groups whose writes are refused (MoltDropNonce()).
  std::map<std::string, std::set<int64_t>, std::less<>> molt_group_maps_;
  std::map<int64_t, std::string> molt_map_groups_;
  std::set<std::string, std::less<>> molt_closed_groups_;
  void MoltIndexMap(int64_t map_id, SessionStorageDataMap* map);
  // MoltBrowser: in `group`'s turn, its areas in every namespace cleared and
  // dropped, and its records forgotten; `closed`: its writes refused from
  // then on.
  void MoltDropGroup(const std::string& group, bool closed);
  // MoltBrowser: GetUsage() walked in turns (molt_dom_storage_split_work.h).
  void MoltWalkGroups(scoped_refptr<MoltUsageWalk> walk,
                      std::optional<std::string> after);
  void MoltListGroup(scoped_refptr<MoltUsageWalk> walk,
                     std::string group,
                     std::optional<int64_t> after);
  void MoltCollectGroupsStep(
      std::string namespace_id,
      std::optional<blink::StorageKey> after,
      std::set<std::string> groups,
      base::OnceCallback<void(std::set<std::string>)> callback);

  // MoltBrowser: a namespace copied or deleted a chunk a task, in the
  // control's turn, one job at a time in the order they came
  // (molt_dom_storage_split_work.h); only for a partition kept per site
  // without a database on the host that holds its control messages
  // (molt_dom_storage_own_thread.h).
  struct MoltNamespaceJob {
    MoltNamespaceJob();
    MoltNamespaceJob(MoltNamespaceJob&&);
    MoltNamespaceJob& operator=(MoltNamespaceJob&&);
    ~MoltNamespaceJob();
    bool is_clone = false;
    // A copy: from `source` to `destination`, the last key copied, and the
    // areas cloned so far. A deletion: of `source`, persisted or not, and
    // the namespace while its areas are freed.
    std::string source;
    std::string destination;
    std::optional<blink::StorageKey> after;
    SessionStorageNamespaceImpl::StorageKeyAreas areas;
    bool should_persist = false;
    std::unique_ptr<SessionStorageNamespaceImpl> going;
    bool areas_freed = false;
  };
  bool MoltSplitsNamespaceWork() const;
  void MoltStartClone(const std::string& source,
                      const std::string& destination);
  void MoltAddJob(MoltNamespaceJob job);
  void MoltRunJobStep(bool posted);
  bool MoltCloneStep(MoltNamespaceJob& job);
  bool MoltDeleteStep(MoltNamespaceJob& job);
  bool MoltForgetNamespaceChunk(const std::string& namespace_id);
  // The copies under way lose `group`'s areas (MoltDropGroup()).
  void MoltScrubClones(const std::string& group);
  bool molt_splits_namespace_work_ = false;
  bool molt_job_step_posted_ = false;
  base::RepeatingClosure molt_on_namespace_job_done_;

  void OnReceiverDisconnected();

  void ShutDown();

  // Passed in by the StorageServiceImpl that owns this object. Used to signal
  // that this SessionStorageImpl can be destructed when the Receiver is
  // disconnected.
  DestructSessionStorageCallback destruct_callback_;
  // Since the session storage object hierarchy references iterators owned by
  // the metadata, make sure it is destroyed last on destruction.
  SessionStorageMetadata metadata_;

  BackingMode backing_mode_;

  enum ConnectionState {
    NO_CONNECTION,
    CONNECTION_IN_PROGRESS,
    CONNECTION_FINISHED,
  } connection_state_ = NO_CONNECTION;

  // The profile data directory, which is an ancestor of the database path.
  // Empty for in-memory databases. When not empty, the owner of
  // `SessionStorageImpl` uses this path as an ID for the `SessionStorageImpl`
  // instance.
  const base::FilePath storage_partition_directory_;

  base::trace_event::MemoryAllocatorDumpGuid memory_dump_id_;

  mojo::Receiver<mojom::SessionStorageControl> receiver_;

  // `database_` is null after failing to open repeatedly.
  std::unique_ptr<AsyncDomStorageDatabase> database_;
  // This can be true even if the profile is not in-memory, since we attempt
  // to create an in-memory DB if on-disk fails. This variable has no meaning
  // if `database_` is null.
  bool in_memory_ = false;
  bool tried_to_recreate_during_open_ = false;

  std::vector<base::OnceClosure> on_database_opened_callbacks_;

  // The removal of items from this map is managed by the refcounting in
  // SessionStorageDataMap.
  // Populated after the database is connected.
  std::map</*map_id=*/int64_t, raw_ptr<SessionStorageDataMap, CtnExperimental>>
      data_maps_;
  // Populated in CreateNamespace, CloneNamespace, and sometimes
  // RegisterShallowClonedNamespace. Items are removed in
  // DeleteNamespace.
  std::map<std::string, std::unique_ptr<SessionStorageNamespaceImpl>>
      namespaces_;
  // MoltBrowser: the jobs (MoltNamespaceJob), the first under way; after
  // the maps and their indexes, which their areas tell as they go.
  std::deque<MoltNamespaceJob> molt_jobs_;

  // Scavenging only happens once.
  bool has_scavenged_ = false;
  // When namespaces are destroyed but marked as persistent, a scavenge should
  // not delete them. Cleared after ScavengeUnusedNamespaces is called.
  std::set<std::string> protected_namespaces_from_scavenge_;

  // Counts consecutive commit errors. If this number reaches a threshold, the
  // whole database is thrown away.
  int commit_error_count_ = 0;
  bool tried_to_recover_from_commit_errors_ = false;

  // Tracks the state of the current recovery cycle, including what triggered
  // it and the outcome of each Destroy() attempt. Populated in
  // DeleteAndRecreateDatabase() and consumed in OnConnectionFinished().
  std::optional<DomStorageRecoveryState> recovery_state_;

  base::WeakPtrFactory<SessionStorageImpl> weak_ptr_factory_{this};
};

}  // namespace storage

#endif  // COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_SESSION_STORAGE_IMPL_H_
