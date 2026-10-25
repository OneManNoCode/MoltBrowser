// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The decisions of net/socket/molt_per_site_socket_pool_policy.h (std-only).

#include "net/socket/molt_per_site_socket_pool_policy.h"

#include <optional>
#include <string>
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

TEST(MoltNetSocketPoolPolicyTest, HandshakeLimits) {
  EXPECT_EQ(MoltSocksHandshakeLimit(false), 16u);
  EXPECT_EQ(MoltSocksHandshakeLimit(true), 8u);
  EXPECT_EQ(kMoltSocksHandshakeBudget, 192u);
  // Eleven other sites at their full share (and every unattributed slot)
  // before a site below its share can wait, as for connections.
  EXPECT_EQ(kMoltSitesToFillHandshakeBudget, 11u);
  EXPECT_LE(10 * kMoltSocksHandshakesPerSite +
                kMoltUnattributedSocksHandshakes +
                kMoltSocksHandshakesPerSite,
            kMoltSocksHandshakeBudget);
  EXPECT_GT(11 * kMoltSocksHandshakesPerSite +
                kMoltUnattributedSocksHandshakes +
                kMoltSocksHandshakesPerSite,
            kMoltSocksHandshakeBudget);
  // Room for Tor's own circuits below its pending-circuit limit.
  EXPECT_LE(kMoltSocksHandshakeBudget + kMoltUnattributedSocksHandshakes +
                kMoltTorOwnPendingCircuits,
            kMoltTorMaxClientCircuitsPending);
}

TEST(MoltNetSocketPoolPolicyTest, OnionHandshakesWeighTheirCircuits) {
  EXPECT_TRUE(MoltIsOnionHost("abcdef.onion"));
  EXPECT_TRUE(MoltIsOnionHost("www.abcdef.onion"));
  EXPECT_TRUE(MoltIsOnionHost("ABCDEF.ONION"));
  EXPECT_TRUE(MoltIsOnionHost("abcdef.onion."));
  EXPECT_FALSE(MoltIsOnionHost(".onion"));
  EXPECT_FALSE(MoltIsOnionHost("onion"));
  EXPECT_FALSE(MoltIsOnionHost("a.onion.example"));
  EXPECT_FALSE(MoltIsOnionHost("aonion"));
  EXPECT_FALSE(MoltIsOnionHost("a.test"));
  EXPECT_FALSE(MoltIsOnionHost("127.0.0.1"));
  EXPECT_FALSE(MoltIsOnionHost(""));
  EXPECT_FALSE(MoltIsOnionHost("."));
  EXPECT_EQ(MoltSocksHandshakeWeight("x.onion"), 3u);
  EXPECT_EQ(MoltSocksHandshakeWeight("x.test"), 1u);
}

using Granted = std::vector<int>;

TEST(MoltNetSocketPoolPolicyTest, WeightedSlotsLimitEachKey) {
  MoltWeightedSlots<std::string, int> slots(100);
  EXPECT_EQ(slots.Acquire("a", 2, 1, 0, 0), Granted({0}));
  EXPECT_EQ(slots.Acquire("a", 2, 1, 1, 0), Granted({1}));
  EXPECT_EQ(slots.Acquire("a", 2, 1, 2, 0), Granted());
  EXPECT_EQ(slots.Acquire("a", 2, 1, 3, 0), Granted());
  // Another key isn't held back by "a" at its own limit.
  EXPECT_EQ(slots.Acquire("b", 2, 1, 10, 0), Granted({10}));
  EXPECT_EQ(slots.InUse("a"), 2u);
  EXPECT_EQ(slots.Waiting("a"), 2u);
  EXPECT_EQ(slots.Total(), 3u);
  // A slot given back goes to the key's oldest waiting one.
  EXPECT_EQ(slots.Release("a", 1), Granted({2}));
  EXPECT_EQ(slots.Release("a", 1), Granted({3}));
  EXPECT_EQ(slots.Release("a", 1), Granted());
  EXPECT_EQ(slots.Release("a", 1), Granted());
  EXPECT_EQ(slots.InUse("a"), 0u);
  // Nothing more to give back.
  EXPECT_EQ(slots.Release("a", 1), Granted());
  EXPECT_EQ(slots.Release("zzz", 1), Granted());
  EXPECT_EQ(slots.Release("b", 1), Granted());
  EXPECT_EQ(slots.KeyCount(), 0u);
  EXPECT_EQ(slots.Total(), 0u);
}

TEST(MoltNetSocketPoolPolicyTest, WeightedSlotsCountWeights) {
  MoltWeightedSlots<std::string, int> slots(100);
  // Weight 3 within a limit of 16: five fit.
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(slots.Acquire("a", 16, 3, i, 0), Granted({i}));
  }
  EXPECT_EQ(slots.InUse("a"), 15u);
  EXPECT_EQ(slots.Acquire("a", 16, 3, 5, 0), Granted());
  // One of weight 1 would fit, but waits its turn behind the older one.
  EXPECT_EQ(slots.Acquire("a", 16, 1, 6, 0), Granted());
  EXPECT_EQ(slots.Release("a", 3), Granted({5, 6}));
  EXPECT_EQ(slots.InUse("a"), 16u);
  EXPECT_EQ(slots.Release("a", 3), Granted());
  EXPECT_EQ(slots.InUse("a"), 13u);
  // Weights are at least 1 and at most the key's limit.
  MoltWeightedSlots<std::string, int> small(100);
  EXPECT_EQ(small.Acquire("x", 2, 0, 1, 0), Granted({1}));
  EXPECT_EQ(small.InUse("x"), 1u);
  EXPECT_EQ(small.Acquire("y", 2, 9, 2, 0), Granted({2}));
  EXPECT_EQ(small.InUse("y"), 2u);
}

// The budget for all keys together: keys at their full share can't make
// another wait until the budget is used, and then the key holding the
// fewest goes first, also before keys that asked earlier.
TEST(MoltNetSocketPoolPolicyTest, WeightedSlotsShareTheBudgetFairly) {
  MoltWeightedSlots<std::string, int> slots(10);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(slots.Acquire("greedy1", 4, 1, i, 0), Granted({i}));
    EXPECT_EQ(slots.Acquire("greedy2", 4, 1, 10 + i, 0), Granted({10 + i}));
  }
  // Two keys at their full share: the polite key still gets two.
  EXPECT_EQ(slots.Acquire("polite", 4, 1, 20, 0), Granted({20}));
  EXPECT_EQ(slots.Acquire("polite", 4, 1, 21, 0), Granted({21}));
  EXPECT_EQ(slots.Total(), 10u);
  // Full: the next waits.
  EXPECT_EQ(slots.Acquire("polite", 4, 1, 22, 0), Granted());
  EXPECT_EQ(slots.Acquire("newcomer", 4, 1, 30, 0), Granted());
  // A freed slot goes to the waiting key holding the fewest.
  EXPECT_EQ(slots.Release("greedy1", 1), Granted({30}));
  EXPECT_EQ(slots.Release("greedy1", 1), Granted({22}));
  // A key holding more doesn't go before one holding fewer that waits.
  EXPECT_EQ(slots.Acquire("polite", 4, 1, 23, 0), Granted());
  EXPECT_EQ(slots.Acquire("greedy2", 4, 1, 14, 0), Granted());
  EXPECT_EQ(slots.Release("greedy2", 1), Granted({23}));
  // greedy2 (3 in use) waits for the budget, polite is at its limit: a slot
  // polite gives back goes to greedy2.
  EXPECT_EQ(slots.Release("polite", 1), Granted({14}));
  // A heavier request of a key holding fewer holds back lighter ones of
  // keys holding more until there is room for it.
  MoltWeightedSlots<std::string, int> w(6);
  EXPECT_EQ(w.Acquire("big", 6, 1, 1, 0), Granted({1}));
  EXPECT_EQ(w.Acquire("big", 6, 1, 2, 0), Granted({2}));
  EXPECT_EQ(w.Acquire("big", 6, 1, 3, 0), Granted({3}));
  EXPECT_EQ(w.Acquire("big", 6, 1, 4, 0), Granted({4}));
  EXPECT_EQ(w.Acquire("onion", 6, 3, 5, 0), Granted());
  EXPECT_EQ(w.Acquire("big", 6, 1, 6, 0), Granted());
  EXPECT_EQ(w.Release("big", 1), Granted({5}));
  EXPECT_EQ(w.Total(), 6u);
}

TEST(MoltNetSocketPoolPolicyTest, WeightedSlotsPriorityThenArrival) {
  MoltWeightedSlots<std::string, int> slots(100);
  EXPECT_EQ(slots.Acquire("a", 1, 1, 0, 0), Granted({0}));
  EXPECT_EQ(slots.Acquire("a", 1, 1, 1, 1), Granted());
  EXPECT_EQ(slots.Acquire("a", 1, 1, 2, 3), Granted());
  EXPECT_EQ(slots.Acquire("a", 1, 1, 3, 3), Granted());
  EXPECT_EQ(slots.Acquire("a", 1, 1, 4, 1), Granted());
  EXPECT_EQ(slots.SetPriority("a", 4, 5), Granted());
  EXPECT_EQ(slots.Release("a", 1), Granted({4}));
  EXPECT_EQ(slots.Release("a", 1), Granted({2}));
  EXPECT_EQ(slots.Release("a", 1), Granted({3}));
  EXPECT_EQ(slots.Release("a", 1), Granted({1}));
  EXPECT_EQ(slots.Release("a", 1), Granted());
  EXPECT_EQ(slots.KeyCount(), 0u);
}

TEST(MoltNetSocketPoolPolicyTest, WeightedSlotsCancel) {
  MoltWeightedSlots<std::string, int> slots(100);
  EXPECT_EQ(slots.Acquire("a", 1, 1, 0, 0), Granted({0}));
  EXPECT_EQ(slots.Acquire("a", 1, 1, 1, 0), Granted());
  EXPECT_EQ(slots.Acquire("a", 1, 1, 2, 0), Granted());
  bool cancelled = false;
  EXPECT_EQ(slots.Cancel("a", 1, &cancelled), Granted());
  EXPECT_TRUE(cancelled);
  EXPECT_EQ(slots.Cancel("a", 1, &cancelled), Granted());
  EXPECT_FALSE(cancelled);
  EXPECT_EQ(slots.Cancel("b", 1, &cancelled), Granted());
  EXPECT_FALSE(cancelled);
  EXPECT_EQ(slots.Release("a", 1), Granted({2}));
  EXPECT_EQ(slots.Release("a", 1), Granted());
  EXPECT_EQ(slots.KeyCount(), 0u);
  // A waiter that held the budget for a key holding fewer is gone: the
  // others may go.
  MoltWeightedSlots<std::string, int> w(4);
  EXPECT_EQ(w.Acquire("big", 4, 1, 1, 0), Granted({1}));
  EXPECT_EQ(w.Acquire("big", 4, 1, 2, 0), Granted({2}));
  EXPECT_EQ(w.Acquire("big", 4, 1, 3, 0), Granted({3}));
  EXPECT_EQ(w.Acquire("onion", 4, 3, 4, 0), Granted());
  EXPECT_EQ(w.Acquire("big", 4, 1, 5, 0), Granted());
  EXPECT_EQ(w.Cancel("onion", 4), Granted({5}));
}

TEST(MoltNetSocketPoolPolicyTest, IsAConstantExpression) {
  static_assert(MoltUsesPerSitePool(kMoltPerSitePoolSize, false));
  static_assert(!MoltUsesPerSitePool(0, false));
  static_assert(MoltPerSiteGroupLimit(48, 6) == 6);
  static_assert(MoltPerSitePoolLimit(48, true, false) == 16);
  static_assert(!MoltMayDropSitePool(false, 100));
  static_assert(MoltBudgetAdmits(1, 2, 0, std::nullopt));
  static_assert(MoltSocksHandshakeLimit(false) == kMoltSocksHandshakesPerSite);
  static_assert(MoltIsOnionHost("a.onion"));
  static_assert(MoltSocksHandshakeWeight("a.test") == kMoltHandshakeWeight);
}

}  // namespace
}  // namespace net
