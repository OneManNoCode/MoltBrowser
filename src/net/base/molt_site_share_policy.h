// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: shares per site of the fixed-size network caches MoltNet's
// network context keeps for all its sites together.
//
// At Chromium 156 a network context keeps several caches whose entries are
// keyed by site (the request's network key: net::NetworkAnonymizationKey or
// net::NetworkIsolationKey) in one container of a fixed size, evicting the
// least recently used entry of any site (or, for CORS preflight results, ten
// entries at a random place) when a new one doesn't fit:
//  * TLS sessions (net::SSLClientSessionCache, 1024 entries): an evicted
//    session is no longer resumed, which the server sees;
//  * HTTP authentication entries (net::HttpAuthCache, 20): an evicted entry's
//    credentials are no longer sent preemptively, and the sign-in prompt
//    comes back;
//  * server properties (net::HttpServerProperties: the servers known to
//    support HTTP/2, 500 entries, and the servers that required HTTP/1.1,
//    100): an evicted HTTP/2 entry lets the next requests open parallel
//    connections instead of waiting for one session, an evicted HTTP/1.1 one
//    makes the next connection offer HTTP/2 again, both seen by the server;
//  * CORS preflight results (network::cors::PreflightCache, 1024): an
//    evicted result is fetched again, an OPTIONS request the server sees;
//  * compression dictionaries kept in memory (network::
//    SharedDictionaryManagerInMemory: 1000 dictionaries and 1% of the
//    machine's memory, half of each per site): past the total, the least
//    recently used dictionaries of any site go, and the next request of
//    their site no longer offers them.
// So one site could fill such a cache to evict another site's entries, and
// the other site's server could tell from its next visit.
//
// In MoltNet's network context each site (MoltSiteOfNetworkKey(): the
// network key's top-level site, for its first-party and third-party entries
// alike) has a share of its own (MoltSiteShares):
//  * a site holds at most `per_site` entries: a new entry of a site at its
//    share takes the place of that site's own least recently used entry;
//  * all sites together hold at most `total` entries: past it, a new entry
//    takes the place of its own site's least recently used entry, or, if
//    its site holds none, is not kept. No entry of another site is ever
//    evicted, and the container evicts nothing on its own, nor for memory
//    pressure.
// Finding a site's least recently used entry costs what the site's own
// record costs (a list of its entries in order of use, beside the cache's
// own order), never a walk past other sites' entries. A site's record goes
// with its last entry.
//
// What a page can still observe: its own site's share; and, once other
// sites hold the whole total between them (16 sites at their full shares),
// that a new entry of a site that holds none isn't kept.
//
// Std-only, so molt_ai_unittests tests it. Not thread-safe: each cache uses
// it on its own sequence.

#ifndef NET_BASE_MOLT_SITE_SHARE_POLICY_H_
#define NET_BASE_MOLT_SITE_SHARE_POLICY_H_

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <utility>

namespace net {

// A cache's shares: what one site, and all sites together, may hold.
struct MoltSiteShareLimits {
  size_t per_site = 0;
  size_t total = 0;

  friend constexpr bool operator==(const MoltSiteShareLimits&,
                                   const MoltSiteShareLimits&) = default;
};

// How many sites' full shares the total of each cache holds.
inline constexpr size_t kMoltSiteShareSites = 16;

constexpr MoltSiteShareLimits MoltSiteShareLimitsOf(size_t per_site) {
  return {per_site, per_site * kMoltSiteShareSites};
}

// TLS sessions: a quarter of Chromium's 1024 for one site (each server the
// site's pages connect to, per privacy mode).
inline constexpr MoltSiteShareLimits kMoltTlsSessionShares =
    MoltSiteShareLimitsOf(256);
// HTTP authentication entries (server credentials): Chromium's 20 for all
// sites, for each site.
inline constexpr MoltSiteShareLimits kMoltHttpAuthShares =
    MoltSiteShareLimitsOf(20);
// Server properties: Chromium's 500 HTTP/2 entries and 100 HTTP/1.1 ones for
// all sites, for each site.
inline constexpr MoltSiteShareLimits kMoltServerInfoShares =
    MoltSiteShareLimitsOf(500);
inline constexpr MoltSiteShareLimits kMoltHttp11RequiredShares =
    MoltSiteShareLimitsOf(100);
// CORS preflight results (one per origin and URL): an eighth of Chromium's
// 1024 for one site.
inline constexpr MoltSiteShareLimits kMoltPreflightShares =
    MoltSiteShareLimitsOf(128);
// Compression dictionaries in memory: 100 dictionaries and 4 MiB per site,
// whatever the machine (Chromium: half of 1000, and half of 1% of the
// machine's memory, at most 200 MiB, so a site could tell roughly how much
// memory the machine has), 16 sites' worth in all: 64 MiB, below what
// Chromium allows on a machine of 6.4 GB or more. A site past its share
// loses its own oldest dictionaries, only compressing less.
inline constexpr MoltSiteShareLimits kMoltDictionaryShares =
    MoltSiteShareLimitsOf(100);
inline constexpr uint64_t kMoltDictionaryBytesPerSite = 4 * 1024 * 1024;
inline constexpr uint64_t kMoltDictionaryBytesTotal =
    kMoltDictionaryBytesPerSite * kMoltSiteShareSites;
// The dictionary storages (one per frame origin and top-level site) kept
// when no request uses them: Chromium's 10 for all sites, for each site. A
// storage no request uses and none keeps is dropped, with its dictionaries.
inline constexpr MoltSiteShareLimits kMoltDictionaryStorageShares =
    MoltSiteShareLimitsOf(10);

// Compression dictionaries are added first and evicted after, as Chromium
// does: while a site, or all sites, hold more than their share
// (`site_count` dictionaries of `site_bytes`, and `all_count` of
// `all_bytes`), the site's own least recently used dictionary goes, its
// newest last; never another site's.
constexpr bool MoltSiteDictionariesOver(size_t site_count,
                                        uint64_t site_bytes,
                                        size_t all_count,
                                        uint64_t all_bytes) {
  return site_count > kMoltDictionaryShares.per_site ||
         site_bytes > kMoltDictionaryBytesPerSite ||
         all_count > kMoltDictionaryShares.total ||
         all_bytes > kMoltDictionaryBytesTotal;
}

// Before a new entry of a site that holds `site_holds` entries, with
// `all_hold` held by all sites: while this is true, the site's own least
// recently used entry goes (the site is at its share, or the cache at its
// total and the site holds some).
constexpr bool MoltSiteShareEvictsOwn(MoltSiteShareLimits limits,
                                      size_t site_holds,
                                      size_t all_hold) {
  return site_holds > 0 &&
         (site_holds >= limits.per_site || all_hold >= limits.total);
}

// Then: whether the new entry is kept. It isn't when the cache is at its
// total and its site holds nothing to make room with; no other site's entry
// is evicted for it.
constexpr bool MoltSiteShareHasRoom(MoltSiteShareLimits limits,
                                    size_t site_holds,
                                    size_t all_hold) {
  return site_holds < limits.per_site && all_hold < limits.total;
}

// The entries of a cache, by site, each site's in order of use. `Site` and
// `Key` are ordered (std::map); a key is held once, its site's list pointing
// at the copy in the index.
template <typename Site, typename Key>
class MoltSiteShares {
 public:
  explicit MoltSiteShares(MoltSiteShareLimits limits) : limits_(limits) {}
  MoltSiteShares(const MoltSiteShares&) = delete;
  MoltSiteShares& operator=(const MoltSiteShares&) = delete;
  ~MoltSiteShares() = default;

  const MoltSiteShareLimits& limits() const { return limits_; }

  // Making room for a new entry of `site`: the key of the entry to evict
  // first, the site's own least recently used, or nullopt once there is
  // nothing (more) to evict (MoltSiteShareEvictsOwn()). The cache evicts it
  // and calls Removed(), then asks again.
  std::optional<Key> NextToEvictFor(const Site& site) const {
    auto it = sites_.find(site);
    const size_t holds = it == sites_.end() ? 0 : it->second.size();
    if (!MoltSiteShareEvictsOwn(limits_, holds, total_)) {
      return std::nullopt;
    }
    return *it->second.back();
  }

  // Then: whether a new entry of `site` is kept (MoltSiteShareHasRoom()).
  bool HasRoomFor(const Site& site) const {
    return MoltSiteShareHasRoom(limits_, HeldBy(site), total_);
  }

  // `key`, of `site`, was stored: it is its site's most recently used. A
  // key already held is only used.
  void Added(const Site& site, const Key& key) {
    if (where_.contains(key)) {
      Used(key);
      return;
    }
    auto [site_it, inserted] = sites_.try_emplace(site);
    auto [where_it, added] = where_.emplace(key, Place());
    site_it->second.push_front(&where_it->first);
    where_it->second.site = site_it;
    where_it->second.position = site_it->second.begin();
    ++total_;
  }

  // `key` was used: it becomes its site's most recently used.
  void Used(const Key& key) {
    auto it = where_.find(key);
    if (it == where_.end()) {
      return;
    }
    Order& order = it->second.site->second;
    order.splice(order.begin(), order, it->second.position);
  }

  // `key` left the cache.
  void Removed(const Key& key) {
    auto it = where_.find(key);
    if (it == where_.end()) {
      return;
    }
    auto site_it = it->second.site;
    site_it->second.erase(it->second.position);
    if (site_it->second.empty()) {
      sites_.erase(site_it);
    }
    where_.erase(it);
    --total_;
  }

  void Clear() {
    sites_.clear();
    where_.clear();
    total_ = 0;
  }

  bool Holds(const Key& key) const { return where_.contains(key); }
  size_t HeldBy(const Site& site) const {
    auto it = sites_.find(site);
    return it == sites_.end() ? 0 : it->second.size();
  }
  size_t total() const { return total_; }
  // How many sites hold entries (their records).
  size_t sites() const { return sites_.size(); }

 private:
  using Order = std::list<const Key*>;  // most recently used first
  using Sites = std::map<Site, Order>;
  struct Place {
    typename Sites::iterator site;
    typename Order::iterator position;
  };

  const MoltSiteShareLimits limits_;
  Sites sites_;
  std::map<Key, Place> where_;
  size_t total_ = 0;
};

}  // namespace net

#endif  // NET_BASE_MOLT_SITE_SHARE_POLICY_H_
