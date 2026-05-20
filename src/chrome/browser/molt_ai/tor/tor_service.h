// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// TorService — process-wide singleton that talks to a locally-running
// Tor over its control port (default 127.0.0.1:9051). Phase B.1.
//
// What this does:
//   - Probe(): TCP-connect to 127.0.0.1:9051, AUTHENTICATE (NULL or
//     COOKIE), then GETINFO version. Returns running/version.
//   - GetCircuits(): query GETINFO circuit-status, parse the list of
//     built circuits + their relay hops (nickname + fingerprint).
//
// What this does NOT do yet (Phase B.2):
//   - Bundle the tor binary. Users install it via Homebrew / package
//     manager and add `ControlPort 9051` to torrc.
//   - Resolve relay IPs / geolocate them. The control protocol's
//     GETINFO ns/id/<fp> can return IPs but it's a per-relay query;
//     we'll batch this when GeoIP lands.
//
// Threading: every public method is called on the UI thread. Each
// public method dispatches the actual socket work to ThreadPool with
// MayBlock(), then bounces the result back. The blocking
// implementations live in the .cc as static helpers — they do their
// own connect/auth/quit per call (no persistent connection yet, since
// we don't subscribe to events yet).

#ifndef CHROME_BROWSER_MOLT_AI_TOR_TOR_SERVICE_H_
#define CHROME_BROWSER_MOLT_AI_TOR_TOR_SERVICE_H_

#include <string>
#include <vector>

#include "base/functional/callback.h"

namespace base {
template <typename T>
class NoDestructor;
}  // namespace base

namespace molt_ai {
namespace tor {

struct TorStatus {
  TorStatus();
  TorStatus(const TorStatus&);
  TorStatus(TorStatus&&);
  TorStatus& operator=(const TorStatus&);
  TorStatus& operator=(TorStatus&&);
  ~TorStatus();

  bool running = false;
  std::string version;            // e.g. "0.4.8.10"
  std::string error;              // human-readable if !running
  std::string control_port_addr;  // "127.0.0.1:9051"
  std::string socks_port_addr;    // "127.0.0.1:9050" (assumed default)
};

struct TorRelay {
  std::string fingerprint;  // 40-hex uppercase
  std::string nickname;     // operator-chosen name (no privacy guarantee)
};

struct TorCircuit {
  TorCircuit();
  TorCircuit(const TorCircuit&);
  TorCircuit(TorCircuit&&);
  TorCircuit& operator=(const TorCircuit&);
  TorCircuit& operator=(TorCircuit&&);
  ~TorCircuit();

  std::string id;           // integer as string, per protocol
  std::string state;        // "BUILT" | "EXTENDED" | "FAILED" | etc.
  std::string purpose;      // "GENERAL" | "DIR_FETCH" | ...
  std::vector<TorRelay> hops;
};

class TorService {
 public:
  static TorService* Get();

  TorService(const TorService&) = delete;
  TorService& operator=(const TorService&) = delete;

  // Probe local Tor. Cheap (~5ms when Tor is running, ~50ms timeout
  // when it isn't — we use a short connect timeout).
  void Probe(base::OnceCallback<void(TorStatus)> on_done);

  // Fetch current circuit list. Each circuit comes with its relay
  // hops in build order (guard first, exit last).
  void GetCircuits(
      base::OnceCallback<void(std::vector<TorCircuit>)> on_done);

 private:
  friend class base::NoDestructor<TorService>;

  TorService();
  ~TorService();
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_TOR_SERVICE_H_
