// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_locale_prefs.h"

#include <array>
#include <cstddef>

#include "build/build_config.h"
#include "chrome/browser/molt_ai/tor/molt_net_locale_prefs_table.h"

namespace molt_ai::tor {

namespace {

// chrome/common/pref_font_webkit_names.h and pref_names.h at 152; the test
// checks them against Chromium's constants.
constexpr std::string_view kFontFamilyPrefPrefix = "webkit.webprefs.fonts.";
constexpr auto kGenericFamilies = std::to_array<std::string_view>(
    {"standard", "fixed", "serif", "sansserif", "cursive", "fantasy",
     "math"});
constexpr std::string_view kMinimumLogicalFontSizePref =
    "webkit.webprefs.minimum_logical_font_size";
constexpr std::string_view kDefaultCharsetPref = "intl.charset_default";

bool IsAsciiLetter(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// webkit.webprefs.fonts.<generic>.<script>, <script> an ISO 15924 code.
bool IsFontFamilyPref(std::string_view name) {
  if (!name.starts_with(kFontFamilyPrefPrefix)) {
    return false;
  }
  name.remove_prefix(kFontFamilyPrefPrefix.size());
  const size_t dot = name.find('.');
  if (dot == std::string_view::npos) {
    return false;
  }
  const std::string_view generic = name.substr(0, dot);
  const std::string_view script = name.substr(dot + 1);
  bool known_generic = false;
  for (std::string_view family : kGenericFamilies) {
    known_generic = known_generic || family == generic;
  }
  if (!known_generic || script.size() != 4) {
    return false;
  }
  for (char c : script) {
    if (!IsAsciiLetter(c)) {
      return false;
    }
  }
  return true;
}

LocaleIndependentDefault StringDefault(std::string_view value) {
  return {.type = LocaleIndependentDefault::Type::kString,
          .string_value = value,
          .integer_value = 0};
}

LocaleIndependentDefault IntegerDefault(int value) {
  return {.type = LocaleIndependentDefault::Type::kInteger,
          .string_value = std::string_view(),
          .integer_value = value};
}

template <size_t N>
LocaleIndependentDefault FontFamily(
    const std::array<MoltNetFontDefault, N>& table,
    std::string_view pref_name) {
  for (const MoltNetFontDefault& row : table) {
    if (row.pref_name == pref_name) {
      return StringDefault(row.family);
    }
  }
  return StringDefault("");
}

}  // namespace

LocalePrefsPlatform CurrentLocalePrefsPlatform() {
#if BUILDFLAG(IS_WIN)
  return LocalePrefsPlatform::kWindows;
#elif BUILDFLAG(IS_MAC)
  return LocalePrefsPlatform::kMac;
#elif BUILDFLAG(IS_LINUX)
  return LocalePrefsPlatform::kLinux;
#else
  return LocalePrefsPlatform::kOther;
#endif
}

std::optional<LocaleIndependentDefault> MoltNetLocaleIndependentDefault(
    std::string_view pref_name,
    LocalePrefsPlatform platform) {
  namespace table = locale_prefs_table;
  if (pref_name == kDefaultCharsetPref) {
    return StringDefault(table::kDefaultEncoding);
  }
  if (pref_name == kMinimumLogicalFontSizePref) {
    switch (platform) {
      case LocalePrefsPlatform::kWindows:
        return IntegerDefault(table::kWindowsMinimumLogicalFontSize);
      case LocalePrefsPlatform::kMac:
        return IntegerDefault(table::kMacMinimumLogicalFontSize);
      case LocalePrefsPlatform::kLinux:
        return IntegerDefault(table::kLinuxMinimumLogicalFontSize);
      case LocalePrefsPlatform::kOther:
        return std::nullopt;
    }
    return std::nullopt;
  }
  if (!IsFontFamilyPref(pref_name)) {
    return std::nullopt;
  }
  switch (platform) {
    case LocalePrefsPlatform::kWindows:
      return FontFamily(table::kWindowsFontDefaults, pref_name);
    case LocalePrefsPlatform::kMac:
      return FontFamily(table::kMacFontDefaults, pref_name);
    case LocalePrefsPlatform::kLinux:
      return FontFamily(table::kLinuxFontDefaults, pref_name);
    case LocalePrefsPlatform::kOther:
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace molt_ai::tor
