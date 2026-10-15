// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://flags entries for MoltShield's runtime kill switches
// (chrome/common/molt_ai/shield/molt_shield_features.h). Expanded inside
// kFeatureEntries in chrome/browser/about_flags.cc, so the upstream file only
// gains an include and one macro line. Each flag defaults to Enabled;
// choosing Disabled removes that hook entirely (takes effect on relaunch).
// --disable-molt-shield turns all of them off for one run.
//
// The macro uses about_flags.cc's kOsAll and FEATURE_VALUE_TYPE.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_ABOUT_FLAGS_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_ABOUT_FLAGS_H_

#include "chrome/common/molt_ai/shield/molt_shield_features.h"

// clang-format off
#define MOLT_SHIELD_FEATURE_ENTRIES                                           \
    {molt_shield::flags::kRequestProxy,                                       \
     "MoltShield: filter page requests",                                      \
     "Kill switch. When disabled, MoltShield installs no proxy in front of "  \
     "page subresource requests (scripts, images, XHR/fetch, ...), so they "  \
     "are no longer checked against the ad and tracker lists.",               \
     kOsAll, FEATURE_VALUE_TYPE(molt_shield::kMoltShieldRequestProxy)},       \
    {molt_shield::flags::kFrameBlocking,                                      \
     "MoltShield: block ad frames",                                           \
     "Kill switch. When disabled, MoltShield no longer checks iframe "        \
     "navigations and stops collapsing ad frames.",                           \
     kOsAll, FEATURE_VALUE_TYPE(molt_shield::kMoltShieldFrameBlocking)},      \
    {molt_shield::flags::kWebSocket,                                          \
     "MoltShield: filter WebSockets",                                         \
     "Kill switch. When disabled, MoltShield no longer intercepts WebSocket " \
     "connections; they are created exactly as without it.",                  \
     kOsAll, FEATURE_VALUE_TYPE(molt_shield::kMoltShieldWebSocket)},          \
    {molt_shield::flags::kKeepAlive,                                          \
     "MoltShield: filter keepalive requests",                                 \
     "Kill switch. When disabled, MoltShield adds no throttle to keepalive "  \
     "requests (navigator.sendBeacon, fetch keepalive).",                     \
     kOsAll, FEATURE_VALUE_TYPE(molt_shield::kMoltShieldKeepAlive)},          \
    {molt_shield::flags::kCosmetics,                                          \
     "MoltShield: cosmetic filtering and scriptlets",                         \
     "Kill switch. When disabled, MoltShield no longer hides ad elements or " \
     "runs anti-ad scriptlets, and attaches nothing to renderer frames.",     \
     kOsAll, FEATURE_VALUE_TYPE(molt_shield::kMoltShieldCosmetics)},          \
    {molt_shield::flags::kConsentKiller,                                      \
     "MoltShield: auto-reject cookie banners",                                \
     "Kill switch. When disabled, MoltShield never injects its "              \
     "cookie-consent auto-reject script.",                                    \
     kOsAll, FEATURE_VALUE_TYPE(molt_shield::kMoltShieldConsentKiller)},      \
    {molt_shield::flags::kRemoveParam,                                        \
     "MoltShield: strip tracking parameters",                                 \
     "Kill switch. When disabled, MoltShield no longer removes tracking "     \
     "parameters (utm_*, fbclid, gclid, ... and list $removeparam rules) "    \
     "from navigations and page requests. Blocking is unaffected.",           \
     kOsAll, FEATURE_VALUE_TYPE(molt_shield::kMoltShieldRemoveParam)}
// clang-format on

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_ABOUT_FLAGS_H_
