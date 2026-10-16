// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/pluggable_transports.h"

#include <optional>

namespace molt_ai {
namespace tor {

namespace {

std::string Parenthesized(std::string_view detail) {
  if (detail.empty()) {
    return std::string();
  }
  return " (" + std::string(detail) + ")";
}

}  // namespace

std::string_view TransportName(BridgeTransport transport) {
  switch (transport) {
    case BridgeTransport::kObfs4:
      return "obfs4";
    case BridgeTransport::kSnowflake:
      return "snowflake";
  }
  return "";
}

std::string_view BundledTransportFileName(BridgeTransport transport) {
  switch (transport) {
    case BridgeTransport::kObfs4:
      return "lyrebird";
    case BridgeTransport::kSnowflake:
      return "snowflake-client";
  }
  return "";
}

std::vector<std::string> LinuxSystemTransportPaths(BridgeTransport transport) {
  switch (transport) {
    case BridgeTransport::kObfs4:
      // Debian/Ubuntu ship lyrebird (and, on older releases or as a
      // transitional name, obfs4proxy); Fedora's "obfs4" package installs
      // obfs4proxy. A symlink between the two is resolved and the target
      // is what gets checked and run.
      return {"/usr/bin/lyrebird", "/usr/bin/obfs4proxy"};
    case BridgeTransport::kSnowflake:
      return {"/usr/bin/snowflake-client"};
  }
  return {};
}

std::vector<BridgeTransport> RequiredTransports(
    const std::vector<std::string>& normalized_bridge_lines) {
  bool obfs4 = false;
  bool snowflake = false;
  for (const std::string& line : normalized_bridge_lines) {
    std::optional<BridgeTransport> transport = GetBridgeTransport(line);
    if (!transport) {
      continue;
    }
    switch (*transport) {
      case BridgeTransport::kObfs4:
        obfs4 = true;
        break;
      case BridgeTransport::kSnowflake:
        snowflake = true;
        break;
    }
  }
  std::vector<BridgeTransport> out;
  if (obfs4) {
    out.push_back(BridgeTransport::kObfs4);
  }
  if (snowflake) {
    out.push_back(BridgeTransport::kSnowflake);
  }
  return out;
}

std::string TransportUnavailableMessage(BridgeTransport transport,
                                        TransportSource source,
                                        bool rejected_existing,
                                        std::string_view detail) {
  const bool obfs4 = transport == BridgeTransport::kObfs4;
  if (source == TransportSource::kBundled) {
    return "This MoltBrowser installation has no usable " +
           std::string(BundledTransportFileName(transport)) + ", which " +
           (obfs4 ? "obfs4" : "snowflake") + " bridges need" +
           Parenthesized(detail) +
           ". Reinstall MoltBrowser or choose another kind of bridge.";
  }
  if (rejected_existing) {
    return obfs4 ? "MoltNet found an obfs4 pluggable transport it won't run" +
                       Parenthesized(detail) +
                       ". Reinstall your distribution's obfs4proxy or "
                       "lyrebird package, then try again."
                 : "MoltNet found a snowflake-client it won't run" +
                       Parenthesized(detail) +
                       ". Reinstall your distribution's snowflake-client "
                       "package, then try again.";
  }
  return obfs4
             ? "obfs4 bridges need the obfs4 pluggable transport, which "
               "isn't installed. Install your distribution's obfs4proxy or "
               "lyrebird package (for example \"sudo apt install "
               "obfs4proxy\" or \"sudo dnf install obfs4\"), then try again."
             : "Snowflake bridges need the snowflake-client pluggable "
               "transport, which isn't installed. Install your "
               "distribution's snowflake-client package (for example "
               "\"sudo apt install snowflake-client\"), then try again.";
}

std::string NoTransportsMessage(TransportSource source) {
  if (source == TransportSource::kBundled) {
    return "This MoltBrowser installation has no pluggable transports, so "
           "bridges can't be used. Reinstall MoltBrowser.";
  }
  return "Install obfs4proxy/snowflake-client to use bridges: obfs4 bridges "
         "need your distribution's obfs4proxy (or lyrebird) package and "
         "snowflake bridges its snowflake-client package (for example "
         "\"sudo apt install obfs4proxy snowflake-client\").";
}

std::string BuiltInBridgesUnavailableMessage(TransportSource source) {
  if (source == TransportSource::kSystemPackages) {
    return "MoltBrowser's Linux packages don't include the built-in bridge "
           "list. Get bridges from bridges.torproject.org (or by email from "
           "bridges@torproject.org) and paste them under Custom bridges.";
  }
  return "This build of MoltBrowser doesn't include the built-in bridge "
         "list. Reinstall MoltBrowser, or get bridges from "
         "bridges.torproject.org and paste them under Custom bridges.";
}

}  // namespace tor
}  // namespace molt_ai
