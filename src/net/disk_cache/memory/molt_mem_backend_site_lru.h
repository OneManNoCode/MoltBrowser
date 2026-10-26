// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: a least-recently-used list per site for MoltNet's in-memory
// HTTP cache (MemBackendImpl with per-site limits,
// molt_mem_backend_budget_policy.h).
//
// Chromium's in-memory cache keeps one least-recently-used list for all of
// its entries. With per-site limits, a write that takes a site past its
// share evicts that site's own least recently used entries; found on the
// one list, that walk passed every other site's entries too, so how long a
// site's write took depended on how many entries the other sites had. Here
// each site also has a list of its own entries (MoltCacheSiteLink, one in
// every MemEntryImpl, beside Chromium's), in the same order, and a site's
// eviction walks only that list: it costs what the site's own entries cost
// (the ones it evicts, and its entries in use, which it skips), whatever
// the other sites hold. Chromium's list stays, for its own paths (dooming
// entries by time, sizes by time, eviction without per-site limits).
//
// What else a site's operations cost doesn't depend on the other sites
// either:
//  * Finding a site's record is a hash lookup by the site's name, in a table
//    made once with room for kMoltCacheMaxSites sites (Reserve()): adding a
//    site never rehashes the table, so it costs the same however many other
//    sites there are. The hash is seeded per table, so which sites share a
//    bucket can't be chosen by naming sites. A site past the cap gets no
//    record: MemBackendImpl doesn't make its entries (HasRoomFor()), as for
//    a write that doesn't fit.
//  * Each entry carries when it was last used on a monotonic clock, set when
//    it moves to the end of its site's list, so a list is always in the
//    order of those times, whatever the wall clock does; expiry reads them.
//  * Unused entries are dropped by a sweep that goes site by site in steps
//    of at most a few entries (SweepStep()), each step a task of its own: a
//    site's sweep costs what its own expiring entries cost, and another
//    site's request waits for one step at most, however many entries other
//    sites have expiring.
//
// Not thread-safe: MemBackendImpl uses it on its sequence. Header-only, so
// a standalone harness instantiates it with a stand-in entry.

#ifndef NET_DISK_CACHE_MEMORY_MOLT_MEM_BACKEND_SITE_LRU_H_
#define NET_DISK_CACHE_MEMORY_MOLT_MEM_BACKEND_SITE_LRU_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "base/containers/linked_list.h"
#include "base/memory/raw_ptr.h"

namespace disk_cache {

// How many sites may have entries in one cache at once: the room the
// table of sites is made with. A site keeps a record while it has an entry
// (each kept at most kMoltHttpCacheIdleExpirySeconds unused), so this many
// is far more than one person visits in that time.
inline constexpr size_t kMoltCacheMaxSites = 4096;

// How many of a site's entries one sweep step looks at, at most.
inline constexpr size_t kMoltCacheSweepStepEntries = 64;

// An entry's place in its site's list. Each entry holds one, made with a
// pointer to the entry itself. Not in any list until the backend puts it in
// one; never removes itself (the backend removes it while the backend
// lives; a backend that is gone left its lists with it, as Chromium's list).
template <typename Entry>
class MoltCacheSiteLink : public base::LinkNode<MoltCacheSiteLink<Entry>> {
 public:
  explicit MoltCacheSiteLink(Entry* entry) : entry_(entry) {}
  MoltCacheSiteLink(const MoltCacheSiteLink&) = delete;
  MoltCacheSiteLink& operator=(const MoltCacheSiteLink&) = delete;

  Entry* entry() const { return entry_; }
  // base::LinkNode's links are null exactly while it is in no list.
  bool linked() const { return this->next() != nullptr; }
  void Unlink() {
    if (linked()) {
      this->RemoveFromList();
    }
  }
  // When the entry was last used, in microseconds on the backend's
  // monotonic clock (set by MoltCacheSiteLru::Touch()).
  int64_t last_used_us() const { return last_used_us_; }

 private:
  template <typename>
  friend class MoltCacheSiteLru;

  const raw_ptr<Entry> entry_;
  int64_t last_used_us_ = 0;
};

// A bijective 64-bit mix (splitmix64's finalizer), so that a seed changes
// which bucket every name lands in.
constexpr uint64_t MoltCacheMixHash(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

// The sites' records: each one's size (its entries' storage sizes, keys
// included) and its list, least recently used first. `Entry` provides
// `parent()` (the parent of a child entry, else null), as MemEntryImpl does.
template <typename Entry>
class MoltCacheSiteLru {
 public:
  using Link = MoltCacheSiteLink<Entry>;

  MoltCacheSiteLru() = default;
  MoltCacheSiteLru(const MoltCacheSiteLru&) = delete;
  MoltCacheSiteLru& operator=(const MoltCacheSiteLru&) = delete;

  // Makes the table of sites, with room for `max_sites` and hashed with
  // `seed`, before any site is added (with sites already in it, nothing
  // changes). Without it the table grows as Chromium's maps do, and
  // HasRoomFor() is always true.
  void Reserve(size_t max_sites, uint64_t seed) {
    if (!sites_.empty()) {
      return;
    }
    max_sites_ = max_sites;
    // The table never holds more than `max_sites` (HasRoomFor()), so with
    // a load factor of 1 it never rehashes.
    Map reserved(max_sites, NameHash{seed});
    reserved.max_load_factor(1.0f);
    reserved.reserve(max_sites);
    sites_ = std::move(reserved);
  }

  // Whether `site` has a record, or one may be made for it.
  bool HasRoomFor(std::string_view site) const {
    return max_sites_ == 0 || sites_.size() < max_sites_ ||
           sites_.find(site) != sites_.end();
  }

  int64_t SizeOf(std::string_view site) const {
    auto it = sites_.find(site);
    return it == sites_.end() ? 0 : it->second->size;
  }

  // `site`'s entries grew (or shrank) by `delta`.
  void AddSize(std::string_view site, int64_t delta) {
    Site& record = Get(site);
    record.size += delta;
    MaybeForget(site);
  }

  // `link`'s entry, of `site`, was made or used at `now_us` (microseconds
  // on a monotonic clock): last in its site's list.
  void Touch(std::string_view site, Link& link, int64_t now_us) {
    link.Unlink();
    link.last_used_us_ = now_us;
    Get(site).lru.Append(&link);
  }

  // `link`'s entry, of `site`, left the cache (it may still count until it
  // is deleted).
  void Remove(std::string_view site, Link& link) {
    link.Unlink();
    MaybeForget(site);
  }

  // While `site` uses more than `target`, walks its own entries from the
  // least recently used and calls `evict` on each that `may_evict` allows.
  // `evict` may delete the entry and its children (which leave the list
  // then); the walk steps past the entry's children that follow it first,
  // as Chromium's eviction does. Never touches another site's entries.
  template <typename MayEvict, typename Evict>
  void EvictTill(std::string_view site,
                 int64_t target,
                 MayEvict may_evict,
                 Evict evict) {
    auto it = sites_.find(site);
    if (it == sites_.end()) {
      return;
    }
    Site* record = it->second.get();
    // The record stays while its entries go.
    ++record->walks;
    const base::LinkNode<Link>* const end = record->lru.end();
    base::LinkNode<Link>* node = record->lru.head();
    while (record->size > target && node != end) {
      Entry* candidate = node->value()->entry();
      node = NextSkippingChildren(node, end);
      if (may_evict(candidate)) {
        evict(candidate);
      }
    }
    --record->walks;
    MaybeForget(site);
  }

  // ---- The sweep, site by site ----

  // Starts a round of the sweep at the first site, unless one is going on.
  // Sites added during the round are swept in it too.
  void StartSweep() {
    if (sweep_cursor_ == nullptr && !order_.empty()) {
      sweep_cursor_ = order_.head()->value();
    }
  }
  bool sweeping() const { return sweep_cursor_ != nullptr; }

  // One step of the round: walks the current site's list from its least
  // recently used entry, looking at `max_entries` at most, and calls `drop`
  // on each that `expired` and `may_drop` allow (`drop` may delete it and
  // its children, as for EvictTill()). The site's turn ends at its first
  // entry that hasn't expired (the rest of its list was used later), at the
  // end of its list, or when a step drops nothing (the entries it looked at
  // are in use); the round then goes on with the next site. Returns whether
  // the round goes on. Costs at most `max_entries` entries of one site.
  template <typename Expired, typename MayDrop, typename Drop>
  bool SweepStep(size_t max_entries,
                 Expired expired,
                 MayDrop may_drop,
                 Drop drop) {
    if (sweep_cursor_ == nullptr) {
      return false;
    }
    Site* record = sweep_cursor_;
    ++record->walks;
    const base::LinkNode<Link>* const end = record->lru.end();
    base::LinkNode<Link>* node = record->lru.head();
    size_t looked = 0;
    size_t dropped = 0;
    bool site_done = true;
    while (node != end) {
      if (looked == max_entries) {
        site_done = dropped == 0;
        break;
      }
      ++looked;
      Entry* candidate = node->value()->entry();
      if (!expired(node->value()->last_used_us())) {
        break;
      }
      node = NextSkippingChildren(node, end);
      if (may_drop(candidate)) {
        drop(candidate);
        ++dropped;
      }
    }
    --record->walks;
    if (site_done) {
      // On to the next site; this one may be forgotten now.
      base::LinkNode<Site>* next = record->next();
      sweep_cursor_ = next == order_.end() ? nullptr : next->value();
      MaybeForget(record->name);
    }
    return sweep_cursor_ != nullptr;
  }

  // Every site's list, in order (to put entries that were made before the
  // limits were set in their sites' lists, or take them out).
  void Clear() {
    for (auto& site : sites_) {
      while (!site.second->lru.empty()) {
        site.second->lru.head()->value()->Unlink();
      }
      site.second->RemoveFromList();
    }
    sweep_cursor_ = nullptr;
    sites_.clear();
  }

  size_t SiteCountForTesting() const { return sites_.size(); }
  size_t BucketCountForTesting() const { return sites_.bucket_count(); }
  // Calls `fn` on each of `site`'s entries, least recently used first.
  template <typename Fn>
  void ForEachForTesting(std::string_view site, Fn fn) const {
    auto it = sites_.find(site);
    if (it == sites_.end()) {
      return;
    }
    for (const base::LinkNode<Link>* node = it->second->lru.head();
         node != it->second->lru.end(); node = node->next()) {
      fn(node->value()->entry());
    }
  }

 private:
  // In `order_`, the order the sweep goes through the sites.
  struct Site : public base::LinkNode<Site> {
    explicit Site(std::string_view site_name) : name(site_name) {}
    const std::string name;
    int64_t size = 0;
    base::LinkedList<Link> lru;
    // Evictions or sweep steps walking the list now: the record stays
    // meanwhile.
    int walks = 0;
  };
  struct NameHash {
    using is_transparent = void;
    size_t operator()(std::string_view name) const {
      return static_cast<size_t>(MoltCacheMixHash(
          static_cast<uint64_t>(std::hash<std::string_view>()(name)) ^ seed));
    }
    uint64_t seed = 0;
  };
  using Map = std::unordered_map<std::string,
                                 std::unique_ptr<Site>,
                                 NameHash,
                                 std::equal_to<>>;

  // The node after `node`, past the children of `node`'s entry that follow
  // it.
  static base::LinkNode<Link>* NextSkippingChildren(
      base::LinkNode<Link>* node,
      const base::LinkNode<Link>* end) {
    const Entry* current = node->value()->entry();
    do {
      node = node->next();
    } while (node != end && node->value()->entry()->parent() == current);
    return node;
  }

  Site& Get(std::string_view site) {
    auto it = sites_.find(site);
    if (it == sites_.end()) {
      it = sites_.emplace(std::string(site), std::make_unique<Site>(site))
               .first;
      order_.Append(it->second.get());
    }
    return *it->second;
  }

  // A site with nothing counted and no entry listed is forgotten.
  void MaybeForget(std::string_view site) {
    auto it = sites_.find(site);
    if (it == sites_.end() || it->second->size > 0 ||
        !it->second->lru.empty() || it->second->walks != 0) {
      return;
    }
    Site* record = it->second.get();
    if (sweep_cursor_ == record) {
      base::LinkNode<Site>* next = record->next();
      sweep_cursor_ = next == order_.end() ? nullptr : next->value();
    }
    record->RemoveFromList();
    sites_.erase(it);
  }

  Map sites_;
  // 0: no cap (Reserve() not called).
  size_t max_sites_ = 0;
  // The sites in the order they were added: the sweep's order.
  base::LinkedList<Site> order_;
  // The site the sweep's next step is for; null between rounds.
  raw_ptr<Site> sweep_cursor_ = nullptr;
};

}  // namespace disk_cache

#endif  // NET_DISK_CACHE_MEMORY_MOLT_MEM_BACKEND_SITE_LRU_H_
