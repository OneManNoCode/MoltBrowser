// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the P2PSocketManager a network context that makes no sockets
// outside its own HTTP network session (molt_own_session_sockets_policy.h:
// MoltNet's) binds a renderer's pipe to, instead of
// services/network/p2p/socket_manager.cc's, which resolves names with the
// context's host resolver, opens UDP sockets outside the proxy and TCP ones
// through a ProxyResolvingClientSocketFactory. Nothing here reaches the
// network:
//  * GetHostAddress(): an empty list (a failed lookup) for every name;
//  * CreateSocket(): no socket; both of its pipes are closed, which the
//    renderer reports as a socket error;
//  * StartNetworkNotifications(): one empty interface list, with no default
//    addresses.
// The pipe itself stays open: the renderer's P2PSocketDispatcher asks again
// at once whenever it closes. The same as the browser's stand-in for
// MoltNet's frames (chrome/browser/molt_ai/tor/molt_net_webrtc_binders.cc),
// for any request that reaches the network service another way.
//
// Header-only (with out-of-class inline definitions), so the hook in
// NetworkContext needs no new build target.

#ifndef SERVICES_NETWORK_MOLT_REFUSING_P2P_SOCKET_MANAGER_H_
#define SERVICES_NETWORK_MOLT_REFUSING_P2P_SOCKET_MANAGER_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/unguessable_token.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
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

namespace network {

class MoltRefusingP2PSocketManager : public mojom::P2PSocketManager {
 public:
  MoltRefusingP2PSocketManager();
  MoltRefusingP2PSocketManager(const MoltRefusingP2PSocketManager&) = delete;
  MoltRefusingP2PSocketManager& operator=(
      const MoltRefusingP2PSocketManager&) = delete;
  ~MoltRefusingP2PSocketManager() override;

  // Binds `receiver` to a manager that lives as long as the pipe.
  static void Bind(mojo::PendingReceiver<mojom::P2PSocketManager> receiver);

  // mojom::P2PSocketManager:
  void StartNetworkNotifications(
      mojo::PendingRemote<mojom::P2PNetworkNotificationClient> client)
      override;
  void GetHostAddress(
      const std::string& host_name,
      std::optional<net::AddressFamily> address_family,
      bool enable_mdns,
      mojom::P2PSocketManager::GetHostAddressCallback callback) override;
  void CreateSocket(
      P2PSocketType type,
      const net::IPEndPoint& local_address,
      const P2PPortRange& port_range,
      const P2PHostAndIPEndPoint& remote_address,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
      const std::optional<base::UnguessableToken>& devtools_token,
      mojo::PendingRemote<mojom::P2PSocketClient> client,
      mojo::PendingReceiver<mojom::P2PSocket> receiver) override;

 private:
  mojo::Remote<mojom::P2PNetworkNotificationClient>
      network_notification_client_;
};

// ---- Definitions ----

inline MoltRefusingP2PSocketManager::MoltRefusingP2PSocketManager() = default;
inline MoltRefusingP2PSocketManager::~MoltRefusingP2PSocketManager() = default;

// static
inline void MoltRefusingP2PSocketManager::Bind(
    mojo::PendingReceiver<mojom::P2PSocketManager> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<MoltRefusingP2PSocketManager>(),
                              std::move(receiver));
}

inline void MoltRefusingP2PSocketManager::StartNetworkNotifications(
    mojo::PendingRemote<mojom::P2PNetworkNotificationClient> client) {
  network_notification_client_.reset();
  network_notification_client_.Bind(std::move(client));
  network_notification_client_->NetworkListChanged(
      net::NetworkInterfaceList(), net::IPAddress(), net::IPAddress());
}

inline void MoltRefusingP2PSocketManager::GetHostAddress(
    const std::string& host_name,
    std::optional<net::AddressFamily> address_family,
    bool enable_mdns,
    mojom::P2PSocketManager::GetHostAddressCallback callback) {
  std::move(callback).Run(std::vector<net::IPAddress>());
}

inline void MoltRefusingP2PSocketManager::CreateSocket(
    P2PSocketType type,
    const net::IPEndPoint& local_address,
    const P2PPortRange& port_range,
    const P2PHostAndIPEndPoint& remote_address,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
    const std::optional<base::UnguessableToken>& devtools_token,
    mojo::PendingRemote<mojom::P2PSocketClient> client,
    mojo::PendingReceiver<mojom::P2PSocket> receiver) {
  // `client` and `receiver` go out of scope here, closing both pipes.
}

}  // namespace network

#endif  // SERVICES_NETWORK_MOLT_REFUSING_P2P_SOCKET_MANAGER_H_
