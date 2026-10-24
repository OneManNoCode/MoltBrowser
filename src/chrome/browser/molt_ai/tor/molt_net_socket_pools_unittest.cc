// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's per-site socket pools (net/socket/molt_per_site_socket_pool_
// policy.h) on Chromium's real HttpNetworkSession and socket pools.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback_helpers.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/tor/molt_net_network_context_policy.h"
#include "net/base/host_port_pair.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/proxy_chain.h"
#include "net/base/proxy_server.h"
#include "net/base/request_priority.h"
#include "net/base/schemeful_site.h"
#include "net/http/http_network_session.h"
#include "net/socket/client_socket_pool.h"
#include "net/socket/connect_job.h"
#include "net/socket/molt_connection_budget.h"
#include "net/socket/molt_per_site_socket_pool_policy.h"
#include "net/socket/molt_per_site_socket_pools.h"
#include "net/spdy/spdy_test_util_common.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

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
    // The site's third-party key counts against the site too.
    EXPECT_EQ(a, session->GetSocketPoolForSite(
                     type, Tor(),
                     net::NetworkAnonymizationKey::CreateCrossSite(
                         net::SchemefulSite(GURL("https://a.test")))));
    // One-off keys no site can be named for (certificate fetches) share one
    // small pool of their own.
    net::ClientSocketPool* one_off = session->GetSocketPoolForSite(
        type, Tor(), net::NetworkAnonymizationKey::CreateTransient());
    EXPECT_EQ(one_off,
              session->GetSocketPoolForSite(
                  type, Tor(), net::NetworkAnonymizationKey::CreateTransient()));
    EXPECT_NE(one_off, a);
    EXPECT_NE(one_off, b);
  }
}

// A key whose top-level site is opaque (a page its server sandboxed) counts
// against the site its origin was derived from.
TEST_F(MoltNetSocketPoolsTest, SandboxedPagesCountAgainstTheirSite) {
  std::unique_ptr<net::HttpNetworkSession> session = MakeSession(true);
  const url::Origin sandboxed =
      url::Origin::Create(GURL("https://www.a.test/x")).DeriveNewOpaqueOrigin();
  const net::NetworkAnonymizationKey key =
      net::NetworkAnonymizationKey::CreateSameSite(net::SchemefulSite(sandboxed));
  EXPECT_EQ(net::MoltBudgetSiteFor(key),
            net::MoltBudgetSite(net::SchemefulSite(GURL("https://a.test"))));
  EXPECT_EQ(session->GetSocketPoolForSite(SocketPoolType::kNormal, Tor(), key),
            session->GetSocketPoolForSite(SocketPoolType::kNormal, Tor(),
                                          SiteKey("https://a.test")));
  // One derived from nothing: no site.
  EXPECT_EQ(net::MoltBudgetSiteFor(net::NetworkAnonymizationKey::CreateSameSite(
                net::SchemefulSite(url::Origin()))),
            std::nullopt);
  EXPECT_EQ(net::MoltBudgetSiteFor(
                net::NetworkAnonymizationKey::CreateTransient()),
            std::nullopt);
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
  // No randomized room beyond it.
  const std::string* additional =
      info.GetDict().FindString("additional_capacity");
  ASSERT_TRUE(additional);
  EXPECT_NE(additional->find("capacity:0"), std::string::npos) << *additional;
  // WebSockets and the one-off keys: smaller fixed limits.
  const base::Value ws =
      session
          ->GetSocketPoolForSite(SocketPoolType::kWebSocket, Tor(),
                                 SiteKey("https://a.test"))
          ->GetInfoAsValue("pool", "socks_socket_pool");
  EXPECT_EQ(ws.GetDict().FindInt("socket_soft_cap"),
            static_cast<int>(net::kMoltPerSiteWebSocketPoolSize));
  const base::Value one_off =
      session
          ->GetSocketPoolForSite(
              SocketPoolType::kNormal, Tor(),
              net::NetworkAnonymizationKey::CreateTransient())
          ->GetInfoAsValue("pool", "socks_socket_pool");
  EXPECT_EQ(one_off.GetDict().FindInt("socket_soft_cap"),
            static_cast<int>(net::kMoltUnattributedPoolSize));
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

// MoltNet's sessions have a connection budget and SOCKS handshake limits;
// every other session has neither.
TEST_F(MoltNetSocketPoolsTest, OnlyMoltNetSessionsLimitHandshakes) {
  std::unique_ptr<net::HttpNetworkSession> moltnet = MakeSession(true);
  std::unique_ptr<net::HttpNetworkSession> regular = MakeSession(false);
  for (bool for_websockets : {false, true}) {
    EXPECT_TRUE(moltnet->CreateCommonConnectJobParams(for_websockets)
                    .molt_socks_handshake_limiter);
    EXPECT_FALSE(regular->CreateCommonConnectJobParams(for_websockets)
                     .molt_socks_handshake_limiter);
  }
}

// Each site has its own handshake slots: a site at its limit waits, another
// site doesn't, and a slot given back goes to the site's oldest waiting job.
TEST_F(MoltNetSocketPoolsTest, HandshakeSlotsPerSite) {
  net::MoltSocksHandshakeLimiter limiter;
  const net::MoltBudgetSite a = net::SchemefulSite(GURL("https://a.test"));
  const net::MoltBudgetSite b = net::SchemefulSite(GURL("https://b.test"));
  std::vector<std::unique_ptr<net::MoltSocksHandshakeLimiter::Slot>> slots;
  for (size_t i = 0; i < net::kMoltSocksHandshakesPerSite; ++i) {
    slots.push_back(limiter.Acquire(a, net::MEDIUM, base::DoNothing()));
    EXPECT_TRUE(slots.back()->granted());
  }
  int granted = 0;
  std::unique_ptr<net::MoltSocksHandshakeLimiter::Slot> waiting =
      limiter.Acquire(a, net::MEDIUM,
                      base::BindLambdaForTesting([&] { ++granted; }));
  EXPECT_FALSE(waiting->granted());
  // Another site isn't held back.
  std::unique_ptr<net::MoltSocksHandshakeLimiter::Slot> other =
      limiter.Acquire(b, net::MEDIUM, base::DoNothing());
  EXPECT_TRUE(other->granted());
  // One of a's handshakes is done: the waiting one goes on (posted).
  slots.pop_back();
  EXPECT_TRUE(waiting->granted());
  EXPECT_EQ(granted, 0);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(granted, 1);
  EXPECT_EQ(limiter.InProgressForTesting(a), net::kMoltSocksHandshakesPerSite);
  // A waiting job that goes away leaves the queue; one that has a slot gives
  // it back.
  std::unique_ptr<net::MoltSocksHandshakeLimiter::Slot> gone =
      limiter.Acquire(a, net::MEDIUM, base::DoNothing());
  EXPECT_EQ(limiter.WaitingForTesting(a), 1u);
  gone.reset();
  EXPECT_EQ(limiter.WaitingForTesting(a), 0u);
  slots.clear();
  waiting.reset();
  EXPECT_EQ(limiter.InProgressForTesting(a), 0u);
  // The one-off keys no site can be named for have fewer.
  std::vector<std::unique_ptr<net::MoltSocksHandshakeLimiter::Slot>> one_off;
  for (size_t i = 0; i < net::kMoltUnattributedSocksHandshakes; ++i) {
    one_off.push_back(
        limiter.Acquire(std::nullopt, net::MEDIUM, base::DoNothing()));
    EXPECT_TRUE(one_off.back()->granted());
  }
  EXPECT_FALSE(
      limiter.Acquire(std::nullopt, net::MEDIUM, base::DoNothing())->granted());
}

}  // namespace
}  // namespace molt_ai::tor
