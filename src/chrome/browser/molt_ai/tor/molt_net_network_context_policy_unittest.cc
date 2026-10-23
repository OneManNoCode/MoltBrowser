// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_network_context_policy.h"

#include <optional>
#include <set>
#include <string>

#include "services/cert_verifier/public/mojom/cert_verifier_service_factory.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::tor {
namespace {

TEST(MoltNetNetworkContextPolicyTest, OtherProfilesKeepChromiumsBehaviour) {
  const MoltNetNetworkContextSettings settings =
      MoltNetNetworkContextSettingsFor(/*is_moltnet_profile=*/false);
  EXPECT_EQ(settings, MoltNetNetworkContextSettings());
  EXPECT_FALSE(settings.isolate_each_cert_fetch);
  EXPECT_FALSE(settings.basic_and_digest_auth_only);
  EXPECT_FALSE(settings.fresh_socks_isolation_password);
}

TEST(MoltNetNetworkContextPolicyTest, MoltNetGetsAll) {
  const MoltNetNetworkContextSettings settings =
      MoltNetNetworkContextSettingsFor(/*is_moltnet_profile=*/true);
  EXPECT_TRUE(settings.isolate_each_cert_fetch);
  EXPECT_TRUE(settings.basic_and_digest_auth_only);
  EXPECT_TRUE(settings.fresh_socks_isolation_password);
}

bool IsUpperHex(const std::string& s) {
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

TEST(MoltNetNetworkContextPolicyTest, SocksPasswordIs128RandomBits) {
  std::set<std::string> seen;
  for (int i = 0; i < 64; ++i) {
    const std::string password = NewSocksIsolationPassword();
    // RFC 1929: 1 to 255 bytes. 32 hex digits = 128 bits.
    EXPECT_EQ(password.size(), 32u);
    EXPECT_TRUE(IsUpperHex(password)) << password;
    EXPECT_NE(password, std::string(32, '0'));
    EXPECT_NE(password, "moltnet");
    seen.insert(password);
  }
  EXPECT_EQ(seen.size(), 64u);
}

TEST(MoltNetNetworkContextPolicyTest, IsAConstantExpression) {
  static_assert(MoltNetNetworkContextSettingsFor(true).isolate_each_cert_fetch);
  static_assert(
      !MoltNetNetworkContextSettingsFor(false).basic_and_digest_auth_only);
}

// What ProfileNetworkContextService hands the network service and the
// certificate verifier: the real mojom structs, as it creates them.
struct Params {
  network::mojom::NetworkContextParamsPtr network_context =
      network::mojom::NetworkContextParams::New();
  cert_verifier::mojom::CertVerifierCreationParamsPtr cert_verifier =
      cert_verifier::mojom::CertVerifierCreationParams::New();

  Params() {
    network_context->http_auth_static_network_context_params =
        network::mojom::HttpAuthStaticNetworkContextParams::New();
  }
};

TEST(MoltNetNetworkContextPolicyTest, MoltNetParamsSetTheFlags) {
  Params params;
  ApplyMoltNetNetworkContextSettings(/*is_moltnet_profile=*/true,
                                     *params.network_context,
                                     *params.cert_verifier);
  EXPECT_TRUE(params.cert_verifier->molt_isolate_each_cert_fetch);
  EXPECT_TRUE(params.network_context->http_auth_static_network_context_params
                  ->molt_basic_and_digest_auth_only);
  ASSERT_TRUE(params.network_context->molt_socks_isolation_password);
  EXPECT_EQ(params.network_context->molt_socks_isolation_password->size(),
            32u);
}

// One password per network context: the hook runs once for each context
// ProfileNetworkContextService configures (each MoltNet profile, so each
// MoltNet identity, has its own), and every context gets a new one. The
// network service keeps the value for the context's lifetime, so it is the
// same for every connection of one context.
TEST(MoltNetNetworkContextPolicyTest, EachMoltNetContextGetsItsOwnPassword) {
  Params first_identity;
  Params second_identity;
  ApplyMoltNetNetworkContextSettings(/*is_moltnet_profile=*/true,
                                     *first_identity.network_context,
                                     *first_identity.cert_verifier);
  const std::optional<std::string> first =
      first_identity.network_context->molt_socks_isolation_password;
  ApplyMoltNetNetworkContextSettings(/*is_moltnet_profile=*/true,
                                     *second_identity.network_context,
                                     *second_identity.cert_verifier);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second_identity.network_context->molt_socks_isolation_password);
  EXPECT_NE(*first,
            *second_identity.network_context->molt_socks_isolation_password);
  // Configuring the second context left the first one's alone.
  EXPECT_EQ(first_identity.network_context->molt_socks_isolation_password,
            first);
}

TEST(MoltNetNetworkContextPolicyTest, RegularParamsLeaveThemOff) {
  Params params;
  // The mojom defaults: what every context the hook never touches gets.
  EXPECT_FALSE(params.cert_verifier->molt_isolate_each_cert_fetch);
  EXPECT_FALSE(params.network_context->http_auth_static_network_context_params
                   ->molt_basic_and_digest_auth_only);
  EXPECT_FALSE(params.network_context->molt_socks_isolation_password);

  // And the hook writes them off for any other profile, even over a stale
  // value.
  params.cert_verifier->molt_isolate_each_cert_fetch = true;
  params.network_context->http_auth_static_network_context_params
      ->molt_basic_and_digest_auth_only = true;
  params.network_context->molt_socks_isolation_password = "stale";
  ApplyMoltNetNetworkContextSettings(/*is_moltnet_profile=*/false,
                                     *params.network_context,
                                     *params.cert_verifier);
  EXPECT_FALSE(params.cert_verifier->molt_isolate_each_cert_fetch);
  EXPECT_FALSE(params.network_context->http_auth_static_network_context_params
                   ->molt_basic_and_digest_auth_only);
  // The regular profile has no password of its own.
  EXPECT_FALSE(params.network_context->molt_socks_isolation_password);
}

}  // namespace
}  // namespace molt_ai::tor
