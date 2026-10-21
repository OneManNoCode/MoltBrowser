// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Chromium's Incognito protections for MoltNet windows.
//
// MoltNet runs in a non-primary off-the-record profile ("MoltNet::Tor-<uuid>",
// molt_net_profile_id.h). Many of Chromium's private-mode protections test
// Profile::IsIncognitoProfile(), Profile::IsPrimaryOTRProfile() or compare
// with OTRProfileID::PrimaryID(), so they skip MoltNet, which then behaves
// less privately than an Incognito window. Each of the hooks below (marked
// "MoltBrowser:") gives MoltNet the Incognito behaviour at one such check;
// every other profile keeps Chromium's (molt_net_incognito_parity_policy.h).
// docs/MOLTNET_COMPLIANCE.md §2 ("Nothing from the regular profile") lists
// every call site that was checked, including the ones left alone.
//
// Nothing written to disk or into the regular profile:
//   * devtools/devtools_window.cc (GetProfileForDevToolsWindow): DevTools
//     for a MoltNet page runs in the MoltNet profile, as for Incognito,
//     instead of the regular one, so its settings (console history,
//     breakpoints with the page's script URLs, network filters, workspace
//     folders) stay in MoltNet's in-memory prefs and links it opens stay in
//     MoltNet.
//   * extensions/chrome_extension_cookies.cc: cookies of extension origins
//     are kept in memory, not in an "Extension Cookies" file in the regular
//     profile's folder.
//   * webshare/mac/sharing_service_operation.mm: navigator.share() with
//     files is refused as in Incognito (the files would be written to the
//     regular profile's folder before handing them to the share sheet).
//   * gcm/gcm_profile_service_factory.cc and
//     gcm/instance_id/instance_id_profile_service_factory.cc: no GCM (push
//     channel) client for MoltNet, whose store would be the regular
//     profile's "GCM Store" folder.
// Nothing about MoltNet pages given to the operating system:
//   * media/chrome_media_session_client.cc: the operating system's media
//     controls (lock screen, MPRIS on Linux, Now Playing on macOS, the
//     Windows media overlay) show a placeholder instead of the page's
//     title, artist (the page's site when it sets none), album and artwork
//     (content's SystemMediaControlsNotifier reads the flag).
//   * net/net_error_diagnostics_dialog_win.cc: no "Run Windows Network
//     Diagnostics" on MoltNet error pages (the tool logs the URL).
// Window and page behaviour:
//   * app_controller_mac.mm (OpenStartupTabsInBrowserWithProfile): a link
//     opened from another application never lands in a MoltNet window,
//     even when one was the last active window; it opens in a regular one.
//   * picture_in_picture/auto_picture_in_picture_tab_helper.cc: automatic
//     picture-in-picture is blocked unless the site was allowed it ("ask"
//     counts as "block").
//
// Also from Incognito, in MoltBrowser code: molt_net_window.cc drops the
// print-preview contents of a MoltNet profile before discarding it, as
// BrowserManagerService does for Incognito.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_INCOGNITO_PARITY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_INCOGNITO_PARITY_H_

#include "chrome/browser/molt_ai/tor/molt_net_incognito_parity_policy.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/profiles/profile.h"

namespace molt_ai {
namespace tor {

// The hooks' replacement for `profile->IsIncognitoProfile()`: true for the
// primary Incognito profile and for MoltNet profiles, false otherwise
// (including null). Header-only, like IsMoltNetProfile().
inline bool GetsIncognitoProtections(const Profile* profile) {
  if (!profile) {
    return false;
  }
  const bool is_off_the_record = profile->IsOffTheRecord();
  return GetsIncognitoProtections(ClassifyPrivateModeProfile(
      is_off_the_record, profile->IsIncognitoProfile(),
      is_off_the_record && IsMoltNetOtrProfileId(profile->GetOTRProfileID())));
}

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_INCOGNITO_PARITY_H_
