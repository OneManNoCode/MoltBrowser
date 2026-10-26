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
// the other sites hold. Finding a site's record is a hash lookup, by the
// site's name. Chromium's list stays, for its own paths (dooming entries by
// time, sizes by time, eviction without per-site limits).
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

 private:
  const raw_ptr<Entry> entry_;
};

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

  // `link`'s entry, of `site`, was made or used: last in its site's list.
  void Touch(std::string_view site, Link& link) {
    link.Unlink();
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
      do {
        node = node->next();
      } while (node != end && node->value()->entry()->parent() == candidate);
      if (may_evict(candidate)) {
        evict(candidate);
      }
    }
    --record->walks;
    MaybeForget(site);
  }

  // Every site's list, in order (to put entries that were made before the
  // limits were set in their sites' lists, or take them out).
  void Clear() {
    for (auto& site : sites_) {
      while (!site.second->lru.empty()) {
        site.second->lru.head()->value()->Unlink();
      }
    }
    sites_.clear();
  }

  size_t SiteCountForTesting() const { return sites_.size(); }
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
  struct Site {
    int64_t size = 0;
    base::LinkedList<Link> lru;
    // Evictions walking the list now: the record stays meanwhile.
    int walks = 0;
  };
  struct NameHash {
    using is_transparent = void;
    size_t operator()(std::string_view name) const {
      return std::hash<std::string_view>()(name);
    }
  };

  Site& Get(std::string_view site) {
    auto it = sites_.find(site);
    if (it == sites_.end()) {
      it = sites_.emplace(std::string(site), std::make_unique<Site>()).first;
    }
    return *it->second;
  }

  // A site with nothing counted and no entry listed is forgotten.
  void MaybeForget(std::string_view site) {
    auto it = sites_.find(site);
    if (it != sites_.end() && it->second->size <= 0 &&
        it->second->lru.empty() && it->second->walks == 0) {
      sites_.erase(it);
    }
  }

  std::unordered_map<std::string, std::unique_ptr<Site>, NameHash,
                     std::equal_to<>>
      sites_;
};

}  // namespace disk_cache

#endif  // NET_DISK_CACHE_MEMORY_MOLT_MEM_BACKEND_SITE_LRU_H_
