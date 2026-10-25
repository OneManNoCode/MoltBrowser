// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What a Background Fetch request of an off-the-record profile (MoltNet's)
// carries from the fetch: the site's isolation, its credentials mode and
// its initiator
// (components/download/internal/background_service/
// molt_registering_site_isolation.h, header-only).

#include "components/download/internal/background_service/molt_registering_site_isolation.h"

#include <optional>

#include "net/base/isolation_info.h"
#include "net/cookies/site_for_cookies.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace download {
namespace {

using network::mojom::CredentialsMode;

url::Origin O(const char* url) {
  return url::Origin::Create(GURL(url));
}

// A service worker's isolation (StorageKey::ToPartialNetIsolationInfo()).
net::IsolationInfo SiteIsolation(const url::Origin& origin) {
  return net::IsolationInfo::Create(net::IsolationInfo::RequestType::kOther,
                                    origin, origin,
                                    net::SiteForCookies::FromOrigin(origin));
}

network::ResourceRequest Request() {
  network::ResourceRequest request;
  request.url = GURL("https://cdn.test/movie.mp4");
  return request;
}

TEST(MoltNetBackgroundFetchRequestTest, CarriesTheSitesIsolation) {
  const std::optional<net::IsolationInfo> isolation =
      SiteIsolation(O("https://a.test"));
  network::ResourceRequest request = Request();
  MoltCarryRegisteringSiteIsolation(isolation, request);
  ASSERT_TRUE(request.trusted_params.has_value());
  EXPECT_TRUE(request.trusted_params->isolation_info.IsEqualForTesting(
      *isolation));
  EXPECT_TRUE(request.site_for_cookies.IsEquivalent(
      net::SiteForCookies::FromOrigin(O("https://a.test"))));
}

// The fetch's own credentials mode and its origin as initiator, as the
// site's own fetch() would send them; Chromium 156's request had
// "include" and no initiator.
TEST(MoltNetBackgroundFetchRequestTest, CarriesCredentialsModeAndInitiator) {
  const std::optional<net::IsolationInfo> isolation =
      SiteIsolation(O("https://a.test"));
  for (CredentialsMode mode :
       {CredentialsMode::kOmit, CredentialsMode::kSameOrigin,
        CredentialsMode::kInclude}) {
    network::ResourceRequest request = Request();
    MoltCarryFetchCredentialsAndInitiator(isolation, mode, O("https://a.test"),
                                          request);
    EXPECT_EQ(request.credentials_mode, mode);
    EXPECT_EQ(request.request_initiator, O("https://a.test"));
  }
}

// A request without the site's isolation (another client of the in-memory
// service) is left as Chromium makes it.
TEST(MoltNetBackgroundFetchRequestTest, OthersLeftAsChromium) {
  for (const std::optional<net::IsolationInfo>& none :
       {std::optional<net::IsolationInfo>(),
        std::optional<net::IsolationInfo>(net::IsolationInfo())}) {
    network::ResourceRequest request = Request();
    MoltCarryRegisteringSiteIsolation(none, request);
    MoltCarryFetchCredentialsAndInitiator(none, CredentialsMode::kOmit,
                                          O("https://a.test"), request);
    EXPECT_FALSE(request.trusted_params.has_value());
    EXPECT_EQ(request.credentials_mode, CredentialsMode::kInclude);
    EXPECT_FALSE(request.request_initiator.has_value());
  }
}

}  // namespace
}  // namespace download
