// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The decisions of net/socket/molt_per_site_socket_pool_policy.h (std-only).

#include "net/socket/molt_per_site_socket_pool_policy.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace net {
namespace {

TEST(MoltNetSocketPoolPolicyTest, PerSitePoolsOnlyWhereAsked) {
  // MoltNet's context, through a proxy (Tor): a pool per site.
  EXPECT_TRUE(MoltUsesPerSitePool(kMoltPerSitePoolSize, false));
  EXPECT_TRUE(MoltUsesPerSitePool(1, false));
  // Direct connections, or any other context: Chromium's pools.
  EXPECT_FALSE(MoltUsesPerSitePool(kMoltPerSitePoolSize, true));
  EXPECT_FALSE(MoltUsesPerSitePool(0, false));
  EXPECT_FALSE(MoltUsesPerSitePool(0, true));
}

TEST(MoltNetSocketPoolPolicyTest, FixedLimitsPerSite) {
  EXPECT_EQ(kMoltPerSitePoolSize, 48u);
  // HTTP: the context's setting; WebSockets and the one-off keys no site can
  // be named for: less, never more than the setting.
  EXPECT_EQ(MoltPerSitePoolLimit(kMoltPerSitePoolSize, false, false), 48u);
  EXPECT_EQ(MoltPerSitePoolLimit(kMoltPerSitePoolSize, true, false), 16u);
  EXPECT_EQ(MoltPerSitePoolLimit(kMoltPerSitePoolSize, false, true), 8u);
  EXPECT_EQ(MoltPerSitePoolLimit(kMoltPerSitePoolSize, true, true), 8u);
  EXPECT_EQ(MoltPerSitePoolLimit(4, true, false), 4u);
  EXPECT_EQ(MoltPerSitePoolLimit(4, false, true), 4u);
  // HTTP: 6 per group; WebSockets: 255, so the site's limit.
  EXPECT_EQ(MoltPerSiteGroupLimit(48, 6), 6u);
  EXPECT_EQ(MoltPerSiteGroupLimit(16, 255), 16u);
  EXPECT_EQ(MoltPerSiteGroupLimit(4, 6), 4u);
}

TEST(MoltNetSocketPoolPolicyTest, OnlyLongUnusedPoolsAreDropped) {
  EXPECT_TRUE(MoltMayDropSitePool(true, kMoltUnusedSitePoolTimeoutSeconds));
  EXPECT_TRUE(MoltMayDropSitePool(true, 3600));
  EXPECT_FALSE(
      MoltMayDropSitePool(true, kMoltUnusedSitePoolTimeoutSeconds - 1));
  EXPECT_FALSE(MoltMayDropSitePool(true, 0));
  // Something uses it (a socket, a request, a pending callback): kept,
  // however long ago it was handed out.
  EXPECT_FALSE(MoltMayDropSitePool(false, 3600));
  // A clock that went backwards doesn't drop anything.
  EXPECT_FALSE(MoltMayDropSitePool(true, -5));
}

// The budget is well inside what one Tor process always accepts, and it
// takes eleven other sites at their full share to make a site below its
// own limits wait for it.
TEST(MoltNetSocketPoolPolicyTest, BudgetFitsTorAndManySites) {
  EXPECT_EQ(kMoltNetConnectionBudget, 768u);
  EXPECT_LE(kMoltNetConnectionBudget + kMoltTorOwnConnections,
            kMoltTorConnLimit - kMoltTorKeptFileDescriptors);
  EXPECT_EQ(kMoltPerSiteConnections, 64u);
  EXPECT_EQ(kMoltUnattributedConnections, 16u);
  EXPECT_EQ(kMoltSitesToFillBudget, 11u);
  // Ten sites at their share, every unattributed connection, and one site
  // one below its share: still room.
  EXPECT_LT(10 * kMoltPerSiteConnections + kMoltUnattributedConnections +
                (kMoltPerSiteConnections - 1),
            kMoltNetConnectionBudget);
  // Eleven: full.
  EXPECT_GE(11 * kMoltPerSiteConnections + kMoltUnattributedConnections +
                (kMoltPerSiteConnections - 1),
            kMoltNetConnectionBudget);
  EXPECT_EQ(MoltSitesToFillBudget(100, 10, 0), 10u);
  EXPECT_EQ(MoltSitesToFillBudget(100, 10, 5), 9u);
}

TEST(MoltNetSocketPoolPolicyTest, BudgetAdmitsWithinItAndSmallestFirst) {
  // Within the budget, nobody waiting: yes.
  EXPECT_TRUE(MoltBudgetAdmits(0, 768, 0, std::nullopt));
  EXPECT_TRUE(MoltBudgetAdmits(767, 768, 40, std::nullopt));
  // Full: no.
  EXPECT_FALSE(MoltBudgetAdmits(768, 768, 0, std::nullopt));
  EXPECT_FALSE(MoltBudgetAdmits(900, 768, 0, std::nullopt));
  // A site using fewer waits for the budget: it goes first.
  EXPECT_FALSE(MoltBudgetAdmits(700, 768, 40, 3));
  // Waiting sites using as many or more don't hold back a smaller one.
  EXPECT_TRUE(MoltBudgetAdmits(700, 768, 3, 3));
  EXPECT_TRUE(MoltBudgetAdmits(700, 768, 3, 40));
}

TEST(MoltNetSocketPoolPolicyTest, WakeOrderIsFewestFirstThenOldest) {
  const std::vector<size_t> in_use = {40, 3, 10, 3, 0};
  EXPECT_EQ(MoltWakeOrder(in_use), (std::vector<size_t>{4, 1, 3, 2, 0}));
  EXPECT_TRUE(MoltWakeOrder(std::vector<size_t>{}).empty());
}

TEST(MoltNetSocketPoolPolicyTest, PreemptsOnlyFromLargerSitesLargestFirst) {
  const std::vector<size_t> in_use = {64, 5, 6, 7, 64, 30};
  // For a waiting site using 5: only those using 7 or more, largest first,
  // ties in the order given.
  EXPECT_EQ(MoltPreemptionOrder(in_use, 5),
            (std::vector<size_t>{0, 4, 5, 3}));
  // Nobody uses more than one more than a site using 63.
  EXPECT_TRUE(MoltPreemptionOrder(in_use, 63).empty());
  EXPECT_EQ(MoltPreemptionOrder(in_use, 62), (std::vector<size_t>{0, 4}));
}

TEST(MoltNetSocketPoolPolicyTest, OpaqueOriginPrecursor) {
  using Result = std::optional<std::string_view>;
  // url::Origin::GetDebugString() of opaque origins (Chromium 156).
  EXPECT_EQ(MoltOpaqueOriginPrecursor(
                "null [internally: (1A2B3C4D5E6F7A8B9C0D1E2F3A4B5C6D) "
                "derived from https://a.example]"),
            Result("https://a.example"));
  EXPECT_EQ(MoltOpaqueOriginPrecursor(
                "null [internally: (nonce TBD) derived from "
                "http://b.example:8080]"),
            Result("http://b.example:8080"));
  EXPECT_EQ(MoltOpaqueOriginPrecursor(
                "null [internally: (ABC) derived from https://[::1]:8443]"),
            Result("https://[::1]:8443"));
  // Without the nonce (GetDebugString(false)).
  EXPECT_EQ(MoltOpaqueOriginPrecursor(
                "null [internally: derived from https://c.example]"),
            Result("https://c.example"));
  // Derived from nothing (one-off keys): no site.
  EXPECT_EQ(MoltOpaqueOriginPrecursor("null [internally: (ABC) anonymous]"),
            std::nullopt);
  EXPECT_EQ(MoltOpaqueOriginPrecursor("null [internally: anonymous]"),
            std::nullopt);
  // Anything else: no site.
  for (const char* bad : {
           "",
           "https://a.example",
           "null",
           "null [internally: (ABC) derived from https://a.example",
           "x null [internally: (ABC) derived from https://a.example]",
           "null [internally: (ABC) derived from ]",
           "null [internally: (ABC) derived from a.example]",
           "null [internally: (ABC) derived from ://a.example]",
           "null [internally: (ABC) derived from https://]",
           "null [internally: (ABC) derived from https://a b]",
           "null [internally: (ABC) derived from https://a\tb]",
       }) {
    EXPECT_EQ(MoltOpaqueOriginPrecursor(bad), std::nullopt) << bad;
  }
  static_assert(MoltOpaqueOriginPrecursor(
                    "null [internally: (A) derived from https://a.test]") ==
                std::string_view("https://a.test"));
}

TEST(MoltNetSocketPoolPolicyTest, HandshakeLimits) {
  EXPECT_EQ(MoltSocksHandshakeLimit(false), 16u);
  EXPECT_EQ(MoltSocksHandshakeLimit(true), 8u);
}

TEST(MoltNetSocketPoolPolicyTest, SlotQueueLimitsEachKeyOnItsOwn) {
  MoltSlotQueue<std::string, int> queue;
  for (int i = 0; i < 2; ++i) {
    EXPECT_TRUE(queue.Acquire("a", 2, i, 0));
  }
  EXPECT_FALSE(queue.Acquire("a", 2, 2, 0));
  EXPECT_FALSE(queue.Acquire("a", 2, 3, 0));
  // Another key isn't held back by "a".
  EXPECT_TRUE(queue.Acquire("b", 2, 10, 0));
  EXPECT_EQ(queue.InUse("a"), 2u);
  EXPECT_EQ(queue.Waiting("a"), 2u);
  EXPECT_EQ(queue.InUse("b"), 1u);
  // A slot given back goes to the oldest waiting one.
  EXPECT_EQ(queue.Release("a"), std::optional<int>(2));
  EXPECT_EQ(queue.InUse("a"), 2u);
  EXPECT_EQ(queue.Release("a"), std::optional<int>(3));
  EXPECT_EQ(queue.Release("a"), std::nullopt);
  EXPECT_EQ(queue.InUse("a"), 1u);
  EXPECT_EQ(queue.Release("a"), std::nullopt);
  EXPECT_EQ(queue.InUse("a"), 0u);
  // Nothing more to give back.
  EXPECT_EQ(queue.Release("a"), std::nullopt);
  EXPECT_EQ(queue.Release("zzz"), std::nullopt);
  EXPECT_EQ(queue.Release("b"), std::nullopt);
  EXPECT_EQ(queue.KeyCount(), 0u);
}

TEST(MoltNetSocketPoolPolicyTest, SlotQueuePriorityThenArrival) {
  MoltSlotQueue<std::string, int> queue;
  EXPECT_TRUE(queue.Acquire("a", 1, 0, 0));
  EXPECT_FALSE(queue.Acquire("a", 1, 1, 1));
  EXPECT_FALSE(queue.Acquire("a", 1, 2, 3));
  EXPECT_FALSE(queue.Acquire("a", 1, 3, 3));
  EXPECT_FALSE(queue.Acquire("a", 1, 4, 1));
  queue.SetPriority("a", 4, 5);
  EXPECT_EQ(queue.Release("a"), std::optional<int>(4));
  EXPECT_EQ(queue.Release("a"), std::optional<int>(2));
  EXPECT_EQ(queue.Release("a"), std::optional<int>(3));
  EXPECT_EQ(queue.Release("a"), std::optional<int>(1));
  EXPECT_EQ(queue.Release("a"), std::nullopt);
  EXPECT_EQ(queue.KeyCount(), 0u);
}

TEST(MoltNetSocketPoolPolicyTest, SlotQueueCancel) {
  MoltSlotQueue<std::string, int> queue;
  EXPECT_TRUE(queue.Acquire("a", 1, 0, 0));
  EXPECT_FALSE(queue.Acquire("a", 1, 1, 0));
  EXPECT_FALSE(queue.Acquire("a", 1, 2, 0));
  EXPECT_TRUE(queue.Cancel("a", 1));
  EXPECT_FALSE(queue.Cancel("a", 1));
  EXPECT_FALSE(queue.Cancel("b", 1));
  EXPECT_EQ(queue.Release("a"), std::optional<int>(2));
  EXPECT_EQ(queue.Release("a"), std::nullopt);
  EXPECT_EQ(queue.KeyCount(), 0u);
  // A key with only a cancelled waiter is gone too.
  EXPECT_TRUE(queue.Acquire("c", 0, 7, 0) == false);
  EXPECT_TRUE(queue.Cancel("c", 7));
  EXPECT_EQ(queue.KeyCount(), 0u);
}

TEST(MoltNetSocketPoolPolicyTest, IsAConstantExpression) {
  static_assert(MoltUsesPerSitePool(kMoltPerSitePoolSize, false));
  static_assert(!MoltUsesPerSitePool(0, false));
  static_assert(MoltPerSiteGroupLimit(48, 6) == 6);
  static_assert(MoltPerSitePoolLimit(48, true, false) == 16);
  static_assert(!MoltMayDropSitePool(false, 100));
  static_assert(MoltBudgetAdmits(1, 2, 0, std::nullopt));
  static_assert(MoltSocksHandshakeLimit(false) == kMoltSocksHandshakesPerSite);
}

}  // namespace
}  // namespace net
