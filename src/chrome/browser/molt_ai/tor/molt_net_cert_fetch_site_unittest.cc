// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's certificate fetches count against the site whose connection
// needed them (net/cert/molt_cert_fetch_site.h), on the real net and mojo
// types: the thread's site, the fetch's network key, and the site carried
// with a verification request across the certificate verifier service's
// interface.

#include <optional>
#include <string>

#include "mojo/public/cpp/test_support/test_utils.h"
#include "net/base/isolation_info.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/schemeful_site.h"
#include "net/cert/cert_verifier.h"
#include "net/cert/molt_cert_fetch_site.h"
#include "net/cert/x509_certificate.h"
#include "net/socket/molt_connection_budget.h"
#include "net/test/cert_test_util.h"
#include "net/test/test_data_directory.h"
#include "services/cert_verifier/cert_net_url_loader/molt_cert_fetch_isolation.h"
#include "services/network/public/cpp/cert_verifier/cert_verifier_mojom_traits.h"
#include "services/network/public/mojom/cert_verifier_service.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace molt_ai::tor {
namespace {

TEST(MoltNetCertFetchSiteTest, TheThreadsSiteWhileItsVerificationRuns) {
  EXPECT_EQ(net::MoltGetCertFetchSite(), nullptr);
  {
    net::MoltScopedCertFetchSite outer("https://a.test");
    ASSERT_TRUE(net::MoltGetCertFetchSite());
    EXPECT_EQ(*net::MoltGetCertFetchSite(), "https://a.test");
    {
      net::MoltScopedCertFetchSite inner("https://b.test");
      EXPECT_EQ(*net::MoltGetCertFetchSite(), "https://b.test");
    }
    EXPECT_EQ(*net::MoltGetCertFetchSite(), "https://a.test");
    {
      net::MoltScopedCertFetchSite none("");
      EXPECT_EQ(net::MoltGetCertFetchSite(), nullptr);
    }
  }
  EXPECT_EQ(net::MoltGetCertFetchSite(), nullptr);
}

// A fetch for a verification naming a site has a one-off key of that site:
// its own nonce (no cache entry, no shared connection or circuit), counted
// against the site's limits.
TEST(MoltNetCertFetchSiteTest, AFetchsKeyCountsAgainstItsSite) {
  const net::IsolationInfo a1 =
      cert_verifier::MoltCertFetchIsolationInfo("https://a.test");
  const net::IsolationInfo a2 =
      cert_verifier::MoltCertFetchIsolationInfo("https://a.test");
  const net::NetworkAnonymizationKey key1 = a1.network_anonymization_key();
  const net::NetworkAnonymizationKey key2 = a2.network_anonymization_key();
  EXPECT_EQ(net::MoltBudgetSiteFor(key1),
            net::MoltBudgetSite(net::SchemefulSite(GURL("https://a.test"))));
  ASSERT_TRUE(key1.GetNonce());
  ASSERT_TRUE(key2.GetNonce());
  EXPECT_NE(*key1.GetNonce(), *key2.GetNonce());
  EXPECT_TRUE(a1.network_isolation_key().IsTransient());
  // No site, or something that isn't one: derived from nothing, counted
  // with the keys no site can be named for.
  for (const std::string& bad :
       {std::string(), std::string("not a site"),
        "https://" + std::string(3000, 'a') + ".test"}) {
    const net::IsolationInfo info =
        cert_verifier::MoltCertFetchIsolationInfo(bad);
    EXPECT_EQ(net::MoltBudgetSiteFor(info.network_anonymization_key()),
              std::nullopt)
        << bad.substr(0, 40);
    EXPECT_TRUE(info.network_isolation_key().IsTransient());
  }
}

// The site travels with the verification request: it is compared with the
// rest (two sites' verifications are never merged) and crosses the
// certificate verifier service's interface.
TEST(MoltNetCertFetchSiteTest, VerificationRequestsCarryTheSite) {
  const scoped_refptr<net::X509Certificate> cert =
      net::ImportCertFromFile(net::GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert);
  auto params = [&](const std::string& site) {
    net::CertVerifier::RequestParams p(cert, "www.example.com", 0, "", "");
    if (!site.empty()) {
      p.MoltSetCertFetchSite(site);
    }
    return p;
  };
  EXPECT_EQ(params(""), params(""));
  EXPECT_EQ(params("https://a.test"), params("https://a.test"));
  EXPECT_FALSE(params("https://a.test") == params("https://b.test"));
  EXPECT_FALSE(params("https://a.test") == params(""));
  EXPECT_TRUE(params("") < params("https://a.test") ||
              params("https://a.test") < params(""));

  for (const std::string& site :
       {std::string(), std::string("https://a.test")}) {
    net::CertVerifier::RequestParams in = params(site);
    net::CertVerifier::RequestParams out;
    ASSERT_TRUE(
        mojo::test::SerializeAndDeserialize<cert_verifier::mojom::RequestParams>(
            in, out));
    EXPECT_EQ(out.molt_cert_fetch_site(), site);
    EXPECT_EQ(out, in);
  }
}

}  // namespace
}  // namespace molt_ai::tor
