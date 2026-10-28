// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: a base::LRUCache or base::LRUCacheSet that, in MoltNet's
// network context, keeps per-site shares of its entries
// (molt_site_share_policy.h says why and how).
//
// MoltSiteLRU<Base, SiteOfKey> is the container `Base` (a base::LRUCache<K, V>
// or base::LRUCacheSet<K>) with Get(), Put(), Erase(), Clear(), Swap(),
// ShrinkToSize(), UpdateMaxSize() and max_size() of its own, which hide the
// base's: without MoltKeepPerSiteShares() each does exactly what the base's
// does. With it:
//  * the container evicts nothing itself: it no longer has a size of its own
//    (NO_AUTO_EVICT, so neither a full container nor the memory coordinator's
//    limits make Put() drop the least recently used entry of any site), and
//    ShrinkToSize() and UpdateMaxSize() do nothing;
//  * the cache calls MoltMakeRoomFor(key) before it puts a new entry: the
//    key's site's own least recently used entries go as the shares say, and
//    it returns whether the new entry is kept;
//  * every Put(), Get(), Erase() and Clear() keeps the shares' record of
//    each site's entries in order of use; Peek() changes nothing, as in the
//    base.
// The cache's own code keeps calling Get(), Put() and Erase() on the
// container as before: the names resolve to these.
//
// `SiteOfKey` is a functor returning a key's site (MoltSiteOfNetworkKey() of
// the key's network key). Header-only: //net's sources are Chromium's
// BUILD.gn, which MoltBrowser keeps small.

#ifndef NET_BASE_MOLT_SITE_LRU_CACHE_H_
#define NET_BASE_MOLT_SITE_LRU_CACHE_H_

#include <optional>
#include <type_traits>
#include <utility>

#include "base/containers/lru_cache.h"
#include "net/base/molt_site_share_policy.h"

namespace net {

template <class Base, class SiteOfKey>
class MoltSiteLRU : public Base {
 public:
  using typename Base::iterator;
  using typename Base::key_type;
  using typename Base::reverse_iterator;
  using typename Base::size_type;
  using typename Base::value_type;
  using Site = std::invoke_result_t<SiteOfKey, const key_type&>;

  using Base::Base;

  // MoltBrowser: from now on, per-site shares of `limits` (see above). The
  // entries already held stay (none, for MoltNet's context, which asks
  // before its first request), counted to their sites.
  void MoltKeepPerSiteShares(MoltSiteShareLimits limits) {
    if (shares_) {
      return;
    }
    Base held(Base::NO_AUTO_EVICT);
    Base::Swap(held);
    shares_.emplace(limits);
    // Least recently used first, so the order is kept.
    for (auto it = held.rbegin(); it != held.rend(); ++it) {
      Put(std::move(*it));
    }
  }
  bool MoltKeepsPerSiteShares() const { return shares_.has_value(); }

  // MoltBrowser: before a new entry for `key`: evicts its site's own least
  // recently used entries as the shares say, and returns whether the new
  // entry is kept. True without shares, or for a key already held.
  bool MoltMakeRoomFor(const key_type& key) {
    if (!shares_ || Base::Peek(key) != Base::end()) {
      return true;
    }
    const Site site = SiteOfKey()(key);
    while (std::optional<key_type> victim = shares_->NextToEvictFor(site)) {
      auto it = Base::Peek(*victim);
      if (it == Base::end()) {
        // Never: the record holds only what the container holds.
        shares_->Removed(*victim);
        continue;
      }
      Erase(it);
    }
    return shares_->HasRoomFor(site);
  }

  const MoltSiteShares<Site, key_type>* molt_shares() const {
    return shares_ ? &*shares_ : nullptr;
  }

  // The base's operations, keeping the record.
  iterator Put(value_type&& value) {
    iterator it = Base::Put(std::move(value));
    if (shares_ && it != Base::end()) {
      shares_->Added(SiteOfKey()(KeyOf(*it)), KeyOf(*it));
    }
    return it;
  }
  template <class K, class V>
  iterator Put(K&& key, V&& value) {
    return Put(value_type{std::forward<K>(key), std::forward<V>(value)});
  }
  template <typename K = key_type>
  iterator Get(const K& key) {
    iterator it = Base::Get(key);
    if (shares_ && it != Base::end()) {
      shares_->Used(KeyOf(*it));
    }
    return it;
  }
  iterator Erase(iterator pos) {
    if (shares_) {
      shares_->Removed(KeyOf(*pos));
    }
    return Base::Erase(pos);
  }
  reverse_iterator Erase(reverse_iterator pos) {
    return reverse_iterator(Erase((++pos).base()));
  }
  void Clear() {
    if (shares_) {
      shares_->Clear();
    }
    Base::Clear();
  }
  // With shares, `other`'s entries are taken in as Put() takes them (over
  // the shares if they are), and this container's go to `other`.
  void Swap(Base& other) {
    if (!shares_) {
      Base::Swap(other);
      return;
    }
    Base taken(Base::NO_AUTO_EVICT);
    taken.Swap(other);
    for (auto it = Base::rbegin(); it != Base::rend(); ++it) {
      other.Put(std::move(*it));
    }
    Clear();
    for (auto it = taken.rbegin(); it != taken.rend(); ++it) {
      Put(std::move(*it));
    }
  }
  // The container's own eviction of the least recently used entries of any
  // site: not with shares.
  void ShrinkToSize(size_type new_size) {
    if (!shares_) {
      Base::ShrinkToSize(new_size);
    }
  }
  void UpdateMaxSize(size_type new_max_size) {
    if (!shares_) {
      Base::UpdateMaxSize(new_max_size);
    }
  }
  size_type max_size() const {
    return shares_ ? shares_->limits().total : Base::max_size();
  }

 private:
  static const key_type& KeyOf(const value_type& value) {
    if constexpr (std::is_same_v<value_type, key_type>) {
      return value;
    } else {
      return value.first;
    }
  }

  std::optional<MoltSiteShares<Site, key_type>> shares_;
};

}  // namespace net

#endif  // NET_BASE_MOLT_SITE_LRU_CACHE_H_
