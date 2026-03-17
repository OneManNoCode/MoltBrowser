// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Sparkle Auto-Update Integration for MoltBrowser.
// Checks for updates via an appcast feed and triggers Sparkle's UI
// to download and install new versions.
//
// Architecture:
//   - SparkleIntegration is a singleton initialized at browser startup
//   - It reads the appcast URL from Info.plist (SUFeedURL key)
//   - Sparkle framework handles the update UI, download, and installation
//   - On macOS only — other platforms use their native update mechanisms

#ifndef CHROME_BROWSER_MOLT_AI_UPDATE_SPARKLE_INTEGRATION_H_
#define CHROME_BROWSER_MOLT_AI_UPDATE_SPARKLE_INTEGRATION_H_

#include <string>

namespace molt_ai {

class SparkleIntegration {
 public:
  // Initialize the Sparkle updater. Call once at browser startup.
  // Reads SUFeedURL from Info.plist to determine the appcast endpoint.
  static void Initialize();

  // Manually trigger an update check (e.g., from "Check for Updates" menu).
  static void CheckForUpdates();

  // Returns true if Sparkle framework is available and initialized.
  static bool IsAvailable();

  // Get the current app version string.
  static std::string GetCurrentVersion();

  // Set the appcast URL programmatically (overrides Info.plist).
  static void SetFeedURL(const std::string& url);

 private:
  SparkleIntegration() = delete;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_UPDATE_SPARKLE_INTEGRATION_H_
