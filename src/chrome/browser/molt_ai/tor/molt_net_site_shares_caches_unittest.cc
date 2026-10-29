// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's per-site shares (net/base/molt_site_share_policy.h) on
// Chromium's real caches: TLS sessions (net::SSLClientSessionCache, also
// under memory pressure), HTTP authentication entries (net::HttpAuthCache),
// server properties (net::HttpServerProperties: HTTP/2 support and the
// HTTP/1.1 requirement) and CORS preflight results
// (network::cors::PreflightCache); and the marks of broken alternative
// services dropped with a nonce. Without the shares each keeps Chromium's
// own behaviour: one site's entries evict another's.

#include <memory>
#include <optional>
#include <string>

#include "base/memory_coordinator/memory_coordinator_features.h"
#include "base/memory_coordinator/test_memory_consumer_registry.h"
#include "base/memory_coordinator/utils.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/simple_test_clock.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "net/base/auth.h"
#include "net/base/host_port_pair.h"
#include "net/base/molt_site_share_policy.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/network_isolation_key.h"
#include "net/base/schemeful_site.h"
#include "net/http/alternative_service.h"
#include "net/http/http_auth.h"
#include "net/http/http_auth_cache.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_server_properties.h"
#include "net/log/net_log_with_source.h"
#include "net/ssl/ssl_client_session_cache.h"
#include "services/network/cors/preflight_cache.h"
#include "services/network/cors/preflight_result.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/scheme_host_port.h"

namespace molt_ai::tor {
namespace {

net::SchemefulSite Site(const std::string& host) {
  return net::SchemefulSite(GURL("https://" + host));
}

net::NetworkAnonymizationKey KeyOf(const std::string& site) {
  return net::NetworkAnonymizationKey::CreateSameSite(Site(site));
}

// A server of `site`'s pages: one of its own subdomains.
url::SchemeHostPort ServerOf(const std::string& site, int i) {
  return url::SchemeHostPort("https", "s" + base::NumberToString(i) + "." + site,
                             443);
}

// TLS sessions.
class MoltNetSiteSharesTlsTest : public testing::Test {
 protected:
  MoltNetSiteSharesTlsTest() : ssl_ctx_(SSL_CTX_new(TLS_method())) {
    clock_.SetNow(base::Time::FromTimeT(1000000000));
  }

  void Insert(net::SSLClientSessionCache& cache,
              const std::string& site,
              int i) {
    bssl::UniquePtr<SSL_SESSION> session(SSL_SESSION_new(ssl_ctx_.get()));
    ASSERT_TRUE(SSL_SESSION_set_protocol_version(session.get(), TLS1_2_VERSION));
    SSL_SESSION_set_time(session.get(), clock_.Now().ToTimeT());
    SSL_SESSION_set_timeout(session.get(), 3600);
    cache.Insert(cache.generation_number(), KeyFor(site, i),
                 std::move(session));
  }

  int Resumable(net::SSLClientSessionCache& cache,
                const std::string& site,
                int n) {
    int found = 0;
    for (int i = 0; i < n; ++i) {
      found += cache.Lookup(KeyFor(site, i)) != nullptr;
    }
    return found;
  }

  static net::SSLClientSessionCache::Key KeyFor(const std::string& site,
                                                int i) {
    net::SSLClientSessionCache::Key key;
    key.server = net::HostPortPair("s" + base::NumberToString(i) + "." + site,
                                   443);
    key.network_anonymization_key = KeyOf(site);
    return key;
  }

  std::unique_ptr<net::SSLClientSessionCache> MakeCache(bool shares) {
    net::SSLClientSessionCache::Config config;
    config.expiration_check_count = 100000;
    auto cache = std::make_unique<net::SSLClientSessionCache>(config);
    cache->SetClockForTesting(&clock_);
    if (shares) {
      cache->MoltKeepPerSiteShares();
    }
    return cache;
  }

  // As net's SSLClientSessionCacheTest does.
  void SimulateMemoryLimitAndRelease(base::MemoryLimit memory_limit) {
    test_memory_consumer_registry_.NotifyUpdateMemoryLimitAsync(
        memory_limit, task_environment_.QuitClosure());
    task_environment_.RunUntilQuit();
    test_memory_consumer_registry_.NotifyReleaseMemoryAsync(
        task_environment_.QuitClosure());
    task_environment_.RunUntilQuit();
  }

  base::test::ScopedFeatureList feature_list_{base::kStatefulMemoryPressure};
  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO};
  base::TestMemoryConsumerRegistry test_memory_consumer_registry_;
  base::SimpleTestClock clock_;

 private:
  bssl::UniquePtr<SSL_CTX> ssl_ctx_;
};

TEST_F(MoltNetSiteSharesTlsTest, AGreedySiteEvictsOnlyItsOwnSessions) {
  for (bool shares : {false, true}) {
    SCOPED_TRACE(shares);
    auto cache = MakeCache(shares);
    for (int i = 0; i < 5; ++i) {
      Insert(*cache, "polite.test", i);
    }
    for (int i = 0; i < 2000; ++i) {
      Insert(*cache, "greedy.test", i);
    }
    if (shares) {
      EXPECT_EQ(Resumable(*cache, "polite.test", 5), 5);
      EXPECT_EQ(cache->size(), 5 + net::kMoltTlsSessionShares.per_site);
      EXPECT_EQ(Resumable(*cache, "greedy.test", 2000),
                static_cast<int>(net::kMoltTlsSessionShares.per_site));
    } else {
      EXPECT_EQ(Resumable(*cache, "polite.test", 5), 0);  // Chromium's
      EXPECT_EQ(cache->size(), 1024u);
    }
  }
}

TEST_F(MoltNetSiteSharesTlsTest, PastTheTotalANewSiteKeepsNothing) {
  auto cache = MakeCache(/*shares=*/true);
  for (int i = 0; i < 5; ++i) {
    Insert(*cache, "polite.test", i);
  }
  for (int s = 0; s < 20; ++s) {
    for (int i = 0; i < 300; ++i) {
      Insert(*cache, "g" + base::NumberToString(s) + ".test", i);
    }
  }
  EXPECT_EQ(cache->size(), net::kMoltTlsSessionShares.total);
  EXPECT_EQ(Resumable(*cache, "polite.test", 5), 5);
  Insert(*cache, "late.test", 0);
  EXPECT_EQ(Resumable(*cache, "late.test", 1), 0);
  EXPECT_EQ(cache->size(), net::kMoltTlsSessionShares.total);
}

TEST_F(MoltNetSiteSharesTlsTest, MemoryPressureEvictsNothingWithShares) {
  for (bool shares : {false, true}) {
    SCOPED_TRACE(shares);
    auto cache = MakeCache(shares);
    for (int i = 0; i < 10; ++i) {
      Insert(*cache, "polite.test", i);
    }
    SimulateMemoryLimitAndRelease(base::kModerateMemoryPressureThreshold);
    SimulateMemoryLimitAndRelease(0);
    EXPECT_EQ(cache->size(), shares ? 10u : 0u);
    SimulateMemoryLimitAndRelease(100);
  }
}

// HTTP authentication entries.
TEST(MoltNetSiteSharesAuthTest, AGreedySiteEvictsOnlyItsOwnEntries) {
  for (bool shares : {false, true}) {
    SCOPED_TRACE(shares);
    net::HttpAuthCache cache(
        /*key_server_entries_by_network_anonymization_key=*/true);
    if (shares) {
      cache.MoltKeepPerSiteShares();
    }
    auto sign_in = [&](const std::string& site, int i) {
      return cache.Add(ServerOf(site, i), net::HttpAuth::AUTH_SERVER, "realm",
                       net::HttpAuth::AUTH_SCHEME_BASIC, KeyOf(site), "Basic",
                       net::AuthCredentials(u"user", u"fake-password"), "/");
    };
    auto remembered = [&](const std::string& site, int n) {
      int found = 0;
      for (int i = 0; i < n; ++i) {
        found += cache.Lookup(ServerOf(site, i), net::HttpAuth::AUTH_SERVER,
                              "realm", net::HttpAuth::AUTH_SCHEME_BASIC,
                              KeyOf(site)) != nullptr;
      }
      return found;
    };
    sign_in("polite.test", 0);
    sign_in("polite.test", 1);
    for (int i = 0; i < 50; ++i) {
      sign_in("greedy.test", i);
    }
    if (shares) {
      EXPECT_EQ(remembered("polite.test", 2), 2);
      EXPECT_EQ(cache.GetEntriesSizeForTesting(),
                2 + net::kMoltHttpAuthShares.per_site);
      for (int s = 0; s < 20; ++s) {
        for (int i = 0; i < 25; ++i) {
          sign_in("g" + base::NumberToString(s) + ".test", i);
        }
      }
      EXPECT_EQ(remembered("polite.test", 2), 2);
      EXPECT_LE(cache.GetEntriesSizeForTesting(),
                net::kMoltHttpAuthShares.total);
      // What a site holding none signs in with is kept all the same: past
      // the total it takes the place of an entry of a site holding the
      // most, so the sign-in prompt doesn't come back on every request.
      for (int i = 0; i < 3; ++i) {
        const std::string late = "late" + base::NumberToString(i) + ".test";
        EXPECT_NE(sign_in(late, 0), nullptr);
        EXPECT_EQ(remembered(late, 1), 1);
      }
      EXPECT_EQ(remembered("polite.test", 2), 2);
      EXPECT_LE(cache.GetEntriesSizeForTesting(),
                net::kMoltHttpAuthShares.total);
    } else {
      EXPECT_EQ(remembered("polite.test", 2), 0);  // Chromium's
      EXPECT_EQ(cache.GetEntriesSizeForTesting(), 20u);
    }
  }
}

// Server properties.
class MoltNetSiteSharesServerPropertiesTest : public testing::Test {
 protected:
  base::test::TaskEnvironment task_environment_;
};

TEST_F(MoltNetSiteSharesServerPropertiesTest,
       AGreedySiteEvictsOnlyItsOwnEntries) {
  for (bool shares : {false, true}) {
    SCOPED_TRACE(shares);
    net::HttpServerProperties properties;
    if (shares) {
      properties.MoltKeepPerSiteShares();
    }
    for (int i = 0; i < 5; ++i) {
      properties.SetSupportsSpdy(ServerOf("polite.test", i),
                                 KeyOf("polite.test"), true);
      properties.SetHTTP11Required(ServerOf("polite.test", i),
                                   KeyOf("polite.test"));
    }
    for (int i = 0; i < 2000; ++i) {
      properties.SetSupportsSpdy(ServerOf("greedy.test", i),
                                 KeyOf("greedy.test"), true);
      properties.SetHTTP11Required(ServerOf("greedy.test", i),
                                   KeyOf("greedy.test"));
    }
    int spdy = 0;
    int http11 = 0;
    for (int i = 0; i < 5; ++i) {
      spdy += properties.GetSupportsSpdy(ServerOf("polite.test", i),
                                         KeyOf("polite.test"));
      http11 += properties.RequiresHTTP11(ServerOf("polite.test", i),
                                          KeyOf("polite.test"));
    }
    EXPECT_EQ(spdy, shares ? 5 : 0);
    EXPECT_EQ(http11, shares ? 5 : 0);
  }
}

// A server's demand for HTTP/1.1 is always kept, as the request that met it
// is sent again over HTTP/1.1 only through it (HttpNetworkTransaction::
// HandleHttp11Required()): past the total, a site holding none takes the
// place of an entry of a site holding the most, and a site holding fewer
// keeps its own.
TEST_F(MoltNetSiteSharesServerPropertiesTest, AnHttp11DemandIsAlwaysKept) {
  net::HttpServerProperties properties;
  properties.MoltKeepPerSiteShares();
  for (int i = 0; i < 2; ++i) {
    properties.SetHTTP11Required(ServerOf("polite.test", i),
                                 KeyOf("polite.test"));
  }
  const int per_site =
      static_cast<int>(net::kMoltHttp11RequiredShares.per_site);
  for (size_t s = 0; s < net::kMoltSiteShareSites + 2; ++s) {
    const std::string site = "g" + base::NumberToString(s) + ".test";
    for (int i = 0; i < per_site; ++i) {
      properties.SetHTTP11Required(ServerOf(site, i), KeyOf(site));
    }
  }
  for (int i = 0; i < 3; ++i) {
    const std::string late = "late" + base::NumberToString(i) + ".test";
    properties.SetHTTP11Required(ServerOf(late, 0), KeyOf(late));
    EXPECT_TRUE(properties.RequiresHTTP11(ServerOf(late, 0), KeyOf(late)));
  }
  for (int i = 0; i < 2; ++i) {
    EXPECT_TRUE(properties.RequiresHTTP11(ServerOf("polite.test", i),
                                          KeyOf("polite.test")));
  }
}

// The marks of broken alternative services kept under a gone page's nonce
// go with it; others stay.
TEST_F(MoltNetSiteSharesServerPropertiesTest, BrokenMarksGoWithTheNonce) {
  net::HttpServerProperties properties;
  const base::UnguessableToken nonce = base::UnguessableToken::Create();
  const net::NetworkAnonymizationKey nonced =
      net::NetworkAnonymizationKey::CreateFromParts(
          Site("a.test"), /*is_cross_site=*/true, nonce);
  const net::AlternativeService quic(net::NextProto::kProtoQUIC, "cdn.test",
                                     443);
  properties.MarkAlternativeServiceBroken(quic, nonced);
  properties.MarkAlternativeServiceBroken(quic, KeyOf("b.test"));
  ASSERT_TRUE(properties.IsAlternativeServiceBroken(quic, nonced));
  properties.MoltClearForNonce(nonce);
  EXPECT_FALSE(properties.IsAlternativeServiceBroken(quic, nonced));
  EXPECT_FALSE(properties.WasAlternativeServiceRecentlyBroken(quic, nonced));
  EXPECT_TRUE(properties.IsAlternativeServiceBroken(quic, KeyOf("b.test")));
}

// CORS preflight results.
TEST(MoltNetSiteSharesPreflightTest, AGreedySiteEvictsOnlyItsOwnResults) {
  for (bool shares : {false, true}) {
    SCOPED_TRACE(shares);
    network::cors::PreflightCache cache;
    if (shares) {
      cache.MoltKeepPerSiteShares();
    }
    auto store = [&](const std::string& site, int i) {
      cache.AppendEntry(
          url::Origin::Create(GURL("https://api." + site)),
          GURL("https://api." + site + "/v1/" + base::NumberToString(i)),
          net::NetworkIsolationKey(Site(site), Site(site)),
          network::cors::PreflightResult::Create(
              network::mojom::CredentialsMode::kInclude, std::string("PUT"),
              std::nullopt, std::string("600"), nullptr));
    };
    auto skips = [&](const std::string& site, int n) {
      int found = 0;
      for (int i = 0; i < n; ++i) {
        found += cache.CheckIfRequestCanSkipPreflight(
            url::Origin::Create(GURL("https://api." + site)),
            GURL("https://api." + site + "/v1/" + base::NumberToString(i)),
            net::NetworkIsolationKey(Site(site), Site(site)),
            network::mojom::CredentialsMode::kInclude, "PUT",
            net::HttpRequestHeaders(), /*is_revalidating=*/false,
            net::NetLogWithSource(), /*acam_preflight_spec_conformant=*/true,
            /*is_ad_auction_trusted_signals_request=*/false);
      }
      return found;
    };
    for (int i = 0; i < 5; ++i) {
      store("polite.test", i);
    }
    // Two greedy sites whose entries sort before and after the polite
    // site's (the cache is a map), so these sit in the middle.
    for (int i = 0; i < 10000; ++i) {
      store("a-greedy.test", i);
      store("z-greedy.test", i);
    }
    if (shares) {
      EXPECT_EQ(skips("polite.test", 5), 5);
      EXPECT_EQ(cache.CountEntriesForTesting(),
                5 + 2 * net::kMoltPreflightShares.per_site);
    } else {
      // Chromium purges ten entries at a random place each time it is
      // full, about 2,000 times here, each reaching one of the polite
      // site's with a chance of about 1.4%: one or more of them goes but
      // for a chance of about 1e-12.
      EXPECT_LT(skips("polite.test", 5), 5);
      EXPECT_LE(cache.CountEntriesForTesting(), 1024u);
    }
  }
}

}  // namespace
}  // namespace molt_ai::tor
