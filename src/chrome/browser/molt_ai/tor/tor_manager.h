// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// TorManager — finds a locally-installed tor binary and runs it as a
// child process of MoltBrowser with a managed torrc.
//
// Phase B.2 scope (this file): the user installs tor themselves
// (`brew install tor`, `apt-get install tor`, etc.) and MoltBrowser
// auto-launches and supervises it on demand. We generate a private
// torrc with cookie auth + a dedicated data directory so we don't
// step on a system-managed Tor instance.
//
// Phase B.3 (later): bundle the tor binary inside MoltBrowser.app so
// nothing has to be installed. That requires codesign + notarization
// work in the packaging scripts, separate from this code.
//
// Threading: public methods are called on the UI thread. The actual
// fork/exec runs on a MayBlock thread-pool task; completion bounces
// back on the UI thread.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_TOR_MANAGER_H_
#define CHROME_BROWSER_MOLT_AI_TOR_TOR_MANAGER_H_

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/process/process.h"

namespace base {
template <typename T>
class NoDestructor;
}  // namespace base

namespace molt_ai {
namespace tor {

struct TorLaunchResult {
  bool success = false;
  std::string error;       // human-readable on failure
  std::string binary_path; // resolved path of the tor binary we launched
  int pid = 0;             // child pid on success
};

class TorManager {
 public:
  static TorManager* Get();

  TorManager(const TorManager&) = delete;
  TorManager& operator=(const TorManager&) = delete;

  // Look for a tor binary. Resolution order:
  //   1. Bundled at MoltBrowser.app/Contents/Resources/tor/tor (the
  //      preferred path — produced by scripts/bundle-tor.sh during
  //      build, so users don't have to install anything).
  //   2. System paths (/opt/homebrew/bin/tor, /usr/local/bin/tor,
  //      /usr/bin/tor, /opt/local/bin/tor). Lets power users override
  //      the bundle with their own tor if they want.
  // Returns empty FilePath if none found. Sync, cheap.
  base::FilePath ResolveTorBinary() const;

  // Returns true iff the binary at ResolveTorBinary() came from our
  // own .app bundle (as opposed to the user's system). Used by the
  // UI to render "bundled tor" vs. "system tor" in /tor status.
  bool IsUsingBundledTor() const;

  // Path to the bundled tor directory (Contents/Resources/tor/). May
  // be empty if we can't resolve the .app location. The GeoIP files
  // live in this directory.
  base::FilePath GetBundledTorDir() const;

  // Returns true iff we currently have a child tor process running
  // (or believe we do).
  bool IsRunning() const { return child_.IsValid(); }

  // Spawn tor with our managed torrc. No-op (returns success) if a
  // child is already running. The callback fires after we've polled
  // the control port and confirmed Tor is reachable, with a generous
  // timeout (Tor bootstrap can take ~15-30s on a cold start).
  void Launch(base::OnceCallback<void(TorLaunchResult)> on_ready);

  // Stop our child tor. Tries control-port SIGNAL HALT first for a
  // clean shutdown; falls back to SIGTERM after a short grace period.
  void Stop();

  // ---- Exit-country selection ("VPN-style" exit relay control) ----
  //
  // Constrain which country Tor uses for the exit relay. |country_code|
  // is a lowercase ISO 3166-1 alpha-2 code ("us", "de", "jp", ...).
  // Pass "" to clear the constraint (Tor picks any exit). The code is
  // stored in memory, best-effort persisted to a small file in the Tor
  // data dir, and applied by rewriting the torrc (adding/removing
  // `ExitNodes {<cc>}` + `StrictNodes 1`). If Tor is currently running
  // we reload it in place (SIGNAL RELOAD then SIGNAL NEWNYM to rebuild
  // circuits through the new exit); otherwise the change takes effect
  // on the next Launch(). Called on the UI thread.
  void SetExitCountry(const std::string& country_code);

  // Current exit-country constraint, or "" if none is set. Reads the
  // in-memory value (which is hydrated from the persisted file on first
  // use). Lowercase ISO alpha-2.
  std::string GetExitCountry() const;

  // A curated list of commonly-used exit-country codes (lowercase ISO
  // alpha-2) suitable for populating a picker UI. This is a static
  // list, not a live query of the consensus; see the .cc note about a
  // future GETINFO-based dynamic list.
  std::vector<std::string> GetAvailableExitCountries() const;

 private:
  friend class base::NoDestructor<TorManager>;

  TorManager();
  ~TorManager();

  // Resolve the data directory we hand to tor as `DataDirectory ...`.
  base::FilePath GetDataDir() const;
  base::FilePath GetTorrcPath() const;

  // File under the Tor data dir where we persist the selected exit
  // country across restarts (a single lowercase ISO code, no newline).
  base::FilePath GetExitCountryPath() const;

  // Lazily load |exit_country_| from disk on first access. Marks
  // |exit_country_loaded_| so we only hit the disk once. const because
  // it only mutates mutable cache fields.
  void EnsureExitCountryLoaded() const;

  // Write the managed torrc to |GetTorrcPath()|. Idempotent. Includes
  // the ExitNodes/StrictNodes lines when |exit_country_| is non-empty.
  bool WriteTorrc();

  // Poll the control port until it answers AUTH, up to |total_ms|.
  // Runs on a worker thread.
  static TorLaunchResult WaitForBootstrap(int total_ms);

  base::Process child_;

  // Selected exit-country (lowercase ISO alpha-2), or "" for "any".
  // Mutable + the loaded flag so the const getters can hydrate from
  // disk on first use without forcing callers through a non-const path.
  mutable std::string exit_country_;
  mutable bool exit_country_loaded_ = false;
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_TOR_MANAGER_H_
