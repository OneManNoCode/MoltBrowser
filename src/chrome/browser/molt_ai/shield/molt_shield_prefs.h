// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShield's per-profile prefs (the user-facing switches that live with
// the profile rather than in MoltShield's browser-wide state).
//
// kRemoveTrackingParams is the "Strip tracking parameters" switch in
// molt://ai-settings. Tracking-parameter stripping runs only when both it
// and the MoltShieldRemoveParam kill switch (chrome://flags/
// #molt-shield-removeparam, molt_shield_features.h) are on; the pref is read
// per request, so changing it needs no relaunch. Incognito and MoltNet
// profiles read their original profile's value.
//
// Registered by ProfileImpl::RegisterProfilePrefs. Kept free of //content
// and Profile so //chrome/browser can link it from there.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_PREFS_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_PREFS_H_

class PrefRegistrySimple;
class PrefService;

namespace molt_ai::shield::prefs {

// bool, default true.
inline constexpr char kRemoveTrackingParams[] =
    "molt_shield.remove_tracking_params";

// MoltShield's profile prefs. Takes the profile's PrefRegistrySyncable too
// (it is a PrefRegistrySimple); nothing here syncs.
void RegisterProfilePrefs(PrefRegistrySimple* registry);

// The "Strip tracking parameters" setting of `prefs`. True (the default)
// when `prefs` is null or the pref isn't registered: stripping is the
// privacy-preserving side.
bool IsRemoveTrackingParamsOn(const PrefService* prefs);

}  // namespace molt_ai::shield::prefs

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_PREFS_H_
