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

 private:
  friend class base::NoDestructor<TorManager>;

  TorManager();
  ~TorManager();

  // Resolve the data directory we hand to tor as `DataDirectory ...`.
  base::FilePath GetDataDir() const;
  base::FilePath GetTorrcPath() const;

  // Write the managed torrc to |GetTorrcPath()|. Idempotent.
  bool WriteTorrc();

  // Poll the control port until it answers AUTH, up to |total_ms|.
  // Runs on a worker thread.
  static TorLaunchResult WaitForBootstrap(int total_ms);

  base::Process child_;
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_TOR_MANAGER_H_
