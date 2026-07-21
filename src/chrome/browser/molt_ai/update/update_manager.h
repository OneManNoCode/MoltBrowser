// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Cross-platform in-app auto-updater for MoltBrowser.
//
// Single source of truth: the GitHub Releases API
//   https://api.github.com/repos/OneManNoCode/MoltBrowser/releases/latest
// (the same release we publish to), so the "what is the latest version"
// check works identically on macOS, Windows, and Linux with no appcast/
// website dependency. The manager:
//   1. Checks for updates on launch (delayed) + on a periodic timer.
//   2. Compares the release tag against kMoltBrowserVersion.
//   3. When newer + auto-update is on, downloads the platform asset in the
//      background to a cache dir.
//   4. Installs by launching the official signed installer (silent where the
//      platform allows) and relaunching.
//
// Lives on the UI thread (browser process). All methods must be called on the
// UI thread. To avoid a //chrome/browser <-> molt_ai dependency cycle, the
// network factory is injected via Initialize() (the caller — e.g. the toolbar
// — lives in the browser layer and has profile access), and the actual browser
// exit on install is performed by the caller, not here.

#ifndef CHROME_BROWSER_MOLT_AI_UPDATE_UPDATE_MANAGER_H_
#define CHROME_BROWSER_MOLT_AI_UPDATE_UPDATE_MANAGER_H_

#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/timer/timer.h"

namespace net {
class HttpResponseHeaders;
}  // namespace net

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace molt_ai {

// High-level state of the updater, surfaced to the UI.
enum class UpdateState {
  kIdle,         // Nothing has happened yet.
  kChecking,     // A version check is in flight.
  kUpToDate,     // Checked; running the latest version.
  kAvailable,    // A newer version exists (not yet downloaded).
  kDownloading,  // Downloading the new installer.
  kDownloaded,   // Installer downloaded; ready to install.
  kInstalling,   // Installer launched; app is about to quit/relaunch.
  kCheckFailed,  // Version check couldn't complete (offline/rate-limited).
                 // Soft state: retried automatically; NOT a broken install.
  kError,        // A download/install operation failed (see error()).
};

class UpdateManager {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // Fired on any change to state/progress/version. Observers re-read the
    // accessors below.
    virtual void OnUpdateChanged() {}
  };

  static UpdateManager* Get();

  UpdateManager(const UpdateManager&) = delete;
  UpdateManager& operator=(const UpdateManager&) = delete;

  // Idempotent. Records the current version, stores the network factory, and
  // schedules the first (delayed) check + the periodic timer. Safe to call
  // from multiple entry points (e.g. each browser window); only the first
  // call takes effect.
  void Initialize(scoped_refptr<network::SharedURLLoaderFactory> factory);

  // Trigger a version check now. |user_initiated| only affects logging/UX.
  void CheckForUpdates(bool user_initiated);

  // Begin downloading the discovered update asset. No-op unless an asset is
  // known and we're not already busy.
  void DownloadUpdate();

  // Launch the downloaded installer (silent where supported). Returns true if
  // the installer process was launched, in which case the CALLER must exit the
  // browser (e.g. chrome::AttemptExit) so the detached installer can replace
  // the app and relaunch it. No-op (returns false) unless state is kDownloaded.
  bool InstallUpdate();

  // Enable/disable fully-automatic background download.
  void SetAutoUpdate(bool enabled);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // ---- State accessors (UI thread) ----
  UpdateState state() const { return state_; }
  const std::string& current_version() const { return current_version_; }
  const std::string& latest_version() const { return latest_version_; }
  const std::string& release_notes() const { return release_notes_; }
  const std::string& release_url() const { return release_url_; }
  const std::string& error() const { return error_; }
  // 0-100 while downloading; -1 if total size is unknown.
  int download_percent() const { return download_percent_; }
  bool update_available() const {
    return state_ == UpdateState::kAvailable ||
           state_ == UpdateState::kDownloading ||
           state_ == UpdateState::kDownloaded ||
           state_ == UpdateState::kInstalling;
  }
  bool auto_update() const { return auto_update_; }
  bool checking() const { return state_ == UpdateState::kChecking; }

 private:
  friend class base::NoDestructor<UpdateManager>;
  UpdateManager();
  ~UpdateManager();

  void OnCheckResponse(bool user_initiated, std::optional<std::string> body);
  void OnDownloadProgress(uint64_t current);
  void OnDownloadComplete(base::FilePath expected_path, base::FilePath path);
  void OnPeriodicTimer();

  // Rate-limit-proof fallback when the Releases API check fails: probe the
  // releases/latest WEB url (a 302 to .../releases/tag/<tag>, served by the
  // site frontend with no meaningful rate limit) and read the tag from the
  // redirect target. Headers-only; no body is downloaded.
  void StartFallbackCheck(bool user_initiated);
  void OnFallbackHeaders(bool user_initiated,
                         scoped_refptr<net::HttpResponseHeaders> headers);

  // Both the API and the fallback probe failed. Keep any previously-known
  // result rather than alarming the user; only surface the soft
  // kCheckFailed state when we have nothing better to show.
  void ResolveCheckFailure();

  void SetState(UpdateState state);
  void SetError(const std::string& message);
  // Soft failure of the version CHECK (offline, rate-limited). Unlike
  // SetError this never means anything is broken; it retries automatically.
  void SetCheckFailed(const std::string& message);
  void NotifyChanged();

  // Canonical (versionless) release asset filename for this platform, e.g.
  // "MoltBrowser-macOS-arm64.dmg" — the fixed names our release pipeline
  // uploads. Empty where the canonical name isn't stable (Windows).
  std::string PlatformAssetName() const;

  // Substring identifying this platform's release asset (e.g. "Setup.exe").
  // Empty if the platform is unsupported.
  std::string PlatformAssetMatcher() const;

  // Platform-specific install of |installer_path|. Returns true if the
  // installer process was launched. Defined per-platform in update_manager.cc.
  bool LaunchInstaller(const base::FilePath& installer_path);

  base::FilePath DownloadDir() const;

  UpdateState state_ = UpdateState::kIdle;
  std::string current_version_;
  std::string latest_version_;
  std::string latest_tag_;
  std::string release_notes_;
  std::string release_url_;
  std::string asset_url_;
  std::string asset_name_;
  int64_t asset_size_ = 0;
  std::string error_;
  int download_percent_ = 0;
  bool auto_update_ = true;
  bool initialized_ = false;
  base::FilePath downloaded_path_;

  scoped_refptr<network::SharedURLLoaderFactory> factory_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::RepeatingTimer periodic_timer_;
  base::OneShotTimer initial_check_timer_;
  base::ObserverList<Observer> observers_;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_UPDATE_UPDATE_MANAGER_H_
