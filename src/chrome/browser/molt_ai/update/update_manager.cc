// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/update/update_manager.h"

#include "base/logging.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_MAC)
#include "chrome/browser/molt_ai/update/sparkle_integration.h"
#elif BUILDFLAG(IS_WIN)
#include "chrome/browser/molt_ai/update/winsparkle_integration.h"
#elif BUILDFLAG(IS_LINUX)
#include "chrome/browser/molt_ai/update/linux_update_integration.h"
#endif

namespace molt_ai {

// static
void UpdateManager::Initialize() {
#if BUILDFLAG(IS_MAC)
  SparkleIntegration::Initialize();
#elif BUILDFLAG(IS_WIN)
  WinSparkleIntegration::Initialize();
#elif BUILDFLAG(IS_LINUX)
  LinuxUpdateIntegration::Initialize();
#else
  LOG(INFO) << "No update mechanism available on this platform";
#endif
}

// static
void UpdateManager::CheckForUpdates() {
#if BUILDFLAG(IS_MAC)
  SparkleIntegration::CheckForUpdates();
#elif BUILDFLAG(IS_WIN)
  WinSparkleIntegration::CheckForUpdates();
#elif BUILDFLAG(IS_LINUX)
  LinuxUpdateIntegration::CheckForUpdates();
#endif
}

// static
void UpdateManager::Shutdown() {
#if BUILDFLAG(IS_WIN)
  WinSparkleIntegration::Shutdown();
#endif
  // macOS Sparkle and Linux don't need explicit shutdown
}

// static
bool UpdateManager::IsAvailable() {
#if BUILDFLAG(IS_MAC)
  return SparkleIntegration::IsAvailable();
#elif BUILDFLAG(IS_WIN)
  return WinSparkleIntegration::IsAvailable();
#elif BUILDFLAG(IS_LINUX)
  return LinuxUpdateIntegration::IsAvailable();
#else
  return false;
#endif
}

// static
std::string UpdateManager::GetCurrentVersion() {
#if BUILDFLAG(IS_MAC)
  return SparkleIntegration::GetCurrentVersion();
#elif BUILDFLAG(IS_WIN)
  return WinSparkleIntegration::GetCurrentVersion();
#elif BUILDFLAG(IS_LINUX)
  return LinuxUpdateIntegration::GetCurrentVersion();
#else
  return "0.1.0";
#endif
}

// static
void UpdateManager::SetFeedURL(const std::string& url) {
#if BUILDFLAG(IS_MAC)
  SparkleIntegration::SetFeedURL(url);
#elif BUILDFLAG(IS_WIN)
  WinSparkleIntegration::SetFeedURL(url);
#elif BUILDFLAG(IS_LINUX)
  LinuxUpdateIntegration::SetReleaseURL(url);
#endif
}

// static
std::string UpdateManager::GetPlatformName() {
#if BUILDFLAG(IS_MAC)
  return "macOS";
#elif BUILDFLAG(IS_WIN)
  return "Windows";
#elif BUILDFLAG(IS_LINUX)
  return "Linux";
#elif BUILDFLAG(IS_ANDROID)
  return "Android";
#elif BUILDFLAG(IS_IOS)
  return "iOS";
#else
  return "Unknown";
#endif
}

}  // namespace molt_ai
