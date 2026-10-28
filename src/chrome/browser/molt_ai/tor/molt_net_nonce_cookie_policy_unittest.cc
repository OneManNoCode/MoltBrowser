// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What MoltNet's cookie store keeps under a page's nonce
// (net/cookies/molt_nonce_cookie_policy.h, std-only): totals per nonce
// partition and per nonce, the cookies refused, and records that go once
// they hold nothing.

#include "net/cookies/molt_nonce_cookie_policy.h"

#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace net {
namespace {

using Totals = MoltNonceCookieTotals<std::string, std::string>;

const char kNonce[] = "nonce";
const char kOtherNonce[] = "other";
const char kPartition[] = "https://a.test";
const char kSecondPartition[] = "https://b.test";

// The limits are well above what an embedded frame uses, and a partition's
// fit within its nonce's.
TEST(MoltNetNonceCookiePolicyTest, TheLimits) {
  EXPECT_EQ(kMoltNoncePartitionMaxCookies, 300u);
  EXPECT_EQ(kMoltNoncePartitionMaxCookieBytes, 32u * 1024);
  EXPECT_EQ(kMoltNonceMaxCookies, 600u);
  EXPECT_EQ(kMoltNonceMaxCookieBytes, 64u * 1024);
  // Chromium's own limit for one domain in a partition fits a partition.
  EXPECT_GE(kMoltNoncePartitionMaxCookies, 180u);
  EXPECT_GE(kMoltNoncePartitionMaxCookieBytes, 10u * 1024);
}

// A partition past its total is over; one cookie less, it isn't.
TEST(MoltNetNonceCookiePolicyTest, APartitionsTotal) {
  Totals totals;
  for (size_t i = 0; i < kMoltNoncePartitionMaxCookies; ++i) {
    totals.Added(kNonce, kPartition, 10);
  }
  EXPECT_FALSE(totals.Over(kNonce, kPartition));
  totals.Added(kNonce, kPartition, 10);
  EXPECT_TRUE(totals.Over(kNonce, kPartition));
  totals.Removed(kNonce, kPartition, 10);
  EXPECT_FALSE(totals.Over(kNonce, kPartition));

  // By bytes too.
  Totals bytes;
  bytes.Added(kNonce, kPartition, kMoltNoncePartitionMaxCookieBytes);
  EXPECT_FALSE(bytes.Over(kNonce, kPartition));
  bytes.Added(kNonce, kPartition, 1);
  EXPECT_TRUE(bytes.Over(kNonce, kPartition));
}

// A nonce's partitions together hold at most the nonce's total; each one
// within its own is still over then.
TEST(MoltNetNonceCookiePolicyTest, ANoncesTotal) {
  Totals totals;
  for (size_t i = 0; i < kMoltNoncePartitionMaxCookies; ++i) {
    totals.Added(kNonce, kPartition, 10);
    totals.Added(kNonce, kSecondPartition, 10);
  }
  EXPECT_EQ(totals.Of(kNonce).cookies, kMoltNonceMaxCookies);
  EXPECT_FALSE(totals.Over(kNonce, kPartition));
  totals.Added(kNonce, kSecondPartition, 10);
  EXPECT_TRUE(totals.Over(kNonce, kPartition));
  EXPECT_TRUE(totals.Over(kNonce, kSecondPartition));
  // Another nonce counts on its own.
  totals.Added(kOtherNonce, kPartition, 10);
  EXPECT_FALSE(totals.Over(kOtherNonce, kPartition));
}

// A refused nonce refuses every cookie of its partitions until it no longer
// is; another nonce is never refused for it.
TEST(MoltNetNonceCookiePolicyTest, ARefusedNonce) {
  Totals totals;
  EXPECT_FALSE(totals.Refuses(kNonce, kPartition, 10));
  totals.SetRefused(kNonce, true);
  EXPECT_TRUE(totals.IsRefused(kNonce));
  EXPECT_TRUE(totals.Refuses(kNonce, kPartition, 10));
  EXPECT_TRUE(totals.Refuses(kNonce, kSecondPartition, 1));
  EXPECT_FALSE(totals.Refuses(kOtherNonce, kPartition, 10));
  totals.SetRefused(kNonce, false);
  EXPECT_FALSE(totals.Refuses(kNonce, kPartition, 10));
}

// A cookie is refused only when the nonce's other partitions leave it no
// room: a partition with cookies of its own makes room by evicting them.
TEST(MoltNetNonceCookiePolicyTest, NoRoomLeftByOtherPartitions) {
  Totals totals;
  for (size_t i = 0; i < kMoltNoncePartitionMaxCookies; ++i) {
    totals.Added(kNonce, kPartition, 10);
    totals.Added(kNonce, kSecondPartition, 10);
  }
  // A third partition: the other two hold the nonce's whole total.
  EXPECT_TRUE(totals.Refuses(kNonce, "https://c.test", 10));
  // Either of the two can evict its own cookies.
  EXPECT_FALSE(totals.Refuses(kNonce, kPartition, 10));
  EXPECT_FALSE(totals.Refuses(kNonce, kSecondPartition, 10));

  // By bytes: the other partition holds all but 50 bytes of the total.
  Totals bytes;
  bytes.Added(kNonce, kPartition, kMoltNonceMaxCookieBytes - 60);
  bytes.Added(kNonce, kSecondPartition, 10);
  EXPECT_TRUE(bytes.Refuses(kNonce, "https://c.test", 100));
  EXPECT_FALSE(bytes.Refuses(kNonce, "https://c.test", 50));
  // A partition holding 10 bytes can't make room for 100 either; one
  // holding nearly all of it can.
  EXPECT_TRUE(bytes.Refuses(kNonce, kSecondPartition, 100));
  EXPECT_FALSE(bytes.Refuses(kNonce, kPartition, 100));
  // A cookie's name and value never take more than a partition may hold.
  EXPECT_FALSE(MoltNonceCookieRefused(false, {}, {},
                                      kMoltCookieMaxNameValueBytes));
}

// A nonce's record, and each partition's, go once they hold nothing, unless
// the nonce is refused: records don't pile up for pages that come once.
TEST(MoltNetNonceCookiePolicyTest, RecordsGoWhenEmpty) {
  Totals totals;
  totals.Added(kNonce, kPartition, 10);
  totals.Added(kNonce, kSecondPartition, 20);
  EXPECT_EQ(totals.PartitionsOf(kNonce),
            (std::vector<std::string>{kPartition, kSecondPartition}));
  totals.Removed(kNonce, kPartition, 10);
  EXPECT_EQ(totals.PartitionsOf(kNonce),
            std::vector<std::string>{kSecondPartition});
  EXPECT_EQ(totals.OfPartition(kNonce, kPartition), MoltNonceCookieCounts());
  totals.Removed(kNonce, kSecondPartition, 20);
  EXPECT_EQ(totals.size(), 0u);

  // A refused nonce stays until it no longer is.
  totals.SetRefused(kNonce, true);
  EXPECT_EQ(totals.size(), 1u);
  totals.SetRefused(kNonce, false);
  EXPECT_EQ(totals.size(), 0u);

  // One with cookies stays when it no longer is refused.
  totals.SetRefused(kNonce, true);
  totals.Added(kNonce, kPartition, 10);
  totals.SetRefused(kNonce, false);
  EXPECT_EQ(totals.size(), 1u);
  totals.Removed(kNonce, kPartition, 10);
  EXPECT_EQ(totals.size(), 0u);

  // Removing what isn't counted changes nothing.
  totals.Removed(kOtherNonce, kPartition, 10);
  EXPECT_EQ(totals.size(), 0u);
}

// A page that reloads itself, each time with credentialless frames that set
// cookies, leaves no record once each page's nonce is dropped.
TEST(MoltNetNonceCookiePolicyTest, ManyPagesLeaveNoRecords) {
  Totals totals;
  for (int page = 0; page < 1000; ++page) {
    const std::string nonce = "n" + std::to_string(page);
    for (int site = 0; site < 3; ++site) {
      const std::string partition = "https://s" + std::to_string(site);
      totals.Added(nonce, partition, 40);
      totals.Added(nonce, partition, 40);
    }
    // The page goes: refused, its cookies deleted; later forgotten.
    totals.SetRefused(nonce, true);
    for (const std::string& partition : totals.PartitionsOf(nonce)) {
      totals.Removed(nonce, partition, 40);
      totals.Removed(nonce, partition, 40);
    }
    totals.SetRefused(nonce, false);
  }
  EXPECT_EQ(totals.size(), 0u);
}

}  // namespace
}  // namespace net
