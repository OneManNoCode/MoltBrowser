// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/services/storage/dom_storage/session_storage_data_map.h"

#include "base/system/sys_info.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "components/services/storage/dom_storage/dom_storage_constants.h"

namespace storage {

// MoltBrowser: no per-site budget by default.
bool SessionStorageDataMap::Listener::MoltMayGrowTo(int64_t,
                                                    const blink::StorageKey&,
                                                    size_t) {
  return true;
}
void SessionStorageDataMap::Listener::MoltShrankTo(int64_t, size_t) {}
base::TimeDelta SessionStorageDataMap::Listener::MoltSiteCommitDelay(
    const blink::StorageKey&) {
  return base::TimeDelta();
}
void SessionStorageDataMap::Listener::MoltSiteCommitted(
    const blink::StorageKey&,
    size_t) {}

// MoltBrowser: the map's budget is its listener's.
bool SessionStorageDataMap::MoltMayGrowTo(size_t new_storage_used) {
  return listener_->MoltMayGrowTo(map_locator_->map_id().value_or(-1),
                                  map_locator_->storage_key(),
                                  new_storage_used);
}
void SessionStorageDataMap::MoltShrankTo(size_t new_storage_used) {
  listener_->MoltShrankTo(map_locator_->map_id().value_or(-1),
                          new_storage_used);
}
base::TimeDelta SessionStorageDataMap::MoltSiteCommitDelay() {
  return listener_->MoltSiteCommitDelay(map_locator_->storage_key());
}
void SessionStorageDataMap::MoltSiteCommitted(size_t bytes) {
  listener_->MoltSiteCommitted(map_locator_->storage_key(), bytes);
}
namespace {

StorageAreaImpl::Options GetOptions() {
  // Delay for a moment after a value is set in anticipation
  // of other values being set, so changes are batched.
  constexpr base::TimeDelta kCommitDefaultDelaySecs = base::Seconds(5);

  // To avoid excessive IO we apply limits to the amount of data being
  // written and the frequency of writes.
  StorageAreaImpl::Options options;
  options.max_size = kPerStorageAreaQuota + kPerStorageAreaOverQuotaAllowance;
  options.default_commit_delay = kCommitDefaultDelaySecs;
  options.max_bytes_per_hour = kPerStorageAreaQuota;
  options.max_commits_per_hour = 60;
  options.cache_mode = StorageAreaImpl::CacheMode::KEYS_ONLY_WHEN_POSSIBLE;
  return options;
}

}  // namespace

// static
scoped_refptr<SessionStorageDataMap> SessionStorageDataMap::CreateFromDisk(
    Listener* listener,
    scoped_refptr<DomStorageDatabase::SharedMapLocator> map_locator,
    AsyncDomStorageDatabase* database) {
  return base::WrapRefCounted(new SessionStorageDataMap(
      listener, std::move(map_locator), database, false));
}

// static
scoped_refptr<SessionStorageDataMap> SessionStorageDataMap::CreateEmpty(
    Listener* listener,
    scoped_refptr<DomStorageDatabase::SharedMapLocator> map_locator,
    AsyncDomStorageDatabase* database) {
  return base::WrapRefCounted(new SessionStorageDataMap(
      listener, std::move(map_locator), database, true));
}

// static
scoped_refptr<SessionStorageDataMap> SessionStorageDataMap::CreateClone(
    Listener* listener,
    scoped_refptr<DomStorageDatabase::SharedMapLocator> map_locator,
    scoped_refptr<SessionStorageDataMap> clone_from) {
  return base::WrapRefCounted(new SessionStorageDataMap(
      listener, std::move(map_locator), std::move(clone_from)));
}

void SessionStorageDataMap::DidCommit(DbStatus status) {
  listener_->OnCommitResult(status);
}

SessionStorageDataMap::SessionStorageDataMap(
    Listener* listener,
    scoped_refptr<DomStorageDatabase::SharedMapLocator> map_locator,
    AsyncDomStorageDatabase* database,
    bool is_empty)
    : listener_(listener),
      map_locator_(std::move(map_locator)),
      storage_area_impl_(std::make_unique<StorageAreaImpl>(
          database,
          map_locator_,
          this,
          GetOptions())),
      storage_area_ptr_(storage_area_impl_.get()) {
  if (is_empty)
    storage_area_impl_->InitializeAsEmpty();
  DCHECK(listener_);
  listener_->OnDataMapCreation(map_locator_->map_id().value(), this);
}

SessionStorageDataMap::SessionStorageDataMap(
    Listener* listener,
    scoped_refptr<DomStorageDatabase::SharedMapLocator> map_locator,
    scoped_refptr<SessionStorageDataMap> forking_from)
    : listener_(listener),
      clone_from_data_map_(std::move(forking_from)),
      map_locator_(std::move(map_locator)),
      storage_area_impl_(clone_from_data_map_->storage_area()
                             ->ForkToNewMap(map_locator_, this, GetOptions())),
      storage_area_ptr_(storage_area_impl_.get()) {
  DCHECK(listener_);
  listener_->OnDataMapCreation(map_locator_->map_id().value(), this);
}

SessionStorageDataMap::~SessionStorageDataMap() {
  listener_->OnDataMapDestruction(map_locator_->map_id().value());
}

void SessionStorageDataMap::RemoveBindingReference() {
  DCHECK_GT(binding_count_, 0);
  --binding_count_;
  if (binding_count_ > 0)
    return;
  // Don't delete ourselves, but do schedule an immediate commit. Possible
  // deletion will happen under memory pressure or when another sessionstorage
  // area is opened.
  storage_area()->ScheduleImmediateCommit();
}

void SessionStorageDataMap::OnMapLoaded() {
  clone_from_data_map_.reset();
}

}  // namespace storage
