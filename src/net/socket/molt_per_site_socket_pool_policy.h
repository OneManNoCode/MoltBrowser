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
//    the opaque origin was derived from; the one-off keys no site can be
//    named for (certificate fetches) share one small pool of their own
//    (MoltBudgetSiteFor() in molt_connection_budget.h).
//
//  * All of the context's connections to Tor together stay within a fixed
//    budget (kMoltNetConnectionBudget), well inside what MoltNet's single
//    Tor process can hold, so that cap can't become a channel between sites
//    either: the budget fits kMoltSitesToFillBudget sites' full shares, so
//    a site below its own limits waits for the budget only while that many
//    other sites are at theirs. When it is reached, the next freed
//    connection goes to the waiting site that uses the fewest, and an idle
//    connection of a site using more than the waiting one is closed for it
//    (MoltPreemptionOrder()).
//
//  * Each site may have at most kMoltSocksHandshakesPerSite SOCKS handshakes
//    in progress at a time: a handshake is what makes Tor build a circuit
//    for a stream it can't put on an existing one, so this bounds the
//    pending circuits one site can cause (Tor's MaxClientCircuitsPending is
//    shared by every site; torrc_builder.h).
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
#include <vector>

namespace net {

// A site's own limits: connections for HTTP (with Chromium's 6 per group
// inside), and WebSockets.
inline constexpr size_t kMoltPerSitePoolSize = 48;
inline constexpr size_t kMoltPerSiteWebSocketPoolSize = 16;
// The pool (per pool type) of the one-off keys no site can be named for.
inline constexpr size_t kMoltUnattributedPoolSize = 8;

// How long a site's pool may sit unused before it is dropped.
inline constexpr int64_t kMoltUnusedSitePoolTimeoutSeconds = 60;

// The most connections MoltNet's network context has open or opening to Tor
// at a time, all sites and both pool types together.
inline constexpr size_t kMoltNetConnectionBudget = 768;

// What a single Tor client process can always hold: Tor refuses to start
// unless it may open ConnLimit file descriptors (MoltNet writes Tor's
// default, 1000, into its torrc), and keeps 32 of them for itself, so a Tor
// that runs accepts at least 968 connections, on every system. The budget
// leaves a fifth of that to Tor's own connections (guards, directory
// fetches, the control port, files).
inline constexpr size_t kMoltTorConnLimit = 1000;
inline constexpr size_t kMoltTorKeptFileDescriptors = 32;
inline constexpr size_t kMoltTorOwnConnections = 200;
static_assert(kMoltNetConnectionBudget + kMoltTorOwnConnections <=
              kMoltTorConnLimit - kMoltTorKeptFileDescriptors);

// SOCKS handshakes in progress at a time, per site and for the unattributed
// one-off keys together.
inline constexpr size_t kMoltSocksHandshakesPerSite = 16;
inline constexpr size_t kMoltUnattributedSocksHandshakes = 8;

// Whether requests through a proxy chain get a pool per site:
// `per_site_pool_size` is the network context's setting (0: Chromium's one
// pool per proxy chain). Direct connections never do (MoltNet makes none;
// its context only ever has Tor's proxy configuration, or one that refuses
// every request).
constexpr bool MoltUsesPerSitePool(size_t per_site_pool_size,
                                   bool proxy_chain_is_direct) {
  return per_site_pool_size > 0 && !proxy_chain_is_direct;
}

// The limit of one site's pool: `per_site_pool_size` (the context's
// setting) for HTTP, less for WebSockets and for the one-off keys no site
// can be named for (`unattributed`).
constexpr size_t MoltPerSitePoolLimit(size_t per_site_pool_size,
                                      bool for_websockets,
                                      bool unattributed) {
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

// The SOCKS handshakes a site may have in progress at a time.
constexpr size_t MoltSocksHandshakeLimit(bool unattributed) {
  return unattributed ? kMoltUnattributedSocksHandshakes
                      : kMoltSocksHandshakesPerSite;
}

// ---- The connection budget ----

// What one site may hold in MoltNet's context, and the unattributed pools.
inline constexpr size_t kMoltPerSiteConnections =
    kMoltPerSitePoolSize + kMoltPerSiteWebSocketPoolSize;
inline constexpr size_t kMoltUnattributedConnections =
    2 * kMoltUnattributedPoolSize;

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

// ---- Which site a key counts against ----

// The serialized origin an opaque origin was derived from, read from
// url::Origin::GetDebugString() of that opaque origin:
//   "null [internally: (<nonce>) derived from https://a.example]"
// nullopt for one derived from nothing ("... anonymous]") or any other text.
// Linear; only the strict shape is accepted.
constexpr std::optional<std::string_view> MoltOpaqueOriginPrecursor(
    std::string_view debug_string) {
  constexpr std::string_view kPrefix = "null [internally:";
  constexpr std::string_view kDerivedFrom = " derived from ";
  if (!debug_string.starts_with(kPrefix) || !debug_string.ends_with("]")) {
    return std::nullopt;
  }
  const size_t at = debug_string.find(kDerivedFrom, kPrefix.size());
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  const size_t start = at + kDerivedFrom.size();
  const std::string_view tuple =
      debug_string.substr(start, debug_string.size() - 1 - start);
  // "<scheme>://<host>[:<port>]": no blanks or controls, a scheme.
  const size_t scheme_end = tuple.find("://");
  if (scheme_end == std::string_view::npos || scheme_end == 0 ||
      scheme_end + 3 >= tuple.size()) {
    return std::nullopt;
  }
  for (char c : tuple) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (uc <= 0x20 || uc == 0x7F) {
      return std::nullopt;
    }
  }
  return tuple;
}

// ---- SOCKS handshake slots ----

// A fixed number of slots per key, given out in order of priority (higher
// first), then of arrival. Ids are the caller's and unique per key.
template <typename Key, typename Id>
class MoltSlotQueue {
 public:
  // Takes one of `key`'s `limit` slots for `id` (true), or queues `id` for
  // the next one freed (false).
  bool Acquire(const Key& key, size_t limit, Id id, int priority) {
    Entry& entry = entries_[key];
    entry.limit = limit;
    if (entry.in_use < entry.limit && entry.waiting.empty()) {
      ++entry.in_use;
      return true;
    }
    entry.waiting.push_back(Waiter{id, priority, next_seq_++});
    return false;
  }

  // Gives one of `key`'s slots back; returns the queued id that holds it now,
  // if any.
  std::optional<Id> Release(const Key& key) {
    auto it = entries_.find(key);
    if (it == entries_.end() || it->second.in_use == 0) {
      return std::nullopt;
    }
    Entry& entry = it->second;
    std::optional<Id> next;
    if (!entry.waiting.empty() && entry.in_use <= entry.limit) {
      auto best = std::min_element(
          entry.waiting.begin(), entry.waiting.end(),
          [](const Waiter& a, const Waiter& b) {
            return a.priority != b.priority ? a.priority > b.priority
                                            : a.seq < b.seq;
          });
      next = best->id;
      entry.waiting.erase(best);
    } else {
      --entry.in_use;
    }
    if (entry.in_use == 0 && entry.waiting.empty()) {
      entries_.erase(it);
    }
    return next;
  }

  // Leaves the queue (true if `id` was queued).
  bool Cancel(const Key& key, Id id) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      return false;
    }
    std::vector<Waiter>& waiting = it->second.waiting;
    auto found = std::find_if(waiting.begin(), waiting.end(),
                              [&](const Waiter& w) { return w.id == id; });
    if (found == waiting.end()) {
      return false;
    }
    waiting.erase(found);
    if (it->second.in_use == 0 && waiting.empty()) {
      entries_.erase(it);
    }
    return true;
  }

  void SetPriority(const Key& key, Id id, int priority) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      return;
    }
    for (Waiter& waiter : it->second.waiting) {
      if (waiter.id == id) {
        waiter.priority = priority;
      }
    }
  }

  size_t InUse(const Key& key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? 0 : it->second.in_use;
  }
  size_t Waiting(const Key& key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? 0 : it->second.waiting.size();
  }
  size_t KeyCount() const { return entries_.size(); }

 private:
  struct Waiter {
    Id id;
    int priority;
    uint64_t seq;
  };
  struct Entry {
    size_t limit = 0;
    size_t in_use = 0;
    std::vector<Waiter> waiting;
  };
  std::map<Key, Entry> entries_;
  uint64_t next_seq_ = 0;
};

}  // namespace net

#endif  // NET_SOCKET_MOLT_PER_SITE_SOCKET_POOL_POLICY_H_
