// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNetWindow — opens/closes MoltNet (Tor) windows and implements
// New Identity. Every entry point (toolbar popover, settings page, /tor open)
// goes through here, so there is exactly one way a MoltNet window comes into
// existence:
//
//   0. MoltNetCpuError() — this computer's processor lets MoltNet give pages
//      the same answers as elsewhere (molt_net_cpu_policy.h; checked again
//      in steps 1 and 2);
//   1. TorManager::Launch() — Tor up with an established circuit;
//   2. MoltNetRouting::GetOrCreateMoltNetProfile() — the isolated OTR
//      profile, with the proxy/leak prefs applied and checked;
//   3. MoltNetRouting::VerifyRouting() — that profile's NetworkContext
//      confirms it routes via Tor's SOCKS port with no fallback;
//   4. only then a Browser window is created for that profile.
//
// MoltNet windows are never session-restored (OTR profiles have no session
// service). When the last MoltNet window of a profile closes, the MoltNet
// profile is destroyed, discarding its cookies, cache and storage.
//
// While a MoltNet profile exists, the settings step 2 checked are watched
// (molt_net_live_guard.h). If one changes so that the check would refuse (a
// policy proxy, override rules, a leak control, blob URL partitioning,
// Incognito turned off), that identity ends as New Identity ends the old
// one, with no new window, and a dialog gives the check's message.
//
// Ending an identity: from that moment its network context refuses every
// request (MoltNetRouting::EndIdentity()); DevTools sessions on its pages
// are detached (their windows close, in any profile); every window of it
// closes whatever is open in it: "Leave site?" (also when DevTools
// intercepts it), JavaScript and authentication dialogs, print preview,
// file and download prompts; and a window still open a moment later gets
// its renderer processes ended and its tabs closed.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_WINDOW_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_WINDOW_H_

#include <string>

#include "base/functional/callback.h"

class GURL;
class Profile;

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {
namespace tor {

class MoltNetWindow {
 public:
  using DoneCallback = base::OnceCallback<void(bool ok, std::string error)>;

  // Opens a new MoltNet window for |from|'s original profile, loading |url|
  // if it is http(s), else the new-tab page. |done| reports failure reasons
  // (Tor couldn't start, a policy controls the proxy, routing couldn't be
  // verified, New Identity is under way, ...). Nothing is ever opened on a
  // direct connection.
  static void Open(Profile* from, const GURL& url, DoneCallback done);

  // Open() on behalf of a user's click in browser-controlled UI shown for
  // |source| (the .onion interstitial, the Onion-Location infobar). A
  // failure is reported in a dialog over |source|'s window. |done| (may be
  // null) runs afterwards either way. Never call this for anything a web
  // page can trigger on its own.
  static void OpenForUserClick(content::WebContents* source,
                               const GURL& url,
                               DoneCallback done = DoneCallback());

  // Closes every MoltNet window belonging to |from|'s original profile.
  static void CloseAll(Profile* from);

  // Tor-Browser-style New Identity, strictly in this order:
  //   1. ends every MoltNet identity of |from|'s original profile: its
  //      network context refuses every request from now on, its
  //      connections and host cache are dropped, and its windows close
  //      whatever is open in them ("Leave site?", also with DevTools open;
  //      see the top of this file);
  //   2. waits until those profiles are destroyed, so nothing of the old
  //      identity (cookies, cache, storage, a late beacon or keepalive
  //      request) is left;
  //   3. sends SIGNAL NEWNYM and waits until Tor has applied it (up to ten
  //      seconds after a previous NEWNYM);
  //   4. opens a fresh MoltNet window.
  // No other MoltNet window opens for that profile meanwhile. If a window
  // of the old identity somehow stays open, it fails after a timeout
  // without changing circuits or opening a window; the old identity keeps
  // refusing every request, and the next Open() makes a new one.
  static void NewIdentity(Profile* from, DoneCallback done);

  // Turns MoltNet off: closes all MoltNet windows (every profile) and stops
  // Tor. Regular windows are unaffected — they were never proxied.
  static void TurnOff();
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_WINDOW_H_
