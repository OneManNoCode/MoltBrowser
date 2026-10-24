// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the per-site socket pools of MoltNet's network context
// (molt_per_site_socket_pool_policy.h says why). Owned by a
// ClientSocketPoolManagerImpl whose session has
// HttpNetworkSessionParams::molt_per_site_socket_pool_size set; one per
// pool type. Header-only: //net's sources are Chromium's BUILD.gn, which
// MoltBrowser doesn't change.

#ifndef NET_SOCKET_MOLT_PER_SITE_SOCKET_POOLS_H_
#define NET_SOCKET_MOLT_PER_SITE_SOCKET_POOLS_H_

#include <map>
#include <memory>
#include <utility>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/proxy_chain.h"
#include "net/socket/connect_job.h"
#include "net/socket/molt_per_site_socket_pool_policy.h"
#include "net/socket/transport_client_socket_pool.h"

namespace net {

class MoltPerSiteSocketPools {
 public:
  // The arguments a ClientSocketPoolManagerImpl gives the pool of a proxy
  // chain, except the limits: `per_site_pool_size` for each site's pool,
  // and MoltPerSiteGroupLimit() of it and `max_sockets_per_group` per group.
  MoltPerSiteSocketPools(size_t per_site_pool_size,
                         size_t max_sockets_per_group,
                         base::TimeDelta unused_idle_socket_timeout,
                         bool is_for_websockets,
                         const CommonConnectJobParams* common_connect_job_params,
                         bool cleanup_on_ip_address_change)
      : per_site_pool_size_(per_site_pool_size),
        per_group_limit_(
            MoltPerSiteGroupLimit(per_site_pool_size, max_sockets_per_group)),
        unused_idle_socket_timeout_(unused_idle_socket_timeout),
        is_for_websockets_(is_for_websockets),
        common_connect_job_params_(common_connect_job_params),
        cleanup_on_ip_address_change_(cleanup_on_ip_address_change) {}

  MoltPerSiteSocketPools(const MoltPerSiteSocketPools&) = delete;
  MoltPerSiteSocketPools& operator=(const MoltPerSiteSocketPools&) = delete;

  // The pool for requests of the site `network_anonymization_key` through
  // `proxy_chain`; made on first use. Drops the site pools nothing has used
  // for a while first, when it makes one.
  ClientSocketPool* Get(const ProxyChain& proxy_chain,
                        const NetworkAnonymizationKey& network_anonymization_key) {
    const base::TimeTicks now = base::TimeTicks::Now();
    Key key(proxy_chain, network_anonymization_key);
    auto it = pools_.find(key);
    if (it == pools_.end()) {
      DropUnusedPools(now);
      Entry entry;
      entry.pool = std::make_unique<TransportClientSocketPool>(
          per_site_pool_size_, per_group_limit_, unused_idle_socket_timeout_,
          proxy_chain, is_for_websockets_, common_connect_job_params_,
          cleanup_on_ip_address_change_);
      it = pools_.emplace(std::move(key), std::move(entry)).first;
    }
    it->second.last_handed_out = now;
    return it->second.pool.get();
  }

  void FlushWithError(int net_error, const char* net_log_reason_utf8) {
    for (auto& [key, entry] : pools_) {
      entry.pool->FlushWithError(net_error, net_log_reason_utf8);
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
          key.first.ToDebugString() + " (per site)",
          key.first.Last().is_socks() ? "socks_socket_pool"
                                      : "http_proxy_socket_pool"));
    }
  }

  size_t PoolCountForTesting() const { return pools_.size(); }

 private:
  using Key = std::pair<ProxyChain, NetworkAnonymizationKey>;
  struct Entry {
    std::unique_ptr<TransportClientSocketPool> pool;
    base::TimeTicks last_handed_out;
  };

  void DropUnusedPools(base::TimeTicks now) {
    std::erase_if(pools_, [now](const auto& item) {
      const Entry& entry = item.second;
      return MoltMayDropSitePool(
          entry.pool->MoltIsUnused(),
          (now - entry.last_handed_out).InSeconds());
    });
  }

  const size_t per_site_pool_size_;
  const size_t per_group_limit_;
  const base::TimeDelta unused_idle_socket_timeout_;
  const bool is_for_websockets_;
  const raw_ptr<const CommonConnectJobParams> common_connect_job_params_;
  const bool cleanup_on_ip_address_change_;
  std::map<Key, Entry> pools_;
};

}  // namespace net

#endif  // NET_SOCKET_MOLT_PER_SITE_SOCKET_POOLS_H_
