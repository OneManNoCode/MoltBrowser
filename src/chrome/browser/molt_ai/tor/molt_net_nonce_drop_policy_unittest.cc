// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What MoltNet's network context drops of what a page's nonce keys, and when
// (services/network/molt_nonce_drop_policy.h, std-only).

#include "services/network/molt_nonce_drop_policy.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace network {
namespace {

// A group closed past its site's bound: its page lives on and uses its
// connections, so only its cookies go (and are refused from then on).
TEST(MoltNetNonceDropPolicyTest, AClosedGroupsCookiesGoAndAreRefused) {
  EXPECT_TRUE(MoltNonceCookiesRefusedAfter(MoltNonceDropStep::kClosed));
  EXPECT_FALSE(MoltNonceNetworkStateDropped(MoltNonceDropStep::kClosed));
}

// A page gone: its cookies go and are refused, and its idle connections and
// cached network state go with them.
TEST(MoltNetNonceDropPolicyTest, AGonePageTakesItsNetworkState) {
  EXPECT_TRUE(MoltNonceCookiesRefusedAfter(MoltNonceDropStep::kGone));
  EXPECT_TRUE(MoltNonceNetworkStateDropped(MoltNonceDropStep::kGone));
}

// A while later: everything again, and the nonce is forgotten, so no record
// stays for a page that came once.
TEST(MoltNetNonceDropPolicyTest, ThenItIsForgotten) {
  EXPECT_FALSE(MoltNonceCookiesRefusedAfter(MoltNonceDropStep::kForget));
  EXPECT_TRUE(MoltNonceNetworkStateDropped(MoltNonceDropStep::kForget));
}

static_assert(MoltNonceCookiesRefusedAfter(MoltNonceDropStep::kGone));
static_assert(!MoltNonceNetworkStateDropped(MoltNonceDropStep::kClosed));

}  // namespace
}  // namespace network
