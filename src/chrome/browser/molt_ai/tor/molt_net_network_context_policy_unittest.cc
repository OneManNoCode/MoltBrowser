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
  EXPECT_FALSE(settings.uncached_cert_verification);
  EXPECT_FALSE(settings.no_shared_cache_entries);
  EXPECT_FALSE(settings.hsts_top_level_navigations_only);
  EXPECT_FALSE(settings.split_http_auth_cache);
  EXPECT_FALSE(settings.no_network_quality_estimator);
  EXPECT_FALSE(settings.block_private_state_tokens);
  EXPECT_FALSE(settings.send_no_client_certificates);
  EXPECT_FALSE(settings.no_doh_probes);
  EXPECT_FALSE(settings.no_device_bound_sessions);
  EXPECT_EQ(settings.per_site_socket_pool_size, 0u);
  EXPECT_FALSE(settings.per_site_keepalive_limits);
}

TEST(MoltNetNetworkContextPolicyTest, MoltNetGetsAll) {
  const MoltNetNetworkContextSettings settings =
      MoltNetNetworkContextSettingsFor(/*is_moltnet_profile=*/true);
  EXPECT_TRUE(settings.isolate_each_cert_fetch);
  EXPECT_TRUE(settings.basic_and_digest_auth_only);
  EXPECT_TRUE(settings.fresh_socks_isolation_password);
  EXPECT_TRUE(settings.uncached_cert_verification);
  EXPECT_TRUE(settings.no_shared_cache_entries);
  EXPECT_TRUE(settings.hsts_top_level_navigations_only);
  EXPECT_TRUE(settings.split_http_auth_cache);
  EXPECT_TRUE(settings.no_network_quality_estimator);
  EXPECT_TRUE(settings.block_private_state_tokens);
  EXPECT_TRUE(settings.send_no_client_certificates);
  EXPECT_TRUE(settings.no_doh_probes);
  EXPECT_TRUE(settings.no_device_bound_sessions);
  // Each site's own fixed limit (net/socket/
  // molt_per_site_socket_pool_policy.h).
  EXPECT_EQ(settings.per_site_socket_pool_size, 48u);
  EXPECT_TRUE(settings.per_site_keepalive_limits);
}

// Private State Tokens: ProfileNetworkContextService asks this when it
// creates a network context and when the anti-abuse content setting
// changes. MoltNet always blocks them; every other profile gets exactly
// Chromium's answer.
TEST(MoltNetNetworkContextPolicyTest, PrivateStateTokensBlockedInMoltNet) {
  EXPECT_TRUE(BlockPrivateStateTokensFor(/*is_moltnet_profile=*/true,
                                         /*chromium_value=*/false));
  EXPECT_TRUE(BlockPrivateStateTokensFor(/*is_moltnet_profile=*/true,
                                         /*chromium_value=*/true));
  EXPECT_FALSE(BlockPrivateStateTokensFor(/*is_moltnet_profile=*/false,
                                          /*chromium_value=*/false));
  EXPECT_TRUE(BlockPrivateStateTokensFor(/*is_moltnet_profile=*/false,
                                         /*chromium_value=*/true));
}

// Device-bound sessions: never in MoltNet; every other profile gets exactly
// Chromium's answer (the feature's state).
TEST(MoltNetNetworkContextPolicyTest, NoDeviceBoundSessionsInMoltNet) {
  EXPECT_FALSE(DeviceBoundSessionsEnabledFor(/*is_moltnet_profile=*/true,
                                             /*chromium_value=*/true));
  EXPECT_FALSE(DeviceBoundSessionsEnabledFor(/*is_moltnet_profile=*/true,
                                             /*chromium_value=*/false));
  EXPECT_TRUE(DeviceBoundSessionsEnabledFor(/*is_moltnet_profile=*/false,
                                            /*chromium_value=*/true));
  EXPECT_FALSE(DeviceBoundSessionsEnabledFor(/*is_moltnet_profile=*/false,
                                             /*chromium_value=*/false));
  static_assert(!DeviceBoundSessionsEnabledFor(true, true));
  static_assert(DeviceBoundSessionsEnabledFor(false, true));
}

// The browser's network-quality estimate and the OS connection type: fixed
// values for MoltNet's renderers, the real ones for every other profile's.
TEST(MoltNetNetworkContextPolicyTest, UniformNetworkStateForMoltNetOnly) {
  EXPECT_TRUE(SendUniformNetworkStateToRenderers(/*is_moltnet_profile=*/true));
  EXPECT_FALSE(
      SendUniformNetworkStateToRenderers(/*is_moltnet_profile=*/false));
  static_assert(SendUniformNetworkStateToRenderers(true));
  static_assert(!SendUniformNetworkStateToRenderers(false));
}

// Downloads keep the network isolation of the frame or download they come
// from in MoltNet; every other profile keeps Chromium's.
TEST(MoltNetNetworkContextPolicyTest, DownloadsKeptWithTheirSiteInMoltNetOnly) {
  static_assert(KeepDownloadsWithTheirSite(/*is_moltnet_profile=*/true));
  static_assert(!KeepDownloadsWithTheirSite(/*is_moltnet_profile=*/false));
  EXPECT_TRUE(KeepDownloadsWithTheirSite(true));
  EXPECT_FALSE(KeepDownloadsWithTheirSite(false));
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
  static_assert(MoltNetNetworkContextSettingsFor(true).split_http_auth_cache);
  static_assert(
      MoltNetNetworkContextSettingsFor(true).no_network_quality_estimator);
  static_assert(
      !MoltNetNetworkContextSettingsFor(false).no_network_quality_estimator);
  static_assert(!BlockPrivateStateTokensFor(false, false));
  static_assert(
      MoltNetNetworkContextSettingsFor(true).send_no_client_certificates);
  static_assert(
      !MoltNetNetworkContextSettingsFor(false).send_no_client_certificates);
  static_assert(MoltNetNetworkContextSettingsFor(true).no_doh_probes);
  static_assert(!MoltNetNetworkContextSettingsFor(false).no_doh_probes);
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
  EXPECT_TRUE(params.network_context->molt_uncached_cert_verification);
  EXPECT_TRUE(params.network_context->molt_no_shared_cache_entries);
  EXPECT_TRUE(params.network_context->molt_hsts_top_level_navigations_only);
  EXPECT_TRUE(params.network_context->molt_no_network_quality_estimator);
  EXPECT_TRUE(params.network_context->molt_send_no_client_certificates);
  EXPECT_TRUE(params.network_context->molt_no_doh_probes);
  EXPECT_EQ(params.network_context->molt_per_site_socket_pool_size, 48u);
  EXPECT_TRUE(params.network_context->molt_per_site_keepalive_limits);
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
  // And the new identity's context gets every per-site setting itself:
  // nothing of it is carried over from the earlier context, whose state
  // goes away with it (New Identity destroys the old profile, and its
  // network context, before the new one is made).
  EXPECT_TRUE(
      second_identity.network_context->molt_uncached_cert_verification);
  EXPECT_TRUE(second_identity.network_context->molt_no_shared_cache_entries);
  EXPECT_TRUE(
      second_identity.network_context->molt_hsts_top_level_navigations_only);
  EXPECT_TRUE(
      second_identity.network_context->molt_no_network_quality_estimator);
  EXPECT_TRUE(
      second_identity.network_context->molt_send_no_client_certificates);
  EXPECT_TRUE(second_identity.network_context->molt_no_doh_probes);
  EXPECT_EQ(second_identity.network_context->molt_per_site_socket_pool_size,
            48u);
  EXPECT_TRUE(second_identity.network_context->molt_per_site_keepalive_limits);
  EXPECT_TRUE(second_identity.cert_verifier->molt_isolate_each_cert_fetch);
}

TEST(MoltNetNetworkContextPolicyTest, RegularParamsLeaveThemOff) {
  Params params;
  // The mojom defaults: what every context the hook never touches gets.
  EXPECT_FALSE(params.cert_verifier->molt_isolate_each_cert_fetch);
  EXPECT_FALSE(params.network_context->http_auth_static_network_context_params
                   ->molt_basic_and_digest_auth_only);
  EXPECT_FALSE(params.network_context->molt_socks_isolation_password);
  EXPECT_FALSE(params.network_context->molt_uncached_cert_verification);
  EXPECT_FALSE(params.network_context->molt_no_shared_cache_entries);
  EXPECT_FALSE(params.network_context->molt_hsts_top_level_navigations_only);
  EXPECT_FALSE(params.network_context->molt_no_network_quality_estimator);
  EXPECT_FALSE(params.network_context->molt_send_no_client_certificates);
  EXPECT_FALSE(params.network_context->molt_no_doh_probes);
  EXPECT_EQ(params.network_context->molt_per_site_socket_pool_size, 0u);
  EXPECT_FALSE(params.network_context->molt_per_site_keepalive_limits);

  // And the hook writes them off for any other profile, even over a stale
  // value.
  params.cert_verifier->molt_isolate_each_cert_fetch = true;
  params.network_context->http_auth_static_network_context_params
      ->molt_basic_and_digest_auth_only = true;
  params.network_context->molt_socks_isolation_password = "stale";
  params.network_context->molt_uncached_cert_verification = true;
  params.network_context->molt_no_shared_cache_entries = true;
  params.network_context->molt_hsts_top_level_navigations_only = true;
  params.network_context->molt_no_network_quality_estimator = true;
  params.network_context->molt_send_no_client_certificates = true;
  params.network_context->molt_no_doh_probes = true;
  params.network_context->molt_per_site_socket_pool_size = 7;
  params.network_context->molt_per_site_keepalive_limits = true;
  ApplyMoltNetNetworkContextSettings(/*is_moltnet_profile=*/false,
                                     *params.network_context,
                                     *params.cert_verifier);
  EXPECT_FALSE(params.cert_verifier->molt_isolate_each_cert_fetch);
  EXPECT_FALSE(params.network_context->http_auth_static_network_context_params
                   ->molt_basic_and_digest_auth_only);
  // The regular profile has no password of its own.
  EXPECT_FALSE(params.network_context->molt_socks_isolation_password);
  EXPECT_FALSE(params.network_context->molt_uncached_cert_verification);
  EXPECT_FALSE(params.network_context->molt_no_shared_cache_entries);
  EXPECT_FALSE(params.network_context->molt_hsts_top_level_navigations_only);
  EXPECT_FALSE(params.network_context->molt_no_network_quality_estimator);
  EXPECT_FALSE(params.network_context->molt_send_no_client_certificates);
  EXPECT_FALSE(params.network_context->molt_no_doh_probes);
  EXPECT_EQ(params.network_context->molt_per_site_socket_pool_size, 0u);
  EXPECT_FALSE(params.network_context->molt_per_site_keepalive_limits);
}

}  // namespace
}  // namespace molt_ai::tor
