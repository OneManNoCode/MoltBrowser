// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// TorControlConnection — a small BLOCKING client for MoltNet's own Tor
// control port. Every connection authenticates with SAFECOOKIE and verifies
// Tor's SERVERHASH first (see tor_control_protocol.h), so:
//   * we never talk to a control port that can't prove it holds the cookie
//     our Tor wrote (port squatters, a system Tor, ...);
//   * there is no NULL/no-auth fallback and no reading of system Tor cookies.
//
// Only ever use it on a base::MayBlock() thread-pool sequence.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_TOR_CONTROL_CONNECTION_H_
#define CHROME_BROWSER_MOLT_AI_TOR_TOR_CONTROL_CONNECTION_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"

namespace molt_ai {
namespace tor {

// Where to reach (and how to authenticate to) the managed Tor's control port.
// Snapshotted on the UI thread from TorManager and handed to worker tasks.
struct TorControlEndpoint {
  uint16_t port = 0;           // 127.0.0.1:<port>, chosen by `ControlPort auto`
  base::FilePath cookie_path;  // our CookieAuthFile
  bool IsValid() const { return port != 0 && !cookie_path.empty(); }
};

class TorControlConnection {
 public:
  enum class AuthResult {
    kOk,
    // Nothing listening yet / connection dropped — worth retrying while Tor
    // is starting up.
    kTransientFailure,
    // Something answered but failed SAFECOOKIE (wrong/missing SERVERHASH,
    // no SAFECOOKIE support, rejected AUTHENTICATE): not our Tor. Never
    // retried, never downgraded to a weaker auth method.
    kAuthenticationFailed,
  };

  TorControlConnection();
  ~TorControlConnection();

  TorControlConnection(const TorControlConnection&) = delete;
  TorControlConnection& operator=(const TorControlConnection&) = delete;

  // Connects to 127.0.0.1:|endpoint.port| and runs PROTOCOLINFO +
  // AUTHCHALLENGE SAFECOOKIE + AUTHENTICATE. |error| gets a human-readable
  // reason on failure.
  AuthResult ConnectAndAuthenticate(const TorControlEndpoint& endpoint,
                                    std::string* error);

  // Sends |command| (CRLF appended) and returns one complete reply, or
  // nullopt on I/O failure / timeout / oversized reply. |command| must not
  // contain CR or LF (rejected, so callers can't smuggle a second command).
  std::optional<std::string> SendCommand(std::string_view command);

  bool is_connected() const;
  void Close();

 private:
  bool Connect(uint16_t port);
  bool SendAll(std::string_view data);
  std::optional<std::string> ReadReply();

  // Platform socket (SOCKET on Windows, an fd elsewhere) stored type-erased
  // so this header doesn't pull in winsock2.h. -1 == INVALID_SOCKET / no fd.
  intptr_t socket_ = -1;
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_TOR_CONTROL_CONNECTION_H_
