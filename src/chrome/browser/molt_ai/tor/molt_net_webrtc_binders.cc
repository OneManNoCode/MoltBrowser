// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_webrtc_binders.h"

#include <utility>

#include "services/network/public/mojom/mdns_responder.mojom.h"

#if BUILDFLAG(IS_P2P_ENABLED)
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/unguessable_token.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "net/base/address_family.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/network_interfaces.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/p2p_socket_type.h"
#include "services/network/public/mojom/p2p.mojom.h"
#endif

namespace molt_ai::tor {

namespace {

#if BUILDFLAG(IS_P2P_ENABLED)

// What a MoltNet frame gets instead of the network service's
// P2PSocketManager (services/network/p2p/socket_manager.cc), which resolves
// names with the network context's own host resolver and opens real UDP and
// TCP sockets. Nothing here reaches the network.
class RefusingP2PSocketManager : public network::mojom::P2PSocketManager {
 public:
  RefusingP2PSocketManager() = default;
  RefusingP2PSocketManager(const RefusingP2PSocketManager&) = delete;
  RefusingP2PSocketManager& operator=(const RefusingP2PSocketManager&) =
      delete;
  ~RefusingP2PSocketManager() override = default;

  // network::mojom::P2PSocketManager:
  void StartNetworkNotifications(
      mojo::PendingRemote<network::mojom::P2PNetworkNotificationClient> client)
      override {
    network_notification_client_.reset();
    network_notification_client_.Bind(std::move(client));
    network_notification_client_->NetworkListChanged(
        net::NetworkInterfaceList(), net::IPAddress(), net::IPAddress());
  }

  void GetHostAddress(
      const std::string& host_name,
      std::optional<net::AddressFamily> address_family,
      bool enable_mdns,
      network::mojom::P2PSocketManager::GetHostAddressCallback callback)
      override {
    std::move(callback).Run(std::vector<net::IPAddress>());
  }

  void CreateSocket(
      network::P2PSocketType type,
      const net::IPEndPoint& local_address,
      const network::P2PPortRange& port_range,
      const network::P2PHostAndIPEndPoint& remote_address,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
      const std::optional<base::UnguessableToken>& devtools_token,
      mojo::PendingRemote<network::mojom::P2PSocketClient> client,
      mojo::PendingReceiver<network::mojom::P2PSocket> receiver) override {
    // `client` and `receiver` go out of scope here, closing both pipes.
  }

 private:
  mojo::Remote<network::mojom::P2PNetworkNotificationClient>
      network_notification_client_;
};

void BindRefusingP2PSocketManagerForFrame(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<network::mojom::P2PSocketManager> receiver) {
  BindRefusingP2PSocketManager(std::move(receiver));
}

#endif  // BUILDFLAG(IS_P2P_ENABLED)

void CloseMdnsResponderForFrame(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<network::mojom::MdnsResponder> receiver) {
  receiver.reset();
}

}  // namespace

void ApplyWebRtcNetworkAccessToFrameBinders(
    const WebRtcNetworkAccess& access,
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map) {
  if (!map) {
    return;
  }
#if BUILDFLAG(IS_P2P_ENABLED)
  if (!access.p2p_socket_manager) {
    map->Add<network::mojom::P2PSocketManager>(
        &BindRefusingP2PSocketManagerForFrame);
  }
#endif
  if (!access.mdns_responder) {
    map->Add<network::mojom::MdnsResponder>(&CloseMdnsResponderForFrame);
  }
}

#if BUILDFLAG(IS_P2P_ENABLED)
void BindRefusingP2PSocketManager(
    mojo::PendingReceiver<network::mojom::P2PSocketManager> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<RefusingP2PSocketManager>(),
                              std::move(receiver));
}
#endif

}  // namespace molt_ai::tor
