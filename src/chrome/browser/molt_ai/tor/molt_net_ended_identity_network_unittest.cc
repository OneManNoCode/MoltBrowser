// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// An ended MoltNet identity's network context stops what it has under way
// when its refusal is applied (services/network/molt_ended_identity_policy.h),
// on Chromium's real ProxyConfigServiceMojo.

#include <memory>
#include <optional>
#include <vector>

#include "base/test/task_environment.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/proxy_server.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_service.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/molt_ended_identity_policy.h"
#include "services/network/proxy_config_service_mojo.h"
#include "services/network/public/mojom/proxy_config.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace network {
namespace {

// What MoltNetRouting::RefuseAllProxyConfig() sends.
net::ProxyConfigWithAnnotation RefuseAll() {
  net::ProxyConfig config;
  config.proxy_rules().type = net::ProxyConfig::ProxyRules::Type::PROXY_LIST;
  config.proxy_rules().single_proxies.Clear();
  config.proxy_rules().bypass_rules.AddRulesToSubtractImplicit();
  return net::ProxyConfigWithAnnotation(config, TRAFFIC_ANNOTATION_FOR_TESTS);
}

// MoltNet's own: SOCKS5 to Tor.
net::ProxyConfigWithAnnotation Tor(int port = 9150) {
  net::ProxyConfig config;
  config.proxy_rules().ParseFromString("socks5://127.0.0.1:" +
                                       std::to_string(port));
  return net::ProxyConfigWithAnnotation(config, TRAFFIC_ANNOTATION_FOR_TESTS);
}

class Observer : public net::ProxyConfigService::Observer {
 public:
  void OnProxyConfigChanged(
      const net::ProxyConfigWithAnnotation& config,
      net::ProxyConfigService::ConfigAvailability) override {
    configs.push_back(config);
  }
  std::vector<net::ProxyConfigWithAnnotation> configs;
};

TEST(MoltNetEndedIdentityNetworkTest, OnlyARefusalOfEverythingCounts) {
  EXPECT_TRUE(MoltIsRefuseAllProxyConfig(true, true, false));
  EXPECT_FALSE(MoltIsRefuseAllProxyConfig(true, false, false));
  EXPECT_FALSE(MoltIsRefuseAllProxyConfig(false, true, false));
  EXPECT_FALSE(MoltIsRefuseAllProxyConfig(true, true, true));
  EXPECT_TRUE(MoltStopsEverythingOnConfig(true, true, true));
  EXPECT_FALSE(MoltStopsEverythingOnConfig(false, true, true));
  EXPECT_FALSE(MoltStopsEverythingOnConfig(true, false, true));
  EXPECT_FALSE(MoltStopsEverythingOnConfig(true, true, false));
}

// The refusal stops everything once, after every observer (the proxy
// resolution service among them) has it; Tor's configuration, and the same
// refusal again, stop nothing.
TEST(MoltNetEndedIdentityNetworkTest, TheRefusalStopsEverythingOnce) {
  base::test::TaskEnvironment task_environment;
  mojo::Remote<mojom::ProxyConfigClient> client;
  ProxyConfigServiceMojo service(client.BindNewPipeAndPassReceiver(), Tor(),
                                 mojo::NullRemote());
  Observer observer;
  service.AddObserver(&observer);
  int stops = 0;
  size_t observed_at_stop = 0;
  service.MoltSetOnRefuseAll(base::BindRepeating(
      [](int* stops, size_t* observed_at_stop, Observer* observer) {
        ++*stops;
        *observed_at_stop = observer->configs.size();
      },
      &stops, &observed_at_stop, &observer));

  client->OnProxyConfigUpdated(Tor(9151));
  client.FlushForTesting();
  EXPECT_EQ(stops, 0);

  client->OnProxyConfigUpdated(RefuseAll());
  client.FlushForTesting();
  EXPECT_EQ(stops, 1);
  EXPECT_EQ(observed_at_stop, 2u);
  ASSERT_EQ(observer.configs.size(), 2u);
  EXPECT_TRUE(observer.configs.back().value().Equals(RefuseAll().value()));

  client->OnProxyConfigUpdated(RefuseAll());
  client.FlushForTesting();
  EXPECT_EQ(stops, 1);
  service.RemoveObserver(&observer);
}

// A context that starts with the refusal (a new context of an ended
// identity) has nothing to stop; one without the callback stops nothing.
TEST(MoltNetEndedIdentityNetworkTest, NothingToStopOtherwise) {
  base::test::TaskEnvironment task_environment;
  mojo::Remote<mojom::ProxyConfigClient> client;
  ProxyConfigServiceMojo service(client.BindNewPipeAndPassReceiver(),
                                 RefuseAll(), mojo::NullRemote());
  int stops = 0;
  service.MoltSetOnRefuseAll(
      base::BindRepeating([](int* stops) { ++*stops; }, &stops));
  client->OnProxyConfigUpdated(RefuseAll());
  client.FlushForTesting();
  EXPECT_EQ(stops, 0);

  mojo::Remote<mojom::ProxyConfigClient> other_client;
  ProxyConfigServiceMojo other(other_client.BindNewPipeAndPassReceiver(),
                               Tor(), mojo::NullRemote());
  other_client->OnProxyConfigUpdated(RefuseAll());
  other_client.FlushForTesting();
  net::ProxyConfigWithAnnotation applied;
  EXPECT_EQ(other.GetLatestProxyConfig(&applied),
            net::ProxyConfigService::CONFIG_VALID);
  EXPECT_TRUE(applied.value().Equals(RefuseAll().value()));
}

}  // namespace
}  // namespace network
