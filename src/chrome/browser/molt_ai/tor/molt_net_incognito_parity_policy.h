// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The pure part of "MoltNet gets Chromium's Incognito protections".
// Standard library only and header-only (like molt_net_profile_id.h), so
// any upstream hook can use it without a new link dependency and
// molt_ai_unittests tests it without a Profile. molt_net_incognito_parity.h
// applies it to a Profile and lists the hooks.
//
// Chromium has two kinds of off-the-record profile: the primary one
// (Incognito, Profile::IsIncognitoProfile()) and non-primary ones (DevTools
// browser contexts, captive-portal and media-router windows, and MoltNet's
// "MoltNet::Tor-<uuid>"). Many of its private-mode protections test for the
// primary one only, so without a hook they skip MoltNet.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_INCOGNITO_PARITY_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_INCOGNITO_PARITY_POLICY_H_

#include <string_view>

namespace molt_ai {
namespace tor {

// The prefix of every MoltNet profile's OTR profile id. MoltNet profiles
// are created with Profile::OTRProfileID::CreateUnique(), which yields
// "<prefix>-<uuid>". Must match the literal in
// Profile::OTRProfileID::AllowsBrowserWindows() (profile.cc).
inline constexpr char kMoltNetOtrProfilePrefix[] = "MoltNet::Tor";

// True iff `otr_profile_id` (an OTRProfileID's string form) is a MoltNet
// profile's: it starts with "MoltNet::Tor-". "MoltNet::Tor" alone,
// "MoltNet::Torx-..." or the prefix anywhere but at the start is not.
inline bool IsMoltNetOtrProfileIdString(std::string_view otr_profile_id) {
  constexpr std::string_view kPrefix = kMoltNetOtrProfilePrefix;
  return otr_profile_id.size() > kPrefix.size() &&
         otr_profile_id.substr(0, kPrefix.size()) == kPrefix &&
         otr_profile_id[kPrefix.size()] == '-';
}

// The kinds of profile Chromium's private-mode checks tell apart.
enum class PrivateModeProfile {
  // A regular profile, the guest's original profile, the system profile.
  kNotOffTheRecord,
  // The primary off-the-record profile of a regular profile:
  // Profile::IsIncognitoProfile().
  kIncognito,
  // A MoltNet (Tor) profile.
  kMoltNet,
  // Every other off-the-record profile: the guest session's, DevTools
  // browser contexts, captive-portal, media-router and test profiles, and
  // (from Chromium 156) an Enterprise Isolated Mode session, the primary
  // off-the-record profile when a policy turns that mode on instead of
  // Incognito (Profile::IsEnterpriseIsolatedModeProfile()).
  kOtherOffTheRecord,
};

// `is_incognito_profile` is Profile::IsIncognitoProfile();
// `has_moltnet_otr_id` is whether the profile's OTR id is a MoltNet one
// (IsMoltNetOtrProfileIdString()). Only off-the-record profiles have one.
inline PrivateModeProfile ClassifyPrivateModeProfile(bool is_off_the_record,
                                                     bool is_incognito_profile,
                                                     bool has_moltnet_otr_id) {
  if (!is_off_the_record) {
    return PrivateModeProfile::kNotOffTheRecord;
  }
  if (is_incognito_profile) {
    return PrivateModeProfile::kIncognito;
  }
  return has_moltnet_otr_id ? PrivateModeProfile::kMoltNet
                            : PrivateModeProfile::kOtherOffTheRecord;
}

// True for the profiles that get Chromium's Incognito protections at the
// hooks listed in molt_net_incognito_parity.h: Incognito itself, as
// upstream, and MoltNet. Every other kind keeps Chromium's behaviour, so
// at each hook the result equals Profile::IsIncognitoProfile() for every
// profile that isn't a MoltNet profile.
inline bool GetsIncognitoProtections(PrivateModeProfile profile) {
  return profile == PrivateModeProfile::kIncognito ||
         profile == PrivateModeProfile::kMoltNet;
}

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_INCOGNITO_PARITY_POLICY_H_
