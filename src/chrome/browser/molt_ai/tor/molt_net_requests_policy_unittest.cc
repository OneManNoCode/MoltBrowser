// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_requests_policy.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::tor {
namespace {

TEST(MoltNetRequestsPolicyTest, OtherProfilesKeepEveryFeature) {
  const MoltNetRequestAccess access =
      MoltNetRequestAccessForProfile(/*is_moltnet_profile=*/false);
  EXPECT_EQ(access, MoltNetRequestAccess());
  EXPECT_TRUE(access.cloud_ai);
  EXPECT_TRUE(access.saved_workflows);
  EXPECT_TRUE(access.safe_browsing);
  EXPECT_TRUE(access.image_descriptions);
  EXPECT_TRUE(access.geolocation);
  EXPECT_TRUE(access.cast);
  EXPECT_TRUE(access.relay_authenticators);
}

TEST(MoltNetRequestsPolicyTest, MoltNetGetsNoneOfThem) {
  const MoltNetRequestAccess access =
      MoltNetRequestAccessForProfile(/*is_moltnet_profile=*/true);
  EXPECT_FALSE(access.cloud_ai);
  EXPECT_FALSE(access.saved_workflows);
  EXPECT_FALSE(access.safe_browsing);
  EXPECT_FALSE(access.image_descriptions);
  EXPECT_FALSE(access.geolocation);
  EXPECT_FALSE(access.cast);
  EXPECT_FALSE(access.relay_authenticators);
}

TEST(MoltNetRequestsPolicyTest, IsAConstantExpression) {
  // Upstream hooks include the header and nothing else.
  static_assert(!MoltNetRequestAccessForProfile(true).cast);
  static_assert(MoltNetRequestAccessForProfile(false).relay_authenticators);
  static_assert(SandboxTabWindowFor(true) == SandboxTabWindow::kMoltNet);
  static_assert(!MayChatActInWindow(true, false, false));
}

TEST(MoltNetRequestsPolicyTest, SandboxTabNeverLeavesMoltNet) {
  EXPECT_EQ(SandboxTabWindow::kMoltNet,
            SandboxTabWindowFor(/*chat_in_moltnet=*/true));
  EXPECT_EQ(SandboxTabWindow::kIncognito,
            SandboxTabWindowFor(/*chat_in_moltnet=*/false));
}

TEST(MoltNetRequestsPolicyTest, ChatActsOnlyOnItsSideOfTheBoundary) {
  struct Case {
    bool chat_in_moltnet;
    bool window_in_moltnet;
    bool same_profile;
    bool allowed;
  } const kCases[] = {
      // Regular chat, regular window (its own, or another profile's: the
      // pre-MoltNet behaviour, unchanged).
      {false, false, true, true},
      {false, false, false, true},
      // MoltNet chat in its own MoltNet window.
      {true, true, true, true},
      // A MoltNet chat's action or link must not run outside Tor.
      {true, false, false, false},
      // A regular chat must not drive a MoltNet tab.
      {false, true, false, false},
      // Two MoltNet sessions (two regular profiles) stay apart too.
      {true, true, false, false},
      // Inconsistent input fails closed.
      {true, false, true, false},
      {false, true, true, false},
  };
  for (const Case& c : kCases) {
    SCOPED_TRACE(testing::Message()
                 << "chat_in_moltnet=" << c.chat_in_moltnet
                 << " window_in_moltnet=" << c.window_in_moltnet
                 << " same_profile=" << c.same_profile);
    EXPECT_EQ(c.allowed, MayChatActInWindow(c.chat_in_moltnet,
                                            c.window_in_moltnet,
                                            c.same_profile));
  }
}

TEST(MoltNetRequestsPolicyTest, RefusalsAreShortAndNameMoltNet) {
  for (const char* message :
       {kCloudAiRefusedInMoltNet, kSavedWorkflowRefusedInMoltNet,
        kOtherWindowRefusedInMoltNet}) {
    SCOPED_TRACE(message);
    const std::string text(message);
    EXPECT_FALSE(text.empty());
    EXPECT_LE(text.size(), 120u);
    EXPECT_NE(std::string::npos, text.find("MoltNet"));
    // The page escapes it, but keep it plain text on one line anyway.
    EXPECT_EQ(std::string::npos, text.find_first_of("<>&\n"));
  }
  EXPECT_NE(std::string::npos,
            std::string(kCloudAiRefusedInMoltNet).find("on-device"));
}

}  // namespace
}  // namespace molt_ai::tor
