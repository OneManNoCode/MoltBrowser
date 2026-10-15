// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// TorManager — runs and supervises MoltNet's own Tor as a child process.
//
// Hardening (see docs/MOLTNET_COMPLIANCE.md):
//   * Only the Tor bundled with MoltBrowser is launched, or on Linux (whose
//     packages don't bundle Tor) the distribution's /usr/bin/tor or
//     /usr/sbin/tor if root owns it and every directory above it. A
//     different binary requires an explicit opt-in
//     (--moltnet-tor-binary=<absolute path>); there is no Homebrew/PATH
//     fallback. All candidates go through FindHelperBinary (audit S18).
//   * `SocksPort auto` + `ControlPort auto` + `ControlPortWriteToFile`: ports
//     are picked by Tor and read back, so nothing squatting on 9050/9051 can
//     impersonate it. A private --defaults-torrc stops a system-wide
//     torrc-defaults from injecting options.
//   * SAFECOOKIE auth with SERVERHASH verification on every control
//     connection (TorControlConnection).
//   * Ownership: `__OwningControllerProcess <browser pid>` plus TAKEOWNERSHIP
//     on a control connection held open for Tor's lifetime, so Tor exits
//     when the browser does (even on a crash).
//   * The exit-country pin is session-only (never persisted) and StrictNodes
//     is set only while a country is selected.
//
// Threading: public methods are called on the UI thread. All file, process
// and socket work runs on MayBlock() thread-pool tasks.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_TOR_MANAGER_H_
#define CHROME_BROWSER_MOLT_AI_TOR_TOR_MANAGER_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process.h"
#include "chrome/browser/molt_ai/tor/tor_control_connection.h"

namespace base {
template <typename T>
class NoDestructor;
}  // namespace base

namespace molt_ai {
namespace tor {

struct TorBootstrapResult;
struct TorSpawnResult;

struct TorLaunchResult {
  bool success = false;
  std::string error;        // human-readable on failure
  std::string binary_path;  // resolved path of the tor binary we launched
  int pid = 0;              // child pid on success
};

// The tor binary MoltNet would run (TorManager::FindTorBinary()).
struct TorBinaryInfo {
  TorBinaryInfo();
  TorBinaryInfo(const TorBinaryInfo&);
  TorBinaryInfo& operator=(const TorBinaryInfo&);
  ~TorBinaryInfo();

  // Canonical path (symlinks resolved); empty if there is no usable tor.
  base::FilePath path;
  // True iff |path| is the tor bundled with MoltBrowser.
  bool bundled = false;
  // When |path| is empty: a user-facing explanation of what to do, e.g.
  // "Tor isn't installed. Install the tor package ..." on Linux.
  std::string error;
};

// How MoltNet reaches the Tor network. Persisted (it is local configuration,
// not browsing data) in <tor data dir>/bridges.json and re-validated on load.
struct BridgeSettings {
  enum class Mode {
    kNone,       // Connect to Tor directly.
    kSnowflake,  // Built-in snowflake bridges (bundled pt_config.json).
    kObfs4,      // Built-in obfs4 bridges (bundled pt_config.json).
    kCustom,     // User-pasted bridge lines (obfs4 / snowflake only).
  };

  BridgeSettings();
  BridgeSettings(const BridgeSettings&);
  BridgeSettings& operator=(const BridgeSettings&);
  ~BridgeSettings();

  Mode mode = Mode::kNone;
  // Raw pasted text for kCustom (validated with ParseBridgeLines).
  std::string custom_lines;
};

// "none" | "snowflake" | "obfs4" | "custom" (the WebUI wire format).
std::string BridgeModeToString(BridgeSettings::Mode mode);
std::optional<BridgeSettings::Mode> BridgeModeFromString(std::string_view s);

class TorManager {
 public:
  // Explicit opt-in to run a non-bundled tor: --moltnet-tor-binary=<abs path>.
  static constexpr char kTorBinarySwitch[] = "moltnet-tor-binary";

  using DoneCallback = base::OnceCallback<void(bool ok, std::string error)>;

  static TorManager* Get();

  TorManager(const TorManager&) = delete;
  TorManager& operator=(const TorManager&) = delete;

  // The tor MoltNet runs: the binary named by --moltnet-tor-binary when that
  // switch is present (and only then); otherwise the bundled tor
  // (MoltBrowser.app/Contents/Resources/tor/tor on macOS,
  // <install dir>/tor/tor[.exe] elsewhere), then on Linux a root-owned
  // /usr/bin/tor or /usr/sbin/tor. Every candidate must pass
  // FindHelperBinary's checks. Never searches PATH or Homebrew. A few
  // stat() calls; fine on the UI thread.
  TorBinaryInfo FindTorBinary() const;

  // FindTorBinary().path: empty if there is no usable tor.
  base::FilePath ResolveTorBinary() const;

  // True iff FindTorBinary() is the bundled binary.
  bool IsUsingBundledTor() const;

  // Bundled tor directory (geoip, pluggable_transports/). May be empty.
  base::FilePath GetBundledTorDir() const;

  // True once our child Tor has bootstrapped: its ports are known and it had
  // an established circuit. (A Tor that later dies keeps reporting true until
  // Stop(); callers that need liveness probe via TorService.)
  bool IsRunning() const;

  // True while a Launch() is in flight.
  bool IsStarting() const { return launching_; }

  // SOCKS port of the running Tor (127.0.0.1), or 0.
  uint16_t socks_port() const { return socks_port_; }

  // Control endpoint of the running Tor; invalid unless IsRunning().
  TorControlEndpoint GetControlEndpoint() const;

  // Starts Tor (no-op if already running) and calls |on_ready| once it has
  // an established circuit, or with an error (bootstrap timeout, bad config,
  // failed authentication). On failure the daemon is torn down. Concurrent
  // calls are coalesced.
  void Launch(base::OnceCallback<void(TorLaunchResult)> on_ready);

  // Stops our child Tor: closes the owning control connection (Tor exits on
  // TAKEOWNERSHIP) and terminates the process.
  void Stop();

  // Notified (UI thread) whenever Tor starts, stops, or changes ports.
  base::CallbackListSubscription AddStateChangedCallback(
      base::RepeatingClosure callback);

  // Sends SIGNAL NEWNYM (fresh circuits for new streams) and reports whether
  // Tor acknowledged it. Fails if Tor isn't running.
  void SendNewIdentitySignal(DoneCallback done);

  // ---- Exit-country selection ----
  // Lowercase ISO 3166-1 alpha-2, or "" for any exit. Session-only: kept in
  // memory, never written to disk, so it resets to "any" on restart. Applied
  // to a running Tor with SETCONF ExitNodes/StrictNodes (or RESETCONF) +
  // NEWNYM. Pinning a country shrinks the set of exits you blend in with.
  void SetExitCountry(const std::string& country_code,
                      DoneCallback done = DoneCallback());
  std::string GetExitCountry() const { return exit_country_; }
  std::vector<std::string> GetAvailableExitCountries() const;

  // ---- Bridges ----
  void GetBridgeSettings(base::OnceCallback<void(BridgeSettings)> callback);
  // Validates (custom lines must all be valid) and persists. Takes effect the
  // next time Tor starts.
  void SetBridgeSettings(const BridgeSettings& settings, DoneCallback done);

 private:
  friend class base::NoDestructor<TorManager>;

  TorManager();
  ~TorManager();

  base::FilePath GetDataDir() const;

  void OnSpawned(uint64_t generation,
                 std::string binary_path,
                 std::unique_ptr<TorSpawnResult> result);
  void OnBootstrapped(uint64_t generation,
                      std::string binary_path,
                      std::unique_ptr<TorBootstrapResult> result);
  void FinishLaunch(const TorLaunchResult& result);
  // Kills |child_| (if any) off the UI thread and closes the owner
  // connection. Does not touch pending launches.
  void TearDownChild();
  void NotifyStateChanged();

  base::Process child_;
  bool launching_ = false;
  // Bumped by Stop(); stale launch replies are discarded.
  uint64_t generation_ = 0;
  std::vector<base::OnceCallback<void(TorLaunchResult)>> pending_launches_;

  uint16_t socks_port_ = 0;
  uint16_t control_port_ = 0;
  // Held open for Tor's lifetime after TAKEOWNERSHIP; closing it makes Tor
  // exit. Only ever destroyed (on a worker thread), never used for I/O on
  // the UI thread.
  std::unique_ptr<TorControlConnection> owner_connection_;

  // Session-only exit-country constraint ("" = any).
  std::string exit_country_;

  base::RepeatingClosureList state_callbacks_;
  base::WeakPtrFactory<TorManager> weak_factory_{this};
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_TOR_MANAGER_H_
