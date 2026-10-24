// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The decisions of net/socket/molt_per_site_socket_pool_policy.h (std-only).

#include "net/socket/molt_per_site_socket_pool_policy.h"

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

TEST(MoltNetSocketPoolPolicyTest, LimitsAreChromiumsDefaults) {
  EXPECT_EQ(kMoltPerSitePoolSize, 128u);
  // HTTP: 6 per group; WebSockets: 255, so the site's limit.
  EXPECT_EQ(MoltPerSiteGroupLimit(kMoltPerSitePoolSize, 6), 6u);
  EXPECT_EQ(MoltPerSiteGroupLimit(kMoltPerSitePoolSize, 255), 128u);
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

TEST(MoltNetSocketPoolPolicyTest, IsAConstantExpression) {
  static_assert(MoltUsesPerSitePool(kMoltPerSitePoolSize, false));
  static_assert(!MoltUsesPerSitePool(0, false));
  static_assert(MoltPerSiteGroupLimit(128, 6) == 6);
  static_assert(!MoltMayDropSitePool(false, 100));
}

}  // namespace
}  // namespace net
