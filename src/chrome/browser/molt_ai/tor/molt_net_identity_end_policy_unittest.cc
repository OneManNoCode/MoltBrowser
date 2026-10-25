// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// When the end of a MoltNet identity counts as done
// (molt_net_identity_end_policy.h, std-only).

#include "chrome/browser/molt_ai/tor/molt_net_identity_end_policy.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace tor {
namespace {

using enum MoltNetEndProbeStep;

// Only "OK, a proxy list with no entry" is the refusal.
TEST(MoltNetIdentityEndPolicyTest, OnlyTheRefusalCounts) {
  EXPECT_TRUE(MoltNetContextRefuses(true, true, true));
  // Still routing (SOCKS5 to Tor, or anything else with an entry).
  EXPECT_FALSE(MoltNetContextRefuses(true, true, false));
  // An error, or no answer (a lost context).
  EXPECT_FALSE(MoltNetContextRefuses(false, false, false));
  EXPECT_FALSE(MoltNetContextRefuses(false, true, true));
  EXPECT_FALSE(MoltNetContextRefuses(true, false, true));
}

// Every context must refuse; one that doesn't yet means asking again, and
// past the deadline giving up.
TEST(MoltNetIdentityEndPolicyTest, EveryContextMustRefuse) {
  EXPECT_EQ(MoltNetEndProbeNext(2, 2, false), kRefuses);
  EXPECT_EQ(MoltNetEndProbeNext(2, 2, true), kRefuses);
  EXPECT_EQ(MoltNetEndProbeNext(2, 1, false), kAskAgain);
  EXPECT_EQ(MoltNetEndProbeNext(2, 0, false), kAskAgain);
  EXPECT_EQ(MoltNetEndProbeNext(2, 1, true), kGiveUp);
  EXPECT_EQ(MoltNetEndProbeNext(1, 0, true), kGiveUp);
}

// No network context: nothing can be sent.
TEST(MoltNetIdentityEndPolicyTest, NoContextRefusesEverything) {
  EXPECT_EQ(MoltNetEndProbeNext(0, 0, false), kRefuses);
}

// Asked often, for long enough, but not forever.
TEST(MoltNetIdentityEndPolicyTest, ProbeTiming) {
  EXPECT_LE(kMoltNetEndProbeIntervalMs, 50);
  EXPECT_GE(kMoltNetEndProbeDeadlineMs, 1000);
  EXPECT_LE(kMoltNetEndProbeDeadlineMs, 10000);
}

TEST(MoltNetIdentityEndPolicyTest, NoNavigationOfAnEndedIdentity) {
  EXPECT_FALSE(MoltNetNavigationMayProceed(true, true));
  EXPECT_TRUE(MoltNetNavigationMayProceed(true, false));
  // Other profiles are never ended.
  EXPECT_TRUE(MoltNetNavigationMayProceed(false, true));
  EXPECT_TRUE(MoltNetNavigationMayProceed(false, false));
}

// Nothing the browser holds for an ended MoltNet identity's pages is sent:
// keepalive requests, fetchLater, downloads, Background Fetch.
TEST(MoltNetIdentityEndPolicyTest, NothingHeldForAPageIsSentAfterTheEnd) {
  EXPECT_FALSE(MoltNetBrowserMaySendForPage(true, true));
  EXPECT_TRUE(MoltNetBrowserMaySendForPage(true, false));
  EXPECT_TRUE(MoltNetBrowserMaySendForPage(false, true));
  EXPECT_TRUE(MoltNetBrowserMaySendForPage(false, false));
  static_assert(!MoltNetBrowserMaySendForPage(true, true));
}

}  // namespace
}  // namespace tor
}  // namespace molt_ai
