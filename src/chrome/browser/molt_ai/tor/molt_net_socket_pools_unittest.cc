// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's per-site socket pools (net/socket/molt_per_site_socket_pool_
// policy.h) on Chromium's real HttpNetworkSession and socket pools.

#include <memory>
#include <string>

#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/tor/molt_net_network_context_policy.h"
#include "net/base/host_port_pair.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/proxy_chain.h"
#include "net/base/proxy_server.h"
#include "net/base/schemeful_site.h"
#include "net/http/http_network_session.h"
#include "net/socket/client_socket_pool.h"
#include "net/socket/connect_job.h"
#include "net/socket/molt_per_site_socket_pool_policy.h"
#include "net/socket/molt_per_site_socket_pools.h"
#include "net/spdy/spdy_test_util_common.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace molt_ai::tor {
namespace {

using SocketPoolType = net::HttpNetworkSession::SocketPoolType;

net::NetworkAnonymizationKey SiteKey(const char* url) {
  return net::NetworkAnonymizationKey::CreateSameSite(
      net::SchemefulSite(GURL(url)));
}

const net::ProxyChain& Tor() {
  static const net::ProxyChain tor(net::ProxyServer::SCHEME_SOCKS5,
                                   net::HostPortPair("127.0.0.1", 9150));
  return tor;
}

class MoltNetSocketPoolsTest : public testing::Test {
 protected:
  std::unique_ptr<net::HttpNetworkSession> MakeSession(bool moltnet) {
    net::HttpNetworkSessionParams params =
        net::SpdySessionDependencies::CreateSessionParams(&deps_);
    // What ApplyMoltNetNetworkContextSettings() asks the network service
    // for, through network::mojom::NetworkContextParams.
    params.molt_per_site_socket_pool_size =
        MoltNetNetworkContextSettingsFor(moltnet).per_site_socket_pool_size;
    return std::make_unique<net::HttpNetworkSession>(
        params, net::SpdySessionDependencies::CreateSessionContext(&deps_));
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO,
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  net::SpdySessionDependencies deps_;
};

TEST_F(MoltNetSocketPoolsTest, EachSiteHasItsOwnPool) {
  std::unique_ptr<net::HttpNetworkSession> session = MakeSession(true);
  for (SocketPoolType type :
       {SocketPoolType::kNormal, SocketPoolType::kWebSocket}) {
    net::ClientSocketPool* a =
        session->GetSocketPoolForSite(type, Tor(), SiteKey("https://a.test"));
    net::ClientSocketPool* b =
        session->GetSocketPoolForSite(type, Tor(), SiteKey("https://b.test"));
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_NE(a, b);
    // The same site gets the same pool, also from one of its subdomains.
    EXPECT_EQ(a, session->GetSocketPoolForSite(
                     type, Tor(), SiteKey("https://www.a.test")));
    // Not the pool all sites share elsewhere.
    EXPECT_NE(a, session->GetSocketPool(type, Tor()));
    // One-off keys (certificate fetches, opaque origins) get their own.
    EXPECT_NE(session->GetSocketPoolForSite(
                  type, Tor(), net::NetworkAnonymizationKey::CreateTransient()),
              session->GetSocketPoolForSite(
                  type, Tor(), net::NetworkAnonymizationKey::CreateTransient()));
  }
}

TEST_F(MoltNetSocketPoolsTest, EachSitePoolHasTheFixedLimits) {
  std::unique_ptr<net::HttpNetworkSession> session = MakeSession(true);
  const base::Value info =
      session
          ->GetSocketPoolForSite(SocketPoolType::kNormal, Tor(),
                                 SiteKey("https://a.test"))
          ->GetInfoAsValue("pool", "socks_socket_pool");
  ASSERT_TRUE(info.is_dict());
  EXPECT_EQ(info.GetDict().FindInt("socket_soft_cap"),
            static_cast<int>(net::kMoltPerSitePoolSize));
  EXPECT_EQ(info.GetDict().FindInt("max_sockets_per_group"), 6);
  // net-internals lists each site's pool, without naming the site.
  const base::Value all = session->SocketPoolInfoToValue();
  ASSERT_TRUE(all.is_list());
  EXPECT_FALSE(all.GetList().empty());
}

TEST_F(MoltNetSocketPoolsTest, DirectConnectionsAndOtherContextsShare) {
  std::unique_ptr<net::HttpNetworkSession> moltnet = MakeSession(true);
  EXPECT_EQ(moltnet->GetSocketPoolForSite(SocketPoolType::kNormal,
                                          net::ProxyChain::Direct(),
                                          SiteKey("https://a.test")),
            moltnet->GetSocketPool(SocketPoolType::kNormal,
                                   net::ProxyChain::Direct()));
  std::unique_ptr<net::HttpNetworkSession> regular = MakeSession(false);
  for (SocketPoolType type :
       {SocketPoolType::kNormal, SocketPoolType::kWebSocket}) {
    net::ClientSocketPool* shared = regular->GetSocketPool(type, Tor());
    EXPECT_EQ(regular->GetSocketPoolForSite(type, Tor(),
                                            SiteKey("https://a.test")),
              shared);
    EXPECT_EQ(regular->GetSocketPoolForSite(type, Tor(),
                                            SiteKey("https://b.test")),
              shared);
  }
}

// A site's pool that nothing has used for a while is dropped when another
// site's pool is made; a recent one isn't.
TEST_F(MoltNetSocketPoolsTest, UnusedSitePoolsAreDropped) {
  std::unique_ptr<net::HttpNetworkSession> session = MakeSession(true);
  const net::CommonConnectJobParams params =
      session->CreateCommonConnectJobParams(/*for_websockets=*/false);
  net::MoltPerSiteSocketPools pools(net::kMoltPerSitePoolSize, 6,
                                    base::Seconds(60),
                                    /*is_for_websockets=*/false, &params,
                                    /*cleanup_on_ip_address_change=*/true);
  pools.Get(Tor(), SiteKey("https://a.test"));
  task_environment_.FastForwardBy(base::Seconds(30));
  pools.Get(Tor(), SiteKey("https://b.test"));
  EXPECT_EQ(pools.PoolCountForTesting(), 2u);
  task_environment_.FastForwardBy(base::Seconds(
      net::kMoltUnusedSitePoolTimeoutSeconds));
  pools.Get(Tor(), SiteKey("https://c.test"));
  EXPECT_EQ(pools.PoolCountForTesting(), 1u);
  // Handing one out keeps it.
  pools.Get(Tor(), SiteKey("https://c.test"));
  task_environment_.FastForwardBy(base::Seconds(30));
  pools.Get(Tor(), SiteKey("https://d.test"));
  EXPECT_EQ(pools.PoolCountForTesting(), 2u);
}

}  // namespace
}  // namespace molt_ai::tor
