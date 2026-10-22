// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's own defaults for the settings whose Chromium default depends on
// the browser's UI language. Pure (std only, plus the generated table), so
// molt_ai_unittests (and a standalone harness) test it; molt_net_pref_
// isolation.cc applies it.
//
// MoltNet already ignores the user's own font, font-size and encoding
// settings: every pref pages can observe is pinned to its registered default
// in MoltNet's profile (molt_net_pref_isolation.h). But at 152 Chromium
// registers these defaults from the UI language's resources
// (PrefsTabHelper::RegisterProfilePrefs, chrome/browser/ui/prefs/
// prefs_tab_helper.cc):
//
//   * webkit.webprefs.fonts.<generic>.<script> (read through FontFamilyCache
//     into WebPreferences): IDS_*_FONT_FAMILY* of locale_settings_
//     {win,mac,linux}.grd, translated per UI language (with a Japanese UI the
//     script-independent fonts are Japanese ones); the per-script defaults
//     of the UI language's own script are left out; ",A,B,C" lists resolve
//     to the first installed family (gfx::FontList::FirstAvailableOrFirst),
//     and on Windows the fixed font is Consolas or Courier New by the
//     ClearType setting;
//   * webkit.webprefs.minimum_logical_font_size: IDS_MINIMUM_LOGICAL_FONT_SIZE
//     (6 on Windows and Linux, 9 on macOS in English; 10 or 12 for some
//     Japanese, Korean and Chinese UIs);
//   * intl.charset_default: IDS_DEFAULT_ENCODING (windows-1252 in English,
//     Shift_JIS in Japanese, GBK in Chinese, ...), document.characterSet of
//     pages that don't declare one.
//
// Pages observe all three, so MoltNet pins them to fixed per-OS values
// instead, the same for every UI language and every machine of that OS:
//
//   * fonts: Chromium's en-US default for the OS, resolved the way a default
//     install resolves it: the first family of the default (or of its list)
//     on the OS's font allow-list (Blink's font_allowlists.cc: what every
//     supported version installs), an alias replaced by the family it stands
//     for (Linux: Times New Roman -> Liberation Serif, Arial -> Liberation
//     Sans), and none ("") when no family is listed. Two exceptions: Windows'
//     fixed font is always Consolas (ClearType is on by default) and Linux's
//     is DejaVu Sans Mono instead of fontconfig's "Monospace", which
//     distributions resolve differently. Every other font pref is "" (no
//     default, as Chromium has for most scripts): Blink then uses the
//     script-independent setting, and for a generic family without one, the
//     standard font;
//   * the minimum logical font size: the OS's en-US value;
//   * the default encoding: windows-1252, on every OS.
//
// The values are generated (molt_net_locale_prefs_table.h) from the pinned
// Chromium's .grd files and the allow-lists by tools/moltnet_locale/
// generate_tables.py; molt_net_locale_prefs_unittest.cc checks the rule and,
// on the build machine, the en-US column against the paks grit builds.
// Independent of the fingerprinting kill switch, like the rest of MoltNet's
// settings isolation.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_LOCALE_PREFS_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_LOCALE_PREFS_H_

#include <optional>
#include <string_view>

namespace molt_ai::tor {

enum class LocalePrefsPlatform { kWindows, kMac, kLinux, kOther };

// The platform this binary is built for. kOther (ChromeOS, Android, ...)
// has no font table: MoltBrowser doesn't run MoltNet there.
LocalePrefsPlatform CurrentLocalePrefsPlatform();

// One row of the generated font table.
struct MoltNetFontDefault {
  // webkit.webprefs.fonts.<generic>.<script>
  std::string_view pref_name;
  // The family MoltNet uses; empty for none.
  std::string_view family;
  // Chromium's registered default with an en-US UI (from the .grd file; a
  // leading comma makes it a list).
  std::string_view en_us_default;
};

// A fixed default for a string or an integer pref.
struct LocaleIndependentDefault {
  enum class Type { kString, kInteger };
  Type type = Type::kString;
  std::string_view string_value;
  int integer_value = 0;
};

// MoltNet's value for `pref_name` on `platform` if Chromium's registered
// default of that pref depends on the UI language, else nullopt (the
// registered default is used):
//   * a font family pref (webkit.webprefs.fonts.<generic>.<script>, one of
//     Chromium's seven generic families and a four-letter script): the
//     table's family, or "" for one it doesn't list; nullopt on kOther;
//   * webkit.webprefs.minimum_logical_font_size: the OS's en-US value;
//     nullopt on kOther;
//   * intl.charset_default: "windows-1252", on every platform.
std::optional<LocaleIndependentDefault> MoltNetLocaleIndependentDefault(
    std::string_view pref_name,
    LocalePrefsPlatform platform);

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_LOCALE_PREFS_H_
