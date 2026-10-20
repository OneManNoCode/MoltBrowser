// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_pref_isolation.h"

#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/tor/molt_net_pref_isolation_policy.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/pref_store.h"
#include "components/sync_preferences/pref_service_syncable.h"

namespace molt_ai::tor {

namespace {

void CollectPinnedPref(std::vector<std::string>* names,
                       const std::string& name,
                       const base::Value& /*value*/) {
  if (IsMoltNetPinnedPref(name)) {
    names->push_back(name);
  }
}

}  // namespace

std::unique_ptr<sync_preferences::PrefServiceSyncable> CreateMoltNetPrefService(
    sync_preferences::PrefServiceSyncable* regular_prefs,
    scoped_refptr<PrefStore> incognito_extension_pref_store) {
  CHECK(regular_prefs);
  // What CreateAutomationPrefService() does for DevTools browser contexts:
  // the Incognito overlay, but with no persistent prefs, so nothing MoltNet
  // writes reaches the regular profile.
  std::unique_ptr<sync_preferences::PrefServiceSyncable> prefs =
      regular_prefs->CreateIncognitoPrefService(
          std::move(incognito_extension_pref_store),
          /*persistent_pref_names=*/{});
  PinMoltNetPrefsToDefaults(prefs.get());
  return prefs;
}

size_t PinMoltNetPrefsToDefaults(PrefService* prefs) {
  if (!prefs) {
    return 0;
  }
  // Every registered pref, by name (iterates the registry, not the stores).
  // Collected first: writing while iterating is fine for the registry, but
  // this way the writes don't depend on that.
  std::vector<std::string> names;
  prefs->IteratePreferenceValues(
      base::BindRepeating(&CollectPinnedPref, base::Unretained(&names)));
  size_t pinned = 0;
  for (const std::string& name : names) {
    // The registered default, including MoltBrowser's overrides of it
    // (molt_privacy_prefs.cc): the forked registry shares the regular
    // profile's defaults.
    const base::Value* default_value = prefs->GetDefaultPrefValue(name);
    if (!default_value) {
      continue;
    }
    // An explicit value in MoltNet's own in-memory store, even though it
    // equals the default: that is what stops the overlay from reading the
    // regular profile's value. PrefService::Set() stores it as is.
    prefs->Set(name, *default_value);
    ++pinned;
  }
  return pinned;
}

}  // namespace molt_ai::tor
