// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Identifies the dedicated MoltNet (Tor) profile. Header-only on purpose so
// low-level code (e.g. the memory recorder) can ask "is this the Tor
// profile?" without linking the rest of MoltNet.
//
// MoltNet windows run in a non-primary off-the-record profile created with
// Profile::OTRProfileID::CreateUnique(kMoltNetOtrProfilePrefix). It has its
// own in-memory storage and its own NetworkContext; only that context is
// proxied through Tor. chrome/browser/profiles/profile.cc allows Browser
// windows for exactly this prefix.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PROFILE_ID_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PROFILE_ID_H_

#include <sstream>
#include <string>

#include "chrome/browser/profiles/profile.h"

namespace molt_ai {
namespace tor {

// Must match the literal in Profile::OTRProfileID::AllowsBrowserWindows().
inline constexpr char kMoltNetOtrProfilePrefix[] = "MoltNet::Tor";

// True iff |profile| is a MoltNet (Tor) profile.
inline bool IsMoltNetProfile(const Profile* profile) {
  if (!profile || !profile->IsOffTheRecord()) {
    return false;
  }
  // OTRProfileID only exposes its string form through operator<< (found by
  // ADL); CreateUnique() yields "<prefix>-<uuid>".
  std::ostringstream id;
  id << profile->GetOTRProfileID();
  const std::string value = id.str();
  const std::string prefix = std::string(kMoltNetOtrProfilePrefix) + "-";
  return value.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PROFILE_ID_H_
