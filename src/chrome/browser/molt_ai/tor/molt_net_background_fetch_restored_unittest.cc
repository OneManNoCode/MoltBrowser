// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A page's Background Fetch download that the download service takes up
// again in a later browser session (components/download/internal/
// background_service/molt_restored_fetch.h): with the fetch's parameters
// back from Background Fetch it keeps its first party on redirects, and one
// whose download item is from the earlier session starts again with them
// rather than resuming without them. On the download service's real
// request parameters (download::RequestParams).

#include "components/download/internal/background_service/molt_restored_fetch.h"

#include "components/download/public/background_service/download_params.h"
#include "net/base/isolation_info.h"
#include "net/cookies/site_for_cookies.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace download {
namespace {

static_assert(MoltRestartsRestoredFetch(/*has_initiator=*/true,
                                        /*has_driver_entry=*/true));
static_assert(!MoltRestartsRestoredFetch(true, false));
static_assert(!MoltRestartsRestoredFetch(false, true));
static_assert(!MoltRestartsRestoredFetch(false, false));

// What the download service holds for a download it read back from its
// database (ProtoConversions::RequestParamsFromProto()): no initiator, no
// isolation, the first party changed on redirects.
RequestParams StoredParams() {
  RequestParams params;
  params.url = GURL("https://b.test/movie.mp4");
  params.method = "GET";
  params.credentials_mode = ::network::mojom::CredentialsMode::kOmit;
  return params;
}

// The same with what Background Fetch gives back for its request
// (BackgroundFetchDelegateBase::DidGetUploadData()).
RequestParams PageFetchParams() {
  RequestParams params = StoredParams();
  const url::Origin page = url::Origin::Create(GURL("https://a.test/"));
  params.initiator = page;
  params.isolation_info = net::IsolationInfo::Create(
      net::IsolationInfo::RequestType::kOther, page, page,
      net::SiteForCookies::FromOrigin(page));
  return params;
}

// A page's fetch whose download item the browser kept from the earlier
// session starts again, keeping its first party.
TEST(MoltNetBackgroundFetchRestoredTest, AnEarlierSessionsFetchStartsAgain) {
  RequestParams params = PageFetchParams();
  ASSERT_TRUE(params.update_first_party_url_on_redirect);
  EXPECT_TRUE(MoltPrepareRestoredFetch(params, /*has_driver_entry=*/true));
  EXPECT_FALSE(params.update_first_party_url_on_redirect);
  // What it starts again with is the fetch's.
  EXPECT_TRUE(params.initiator.has_value());
  EXPECT_TRUE(params.isolation_info.has_value());
  EXPECT_EQ(params.credentials_mode, ::network::mojom::CredentialsMode::kOmit);
}

// One with no item yet starts as Chromium starts it, keeping its first
// party on redirects as in its first session.
TEST(MoltNetBackgroundFetchRestoredTest, AFreshStartKeepsItsFirstParty) {
  RequestParams params = PageFetchParams();
  EXPECT_FALSE(MoltPrepareRestoredFetch(params, /*has_driver_entry=*/false));
  EXPECT_FALSE(params.update_first_party_url_on_redirect);
}

// Downloads of the download service's other clients, and a fetch whose
// parameters didn't come back, go on as in Chromium: resumed, and their
// first party unchanged.
TEST(MoltNetBackgroundFetchRestoredTest, OtherDownloadsGoOnAsChromium) {
  for (const bool has_driver_entry : {false, true}) {
    RequestParams params = StoredParams();
    EXPECT_FALSE(MoltPrepareRestoredFetch(params, has_driver_entry));
    EXPECT_TRUE(params.update_first_party_url_on_redirect);
    EXPECT_FALSE(params.initiator.has_value());
  }
}

}  // namespace
}  // namespace download
