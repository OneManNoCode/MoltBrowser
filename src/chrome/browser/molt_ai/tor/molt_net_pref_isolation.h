// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's settings isolation from the regular profile.
//
// MoltNet windows run in an off-the-record profile of the regular profile
// (molt_net_profile_id.h). Chromium builds an off-the-record profile's prefs
// as an in-memory overlay over the parent's: reads fall through to the
// regular profile for every pref the overlay doesn't hold, and a fixed list
// of prefs (prefs::GetIncognitoPersistentPrefsAllowlist(): third-party
// cookie mode, bookmarks bar, DevTools, ...) is written through to it.
// Content settings live in prefs too, so the same overlay is how Incognito
// inherits site permissions, content-setting defaults, embargo and the
// rest.
//
// For MoltNet that is wrong in both directions, so its pref service differs
// from Incognito's in two ways:
//   * Nothing is written through: like DevTools' throwaway browser contexts
//     (CreateAutomationPrefService), the persistent list is empty, so
//     nothing done in a MoltNet window changes the regular profile.
//   * Every pref that pages can observe (molt_net_pref_isolation_policy.h) is
//     set to MoltBrowser's registered default in MoltNet's own store before
//     anything reads it (where Chromium registers a default that follows
//     the UI language -- font families, minimum logical font size, default
//     encoding -- to MoltNet's fixed per-OS value instead,
//     molt_net_locale_prefs.h). The overlay then never reads the regular
//     profile's value of those prefs, and later changes there don't reach
//     MoltNet (OverlayUserPrefStore ignores the parent's changes to a pref
//     its own store holds).
//
// One hook makes it apply: OffTheRecordProfileImpl's constructor calls
// CreateMoltNetPrefService() for MoltNet profile ids instead of building the
// Incognito pref service. Because it works at the pref layer, everything
// downstream follows without hooks of its own: HostContentSettingsMap's
// pref and default providers (and so PermissionContextBase,
// navigator.permissions.query, PermissionDecisionAutoBlocker, MoltShield's
// per-site switch), WebPreferences (ChromeContentBrowserClient::
// OverrideWebPreferences, FontFamilyCache), RendererPreferences
// (renderer_preferences_util.cc), CookieSettings, PrivacySandboxSettings and
// the network context's Accept-Language.
//
// What MoltNet sets afterwards (MoltNetRouting::ApplyRoutingPrefs) and what
// changes during the session (site permissions granted in a MoltNet window,
// ...) is kept, in memory, until the profile goes away at New Identity or
// when the last MoltNet window closes. Policy values rank above the user
// store and still apply; extension-controlled values don't, because
// MoltNet's extension pref store is empty (CreateMoltNetExtensionPrefStore,
// molt_net_isolation.h).

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PREF_ISOLATION_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PREF_ISOLATION_H_

#include <stddef.h>

#include <memory>

#include "base/memory/scoped_refptr.h"

class PrefService;
class PrefStore;

namespace sync_preferences {
class PrefServiceSyncable;
}  // namespace sync_preferences

namespace molt_ai::tor {

// The pref service of a new MoltNet profile whose parent's prefs are
// `regular_prefs`: an off-the-record overlay that writes nothing through to
// `regular_prefs`, with every pinned pref already at its registered
// default. `incognito_extension_pref_store` is the extension-controlled
// store; MoltNet passes an empty one (CreateMoltNetExtensionPrefStore).
std::unique_ptr<sync_preferences::PrefServiceSyncable> CreateMoltNetPrefService(
    sync_preferences::PrefServiceSyncable* regular_prefs,
    scoped_refptr<PrefStore> incognito_extension_pref_store);

// Writes the registered default of every registered pref that
// IsMoltNetPinnedPref() names into `prefs`' user store (MoltNet's fixed
// value for those whose default follows the UI language,
// MoltNetLocaleIndependentDefault()). Returns how many it wrote. Only for a
// MoltNet pref service (on any other one it would replace the user's
// settings), and before anything reads it.
size_t PinMoltNetPrefsToDefaults(PrefService* prefs);

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PREF_ISOLATION_H_
