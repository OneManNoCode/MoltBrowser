// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Which pluggable-transport programs MoltNet's Tor may run for bridges, and
// what to tell the user when one is missing. Pure (no //base), so it is
// unit-tested standalone (pluggable_transports_unittest.cc).
//
//   obfs4 bridges     -> lyrebird (Tor's maintained obfs4 client; older
//                        distributions package it as obfs4proxy)
//   snowflake bridges -> snowflake-client
//
// TorManager accepts, in this order (FindHelperBinary, helper_binary.h):
//   1. the copy bundled with MoltBrowser in <tor dir>/pluggable_transports/;
//   2. on Linux only, the distribution's package at one of the fixed paths
//      below, if root owns the file and every directory above it and none
//      of them is group- or world-writable: the same checks as the system
//      tor. Never PATH, /usr/local, /opt or the home directory.
// A bundled transport is written to the torrc relative to Tor's working
// directory; a system one by its canonical absolute path.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_PLUGGABLE_TRANSPORTS_H_
#define CHROME_BROWSER_MOLT_AI_TOR_PLUGGABLE_TRANSPORTS_H_

#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/tor/torrc_builder.h"

namespace molt_ai {
namespace tor {

// The transport's name in torrc and bridge lines: "obfs4" or "snowflake".
std::string_view TransportName(BridgeTransport transport);

// File name of the bundled executable, without a platform suffix (".exe"):
// "lyrebird" or "snowflake-client".
std::string_view BundledTransportFileName(BridgeTransport transport);

// The fixed system paths MoltNet tries on Linux, in order:
//   obfs4:     /usr/bin/lyrebird, /usr/bin/obfs4proxy
//   snowflake: /usr/bin/snowflake-client
std::vector<std::string> LinuxSystemTransportPaths(BridgeTransport transport);

// The transports |normalized_bridge_lines| (NormalizeBridgeLine output)
// need: each at most once, obfs4 before snowflake. Lines of an unknown
// transport are ignored here (BuildTorrc rejects them).
std::vector<BridgeTransport> RequiredTransports(
    const std::vector<std::string>& normalized_bridge_lines);

// Where a platform's transports come from, for the user-facing text below.
enum class TransportSource {
  // Linux: the distribution's packages (MoltBrowser doesn't bundle them).
  kSystemPackages,
  // macOS / Windows: only the copy bundled with MoltBrowser.
  kBundled,
};

// User-facing explanation (one or two sentences, ending with a period) of
// why bridges of |transport| can't be used and what to do.
// |rejected_existing|: a candidate exists but failed the ownership or
// permission checks; |detail| is that refusal (or "not found"), shown only
// then and for bundled builds.
std::string TransportUnavailableMessage(BridgeTransport transport,
                                        TransportSource source,
                                        bool rejected_existing,
                                        std::string_view detail);

// Shown in settings when no transport at all is usable.
std::string NoTransportsMessage(TransportSource source);

// Why the built-in snowflake / obfs4 bridge list (Tor Browser's
// pt_config.json) can't be used.
std::string BuiltInBridgesUnavailableMessage(TransportSource source);

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_PLUGGABLE_TRANSPORTS_H_
