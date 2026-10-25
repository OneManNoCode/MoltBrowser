// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the per-site socket pools of MoltNet's network context
// (molt_per_site_socket_pool_policy.h says why). Owned by a
// ClientSocketPoolManagerImpl whose session has
// HttpNetworkSessionParams::molt_per_site_socket_pool_size set; one per
// pool type. Each pool's limits are fixed, and with the process's
// MoltConnectionBudget (molt_connection_budget.h: one for every session
// that goes to MoltNet's Tor) every pool asks the budget before it opens a
// socket. The pools start no backup connect jobs: those are for a lost
// connection attempt to the destination, and here every connection goes
// to Tor on this machine; a second attempt would only be a second stream
// for Tor to set up, outside the budget. Header-only: //net's sources are
// Chromium's BUILD.gn, which MoltBrowser doesn't change.

#ifndef NET_SOCKET_MOLT_PER_SITE_SOCKET_POOLS_H_
#define NET_SOCKET_MOLT_PER_SITE_SOCKET_POOLS_H_

#include <algorithm>
#include <compare>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/proxy_chain.h"
#include "net/socket/client_socket_pool.h"
#include "net/socket/connect_job.h"
#include "net/socket/molt_connection_budget.h"
#include "net/socket/molt_per_site_socket_pool_policy.h"
#include "net/socket/transport_client_socket_pool.h"

namespace net {

class MoltPerSiteSocketPools : public ClientSocketPool::MoltSocketGate,
                               public MoltConnectionBudget::Member {
 public:
  // The arguments a ClientSocketPoolManagerImpl gives the pool of a proxy
  // chain, except the limits: MoltPerSitePoolLimit() of
  // `per_site_pool_size` for each site's pool, and MoltPerSiteGroupLimit()
  // of that and `max_sockets_per_group` per group. `budget` (may be null)
  // outlives this; `budget_owner` identifies the session (the same for its
  // normal and WebSocket pools).
  MoltPerSiteSocketPools(size_t per_site_pool_size,
                         size_t max_sockets_per_group,
                         base::TimeDelta unused_idle_socket_timeout,
                         bool is_for_websockets,
                         const CommonConnectJobParams* common_connect_job_params,
                         bool cleanup_on_ip_address_change,
                         MoltConnectionBudget* budget = nullptr,
                         uintptr_t budget_owner = 0)
      : per_site_pool_size_(per_site_pool_size),
        max_sockets_per_group_(max_sockets_per_group),
        unused_idle_socket_timeout_(unused_idle_socket_timeout),
        is_for_websockets_(is_for_websockets),
        common_connect_job_params_(common_connect_job_params),
        cleanup_on_ip_address_change_(cleanup_on_ip_address_change),
        budget_(budget) {
    if (budget_) {
      budget_->AddMember(this, budget_owner);
    }
  }

  MoltPerSiteSocketPools(const MoltPerSiteSocketPools&) = delete;
  MoltPerSiteSocketPools& operator=(const MoltPerSiteSocketPools&) = delete;

  ~MoltPerSiteSocketPools() override {
    if (budget_) {
      budget_->RemoveMember(this);
    }
    // What the pools release while they go no longer concerns the budget.
    budget_ = nullptr;
    pools_.clear();
  }

  // The pool for requests of network key `network_anonymization_key`
  // through `proxy_chain`: its site's (MoltBudgetSiteFor()), or for a key
  // with a nonce the site's pool for one-off keys (MoltUsesSiteOneOffPool());
  // made on first use. Drops the site pools nothing has used for a while
  // first, when it makes one.
  ClientSocketPool* Get(const ProxyChain& proxy_chain,
                        const NetworkAnonymizationKey& network_anonymization_key) {
    const base::TimeTicks now = base::TimeTicks::Now();
    MoltBudgetSite site = MoltBudgetSiteFor(network_anonymization_key);
    const bool one_off = MoltUsesSiteOneOffPool(
        is_for_websockets_, site.has_value(),
        network_anonymization_key.GetNonce().has_value());
    Key key{proxy_chain, std::move(site), one_off};
    auto it = pools_.find(key);
    if (it == pools_.end()) {
      DropUnusedPools(now);
      const size_t limit =
          MoltPerSitePoolLimit(per_site_pool_size_, is_for_websockets_,
                               !key.site.has_value(), key.one_off);
      Entry entry;
      entry.pool = std::make_unique<TransportClientSocketPool>(
          limit, MoltPerSiteGroupLimit(limit, max_sockets_per_group_),
          unused_idle_socket_timeout_, proxy_chain, is_for_websockets_,
          common_connect_job_params_, cleanup_on_ip_address_change_);
      entry.pool->MoltSetSocketGate(this);
      entry.pool->MoltDisableBackupConnectJobs();
      entry.pool->MoltSetIgnoreLimitsOverflow(MoltIgnoreLimitsOverflowFor(
          is_for_websockets_, !key.site.has_value(), key.one_off));
      entry.limit = limit;
      it = pools_.emplace(std::move(key), std::move(entry)).first;
    }
    it->second.last_handed_out = now;
    return it->second.pool.get();
  }

  void FlushWithError(int net_error, const char* net_log_reason_utf8) {
    for (auto& [key, entry] : pools_) {
      entry.pool->FlushWithError(net_error, net_log_reason_utf8);
    }
    // Cancelled connect jobs leave room the pools don't report.
    if (budget_) {
      budget_->OnReleased();
    }
  }

  void CloseIdleSockets(const char* net_log_reason_utf8) {
    for (auto& [key, entry] : pools_) {
      entry.pool->CloseIdleSockets(net_log_reason_utf8);
    }
  }

  // Appends each site pool's summary (net-internals, net-export). The site
  // itself isn't named: a pool shows as its proxy chain, like Chromium's.
  void AppendInfo(base::ListValue& list) const {
    for (const auto& [key, entry] : pools_) {
      // As ClientSocketPoolManagerImpl::SocketPoolInfoToValue() names a
      // proxy chain's pool (never direct here).
      list.Append(entry.pool->GetInfoAsValue(
          key.proxy_chain.ToDebugString() + " (per site)",
          key.proxy_chain.Last().is_socks() ? "socks_socket_pool"
                                            : "http_proxy_socket_pool"));
    }
  }

  size_t PoolCountForTesting() const { return pools_.size(); }

  // ClientSocketPool::MoltSocketGate:
  bool MoltMayAllocate(const ClientSocketPool* pool) override {
    if (!budget_) {
      return true;
    }
    for (const auto& [key, entry] : pools_) {
      if (entry.pool.get() == pool) {
        return budget_->MayAllocate(this, key.site);
      }
    }
    return true;
  }
  void MoltOnReleased(const ClientSocketPool* pool) override {
    if (budget_) {
      budget_->OnReleased();
    }
  }

  // MoltConnectionBudget::Member:
  void MoltAddLoads(std::map<MoltBudgetSite, size_t>& loads,
                    size_t& within_limits) const override {
    for (const auto& [key, entry] : pools_) {
      const size_t in_use = entry.pool->SocketsInUse();
      loads[key.site] += in_use;
      // Sockets past the pool's limit (requests that ignore the limits)
      // count against the site, but come out of Tor's margin, not the
      // budget other sites share (kMoltIgnoreLimitsOverflow).
      within_limits += std::min(in_use, entry.limit);
    }
  }
  bool MoltHasStalledRequest(const MoltBudgetSite& site) const override {
    for (const auto& [key, entry] : pools_) {
      if (key.site == site && entry.pool->MoltHasStalledRequest()) {
        return true;
      }
    }
    return false;
  }
  void MoltRetryStalledRequests(const MoltBudgetSite& site) override {
    for (TransportClientSocketPool* pool : PoolsOf(site)) {
      pool->MoltRetryStalledRequests();
    }
  }
  bool MoltCloseOneIdleSocket(const MoltBudgetSite& site) override {
    for (TransportClientSocketPool* pool : PoolsOf(site)) {
      if (pool->MoltCloseOneIdleSocket()) {
        return true;
      }
    }
    return false;
  }
  bool MoltCloseIdleSessions(const MoltBudgetSite& site) override {
    bool any = false;
    for (TransportClientSocketPool* pool : PoolsOf(site)) {
      any |= pool->MoltCloseIdleSessions();
    }
    return any;
  }

 private:
  // A pool: the proxy chain, the site it counts against, and whether it is
  // that site's pool for one-off keys (MoltUsesSiteOneOffPool()).
  struct Key {
    ProxyChain proxy_chain;
    MoltBudgetSite site;
    bool one_off = false;
    friend bool operator==(const Key&, const Key&) = default;
    friend auto operator<=>(const Key&, const Key&) = default;
  };
  struct Entry {
    std::unique_ptr<TransportClientSocketPool> pool;
    // The pool's limit (MoltPerSitePoolLimit()).
    size_t limit = 0;
    base::TimeTicks last_handed_out;
  };

  // `site`'s pools (per proxy chain, and its one-off keys'), collected
  // first: what a pool does when asked doesn't change the map, but this
  // doesn't rely on it.
  std::vector<TransportClientSocketPool*> PoolsOf(
      const MoltBudgetSite& site) const {
    std::vector<TransportClientSocketPool*> pools;
    for (const auto& [key, entry] : pools_) {
      if (key.site == site) {
        pools.push_back(entry.pool.get());
      }
    }
    return pools;
  }

  void DropUnusedPools(base::TimeTicks now) {
    std::erase_if(pools_, [now](const auto& item) {
      const Entry& entry = item.second;
      return MoltMayDropSitePool(
          entry.pool->MoltIsUnused(),
          (now - entry.last_handed_out).InSeconds());
    });
  }

  const size_t per_site_pool_size_;
  const size_t max_sockets_per_group_;
  const base::TimeDelta unused_idle_socket_timeout_;
  const bool is_for_websockets_;
  const raw_ptr<const CommonConnectJobParams> common_connect_job_params_;
  const bool cleanup_on_ip_address_change_;
  raw_ptr<MoltConnectionBudget> budget_;
  std::map<Key, Entry> pools_;
};

}  // namespace net

#endif  // NET_SOCKET_MOLT_PER_SITE_SOCKET_POOLS_H_
