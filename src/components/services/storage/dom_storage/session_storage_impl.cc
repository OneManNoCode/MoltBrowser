// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/services/storage/dom_storage/session_storage_impl.h"

#include <inttypes.h>

#include <utility>

#include "base/barrier_closure.h"
#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/ptr_util.h"
#include "base/memory/ref_counted.h"  // MoltBrowser
#include "base/metrics/histogram_functions.h"
#include "base/not_fatal_until.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/system/sys_info.h"
#include "base/task/sequenced_task_runner.h"
#include "base/trace_event/memory_dump_manager.h"
#include "build/build_config.h"
#include "components/services/storage/dom_storage/async_dom_storage_database.h"
#include "components/services/storage/dom_storage/dom_storage_constants.h"
#include "components/services/storage/dom_storage/dom_storage_database.h"
#include "components/services/storage/dom_storage/dom_storage_histogram_helper.h"
#include "components/services/storage/dom_storage/features.h"
#include "components/services/storage/dom_storage/molt_dom_storage_site.h"  // MoltBrowser
#include "components/services/storage/dom_storage/molt_dom_storage_site_policy.h"  // MoltBrowser
#include "components/services/storage/dom_storage/molt_dom_storage_split_work.h"  // MoltBrowser
#include "components/services/storage/dom_storage/session_storage_area_impl.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"

namespace storage {

namespace {

// Limits on the cache size and number of areas in memory, over which the areas
// are purged.
#if BUILDFLAG(IS_ANDROID)
const unsigned kMaxSessionStorageAreaCount = 10;
const size_t kMaxSessionStorageCacheSize = 2 * 1024 * 1024;
#else
const unsigned kMaxSessionStorageAreaCount = 50;
const size_t kMaxSessionStorageCacheSize = 20 * 1024 * 1024;
#endif

}  // namespace

SessionStorageImpl::SessionStorageImpl(
    const base::FilePath& storage_partition_directory,
    BackingMode backing_mode,
    DestructSessionStorageCallback destruct_callback,
    mojo::PendingReceiver<mojom::SessionStorageControl> receiver)
    : destruct_callback_(std::move(destruct_callback)),
      backing_mode_(backing_mode),
      storage_partition_directory_(storage_partition_directory),
      memory_dump_id_(base::StringPrintf("SessionStorage/0x%" PRIXPTR,
                                         reinterpret_cast<uintptr_t>(this))),
      receiver_(this, std::move(receiver)) {
  base::trace_event::MemoryDumpManager::GetInstance()
      ->RegisterDumpProviderWithSequencedTaskRunner(
          this, "SessionStorage",
          base::SequencedTaskRunner::GetCurrentDefault(),
          base::trace_event::MemoryDumpProvider::Options());
  receiver_.set_disconnect_handler(
      base::BindOnce(&SessionStorageImpl::OnReceiverDisconnected,
                     weak_ptr_factory_.GetWeakPtr()));
}

SessionStorageImpl::~SessionStorageImpl() {
  ShutDown();
  base::trace_event::MemoryDumpManager::GetInstance()->UnregisterDumpProvider(
      this);
}

void SessionStorageImpl::BindNamespace(
    const std::string& namespace_id,
    mojo::PendingReceiver<blink::mojom::SessionStorageNamespace> receiver) {
  if (connection_state_ != CONNECTION_FINISHED) {
    RunWhenConnected(base::BindOnce(&SessionStorageImpl::BindNamespace,
                                    weak_ptr_factory_.GetWeakPtr(),
                                    namespace_id, std::move(receiver)));
    return;
  }
  auto found = namespaces_.find(namespace_id);
  if (found == namespaces_.end()) {
    return;
  }

  if (found->second->state() ==
      SessionStorageNamespaceImpl::State::kNotPopulated) {
    found->second->PopulateFromMetadata(
        database_.get(), metadata_.GetOrCreateNamespaceEntry(namespace_id));
  }

  // MoltBrowser: with caches per site, binding a namespace purges nothing
  // (it has no site); its areas are purged when their own site opens one.
  if (!molt_caches_per_site_) {
    PurgeUnusedAreasIfNeeded();
  }
  found->second->Bind(std::move(receiver));
}

void SessionStorageImpl::BindStorageArea(
    const blink::StorageKey& storage_key,
    const std::string& namespace_id,
    mojo::PendingReceiver<blink::mojom::StorageArea> receiver) {
  if (connection_state_ != CONNECTION_FINISHED) {
    RunWhenConnected(base::BindOnce(&SessionStorageImpl::BindStorageArea,
                                    weak_ptr_factory_.GetWeakPtr(), storage_key,
                                    namespace_id, std::move(receiver)));
    return;
  }

  auto found = namespaces_.find(namespace_id);
  if (found == namespaces_.end()) {
    return;
  }

  SessionStorageMetadata::NamespaceEntry namespace_entry =
      metadata_.GetOrCreateNamespaceEntry(namespace_id);

  if (found->second->state() ==
      SessionStorageNamespaceImpl::State::kNotPopulated) {
    found->second->PopulateFromMetadata(database_.get(), namespace_entry);
  }

  // MoltBrowser: with caches per site, only this site's unbound areas, by
  // its own limits (molt_dom_storage_site_policy.h); without a database
  // there is none to purge (an unbound area that holds nothing is gone
  // already: MoltAreaUnbound()).
  if (molt_caches_per_site_) {
    if (database_) {
      MoltPurgeUnusedAreasOfSite(storage_key);
    }
  } else {
    PurgeUnusedAreasIfNeeded();
  }
  found->second->OpenArea(storage_key, std::move(receiver), namespace_entry);
}

void SessionStorageImpl::CreateNamespace(const std::string& namespace_id) {
  if (namespaces_.find(namespace_id) != namespaces_.end())
    return;

  namespaces_.emplace(std::make_pair(
      namespace_id, CreateSessionStorageNamespaceImpl(namespace_id)));
}

void SessionStorageImpl::CloneNamespace(
    const std::string& clone_from_namespace_id,
    const std::string& clone_to_namespace_id,
    mojom::SessionStorageCloneType clone_type) {
  if (namespaces_.find(clone_to_namespace_id) != namespaces_.end()) {
    // Non-immediate clones expect to be paired with a |Clone| from the mojo
    // namespace object. If that clone has already happened, then we don't need
    // to do anything here.
    // However, immediate clones happen without a |Clone| from the mojo
    // namespace object, so there should never be a namespace already populated
    // for an immediate clone.
    DCHECK_NE(clone_type, mojom::SessionStorageCloneType::kImmediate);
    return;
  }

  auto clone_from_ns = namespaces_.find(clone_from_namespace_id);
  std::unique_ptr<SessionStorageNamespaceImpl> clone_to_namespace_impl =
      CreateSessionStorageNamespaceImpl(clone_to_namespace_id);
  switch (clone_type) {
    case mojom::SessionStorageCloneType::kImmediate: {
      // If the namespace doesn't exist or it's not populated yet, just create
      // an empty session storage by not marking it as pending a clone.
      if (clone_from_ns == namespaces_.end() ||
          !clone_from_ns->second->IsPopulated()) {
        break;
      }
      clone_from_ns->second->Clone(clone_to_namespace_id);
      return;
    }
    case mojom::SessionStorageCloneType::kWaitForCloneOnNamespace:
      if (clone_from_ns != namespaces_.end()) {
        // The namespace exists and is in-use, so wait until receiving a clone
        // call on that mojo binding.
        clone_to_namespace_impl->SetPendingPopulationFromParentNamespace(
            clone_from_namespace_id);
        clone_from_ns->second->AddChildNamespaceWaitingForClone(
            clone_to_namespace_id);
      } else if (metadata_.namespace_storage_key_map().contains(
                     clone_from_namespace_id)) {
        CHECK_EQ(connection_state_, CONNECTION_FINISHED);

        // The namespace exists on disk but is not in-use, so do the appropriate
        // metadata operations to clone the namespace and set up the new object.
        auto source_namespace_entry =
            metadata_.GetOrCreateNamespaceEntry(clone_from_namespace_id);
        auto namespace_entry =
            metadata_.GetOrCreateNamespaceEntry(clone_to_namespace_id);
        metadata_.RegisterShallowClonedNamespace(source_namespace_entry,
                                                 namespace_entry);
        if (database_) {
          database_->PutMetadata(
              SessionStorageMetadata::ToDomStorageMetadata(namespace_entry),
              base::BindOnce(&SessionStorageImpl::OnCommitResult,
                             weak_ptr_factory_.GetWeakPtr()));
        }
      }
      // If there is no sign of a source namespace, just run with an empty
      // namespace.
      break;
    default:
      NOTREACHED();
  }
  namespaces_.emplace(
      std::piecewise_construct, std::forward_as_tuple(clone_to_namespace_id),
      std::forward_as_tuple(std::move(clone_to_namespace_impl)));
}

void SessionStorageImpl::DeleteNamespace(const std::string& namespace_id,
                                         bool should_persist) {
  auto namespace_it = namespaces_.find(namespace_id);
  // If the namespace has pending clones, do the clone now before destroying it.
  if (namespace_it != namespaces_.end()) {
    SessionStorageNamespaceImpl* namespace_ptr = namespace_it->second.get();
    if (namespace_ptr->HasChildNamespacesWaitingForClone()) {
      // Wait until we are connected, as it simplifies our choices.
      if (connection_state_ != CONNECTION_FINISHED) {
        RunWhenConnected(base::BindOnce(&SessionStorageImpl::DeleteNamespace,
                                        weak_ptr_factory_.GetWeakPtr(),
                                        namespace_id, should_persist));
        return;
      }
      namespace_ptr->CloneAllNamespacesWaitingForClone(database_.get(),
                                                       &metadata_, namespaces_);
    }

    // This namespace is still waiting for the `Clone()` call from the renderer
    // that owns the source namespace. That call travels on a different pipe
    // than this deletion, so it can still arrive. Tell the source namespace to
    // drop it, as otherwise `RegisterShallowClonedNamespace()` would recreate
    // this namespace with nobody left to delete it. This only applies to
    // namespaces that are not persisted: window creation being refused after
    // the renderer requested the clone always deletes without persisting,
    // whereas a persisted namespace belongs to a window that was created, and
    // its clone still has to be populated for session restore.
    if (!should_persist &&
        namespace_ptr->state() ==
            SessionStorageNamespaceImpl::State::kNotPopulatedAndPendingClone) {
      auto parent_it = namespaces_.find(
          namespace_ptr->pending_population_from_parent_namespace());
      if (parent_it != namespaces_.end()) {
        parent_it->second->AbandonChildNamespaceWaitingForClone(namespace_id);
      }
    }

    // MoltBrowser: a namespace that is copied from, or doesn't fit one
    // task, goes a chunk a task after the copies, its keys forgotten then
    // (molt_dom_storage_split_work.h).
    if (MoltSplitsNamespaceWork() && connection_state_ == CONNECTION_FINISHED &&
        (MoltIsNamespaceBusy(namespace_id) ||
         !namespace_ptr->MoltFitsOneFreeStep())) {
      if (!has_scavenged_ && should_persist) {
        protected_namespaces_from_scavenge_.insert(namespace_id);
      }
      MoltNamespaceJob job;
      job.source = namespace_id;
      job.should_persist = should_persist;
      MoltAddJob(std::move(job));
      return;
    }

    // The object hierarchy uses iterators bound to the metadata object, so
    // make sure to delete the object hierarchy first.
    namespaces_.erase(namespace_it);
  }

  if (!has_scavenged_ && should_persist)
    protected_namespaces_from_scavenge_.insert(namespace_id);

  if (!should_persist) {
    RunWhenConnected(base::BindOnce(
        &SessionStorageImpl::DeleteNamespacesFromMetadataAndDatabase,
        weak_ptr_factory_.GetWeakPtr(),
        std::vector<std::string>({namespace_id})));
  }
}

void SessionStorageImpl::Flush() {
  if (connection_state_ != CONNECTION_FINISHED) {
    RunWhenConnected(base::BindOnce(&SessionStorageImpl::Flush,
                                    weak_ptr_factory_.GetWeakPtr()));
    return;
  }

  for (const auto& it : data_maps_)
    it.second->storage_area()->ScheduleImmediateCommit();
}

void SessionStorageImpl::GetUsage(GetUsageCallback callback) {
  if (connection_state_ != CONNECTION_FINISHED) {
    RunWhenConnected(base::BindOnce(&SessionStorageImpl::GetUsage,
                                    weak_ptr_factory_.GetWeakPtr(),
                                    std::move(callback)));
    return;
  }

  // MoltBrowser: kept per site without a database, the maps in use are what
  // is stored, listed group by group in their own turns, a step at a time
  // (molt_dom_storage_split_work.h).
  if (molt_caches_per_site_ && molt_scheduler_ && !database_) {
    MoltWalkGroups(base::MakeRefCounted<MoltUsageWalk>(std::move(callback)),
                   std::nullopt);
    return;
  }

  const SessionStorageMetadata::NamespaceStorageKeyMap& all_namespaces =
      metadata_.namespace_storage_key_map();

  std::vector<mojom::SessionStorageUsageInfoPtr> result;
  result.reserve(all_namespaces.size());
  for (const auto& pair : all_namespaces) {
    for (const auto& storage_key_map_pair : pair.second) {
      result.push_back(mojom::SessionStorageUsageInfo::New(
          storage_key_map_pair.first, pair.first));
    }
  }
  std::move(callback).Run(std::move(result));
}

void SessionStorageImpl::DeleteStorage(const blink::StorageKey& storage_key,
                                       const std::string& namespace_id,
                                       DeleteStorageCallback callback) {
  if (connection_state_ != CONNECTION_FINISHED) {
    RunWhenConnected(base::BindOnce(&SessionStorageImpl::DeleteStorage,
                                    weak_ptr_factory_.GetWeakPtr(), storage_key,
                                    namespace_id, std::move(callback)));
    return;
  }

  auto found = namespaces_.find(namespace_id);
  if (found != namespaces_.end() &&
      found->second->state() !=
          SessionStorageNamespaceImpl::State::kNotPopulated) {
    found->second->RemoveStorageKeyData(storage_key, std::move(callback));
    return;
  }

  // If we don't have the namespace loaded, then we can delete it all using the
  // metadata.
  scoped_refptr<DomStorageDatabase::SharedMapLocator> map_locator =
      metadata_.TakeExistingMap(namespace_id, storage_key);
  if (!map_locator || !database_) {
    // Nothing to delete.
    std::move(callback).Run();
    return;
  }

  // Delete `storage_key` from `namespace_id` in the database.  Also delete
  // `map_locator` when not referenced by a cloned session.
  std::vector<DomStorageDatabase::MapLocator> maps_to_delete;
  if (map_locator->session_ids().empty()) {
    maps_to_delete.emplace_back(std::move(*map_locator));
  }
  MoltForgetMaps(maps_to_delete);  // MoltBrowser
  database_->DeleteStorageKeysFromSession(
      namespace_id, /*metadata_to_delete=*/{storage_key},
      std::move(maps_to_delete),
      base::BindOnce(&SessionStorageImpl::OnCommitResult,
                     weak_ptr_factory_.GetWeakPtr())
          .Then(std::move(callback)));
}

void SessionStorageImpl::CleanUpStorage(CleanUpStorageCallback callback) {
  if (connection_state_ != CONNECTION_FINISHED) {
    RunWhenConnected(base::BindOnce(&SessionStorageImpl::CleanUpStorage,
                                    weak_ptr_factory_.GetWeakPtr(),
                                    std::move(callback)));
    return;
  }
  if (database_) {
    for (const auto& it : data_maps_) {
      it.second->storage_area()->ScheduleImmediateCommit();
    }
    database_->CleanUpStaleData(
        base::IgnoreArgs<DbStatus>(std::move(callback)));
  } else {
    std::move(callback).Run();
  }
}

void SessionStorageImpl::ShutDown() {
  receiver_.reset();

  // The namespaces will DCHECK if they are destructed with pending clones. It
  // is valid to drop these on shutdown.
  for (auto& namespace_pair : namespaces_) {
    namespace_pair.second->ClearChildNamespacesWaitingForClone();
  }

  // Nothing to do if no connection to the database was ever finished.
  if (connection_state_ == CONNECTION_FINISHED) {
    // Flush any uncommitted data.
    for (const auto& it : data_maps_) {
      auto* area = it.second->storage_area();
      area->ScheduleImmediateCommit();
    }
  }
}

void SessionStorageImpl::PurgeMemory() {
  // MoltBrowser: with caches per site, nothing drops every site's caches
  // at once (molt_dom_storage_site_policy.h).
  if (molt_caches_per_site_) {
    return;
  }
  // Purge all areas that don't have bindings.
  for (const auto& namespace_pair : namespaces_) {
    namespace_pair.second->PurgeUnboundAreas();
  }
  // Purge memory from bound maps.
  for (const auto& data_map_pair : data_maps_) {
    data_map_pair.second->storage_area()->PurgeMemory();
  }
}

void SessionStorageImpl::PurgeUnusedAreasIfNeeded() {
  size_t total_cache_size, unused_area_count;
  GetStatistics(&total_cache_size, &unused_area_count);

  // Nothing to purge.
  if (!unused_area_count) {
    return;
  }

  bool cache_size_limit_exceeded =
      total_cache_size > kMaxSessionStorageCacheSize;

  bool area_count_limit_exceeded =
      data_maps_.size() > kMaxSessionStorageAreaCount;

  if (cache_size_limit_exceeded || area_count_limit_exceeded ||
      base::SysInfo::IsLowEndDeviceOrPartialLowEndModeEnabled()) {
    // Purge all areas that don't have bindings.
    for (const auto& namespace_pair : namespaces_) {
      namespace_pair.second->PurgeUnboundAreas();
    }
  }
}

void SessionStorageImpl::ScavengeUnusedNamespaces() {
  if (has_scavenged_) {
    return;
  }
  if (connection_state_ != CONNECTION_FINISHED) {
    RunWhenConnected(
        base::BindOnce(&SessionStorageImpl::ScavengeUnusedNamespaces,
                       weak_ptr_factory_.GetWeakPtr()));
    return;
  }
  has_scavenged_ = true;
  std::vector<std::string> namespaces_to_delete;
  for (const auto& metadata_namespace : metadata_.namespace_storage_key_map()) {
    const std::string& namespace_id = metadata_namespace.first;
    if (namespaces_.contains(namespace_id) ||
        protected_namespaces_from_scavenge_.contains(namespace_id)) {
      continue;
    }
    namespaces_to_delete.push_back(namespace_id);
  }
  DeleteNamespacesFromMetadataAndDatabase(std::move(namespaces_to_delete));
  protected_namespaces_from_scavenge_.clear();
}

bool SessionStorageImpl::OnMemoryDump(
    const base::trace_event::MemoryDumpArgs& args,
    base::trace_event::ProcessMemoryDump* pmd) {
  if (connection_state_ != CONNECTION_FINISHED)
    return true;

  std::string context_name =
      base::StringPrintf("site_storage/sessionstorage/0x%" PRIXPTR,
                         reinterpret_cast<uintptr_t>(this));

  // MoltBrowser: kept per site without a database, the partition's totals,
  // kept as they change: no walk over every site's maps in one task, and no
  // area named (molt_dom_storage_split_work.h).
  if (molt_caches_per_site_ && !database_) {
    auto* mad = pmd->CreateAllocatorDump(context_name + "/cache_size");
    mad->AddScalar(base::trace_event::MemoryAllocatorDump::kNameSize,
                   base::trace_event::MemoryAllocatorDump::kUnitsBytes,
                   molt_site_budget_.totals().total());
    mad->AddScalar("total_areas",
                   base::trace_event::MemoryAllocatorDump::kUnitsObjects,
                   data_maps_.size());
    return true;
  }

  if (database_) {
    // Account for database memory usage, which actually lives in the file
    // service.
    auto* global_dump = pmd->CreateSharedGlobalAllocatorDump(memory_dump_id_);
    // The size of the database dump will be added by the database service.
    auto* db_mad = pmd->CreateAllocatorDump(
        context_name + (database_->is_sqlite() ? "/sqlite" : "/leveldb"));
    // Specifies that the current context is responsible for keeping memory
    // alive.
    int kImportance = 2;
    pmd->AddOwnershipEdge(db_mad->guid(), global_dump->guid(), kImportance);
  }

  if (args.level_of_detail ==
      base::trace_event::MemoryDumpLevelOfDetail::kBackground) {
    size_t total_cache_size, unused_area_count;
    GetStatistics(&total_cache_size, &unused_area_count);
    auto* mad = pmd->CreateAllocatorDump(context_name + "/cache_size");
    mad->AddScalar(base::trace_event::MemoryAllocatorDump::kNameSize,
                   base::trace_event::MemoryAllocatorDump::kUnitsBytes,
                   total_cache_size);
    mad->AddScalar("total_areas",
                   base::trace_event::MemoryAllocatorDump::kUnitsObjects,
                   data_maps_.size());
    return true;
  }
  for (const auto& it : data_maps_) {
    const auto& storage_key = it.second->map_locator().storage_key();
    std::string storage_key_str =
        storage_key.GetMemoryDumpString(/*max_length=*/50);
    std::string area_dump_name = base::StringPrintf(
        "%s/%s/0x%" PRIXPTR, context_name.c_str(), storage_key_str.c_str(),
        reinterpret_cast<uintptr_t>(it.second->storage_area()));
    it.second->storage_area()->OnMemoryDump(area_dump_name, pmd);
  }
  return true;
}

const base::FilePath& SessionStorageImpl::GetStoragePartitionDirectory() const {
  return storage_partition_directory_;
}

void SessionStorageImpl::FlushAreaForTesting(
    const std::string& namespace_id,
    const blink::StorageKey& storage_key) {
  if (connection_state_ != CONNECTION_FINISHED)
    return;
  const auto& it = namespaces_.find(namespace_id);
  if (it == namespaces_.end())
    return;
  it->second->FlushStorageKeyForTesting(storage_key);
}

void SessionStorageImpl::SetDatabaseOpenCallbackForTesting(
    base::OnceClosure callback) {
  RunWhenConnected(std::move(callback));
}

void SessionStorageImpl::OnDataMapCreation(int64_t map_id,
                                           SessionStorageDataMap* map) {
  auto result = data_maps_.emplace(std::piecewise_construct,
                                   std::forward_as_tuple(map_id),
                                   std::forward_as_tuple(map));

  // `map_id` must identify a unique new map that did not exist in `data_maps_`.
  CHECK(result.second);
  // MoltBrowser: in its group's index, with caches per site.
  if (molt_caches_per_site_) {
    MoltIndexMap(map_id, map);
  }
}

void SessionStorageImpl::OnDataMapDestruction(int64_t map_id) {
  data_maps_.erase(map_id);
  // MoltBrowser: out of its group's index.
  if (auto group = molt_map_groups_.find(map_id);
      group != molt_map_groups_.end()) {
    if (auto maps = molt_group_maps_.find(group->second);
        maps != molt_group_maps_.end()) {
      maps->second.erase(map_id);
      if (maps->second.empty()) {
        molt_group_maps_.erase(maps);
      }
    }
    molt_map_groups_.erase(group);
  }
}

void SessionStorageImpl::OnCommitResult(DbStatus status) {
  // Previous commit errors deleted and recreated the database below. Ignore
  // additional errors from the old database while waiting for the new database
  // to open. The `!database_` check additionally handles the case where
  // recovery failed completely and we are now running without a database.
  if (connection_state_ != CONNECTION_FINISHED || !database_) {
    return;
  }
  if (status.ok()) {
    const DatabaseMetricsType metrics_type = database_->metrics_type();
    if (commit_error_count_ > 0 && tried_to_recover_from_commit_errors_) {
      base::UmaHistogramEnumeration(
          base::StrCat(
              {"Storage.SessionStorage.Recovery.CommitErrorThresholdExceeded",
               MaybeGetOnDiskExperimentalSuffix(metrics_type)}),
          DomStorageDatabaseRecoveryOutcome::
              kTransientErrorsAfterAttemptedRecovery);
    }
    RecordCommitErrorCountAtReset("SessionStorage", commit_error_count_,
                                  metrics_type);
    commit_error_count_ = 0;
    return;
  }

  commit_error_count_++;
  if (commit_error_count_ > kCommitErrorThreshold) {
    if (tried_to_recover_from_commit_errors_) {
      // We already tried to recover from a high commit error rate before, but
      // are still having problems: there isn't really anything left to try, so
      // just ignore errors.
      base::UmaHistogramEnumeration(
          base::StrCat(
              {"Storage.SessionStorage.Recovery.CommitErrorThresholdExceeded",
               MaybeGetOnDiskExperimentalSuffix(database_->metrics_type())}),
          DomStorageDatabaseRecoveryOutcome::
              kOngoingErrorsAfterAttemptedRecovery);
      return;
    }
    tried_to_recover_from_commit_errors_ = true;

    // Deleting StorageAreas in here could cause more commits (and commit
    // errors), but those commits won't reach OnCommitResult because the area
    // will have been deleted before the commit finishes.
    DeleteAndRecreateDatabase(
        DomStorageRecoveryReason::kCommitErrorThresholdExceeded);
  }
}

scoped_refptr<SessionStorageDataMap>
SessionStorageImpl::MaybeGetExistingDataMapForId(int64_t map_id) {
  auto it = data_maps_.find(map_id);
  if (it == data_maps_.end())
    return nullptr;
  return base::WrapRefCounted(it->second);
}

void SessionStorageImpl::RegisterShallowClonedNamespace(
    const std::string& source_namespace_id,
    const std::string& new_namespace_id,
    const SessionStorageNamespaceImpl::StorageKeyAreas& clone_from_areas) {
  bool found = false;
  auto it = namespaces_.find(new_namespace_id);
  if (it != namespaces_.end()) {
    found = true;
    if (it->second->IsPopulated()) {
      // Assumes this method is called on a stack handling a mojo message.
      receiver_.ReportBadMessage("Cannot clone to already populated namespace");
      return;
    }
  }

  CHECK_EQ(connection_state_, CONNECTION_FINISHED);

  // MoltBrowser: a copy of more keys than one task takes, or that a copy or
  // a deletion under way involves, is made a chunk a task
  // (molt_dom_storage_split_work.h).
  if (MoltSplitsNamespaceWork()) {
    auto source_keys =
        metadata_.namespace_storage_key_map().find(source_namespace_id);
    if (MoltIsNamespaceBusy(source_namespace_id) ||
        MoltIsNamespaceBusy(new_namespace_id) ||
        (source_keys != metadata_.namespace_storage_key_map().end() &&
         source_keys->second.size() > kMoltDomStorageAreasPerTask)) {
      MoltStartClone(source_namespace_id, new_namespace_id);
      return;
    }
  }

  auto source_namespace_entry =
      metadata_.GetOrCreateNamespaceEntry(source_namespace_id);
  auto namespace_entry = metadata_.GetOrCreateNamespaceEntry(new_namespace_id);
  metadata_.RegisterShallowClonedNamespace(source_namespace_entry,
                                           namespace_entry);

  if (database_) {
    database_->PutMetadata(
        SessionStorageMetadata::ToDomStorageMetadata(namespace_entry),
        base::BindOnce(&SessionStorageImpl::OnCommitResult,
                       weak_ptr_factory_.GetWeakPtr()));
  }

  if (found) {
    it->second->PopulateAsClone(database_.get(), namespace_entry,
                                clone_from_areas);
    return;
  }

  auto namespace_impl = CreateSessionStorageNamespaceImpl(new_namespace_id);
  namespace_impl->PopulateAsClone(database_.get(), namespace_entry,
                                  clone_from_areas);
  namespaces_.emplace(std::piecewise_construct,
                      std::forward_as_tuple(new_namespace_id),
                      std::forward_as_tuple(std::move(namespace_impl)));
}

std::unique_ptr<SessionStorageNamespaceImpl>
SessionStorageImpl::CreateSessionStorageNamespaceImpl(
    std::string namespace_id) {
  SessionStorageAreaImpl::RegisterNewAreaMap map_id_callback =
      base::BindRepeating(&SessionStorageMetadata::RegisterNewMap,
                          base::Unretained(&metadata_));

  return std::make_unique<SessionStorageNamespaceImpl>(
      std::move(namespace_id), this, std::move(map_id_callback), this);
}

void SessionStorageImpl::DeleteNamespacesFromMetadataAndDatabase(
    std::vector<std::string> namespace_ids) {
  CHECK_EQ(connection_state_, CONNECTION_FINISHED);

  // Remove each namespace from `metadata_`.
  std::vector<DomStorageDatabase::MapLocator> maps_to_delete;
  for (const std::string& namespace_id : namespace_ids) {
    std::map<blink::StorageKey,
             scoped_refptr<DomStorageDatabase::SharedMapLocator>>
        namespace_to_delete = metadata_.TakeNamespace(namespace_id);

    // Find unreferenced map key/value pairs to delete from `database_`.
    for (auto& [storage_key, map_locator] : namespace_to_delete) {
      if (map_locator->session_ids().empty()) {
        maps_to_delete.emplace_back(std::move(*map_locator));
      }
    }
  }

  // MoltBrowser: what those maps held is gone.
  MoltForgetMaps(maps_to_delete);

  // Delete the namespaces and map key/values from `database_`.
  if (!database_) {
    return;
  }
  database_->DeleteSessions(std::move(namespace_ids), std::move(maps_to_delete),
                            base::BindOnce(&SessionStorageImpl::OnCommitResult,
                                           weak_ptr_factory_.GetWeakPtr()));
}

void SessionStorageImpl::RunWhenConnected(base::OnceClosure callback) {
  switch (connection_state_) {
    case NO_CONNECTION:
      // If we don't have a filesystem_connection_, we'll need to establish one.
      connection_state_ = CONNECTION_IN_PROGRESS;
      receiver_.Pause();
      on_database_opened_callbacks_.push_back(std::move(callback));
      InitiateConnection();
      return;
    case CONNECTION_IN_PROGRESS:
      // Queue this OpenSessionStorage call for when we have a level db pointer.
      on_database_opened_callbacks_.push_back(std::move(callback));
      return;
    case CONNECTION_FINISHED:
      std::move(callback).Run();
      return;
  }
  NOTREACHED();
}

void SessionStorageImpl::InitiateConnection(
    bool in_memory_only,
    bool destroy_existing_db_for_recovery) {
  CHECK_EQ(connection_state_, CONNECTION_IN_PROGRESS);

  // Use an in-memory database unless we were given a usable subdirectory and
  // weren't asked to stay in memory.
  in_memory_ = in_memory_only || backing_mode_ == BackingMode::kNoDisk ||
               storage_partition_directory_.empty();
  // MoltBrowser: a partition that keeps DOM storage per site in memory keeps
  // no database; its maps hold their items themselves, and opening one never
  // waits on the database's sequence (molt_dom_storage_site_policy.h).
  if (MoltDomStorageKeepsNoDatabase(molt_caches_per_site_, in_memory_)) {
    OnConnectionFinished();
    return;
  }
  const base::FilePath dir_to_open =
      in_memory_ ? base::FilePath() : storage_partition_directory_;

  // Recovery destroys the pre-existing on-disk database before reopening (which
  // may itself be in-memory). `kClearDiskStateOnOpen` also destroys any
  // pre-existing on-disk state, but only when actually opening on disk (an
  // empty dir has nothing to destroy). An empty `dir_to_destroy` means no
  // destroy.
  const bool destroy_existing =
      destroy_existing_db_for_recovery ||
      (!in_memory_ && backing_mode_ == BackingMode::kClearDiskStateOnOpen);
  const base::FilePath dir_to_destroy =
      destroy_existing ? storage_partition_directory_ : base::FilePath();
  database_ = AsyncDomStorageDatabase::Open(
      StorageType::kSessionStorage, dir_to_open, memory_dump_id_,
      dir_to_destroy,
      base::BindOnce(&SessionStorageImpl::OnDatabaseOpened,
                     weak_ptr_factory_.GetWeakPtr()));
}

void SessionStorageImpl::OnDatabaseOpened(
    AsyncDomStorageDatabase::OpenOutcome outcome) {
  // If this open destroyed a pre-existing database, log the destroy result.
  // This covers both recovery and the `kClearDiskStateOnOpen` path. If
  // `recovery_state_` is set, then add the destroy's result to it.
  if (outcome.destroy_outcome) {
    outcome.destroy_outcome->status.Log(
        "Storage.SessionStorage.DestroyDatabase",
        outcome.destroy_outcome->destroyed_db_metrics_type);
    if (recovery_state_) {
      recovery_state_->AddDestroyResult(outcome.destroy_outcome->status.ok());
    }
  }

  if (!outcome.open_status.ok()) {
    // If we failed to open the database, try to delete and recreate the
    // database, or ultimately fallback to an in-memory database.
    DeleteAndRecreateDatabase(DomStorageRecoveryReason::kOpenFailure);
    return;
  }

  if (!database_) {
    // Some tests only simulate database connection without a database being
    // present.
    OnConnectionFinished();
    return;
  }

  database_->ReadAllMetadata(
      base::BindOnce(&SessionStorageImpl::OnGotDatabaseMetadata,
                     weak_ptr_factory_.GetWeakPtr()));
}

void SessionStorageImpl::OnGotDatabaseMetadata(
    StatusOr<DomStorageDatabase::Metadata> all_metadata) {
  if (!all_metadata.has_value()) {
    DeleteAndRecreateDatabase(DomStorageRecoveryReason::kMetadataReadFailure);
    return;
  }

  metadata_.Initialize(*std::move(all_metadata));

  OnConnectionFinished();
}

void SessionStorageImpl::OnConnectionFinished() {
  CHECK_EQ(connection_state_, CONNECTION_IN_PROGRESS);

  // If connection was opened successfully, reset tried_to_recreate_during_open_
  // to enable recreating the database on future errors.
  if (database_)
    tried_to_recreate_during_open_ = false;

  // Emit recovery histogram if we just completed a recovery cycle.
  if (recovery_state_) {
    LogDomStorageRecoveryOutcome("SessionStorage", *recovery_state_,
                                 /*has_database=*/database_ != nullptr,
                                 in_memory_);
    recovery_state_.reset();
  }

  // |database_| should be known to either be valid or invalid by now. Run our
  // delayed bindings.
  connection_state_ = CONNECTION_FINISHED;
  receiver_.Resume();
  std::vector<base::OnceClosure> callbacks;
  std::swap(callbacks, on_database_opened_callbacks_);
  for (size_t i = 0; i < callbacks.size(); ++i)
    std::move(callbacks[i]).Run();
}

void SessionStorageImpl::PurgeAllNamespaceDataMaps() {
  // Destroy all `SessionStorageDataMap` instances by re-initializing each
  // namespace.
  for (const auto& [namespace_id, namespace_impl] : namespaces_) {
    namespaces_.insert_or_assign(
        namespace_id, CreateSessionStorageNamespaceImpl(namespace_id));
  }
  CHECK(data_maps_.empty());
}

void SessionStorageImpl::DeleteAndRecreateDatabase(
    DomStorageRecoveryReason reason) {
  CHECK(database_);
  const DatabaseMetricsType metrics_type = database_->metrics_type();

  // Record the reason that initiated this recovery cycle. Only the first
  // reason is kept when recovery re-enters (e.g. open-fail after destroy).
  if (!recovery_state_) {
    recovery_state_.emplace(reason, metrics_type);
  }

  // We're about to set database_ to null, so delete the StorageAreas
  // that might still be using the old database.
  PurgeAllNamespaceDataMaps();

  // Reset state to be in process of connecting. This will cause requests for
  // StorageAreas to be queued until the connection is complete.
  connection_state_ = CONNECTION_IN_PROGRESS;
  receiver_.Pause();
  RecordCommitErrorCountAtReset("SessionStorage", commit_error_count_,
                                metrics_type);
  commit_error_count_ = 0;
  database_.reset();

  bool recreate_in_memory = false;

  // If tried to recreate database on disk already, try again but this time
  // in memory.
  if (tried_to_recreate_during_open_) {
    if (in_memory_) {
      // Give up completely, run without any database.
      OnConnectionFinished();
      return;
    }
    recreate_in_memory = true;
  }

  tried_to_recreate_during_open_ = true;

  protected_namespaces_from_scavenge_.clear();

  // `!in_memory_` means the old database was on-disk and must be destroyed
  // before reopening.
  InitiateConnection(recreate_in_memory,
                     /*destroy_existing_db_for_recovery=*/!in_memory_);
}

// MoltBrowser:
void SessionStorageImpl::MoltKeepCachesPerSite() {
  MoltKeepCachesPerSiteOn(nullptr);
}

// MoltBrowser:
void SessionStorageImpl::MoltKeepCachesPerSiteOn(
    scoped_refptr<MoltDomStorageFairScheduler> scheduler) {
  molt_caches_per_site_ = true;
  // On the host that holds the control messages, the namespaces' work is
  // split (molt_dom_storage_split_work.h).
  molt_splits_namespace_work_ = scheduler != nullptr;
  if (!molt_scheduler_) {
    molt_scheduler_ =
        scheduler ? std::move(scheduler)
                  : base::MakeRefCounted<MoltDomStorageFairScheduler>(
                        base::SequencedTaskRunner::GetCurrentDefault());
  }
  for (const auto& [map_id, map] : data_maps_) {
    MoltIndexMap(map_id, map);
  }
}

// MoltBrowser: a namespace's pipes (a tab's renderers' Clone() calls) take
// a turn of their own, as a site's do (molt_dom_storage_fair_scheduler.h).
scoped_refptr<base::SequencedTaskRunner>
SessionStorageImpl::MoltNamespaceTaskRunner(const std::string& namespace_id) {
  return molt_scheduler_ ? molt_scheduler_->RunnerFor(
                               MoltDomStorageNamespaceGroup(namespace_id))
                         : nullptr;
}

// MoltBrowser:
void SessionStorageImpl::MoltSetGrowthObserver(
    mojo::PendingRemote<mojom::MoltDomStorageGrowthObserver> observer) {
  molt_site_budget_.SetGrowthObserver(std::move(observer));
}

// MoltBrowser: a map counts to its storage key's top-level site's group
// (molt_dom_storage_site.h), in every namespace (tab) that uses it.
bool SessionStorageImpl::MoltMayGrowTo(int64_t map_id,
                                       const blink::StorageKey& storage_key,
                                       size_t new_storage_used) {
  if (!molt_caches_per_site_) {
    return true;
  }
  const std::string site = MoltDomStorageSiteOf(storage_key);
  return !molt_closed_groups_.contains(site) &&
         molt_site_budget_.MayGrowTo(base::NumberToString(map_id), site,
                                     new_storage_used);
}

void SessionStorageImpl::MoltShrankTo(int64_t map_id,
                                      const blink::StorageKey& storage_key,
                                      size_t new_storage_used) {
  if (molt_caches_per_site_) {
    molt_site_budget_.ShrankTo(base::NumberToString(map_id),
                               MoltDomStorageSiteOf(storage_key),
                               new_storage_used);
  }
}

base::TimeDelta SessionStorageImpl::MoltSiteCommitDelay(
    const blink::StorageKey& storage_key) {
  return molt_caches_per_site_
             ? molt_site_budget_.CommitDelay(
                   MoltDomStorageSiteOf(storage_key))
             : base::TimeDelta();
}

void SessionStorageImpl::MoltSiteCommitted(
    const blink::StorageKey& storage_key,
    size_t bytes) {
  if (molt_caches_per_site_) {
    molt_site_budget_.Committed(MoltDomStorageSiteOf(storage_key),
                                bytes);
  }
}

bool SessionStorageImpl::MoltMayWrite(const blink::StorageKey& storage_key,
                                      size_t bytes) {
  if (!molt_caches_per_site_) {
    return true;
  }
  const std::string site = MoltDomStorageSiteOf(storage_key);
  return !molt_closed_groups_.contains(site) &&
         molt_site_budget_.MayWrite(site, bytes);
}

scoped_refptr<base::SequencedTaskRunner> SessionStorageImpl::MoltTaskRunner(
    const blink::StorageKey& storage_key) {
  return molt_scheduler_
             ? molt_scheduler_->RunnerFor(MoltDomStorageSiteOf(storage_key))
             : nullptr;
}

// MoltBrowser: see molt_dom_storage_site_policy.h,
// MoltDomStorageDropsUnusedArea().
void SessionStorageImpl::MoltAreaUnbound(const std::string& namespace_id,
                                         const blink::StorageKey& storage_key) {
  if (!MoltDomStorageDropsUnusedArea(molt_caches_per_site_,
                                     database_ != nullptr)) {
    return;
  }
  MoltTaskRunner(storage_key)
      ->PostTask(FROM_HERE,
                 base::BindOnce(&SessionStorageImpl::MoltDropAreaIfUnused,
                                weak_ptr_factory_.GetWeakPtr(), namespace_id,
                                storage_key));
}

void SessionStorageImpl::MoltDropAreaIfUnused(
    const std::string& namespace_id,
    const blink::StorageKey& storage_key) {
  auto found = namespaces_.find(namespace_id);
  if (found != namespaces_.end()) {
    found->second->MoltDropAreaIfUnused(storage_key);
  }
}

void SessionStorageImpl::MoltForgetMaps(
    const std::vector<DomStorageDatabase::MapLocator>& maps) {
  for (const DomStorageDatabase::MapLocator& map : maps) {
    if (map.map_id()) {
      molt_site_budget_.Forget(base::NumberToString(*map.map_id()));
    }
  }
}

// MoltBrowser:
void SessionStorageImpl::MoltPurgeUnusedAreasOfSite(
    const blink::StorageKey& storage_key) {
  const std::string site = MoltDomStorageSiteOf(storage_key);
  size_t site_maps = 0;
  size_t site_unused_maps = 0;
  size_t site_cache_bytes = 0;
  for (const auto& [map_id, data_map] : data_maps_) {
    if (MoltDomStorageSiteOf(data_map->map_locator().storage_key()) !=
        site) {
      continue;
    }
    ++site_maps;
    site_cache_bytes += data_map->storage_area()->memory_used();
    if (data_map->binding_count() == 0 &&
        MoltDomStorageMayDropArea(
            database_ != nullptr,
            data_map->storage_area()->molt_holds_items())) {
      ++site_unused_maps;
    }
  }
  if (!MoltDomStoragePurgesSite(site_maps, site_unused_maps,
                                site_cache_bytes)) {
    return;
  }
  for (const auto& namespace_pair : namespaces_) {
    namespace_pair.second->MoltPurgeUnboundAreasOfSite(site);
  }
}

// MoltBrowser: a GetUsage() walked in turns, its result gathered from each
// group's turn (molt_dom_storage_split_work.h).
class SessionStorageImpl::MoltUsageWalk
    : public base::RefCounted<SessionStorageImpl::MoltUsageWalk> {
 public:
  explicit MoltUsageWalk(GetUsageCallback callback)
      : callback_(std::move(callback)) {}

  std::vector<mojom::SessionStorageUsageInfoPtr> result;
  size_t pending = 0;
  bool walked = false;

  void MaybeFinish() {
    if (walked && pending == 0 && callback_) {
      std::move(callback_).Run(std::move(result));
    }
  }

 private:
  friend class base::RefCounted<MoltUsageWalk>;
  ~MoltUsageWalk() = default;

  GetUsageCallback callback_;
};

// MoltBrowser:
void SessionStorageImpl::MoltWalkGroups(scoped_refptr<MoltUsageWalk> walk,
                                        std::optional<std::string> after) {
  bool more = false;
  const std::vector<std::string> groups = MoltDomStorageNextChunk(
      molt_group_maps_, after ? &*after : nullptr,
      kMoltDomStorageGroupsPerTask, &more);
  for (const std::string& group : groups) {
    ++walk->pending;
    molt_scheduler_->Post(
        group, base::BindOnce(&SessionStorageImpl::MoltListGroup,
                              weak_ptr_factory_.GetWeakPtr(), walk, group,
                              std::optional<int64_t>()));
  }
  if (more) {
    molt_scheduler_->Post(
        kMoltDomStorageControlGroup,
        base::BindOnce(&SessionStorageImpl::MoltWalkGroups,
                       weak_ptr_factory_.GetWeakPtr(), walk, groups.back()));
    return;
  }
  walk->walked = true;
  walk->MaybeFinish();
}

// MoltBrowser: each of the group's maps in every namespace that uses it.
void SessionStorageImpl::MoltListGroup(scoped_refptr<MoltUsageWalk> walk,
                                       std::string group,
                                       std::optional<int64_t> after) {
  bool more = false;
  std::vector<int64_t> map_ids;
  if (auto it = molt_group_maps_.find(group); it != molt_group_maps_.end()) {
    map_ids = MoltDomStorageNextChunk(it->second, after ? &*after : nullptr,
                                      kMoltDomStorageAreasPerTask, &more);
  }
  for (int64_t map_id : map_ids) {
    auto map = data_maps_.find(map_id);
    if (map == data_maps_.end()) {
      continue;
    }
    const DomStorageDatabase::SharedMapLocator& locator =
        map->second->map_locator();
    for (const std::string& namespace_id : locator.session_ids()) {
      walk->result.push_back(mojom::SessionStorageUsageInfo::New(
          locator.storage_key(), namespace_id));
    }
  }
  if (more) {
    molt_scheduler_->Post(
        group, base::BindOnce(&SessionStorageImpl::MoltListGroup,
                              weak_ptr_factory_.GetWeakPtr(), walk, group,
                              map_ids.back()));
    return;
  }
  --walk->pending;
  walk->MaybeFinish();
}

// MoltBrowser:
void SessionStorageImpl::MoltIndexMap(int64_t map_id,
                                      SessionStorageDataMap* map) {
  const std::string group =
      MoltDomStorageSiteOf(map->map_locator().storage_key());
  molt_group_maps_[group].insert(map_id);
  molt_map_groups_[map_id] = group;
}

// MoltBrowser: see session_storage_control.mojom.
void SessionStorageImpl::MoltDropNonce(const base::UnguessableToken& nonce,
                                       bool closed) {
  if (!molt_caches_per_site_ || !molt_scheduler_) {
    return;
  }
  const std::string group =
      MoltDomStorageGroupName(MoltDomStorageGroupKind::kNonce, nonce.ToString());
  molt_scheduler_->Post(group,
                        base::BindOnce(&SessionStorageImpl::MoltDropGroup,
                                       weak_ptr_factory_.GetWeakPtr(), group,
                                       closed));
}

void SessionStorageImpl::MoltDropGroup(const std::string& group, bool closed) {
  if (closed) {
    molt_closed_groups_.insert(group);
  } else {
    molt_closed_groups_.erase(group);
  }
  MoltScrubClones(group);
  // The group's areas, by storage key and namespace, taken first: clearing a
  // map shared by a copy forks it.
  std::vector<std::pair<blink::StorageKey, std::string>> areas;
  if (auto it = molt_group_maps_.find(group); it != molt_group_maps_.end()) {
    for (int64_t map_id : it->second) {
      auto map = data_maps_.find(map_id);
      if (map == data_maps_.end()) {
        continue;
      }
      const DomStorageDatabase::SharedMapLocator& locator =
          map->second->map_locator();
      for (const std::string& namespace_id : locator.session_ids()) {
        areas.emplace_back(locator.storage_key(), namespace_id);
      }
    }
  }
  for (const auto& [storage_key, namespace_id] : areas) {
    auto found = namespaces_.find(namespace_id);
    if (found == namespaces_.end() || !found->second->IsPopulated()) {
      continue;
    }
    found->second->RemoveStorageKeyData(storage_key, base::DoNothing());
    found->second->MoltDropAreaIfUnused(storage_key);
  }
  molt_site_budget_.ForgetSite(group);
}

// MoltBrowser:
void SessionStorageImpl::MoltCollectGroups(
    const std::string& namespace_id,
    base::OnceCallback<void(std::set<std::string>)> callback) {
  MoltCollectGroupsStep(namespace_id, std::nullopt, {}, std::move(callback));
}

// MoltBrowser:
void SessionStorageImpl::MoltCollectGroupsStep(
    std::string namespace_id,
    std::optional<blink::StorageKey> after,
    std::set<std::string> groups,
    base::OnceCallback<void(std::set<std::string>)> callback) {
  bool more = false;
  if (auto found = namespaces_.find(namespace_id); found != namespaces_.end()) {
    const std::vector<blink::StorageKey> keys = MoltDomStorageNextChunk(
        found->second->molt_storage_key_areas(), after ? &*after : nullptr,
        kMoltDomStorageAreasPerTask, &more);
    for (const blink::StorageKey& key : keys) {
      groups.insert(MoltDomStorageSiteOf(key));
    }
    if (!keys.empty()) {
      after = keys.back();
    }
  }
  if (!more || !molt_scheduler_) {
    std::move(callback).Run(std::move(groups));
    return;
  }
  molt_scheduler_->Post(
      kMoltDomStorageControlGroup,
      base::BindOnce(&SessionStorageImpl::MoltCollectGroupsStep,
                     weak_ptr_factory_.GetWeakPtr(), std::move(namespace_id),
                     std::move(after), std::move(groups), std::move(callback)));
}

SessionStorageImpl::MoltNamespaceJob::MoltNamespaceJob() = default;
SessionStorageImpl::MoltNamespaceJob::MoltNamespaceJob(MoltNamespaceJob&&) =
    default;
SessionStorageImpl::MoltNamespaceJob&
SessionStorageImpl::MoltNamespaceJob::operator=(MoltNamespaceJob&&) = default;
SessionStorageImpl::MoltNamespaceJob::~MoltNamespaceJob() = default;

// MoltBrowser:
bool SessionStorageImpl::MoltSplitsNamespaceWork() const {
  return molt_splits_namespace_work_ && molt_scheduler_ && !database_;
}

// MoltBrowser:
bool SessionStorageImpl::MoltIsNamespaceBusy(
    const std::string& namespace_id) const {
  for (const MoltNamespaceJob& job : molt_jobs_) {
    if (job.source == namespace_id ||
        (job.is_clone && job.destination == namespace_id)) {
      return true;
    }
  }
  return false;
}

// MoltBrowser:
void SessionStorageImpl::MoltSetOnNamespaceJobDone(
    base::RepeatingClosure callback) {
  molt_on_namespace_job_done_ = std::move(callback);
}

// MoltBrowser: the copy is a namespace waiting for its population, as one
// the renderer is still to copy, so what is asked of it waits.
void SessionStorageImpl::MoltStartClone(const std::string& source,
                                        const std::string& destination) {
  auto it = namespaces_.find(destination);
  if (it == namespaces_.end()) {
    it = namespaces_
             .emplace(destination,
                      CreateSessionStorageNamespaceImpl(destination))
             .first;
  }
  if (it->second->state() ==
      SessionStorageNamespaceImpl::State::kNotPopulated) {
    it->second->SetPendingPopulationFromParentNamespace(source);
  }
  MoltNamespaceJob job;
  job.is_clone = true;
  job.source = source;
  job.destination = destination;
  MoltAddJob(std::move(job));
}

// MoltBrowser: the first step at once if no job is under way.
void SessionStorageImpl::MoltAddJob(MoltNamespaceJob job) {
  molt_jobs_.push_back(std::move(job));
  if (molt_jobs_.size() == 1) {
    MoltRunJobStep(/*posted=*/false);
  }
}

// MoltBrowser:
void SessionStorageImpl::MoltRunJobStep(bool posted) {
  if (posted) {
    molt_job_step_posted_ = false;
  }
  if (molt_jobs_.empty()) {
    return;
  }
  // A step may add jobs (a copy's population runs what waited for it); the
  // job stays first, so it stays where it is.
  MoltNamespaceJob& job = molt_jobs_.front();
  const bool more = job.is_clone ? MoltCloneStep(job) : MoltDeleteStep(job);
  if (!more) {
    molt_jobs_.pop_front();
    // What waited for it runs in a task of its own.
    if (molt_on_namespace_job_done_) {
      molt_scheduler_->Post(kMoltDomStorageControlGroup,
                            molt_on_namespace_job_done_);
    }
  }
  if (!molt_jobs_.empty() && !molt_job_step_posted_) {
    molt_job_step_posted_ = true;
    molt_scheduler_->Post(kMoltDomStorageControlGroup,
                          base::BindOnce(&SessionStorageImpl::MoltRunJobStep,
                                         weak_ptr_factory_.GetWeakPtr(),
                                         /*posted=*/true));
  }
}

// MoltBrowser: as SessionStorageMetadata::RegisterShallowClonedNamespace()
// and SessionStorageNamespaceImpl::PopulateAsClone(), a chunk of keys a
// step, each key's metadata and area together; the copy is populated with
// them at the end. A key the source gains past the chunk copied is copied,
// as a write to an area not copied yet is.
bool SessionStorageImpl::MoltCloneStep(MoltNamespaceJob& job) {
  auto destination = namespaces_.find(job.destination);
  if (destination == namespaces_.end() ||
      destination->second->IsPopulated()) {
    // Gone (or made) meanwhile: the areas cloned go with the job.
    return false;
  }
  auto source = namespaces_.find(job.source);
  const bool source_keys =
      metadata_.namespace_storage_key_map().contains(job.source);
  auto destination_entry = metadata_.GetOrCreateNamespaceEntry(job.destination);
  if (source != namespaces_.end() && source->second->IsPopulated() &&
      source_keys) {
    auto source_entry = metadata_.GetOrCreateNamespaceEntry(job.source);
    bool more = false;
    const std::vector<blink::StorageKey> keys = MoltDomStorageNextChunk(
        source_entry->second, job.after ? &*job.after : nullptr,
        kMoltDomStorageAreasPerTask, &more);
    const SessionStorageNamespaceImpl::StorageKeyAreas& source_areas =
        source->second->molt_storage_key_areas();
    for (const blink::StorageKey& key : keys) {
      auto locator = source_entry->second.find(key);
      if (destination_entry->second.emplace(key, locator->second).second) {
        locator->second->AddSession(job.destination);
      }
      if (auto area = source_areas.find(key); area != source_areas.end()) {
        job.areas.emplace(key, area->second->Clone(job.destination));
      }
    }
    if (!keys.empty()) {
      job.after = keys.back();
    }
    if (more) {
      return true;
    }
  }
  destination->second->MoltPopulateWithClonedAreas(
      database_.get(), destination_entry, std::move(job.areas));
  return false;
}

// MoltBrowser: as DeleteNamespace() and
// DeleteNamespacesFromMetadataAndDatabase(): out of the namespaces (its
// pipes closed), its areas freed a step's worth a step, then, unless it
// persists, its keys forgotten a chunk a step.
bool SessionStorageImpl::MoltDeleteStep(MoltNamespaceJob& job) {
  if (!job.areas_freed) {
    if (!job.going) {
      auto it = namespaces_.find(job.source);
      if (it != namespaces_.end()) {
        job.going = std::move(it->second);
        namespaces_.erase(it);
        job.going->MoltCloseReceivers();
      }
    }
    if (job.going && job.going->MoltFreeAreasChunk()) {
      return true;
    }
    job.going.reset();
    job.areas_freed = true;
    // Its keys from the next step on.
    return !job.should_persist;
  }
  return !job.should_persist && MoltForgetNamespaceChunk(job.source);
}

// MoltBrowser: as SessionStorageMetadata::TakeNamespace(), a chunk of keys;
// whether any are left.
bool SessionStorageImpl::MoltForgetNamespaceChunk(
    const std::string& namespace_id) {
  if (!metadata_.namespace_storage_key_map().contains(namespace_id)) {
    return false;
  }
  auto entry = metadata_.GetOrCreateNamespaceEntry(namespace_id);
  std::vector<DomStorageDatabase::MapLocator> maps_to_delete;
  size_t keys = 0;
  while (!entry->second.empty() && keys < kMoltDomStorageAreasPerTask) {
    auto first = entry->second.begin();
    first->second->RemoveSession(namespace_id);
    if (first->second->session_ids().empty()) {
      maps_to_delete.push_back(first->second->Clone());
    }
    entry->second.erase(first);
    ++keys;
  }
  MoltForgetMaps(maps_to_delete);
  if (!entry->second.empty()) {
    return true;
  }
  // Empty now: only its entry goes.
  metadata_.TakeNamespace(namespace_id);
  return false;
}

// MoltBrowser:
void SessionStorageImpl::MoltScrubClones(const std::string& group) {
  for (MoltNamespaceJob& job : molt_jobs_) {
    if (!job.is_clone) {
      continue;
    }
    for (auto it = job.areas.begin(); it != job.areas.end();) {
      if (MoltDomStorageSiteOf(it->first) != group) {
        ++it;
        continue;
      }
      if (metadata_.namespace_storage_key_map().contains(job.destination)) {
        auto entry = metadata_.GetOrCreateNamespaceEntry(job.destination);
        if (auto key = entry->second.find(it->first);
            key != entry->second.end()) {
          key->second->RemoveSession(job.destination);
          entry->second.erase(key);
        }
      }
      it = job.areas.erase(it);
    }
  }
}

bool SessionStorageImpl::MoltHasClonesWaiting(
    const std::string& namespace_id) {
  auto found = namespaces_.find(namespace_id);
  return found != namespaces_.end() &&
         found->second->HasChildNamespacesWaitingForClone();
}

void SessionStorageImpl::GetStatistics(size_t* total_cache_size,
                                       size_t* unused_area_count) {
  *total_cache_size = 0;
  *unused_area_count = 0;
  for (const auto& it : data_maps_) {
    *total_cache_size += it.second->storage_area()->memory_used();
    if (it.second->binding_count() == 0)
      (*unused_area_count)++;
  }
}

void SessionStorageImpl::OnReceiverDisconnected() {
  std::move(destruct_callback_).Run(this);
}

}  // namespace storage
