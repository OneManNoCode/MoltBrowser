// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_prefs.h"

#include "components/pref_registry/pref_registry_syncable.h"

namespace molt_ai {
namespace memory {
namespace prefs {

int SanitizeRetentionDays(int days) {
  switch (days) {
    case 0:
    case 30:
    case 90:
    case 365:
      return days;
    default:
      return kDefaultRetentionDays;
  }
}

void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry) {
  // None of these sync: browsing memory is strictly on-device.
  registry->RegisterBooleanPref(kEnabled, false);
  registry->RegisterBooleanPref(kPaused, false);
  registry->RegisterIntegerPref(kRetentionDays, kDefaultRetentionDays);
  registry->RegisterBooleanPref(kOptInNoticeDismissed, false);
}

}  // namespace prefs
}  // namespace memory
}  // namespace molt_ai
