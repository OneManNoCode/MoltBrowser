// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/tor_service.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>
#include <map>
#include <sstream>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"

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

constexpr const char* kControlHost = "127.0.0.1";
constexpr int kControlPort = 9051;
constexpr int kConnectTimeoutMs = 300;   // Tor is local: fast or absent.
constexpr int kIoTimeoutMs = 1000;       // Per send/recv slice.

// One blocking control-port session. Connects, authenticates, runs
// |command|, closes. Returns the raw response body (lines between
// the initial response and the "250 OK" terminator). Empty on failure.
struct ControlResult {
  bool ok = false;
  std::string body;        // Multi-line response payload.
  std::string version;     // Filled by Probe path.
  std::string error;       // Human-readable.
};

// Read all bytes off the socket until we see a "250 OK\r\n" line on
// its own, or the connection closes. Caps at 256 KB so a broken Tor
// can't OOM us.
bool ReadFullResponse(int fd, std::string& out) {
  out.clear();
  out.reserve(2048);
  char buf[4096];
  constexpr size_t kCap = 256 * 1024;
  while (out.size() < kCap) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return !out.empty();
    out.append(buf, static_cast<size_t>(n));
    // Look for "250 OK\r\n" at the end (Tor's standard success
    // terminator). Some commands send "250 OK" as the only response.
    if (out.size() >= 8 &&
        out.find("\r\n250 OK\r\n") != std::string::npos) {
      return true;
    }
    if (out.size() >= 8 && out.substr(0, 8) == "250 OK\r\n") {
      return true;
    }
    if (out.find("\r\n515 ") != std::string::npos ||
        out.find("\r\n514 ") != std::string::npos ||
        out.find("\r\n551 ") != std::string::npos) {
      // Authentication / command errors.
      return true;
    }
  }
  return true;
}

// Set send/recv timeout on |fd|.
void SetTimeouts(int fd) {
  struct timeval tv;
  tv.tv_sec = kIoTimeoutMs / 1000;
  tv.tv_usec = (kIoTimeoutMs % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// Connect with a configurable timeout using non-blocking + select.
int ConnectWithTimeout(int fd, const struct sockaddr* addr, socklen_t len) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int r = connect(fd, addr, len);
  if (r == 0) {
    fcntl(fd, F_SETFL, flags);
    return 0;
  }
  if (errno != EINPROGRESS) return -1;
  fd_set wset;
  FD_ZERO(&wset);
  FD_SET(fd, &wset);
  struct timeval tv;
  tv.tv_sec = kConnectTimeoutMs / 1000;
  tv.tv_usec = (kConnectTimeoutMs % 1000) * 1000;
  int sr = select(fd + 1, nullptr, &wset, nullptr, &tv);
  if (sr <= 0) return -1;
  int err = 0;
  socklen_t errlen = sizeof(err);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) return -1;
  if (err != 0) { errno = err; return -1; }
  fcntl(fd, F_SETFL, flags);
  return 0;
}

// Hex-encode a byte string for AUTHENTICATE <hex>.
std::string HexEncode(const std::string& bytes) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out += kHex[(c >> 4) & 0xF];
    out += kHex[c & 0xF];
  }
  return out;
}

// Attempt to load the Tor control auth cookie. We try a few common
// macOS / Linux locations. The cookie is a 32-byte binary blob.
// Returns empty string if not found / unreadable.
std::string TryReadAuthCookie(const std::string& path_hint) {
  if (!path_hint.empty()) {
    std::string contents;
    if (base::ReadFileToString(base::FilePath(path_hint), &contents) &&
        contents.size() >= 16) {
      return contents;
    }
  }
  static const char* kCommonPaths[] = {
      "/usr/local/var/lib/tor/control_auth_cookie",        // Intel brew
      "/opt/homebrew/var/lib/tor/control_auth_cookie",     // Apple Silicon brew
      "/var/lib/tor/control_auth_cookie",                   // Linux pkg
  };
  for (const char* p : kCommonPaths) {
    std::string contents;
    if (base::ReadFileToString(base::FilePath(p), &contents) &&
        contents.size() >= 16) {
      return contents;
    }
  }
  return {};
}

// Parse the PROTOCOLINFO response to learn which AUTH methods Tor
// supports, and the cookie path if any.
// Lines look like:
//   250-PROTOCOLINFO 1
//   250-AUTH METHODS=NULL,COOKIE,SAFECOOKIE COOKIEFILE="/path"
//   250-VERSION Tor="0.4.8.10"
//   250 OK
struct ProtocolInfo {
  bool null_auth = false;
  bool cookie_auth = false;
  std::string cookie_path;
  std::string version;
};
ProtocolInfo ParseProtocolInfo(const std::string& resp) {
  ProtocolInfo pi;
  for (const auto& line : base::SplitString(
           resp, "\r\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (line.find("AUTH METHODS=") != std::string::npos) {
      size_t mp = line.find("METHODS=");
      size_t end = line.find(' ', mp);
      std::string methods = line.substr(mp + 8,
                                         end == std::string::npos
                                             ? std::string::npos
                                             : end - (mp + 8));
      for (const auto& m :
           base::SplitString(methods, ",", base::TRIM_WHITESPACE,
                             base::SPLIT_WANT_NONEMPTY)) {
        if (m == "NULL") pi.null_auth = true;
        if (m == "COOKIE") pi.cookie_auth = true;
      }
      size_t cp = line.find("COOKIEFILE=\"");
      if (cp != std::string::npos) {
        size_t s = cp + 12;
        size_t e = line.find('\"', s);
        if (e != std::string::npos) pi.cookie_path = line.substr(s, e - s);
      }
    }
    size_t vp = line.find("VERSION Tor=\"");
    if (vp != std::string::npos) {
      size_t s = vp + 13;
      size_t e = line.find('\"', s);
      if (e != std::string::npos) pi.version = line.substr(s, e - s);
    }
  }
  return pi;
}

// Open one control-port session, authenticate, run |command|, return
// the response body. All operations are blocking; meant to run on a
// MayBlock() thread-pool task.
ControlResult RunOneCommand(const std::string& command) {
  ControlResult r;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    r.error = "socket() failed";
    return r;
  }
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kControlPort);
  inet_pton(AF_INET, kControlHost, &addr.sin_addr);
  if (ConnectWithTimeout(fd, reinterpret_cast<struct sockaddr*>(&addr),
                          sizeof(addr)) != 0) {
    close(fd);
    r.error = "no local Tor (connect refused/timeout on 127.0.0.1:9051)";
    return r;
  }
  SetTimeouts(fd);

  // 1) PROTOCOLINFO to learn supported auth methods + version.
  if (send(fd, "PROTOCOLINFO 1\r\n", 16, 0) < 0) {
    close(fd); r.error = "send PROTOCOLINFO failed"; return r;
  }
  std::string pi_resp;
  ReadFullResponse(fd, pi_resp);
  ProtocolInfo pi = ParseProtocolInfo(pi_resp);
  r.version = pi.version;

  // 2) AUTHENTICATE.
  std::string auth_cmd;
  if (pi.null_auth) {
    auth_cmd = "AUTHENTICATE\r\n";
  } else if (pi.cookie_auth) {
    std::string cookie = TryReadAuthCookie(pi.cookie_path);
    if (cookie.empty()) {
      close(fd);
      r.error = "Tor requires cookie auth but cookie file unreadable. "
                "Add `CookieAuthFileGroupReadable 1` to torrc.";
      return r;
    }
    auth_cmd = "AUTHENTICATE " + HexEncode(cookie) + "\r\n";
  } else {
    close(fd);
    r.error =
        "Tor requires HASHEDPASSWORD or SAFECOOKIE auth — not yet supported.";
    return r;
  }
  if (send(fd, auth_cmd.data(), auth_cmd.size(), 0) < 0) {
    close(fd); r.error = "send AUTHENTICATE failed"; return r;
  }
  std::string auth_resp;
  ReadFullResponse(fd, auth_resp);
  if (auth_resp.find("250 OK") == std::string::npos) {
    close(fd);
    r.error = "Tor rejected auth: " + auth_resp.substr(0, 80);
    return r;
  }

  // 3) The actual command.
  std::string cmd_with_eol = command + "\r\n";
  if (send(fd, cmd_with_eol.data(), cmd_with_eol.size(), 0) < 0) {
    close(fd); r.error = "send command failed"; return r;
  }
  ReadFullResponse(fd, r.body);

  // 4) Polite QUIT.
  send(fd, "QUIT\r\n", 6, 0);
  close(fd);
  r.ok = true;
  return r;
}

// PROBE: a minimal command (we use GETINFO version, since
// PROTOCOLINFO already gave us version in RunOneCommand).
TorStatus ProbeBlocking() {
  TorStatus s;
  s.control_port_addr = std::string(kControlHost) + ":" +
                         std::to_string(kControlPort);
  s.socks_port_addr = std::string(kControlHost) + ":9050";
  ControlResult r = RunOneCommand("GETINFO version");
  if (!r.ok) {
    s.running = false;
    s.error = r.error;
    return s;
  }
  s.running = true;
  s.version = r.version;
  // If GETINFO version returned a fresher value, prefer it.
  size_t vp = r.body.find("250-version=");
  if (vp != std::string::npos) {
    size_t s_ = vp + 12;
    size_t e = r.body.find("\r\n", s_);
    if (e != std::string::npos) s.version = r.body.substr(s_, e - s_);
  }
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

// Forward decl — defined below.
std::vector<TorCircuit> GetCircuitsBlocking();

std::vector<TorCircuit> GetCircuitsEnrichedBlocking() {
  std::vector<TorCircuit> circuits = GetCircuitsBlocking();
  // For each unique fingerprint across all hops, resolve IP + country.
  // Cache the lookups so we only ask once per fingerprint per call.
  // |ip_cache|[fp] = ip; |country_cache|[ip] = ISO code.
  std::map<std::string, std::string> ip_cache;
  std::map<std::string, std::string> country_cache;
  for (auto& c : circuits) {
    for (auto& h : c.hops) {
      if (h.fingerprint.empty()) continue;
      // Resolve IP.
      auto it = ip_cache.find(h.fingerprint);
      if (it == ip_cache.end()) {
        ControlResult r =
            RunOneCommand("GETINFO ns/id/" + h.fingerprint);
        std::string ip = r.ok ? ParseIpFromNsBlock(r.body) : std::string();
        ip_cache[h.fingerprint] = ip;
        h.ip = ip;
      } else {
        h.ip = it->second;
      }
      if (h.ip.empty()) continue;
      // Resolve country.
      auto cit = country_cache.find(h.ip);
      if (cit == country_cache.end()) {
        ControlResult r =
            RunOneCommand("GETINFO ip-to-country/" + h.ip);
        std::string cc =
            r.ok ? ParseCountryReply(r.body) : std::string();
        country_cache[h.ip] = cc;
        h.country = cc;
      } else {
        h.country = cit->second;
      }
    }
  }
  return circuits;
}

std::vector<TorCircuit> GetCircuitsBlocking() {
  std::vector<TorCircuit> out;
  ControlResult r = RunOneCommand("GETINFO circuit-status");
  if (!r.ok) return out;
  // The reply uses Tor's "data" reply form:
  //   250+circuit-status=
  //   <line>
  //   <line>
  //   .
  //   250 OK
  size_t start = r.body.find("250+circuit-status=");
  if (start == std::string::npos) {
    // Single-line form: 250-circuit-status=<lines newline-joined>
    size_t alt = r.body.find("250-circuit-status=");
    if (alt == std::string::npos) return out;
    size_t s = alt + 19;
    size_t e = r.body.find("\r\n", s);
    if (e == std::string::npos) return out;
    out.push_back(ParseCircuitLine(r.body.substr(s, e - s)));
    return out;
  }
  size_t s = r.body.find("\r\n", start);
  if (s == std::string::npos) return out;
  s += 2;
  while (s < r.body.size()) {
    size_t e = r.body.find("\r\n", s);
    if (e == std::string::npos) break;
    std::string line = r.body.substr(s, e - s);
    if (line == ".") break;
    if (!line.empty()) out.push_back(ParseCircuitLine(line));
    s = e + 2;
  }
  return out;
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
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ProbeBlocking),
      std::move(on_done));
}

void TorService::GetCircuits(
    base::OnceCallback<void(std::vector<TorCircuit>)> on_done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&GetCircuitsBlocking),
      std::move(on_done));
}

void TorService::GetCircuitsEnriched(
    base::OnceCallback<void(std::vector<TorCircuit>)> on_done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&GetCircuitsEnrichedBlocking),
      std::move(on_done));
}

}  // namespace tor
}  // namespace molt_ai
