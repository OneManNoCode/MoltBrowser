// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's torrc generation (tor/torrc_builder.h) on what the user pastes
// (bridge lines, from a web page, a message or BridgeDB) and on paths and
// settings from the profile. A torrc line is a trust boundary: an injected
// newline would add Tor options (a SocksPort on 0.0.0.0, a ControlPort
// without authentication).
//
// Checks: no memory or UB error; ParseBridgeLines() gives at most
// kMaxBridgeLines distinct lines, each already normalized (normalizing is
// idempotent) and of a known transport; every rendered torrc is the fixed
// set of options with one option per line: only the expected keywords, the
// fixed SocksPort / ControlPort lines exactly once, no control characters
// and no comment after the header; validators agree with what they promise.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/browser/molt_ai/tor/pluggable_transports.h"
#include "chrome/browser/molt_ai/tor/torrc_builder.h"

namespace {

using molt_ai::tor::kTorConnLimit;
using molt_ai::tor::kTorMaxClientCircuitsPending;
using molt_ai::tor::TorrcOptions;

std::string Field(FuzzedDataProvider* p, const char* typical) {
  return p->ConsumeBool() ? std::string(typical)
                          : p->ConsumeRandomLengthString(128);
}

void CheckTorrc(const std::string& torrc, const TorrcOptions& options) {
  static const std::set<std::string> kKeys = {
      "SocksPort",     "ControlPort",   "ControlPortWriteToFile",
      "CookieAuthentication", "CookieAuthFile", "DataDirectory",
      "Log",           "AvoidDiskWrites", "GeoIPFile",
      "GeoIPv6File",   "UseBridges",    "ClientTransportPlugin",
      "Bridge",        "ExitNodes",     "StrictNodes",
      "ConnLimit",     "MaxClientCircuitsPending",
  };
  static const std::set<std::string> kRepeatable = {"ClientTransportPlugin",
                                                    "Bridge"};
  CHECK(!torrc.empty() && torrc.back() == '\n');
  std::map<std::string, int> seen;
  size_t line_start = 0;
  bool first = true;
  while (line_start < torrc.size()) {
    const size_t end = torrc.find('\n', line_start);
    CHECK(end != std::string::npos);
    const std::string line = torrc.substr(line_start, end - line_start);
    line_start = end + 1;
    for (char ch : line) {
      const auto c = static_cast<unsigned char>(ch);
      CHECK(c >= 0x20 && c != 0x7F) << "a control character in the torrc";
    }
    if (first) {
      CHECK(line.rfind("# ", 0) == 0);
      first = false;
      continue;
    }
    CHECK(line.find('#') == std::string::npos) << "a comment in a value";
    CHECK(!line.empty() && line.back() != '\\') << "a line continuation";
    const std::string key = line.substr(0, line.find(' '));
    CHECK(kKeys.count(key)) << "unexpected torrc option: " << key;
    if (!kRepeatable.count(key)) CHECK(++seen[key] == 1) << "repeated " << key;
    if (key == "SocksPort") {
      CHECK(line == "SocksPort auto IsolateSOCKSAuth KeepAliveIsolateSOCKSAuth");
    }
    if (key == "ControlPort") CHECK(line == "ControlPort auto");
    if (key == "ConnLimit") {
      CHECK(line == "ConnLimit " + std::to_string(kTorConnLimit));
    }
    if (key == "MaxClientCircuitsPending") {
      CHECK(line == "MaxClientCircuitsPending " +
                        std::to_string(kTorMaxClientCircuitsPending));
    }
  }
  CHECK(seen["SocksPort"] == 1 && seen["ControlPort"] == 1 &&
        seen["CookieAuthentication"] == 1 && seen["ConnLimit"] == 1 &&
        seen["MaxClientCircuitsPending"] == 1);
  size_t bridges = 0;
  for (size_t at = 0; (at = torrc.find("\nBridge ", at)) != std::string::npos;
       ++at) {
    ++bridges;
  }
  CHECK(bridges == options.bridge_lines.size());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai::tor;
  FuzzedDataProvider p(data, size);

  // A pasted block of bridge lines.
  std::string error;
  const std::string pasted = p.ConsumeRandomLengthString(8192);
  const std::optional<std::vector<std::string>> parsed =
      ParseBridgeLines(pasted, &error);
  if (parsed) {
    CHECK(parsed->size() <= kMaxBridgeLines);
    std::set<std::string> distinct(parsed->begin(), parsed->end());
    CHECK(distinct.size() == parsed->size()) << "duplicate bridge lines";
    for (const std::string& line : *parsed) {
      const std::optional<std::string> again = NormalizeBridgeLine(line, nullptr);
      CHECK(again && *again == line) << "a parsed line isn't normalized";
      CHECK(GetBridgeTransport(line).has_value());
    }
    RequiredTransports(*parsed);
  } else {
    CHECK(!error.empty());
  }

  // One line on its own.
  const std::string one = p.ConsumeRandomLengthString(1024);
  const std::optional<std::string> normalized = NormalizeBridgeLine(one, nullptr);
  if (normalized) {
    CHECK(NormalizeBridgeLine(*normalized, nullptr) == normalized);
    CHECK(IsSafeTorrcValue(*normalized));
    CHECK(normalized->find('\n') == std::string::npos);
  }

  TorrcOptions options;
  options.data_dir = Field(&p, "/home/u/.moltbrowser/tor/data");
  options.cookie_file = Field(&p, "/home/u/.moltbrowser/tor/data/control_auth_cookie");
  options.control_port_file = Field(&p, "/home/u/.moltbrowser/tor/data/control_port");
  options.log_file = Field(&p, "/home/u/.moltbrowser/tor/tor.log");
  options.geoip_file = p.ConsumeBool() ? "" : Field(&p, "/opt/molt/tor/geoip");
  options.geoip6_file = p.ConsumeBool() ? "" : Field(&p, "/opt/molt/tor/geoip6");
  options.exit_country = p.ConsumeBool() ? "" : Field(&p, "de");
  options.obfs4_exec = Field(&p, "./pluggable_transports/lyrebird");
  options.snowflake_exec = Field(&p, "/usr/bin/snowflake-client");
  if (parsed && p.ConsumeBool()) {
    options.bridge_lines = *parsed;
  } else if (normalized) {
    options.bridge_lines.push_back(*normalized);
  }
  if (p.ConsumeBool()) {
    // Not normalized: BuildTorrc must refuse it, not render it.
    options.bridge_lines.push_back(p.ConsumeRandomLengthString(256));
  }
  const std::optional<std::string> torrc = BuildTorrc(options, &error);
  if (torrc) {
    CheckTorrc(*torrc, options);
  }

  // The validators.
  for (const std::string* value :
       {&options.data_dir, &options.obfs4_exec, &options.exit_country}) {
    if (IsSafeTorrcValue(*value)) {
      CHECK(!value->empty());
      for (char ch : *value) {
        const auto c = static_cast<unsigned char>(ch);
        CHECK(c >= 0x20 && c != 0x7F && ch != '#');
      }
      CHECK(value->front() != '"' && value->front() != ' ' &&
            value->back() != ' ' && value->back() != '\\');
    }
    if (IsValidTransportExec(*value)) {
      CHECK(IsSafeTorrcValue(*value));
      CHECK(value->find(' ') == std::string::npos);
      // No ".." component, and never a bare program name (PATH lookup).
      std::string path = *value;
      for (char& c : path) {
        if (c == '\\') c = '/';
      }
      CHECK(("/" + path + "/").find("/../") == std::string::npos);
      CHECK(path.find('/') != std::string::npos);
    }
    if (IsValidExitCountry(*value)) {
      CHECK(value->size() == 2 && (*value)[0] >= 'a' && (*value)[0] <= 'z' &&
            (*value)[1] >= 'a' && (*value)[1] <= 'z');
    }
  }
  return 0;
}
