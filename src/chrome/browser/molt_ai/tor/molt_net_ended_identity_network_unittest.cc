// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// An ended MoltNet identity's network context stops what it has under way
// when its refusal is applied, and starts nothing after it
// (services/network/molt_ended_identity_policy.h), on Chromium's real
// ProxyConfigServiceMojo, NetworkContext and SOCKS5ClientSocket.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/address_list.h"
#include "net/base/host_port_pair.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_address.h"
#include "net/base/net_errors.h"
#include "net/base/proxy_server.h"
#include "net/base/test_completion_callback.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_service.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/socket/molt_identity_end.h"
#include "net/socket/socket_test_util.h"
#include "net/socket/socks5_client_socket.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/molt_ended_identity_policy.h"
#include "services/network/network_context.h"
#include "services/network/network_service.h"
#include "services/network/proxy_config_service_mojo.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/proxy_config.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/test/fake_test_cert_verifier_params_factory.h"
#include "services/network/test/test_url_loader_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

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

// Whether the configuration applied refuses everything: what a context made
// after its identity ended starts with.
TEST(MoltNetEndedIdentityNetworkTest, KnowsWhenTheRefusalIsApplied) {
  base::test::TaskEnvironment task_environment;
  mojo::Remote<mojom::ProxyConfigClient> client;
  ProxyConfigServiceMojo born_ended(client.BindNewPipeAndPassReceiver(),
                                    RefuseAll(), mojo::NullRemote());
  EXPECT_TRUE(born_ended.MoltRefusesAll());

  mojo::Remote<mojom::ProxyConfigClient> tor_client;
  ProxyConfigServiceMojo tor(tor_client.BindNewPipeAndPassReceiver(), Tor(),
                             mojo::NullRemote());
  EXPECT_FALSE(tor.MoltRefusesAll());
  tor_client->OnProxyConfigUpdated(RefuseAll());
  tor_client.FlushForTesting();
  EXPECT_TRUE(tor.MoltRefusesAll());

  mojo::Remote<mojom::ProxyConfigClient> pending_client;
  ProxyConfigServiceMojo pending(pending_client.BindNewPipeAndPassReceiver(),
                                 std::nullopt, mojo::NullRemote());
  EXPECT_FALSE(pending.MoltRefusesAll());
}

constexpr char kConnectLocalhost80[] = {
    0x05,  // Version
    0x01,  // CONNECT
    0x00,  // Reserved
    0x03,  // A domain name
    0x09,  // of 9 bytes
    'l',  'o', 'c', 'a', 'l', 'h', 'o', 's', 't',
    0x00, 0x50,  // Port 80
};

std::unique_ptr<net::MockTCPClientSocket> ConnectedTcp(
    net::SocketDataProvider* data) {
  auto tcp = std::make_unique<net::MockTCPClientSocket>(
      net::AddressList::CreateFromIPAddress(net::IPAddress::IPv4Localhost(),
                                            9150),
      nullptr, data);
  net::TestCompletionCallback callback;
  EXPECT_EQ(callback.GetResult(tcp->Connect(callback.callback())), net::OK);
  return tcp;
}

// Once the identity ends, a SOCKS connection to Tor writes nothing more,
// whatever the layer above asks.
TEST(MoltNetEndedIdentityNetworkTest, AnEndedIdentitysConnectionWritesNothing) {
  base::test::TaskEnvironment task_environment;
  const std::string before = "before the end";
  net::MockWrite writes[] = {
      net::MockWrite(net::SYNCHRONOUS, net::kSOCKS5GreetRequest),
      net::MockWrite(net::SYNCHRONOUS, base::as_byte_span(kConnectLocalhost80)),
      net::MockWrite(net::SYNCHRONOUS, before)};
  net::MockRead reads[] = {
      net::MockRead(net::SYNCHRONOUS, net::kSOCKS5GreetResponse),
      net::MockRead(net::SYNCHRONOUS, net::kSOCKS5OkResponse),
      net::MockRead(net::SYNCHRONOUS, net::ERR_IO_PENDING)};
  net::StaticSocketDataProvider data(reads, writes);
  net::SOCKS5ClientSocket socket(ConnectedTcp(&data),
                                 net::HostPortPair("localhost", 80),
                                 TRAFFIC_ANNOTATION_FOR_TESTS);
  auto end = base::MakeRefCounted<net::MoltIdentityEnd>();
  socket.MoltSetIdentityEnd(end);
  net::TestCompletionCallback callback;
  ASSERT_EQ(callback.GetResult(socket.Connect(callback.callback())), net::OK);

  auto first = base::MakeRefCounted<net::StringIOBuffer>(before);
  EXPECT_EQ(callback.GetResult(socket.Write(first.get(), first->size(),
                                            callback.callback(),
                                            TRAFFIC_ANNOTATION_FOR_TESTS)),
            static_cast<int>(before.size()));

  end->Mark();
  auto second = base::MakeRefCounted<net::StringIOBuffer>(std::string("after"));
  EXPECT_EQ(socket.Write(second.get(), second->size(), callback.callback(),
                         TRAFFIC_ANNOTATION_FOR_TESTS),
            net::ERR_ABORTED);
  EXPECT_TRUE(data.AllWriteDataConsumed());
}

// Nor does a SOCKS handshake that hasn't sent its CONNECT yet: Tor opens no
// stream for the identity.
TEST(MoltNetEndedIdentityNetworkTest, AnEndedIdentityOpensNoStream) {
  base::test::TaskEnvironment task_environment;
  net::MockWrite writes[] = {
      net::MockWrite(net::SYNCHRONOUS, net::kSOCKS5GreetRequest)};
  net::MockRead reads[] = {
      net::MockRead(net::SYNCHRONOUS, net::kSOCKS5GreetResponse)};
  net::StaticSocketDataProvider data(reads, writes);
  net::SOCKS5ClientSocket socket(ConnectedTcp(&data),
                                 net::HostPortPair("localhost", 80),
                                 TRAFFIC_ANNOTATION_FOR_TESTS);
  auto end = base::MakeRefCounted<net::MoltIdentityEnd>();
  end->Mark();
  socket.MoltSetIdentityEnd(end);
  net::TestCompletionCallback callback;
  EXPECT_EQ(callback.GetResult(socket.Connect(callback.callback())),
            net::ERR_ABORTED);
  EXPECT_TRUE(data.AllWriteDataConsumed());
}

// A context with MoltNet's setting, on the real network service: from the
// task in which its refusal is applied, it starts no request, also on a URL
// loader factory made afterwards; it still answers proxy lookups.
class MoltNetEndedContextTest : public testing::Test {
 protected:
  std::unique_ptr<NetworkContext> MakeContext(
      net::ProxyConfigWithAnnotation initial) {
    auto params = mojom::NetworkContextParams::New();
    params->cert_verifier_params =
        FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
    params->initial_proxy_config = std::move(initial);
    params->proxy_config_client_receiver =
        proxy_config_client_.BindNewPipeAndPassReceiver();
    params->molt_stop_all_when_refusing = true;
    return std::make_unique<NetworkContext>(
        service_.get(), context_remote_.BindNewPipeAndPassReceiver(),
        std::move(params));
  }

  int Fetch(NetworkContext& context) {
    mojo::Remote<mojom::URLLoaderFactory> factory;
    auto factory_params = mojom::URLLoaderFactoryParams::New();
    factory_params->process_id = OriginatingProcessId::browser();
    factory_params->is_orb_enabled = false;
    context.CreateURLLoaderFactory(factory.BindNewPipeAndPassReceiver(),
                                   std::move(factory_params));
    ResourceRequest request;
    request.url = GURL("https://example.com/");
    request.method = "GET";
    TestURLLoaderClient client;
    mojo::PendingRemote<mojom::URLLoader> loader;
    factory->CreateLoaderAndStart(
        loader.InitWithNewPipeAndPassReceiver(), /*request_id=*/0,
        mojom::kURLLoadOptionNone, request, client.CreateRemote(),
        net::MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS));
    client.RunUntilComplete();
    return client.completion_status().error_code;
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO};
  std::unique_ptr<NetworkService> service_ = NetworkService::CreateForTesting();
  mojo::Remote<mojom::ProxyConfigClient> proxy_config_client_;
  mojo::Remote<mojom::NetworkContext> context_remote_;
};

TEST_F(MoltNetEndedContextTest, StartsNothingFromTheEnd) {
  std::unique_ptr<NetworkContext> context = MakeContext(Tor());
  EXPECT_TRUE(context->MoltAdmits(MoltNetworkEntry::kURLLoader));

  proxy_config_client_->OnProxyConfigUpdated(RefuseAll());
  base::RunLoop flushed;
  proxy_config_client_->FlushProxyConfig(flushed.QuitClosure());
  flushed.Run();

  // Ended by the time the browser hears back on the same pipe.
  EXPECT_FALSE(context->MoltAdmits(MoltNetworkEntry::kURLLoader));
  EXPECT_FALSE(context->MoltAdmits(MoltNetworkEntry::kWebSocket));
  EXPECT_FALSE(context->MoltAdmits(MoltNetworkEntry::kPreconnect));
  EXPECT_TRUE(context->MoltAdmits(MoltNetworkEntry::kProxyLookup));
  EXPECT_EQ(Fetch(*context), net::ERR_ABORTED);
}

TEST_F(MoltNetEndedContextTest, AContextMadeAfterTheEndStartsEnded) {
  std::unique_ptr<NetworkContext> context = MakeContext(RefuseAll());
  EXPECT_FALSE(context->MoltAdmits(MoltNetworkEntry::kURLLoader));
  EXPECT_EQ(Fetch(*context), net::ERR_ABORTED);
}

}  // namespace
}  // namespace network
