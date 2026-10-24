// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Pure torrc generation + validation for MoltNet's managed Tor.
//
// Every value spliced into the torrc goes through a validator here, because a
// torrc line is a trust boundary: a stray newline in a path or a pasted
// bridge line would let it inject arbitrary Tor options (e.g. an extra
// SocksPort on 0.0.0.0, or a ControlPort without auth). Unit-tested in
// torrc_builder_unittest.cc.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_TORRC_BUILDER_H_
#define CHROME_BROWSER_MOLT_AI_TOR_TORRC_BUILDER_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// MoltNet's connection limits (std-only), which Tor's own must fit.
#include "net/socket/molt_per_site_socket_pool_policy.h"

namespace molt_ai {
namespace tor {

// Tor's capacity, as MoltNet's torrc sets it (all of MoltNet's sites share
// its one Tor process; net/socket/molt_per_site_socket_pool_policy.h keeps
// MoltNet's side within it):
//
//  * ConnLimit: Tor refuses to start unless it may open this many file
//    descriptors, and keeps 32 of them for itself; MoltNet states Tor's
//    default, the floor its connection budget is sized against. Tor itself
//    raises its descriptor limit as far as the system allows.
inline constexpr size_t kTorConnLimit = 1000;
static_assert(kTorConnLimit == net::kMoltTorConnLimit);
//  * MaxClientCircuitsPending: how many circuits Tor builds for streams at
//    a time, for all sites together; a stream that needs a new circuit
//    beyond it waits. Twice Tor's default (32), so that no one site can
//    fill it: a site has at most kMoltSocksHandshakesPerSite handshakes in
//    progress, and a connection needs at most three new circuits at a time
//    (an onion service's descriptor, introduction and rendezvous circuits),
//    with the one-off certificate fetches' handshakes on top.
inline constexpr size_t kTorMaxClientCircuitsPending = 64;
inline constexpr size_t kTorNewCircuitsPerConnection = 3;
static_assert(net::kMoltSocksHandshakesPerSite * kTorNewCircuitsPerConnection +
                  net::kMoltUnattributedSocksHandshakes <
              kTorMaxClientCircuitsPending);

// Pluggable transports MoltNet knows how to drive.
enum class BridgeTransport {
  kObfs4,      // via lyrebird
  kSnowflake,  // via snowflake-client
};

struct TorrcOptions {
  TorrcOptions();
  TorrcOptions(const TorrcOptions&);
  TorrcOptions& operator=(const TorrcOptions&);
  ~TorrcOptions();

  // Absolute paths (UTF-8). Required.
  std::string data_dir;
  std::string cookie_file;
  std::string control_port_file;
  std::string log_file;
  // Absolute paths to the bundled GeoIP DBs. Optional ("" = omit).
  std::string geoip_file;
  std::string geoip6_file;

  // Exit-country constraint: "" (any) or a lowercase ISO 3166-1 alpha-2 code.
  // When set, StrictNodes is enabled so Tor refuses rather than silently
  // using another country.
  std::string exit_country;

  // Validated, normalized bridge lines (see NormalizeBridgeLine), without the
  // "Bridge " keyword. Empty = connect directly to the Tor network.
  std::vector<std::string> bridge_lines;

  // Executables for the pluggable transports, as Tor should exec them (see
  // IsValidTransportExec): a bundled transport RELATIVE to Tor's working
  // directory ("./pluggable_transports/lyrebird"; TorManager launches tor
  // with its cwd set to the bundled tor dir), or, on Linux, the canonical
  // absolute path of a root-owned system package ("/usr/bin/lyrebird",
  // pluggable_transports.h). Tor splits the ClientTransportPlugin line on
  // spaces, so an absolute path under e.g. "Application Support" would
  // break; that is why bundled ones are relative. "" = no usable transport.
  std::string obfs4_exec;      // lyrebird / obfs4proxy
  std::string snowflake_exec;  // snowflake-client
};

// Renders the managed torrc. Returns nullopt (and sets |error|) if any field
// is unsafe to place in a torrc line or a bridge needs a transport that has
// no valid exec path. Ports are always `auto` (Tor picks free ports; MoltNet reads them
// back from ControlPortWriteToFile / GETINFO), so a squatter on 9050/9051 can
// never intercept MoltNet traffic.
std::optional<std::string> BuildTorrc(const TorrcOptions& options,
                                      std::string* error);

// True iff |cc| is exactly two lowercase ASCII letters.
bool IsValidExitCountry(std::string_view cc);

// True iff |value| can be the value of a torrc line verbatim: non-empty, no
// control characters, no '#' (starts a comment), no leading quote (would be
// parsed as a QuotedString), no leading/trailing whitespace, and no trailing
// backslash (line continuation).
bool IsSafeTorrcValue(std::string_view value);

// True iff |exec| may follow "ClientTransportPlugin <transport> exec": a
// safe torrc value without blanks (Tor splits the line on them) that is
// either
//   * a canonical absolute POSIX path ("/usr/bin/lyrebird": no empty, "."
//     or ".." components, no trailing slash, no backslash), or
//   * a path relative to Tor's working directory that starts with a
//     directory ("./pluggable_transports/lyrebird",
//     "pluggable_transports\\lyrebird.exe"; '/' or '\\' separators; only
//     the first component may be "."; no ".." or empty components; no
//     drive letter), so it never names a bare program that could be looked
//     up on PATH.
bool IsValidTransportExec(std::string_view exec);

// Validates one bridge line (optionally prefixed with "Bridge ") and returns
// it normalized: single spaces, uppercase fingerprint. Accepted forms:
//   obfs4 <IPv4|[IPv6]>:<port> <40-hex fingerprint> cert=<base64> iat-mode=<0-2>
//   snowflake <IPv4|[IPv6]>:<port> <40-hex fingerprint> [key=value ...]
// Keys must match [a-z][a-z0-9_-]* and values a conservative URL-safe
// charset, so nothing can escape the line. nullopt (+ |error|) otherwise.
std::optional<std::string> NormalizeBridgeLine(std::string_view line,
                                               std::string* error);

// Parses a pasted block of bridge lines (one per line; blank lines and lines
// starting with '#' are ignored; duplicates dropped). nullopt with an error
// naming the offending line if ANY line is invalid — we never apply a
// partially-valid set. At most kMaxBridgeLines lines.
inline constexpr size_t kMaxBridgeLines = 32;
std::optional<std::vector<std::string>> ParseBridgeLines(std::string_view text,
                                                         std::string* error);

// Transport of a normalized bridge line, or nullopt if unknown.
std::optional<BridgeTransport> GetBridgeTransport(
    std::string_view normalized_line);

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_TORRC_BUILDER_H_
