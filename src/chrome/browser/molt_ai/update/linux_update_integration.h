// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Linux Auto-Update Integration for MoltBrowser.
// Supports multiple distribution formats:
//   - AppImage: Self-update via appimagetool zsync
//   - Flatpak: Update via flatpak CLI
//   - Deb/RPM: Update via apt/dnf repository
//   - Snap: Update via snapd
//
// The update mechanism is auto-detected based on how MoltBrowser was installed.
// Falls back to checking a release URL for manual download if no package
// manager integration is available.

#ifndef CHROME_BROWSER_MOLT_AI_UPDATE_LINUX_UPDATE_INTEGRATION_H_
#define CHROME_BROWSER_MOLT_AI_UPDATE_LINUX_UPDATE_INTEGRATION_H_

#include <string>

namespace molt_ai {

enum class LinuxPackageFormat {
  kUnknown,
  kAppImage,
  kFlatpak,
  kDeb,
  kRpm,
  kSnap,
  kTarball,
};

class LinuxUpdateIntegration {
 public:
  // Initialize the update system. Detects package format automatically.
  static void Initialize();

  // Check for updates. Opens browser to download page if no native
  // package manager update is available.
  static void CheckForUpdates();

  // Returns true if an update mechanism is available.
  static bool IsAvailable();

  // Get the detected package format.
  static LinuxPackageFormat GetPackageFormat();

  // Get the current app version string.
  static std::string GetCurrentVersion();

  // Set the release check URL.
  static void SetReleaseURL(const std::string& url);

 private:
  LinuxUpdateIntegration() = delete;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_UPDATE_LINUX_UPDATE_INTEGRATION_H_
