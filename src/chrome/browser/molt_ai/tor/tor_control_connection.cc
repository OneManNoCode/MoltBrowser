// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/tor_control_connection.h"

#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
// Winsock must be included before <windows.h>; pull in both via winsock2.
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif  // BUILDFLAG(IS_WIN)

#include <mutex>
#include <vector>

#include "base/files/file_util.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/tor/tor_control_protocol.h"
#include "crypto/random.h"

namespace molt_ai {
namespace tor {

namespace {

constexpr int kConnectTimeoutMs = 500;    // Tor is local: fast or absent.
constexpr int kIoTimeoutMs = 2000;        // Per send/recv slice.
constexpr base::TimeDelta kReplyDeadline = base::Seconds(15);
constexpr size_t kMaxReplyBytes = 1024 * 1024;

#if BUILDFLAG(IS_WIN)
using SocketHandle = SOCKET;
const SocketHandle kInvalidSocket = INVALID_SOCKET;

// Initialize Winsock exactly once for the process. We deliberately never call
// WSACleanup(): the browser uses sockets for its whole lifetime.
void EnsureWinsockStarted() {
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
  });
}

void CloseSocketHandle(SocketHandle fd) {
  closesocket(fd);
}

void SetIoTimeouts(SocketHandle fd) {
  // Winsock takes SO_RCVTIMEO/SO_SNDTIMEO as a DWORD of milliseconds.
  DWORD ms = static_cast<DWORD>(kIoTimeoutMs);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms),
             sizeof(ms));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms),
             sizeof(ms));
}

bool ConnectWithTimeout(SocketHandle fd, const sockaddr_in& addr) {
  u_long nonblocking = 1;
  ioctlsocket(fd, FIONBIO, &nonblocking);
  int r = connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  if (r != 0) {
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
      return false;
    }
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    fd_set eset;
    FD_ZERO(&eset);
    FD_SET(fd, &eset);
    timeval tv;
    tv.tv_sec = kConnectTimeoutMs / 1000;
    tv.tv_usec = (kConnectTimeoutMs % 1000) * 1000;
    if (select(0, nullptr, &wset, &eset, &tv) <= 0 || FD_ISSET(fd, &eset)) {
      return false;
    }
    int err = 0;
    int errlen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err),
                   &errlen) != 0 ||
        err != 0) {
      return false;
    }
  }
  u_long blocking = 0;
  ioctlsocket(fd, FIONBIO, &blocking);
  return true;
}

int SendBytes(SocketHandle fd, const char* data, size_t len) {
  return send(fd, data, static_cast<int>(len), 0);
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

void EnsureWinsockStarted() {}

void CloseSocketHandle(SocketHandle fd) {
  close(fd);
}

void SetIoTimeouts(SocketHandle fd) {
  timeval tv;
  tv.tv_sec = kIoTimeoutMs / 1000;
  tv.tv_usec = (kIoTimeoutMs % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#if defined(SO_NOSIGPIPE)
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

bool ConnectWithTimeout(SocketHandle fd, const sockaddr_in& addr) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int r = connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  if (r != 0) {
    if (errno != EINPROGRESS) {
      return false;
    }
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    timeval tv;
    tv.tv_sec = kConnectTimeoutMs / 1000;
    tv.tv_usec = (kConnectTimeoutMs % 1000) * 1000;
    if (select(fd + 1, nullptr, &wset, nullptr, &tv) <= 0) {
      return false;
    }
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
      return false;
    }
  }
  fcntl(fd, F_SETFL, flags);
  return true;
}

int SendBytes(SocketHandle fd, const char* data, size_t len) {
#if defined(MSG_NOSIGNAL)
  return static_cast<int>(send(fd, data, len, MSG_NOSIGNAL));
#else
  return static_cast<int>(send(fd, data, len, 0));
#endif
}
#endif  // BUILDFLAG(IS_WIN)

SocketHandle ToHandle(intptr_t value) {
  return static_cast<SocketHandle>(value);
}

intptr_t FromHandle(SocketHandle handle) {
  return static_cast<intptr_t>(handle);
}

}  // namespace

TorControlConnection::TorControlConnection() = default;

TorControlConnection::~TorControlConnection() {
  Close();
}

bool TorControlConnection::is_connected() const {
  return socket_ != -1;
}

void TorControlConnection::Close() {
  if (socket_ != -1) {
    CloseSocketHandle(ToHandle(socket_));
    socket_ = -1;
  }
}

bool TorControlConnection::Connect(uint16_t port) {
  Close();
  EnsureWinsockStarted();
  SocketHandle fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == kInvalidSocket) {
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(0x7F000001);  // 127.0.0.1 only.
  if (!ConnectWithTimeout(fd, addr)) {
    CloseSocketHandle(fd);
    return false;
  }
  SetIoTimeouts(fd);
  socket_ = FromHandle(fd);
  return true;
}

bool TorControlConnection::SendAll(std::string_view data) {
  if (socket_ == -1) {
    return false;
  }
  while (!data.empty()) {
    int n = SendBytes(ToHandle(socket_), data.data(), data.size());
    if (n <= 0) {
      Close();
      return false;
    }
    data.remove_prefix(static_cast<size_t>(n));
  }
  return true;
}

std::optional<std::string> TorControlConnection::ReadReply() {
  if (socket_ == -1) {
    return std::nullopt;
  }
  const base::TimeTicks deadline = base::TimeTicks::Now() + kReplyDeadline;
  std::string reply;
  char buf[4096];
  while (!control::IsCompleteReply(reply)) {
    if (reply.size() > kMaxReplyBytes || base::TimeTicks::Now() > deadline) {
      Close();
      return std::nullopt;
    }
    // recv() returns ssize_t on POSIX and int on winsock; <= 0 means closed,
    // error or SO_RCVTIMEO timeout.
    int n = static_cast<int>(recv(ToHandle(socket_), buf, sizeof(buf), 0));
    if (n <= 0) {
      Close();
      return std::nullopt;
    }
    reply.append(buf, static_cast<size_t>(n));
  }
  return reply;
}

std::optional<std::string> TorControlConnection::SendCommand(
    std::string_view command) {
  if (command.empty() ||
      command.find_first_of("\r\n") != std::string_view::npos) {
    return std::nullopt;
  }
  std::string line(command);
  line += "\r\n";
  if (!SendAll(line)) {
    return std::nullopt;
  }
  return ReadReply();
}

TorControlConnection::AuthResult TorControlConnection::ConnectAndAuthenticate(
    const TorControlEndpoint& endpoint,
    std::string* error) {
  auto fail = [this, error](AuthResult result, const char* why) {
    Close();
    if (error) {
      *error = why;
    }
    return result;
  };
  if (!endpoint.IsValid()) {
    return fail(AuthResult::kTransientFailure, "Tor control port unknown");
  }
  if (!Connect(endpoint.port)) {
    return fail(AuthResult::kTransientFailure,
                "Tor control port not reachable");
  }

  std::optional<std::string> reply = SendCommand("PROTOCOLINFO 1");
  if (!reply) {
    return fail(AuthResult::kTransientFailure, "no PROTOCOLINFO reply");
  }
  std::optional<control::ProtocolInfo> info =
      control::ParseProtocolInfo(*reply);
  if (!info) {
    return fail(AuthResult::kAuthenticationFailed,
                "control port sent an invalid PROTOCOLINFO reply");
  }
  // SAFECOOKIE only. NULL, HASHEDPASSWORD and plain COOKIE are never used:
  // plain COOKIE would hand our secret to whoever answered.
  if (!info->safe_cookie_auth) {
    return fail(AuthResult::kAuthenticationFailed,
                "control port does not offer SAFECOOKIE authentication");
  }

  // Our own CookieAuthFile, never a path suggested by the peer. Exactly 32
  // bytes per control-spec; anything else is not (yet) a valid cookie.
  std::string cookie;
  if (!base::ReadFileToStringWithMaxSize(endpoint.cookie_path, &cookie,
                                         control::kCookieSize + 1) ||
      cookie.size() != control::kCookieSize) {
    return fail(AuthResult::kTransientFailure,
                "Tor auth cookie not readable yet");
  }

  std::vector<uint8_t> nonce_bytes =
      crypto::RandBytesAsVector(control::kNonceSize);
  const std::string client_nonce(nonce_bytes.begin(), nonce_bytes.end());
  reply = SendCommand(control::BuildAuthChallengeCommand(client_nonce));
  if (!reply) {
    return fail(AuthResult::kTransientFailure, "no AUTHCHALLENGE reply");
  }
  std::optional<control::AuthChallenge> challenge =
      control::ParseAuthChallenge(*reply);
  if (!challenge) {
    return fail(AuthResult::kAuthenticationFailed,
                "control port rejected AUTHCHALLENGE SAFECOOKIE");
  }
  // The peer must prove it knows our cookie BEFORE we prove we do.
  if (!control::VerifySafeCookieServerHash(cookie, client_nonce,
                                           challenge->server_nonce,
                                           challenge->server_hash)) {
    return fail(AuthResult::kAuthenticationFailed,
                "control port failed SAFECOOKIE SERVERHASH verification "
                "(not MoltNet's Tor)");
  }
  reply = SendCommand(control::BuildSafeCookieAuthenticateCommand(
      cookie, client_nonce, challenge->server_nonce));
  if (!reply) {
    return fail(AuthResult::kTransientFailure, "no AUTHENTICATE reply");
  }
  if (!control::IsOkReply(*reply)) {
    return fail(AuthResult::kAuthenticationFailed,
                "Tor rejected SAFECOOKIE AUTHENTICATE");
  }
  return AuthResult::kOk;
}

}  // namespace tor
}  // namespace molt_ai
