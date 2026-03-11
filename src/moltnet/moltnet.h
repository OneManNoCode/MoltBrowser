// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef MOLTNET_MOLTNET_H_
#define MOLTNET_MOLTNET_H_

#include <memory>
#include <string>
#include <vector>

namespace moltnet {

// Connection status
enum class ConnectionStatus {
  DISCONNECTED,
  CONNECTING,
  CONNECTED,
  ERROR
};

// Privacy routing mode
enum class RoutingMode {
  DIRECT,        // No privacy routing (standard connection)
  PROXY,         // Single-hop proxy
  MULTI_HOP,     // Multi-hop relay chain (Tor-like)
  P2P_RELAY      // Peer-to-peer relay network
};

// Circuit/relay information
struct RelayInfo {
  std::string relay_id;
  std::string country_code;
  std::string ip_hint;      // Partial IP for user display
  int latency_ms;
  bool is_exit_node;
};

// Network statistics
struct NetworkStats {
  ConnectionStatus status;
  RoutingMode current_mode;
  int relay_count;
  int estimated_latency_ms;
  std::string apparent_ip;
  std::string apparent_country;
  size_t bytes_sent;
  size_t bytes_received;
  int64_t connected_since;
};

// ============================================================
// MoltNet — Privacy network layer
// ============================================================
// Privacy network enabling IP protection using decentralized
// routing technologies.
//
// Technologies:
//   - Tor integration (SOCKS5 proxy to Tor daemon)
//   - Proxy routing layers
//   - Peer-to-peer relay nodes
//
// Goal: provide strong IP privacy comparable to Tor-level
// anonymity where technically feasible.
//
// Architecture:
//   Browser Network Stack → MoltNet → Tor/Proxy → Internet
//
// Features:
//   - Adaptive routing (choose route based on task)
//   - Garlic bundling (bundle multiple requests)
//   - Uni-tunnels (single-use circuits)
//   - Exit diversity (rotate exit nodes)
// ============================================================
class MoltNet {
 public:
  MoltNet();
  ~MoltNet();

  // ---- Lifecycle ----

  // Initialize MoltNet with configuration directory
  bool Initialize(const std::string& config_dir);

  // Shutdown and close all connections
  void Shutdown();

  // ---- Connection ----

  // Connect to the privacy network
  bool Connect(RoutingMode mode = RoutingMode::MULTI_HOP);

  // Disconnect from the privacy network
  void Disconnect();

  // Get current connection status
  ConnectionStatus GetStatus() const;

  // Get network statistics
  NetworkStats GetNetworkStats() const;

  // ---- Routing ----

  // Set routing mode
  void SetRoutingMode(RoutingMode mode);

  // Get current routing mode
  RoutingMode GetRoutingMode() const;

  // Get current relay chain
  std::vector<RelayInfo> GetCurrentCircuit() const;

  // Request a new circuit (new relay chain)
  bool NewCircuit();

  // ---- Configuration ----

  // Set preferred exit country
  void SetExitCountry(const std::string& country_code);

  // Set SOCKS5 proxy configuration (for Tor integration)
  void SetSOCKS5Proxy(const std::string& host, int port);

  // Get the local proxy address for browser network stack
  std::string GetLocalProxyAddress() const;
  int GetLocalProxyPort() const;

  // ---- Selective Routing ----

  // Route only specific domains through privacy network
  void AddPrivacyDomain(const std::string& domain);

  // Exclude domain from privacy routing
  void AddDirectDomain(const std::string& domain);

  // Check if a domain should be routed through privacy network
  bool ShouldRoutePrivately(const std::string& domain) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace moltnet

#endif  // MOLTNET_MOLTNET_H_
