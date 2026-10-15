// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNetWindow — opens/closes MoltNet (Tor) windows and implements
// New Identity. Every entry point (toolbar popover, settings page, /tor open)
// goes through here, so there is exactly one way a MoltNet window comes into
// existence:
//
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
  // verified, ...). Nothing is ever opened on a direct connection.
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

  // Tor-Browser-style New Identity: closes the MoltNet windows, drops all
  // connections + the host cache, discards the MoltNet profile (fresh
  // cookies/cache/storage), sends SIGNAL NEWNYM (checked), then opens a fresh
  // MoltNet window.
  static void NewIdentity(Profile* from, DoneCallback done);

  // Turns MoltNet off: closes all MoltNet windows (every profile) and stops
  // Tor. Regular windows are unaffected — they were never proxied.
  static void TurnOff();
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_WINDOW_H_
