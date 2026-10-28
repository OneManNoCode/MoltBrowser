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
