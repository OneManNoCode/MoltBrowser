// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_pref_isolation.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/tor/molt_net_language_policy.h"
#include "chrome/browser/molt_ai/tor/molt_net_locale_prefs.h"
#include "chrome/browser/molt_ai/tor/molt_net_pref_isolation_policy.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_features.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/value_map_pref_store.h"
#include "components/prefs/writeable_pref_store.h"
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

// The value MoltNet pins `name` to: MoltNet's fixed default where Chromium's
// registered one depends on the UI language (molt_net_locale_prefs.h), else
// `registered`. A fixed value of another type than the registered one (a
// renamed or retyped pref) is ignored.
base::Value MoltNetDefault(const std::string& name,
                           const base::Value& registered) {
  const std::optional<LocaleIndependentDefault> fixed =
      MoltNetLocaleIndependentDefault(name, CurrentLocalePrefsPlatform());
  if (fixed) {
    if (fixed->type == LocaleIndependentDefault::Type::kString &&
        registered.is_string()) {
      return base::Value(std::string(fixed->string_value));
    }
    if (fixed->type == LocaleIndependentDefault::Type::kInteger &&
        registered.is_int()) {
      return base::Value(fixed->integer_value);
    }
  }
  return registered.Clone();
}

}  // namespace

std::unique_ptr<sync_preferences::PrefServiceSyncable> CreateMoltNetPrefService(
    sync_preferences::PrefServiceSyncable* regular_prefs,
    scoped_refptr<ValueMapPrefStore> top_store) {
  CHECK(regular_prefs);
  if (!top_store) {
    top_store = base::MakeRefCounted<ValueMapPrefStore>();
  }
  // What CreateAutomationPrefService() does for DevTools browser contexts:
  // the Incognito overlay, but with no persistent prefs, so nothing MoltNet
  // writes reaches the regular profile.
  std::unique_ptr<sync_preferences::PrefServiceSyncable> prefs =
      regular_prefs->CreateIncognitoPrefService(top_store,
                                                /*persistent_pref_names=*/{});
  PinMoltNetPrefsToDefaults(prefs.get());
  // Above MoltNet's own store, so neither LanguagePrefs (the ForcedLanguages
  // policy, the user's selected languages) nor anything else that writes the
  // profile's settings can change them (molt_net_language_policy.h).
  FixMoltNetLanguages(prefs.get(), top_store.get(),
                      fingerprinting::IsFingerprintingProtectionEnabled());
  return prefs;
}

size_t FixMoltNetLanguages(PrefService* prefs,
                           ValueMapPrefStore* top_store,
                           bool uniform) {
  if (!prefs || !top_store) {
    return 0;
  }
  // The list is made from the registered default of "intl.accept_languages"
  // (with MoltBrowser's overrides: the forked registry shares the regular
  // profile's defaults); without that pref there is nothing to fix.
  const PrefService::Preference* accept_languages =
      prefs->FindPreference(kMoltNetFixedLanguagePrefs[0]);
  if (!accept_languages ||
      accept_languages->GetType() != base::Value::Type::STRING) {
    return 0;
  }
  const base::Value* registered =
      prefs->GetDefaultPrefValue(kMoltNetFixedLanguagePrefs[0]);
  const std::string languages = MoltNetLanguageList(
      uniform, registered && registered->is_string() ? registered->GetString()
                                                     : std::string());
  size_t fixed = 0;
  for (std::string_view name : kMoltNetFixedLanguagePrefs) {
    const PrefService::Preference* pref = prefs->FindPreference(name);
    if (!pref || pref->GetType() != base::Value::Type::STRING) {
      continue;
    }
    top_store->SetValue(name, base::Value(languages),
                        WriteablePrefStore::DEFAULT_PREF_WRITE_FLAGS);
    ++fixed;
  }
  return fixed;
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
    // regular profile's value. PrefService::Set() stores it as is. Defaults
    // that follow the UI language (fonts, ...) get MoltNet's fixed ones.
    prefs->Set(name, MoltNetDefault(name, *default_value));
    ++pinned;
  }
  return pinned;
}

}  // namespace molt_ai::tor
