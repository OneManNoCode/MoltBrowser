// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: per-site connection limits for MoltNet's network context.
//
// Chromium keeps one socket pool per proxy chain in each network context
// (ClientSocketPoolManagerImpl), with two limits: 6 connections per group (a
// destination, for one network anonymization key, ...) and 128 per proxy
// chain for the whole pool (ClientSocketPoolManager::
// max_sockets_per_proxy_chain(), which the MaxConnectionsPerProxy policy
// changes; at Chromium 156 the pool may also grow past it by a random
// amount, up to twice, net/socket/socket_pool_additional_capacity.h). All of
// MoltNet's traffic goes through one proxy chain, Tor's SOCKS5 port, so
// every site would share that pool: one site keeping its connections open
// makes every other site's new connections wait, and a site open at the same
// time can measure that wait.
//
// In MoltNet's network context:
//
//  * Each site gets socket pools of its own for a proxy chain
//    (molt_per_site_socket_pools.h): the same TransportClientSocketPool, with
//    Chromium's per-group limit and fixed limits that MaxConnectionsPerProxy
//    doesn't change and that don't grow at random. A site is the top-level
//    site of the request's network anonymization key, so a page's
//    first-party and third-party keys count together; a key whose top-level
//    site is opaque (a page the server sandboxed) counts against the site
//    the opaque origin was derived from; a certificate fetch counts against
//    the site whose connection needed it (its one-off key has that site as
//    top-level site, net/cert/molt_cert_fetch_site.h); the one-off keys no
//    site can be named for share one small pool of their own
//    (MoltBudgetSiteFor() in molt_connection_budget.h).
//
//  * No pool waits on itself (MoltUsesCertFetchPool()). A connection whose
//    certificate check downloads a certificate (AIA, OCSP, CRL) holds its
//    slot until that download is done, so the downloads get pools of their
//    own, one per site and one for the keys of no site, which nothing else
//    uses: the certificate fetcher marks its requests as ones that may not
//    start certificate downloads of their own
//    (LOAD_DISABLE_CERT_NETWORK_FETCHES; they are plain HTTP anyway), and
//    only requests so marked go there. So a pool's connections can wait
//    only for a pool whose connections wait for nothing, whatever the
//    sites do: a site's main pool, with its credentialless iframes' one-off
//    keys, and the pool of the keys of no site depend on certificate-fetch
//    pools, and those on nothing.
//
//  * All connections to Tor together stay within a fixed budget
//    (kMoltNetConnectionBudget), well inside what MoltNet's single Tor
//    process can hold, so that cap can't become a channel between sites
//    either: the budget fits kMoltSitesToFillBudget sites' full shares, so
//    a site below its own limits waits for the budget only while that many
//    other sites are at theirs. When it is reached, the next freed
//    connection goes to the waiting site that uses the fewest, and an idle
//    connection of a site using more than the waiting one is closed for it
//    (MoltPreemptionOrder()). The budget is Tor's, not a network context's:
//    every MoltNet network context alive at the same time (the identity New
//    Identity is ending and the next one) draws on the same one, and a
//    site of one identity is a site of its own in it.
//
//  * Requests that may ignore the limits (synchronous XMLHttpRequests) skip
//    only the per-group limit; past the site's own they go only by a few,
//    outside the budget (kMoltIgnoreLimitsOverflow).
//
//  * SOCKS handshakes in progress, which are what make Tor build circuits
//    for streams it can't put on an existing one, are limited the same way
//    (MoltWeightedSlots): each one weighs the circuits it can make Tor
//    build at a time (MoltSocksHandshakeWeight(): 3 for an onion service,
//    else 1), a site may have kMoltSocksHandshakesPerSite of weight in
//    progress, and all sites together kMoltSocksHandshakeBudget, which fits
//    kMoltSitesToFillHandshakeBudget sites' full shares and stays below
//    Tor's MaxClientCircuitsPending with room for Tor's own circuits
//    (torrc_builder.h). A freed slot goes to the waiting site with the
//    fewest in progress first.
//
// Every other network context keeps Chromium's single pool per proxy chain.
// Std-only, so molt_ai_unittests and standalone harnesses test it.

#ifndef NET_SOCKET_MOLT_PER_SITE_SOCKET_POOL_POLICY_H_
#define NET_SOCKET_MOLT_PER_SITE_SOCKET_POOL_POLICY_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace net {

// A site's own limits: connections for HTTP (with Chromium's 6 per group
// inside), and WebSockets.
inline constexpr size_t kMoltPerSitePoolSize = 48;
inline constexpr size_t kMoltPerSiteWebSocketPoolSize = 16;
// The pool (per pool type) of the one-off keys no site can be named for.
inline constexpr size_t kMoltUnattributedPoolSize = 8;
// The certificate-fetch pools (HTTP only; MoltUsesCertFetchPool()): a
// site's, and the one for the keys of no site.
inline constexpr size_t kMoltPerSiteCertFetchPoolSize = 4;
inline constexpr size_t kMoltUnattributedCertFetchPoolSize = 4;

// How long a site's pool may sit unused before it is dropped.
inline constexpr int64_t kMoltUnusedSitePoolTimeoutSeconds = 60;

// The most connections MoltNet's network context has open or opening to Tor
// at a time, all sites and both pool types together.
inline constexpr size_t kMoltNetConnectionBudget = 768;

// What a single Tor client process can always hold: Tor refuses to start
// unless it may open ConnLimit file descriptors (MoltNet writes Tor's
// default, 1000, into its torrc), and keeps 32 of them for itself, so a Tor
// that runs accepts at least 968 connections, on every system. The budget
// leaves a fifth of that beyond it: for the few sockets requests that ignore
// the limits may take past their site's pool (kMoltIgnoreLimitsOverflow), and
// at least kMoltTorOwnConnectionsFloor for Tor's own connections (its
// listeners, the control connection, guard and bridge connections, which
// also carry its directory fetches, and files).
inline constexpr size_t kMoltTorConnLimit = 1000;
inline constexpr size_t kMoltTorKeptFileDescriptors = 32;
inline constexpr size_t kMoltTorOwnConnections = 200;
inline constexpr size_t kMoltTorOwnConnectionsFloor = 128;
static_assert(kMoltNetConnectionBudget + kMoltTorOwnConnections <=
              kMoltTorConnLimit - kMoltTorKeptFileDescriptors);

// Requests that may ignore the limits (LOAD_IGNORE_LIMITS: Blink sets it on
// every synchronous XMLHttpRequest, whose thread waits for it, so that it
// never waits behind requests of its own page that can't finish while that
// thread waits) skip the per-group limit, as in Chromium, but not their
// site's: they wait for the site's pool and the budget like any request,
// except that once the site's HTTP pool holds its whole limit, a few more
// may go past it (MoltIgnoreLimitsMayOverflow()). Those few aren't counted
// in the budget other sites share (MoltPerSiteSocketPools::MoltAddLoads()),
// they come out of the margin above: as only sockets within a pool's limit
// count in the budget, at most kMoltOverflowingPools HTTP pools hold their
// whole limit at a time, so at most kMoltIgnoreLimitsOverflowTotal sockets
// go past the limits, whatever the sites do.
inline constexpr size_t kMoltIgnoreLimitsOverflow = 4;
inline constexpr size_t kMoltOverflowingPools =
    kMoltNetConnectionBudget / kMoltPerSitePoolSize;
inline constexpr size_t kMoltIgnoreLimitsOverflowTotal =
    kMoltOverflowingPools * kMoltIgnoreLimitsOverflow;
static_assert(kMoltIgnoreLimitsOverflowTotal + kMoltTorOwnConnectionsFloor <=
              kMoltTorOwnConnections);
static_assert(kMoltNetConnectionBudget + kMoltIgnoreLimitsOverflowTotal +
                  kMoltTorOwnConnectionsFloor <=
              kMoltTorConnLimit - kMoltTorKeptFileDescriptors);

// How far a site's pool may go past its limit for requests that ignore the
// limits: its HTTP pool only (WebSockets never ignore them; the keys of no
// site and the certificate-fetch pools keep their small pools).
constexpr size_t MoltIgnoreLimitsOverflowFor(bool for_websockets,
                                             bool unattributed,
                                             bool cert_fetch_pool) {
  return for_websockets || unattributed || cert_fetch_pool
             ? 0
             : kMoltIgnoreLimitsOverflow;
}

// Whether a request that ignores the limits may take a socket past its
// pool's `limit` now: only while the pool holds its whole limit (`in_use`,
// so the budget isn't what holds it back) and fewer than `overflow` more.
constexpr bool MoltIgnoreLimitsMayOverflow(size_t in_use,
                                           size_t limit,
                                           size_t overflow) {
  return in_use >= limit && in_use < limit + overflow;
}

// SOCKS handshakes in progress at a time, by weight
// (MoltSocksHandshakeWeight()): per site, for the keys no site can be named
// for together, and for every MoltNet network context together (one Tor).
inline constexpr size_t kMoltSocksHandshakesPerSite = 16;
inline constexpr size_t kMoltUnattributedSocksHandshakes = 8;
inline constexpr size_t kMoltSocksHandshakeBudget = 192;

// The most circuits one SOCKS handshake can make Tor build at a time: an
// onion service needs its descriptor fetched, then an introduction and a
// rendezvous circuit; any other destination one circuit, when no open or
// pending circuit can take the stream.
inline constexpr size_t kMoltOnionHandshakeWeight = 3;
inline constexpr size_t kMoltHandshakeWeight = 1;

// Tor builds at most this many circuits at a time for all of its clients'
// streams (MaxClientCircuitsPending, as MoltNet's torrc sets it), counting
// the ones it builds on its own (preemptive circuits for predicted ports, up
// to 14 kept unused, and circuits it keeps building to measure build times
// after they exceed the timeout), for which kMoltTorOwnPendingCircuits stay
// free.
inline constexpr size_t kMoltTorMaxClientCircuitsPending = 256;
inline constexpr size_t kMoltTorOwnPendingCircuits = 32;
static_assert(kMoltSocksHandshakeBudget + kMoltUnattributedSocksHandshakes +
                  kMoltTorOwnPendingCircuits <=
              kMoltTorMaxClientCircuitsPending);
static_assert(kMoltOnionHandshakeWeight <= kMoltUnattributedSocksHandshakes);

// Whether requests through a proxy chain get a pool per site:
// `per_site_pool_size` is the network context's setting (0: Chromium's one
// pool per proxy chain). Direct connections never do (MoltNet makes none;
// its context only ever has Tor's proxy configuration, or one that refuses
// every request).
constexpr bool MoltUsesPerSitePool(size_t per_site_pool_size,
                                   bool proxy_chain_is_direct) {
  return per_site_pool_size > 0 && !proxy_chain_is_direct;
}

// Whether a request goes to a certificate-fetch pool (its site's, or the
// one for the keys of no site): an HTTP request (`for_websockets` false)
// that may not start certificate downloads (`no_cert_fetches`: its load
// flags have LOAD_DISABLE_CERT_NETWORK_FETCHES, as the certificate
// fetcher's requests do in MoltNet). Such a request's connection never
// waits for a certificate download, so it never waits for another
// connection; every other request's may, for the downloads its check
// starts, which therefore must never need a slot such connections hold:
// with every slot of a pool held by connections waiting for their checks,
// the downloads would wait for slots only they can free (until the
// fetcher's timeout failed the checks).
constexpr bool MoltUsesCertFetchPool(bool for_websockets,
                                     bool no_cert_fetches) {
  return !for_websockets && no_cert_fetches;
}

// The limit of one site's pool: `per_site_pool_size` (the context's
// setting) for HTTP, less for WebSockets, for the one-off keys no site can
// be named for (`unattributed`) and for the certificate-fetch pools
// (`cert_fetch_pool`, MoltUsesCertFetchPool(): a site's, or, with
// `unattributed`, the one for the keys of no site).
constexpr size_t MoltPerSitePoolLimit(size_t per_site_pool_size,
                                      bool for_websockets,
                                      bool unattributed,
                                      bool cert_fetch_pool = false) {
  if (cert_fetch_pool) {
    return std::min(per_site_pool_size,
                    unattributed ? kMoltUnattributedCertFetchPoolSize
                                 : kMoltPerSiteCertFetchPoolSize);
  }
  if (unattributed) {
    return std::min(per_site_pool_size, kMoltUnattributedPoolSize);
  }
  if (for_websockets) {
    return std::min(per_site_pool_size, kMoltPerSiteWebSocketPoolSize);
  }
  return per_site_pool_size;
}

// The per-group limit of a site's pool: Chromium's for the pool type,
// never more than the pool's own limit (as for a proxy chain's pool).
constexpr size_t MoltPerSiteGroupLimit(size_t site_pool_limit,
                                       size_t max_sockets_per_group) {
  return std::min(site_pool_limit, max_sockets_per_group);
}

// Whether a site's pool may be dropped: nothing uses it (no group, socket,
// connect job, pending callback or higher-layered pool) and it hasn't been
// handed out for the timeout. A pool that is handed out is used at once
// (ClientSocketHandle::Init(), in the same task), so a pool that was
// unused at both ends of the timeout holds nothing a request could need.
constexpr bool MoltMayDropSitePool(bool pool_unused,
                                   int64_t seconds_since_handed_out) {
  return pool_unused &&
         seconds_since_handed_out >= kMoltUnusedSitePoolTimeoutSeconds;
}

// The weight of SOCKS handshakes a site may have in progress at a time.
constexpr size_t MoltSocksHandshakeLimit(bool unattributed) {
  return unattributed ? kMoltUnattributedSocksHandshakes
                      : kMoltSocksHandshakesPerSite;
}

// Whether `host` (a SOCKS destination: a hostname, or an IP literal) is an
// onion service's: it ends in ".onion", in any case, with or without the
// final dot.
constexpr bool MoltIsOnionHost(std::string_view host) {
  if (!host.empty() && host.back() == '.') {
    host.remove_suffix(1);
  }
  constexpr std::string_view kOnion = ".onion";
  if (host.size() <= kOnion.size()) {
    return false;
  }
  const std::string_view tail = host.substr(host.size() - kOnion.size());
  for (size_t i = 0; i < kOnion.size(); ++i) {
    char c = tail[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != kOnion[i]) {
      return false;
    }
  }
  return true;
}

// The weight of a SOCKS handshake to `host`: the circuits it can make Tor
// build at a time.
constexpr size_t MoltSocksHandshakeWeight(std::string_view host) {
  return MoltIsOnionHost(host) ? kMoltOnionHandshakeWeight
                               : kMoltHandshakeWeight;
}

// ---- The connection budget ----

// What one site may hold in MoltNet's context, and the unattributed pools.
inline constexpr size_t kMoltPerSiteConnections =
    kMoltPerSitePoolSize + kMoltPerSiteWebSocketPoolSize +
    kMoltPerSiteCertFetchPoolSize;
inline constexpr size_t kMoltUnattributedConnections =
    2 * kMoltUnattributedPoolSize + kMoltUnattributedCertFetchPoolSize;

// How many other sites must hold their full share (with every unattributed
// connection in use too) before a site that is still below its own limits
// can be made to wait for the budget: it waits only while the context holds
// the whole budget, and it holds at most one less than its share itself.
constexpr size_t MoltSitesToFillBudget(size_t budget,
                                       size_t per_site,
                                       size_t unattributed) {
  // Others must hold at least budget - (per_site - 1) - unattributed.
  const size_t needed = budget - (per_site - 1) - unattributed;
  return (needed + per_site - 1) / per_site;
}
inline constexpr size_t kMoltSitesToFillBudget =
    MoltSitesToFillBudget(kMoltNetConnectionBudget, kMoltPerSiteConnections,
                          kMoltUnattributedConnections);
static_assert(kMoltSitesToFillBudget == 11);
// Eleven sites at their full share, each also past its limit by all it may
// (kMoltIgnoreLimitsOverflow), with every unattributed connection, still
// leave Tor its own.
static_assert(kMoltSitesToFillBudget *
                      (kMoltPerSiteConnections + kMoltIgnoreLimitsOverflow) +
                  kMoltUnattributedConnections <=
              kMoltTorConnLimit - kMoltTorKeptFileDescriptors -
                  kMoltTorOwnConnectionsFloor);

// The same for SOCKS handshakes. With weights the rule is unchanged: a site
// holding at most its share less the weight w it asks for waits only while
// the others hold more than the budget less w less what it holds, so more
// than budget - share; one less than that is budget - (share - 1).
inline constexpr size_t kMoltSitesToFillHandshakeBudget =
    MoltSitesToFillBudget(kMoltSocksHandshakeBudget,
                          kMoltSocksHandshakesPerSite,
                          kMoltUnattributedSocksHandshakes);
static_assert(kMoltSitesToFillHandshakeBudget == 11);

// Whether a site using `site_in_use` connections may open one more while
// the context has `total_in_use` open or opening (the site's own limits are
// its pools'): only within the budget, and only if no site using fewer is
// already waiting for it (that one gets the next freed connection).
constexpr bool MoltBudgetAdmits(size_t total_in_use,
                                size_t budget,
                                size_t site_in_use,
                                std::optional<size_t> smallest_waiting) {
  if (total_in_use >= budget) {
    return false;
  }
  return !smallest_waiting || *smallest_waiting >= site_in_use;
}

// The order in which waiting sites get freed connections: the one using the
// fewest first; among equals, the one that has waited longest (waiters are
// given oldest first).
inline std::vector<size_t> MoltWakeOrder(std::span<const size_t> in_use) {
  std::vector<size_t> order(in_use.size());
  for (size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return in_use[a] < in_use[b];
  });
  return order;
}

// While the budget is full: whose idle connections may be closed so that a
// waiting site using `waiter_in_use` gets one, in the order to try them.
// Only sites that would still use at least as many as the waiter afterwards
// (more than one more than it now); the one using the most first, ties in
// the order given.
inline std::vector<size_t> MoltPreemptionOrder(std::span<const size_t> in_use,
                                               size_t waiter_in_use) {
  std::vector<size_t> order;
  for (size_t i = 0; i < in_use.size(); ++i) {
    if (in_use[i] > waiter_in_use + 1) {
      order.push_back(i);
    }
  }
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return in_use[a] > in_use[b];
  });
  return order;
}

// ---- SOCKS handshake slots ----

// Slots handed out by weight within a limit per key and a budget for all
// keys together, as MoltConnectionBudget hands out connections: a key's
// request is granted when its key has room for its weight and the budget
// has too, and no key holding fewer slots is waiting for the budget (that
// one goes first). Within a key, by priority (higher first), then by
// arrival. Ids are the caller's and unique per key. Every call returns the
// ids it granted, in the order granted (Acquire()'s own id among them if it
// was granted at once).
template <typename Key, typename Id>
class MoltWeightedSlots {
 public:
  explicit MoltWeightedSlots(size_t budget) : budget_(budget) {}

  // Asks for `weight` of `key`'s `limit` slots (and of the budget) for
  // `id`. A weight is at least 1 and at most the limit.
  std::vector<Id> Acquire(const Key& key,
                          size_t limit,
                          size_t weight,
                          Id id,
                          int priority) {
    Entry& entry = entries_[key];
    entry.limit = std::max<size_t>(limit, 1);
    const size_t w = std::clamp<size_t>(weight, 1, entry.limit);
    entry.waiting.push_back(Waiter{id, w, priority, next_seq_++});
    return Grant();
  }

  // Gives back the `weight` that `key`'s granted id held.
  std::vector<Id> Release(const Key& key, size_t weight) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      return {};
    }
    Entry& entry = it->second;
    const size_t w =
        std::min(std::clamp<size_t>(weight, 1, entry.limit), entry.in_use);
    entry.in_use -= w;
    total_ -= std::min(w, total_);
    return Grant();
  }

  // Leaves the queue (`*cancelled`: whether `id` was queued). A waiter that
  // held others back is gone, so others may be granted.
  std::vector<Id> Cancel(const Key& key, Id id, bool* cancelled = nullptr) {
    bool found_id = false;
    auto it = entries_.find(key);
    if (it != entries_.end()) {
      std::vector<Waiter>& waiting = it->second.waiting;
      auto found = std::find_if(waiting.begin(), waiting.end(),
                                [&](const Waiter& w) { return w.id == id; });
      if (found != waiting.end()) {
        waiting.erase(found);
        found_id = true;
      }
    }
    if (cancelled) {
      *cancelled = found_id;
    }
    return found_id ? Grant() : std::vector<Id>();
  }

  std::vector<Id> SetPriority(const Key& key, Id id, int priority) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      return {};
    }
    for (Waiter& waiter : it->second.waiting) {
      if (waiter.id == id) {
        waiter.priority = priority;
      }
    }
    return Grant();
  }

  // The weight `key` holds, and how many of its requests wait.
  size_t InUse(const Key& key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? 0 : it->second.in_use;
  }
  size_t Waiting(const Key& key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? 0 : it->second.waiting.size();
  }
  size_t Total() const { return total_; }
  size_t KeyCount() const { return entries_.size(); }
  size_t budget() const { return budget_; }

 private:
  struct Waiter {
    Id id;
    size_t weight;
    int priority;
    uint64_t seq;
  };
  struct Entry {
    size_t limit = 1;
    size_t in_use = 0;
    std::vector<Waiter> waiting;
  };

  static typename std::vector<Waiter>::iterator Best(
      std::vector<Waiter>& waiting) {
    return std::min_element(waiting.begin(), waiting.end(),
                            [](const Waiter& a, const Waiter& b) {
                              return a.priority != b.priority
                                         ? a.priority > b.priority
                                         : a.seq < b.seq;
                            });
  }

  static uint64_t OldestSeq(const std::vector<Waiter>& waiting) {
    uint64_t oldest = waiting.front().seq;
    for (const Waiter& w : waiting) {
      oldest = std::min(oldest, w.seq);
    }
    return oldest;
  }

  // Grants what can be granted: the keys that wait, the one holding the
  // fewest first (then the one waiting longest). A key at its own limit
  // doesn't hold the others back; one that the budget can't serve does.
  std::vector<Id> Grant() {
    std::vector<Id> granted;
    while (true) {
      std::vector<std::pair<std::pair<size_t, uint64_t>, Entry*>> order;
      for (auto& [key, entry] : entries_) {
        if (!entry.waiting.empty()) {
          order.push_back({{entry.in_use, OldestSeq(entry.waiting)}, &entry});
        }
      }
      std::sort(order.begin(), order.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      bool progressed = false;
      bool blocked = false;
      for (const auto& item : order) {
        Entry& entry = *item.second;
        auto best = Best(entry.waiting);
        if (entry.in_use + best->weight > entry.limit) {
          continue;
        }
        if (total_ + best->weight > budget_) {
          blocked = true;
          break;
        }
        entry.in_use += best->weight;
        total_ += best->weight;
        granted.push_back(best->id);
        entry.waiting.erase(best);
        progressed = true;
        break;
      }
      if (!progressed || blocked) {
        break;
      }
    }
    std::erase_if(entries_, [](const auto& item) {
      return item.second.in_use == 0 && item.second.waiting.empty();
    });
    return granted;
  }

  const size_t budget_;
  size_t total_ = 0;
  std::map<Key, Entry> entries_;
  uint64_t next_seq_ = 0;
};

}  // namespace net

#endif  // NET_SOCKET_MOLT_PER_SITE_SOCKET_POOL_POLICY_H_
