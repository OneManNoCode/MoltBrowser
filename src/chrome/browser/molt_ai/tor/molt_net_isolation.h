// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What keeps the regular profile out of MoltNet windows, on the extensions
// and services side. MoltNet runs in a non-primary off-the-record profile of
// the user's regular profile, and Chromium treats every such profile like
// Incognito: extensions the user allowed in Incognito run there too, and
// several profile-keyed services read the regular profile's data. Anything
// web content can observe that comes from there is the same in every MoltNet
// session, after every New Identity, and links MoltNet to the regular
// browsing identity. The decisions live here; the upstream hooks (marked
// "MoltBrowser:") only ask. Pure logic: molt_net_isolation_policy.h.
//
// Extensions (ExtensionsBrowserClient::IsExtensionKeptOutOfContext):
//   * only MoltBrowser's own component extensions (the PDF viewer) run in
//     MoltNet; extensions/browser/extension_util.cc treats every other one
//     as not allowed in Incognito, and as invisible to MoltNet renderers,
//     for MoltNet profiles (no background page or service worker, content
//     script, extension page, web-accessible resource, page-to-extension
//     messaging, toolbar button, omnibox keyword or chrome:// override);
//     MoltNet windows list no side panel of theirs
//     (extension_side_panel_manager.cc);
//   * events about MoltNet (tabs, windows, webNavigation, downloads,
//     cookies, context menus, commands, ...) never reach them
//     (events/event_dispatch_helper.cc);
//   * MoltNet requests are never proxied for webRequest or
//     declarativeNetRequest (api/web_request/web_request_api.cc);
//   * MoltNet windows and tabs are invisible to the tabs and windows APIs
//     (browser_extension_window_controller.cc), never the window an
//     extension opens tabs in (chrome_extension_function_details.cc,
//     browser_window_util.cc), can't be debugged
//     (api/debugger/debugger_api.cc), and never give them activeTab
//     (permissions/active_tab_permission_granter.cc);
//   * no extension-controlled preference applies to MoltNet's prefs
//     (off_the_record_profile_impl.cc).
//
// Services:
//   * certificate-error exceptions are MoltNet's own, in memory
//     (CreateMoltNetSSLHostStateDelegate; the regular profile's are
//     inherited by an off-the-record HostContentSettingsMap).
//   * The rest are one-line hooks next to the code they change, each
//     asking IsMoltNetProfile() (molt_net_profile_id.h): no high media
//     engagement (media_engagement_service.cc), no engaged sites for
//     lookalike warnings (lookalike_url_navigation_throttle.cc), no
//     navigator.storage.persist() grant (persistent_storage_permission_
//     context.cc), Incognito's third-party-cookie blocking and Privacy
//     Sandbox off (cookie_settings_factory.cc, privacy_sandbox_settings_
//     delegate.cc), no ambient authentication (profile_network_context_
//     service.cc) and no "unfamiliar site" heuristic
//     (site_familiarity_utils.cc). docs/MOLTNET_COMPLIANCE.md §2 explains
//     what each one stops a page from seeing.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ISOLATION_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ISOLATION_H_

#include <memory>

#include "base/memory/scoped_refptr.h"

class HostContentSettingsMap;
class KeyedService;
class PrefStore;
class Profile;

namespace content {
class BrowserContext;
}  // namespace content

namespace extensions {
class Extension;
}  // namespace extensions

namespace molt_ai {
namespace tor {

// True if `context` is a MoltNet profile. Null-safe.
bool IsMoltNetContext(content::BrowserContext* context);

// ChromeExtensionsBrowserClient::IsExtensionKeptOutOfContext: true if
// `context` is a MoltNet profile and `extension` isn't one of MoltBrowser's
// component extensions. `extension` is null when the caller only has an ID
// (not loaded, or an unknown caller): kept out. False for every other
// profile, which keeps Chromium's own rules.
bool IsExtensionKeptOutOf(const extensions::Extension* extension,
                          content::BrowserContext* context);

// The extension-controlled preference store for a MoltNet profile's pref
// service: empty and never written, so no extension's proxy, privacy,
// font or other controlled setting applies in MoltNet. (Other off-the-
// record profiles get Profile::CreateExtensionPrefStore(original, true),
// which carries the settings of every extension allowed in Incognito.)
scoped_refptr<PrefStore> CreateMoltNetExtensionPrefStore();

// The SSLHostStateDelegate of a MoltNet profile. Like Chromium's
// StatefulSSLHostStateDelegate, except that certificate-error exceptions
// live in memory: a decision made in MoltNet lasts until the profile goes
// away (New Identity, last MoltNet window closed), and the regular
// profile's decisions, which its HostContentSettingsMap would hand down,
// never apply. HTTPS-Only exceptions (not inherited) are unchanged.
std::unique_ptr<KeyedService> CreateMoltNetSSLHostStateDelegate(
    Profile* profile,
    HostContentSettingsMap* host_content_settings_map);

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ISOLATION_H_
