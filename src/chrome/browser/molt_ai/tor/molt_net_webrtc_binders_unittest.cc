// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_webrtc_binders.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "chrome/common/molt_ai/tor/molt_net_webrtc_policy.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/message_pipe.h"
#include "services/network/public/cpp/network_service_buildflags.h"
#include "services/network/public/mojom/mdns_responder.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_P2P_ENABLED)
#include <optional>
#include <string>

#include "base/test/test_future.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "net/base/address_family.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/network_interfaces.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/public/cpp/p2p_socket_type.h"
#include "services/network/public/mojom/p2p.mojom.h"
#endif

namespace content {
class RenderFrameHost;
}  // namespace content

namespace molt_ai::tor {
namespace {

using FrameBinderMap = mojo::BinderMapWithContext<content::RenderFrameHost*>;

// Stand-ins for content's own binders (BindSocketManager and
// RenderFrameHostImpl::CreateMdnsResponder): they count, and keep the pipe.
int g_upstream_p2p_binds = 0;
int g_upstream_mdns_binds = 0;
std::vector<mojo::ScopedMessagePipeHandle>* g_kept_pipes = nullptr;

#if BUILDFLAG(IS_P2P_ENABLED)
void UpstreamP2PBinder(
    content::RenderFrameHost*,
    mojo::PendingReceiver<network::mojom::P2PSocketManager> receiver) {
  ++g_upstream_p2p_binds;
  g_kept_pipes->push_back(receiver.PassPipe());
}
#endif

void UpstreamMdnsBinder(
    content::RenderFrameHost*,
    mojo::PendingReceiver<network::mojom::MdnsResponder> receiver) {
  ++g_upstream_mdns_binds;
  g_kept_pipes->push_back(receiver.PassPipe());
}

class MoltNetWebRtcBindersTest : public testing::Test {
 protected:
  MoltNetWebRtcBindersTest() {
    g_upstream_p2p_binds = 0;
    g_upstream_mdns_binds = 0;
    g_kept_pipes = &kept_pipes_;
    // What content registers for an ordinary frame.
#if BUILDFLAG(IS_P2P_ENABLED)
    map_.Add<network::mojom::P2PSocketManager>(&UpstreamP2PBinder);
#endif
    map_.Add<network::mojom::MdnsResponder>(&UpstreamMdnsBinder);
  }
  ~MoltNetWebRtcBindersTest() override { g_kept_pipes = nullptr; }

  template <typename Interface>
  void Bind(mojo::Remote<Interface>& remote) {
    mojo::GenericPendingReceiver receiver(remote.BindNewPipeAndPassReceiver());
    ASSERT_TRUE(map_.TryBind(/*context=*/nullptr, &receiver));
  }

  base::test::TaskEnvironment task_environment_;
  std::vector<mojo::ScopedMessagePipeHandle> kept_pipes_;
  FrameBinderMap map_;
};

TEST_F(MoltNetWebRtcBindersTest, RegularProfilesKeepContentsBinders) {
  ApplyWebRtcNetworkAccessToFrameBinders(
      WebRtcNetworkAccessForProfile(/*is_moltnet_profile=*/false), &map_);
  mojo::Remote<network::mojom::MdnsResponder> responder;
  Bind(responder);
  EXPECT_EQ(g_upstream_mdns_binds, 1);
#if BUILDFLAG(IS_P2P_ENABLED)
  mojo::Remote<network::mojom::P2PSocketManager> manager;
  Bind(manager);
  EXPECT_EQ(g_upstream_p2p_binds, 1);
#endif
}

TEST_F(MoltNetWebRtcBindersTest, MoltNetClosesTheMdnsResponder) {
  ApplyWebRtcNetworkAccessToFrameBinders(
      WebRtcNetworkAccessForProfile(/*is_moltnet_profile=*/true), &map_);
  mojo::Remote<network::mojom::MdnsResponder> responder;
  Bind(responder);
  base::RunLoop closed;
  responder.set_disconnect_handler(closed.QuitClosure());
  closed.Run();
  EXPECT_FALSE(responder.is_connected());
  EXPECT_EQ(g_upstream_mdns_binds, 0);
}

TEST_F(MoltNetWebRtcBindersTest, MoltNetFrameWithoutContentBindersStillBinds) {
  // Fenced frames (and Connection-Allowlist pages) get no WebRTC binders
  // from content; a MoltNet renderer that asks anyway gets the refusing
  // ones instead of being killed.
  FrameBinderMap empty;
  ApplyWebRtcNetworkAccessToFrameBinders(
      WebRtcNetworkAccessForProfile(/*is_moltnet_profile=*/true), &empty);
  EXPECT_TRUE(empty.Contains<network::mojom::MdnsResponder>());
#if BUILDFLAG(IS_P2P_ENABLED)
  EXPECT_TRUE(empty.Contains<network::mojom::P2PSocketManager>());
#endif
}

#if BUILDFLAG(IS_P2P_ENABLED)

class FakeNetworkNotificationClient
    : public network::mojom::P2PNetworkNotificationClient {
 public:
  explicit FakeNetworkNotificationClient(base::OnceClosure on_list)
      : on_list_(std::move(on_list)) {}

  mojo::PendingRemote<network::mojom::P2PNetworkNotificationClient>
  BindNewPipeAndPassRemote() {
    return receiver_.BindNewPipeAndPassRemote();
  }

  // network::mojom::P2PNetworkNotificationClient:
  void NetworkListChanged(const std::vector<net::NetworkInterface>& networks,
                          const net::IPAddress& default_ipv4_local_address,
                          const net::IPAddress& default_ipv6_local_address)
      override {
    ++lists_;
    networks_ = networks.size();
    ipv4_empty_ = default_ipv4_local_address.empty();
    ipv6_empty_ = default_ipv6_local_address.empty();
    if (on_list_) {
      std::move(on_list_).Run();
    }
  }

  int lists_ = 0;
  size_t networks_ = 1;
  bool ipv4_empty_ = false;
  bool ipv6_empty_ = false;

 private:
  base::OnceClosure on_list_;
  mojo::Receiver<network::mojom::P2PNetworkNotificationClient> receiver_{
      this};
};

class MoltNetRefusingP2PSocketManagerTest : public MoltNetWebRtcBindersTest {
 protected:
  void SetUp() override {
    ApplyWebRtcNetworkAccessToFrameBinders(
        WebRtcNetworkAccessForProfile(/*is_moltnet_profile=*/true), &map_);
    Bind(manager_);
    manager_.set_disconnect_handler(
        base::BindOnce([](bool* closed) { *closed = true; },
                       &manager_closed_));
  }

  void TearDown() override {
    // The renderer's P2PSocketDispatcher reconnects whenever the manager's
    // pipe closes; it must stay open, and content's binder is never used.
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(manager_closed_);
    EXPECT_EQ(g_upstream_p2p_binds, 0);
  }

  // Creates a socket as IpcPacketSocketFactory does and returns once the
  // manager has closed both of its ends.
  void ExpectSocketRefused(network::P2PSocketType type,
                           const network::P2PHostAndIPEndPoint& remote) {
    mojo::PendingRemote<network::mojom::P2PSocketClient> client;
    mojo::PendingReceiver<network::mojom::P2PSocketClient> client_end =
        client.InitWithNewPipeAndPassReceiver();
    mojo::Remote<network::mojom::P2PSocket> socket;
    mojo::PendingReceiver<network::mojom::P2PSocket> socket_end =
        socket.BindNewPipeAndPassReceiver();
    base::RunLoop closed;
    socket.set_disconnect_handler(closed.QuitClosure());
    manager_->CreateSocket(
        type, net::IPEndPoint(net::IPAddress::IPv4AllZeros(), 0),
        network::P2PPortRange(), remote,
        net::MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS),
        /*devtools_token=*/std::nullopt, std::move(client),
        std::move(socket_end));
    closed.Run();
    EXPECT_FALSE(socket.is_connected());
    mojo::ScopedMessagePipeHandle client_pipe = client_end.PassPipe();
    EXPECT_TRUE(client_pipe->QuerySignalsState().peer_closed());
  }

  mojo::Remote<network::mojom::P2PSocketManager> manager_;
  bool manager_closed_ = false;
};

TEST_F(MoltNetRefusingP2PSocketManagerTest, ResolvesNoName) {
  // A TURN server or remote candidate by name, an mDNS candidate, a name
  // with and without the trailing dot, and an IP literal: nothing is
  // resolved, so nothing reaches a DNS server.
  for (const char* name :
       {"unique-1234.attacker.example", "unique-1234.attacker.example.",
        "0a1b2c3d-1111-2222-3333-444455556666.local", "localhost",
        "192.0.2.1", ""}) {
    for (bool enable_mdns : {false, true}) {
      SCOPED_TRACE(testing::Message() << name << " mdns=" << enable_mdns);
      base::test::TestFuture<const std::vector<net::IPAddress>&> addresses;
      manager_->GetHostAddress(name, /*address_family=*/std::nullopt,
                               enable_mdns, addresses.GetCallback());
      EXPECT_TRUE(addresses.Get().empty());
    }
  }
  base::test::TestFuture<const std::vector<net::IPAddress>&> v6;
  manager_->GetHostAddress("turn.attacker.example",
                           net::ADDRESS_FAMILY_IPV6, /*enable_mdns=*/false,
                           v6.GetCallback());
  EXPECT_TRUE(v6.Get().empty());
}

TEST_F(MoltNetRefusingP2PSocketManagerTest, OpensNoSocket) {
  const network::P2PHostAndIPEndPoint turn_by_name(
      "turn.attacker.example", net::IPEndPoint(net::IPAddress(), 3478));
  const network::P2PHostAndIPEndPoint turn_by_ip(
      std::string(), net::IPEndPoint(net::IPAddress(192, 0, 2, 1), 3478));
  ExpectSocketRefused(network::P2P_SOCKET_UDP,
                      network::P2PHostAndIPEndPoint());
  ExpectSocketRefused(network::P2P_SOCKET_UDP, turn_by_ip);
  ExpectSocketRefused(network::P2P_SOCKET_STUN_TCP_CLIENT, turn_by_name);
  ExpectSocketRefused(network::P2P_SOCKET_STUN_TCP_CLIENT, turn_by_ip);
  ExpectSocketRefused(network::P2P_SOCKET_STUN_TLS_CLIENT, turn_by_name);
  ExpectSocketRefused(network::P2P_SOCKET_TCP_CLIENT, turn_by_ip);
}

TEST_F(MoltNetRefusingP2PSocketManagerTest, ReportsNoNetworkInterface) {
  base::RunLoop got_list;
  FakeNetworkNotificationClient client(got_list.QuitClosure());
  manager_->StartNetworkNotifications(client.BindNewPipeAndPassRemote());
  got_list.Run();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(client.lists_, 1);
  EXPECT_EQ(client.networks_, 0u);
  EXPECT_TRUE(client.ipv4_empty_);
  EXPECT_TRUE(client.ipv6_empty_);
}

TEST_F(MoltNetRefusingP2PSocketManagerTest, EachFrameGetsItsOwnManager) {
  mojo::Remote<network::mojom::P2PSocketManager> second;
  Bind(second);
  base::test::TestFuture<const std::vector<net::IPAddress>&> addresses;
  second->GetHostAddress("unique-5678.attacker.example", std::nullopt,
                         /*enable_mdns=*/true, addresses.GetCallback());
  EXPECT_TRUE(addresses.Get().empty());
  // Closing one doesn't affect the other.
  second.reset();
  base::test::TestFuture<const std::vector<net::IPAddress>&> still;
  manager_->GetHostAddress("unique-5678.attacker.example", std::nullopt,
                           /*enable_mdns=*/false, still.GetCallback());
  EXPECT_TRUE(still.Get().empty());
}

#endif  // BUILDFLAG(IS_P2P_ENABLED)

}  // namespace
}  // namespace molt_ai::tor
