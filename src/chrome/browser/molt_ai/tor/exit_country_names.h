// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Shared display names for the curated Tor exit-country codes, so
// views-level UI (e.g. the toolbar exit-country menu) and WebUI
// handlers can render the same human-readable labels. Header-only:
// pure function of its input, no state.
//
// NOTE: molt_ai_chat_handler_tor.cc and molt_ai_settings_ui.cc still
// carry their own historical copies of this map; keep all of them in
// sync with TorManager::GetAvailableExitCountries().

#ifndef CHROME_BROWSER_MOLT_AI_TOR_EXIT_COUNTRY_NAMES_H_
#define CHROME_BROWSER_MOLT_AI_TOR_EXIT_COUNTRY_NAMES_H_

#include <map>
#include <string>

#include "base/strings/string_util.h"

namespace molt_ai {
namespace tor {

// Display names for the curated exit-country codes. Keep in sync with
// TorManager::GetAvailableExitCountries(); any code not found here is
// rendered as its uppercased ISO code (e.g. "xx" -> "XX") so pickers
// never show a blank.
inline std::string ExitCountryDisplayName(const std::string& cc) {
  static const auto* const kNames =
      new std::map<std::string, std::string>{
          {"us", "United States"}, {"gb", "United Kingdom"},
          {"de", "Germany"},       {"fr", "France"},
          {"nl", "Netherlands"},   {"ch", "Switzerland"},
          {"se", "Sweden"},        {"no", "Norway"},
          {"fi", "Finland"},       {"ca", "Canada"},
          {"jp", "Japan"},         {"sg", "Singapore"},
          {"au", "Australia"},     {"es", "Spain"},
          {"it", "Italy"},         {"at", "Austria"},
          {"pl", "Poland"},        {"cz", "Czechia"},
          {"ro", "Romania"},       {"is", "Iceland"},
      };
  auto it = kNames->find(cc);
  if (it != kNames->end())
    return it->second;
  return base::ToUpperASCII(cc);
}

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_EXIT_COUNTRY_NAMES_H_
