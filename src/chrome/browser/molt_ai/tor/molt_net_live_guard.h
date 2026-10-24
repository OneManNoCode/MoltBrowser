// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNetLiveGuard: the check MoltNet runs before a window opens, run again
// whenever one of the settings it reads changes while MoltNet is open.
//
// For each MoltNet profile, from when MoltNet makes it until it is
// destroyed, the guard watches the settings that
// MoltNetRouting::SettingsError() reads (the proxy, the proxy override
// rules, the leak controls, blob URL partitioning: molt_net_live_policy.h),
// in the MoltNet profile's settings and in those of the regular profile it
// was made from. MoltNet's settings store shares the regular profile's
// policy store, so a policy change (a cloud policy refresh, a changed GPO,
// plist or policy file) notifies both. The check itself always reads the
// MoltNet profile's effective values, so a change on the regular side that
// doesn't reach MoltNet (an extension taking over the regular profile's
// proxy, a user setting) leaves MoltNet as it is, as the open check would.
//
// If the check refuses, the guard runs |on_refused| once for that profile,
// with the check's message. MoltNetWindow then ends that identity as New
// Identity does: its connections are dropped, its windows close (a
// "Leave site?" prompt can't keep one open) and its profile is destroyed;
// the user is told why.
//
// Between the change and the end, MoltNet's network context keeps MoltNet's
// Tor proxy configuration whatever the settings say
// (MoltNetRouting::MayApplyProxyConfig(), asked by ProxyConfigMonitor), so
// no request of that identity leaves outside Tor meanwhile.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_LIVE_GUARD_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_LIVE_GUARD_H_

#include <string>

#include "base/functional/callback.h"

class Profile;

namespace molt_ai::tor {

class MoltNetLiveGuard {
 public:
  // Runs inside the settings-change notification: it must not destroy the
  // profile, or close windows, synchronously (post a task for that).
  using RefusedCallback =
      base::RepeatingCallback<void(Profile* moltnet_profile,
                                   const std::string& error)>;

  // Starts watching |moltnet_profile|, a MoltNet profile whose settings
  // just passed the check. Does nothing for any other profile, or if it is
  // watched already. The watch ends when the profile (or the profile it was
  // made from) is about to be destroyed.
  static void Watch(Profile* moltnet_profile, RefusedCallback on_refused);

  // Whether |moltnet_profile| is watched (for tests).
  static bool IsWatchingForTesting(Profile* moltnet_profile);

  MoltNetLiveGuard() = delete;
};

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_LIVE_GUARD_H_
