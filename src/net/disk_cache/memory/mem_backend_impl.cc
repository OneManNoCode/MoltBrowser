// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/disk_cache/memory/mem_backend_impl.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

#include "base/byte_size.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/location.h"  // MoltBrowser
#include "base/rand_util.h"  // MoltBrowser
#include "base/task/sequenced_task_runner.h"  // MoltBrowser
#include "base/logging.h"
#include "base/memory_coordinator/memory_consumer.h"
#include "base/memory_coordinator/memory_coordinator_features.h"
#include "base/memory_coordinator/traits.h"
#include "base/memory_coordinator/utils.h"
#include "base/numerics/safe_conversions.h"
#include "base/system/sys_info.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/clock.h"
#include "base/types/expected.h"
#include "net/base/net_errors.h"
#include "net/disk_cache/cache_util.h"
#include "net/disk_cache/memory/mem_entry_impl.h"

using base::Time;

namespace disk_cache {

namespace {

const int32_t kDefaultInMemoryCacheSize = 10 * 1024 * 1024;
const int32_t kMaxMemoryCacheSize = kDefaultInMemoryCacheSize * 5;

int32_t CalculateDefaultMaxSize() {
  // The default max size is based on amount of physical memory of the machine.

  base::ByteSize total_memory = base::SysInfo::AmountOfTotalPhysicalMemory();
  if (total_memory.is_zero()) {
    return kDefaultInMemoryCacheSize;
  }

  // We want to use up to 2% of the computer's memory, with a limit of 50 MB,
  // reached on system with more than 2.5 GB of RAM.
  if (total_memory >= base::MiB(2500)) {
    return kMaxMemoryCacheSize;
  }

  base::ByteSize max_size = total_memory * 2 / 100;
  return base::checked_cast<int32_t>(max_size.InBytes());
}

// Returns the next entry after |node| in |lru_list| that's not a child
// of |node|.  This is useful when dooming, since dooming a parent entry
// will also doom its children.
base::LinkNode<MemEntryImpl>* NextSkippingChildren(
    const base::LinkedList<MemEntryImpl>& lru_list,
    base::LinkNode<MemEntryImpl>* node) {
  MemEntryImpl* cur = node->value();
  do {
    node = node->next();
  } while (node != lru_list.end() && node->value()->parent() == cur);
  return node;
}

constexpr base::MemoryConsumerTraits kMemBackendImplTraits(
    // Scales with system RAM up to a cap in the tens of MBs.
    base::MemoryConsumerTraits::EstimatedMemoryUsage::kMedium,
    // Eviction traverses linked list and erases map entries.
    base::MemoryConsumerTraits::ReleaseMemoryCost::kRequiresTraversal,
    // Evicted entries can be re-fetched from network.
    base::MemoryConsumerTraits::InformationRetention::kLossless,
    // Eviction runs inline synchronously.
    base::MemoryConsumerTraits::ExecutionType::kSynchronous);

}  // namespace

MemBackendImpl::MemBackendImpl(net::NetLog* net_log)
    : Backend(net::MEMORY_CACHE),
      net_log_(net_log),
      memory_consumer_registration_(
          "MemBackendImpl",
          kMemBackendImplTraits,
          this,
          base::AsyncMemoryConsumerRegistration::CheckUnregister::kDisabled) {}

MemBackendImpl::~MemBackendImpl() {
  while (!entries_.empty())
    entries_.begin()->second->Doom();

  if (!post_cleanup_callback_.is_null())
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, std::move(post_cleanup_callback_));
}

// static
std::unique_ptr<MemBackendImpl> MemBackendImpl::CreateBackend(
    int64_t max_bytes,
    net::NetLog* net_log) {
  if (max_bytes < 0 || max_bytes > std::numeric_limits<int32_t>::max()) {
    LOG(ERROR) << "Unable to create cache";
    return nullptr;
  }

  auto cache = std::make_unique<MemBackendImpl>(net_log);

  // `max_bytes` is guaranteed to fit because of the check above.
  cache->Init(base::checked_cast<int32_t>(max_bytes));

  return cache;
}

int64_t MemBackendImpl::MaxFileSize() const {
  // MoltBrowser: with per-site limits, the budget's (half a share).
  if (MoltHasBudget()) {
    return molt_budget_.max_entry_bytes;
  }
  return max_size_ / 8;
}

void MemBackendImpl::OnEntryInserted(MemEntryImpl* entry) {
  lru_list_.Append(entry);
  // MoltBrowser: and last in its site's list, with per-site limits.
  if (MoltHasBudget()) {
    molt_sites_.Touch(MoltSiteOf(entry), entry->molt_site_link(), MoltNowUs());
  }
}

void MemBackendImpl::OnEntryUpdated(MemEntryImpl* entry) {
  // LinkedList<>::RemoveFromList() removes |entry| from |lru_list_|.
  entry->RemoveFromList();
  lru_list_.Append(entry);
  // MoltBrowser: and in its site's list, with per-site limits.
  if (MoltHasBudget()) {
    molt_sites_.Touch(MoltSiteOf(entry), entry->molt_site_link(), MoltNowUs());
  }
}

void MemBackendImpl::OnEntryDoomed(MemEntryImpl* entry) {
  if (entry->type() == MemEntryImpl::EntryType::kParent)
    entries_.erase(entry->key());
  // LinkedList<>::RemoveFromList() removes |entry| from |lru_list_|.
  entry->RemoveFromList();
  // MoltBrowser: and from its site's list, with per-site limits.
  if (MoltHasBudget()) {
    molt_sites_.Remove(MoltSiteOf(entry), entry->molt_site_link());
  }
}

void MemBackendImpl::ModifyStorageSize(int32_t delta) {
  current_size_ += delta;
  if (delta > 0)
    EvictIfNeeded();
}

bool MemBackendImpl::HasExceededStorageSize() const {
  return current_size_ > current_max_size_;
}

// MoltBrowser: per-site limits (molt_mem_backend_budget_policy.h). Without
// them, Chromium's ModifyStorageSize() and HasExceededStorageSize().
void MemBackendImpl::ModifyStorageSize(const MemEntryImpl* entry,
                                       int32_t delta) {
  if (!MoltHasBudget()) {
    ModifyStorageSize(delta);
    return;
  }
  const std::string site(MoltSiteOf(entry));
  current_size_ += delta;
  molt_sites_.AddSize(site, delta);
  // Only the writing site's own entries make room for it; nothing is
  // evicted for the total.
  if (delta > 0 && MoltCacheSiteOverShare(MoltSiteSize(site), molt_budget_)) {
    MoltEvictSiteTill(site, MoltCacheSiteEvictionTarget(molt_budget_));
  }
}

bool MemBackendImpl::HasExceededStorageSize(const MemEntryImpl* entry) const {
  if (!MoltHasBudget()) {
    return HasExceededStorageSize();
  }
  return MoltCacheWriteRefused(MoltSiteSize(MoltSiteOf(entry)), current_size_,
                               molt_budget_);
}

// static
std::string_view MemBackendImpl::MoltSiteOf(const MemEntryImpl* entry) {
  const MemEntryImpl* parent =
      entry->type() == MemEntryImpl::EntryType::kChild ? entry->parent()
                                                       : entry;
  return MoltHttpCacheSiteOfKey(parent->key());
}

int64_t MemBackendImpl::MoltSiteSize(std::string_view site) const {
  return molt_sites_.SizeOf(site);
}

void MemBackendImpl::MoltEvictSiteTill(std::string_view site, int64_t target) {
  // `site` may point into an entry that goes: keep a copy. Only the site's
  // own list is walked (molt_mem_backend_site_lru.h).
  const std::string own(site);
  molt_sites_.EvictTill(
      own, target,
      [](const MemEntryImpl* candidate) { return !candidate->InUse(); },
      [](MemEntryImpl* candidate) { candidate->Doom(); });
}

bool MemBackendImpl::MoltMayCreateEntry(const std::string& key) {
  const std::string_view site = MoltHttpCacheSiteOfKey(key);
  // A site past the cap on sites isn't given a record: its entry isn't
  // made, as one that doesn't fit isn't (molt_mem_backend_site_lru.h).
  if (!molt_sites_.HasRoomFor(site)) {
    return false;
  }
  const int64_t key_bytes = static_cast<int64_t>(key.size());
  if (MoltCacheSiteMakesRoomForEntry(MoltSiteSize(site), key_bytes,
                                     molt_budget_)) {
    MoltEvictSiteTill(site,
                      MoltCacheSiteEntryEvictionTarget(key_bytes, molt_budget_));
  }
  return !MoltCacheEntryRefused(MoltSiteSize(site), current_size_, key_bytes,
                                molt_budget_);
}

// static
int64_t MemBackendImpl::MoltNowUs() {
  return (base::TimeTicks::Now() - base::TimeTicks()).InMicroseconds();
}

bool MemBackendImpl::MoltIsExpiredSince(int64_t last_used_us) const {
  return MoltCacheEntryExpired(
      (MoltNowUs() - last_used_us) / base::Time::kMicrosecondsPerSecond,
      molt_budget_);
}

bool MemBackendImpl::MoltIsExpired(MemEntryImpl* entry) {
  // The monotonic time of its last use, by which its site's list is ordered
  // (molt_mem_backend_site_lru.h), not the wall clock's.
  return MoltIsExpiredSince(entry->molt_site_link().last_used_us());
}

void MemBackendImpl::MoltStartSweep() {
  // A round still going on (its next step is posted) goes on.
  if (molt_sites_.sweeping()) {
    return;
  }
  molt_sites_.StartSweep();
  MoltSweepStep();
}

void MemBackendImpl::MoltSweepStep() {
  // One step: a few entries of one site, from its least recently used, up
  // to its first that hasn't expired (molt_mem_backend_site_lru.h); the
  // next step is a task of its own, so a request waits for one step at
  // most, however many entries the sites have expiring.
  const bool more = molt_sites_.SweepStep(
      kMoltCacheSweepStepEntries,
      [this](int64_t last_used_us) { return MoltIsExpiredSince(last_used_us); },
      [](const MemEntryImpl* candidate) { return !candidate->InUse(); },
      [](MemEntryImpl* candidate) { candidate->Doom(); });
  if (more) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&MemBackendImpl::MoltSweepStep,
                                  weak_factory_.GetWeakPtr()));
  }
}

void MemBackendImpl::MoltSetPerSiteBudget(const MoltCacheSiteBudget& budget) {
  if (!MoltUsesCacheSiteBudget(budget)) {
    return;
  }
  molt_budget_ = budget;
  // Every entry so far counts against its site and is in its site's list,
  // in the order of |lru_list_| (normally there is none yet: the budget is
  // set when the backend is made). The table of sites is made with room
  // for the cap, so adding a site never rehashes it.
  molt_sites_.Clear();
  molt_sites_.Reserve(kMoltCacheMaxSites, base::RandUint64());
  const int64_t now_us = MoltNowUs();
  for (base::LinkNode<MemEntryImpl>* node = lru_list_.head();
       node != lru_list_.end(); node = node->next()) {
    MemEntryImpl* entry = node->value();
    molt_sites_.Touch(MoltSiteOf(entry), entry->molt_site_link(), now_us);
    molt_sites_.AddSize(MoltSiteOf(entry), entry->GetStorageSize());
  }
  max_size_ = base::saturated_cast<int32_t>(budget.total_bytes);
  current_max_size_ = max_size_;
  if (budget.idle_expiry_seconds > 0 && budget.sweep_interval_seconds > 0) {
    molt_expiry_sweep_.Start(
        FROM_HERE, base::Seconds(budget.sweep_interval_seconds),
        base::BindRepeating(&MemBackendImpl::MoltStartSweep,
                            base::Unretained(this)));
  }
}

int64_t MemBackendImpl::MoltSiteSizeForTesting(std::string_view site) const {
  return MoltSiteSize(site);
}

void MemBackendImpl::SetPostCleanupCallback(base::OnceClosure cb) {
  DCHECK(post_cleanup_callback_.is_null());
  post_cleanup_callback_ = std::move(cb);
}

// static
base::Time MemBackendImpl::Now(const base::WeakPtr<MemBackendImpl>& self) {
  MemBackendImpl* instance = self.get();
  if (instance && instance->custom_clock_for_testing_)
    return instance->custom_clock_for_testing_->Now();
  return Time::Now();
}

void MemBackendImpl::SetClockForTesting(base::Clock* clock) {
  custom_clock_for_testing_ = clock;
}

base::expected<int32_t, net::Error> MemBackendImpl::GetEntryCount(
    GetEntryCountCallback callback) const {
  return base::ok(static_cast<int32_t>(entries_.size()));
}

EntryResult MemBackendImpl::OpenOrCreateEntry(const std::string& key,
                                              net::RequestPriority priority,
                                              EntryResultCallback callback) {
  EntryResult result = OpenEntry(key, priority, EntryResultCallback());
  if (result.net_error() == net::OK)
    return result;

  // Key was not opened, try creating it instead.
  return CreateEntry(key, priority, EntryResultCallback());
}

EntryResult MemBackendImpl::OpenEntry(const std::string& key,
                                      net::RequestPriority request_priority,
                                      EntryResultCallback callback) {
  auto it = entries_.find(key);
  if (it == entries_.end())
    return EntryResult::MakeError(net::ERR_FAILED);

  // MoltBrowser: with per-site limits, an entry unused for the idle expiry
  // is gone, whatever other sites did meanwhile.
  if (MoltHasBudget() && !it->second->InUse() && MoltIsExpired(it->second)) {
    it->second->Doom();
    return EntryResult::MakeError(net::ERR_FAILED);
  }

  it->second->Open();

  return EntryResult::MakeOpened(it->second);
}

EntryResult MemBackendImpl::CreateEntry(const std::string& key,
                                        net::RequestPriority request_priority,
                                        EntryResultCallback callback) {
  // MoltBrowser: with per-site limits, a new entry's key counts like its
  // data: none its site's own entries can't make room for, or past the
  // total (molt_mem_backend_budget_policy.h).
  if (MoltHasBudget() && !MoltMayCreateEntry(key)) {
    return EntryResult::MakeError(net::ERR_INSUFFICIENT_RESOURCES);
  }
  std::pair<EntryMap::iterator, bool> create_result =
      entries_.insert(EntryMap::value_type(key, nullptr));
  const bool did_insert = create_result.second;
  if (!did_insert)
    return EntryResult::MakeError(net::ERR_FAILED);

  MemEntryImpl* cache_entry =
      new MemEntryImpl(weak_factory_.GetWeakPtr(), key, net_log_);
  create_result.first->second = cache_entry;
  return EntryResult::MakeCreated(cache_entry);
}

net::Error MemBackendImpl::DoomEntry(const std::string& key,
                                     net::RequestPriority priority,
                                     CompletionOnceCallback callback) {
  auto it = entries_.find(key);
  if (it == entries_.end())
    return net::ERR_FAILED;

  it->second->Doom();
  return net::OK;
}

net::Error MemBackendImpl::DoomAllEntries(CompletionOnceCallback callback) {
  return DoomEntriesBetween(Time(), Time(), std::move(callback));
}

net::Error MemBackendImpl::DoomEntriesBetween(Time initial_time,
                                              Time end_time,
                                              CompletionOnceCallback callback) {
  if (end_time.is_null())
    end_time = Time::Max();
  DCHECK_GE(end_time, initial_time);

  base::LinkNode<MemEntryImpl>* node = lru_list_.head();
  while (node != lru_list_.end()) {
    MemEntryImpl* candidate = node->value();
    node = NextSkippingChildren(lru_list_, node);

    if (candidate->GetLastUsed() >= initial_time &&
        candidate->GetLastUsed() < end_time) {
      candidate->Doom();
    }
  }

  return net::OK;
}

net::Error MemBackendImpl::DoomEntriesSince(Time initial_time,
                                            CompletionOnceCallback callback) {
  return DoomEntriesBetween(initial_time, Time::Max(), std::move(callback));
}

int64_t MemBackendImpl::CalculateSizeOfAllEntries(
    Int64CompletionOnceCallback callback) {
  return current_size_;
}

int64_t MemBackendImpl::CalculateSizeOfEntriesBetween(
    base::Time initial_time,
    base::Time end_time,
    Int64CompletionOnceCallback callback) {
  if (end_time.is_null())
    end_time = Time::Max();
  DCHECK_GE(end_time, initial_time);

  int size = 0;
  base::LinkNode<MemEntryImpl>* node = lru_list_.head();
  while (node != lru_list_.end()) {
    MemEntryImpl* entry = node->value();
    if (entry->GetLastUsed() >= initial_time &&
        entry->GetLastUsed() < end_time) {
      size += entry->GetStorageSize();
    }
    node = node->next();
  }
  return size;
}

class MemBackendImpl::MemIterator final : public Backend::Iterator {
 public:
  explicit MemIterator(base::WeakPtr<MemBackendImpl> backend)
      : backend_(backend) {}

  EntryResult OpenNextEntry(EntryResultCallback callback) override {
    if (!backend_)
      return EntryResult::MakeError(net::ERR_FAILED);

    if (!backend_keys_) {
      backend_keys_ = std::make_unique<Strings>(backend_->entries_.size());
      for (const auto& iter : backend_->entries_)
        backend_keys_->push_back(iter.first);
      current_ = backend_keys_->begin();
    } else {
      current_++;
    }

    while (true) {
      if (current_ == backend_keys_->end()) {
        backend_keys_.reset();
        return EntryResult::MakeError(net::ERR_FAILED);
      }

      const auto& entry_iter = backend_->entries_.find(*current_);
      if (entry_iter == backend_->entries_.end()) {
        // The key is no longer in the cache, move on to the next key.
        current_++;
        continue;
      }

      entry_iter->second->Open();
      return EntryResult::MakeOpened(entry_iter->second);
    }
  }

 private:
  using Strings = std::vector<std::string>;

  base::WeakPtr<MemBackendImpl> backend_;
  std::unique_ptr<Strings> backend_keys_;
  Strings::iterator current_;
};

std::unique_ptr<Backend::Iterator> MemBackendImpl::CreateIterator() {
  return std::make_unique<MemIterator>(weak_factory_.GetWeakPtr());
}

void MemBackendImpl::OnExternalCacheHit(const std::string& key) {
  auto it = entries_.find(key);
  if (it != entries_.end())
    it->second->UpdateStateOnUse();
}

void MemBackendImpl::SetMaxBytes(base::ByteSize max_bytes) {
  // MoltBrowser: per-site limits are fixed.
  if (MoltHasBudget()) {
    return;
  }
  max_size_ = base::saturated_cast<int32_t>(max_bytes.InBytes());
  if (base::FeatureList::IsEnabled(base::kStatefulMemoryPressure)) {
    current_max_size_ = CalculateTargetMemoryLimit();
  } else {
    current_max_size_ = max_size_;
  }
  EvictTill(current_max_size_);
}

base::ByteSize MemBackendImpl::GetMaxBytesForTesting() const {
  return base::ByteSize(base::checked_cast<uint64_t>(max_size_));
}

void MemBackendImpl::Init(int32_t max_bytes) {
  max_size_ = max_bytes ? max_bytes : CalculateDefaultMaxSize();
  current_max_size_ = max_size_;
}

void MemBackendImpl::EvictIfNeeded() {
  if (current_size_ <= current_max_size_) {
    return;
  }
  // Evict 10% more than necessary to avoid evicting on every insertion when the
  // cache is full.
  int target_size = current_max_size_ - (current_max_size_ / 10);
  EvictTill(target_size);
}

void MemBackendImpl::EvictTill(int target_size) {
  base::LinkNode<MemEntryImpl>* entry = lru_list_.head();
  while (current_size_ > target_size && entry != lru_list_.end()) {
    MemEntryImpl* to_doom = entry->value();
    entry = NextSkippingChildren(lru_list_, entry);

    if (!to_doom->InUse())
      to_doom->Doom();
  }
}

int32_t MemBackendImpl::CalculateTargetMemoryLimit() const {
  if (memory_limit() <= base::MemoryLimit::ModeratePressureThreshold()) {
    // Under moderate pressure or worse, we use linear interpolation to ensure
    // the cache is never completely cleared. We map the [0, 50] memory limit
    // range to [10%, 50%] of max_size_.
    float min = max_size_ / 10.0f;
    float max = max_size_ / 2.0f;
    return base::checked_cast<int32_t>(
        std::lerp(min, max, memory_limit().ratio() / 0.5));
  }

  return memory_limit().Scale(max_size_);
}

void MemBackendImpl::OnUpdateMemoryLimit() {
  // MoltBrowser: per-site limits don't follow the machine's memory.
  if (!base::FeatureList::IsEnabled(base::kStatefulMemoryPressure) ||
      MoltHasBudget()) {
    return;
  }
  // IMPORTANT: Ensure no memory is released during this call.
  // By using std::max, we ensure the new limit is at least the current size,
  // preventing growth without triggering immediate eviction.
  current_max_size_ = std::max(current_size_, CalculateTargetMemoryLimit());
}

void MemBackendImpl::OnReleaseMemory() {
  // MoltBrowser: nor does what per-site limits keep.
  if (MoltHasBudget()) {
    return;
  }
  if (base::FeatureList::IsEnabled(base::kStatefulMemoryPressure)) {
    // Now we actually evict entries to reach the target size.
    current_max_size_ = CalculateTargetMemoryLimit();
    EvictTill(current_max_size_);
  } else {
    // Stateless behavior, evict to specific limits.
    if (memory_limit() <= base::MemoryLimit::CriticalPressureThreshold()) {
      EvictTill(max_size_ / 10);
    } else if (memory_limit() <=
               base::MemoryLimit::ModeratePressureThreshold()) {
      EvictTill(max_size_ / 2);
    }
  }
}

}  // namespace disk_cache
