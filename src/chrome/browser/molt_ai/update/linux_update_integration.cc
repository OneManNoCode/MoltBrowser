// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/update/linux_update_integration.h"

#include "base/logging.h"

#if BUILDFLAG(IS_LINUX)
#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/process/launch.h"
#include "base/strings/string_util.h"
#endif

namespace {
#if BUILDFLAG(IS_LINUX)
molt_ai::LinuxPackageFormat g_format = molt_ai::LinuxPackageFormat::kUnknown;
bool g_initialized = false;
std::string g_release_url = "https://github.com/OneManNoCode/MoltBrowser/releases/latest";

molt_ai::LinuxPackageFormat DetectPackageFormat() {
  // Check if running as AppImage
  auto env = base::Environment::Create();
  std::string appimage_path;
  if (env->GetVar("APPIMAGE", &appimage_path)) {
    return molt_ai::LinuxPackageFormat::kAppImage;
  }

  // Check if installed via Flatpak
  std::string flatpak_id;
  if (env->GetVar("FLATPAK_ID", &flatpak_id)) {
    return molt_ai::LinuxPackageFormat::kFlatpak;
  }

  // Check if installed via Snap
  std::string snap;
  if (env->GetVar("SNAP", &snap)) {
    return molt_ai::LinuxPackageFormat::kSnap;
  }

  // Check for dpkg (Debian/Ubuntu)
  if (base::PathExists(base::FilePath("/var/lib/dpkg/info/moltbrowser.list"))) {
    return molt_ai::LinuxPackageFormat::kDeb;
  }

  // Check for rpm
  if (base::PathExists(base::FilePath("/usr/share/doc/moltbrowser"))) {
    // Could be either deb or rpm; check rpm db
    std::string output;
    if (base::GetAppOutput({"rpm", "-q", "moltbrowser"}, &output)) {
      return molt_ai::LinuxPackageFormat::kRpm;
    }
  }

  return molt_ai::LinuxPackageFormat::kTarball;
}
#endif  // BUILDFLAG(IS_LINUX)
}  // namespace

namespace molt_ai {

// static
void LinuxUpdateIntegration::Initialize() {
#if BUILDFLAG(IS_LINUX)
  if (g_initialized)
    return;
  g_format = DetectPackageFormat();
  g_initialized = true;

  const char* format_name = "unknown";
  switch (g_format) {
    case LinuxPackageFormat::kAppImage: format_name = "AppImage"; break;
    case LinuxPackageFormat::kFlatpak: format_name = "Flatpak"; break;
    case LinuxPackageFormat::kDeb: format_name = "deb"; break;
    case LinuxPackageFormat::kRpm: format_name = "rpm"; break;
    case LinuxPackageFormat::kSnap: format_name = "Snap"; break;
    case LinuxPackageFormat::kTarball: format_name = "tarball"; break;
    default: break;
  }
  LOG(INFO) << "Linux update system initialized — package format: "
            << format_name;
#else
  LOG(WARNING) << "Linux update integration is Linux-only";
#endif
}

// static
void LinuxUpdateIntegration::CheckForUpdates() {
#if BUILDFLAG(IS_LINUX)
  if (!g_initialized)
    return;

  switch (g_format) {
    case LinuxPackageFormat::kFlatpak: {
      std::string output;
      base::GetAppOutput({"flatpak", "update", "--user",
                          "com.geneye.MoltBrowser"}, &output);
      break;
    }
    case LinuxPackageFormat::kSnap: {
      std::string output;
      base::GetAppOutput({"snap", "refresh", "moltbrowser"}, &output);
      break;
    }
    default:
      // For AppImage, deb, rpm, tarball — open release page in browser
      LOG(INFO) << "Update check: visit " << g_release_url;
      break;
  }
#endif
}

// static
bool LinuxUpdateIntegration::IsAvailable() {
#if BUILDFLAG(IS_LINUX)
  return g_initialized;
#else
  return false;
#endif
}

// static
LinuxPackageFormat LinuxUpdateIntegration::GetPackageFormat() {
#if BUILDFLAG(IS_LINUX)
  return g_format;
#else
  return LinuxPackageFormat::kUnknown;
#endif
}

// static
std::string LinuxUpdateIntegration::GetCurrentVersion() {
  return "0.1.0";
}

// static
void LinuxUpdateIntegration::SetReleaseURL(const std::string& url) {
#if BUILDFLAG(IS_LINUX)
  g_release_url = url;
#endif
}

}  // namespace molt_ai
