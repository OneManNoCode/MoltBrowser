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
#include <vector>

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

// A group's record of storage keys is bounded: once it holds
// kMoltNetNonceStorageKeysPerGroup, another one closes the group.
TEST(MoltNetNonceStoragePolicyTest, AGroupsKeysAreBounded) {
  EXPECT_EQ(kMoltNetNonceStorageKeysPerGroup, 256u);
  EXPECT_FALSE(MoltNetNonceGroupFull(0));
  EXPECT_FALSE(MoltNetNonceGroupFull(kMoltNetNonceStorageKeysPerGroup - 1));
  EXPECT_TRUE(MoltNetNonceGroupFull(kMoltNetNonceStorageKeysPerGroup));
}

// A group closed past its own bound no longer counts to its site's, and is
// its page's until the page goes.
TEST(MoltNetNonceStoragePolicyTest, AGroupClosedPastItsOwnBound) {
  Groups groups;
  for (size_t i = 0; i < kMoltNetNonceGroupsPerSite; ++i) {
    ASSERT_FALSE(groups.Added(kSite, Nonce(i)).has_value());
  }
  EXPECT_TRUE(groups.Close(Nonce(3)));
  EXPECT_FALSE(groups.Close(Nonce(3)));
  EXPECT_TRUE(groups.IsTracked(Nonce(3)));
  EXPECT_EQ(groups.open_count(kSite), kMoltNetNonceGroupsPerSite - 1);
  // So the next page of the site closes no other group.
  EXPECT_FALSE(groups.Added(kSite, "next").has_value());
  groups.Gone(Nonce(3));
  EXPECT_FALSE(groups.IsTracked(Nonce(3)));
  EXPECT_FALSE(groups.Close("unknown"));
  // A site's last open group closed leaves no record for the site.
  Groups one;
  ASSERT_FALSE(one.Added(kOther, "only").has_value());
  EXPECT_TRUE(one.Close("only"));
  EXPECT_EQ(one.site_count(), 0u);
  one.Gone("only");
  EXPECT_EQ(one.nonce_count(), 0u);
}

// A nonce that successive pages share (a fenced frame navigating itself)
// goes with the last of them, not the first.
TEST(MoltNetNonceStoragePolicyTest, ANonceSharedBySuccessivePages) {
  MoltNetNoncePageCounts<std::string> pages;
  pages.Uses("fenced");   // its first document's page
  pages.Uses("fenced");   // the next one, made before the first goes
  EXPECT_EQ(pages.PagesOf("fenced"), 2u);
  EXPECT_FALSE(pages.Left("fenced"));
  pages.Uses("fenced");   // and another
  EXPECT_FALSE(pages.Left("fenced"));
  EXPECT_TRUE(pages.Left("fenced"));
  EXPECT_EQ(pages.size(), 0u);
  // A credentialless frame's nonce is its page's alone.
  pages.Uses("credentialless");
  EXPECT_TRUE(pages.Left("credentialless"));
  // One no page noted goes at once.
  EXPECT_TRUE(pages.Left("unknown"));
  EXPECT_EQ(pages.size(), 0u);
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

using Refusals = MoltNetNonceRefusals<std::string, std::string>;

const char kPartition[] = "/profile";
const char kOtherPartition[] = "/profile/Storage/ext/app";

// What a partition's network context made again refuses: a closed group's
// nonce while its page lives on, a gone page's until it is forgotten, and
// nothing of another partition's.
TEST(MoltNetNonceStoragePolicyTest, RefusalsForAContextMadeAgain) {
  Refusals refusals;
  EXPECT_TRUE(refusals.RefusedIn(kPartition).empty());
  refusals.Closed("closed", kPartition);
  refusals.Gone("gone", kPartition);
  refusals.Closed("elsewhere", kOtherPartition);
  EXPECT_EQ(refusals.RefusedIn(kPartition),
            (std::vector<std::string>{"closed", "gone"}));
  EXPECT_EQ(refusals.RefusedIn(kOtherPartition),
            std::vector<std::string>{"elsewhere"});
  // The closed group's page goes: still refused, until forgotten.
  refusals.Gone("closed", kPartition);
  EXPECT_TRUE(refusals.IsRefused("closed"));
  refusals.Forgotten("closed");
  refusals.Forgotten("gone");
  EXPECT_TRUE(refusals.RefusedIn(kPartition).empty());
  EXPECT_FALSE(refusals.IsRefused("gone"));
  EXPECT_EQ(refusals.size(), 1u);
}

// The record follows the steps the network context is sent (kClosed and
// kGone refuse, kForget no longer does), so it never refuses what the
// context doesn't, and records go once forgotten.
TEST(MoltNetNonceStoragePolicyTest, RefusalsFollowTheSteps) {
  static_assert(MoltNetNonceSweepsCookiesLater(/*closed=*/false));
  static_assert(!MoltNetNonceSweepsCookiesLater(/*closed=*/true));
  Refusals refusals;
  for (int page = 0; page < 1000; ++page) {
    const std::string nonce = Nonce(page);
    if (page % 3 == 0) {
      refusals.Closed(nonce, kPartition);
    }
    refusals.Gone(nonce, kPartition);
    EXPECT_TRUE(refusals.IsRefused(nonce));
    refusals.Forgotten(nonce);
  }
  EXPECT_EQ(refusals.size(), 0u);
  // Forgetting what isn't refused changes nothing.
  refusals.Forgotten("unknown");
  EXPECT_EQ(refusals.size(), 0u);
}

}  // namespace
}  // namespace molt_ai::tor
