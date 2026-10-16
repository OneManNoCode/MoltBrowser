// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_prefs.h"

#include "base/values.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"

namespace molt_ai::shield::prefs {

void RegisterProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterBooleanPref(kRemoveTrackingParams, true);
}

bool IsRemoveTrackingParamsOn(const PrefService* prefs) {
  const PrefService::Preference* pref =
      prefs ? prefs->FindPreference(kRemoveTrackingParams) : nullptr;
  if (!pref || !pref->GetValue()) {
    return true;
  }
  return pref->GetValue()->GetIfBool().value_or(true);
}

}  // namespace molt_ai::shield::prefs
