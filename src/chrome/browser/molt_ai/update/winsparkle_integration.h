// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// WinSparkle Auto-Update Integration for MoltBrowser (Windows).
// Uses the WinSparkle library (https://winsparkle.org/) to check
// for updates via an appcast feed — same feed format as Sparkle on macOS.
//
// Architecture:
//   - WinSparkleIntegration is a singleton initialized at browser startup
//   - It reads the appcast URL from a compiled-in constant or registry
//   - WinSparkle handles the update UI, download, and installation
//   - Windows only — macOS uses Sparkle, Linux uses its own mechanism

#ifndef CHROME_BROWSER_MOLT_AI_UPDATE_WINSPARKLE_INTEGRATION_H_
#define CHROME_BROWSER_MOLT_AI_UPDATE_WINSPARKLE_INTEGRATION_H_

#include <string>

namespace molt_ai {

class WinSparkleIntegration {
 public:
  // Initialize WinSparkle updater. Call once at browser startup.
  // Configures appcast URL, app version, and company name.
  static void Initialize();

  // Manually trigger an update check (e.g., from Help > Check for Updates).
  static void CheckForUpdates();

  // Clean up WinSparkle resources. Call at browser shutdown.
  static void Shutdown();

  // Returns true if WinSparkle DLL is available and initialized.
  static bool IsAvailable();

  // Get the current app version string.
  static std::string GetCurrentVersion();

  // Set the appcast URL programmatically.
  static void SetFeedURL(const std::string& url);

 private:
  WinSparkleIntegration() = delete;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_UPDATE_WINSPARKLE_INTEGRATION_H_
