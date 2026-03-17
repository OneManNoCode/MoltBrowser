// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Cross-Platform Update Manager for MoltBrowser.
// Unified facade that dispatches to the platform-specific update mechanism:
//   - macOS:   Sparkle framework
//   - Windows: WinSparkle DLL
//   - Linux:   Package manager detection + AppImage/Flatpak/Snap/apt/dnf
//
// Usage:
//   molt_ai::UpdateManager::Initialize();   // At browser startup
//   molt_ai::UpdateManager::CheckForUpdates();  // From menu item

#ifndef CHROME_BROWSER_MOLT_AI_UPDATE_UPDATE_MANAGER_H_
#define CHROME_BROWSER_MOLT_AI_UPDATE_UPDATE_MANAGER_H_

#include <string>

namespace molt_ai {

class UpdateManager {
 public:
  // Initialize the platform-specific update system.
  // Call once at browser startup.
  static void Initialize();

  // Trigger a manual update check.
  static void CheckForUpdates();

  // Clean up resources. Call at browser shutdown.
  static void Shutdown();

  // Returns true if an update mechanism is available on this platform.
  static bool IsAvailable();

  // Get the current app version.
  static std::string GetCurrentVersion();

  // Set the update feed URL (overrides compiled-in default).
  static void SetFeedURL(const std::string& url);

  // Get the platform name for update purposes.
  static std::string GetPlatformName();

 private:
  UpdateManager() = delete;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_UPDATE_UPDATE_MANAGER_H_
