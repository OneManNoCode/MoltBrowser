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

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace tor {

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

  // Executables for the pluggable transports, as Tor should exec them. These
  // are RELATIVE to Tor's working directory (TorManager launches tor with its
  // cwd set to the bundled tor dir) because Tor splits the
  // ClientTransportPlugin line on spaces, so an absolute path under e.g.
  // "Application Support" would break. "" = transport not bundled.
  std::string lyrebird_exec;
  std::string snowflake_exec;
};

// Renders the managed torrc. Returns nullopt (and sets |error|) if any field
// is unsafe to place in a torrc line or a bridge needs a transport that isn't
// bundled. Ports are always `auto` (Tor picks free ports; MoltNet reads them
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
