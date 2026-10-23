// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_network_context_policy.h"

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
}

TEST(MoltNetNetworkContextPolicyTest, MoltNetGetsBoth) {
  const MoltNetNetworkContextSettings settings =
      MoltNetNetworkContextSettingsFor(/*is_moltnet_profile=*/true);
  EXPECT_TRUE(settings.isolate_each_cert_fetch);
  EXPECT_TRUE(settings.basic_and_digest_auth_only);
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
}

TEST(MoltNetNetworkContextPolicyTest, RegularParamsLeaveThemOff) {
  Params params;
  // The mojom defaults: what every context the hook never touches gets.
  EXPECT_FALSE(params.cert_verifier->molt_isolate_each_cert_fetch);
  EXPECT_FALSE(params.network_context->http_auth_static_network_context_params
                   ->molt_basic_and_digest_auth_only);

  // And the hook writes them off for any other profile, even over a stale
  // value.
  params.cert_verifier->molt_isolate_each_cert_fetch = true;
  params.network_context->http_auth_static_network_context_params
      ->molt_basic_and_digest_auth_only = true;
  ApplyMoltNetNetworkContextSettings(/*is_moltnet_profile=*/false,
                                     *params.network_context,
                                     *params.cert_verifier);
  EXPECT_FALSE(params.cert_verifier->molt_isolate_each_cert_fetch);
  EXPECT_FALSE(params.network_context->http_auth_static_network_context_params
                   ->molt_basic_and_digest_auth_only);
}

}  // namespace
}  // namespace molt_ai::tor
