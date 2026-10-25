// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Which of MoltNet's renderer processes serve one BrowsingInstance each
// (molt_net_process_policy.h and
// content/browser/renderer_host/molt_browsing_instance_processes_policy.h,
// both std-only).

#include "chrome/browser/molt_ai/tor/molt_net_process_policy.h"

#include "content/browser/renderer_host/molt_browsing_instance_processes_policy.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace tor {
namespace {

TEST(MoltNetProcessPolicyTest, MoltNetWebContentIsKeptApart) {
  for (const char* scheme : {"https", "http", "data", "blob", "about", "file",
                             "filesystem", ""}) {
    EXPECT_TRUE(KeepBrowsingInstancesApart(true, scheme)) << scheme;
  }
}

TEST(MoltNetProcessPolicyTest, BrowserPagesKeepChromiumsSharing) {
  for (const char* scheme :
       {"chrome", "chrome-untrusted", "devtools", "chrome-extension"}) {
    EXPECT_FALSE(KeepBrowsingInstancesApart(true, scheme)) << scheme;
  }
  // Not a prefix match.
  EXPECT_TRUE(KeepBrowsingInstancesApart(true, "chromex"));
}

TEST(MoltNetProcessPolicyTest, OtherProfilesKeepChromiumsSharing) {
  for (const char* scheme : {"https", "chrome", "data"}) {
    EXPECT_FALSE(KeepBrowsingInstancesApart(false, scheme)) << scheme;
  }
}

// A process that serves another BrowsingInstance isn't suitable where they
// are kept apart; everywhere else Chromium decides.
TEST(MoltNetProcessPolicyTest, OneBrowsingInstancePerProcess) {
  EXPECT_FALSE(content::MoltMayServeBrowsingInstance(true, true));
  EXPECT_TRUE(content::MoltMayServeBrowsingInstance(true, false));
  EXPECT_TRUE(content::MoltMayServeBrowsingInstance(false, true));
  EXPECT_TRUE(content::MoltMayServeBrowsingInstance(false, false));
}

}  // namespace
}  // namespace tor
}  // namespace molt_ai
