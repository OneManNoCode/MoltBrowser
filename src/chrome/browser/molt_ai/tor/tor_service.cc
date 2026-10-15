// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/tor_service.h"

#include <map>
#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/molt_ai/tor/tor_control_connection.h"
#include "chrome/browser/molt_ai/tor/tor_control_protocol.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"

namespace molt_ai {
namespace tor {

TorStatus::TorStatus() = default;
TorStatus::TorStatus(const TorStatus&) = default;
TorStatus::TorStatus(TorStatus&&) = default;
TorStatus& TorStatus::operator=(const TorStatus&) = default;
TorStatus& TorStatus::operator=(TorStatus&&) = default;
TorStatus::~TorStatus() = default;

TorCircuit::TorCircuit() = default;
TorCircuit::TorCircuit(const TorCircuit&) = default;
TorCircuit::TorCircuit(TorCircuit&&) = default;
TorCircuit& TorCircuit::operator=(const TorCircuit&) = default;
TorCircuit& TorCircuit::operator=(TorCircuit&&) = default;
TorCircuit::~TorCircuit() = default;

TorRelay::TorRelay() = default;
TorRelay::TorRelay(const TorRelay&) = default;
TorRelay::TorRelay(TorRelay&&) = default;
TorRelay& TorRelay::operator=(const TorRelay&) = default;
TorRelay& TorRelay::operator=(TorRelay&&) = default;
TorRelay::~TorRelay() = default;

namespace {

// One authenticated (SAFECOOKIE) control session for a batch of commands.
// Only MoltNet's own Tor is ever queried: the endpoint comes from
// TorManager, and there is no fallback to 127.0.0.1:9051, NULL auth, or a
// system Tor's cookie file.
struct ControlResult {
  bool ok = false;
  std::string body;   // Raw reply to the command.
  std::string error;  // Human-readable.
};

ControlResult RunOneCommand(TorControlConnection& connection,
                            const std::string& command) {
  ControlResult r;
  std::optional<std::string> reply = connection.SendCommand(command);
  if (!reply) {
    r.error = "Tor control connection lost";
    return r;
  }
  if (control::GetReplyStatus(*reply) != 250) {
    r.error = "Tor rejected the request";
    return r;
  }
  r.ok = true;
  r.body = std::move(*reply);
  return r;
}

TorStatus ProbeBlocking(TorControlEndpoint endpoint, uint16_t socks_port) {
  TorStatus s;
  if (!endpoint.IsValid()) {
    s.error = "MoltNet's Tor isn't running.";
    return s;
  }
  s.control_port_addr = "127.0.0.1:" + std::to_string(endpoint.port);
  s.socks_port_addr = "127.0.0.1:" + std::to_string(socks_port);
  TorControlConnection connection;
  std::string error;
  if (connection.ConnectAndAuthenticate(endpoint, &error) !=
      TorControlConnection::AuthResult::kOk) {
    s.error = error;
    return s;
  }
  ControlResult r = RunOneCommand(connection, "GETINFO version");
  if (!r.ok) {
    s.error = r.error;
    return s;
  }
  s.running = true;
  s.version =
      control::ParseGetInfoValue(r.body, "version").value_or(std::string());
  return s;
}

// Parse a single circuit-status line. Format (see tor-spec/control-spec):
//   CircuitID SP Status SP Path [SP BuildFlags] [SP Purpose=...] ...
// where Path = "$FP1~Nick1,$FP2~Nick2,$FP3~Nick3"
TorCircuit ParseCircuitLine(const std::string& line) {
  TorCircuit c;
  std::vector<std::string> parts =
      base::SplitString(line, " ", base::TRIM_WHITESPACE,
                        base::SPLIT_WANT_NONEMPTY);
  if (parts.size() < 2) return c;
  c.id = parts[0];
  c.state = parts[1];
  // The third field is the path *if* state is BUILT/EXTENDED/...
  // For LAUNCHED there may be no path yet.
  if (parts.size() >= 3 && !parts[2].empty() && parts[2][0] == '$') {
    auto hops = base::SplitString(parts[2], ",", base::TRIM_WHITESPACE,
                                   base::SPLIT_WANT_NONEMPTY);
    for (const auto& h : hops) {
      TorRelay relay;
      // Expected form: $FP~Nick or $FP=Nick or $FP
      size_t sep = h.find('~');
      if (sep == std::string::npos) sep = h.find('=');
      if (sep == std::string::npos) {
        relay.fingerprint = (h.size() >= 1 && h[0] == '$') ? h.substr(1) : h;
      } else {
        relay.fingerprint = (h.size() >= 1 && h[0] == '$')
                                ? h.substr(1, sep - 1)
                                : h.substr(0, sep);
        relay.nickname = h.substr(sep + 1);
      }
      c.hops.push_back(std::move(relay));
    }
  }
  // Hunt for PURPOSE=...
  for (const auto& p : parts) {
    if (p.rfind("PURPOSE=", 0) == 0) {
      c.purpose = p.substr(8);
      break;
    }
  }
  return c;
}

// Parse the IP from a "ns/id/<fp>" response. Tor responds with a
// 250+ns/id/...= block of consensus lines. The "r " line contains:
//   r <nickname> <id-base64> <descriptor-base64> <date> <time> <ip> <orport> <dirport>
std::string ParseIpFromNsBlock(const std::string& body) {
  size_t s = body.find("\r\nr ");
  if (s == std::string::npos) return {};
  s += 4;
  size_t e = body.find("\r\n", s);
  if (e == std::string::npos) return {};
  std::string line = body.substr(s, e - s);
  // Tokenize on space; IP is field index 5 (0-based).
  auto parts = base::SplitString(line, " ", base::TRIM_WHITESPACE,
                                  base::SPLIT_WANT_NONEMPTY);
  if (parts.size() < 6) return {};
  return parts[5];
}

// Parse the country code from "250-ip-to-country/<ip>=XX".
std::string ParseCountryReply(const std::string& body) {
  size_t s = body.find("=");
  if (s == std::string::npos) return {};
  ++s;
  size_t e = body.find("\r\n", s);
  std::string code = e == std::string::npos ? body.substr(s)
                                              : body.substr(s, e - s);
  // Tor returns "??" when the IP isn't in its GeoIP DB.
  if (code == "??" || code.empty()) return {};
  return code;
}

std::vector<TorCircuit> ParseCircuitStatus(const std::string& body) {
  std::vector<TorCircuit> out;
  // The reply uses Tor's "data" reply form:
  //   250+circuit-status=
  //   <line>
  //   <line>
  //   .
  //   250 OK
  size_t start = body.find("250+circuit-status=");
  if (start == std::string::npos) {
    // Single-line form: 250-circuit-status=<one circuit> (or empty).
    size_t alt = body.find("250-circuit-status=");
    if (alt == std::string::npos) return out;
    size_t s = alt + 19;
    size_t e = body.find("\r\n", s);
    if (e == std::string::npos || e == s) return out;
    out.push_back(ParseCircuitLine(body.substr(s, e - s)));
    return out;
  }
  size_t s = body.find("\r\n", start);
  if (s == std::string::npos) return out;
  s += 2;
  while (s < body.size()) {
    size_t e = body.find("\r\n", s);
    if (e == std::string::npos) break;
    std::string line = body.substr(s, e - s);
    if (line == ".") break;
    if (!line.empty()) out.push_back(ParseCircuitLine(line));
    s = e + 2;
  }
  return out;
}

// All queries share ONE authenticated session (each SAFECOOKIE handshake is
// three round-trips, so per-query sessions would be wasteful).
std::vector<TorCircuit> GetCircuitsBlocking(TorControlEndpoint endpoint,
                                            bool enrich) {
  TorControlConnection connection;
  std::string error;
  if (connection.ConnectAndAuthenticate(endpoint, &error) !=
      TorControlConnection::AuthResult::kOk) {
    return {};
  }
  ControlResult r = RunOneCommand(connection, "GETINFO circuit-status");
  if (!r.ok) return {};
  std::vector<TorCircuit> circuits = ParseCircuitStatus(r.body);
  if (!enrich) return circuits;

  // For each unique fingerprint across all hops, resolve IP + country from
  // Tor's own consensus + GeoIP DB (no third-party lookup service).
  // |ip_cache|[fp] = ip; |country_cache|[ip] = ISO code.
  std::map<std::string, std::string> ip_cache;
  std::map<std::string, std::string> country_cache;
  for (auto& c : circuits) {
    for (auto& h : c.hops) {
      // Fingerprints come from Tor, but they are spliced into a command:
      // insist on 40 hex chars so nothing else can ride along.
      if (h.fingerprint.size() != 40 ||
          !control::HexDecode(h.fingerprint)) {
        continue;
      }
      auto it = ip_cache.find(h.fingerprint);
      if (it == ip_cache.end()) {
        ControlResult ns =
            RunOneCommand(connection, "GETINFO ns/id/" + h.fingerprint);
        std::string ip = ns.ok ? ParseIpFromNsBlock(ns.body) : std::string();
        ip_cache[h.fingerprint] = ip;
        h.ip = ip;
      } else {
        h.ip = it->second;
      }
      if (h.ip.empty() ||
          h.ip.find_first_not_of("0123456789abcdefABCDEF.:") !=
              std::string::npos) {
        continue;
      }
      auto cit = country_cache.find(h.ip);
      if (cit == country_cache.end()) {
        ControlResult geo =
            RunOneCommand(connection, "GETINFO ip-to-country/" + h.ip);
        std::string cc = geo.ok ? ParseCountryReply(geo.body) : std::string();
        country_cache[h.ip] = cc;
        h.country = cc;
      } else {
        h.country = cit->second;
      }
    }
  }
  return circuits;
}

}  // namespace

// static
TorService* TorService::Get() {
  static base::NoDestructor<TorService> instance;
  return instance.get();
}

TorService::TorService() = default;
TorService::~TorService() = default;

void TorService::Probe(base::OnceCallback<void(TorStatus)> on_done) {
  TorManager* manager = TorManager::Get();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ProbeBlocking, manager->GetControlEndpoint(),
                     manager->socks_port()),
      std::move(on_done));
}

void TorService::GetCircuits(
    base::OnceCallback<void(std::vector<TorCircuit>)> on_done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&GetCircuitsBlocking,
                     TorManager::Get()->GetControlEndpoint(),
                     /*enrich=*/false),
      std::move(on_done));
}

void TorService::GetCircuitsEnriched(
    base::OnceCallback<void(std::vector<TorCircuit>)> on_done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&GetCircuitsBlocking,
                     TorManager::Get()->GetControlEndpoint(),
                     /*enrich=*/true),
      std::move(on_done));
}

}  // namespace tor
}  // namespace molt_ai
