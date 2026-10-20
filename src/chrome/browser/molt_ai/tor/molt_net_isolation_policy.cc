// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_isolation_policy.h"

namespace molt_ai::tor {

bool IsMoltNetPinnedPref(std::string_view pref_name) {
  for (std::string_view name : kMoltNetPinnedPrefNames) {
    if (pref_name == name) {
      return true;
    }
  }
  for (std::string_view prefix : kMoltNetPinnedPrefPrefixes) {
    if (pref_name.size() > prefix.size() &&
        pref_name.substr(0, prefix.size()) == prefix) {
      return true;
    }
  }
  return false;
}

}  // namespace molt_ai::tor
