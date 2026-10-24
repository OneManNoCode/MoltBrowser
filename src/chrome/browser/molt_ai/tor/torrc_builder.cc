// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/torrc_builder.h"

#include <set>
#include <string>
#include <utility>

namespace molt_ai {
namespace tor {

namespace {

constexpr size_t kMaxBridgeLineLength = 4096;
constexpr size_t kMaxParamValueLength = 2048;

bool IsAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}
bool IsAsciiLower(char c) {
  return c >= 'a' && c <= 'z';
}
bool IsAsciiAlnum(char c) {
  return IsAsciiDigit(c) || IsAsciiLower(c) || (c >= 'A' && c <= 'Z');
}
bool IsHexDigit(char c) {
  return IsAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool IsBlank(char c) {
  return c == ' ' || c == '\t';
}

std::string_view TrimBlanks(std::string_view s) {
  while (!s.empty() && (IsBlank(s.front()) || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (IsBlank(s.back()) || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

std::vector<std::string_view> SplitOnBlanks(std::string_view s) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && IsBlank(s[i])) {
      ++i;
    }
    size_t start = i;
    while (i < s.size() && !IsBlank(s[i])) {
      ++i;
    }
    if (i > start) {
      out.push_back(s.substr(start, i - start));
    }
  }
  return out;
}

// Decimal 1..65535, no sign, at most 5 digits.
bool IsValidPort(std::string_view port) {
  if (port.empty() || port.size() > 5) {
    return false;
  }
  unsigned value = 0;
  for (char c : port) {
    if (!IsAsciiDigit(c)) {
      return false;
    }
    value = value * 10 + static_cast<unsigned>(c - '0');
  }
  return value >= 1 && value <= 65535;
}

bool IsValidIPv4(std::string_view host) {
  int octets = 0;
  while (true) {
    size_t dot = host.find('.');
    std::string_view part = host.substr(0, dot);
    if (part.empty() || part.size() > 3) {
      return false;
    }
    unsigned value = 0;
    for (char c : part) {
      if (!IsAsciiDigit(c)) {
        return false;
      }
      value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value > 255) {
      return false;
    }
    ++octets;
    if (dot == std::string_view::npos) {
      break;
    }
    host.remove_prefix(dot + 1);
  }
  return octets == 4;
}

bool IsValidIPv6Literal(std::string_view host) {
  if (host.size() < 2 || host.size() > 45) {
    return false;
  }
  int colons = 0;
  for (char c : host) {
    if (c == ':') {
      ++colons;
    } else if (!IsHexDigit(c) && c != '.') {
      return false;
    }
  }
  return colons >= 2;
}

// "<IPv4>:<port>" or "[<IPv6>]:<port>".
bool IsValidBridgeAddress(std::string_view addr) {
  size_t colon = addr.rfind(':');
  if (colon == std::string_view::npos ||
      !IsValidPort(addr.substr(colon + 1))) {
    return false;
  }
  std::string_view host = addr.substr(0, colon);
  if (!host.empty() && host.front() == '[') {
    if (host.size() < 3 || host.back() != ']') {
      return false;
    }
    return IsValidIPv6Literal(host.substr(1, host.size() - 2));
  }
  return IsValidIPv4(host);
}

bool IsValidParamKey(std::string_view key) {
  if (key.empty() || key.size() > 32 || !IsAsciiLower(key[0])) {
    return false;
  }
  for (char c : key) {
    if (!IsAsciiLower(c) && !IsAsciiDigit(c) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

// URL-safe-ish value charset. Deliberately excludes whitespace, quotes,
// backslash, '#', '{', '}', '$', '!', ';', '*', '|', '<', '>' and '`'.
bool IsValidParamValue(std::string_view value) {
  if (value.empty() || value.size() > kMaxParamValueLength) {
    return false;
  }
  static constexpr std::string_view kExtra = "+/=:,.-_~%@?&[]";
  for (char c : value) {
    if (!IsAsciiAlnum(c) && kExtra.find(c) == std::string_view::npos) {
      return false;
    }
  }
  return true;
}

bool IsValidObfs4Cert(std::string_view cert) {
  size_t padding = 0;
  while (!cert.empty() && cert.back() == '=') {
    cert.remove_suffix(1);
    ++padding;
  }
  if (padding > 2 || cert.size() < 40 || cert.size() > 120) {
    return false;
  }
  for (char c : cert) {
    if (!IsAsciiAlnum(c) && c != '+' && c != '/') {
      return false;
    }
  }
  return true;
}

// Splits |path| on '/' (and on '\\' too if |backslash_separates|).
std::vector<std::string_view> SplitPathComponents(std::string_view path,
                                                  bool backslash_separates) {
  std::vector<std::string_view> out;
  size_t start = 0;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/' ||
        (backslash_separates && path[i] == '\\')) {
      out.push_back(path.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

}  // namespace

TorrcOptions::TorrcOptions() = default;
TorrcOptions::TorrcOptions(const TorrcOptions&) = default;
TorrcOptions& TorrcOptions::operator=(const TorrcOptions&) = default;
TorrcOptions::~TorrcOptions() = default;

bool IsValidExitCountry(std::string_view cc) {
  return cc.size() == 2 && IsAsciiLower(cc[0]) && IsAsciiLower(cc[1]);
}

bool IsValidTransportExec(std::string_view exec) {
  if (!IsSafeTorrcValue(exec)) {
    return false;
  }
  for (char c : exec) {
    if (IsBlank(c)) {
      return false;  // Tor splits ClientTransportPlugin on blanks.
    }
  }
  if (exec.front() == '/') {
    // A canonical absolute POSIX path (a system package on Linux).
    if (exec.find('\\') != std::string_view::npos) {
      return false;
    }
    for (std::string_view component :
         SplitPathComponents(exec.substr(1), /*backslash_separates=*/false)) {
      if (component.empty() || component == "." || component == "..") {
        return false;
      }
    }
    return true;
  }
  // Relative to Tor's working directory (a bundled transport). No drive
  // letter, and no root-relative or UNC Windows path.
  if (exec.front() == '\\' || (exec.size() >= 2 && exec[1] == ':')) {
    return false;
  }
  const std::vector<std::string_view> components =
      SplitPathComponents(exec, /*backslash_separates=*/true);
  size_t named = 0;
  for (size_t i = 0; i < components.size(); ++i) {
    const std::string_view component = components[i];
    if (i == 0 && component == ".") {
      continue;
    }
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    ++named;
  }
  // "./<program>" or "<dir>/<program>": never a bare name, which could be
  // looked up on PATH.
  return components.size() >= 2 && named >= 1;
}

bool IsSafeTorrcValue(std::string_view value) {
  if (value.empty() || value.front() == '"' || IsBlank(value.front()) ||
      IsBlank(value.back()) || value.back() == '\\') {
    return false;
  }
  for (char c : value) {
    unsigned char uc = static_cast<unsigned char>(c);
    // Control characters (incl. CR, LF, NUL, TAB) and DEL are never valid;
    // UTF-8 bytes (>= 0x80) are fine in paths.
    if (uc < 0x20 || uc == 0x7F || c == '#') {
      return false;
    }
  }
  return true;
}

std::optional<std::string> NormalizeBridgeLine(std::string_view line,
                                               std::string* error) {
  auto fail = [error](const char* why) -> std::optional<std::string> {
    if (error) {
      *error = why;
    }
    return std::nullopt;
  };
  line = TrimBlanks(line);
  if (line.size() > kMaxBridgeLineLength) {
    return fail("bridge line is too long");
  }
  for (char c : line) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (!(uc >= 0x21 && uc <= 0x7E) && !IsBlank(c)) {
      return fail("bridge line contains an invalid character");
    }
  }
  std::vector<std::string_view> tokens = SplitOnBlanks(line);
  if (!tokens.empty() && tokens[0] == "Bridge") {
    tokens.erase(tokens.begin());
  }
  if (tokens.size() < 3) {
    return fail("expected: <transport> <address:port> <fingerprint> ...");
  }
  const std::string_view transport = tokens[0];
  const bool is_obfs4 = transport == "obfs4";
  if (!is_obfs4 && transport != "snowflake") {
    return fail("only obfs4 and snowflake bridges are supported");
  }
  if (!IsValidBridgeAddress(tokens[1])) {
    return fail("invalid bridge address (expected IPv4:port or [IPv6]:port)");
  }
  if (tokens[2].size() != 40) {
    return fail("invalid fingerprint (expected 40 hex characters)");
  }
  std::string fingerprint;
  for (char c : tokens[2]) {
    if (!IsHexDigit(c)) {
      return fail("invalid fingerprint (expected 40 hex characters)");
    }
    fingerprint.push_back(c >= 'a' && c <= 'f' ? static_cast<char>(c - 32)
                                               : c);
  }

  std::set<std::string_view> keys;
  bool has_cert = false;
  bool has_iat = false;
  for (size_t i = 3; i < tokens.size(); ++i) {
    std::string_view token = tokens[i];
    size_t eq = token.find('=');
    if (eq == std::string_view::npos) {
      return fail("bridge options must be key=value");
    }
    std::string_view key = token.substr(0, eq);
    std::string_view value = token.substr(eq + 1);
    if (!IsValidParamKey(key) || !IsValidParamValue(value)) {
      return fail("invalid bridge option");
    }
    if (!keys.insert(key).second) {
      return fail("duplicate bridge option");
    }
    if (is_obfs4) {
      if (key == "cert") {
        if (!IsValidObfs4Cert(value)) {
          return fail("invalid obfs4 cert");
        }
        has_cert = true;
      } else if (key == "iat-mode") {
        if (value != "0" && value != "1" && value != "2") {
          return fail("invalid obfs4 iat-mode");
        }
        has_iat = true;
      } else {
        return fail("unexpected obfs4 option");
      }
    }
  }
  if (is_obfs4 && (!has_cert || !has_iat)) {
    return fail("obfs4 bridges need cert= and iat-mode=");
  }

  std::string out(transport);
  out += ' ';
  out += tokens[1];
  out += ' ';
  out += fingerprint;
  for (size_t i = 3; i < tokens.size(); ++i) {
    out += ' ';
    out += tokens[i];
  }
  return out;
}

std::optional<std::vector<std::string>> ParseBridgeLines(std::string_view text,
                                                         std::string* error) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  size_t line_number = 0;
  size_t pos = 0;
  while (pos <= text.size()) {
    ++line_number;
    const size_t nl = text.find('\n', pos);
    std::string_view raw =
        nl == std::string_view::npos ? text.substr(pos)
                                     : text.substr(pos, nl - pos);
    pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
    std::string_view line = TrimBlanks(raw);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::string why;
    std::optional<std::string> normalized = NormalizeBridgeLine(line, &why);
    if (!normalized) {
      if (error) {
        *error = "Line " + std::to_string(line_number) + ": " + why;
      }
      return std::nullopt;
    }
    if (seen.insert(*normalized).second) {
      out.push_back(std::move(*normalized));
    }
    if (out.size() > kMaxBridgeLines) {
      if (error) {
        *error = "Too many bridge lines (max " +
                 std::to_string(kMaxBridgeLines) + ")";
      }
      return std::nullopt;
    }
  }
  return out;
}

std::optional<BridgeTransport> GetBridgeTransport(
    std::string_view normalized_line) {
  std::string_view transport =
      normalized_line.substr(0, normalized_line.find(' '));
  if (transport == "obfs4") {
    return BridgeTransport::kObfs4;
  }
  if (transport == "snowflake") {
    return BridgeTransport::kSnowflake;
  }
  return std::nullopt;
}

std::optional<std::string> BuildTorrc(const TorrcOptions& options,
                                      std::string* error) {
  auto fail = [error](std::string why) -> std::optional<std::string> {
    if (error) {
      *error = std::move(why);
    }
    return std::nullopt;
  };
  const std::pair<const char*, const std::string*> required[] = {
      {"DataDirectory", &options.data_dir},
      {"CookieAuthFile", &options.cookie_file},
      {"ControlPortWriteToFile", &options.control_port_file},
      {"Log", &options.log_file},
  };
  for (const auto& [name, value] : required) {
    if (!IsSafeTorrcValue(*value)) {
      return fail(std::string("unsafe or empty path for ") + name);
    }
  }
  if (!options.geoip_file.empty() && !IsSafeTorrcValue(options.geoip_file)) {
    return fail("unsafe GeoIPFile path");
  }
  if (!options.geoip6_file.empty() && !IsSafeTorrcValue(options.geoip6_file)) {
    return fail("unsafe GeoIPv6File path");
  }
  if (!options.exit_country.empty() &&
      !IsValidExitCountry(options.exit_country)) {
    return fail("invalid exit country");
  }

  bool needs_obfs4 = false;
  bool needs_snowflake = false;
  for (const std::string& line : options.bridge_lines) {
    // Defense in depth: re-validate even though callers pass normalized
    // lines, and require the stored form to already be canonical.
    std::optional<std::string> normalized = NormalizeBridgeLine(line, nullptr);
    if (!normalized || *normalized != line) {
      return fail("invalid bridge line");
    }
    switch (*GetBridgeTransport(line)) {
      case BridgeTransport::kObfs4:
        needs_obfs4 = true;
        break;
      case BridgeTransport::kSnowflake:
        needs_snowflake = true;
        break;
    }
  }
  if (needs_obfs4 && !IsValidTransportExec(options.obfs4_exec)) {
    return fail("obfs4 bridges need a usable obfs4 transport");
  }
  if (needs_snowflake && !IsValidTransportExec(options.snowflake_exec)) {
    return fail("snowflake bridges need a usable snowflake transport");
  }

  std::string t;
  t += "# Auto-generated by MoltBrowser on every launch. Do not edit.\n";
  // Ports are chosen by Tor (`auto`) and read back, never fixed: a process
  // squatting on 9050/9051 can't impersonate MoltNet's Tor. IsolateSOCKSAuth
  // (Tor's default, stated explicitly) gives each SOCKS username/password —
  // MoltBrowser sends the top-level site — its own circuit.
  t += "SocksPort auto IsolateSOCKSAuth KeepAliveIsolateSOCKSAuth\n";
  t += "ControlPort auto\n";
  t += "ControlPortWriteToFile " + options.control_port_file + "\n";
  t += "CookieAuthentication 1\n";
  t += "CookieAuthFile " + options.cookie_file + "\n";
  t += "DataDirectory " + options.data_dir + "\n";
  t += "Log notice file " + options.log_file + "\n";
  t += "AvoidDiskWrites 1\n";
  // Capacity every MoltNet site shares (torrc_builder.h).
  t += "ConnLimit " + std::to_string(kTorConnLimit) + "\n";
  t += "MaxClientCircuitsPending " +
       std::to_string(kTorMaxClientCircuitsPending) + "\n";
  if (!options.geoip_file.empty()) {
    t += "GeoIPFile " + options.geoip_file + "\n";
  }
  if (!options.geoip6_file.empty()) {
    t += "GeoIPv6File " + options.geoip6_file + "\n";
  }
  if (!options.bridge_lines.empty()) {
    t += "UseBridges 1\n";
    if (needs_obfs4) {
      t += "ClientTransportPlugin obfs4 exec " + options.obfs4_exec + "\n";
    }
    if (needs_snowflake) {
      t += "ClientTransportPlugin snowflake exec " + options.snowflake_exec +
           "\n";
    }
    for (const std::string& line : options.bridge_lines) {
      t += "Bridge " + line + "\n";
    }
  }
  if (!options.exit_country.empty()) {
    t += "ExitNodes {" + options.exit_country + "}\n";
    t += "StrictNodes 1\n";
  }
  return t;
}

}  // namespace tor
}  // namespace molt_ai
