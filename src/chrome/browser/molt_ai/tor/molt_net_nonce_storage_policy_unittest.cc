// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's nonce groups per top-level site
// (chrome/browser/molt_ai/tor/molt_net_nonce_storage_policy.h, std-only):
// a page's group goes with it, and a site holds at most
// kMoltNetNonceGroupsPerSite at once, closing its own oldest past that.

#include "chrome/browser/molt_ai/tor/molt_net_nonce_storage_policy.h"

#include <optional>
#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::tor {
namespace {

using Groups = MoltNetNonceGroups<std::string>;

const char kSite[] = "https://a.test";
const char kOther[] = "https://b.test";

std::string Nonce(int i) {
  return "n" + std::to_string(i);
}

// A page that reloads itself gets a fresh nonce each time; with the old page
// gone each time, nothing builds up.
TEST(MoltNetNonceStoragePolicyTest, AGroupGoesWithItsPage) {
  Groups groups;
  for (int i = 0; i < 10000; ++i) {
    EXPECT_FALSE(groups.Added(kSite, Nonce(i)).has_value());
    groups.Gone(Nonce(i));
  }
  EXPECT_EQ(groups.nonce_count(), 0u);
  EXPECT_EQ(groups.site_count(), 0u);
}

// Past its bound, a site's oldest open group is closed; another site's
// groups never count, and never are.
TEST(MoltNetNonceStoragePolicyTest, ASitesBoundClosesItsOwnOldest) {
  Groups groups;
  EXPECT_EQ(kMoltNetNonceGroupsPerSite, 32u);
  ASSERT_FALSE(groups.Added(kOther, "other").has_value());
  for (size_t i = 0; i < kMoltNetNonceGroupsPerSite; ++i) {
    ASSERT_FALSE(groups.Added(kSite, Nonce(i)).has_value()) << i;
  }
  EXPECT_EQ(groups.Added(kSite, "late"), Nonce(0));
  EXPECT_EQ(groups.Added(kSite, "later"), Nonce(1));
  EXPECT_EQ(groups.open_count(kSite), kMoltNetNonceGroupsPerSite);
  EXPECT_EQ(groups.open_count(kOther), 1u);
  // A closed group is still its page's until the page goes.
  EXPECT_TRUE(groups.IsTracked(Nonce(0)));
  groups.Gone(Nonce(0));
  EXPECT_FALSE(groups.IsTracked(Nonce(0)));
  EXPECT_EQ(groups.open_count(kSite), kMoltNetNonceGroupsPerSite);
  // An open one going makes room.
  groups.Gone(Nonce(5));
  EXPECT_FALSE(groups.Added(kSite, "room").has_value());
}

// A page counts to its top-level site; a page served sandboxed (an opaque
// origin, which serializes as "null" whatever site served it) to the site it
// came from; one with no such site on its own.
TEST(MoltNetNonceStoragePolicyTest, TheSiteAPageCountsTo) {
  EXPECT_EQ(MoltNetNonceGroupSite(false, kSite, "", "n1"), kSite);
  EXPECT_EQ(MoltNetNonceGroupSite(true, "null", kSite, "n1"), kSite);
  EXPECT_EQ(MoltNetNonceGroupSite(true, "null", kOther, "n1"), kOther);
  EXPECT_EQ(MoltNetNonceGroupSite(true, "null", "", "n1"), "page n1");
  EXPECT_NE(MoltNetNonceGroupSite(true, "null", "", "n1"),
            MoltNetNonceGroupSite(true, "null", "", "n2"));
  EXPECT_NE(MoltNetNonceGroupSite(true, "null", "", "n1"), "null");
}

// One site's sandboxed pages, however many, never close another site's
// sandboxed page's group: they count to their own site's bound.
TEST(MoltNetNonceStoragePolicyTest, SandboxedPagesCountToTheirOwnSite) {
  Groups groups;
  ASSERT_FALSE(
      groups.Added(MoltNetNonceGroupSite(true, "null", kOther, "victim"),
                   "victim")
          .has_value());
  for (int i = 0; i < 4 * static_cast<int>(kMoltNetNonceGroupsPerSite); ++i) {
    const std::optional<std::string> closed = groups.Added(
        MoltNetNonceGroupSite(true, "null", kSite, Nonce(i)), Nonce(i));
    EXPECT_NE(closed, std::optional<std::string>("victim")) << i;
  }
  EXPECT_EQ(groups.open_count(kOther), 1u);
  EXPECT_EQ(groups.open_count(kSite), kMoltNetNonceGroupsPerSite);
  EXPECT_EQ(groups.open_count("null"), 0u);
}

// A gone page's cookie partitions are deleted again later, past the longest
// a keepalive request of the page may set a cookie in them; a closed group's
// page lives on, so its cookies go again when it goes.
static_assert(kMoltNetNonceCookieSweepSeconds >
              kMoltNetKeepaliveOutlivesPageSeconds);
TEST(MoltNetNonceStoragePolicyTest, AGonePagesCookiesGoAgainLater) {
  EXPECT_TRUE(MoltNetNonceSweepsCookiesLater(/*closed=*/false));
  EXPECT_FALSE(MoltNetNonceSweepsCookiesLater(/*closed=*/true));
  EXPECT_EQ(kMoltNetKeepaliveOutlivesPageSeconds, 30);
}

// A nonce seen again (another frame of the same page) is nothing new.
TEST(MoltNetNonceStoragePolicyTest, ANonceCountsOnce) {
  Groups groups;
  EXPECT_FALSE(groups.Added(kSite, "x").has_value());
  EXPECT_FALSE(groups.Added(kSite, "x").has_value());
  EXPECT_EQ(groups.open_count(kSite), 1u);
  groups.Gone("x");
  groups.Gone("x");
  groups.Gone("unknown");
  EXPECT_EQ(groups.nonce_count(), 0u);
}

}  // namespace
}  // namespace molt_ai::tor
