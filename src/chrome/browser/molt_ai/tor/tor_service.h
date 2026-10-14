// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// TorService — read-only queries against MoltNet's OWN Tor (the one
// TorManager launched), over a SAFECOOKIE-authenticated control connection
// (TorControlConnection):
//   - Probe(): authenticate, then GETINFO version.
//   - GetCircuits(): GETINFO circuit-status -> built circuits + relay hops.
//   - GetCircuitsEnriched(): also resolves each hop's IP (GETINFO ns/id/<fp>)
//     and country (GETINFO ip-to-country/<ip>) from Tor's own consensus and
//     GeoIP DB — no third-party IP-lookup service is contacted.
//
// There is deliberately no support for a system Tor: no probing of
// 127.0.0.1:9051, no NULL/password auth, no reading of /var/lib/tor cookies.
// If MoltNet's Tor isn't running, every query reports "not running".
//
// Threading: public methods are called on the UI thread (they snapshot the
// control endpoint from TorManager there); the blocking socket work runs on
// a MayBlock() thread-pool task and the result bounces back.

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
  std::string control_port_addr;  // "127.0.0.1:<auto port>"
  std::string socks_port_addr;    // "127.0.0.1:<auto port>"
};

struct TorRelay {
  TorRelay();
  TorRelay(const TorRelay&);
  TorRelay(TorRelay&&);
  TorRelay& operator=(const TorRelay&);
  TorRelay& operator=(TorRelay&&);
  ~TorRelay();

  std::string fingerprint;  // 40-hex uppercase
  std::string nickname;     // operator-chosen name (no privacy guarantee)
  // Phase B.2 enrichment. Filled by GetCircuitsEnriched() via
  // GETINFO ns/id/<fp> (resolves to consensus document with IP) and
  // GETINFO ip-to-country/<ip>. Empty if enrichment didn't run or
  // couldn't resolve.
  std::string ip;
  std::string country;      // ISO 3166-1 alpha-2 from Tor's GeoIP DB
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

  // Probe MoltNet's Tor (authenticated). Reports not-running immediately if
  // TorManager has no running Tor.
  void Probe(base::OnceCallback<void(TorStatus)> on_done);

  // Fetch current circuit list. Each circuit comes with its relay
  // hops in build order (guard first, exit last).
  void GetCircuits(
      base::OnceCallback<void(std::vector<TorCircuit>)> on_done);

  // Like GetCircuits() but also resolves each hop's IP (via
  // GETINFO ns/id/<fingerprint>) and country (via
  // GETINFO ip-to-country/<ip>). Costs N+1 extra round-trips per
  // unique relay (~50ms total over local socket for a typical
  // 3-hop circuit). Used by the visualizer to show flags.
  void GetCircuitsEnriched(
      base::OnceCallback<void(std::vector<TorCircuit>)> on_done);

 private:
  friend class base::NoDestructor<TorService>;

  TorService();
  ~TorService();
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_TOR_SERVICE_H_
