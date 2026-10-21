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

#include "chrome/browser/molt_ai/tor/molt_net_incognito_parity_policy.h"
#include "chrome/browser/profiles/profile.h"

namespace molt_ai {
namespace tor {

// kMoltNetOtrProfilePrefix ("MoltNet::Tor", which must match the literal in
// Profile::OTRProfileID::AllowsBrowserWindows()) and the id check itself
// (IsMoltNetOtrProfileIdString()) live in the std-only
// molt_net_incognito_parity_policy.h, where molt_ai_unittests tests them.

// True iff |otr_profile_id| is a MoltNet (Tor) profile's id. Usable before
// the profile exists (OffTheRecordProfileImpl's constructor builds MoltNet's
// pref service with it, molt_net_isolation.h).
inline bool IsMoltNetOtrProfileId(const Profile::OTRProfileID& otr_profile_id) {
  // OTRProfileID only exposes its string form through operator<< (found by
  // ADL); CreateUnique() yields "<prefix>-<uuid>".
  std::ostringstream id;
  id << otr_profile_id;
  return IsMoltNetOtrProfileIdString(id.str());
}

// True iff |profile| is a MoltNet (Tor) profile.
inline bool IsMoltNetProfile(const Profile* profile) {
  if (!profile || !profile->IsOffTheRecord()) {
    return false;
  }
  return IsMoltNetOtrProfileId(profile->GetOTRProfileID());
}

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PROFILE_ID_H_
