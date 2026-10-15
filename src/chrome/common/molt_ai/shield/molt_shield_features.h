// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Runtime kill switches for MoltShield's hooks into page loading. Each hook
// is guarded where it is installed; when its feature is disabled the hook is
// not installed at all (no proxy, no throttle, no renderer agent), so a bad
// interaction can be turned off without a new build:
//
//   chrome://flags/#molt-shield-request-proxy    MoltShieldRequestProxy
//   chrome://flags/#molt-shield-frame-blocking   MoltShieldFrameBlocking
//   chrome://flags/#molt-shield-websocket        MoltShieldWebSocket
//   chrome://flags/#molt-shield-keepalive        MoltShieldKeepAlive
//   chrome://flags/#molt-shield-cosmetics        MoltShieldCosmetics
//   chrome://flags/#molt-shield-consent-killer   MoltShieldConsentKiller
//
// or on the command line, e.g. --disable-features=MoltShieldRequestProxy.
// --disable-molt-shield turns every hook off (and keeps the engine and list
// updater from ever starting). All features are enabled by default.
//
// Shared by the browser and the renderer (the cosmetic agent); feature state
// reaches renderers through the usual --enable/--disable-features
// propagation, and --disable-molt-shield is copied to renderers by
// ChromeContentBrowserClient::AppendExtraCommandLineSwitches.

#ifndef CHROME_COMMON_MOLT_AI_SHIELD_MOLT_SHIELD_FEATURES_H_
#define CHROME_COMMON_MOLT_AI_SHIELD_MOLT_SHIELD_FEATURES_H_

#include <string_view>
#include <vector>

#include "base/feature_list.h"

namespace molt_shield {

namespace switches {
// Disables every MoltShield hook for this run.
inline constexpr char kDisableMoltShield[] = "disable-molt-shield";
}  // namespace switches

// Subresource requests through MoltShieldProxyingURLLoaderFactory.
BASE_DECLARE_FEATURE(kMoltShieldRequestProxy);
// Ad iframe blocking in MoltShieldNavigationThrottle.
BASE_DECLARE_FEATURE(kMoltShieldFrameBlocking);
// WebSocket interception (WillInterceptWebSocket / CreateWebSocket).
BASE_DECLARE_FEATURE(kMoltShieldWebSocket);
// Throttle for keepalive requests (beacons, fetch keepalive).
BASE_DECLARE_FEATURE(kMoltShieldKeepAlive);
// Cosmetic filtering + scriptlets: the navigation throttle's cosmetic
// prefetch, the tab helper's delivery and the renderer CosmeticFilterAgent.
BASE_DECLARE_FEATURE(kMoltShieldCosmetics);
// Cookie-consent auto-reject (ConsentKillerTabHelper).
BASE_DECLARE_FEATURE(kMoltShieldConsentKiller);

// chrome://flags internal names (about_flags.cc entries come from
// chrome/browser/molt_ai/shield/molt_shield_about_flags.h).
namespace flags {
inline constexpr char kRequestProxy[] = "molt-shield-request-proxy";
inline constexpr char kFrameBlocking[] = "molt-shield-frame-blocking";
inline constexpr char kWebSocket[] = "molt-shield-websocket";
inline constexpr char kKeepAlive[] = "molt-shield-keepalive";
inline constexpr char kCosmetics[] = "molt-shield-cosmetics";
inline constexpr char kConsentKiller[] = "molt-shield-consent-killer";
}  // namespace flags

// True when --disable-molt-shield is on this process's command line.
bool IsMoltShieldDisabledByCommandLine();

// The kill-switch state of each hook: the feature is enabled and
// --disable-molt-shield is absent. (User-facing settings — the global and
// per-site Shields switches — are checked separately by the hooks.)
bool IsRequestProxyEnabled();
bool IsFrameBlockingEnabled();
bool IsWebSocketEnabled();
bool IsKeepAliveEnabled();
bool IsCosmeticsEnabled();
bool IsConsentKillerEnabled();

// chrome://flags names of the hooks whose feature is disabled (through
// chrome://flags or --disable-features), for the settings page.
// --disable-molt-shield is reported by IsMoltShieldDisabledByCommandLine().
std::vector<std::string_view> FlagNamesOfDisabledHooks();

}  // namespace molt_shield

#endif  // CHROME_COMMON_MOLT_AI_SHIELD_MOLT_SHIELD_FEATURES_H_
