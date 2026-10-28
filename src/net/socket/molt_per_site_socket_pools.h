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
// socket: the certificate-fetch pools a budget of their own, so that they
// never wait behind the connections whose certificate checks wait for
// them. The pools start no backup connect jobs: those are for a lost
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
#include "base/unguessable_token.h"
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
  // of that and `max_sockets_per_group` per group. `budget` and
  // `cert_fetch_budget` (may be null: those pools aren't held back) outlive
  // this: the certificate-fetch pools draw on `cert_fetch_budget`, the
  // others on `budget`; `budget_owner` identifies the session (the same
  // for its normal and WebSocket pools).
  MoltPerSiteSocketPools(
      size_t per_site_pool_size,
      size_t max_sockets_per_group,
      base::TimeDelta unused_idle_socket_timeout,
      bool is_for_websockets,
      const CommonConnectJobParams* common_connect_job_params,
      bool cleanup_on_ip_address_change,
      MoltConnectionBudget* budget = nullptr,
      uintptr_t budget_owner = 0,
      MoltConnectionBudget* cert_fetch_budget = nullptr);

  MoltPerSiteSocketPools(const MoltPerSiteSocketPools&) = delete;
  MoltPerSiteSocketPools& operator=(const MoltPerSiteSocketPools&) = delete;

  ~MoltPerSiteSocketPools() override;

  // The pool for requests of network key `network_anonymization_key`
  // through `proxy_chain`: its site's (MoltBudgetSiteFor()), or, for a
  // request that may not start certificate downloads (`no_cert_fetches`:
  // the certificate fetcher's own), that site's certificate-fetch pool
  // (MoltUsesCertFetchPool()); made on first use. Drops the site pools
  // nothing has used for a while first, when it makes one.
  ClientSocketPool* Get(const ProxyChain& proxy_chain,
                        const NetworkAnonymizationKey& network_anonymization_key,
                        bool no_cert_fetches = false) {
    const base::TimeTicks now = base::TimeTicks::Now();
    MoltBudgetSite site = MoltBudgetSiteFor(network_anonymization_key);
    const bool cert_fetch_pool =
        MoltUsesCertFetchPool(is_for_websockets_, no_cert_fetches);
    Key key{proxy_chain, std::move(site), cert_fetch_pool};
    auto it = pools_.find(key);
    if (it == pools_.end()) {
      DropUnusedPools(now);
      const size_t limit =
          MoltPerSitePoolLimit(per_site_pool_size_, is_for_websockets_,
                               !key.site.has_value(), key.cert_fetch_pool);
      Entry entry;
      entry.pool = std::make_unique<TransportClientSocketPool>(
          limit, MoltPerSiteGroupLimit(limit, max_sockets_per_group_),
          unused_idle_socket_timeout_, proxy_chain, is_for_websockets_,
          common_connect_job_params_, cleanup_on_ip_address_change_);
      entry.pool->MoltSetSocketGate(this);
      entry.pool->MoltDisableBackupConnectJobs();
      entry.pool->MoltSetIgnoreLimitsOverflow(MoltIgnoreLimitsOverflowFor(
          is_for_websockets_, !key.site.has_value(), key.cert_fetch_pool));
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
    if (cert_fetch_budget_) {
      cert_fetch_budget_->OnReleased();
    }
  }

  void CloseIdleSockets(const char* net_log_reason_utf8) {
    for (auto& [key, entry] : pools_) {
      entry.pool->CloseIdleSockets(net_log_reason_utf8);
    }
  }

  // The idle sockets of the groups whose network key carries `nonce` (a gone
  // page's credentialless frames': services/network/molt_nonce_drop_policy.h),
  // in every site's pools (they are in the page's site's).
  void MoltCloseIdleSocketsOfNonce(const base::UnguessableToken& nonce,
                                   const char* net_log_reason_utf8) {
    for (auto& [key, entry] : pools_) {
      entry.pool->MoltCloseIdleSocketsOfNonce(nonce, net_log_reason_utf8);
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
  // The member the certificate-fetch pools are of their budget.
  MoltConnectionBudget::Member& CertFetchMemberForTesting() {
    return cert_fetch_member_;
  }

  // ClientSocketPool::MoltSocketGate:
  bool MoltMayAllocate(const ClientSocketPool* pool) override;
  void MoltOnReleased(const ClientSocketPool* pool) override;

  // MoltConnectionBudget::Member, for `budget`: every pool but the
  // certificate-fetch pools.
  void MoltAddLoads(std::map<MoltBudgetSite, size_t>& loads,
                    size_t& within_limits) const override;
  bool MoltHasStalledRequest(const MoltBudgetSite& site) const override;
  void MoltRetryStalledRequests(const MoltBudgetSite& site) override;
  bool MoltCloseOneIdleSocket(const MoltBudgetSite& site) override;
  bool MoltCloseIdleSessions(const MoltBudgetSite& site) override;

 private:
  // The certificate-fetch pools as a member of `cert_fetch_budget`.
  class CertFetchMember : public MoltConnectionBudget::Member {
   public:
    explicit CertFetchMember(MoltPerSiteSocketPools* pools) : pools_(pools) {}
    void MoltAddLoads(std::map<MoltBudgetSite, size_t>& loads,
                      size_t& within_limits) const override;
    bool MoltHasStalledRequest(const MoltBudgetSite& site) const override;
    void MoltRetryStalledRequests(const MoltBudgetSite& site) override;
    bool MoltCloseOneIdleSocket(const MoltBudgetSite& site) override;
    bool MoltCloseIdleSessions(const MoltBudgetSite& site) override;

   private:
    const raw_ptr<MoltPerSiteSocketPools> pools_;
  };

  // The budget members' work, for the certificate-fetch pools or for the
  // others.
  void AddLoads(bool cert_fetch_pools,
                std::map<MoltBudgetSite, size_t>& loads,
                size_t& within_limits) const {
    for (const auto& [key, entry] : pools_) {
      if (key.cert_fetch_pool != cert_fetch_pools) {
        continue;
      }
      const size_t in_use = entry.pool->SocketsInUse();
      loads[key.site] += in_use;
      // Sockets past the pool's limit (requests that ignore the limits)
      // count against the site, but come out of Tor's margin, not the
      // budget other sites share (kMoltIgnoreLimitsOverflow).
      within_limits += std::min(in_use, entry.limit);
    }
  }
  bool HasStalledRequest(bool cert_fetch_pools,
                         const MoltBudgetSite& site) const {
    for (TransportClientSocketPool* pool : PoolsOf(site, cert_fetch_pools)) {
      if (pool->MoltHasStalledRequest()) {
        return true;
      }
    }
    return false;
  }
  void RetryStalledRequests(bool cert_fetch_pools, const MoltBudgetSite& site) {
    for (TransportClientSocketPool* pool : PoolsOf(site, cert_fetch_pools)) {
      pool->MoltRetryStalledRequests();
    }
  }
  bool CloseOneIdleSocket(bool cert_fetch_pools, const MoltBudgetSite& site) {
    for (TransportClientSocketPool* pool : PoolsOf(site, cert_fetch_pools)) {
      if (pool->MoltCloseOneIdleSocket()) {
        return true;
      }
    }
    return false;
  }
  bool CloseIdleSessions(bool cert_fetch_pools, const MoltBudgetSite& site) {
    bool any = false;
    for (TransportClientSocketPool* pool : PoolsOf(site, cert_fetch_pools)) {
      any |= pool->MoltCloseIdleSessions();
    }
    return any;
  }

  // A pool: the proxy chain, the site it counts against, and whether it is
  // that site's certificate-fetch pool (MoltUsesCertFetchPool()).
  struct Key {
    ProxyChain proxy_chain;
    MoltBudgetSite site;
    bool cert_fetch_pool = false;
    friend bool operator==(const Key&, const Key&) = default;
    friend auto operator<=>(const Key&, const Key&) = default;
  };
  struct Entry {
    std::unique_ptr<TransportClientSocketPool> pool;
    // The pool's limit (MoltPerSitePoolLimit()).
    size_t limit = 0;
    base::TimeTicks last_handed_out;
  };

  // `site`'s certificate-fetch pools, or its other pools (per proxy chain),
  // collected first: what a pool does when asked doesn't change the map,
  // but this doesn't rely on it.
  std::vector<TransportClientSocketPool*> PoolsOf(const MoltBudgetSite& site,
                                                  bool cert_fetch_pools) const {
    std::vector<TransportClientSocketPool*> pools;
    for (const auto& [key, entry] : pools_) {
      if (key.site == site && key.cert_fetch_pool == cert_fetch_pools) {
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
  raw_ptr<MoltConnectionBudget> cert_fetch_budget_;
  CertFetchMember cert_fetch_member_{this};
  std::map<Key, Entry> pools_;
};

inline MoltPerSiteSocketPools::MoltPerSiteSocketPools(
    size_t per_site_pool_size,
    size_t max_sockets_per_group,
    base::TimeDelta unused_idle_socket_timeout,
    bool is_for_websockets,
    const CommonConnectJobParams* common_connect_job_params,
    bool cleanup_on_ip_address_change,
    MoltConnectionBudget* budget,
    uintptr_t budget_owner,
    MoltConnectionBudget* cert_fetch_budget)
    : per_site_pool_size_(per_site_pool_size),
      max_sockets_per_group_(max_sockets_per_group),
      unused_idle_socket_timeout_(unused_idle_socket_timeout),
      is_for_websockets_(is_for_websockets),
      common_connect_job_params_(common_connect_job_params),
      cleanup_on_ip_address_change_(cleanup_on_ip_address_change),
      budget_(budget),
      cert_fetch_budget_(cert_fetch_budget) {
  if (budget_) {
    budget_->AddMember(this, budget_owner);
  }
  if (cert_fetch_budget_) {
    cert_fetch_budget_->AddMember(&cert_fetch_member_, budget_owner);
  }
}

inline MoltPerSiteSocketPools::~MoltPerSiteSocketPools() {
  if (budget_) {
    budget_->RemoveMember(this);
  }
  if (cert_fetch_budget_) {
    cert_fetch_budget_->RemoveMember(&cert_fetch_member_);
  }
  // What the pools release while they go no longer concerns the budgets.
  budget_ = nullptr;
  cert_fetch_budget_ = nullptr;
  pools_.clear();
}

inline bool MoltPerSiteSocketPools::MoltMayAllocate(
    const ClientSocketPool* pool) {
  for (const auto& [key, entry] : pools_) {
    if (entry.pool.get() == pool) {
      if (key.cert_fetch_pool) {
        return !cert_fetch_budget_ ||
               cert_fetch_budget_->MayAllocate(&cert_fetch_member_, key.site);
      }
      return !budget_ || budget_->MayAllocate(this, key.site);
    }
  }
  return true;
}

inline void MoltPerSiteSocketPools::MoltOnReleased(
    const ClientSocketPool* pool) {
  for (const auto& [key, entry] : pools_) {
    if (entry.pool.get() == pool) {
      if (MoltConnectionBudget* budget =
              key.cert_fetch_pool ? cert_fetch_budget_ : budget_) {
        budget->OnReleased();
      }
      return;
    }
  }
}

inline void MoltPerSiteSocketPools::MoltAddLoads(
    std::map<MoltBudgetSite, size_t>& loads,
    size_t& within_limits) const {
  AddLoads(/*cert_fetch_pools=*/false, loads, within_limits);
}

inline bool MoltPerSiteSocketPools::MoltHasStalledRequest(
    const MoltBudgetSite& site) const {
  return HasStalledRequest(/*cert_fetch_pools=*/false, site);
}

inline void MoltPerSiteSocketPools::MoltRetryStalledRequests(
    const MoltBudgetSite& site) {
  RetryStalledRequests(/*cert_fetch_pools=*/false, site);
}

inline bool MoltPerSiteSocketPools::MoltCloseOneIdleSocket(
    const MoltBudgetSite& site) {
  return CloseOneIdleSocket(/*cert_fetch_pools=*/false, site);
}

inline bool MoltPerSiteSocketPools::MoltCloseIdleSessions(
    const MoltBudgetSite& site) {
  return CloseIdleSessions(/*cert_fetch_pools=*/false, site);
}

inline void MoltPerSiteSocketPools::CertFetchMember::MoltAddLoads(
    std::map<MoltBudgetSite, size_t>& loads,
    size_t& within_limits) const {
  pools_->AddLoads(/*cert_fetch_pools=*/true, loads, within_limits);
}

inline bool MoltPerSiteSocketPools::CertFetchMember::MoltHasStalledRequest(
    const MoltBudgetSite& site) const {
  return pools_->HasStalledRequest(/*cert_fetch_pools=*/true, site);
}

inline void MoltPerSiteSocketPools::CertFetchMember::MoltRetryStalledRequests(
    const MoltBudgetSite& site) {
  pools_->RetryStalledRequests(/*cert_fetch_pools=*/true, site);
}

inline bool MoltPerSiteSocketPools::CertFetchMember::MoltCloseOneIdleSocket(
    const MoltBudgetSite& site) {
  return pools_->CloseOneIdleSocket(/*cert_fetch_pools=*/true, site);
}

inline bool MoltPerSiteSocketPools::CertFetchMember::MoltCloseIdleSessions(
    const MoltBudgetSite& site) {
  return pools_->CloseIdleSessions(/*cert_fetch_pools=*/true, site);
}

}  // namespace net

#endif  // NET_SOCKET_MOLT_PER_SITE_SOCKET_POOLS_H_
