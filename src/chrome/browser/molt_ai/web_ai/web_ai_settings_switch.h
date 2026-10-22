// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The user's switch for the built-in AI APIs: "web_ai_apis" in
// ~/.moltbrowser/settings.json, default true. False makes every site, in
// every profile, see "unavailable". RuntimeWebAIBackend reads it with the
// device facts; molt://ai-settings ("Sites using on-device AI") turns it on
// and off, then calls RuntimeWebAIBackend::InvalidateDeviceFacts() so it
// applies to the next request.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SETTINGS_SWITCH_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SETTINGS_SWITCH_H_

namespace molt_ai::web_ai {

inline constexpr char kWebAIApisSettingsKey[] = "web_ai_apis";

// Blocking (file I/O). True unless settings.json says false.
bool ReadWebAIApisSwitch();

// Blocking (file I/O). Sets the key and keeps every other one. False, and
// nothing written, if settings.json exists but isn't a JSON object (it is
// left for the user to fix), or if it can't be written.
bool WriteWebAIApisSwitch(bool enabled);

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SETTINGS_SWITCH_H_
