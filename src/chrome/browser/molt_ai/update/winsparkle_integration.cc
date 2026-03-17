// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/update/winsparkle_integration.h"

#include "base/logging.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

// WinSparkle function types — loaded dynamically to avoid hard DLL dependency
typedef void (*win_sparkle_init_fn)();
typedef void (*win_sparkle_cleanup_fn)();
typedef void (*win_sparkle_check_update_with_ui_fn)();
typedef void (*win_sparkle_set_appcast_url_fn)(const char*);
typedef void (*win_sparkle_set_app_details_fn)(const wchar_t*, const wchar_t*,
                                                const wchar_t*);

namespace {
HMODULE g_winsparkle_dll = nullptr;
bool g_initialized = false;

win_sparkle_init_fn g_init = nullptr;
win_sparkle_cleanup_fn g_cleanup = nullptr;
win_sparkle_check_update_with_ui_fn g_check_update = nullptr;
win_sparkle_set_appcast_url_fn g_set_appcast = nullptr;
win_sparkle_set_app_details_fn g_set_app_details = nullptr;

bool LoadWinSparkle() {
  g_winsparkle_dll = LoadLibraryW(L"WinSparkle.dll");
  if (!g_winsparkle_dll) {
    LOG(WARNING) << "WinSparkle.dll not found — auto-update disabled";
    return false;
  }

  g_init = reinterpret_cast<win_sparkle_init_fn>(
      GetProcAddress(g_winsparkle_dll, "win_sparkle_init"));
  g_cleanup = reinterpret_cast<win_sparkle_cleanup_fn>(
      GetProcAddress(g_winsparkle_dll, "win_sparkle_cleanup"));
  g_check_update = reinterpret_cast<win_sparkle_check_update_with_ui_fn>(
      GetProcAddress(g_winsparkle_dll,
                     "win_sparkle_check_update_with_ui"));
  g_set_appcast = reinterpret_cast<win_sparkle_set_appcast_url_fn>(
      GetProcAddress(g_winsparkle_dll, "win_sparkle_set_appcast_url"));
  g_set_app_details = reinterpret_cast<win_sparkle_set_app_details_fn>(
      GetProcAddress(g_winsparkle_dll, "win_sparkle_set_app_details"));

  return g_init && g_cleanup && g_check_update && g_set_appcast;
}
}  // namespace
#endif  // BUILDFLAG(IS_WIN)

namespace molt_ai {

// static
void WinSparkleIntegration::Initialize() {
#if BUILDFLAG(IS_WIN)
  if (g_initialized)
    return;

  if (!LoadWinSparkle())
    return;

  // Set app details for WinSparkle registry keys
  if (g_set_app_details) {
    g_set_app_details(L"GenEye AI Labs", L"MoltBrowser", L"0.1.0");
  }

  // Set appcast URL
  if (g_set_appcast) {
    g_set_appcast("https://moltbrowser.com/update/appcast-win.xml");
  }

  g_init();
  g_initialized = true;
  LOG(INFO) << "WinSparkle initialized — auto-update enabled";
#else
  LOG(WARNING) << "WinSparkle is Windows-only";
#endif
}

// static
void WinSparkleIntegration::CheckForUpdates() {
#if BUILDFLAG(IS_WIN)
  if (!g_initialized || !g_check_update)
    return;
  g_check_update();
#endif
}

// static
void WinSparkleIntegration::Shutdown() {
#if BUILDFLAG(IS_WIN)
  if (!g_initialized || !g_cleanup)
    return;
  g_cleanup();
  if (g_winsparkle_dll) {
    FreeLibrary(g_winsparkle_dll);
    g_winsparkle_dll = nullptr;
  }
  g_initialized = false;
#endif
}

// static
bool WinSparkleIntegration::IsAvailable() {
#if BUILDFLAG(IS_WIN)
  return g_initialized;
#else
  return false;
#endif
}

// static
std::string WinSparkleIntegration::GetCurrentVersion() {
  return "0.1.0";
}

// static
void WinSparkleIntegration::SetFeedURL(const std::string& url) {
#if BUILDFLAG(IS_WIN)
  if (g_set_appcast) {
    g_set_appcast(url.c_str());
  }
#endif
}

}  // namespace molt_ai
