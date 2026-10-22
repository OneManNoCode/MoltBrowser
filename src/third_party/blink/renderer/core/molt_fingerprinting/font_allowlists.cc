// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The installed font families web content may see in protected contexts:
// for each platform, the families its OS installs by default, so that every
// MoltBrowser user on that OS sees the same fonts (docs/FINGERPRINTING.md,
// "Fonts", has the provenance and the rules). Built from the OS vendors'
// published font lists and default installs; no other browser's list or
// code was used.
//
// Rules for editing:
//  * Only fonts that every supported version of the OS installs by default
//    (not fonts downloaded on demand, Windows "Features on Demand" language
//    packs, Office/Adobe/app fonts, or distribution extras). A font that is
//    missing on some supported version splits users into two groups.
//  * A name that is missing here only costs compatibility (the page falls
//    back to its next family); a name that is wrongly here costs privacy.
//  * Other names of a listed family (legacy per-weight family names,
//    localized names, PostScript names) may be listed too: they resolve to
//    the same font on every machine.
//  * `alias_of` entries are for names that resolve to different families on
//    different machines (see font_allowlists.h).
//  * MoltNet's default font settings are picked from these lists: after an
//    edit, run tools/moltnet_locale/generate_tables.py --write.

#include "third_party/blink/renderer/core/molt_fingerprinting/font_allowlists.h"

namespace blink::molt_fingerprinting {

namespace {

// Windows 10 (22H2) and Windows 11: the fonts every edition installs in every
// display language, i.e. the intersection of Microsoft's Windows 10 and
// Windows 11 font lists without their "Features on Demand" supplemental
// fonts (Meiryo, MS Mincho, SimHei, Gulim, Arial Nova, ... are not here) and
// without the Windows 11 additions (Segoe UI Variable, Segoe Fluent Icons,
// Cascadia Code / Mono, Sans Serif Collection). DirectWrite reports some
// faces under their legacy (per-weight) family names, so those are listed
// as well.
constexpr FontListEntry kWindowsFonts[] = {
    {"Arial"},
    {"Arial Black"},
    {"Bahnschrift"},
    {"Bahnschrift Condensed"},
    {"Bahnschrift Light"},
    {"Bahnschrift Light Condensed"},
    {"Bahnschrift Light SemiCondensed"},
    {"Bahnschrift SemiBold"},
    {"Bahnschrift SemiBold Condensed"},
    {"Bahnschrift SemiCondensed"},
    {"Bahnschrift SemiLight"},
    {"Bahnschrift SemiLight Condensed"},
    {"Calibri"},
    {"Calibri Light"},
    {"Cambria"},
    {"Cambria Math"},
    {"Candara"},
    {"Candara Light"},
    {"Comic Sans MS"},
    {"Consolas"},
    {"Constantia"},
    {"Corbel"},
    {"Corbel Light"},
    {"Courier New"},
    {"Ebrima"},
    {"Franklin Gothic Medium"},
    {"Gabriola"},
    {"Gadugi"},
    {"Georgia"},
    {"HoloLens MDL2 Assets"},
    {"Impact"},
    {"Ink Free"},
    {"Javanese Text"},
    {"Leelawadee UI"},
    {"Leelawadee UI Semilight"},
    {"Lucida Console"},
    {"Lucida Sans Unicode"},
    {"Malgun Gothic"},
    {"Malgun Gothic Semilight"},
    {"Marlett"},
    {"Microsoft Himalaya"},
    {"Microsoft JhengHei"},
    {"Microsoft JhengHei Light"},
    {"Microsoft JhengHei UI"},
    {"Microsoft JhengHei UI Light"},
    {"Microsoft New Tai Lue"},
    {"Microsoft PhagsPa"},
    {"Microsoft Sans Serif"},
    {"Microsoft Tai Le"},
    {"Microsoft YaHei"},
    {"Microsoft YaHei Light"},
    {"Microsoft YaHei UI"},
    {"Microsoft YaHei UI Light"},
    {"Microsoft Yi Baiti"},
    {"MingLiU-ExtB"},
    {"MingLiU_HKSCS-ExtB"},
    {"Mongolian Baiti"},
    {"MS Gothic"},
    {"MS PGothic"},
    {"MS UI Gothic"},
    {"MV Boli"},
    {"Myanmar Text"},
    {"Nirmala UI"},
    {"Nirmala UI Semilight"},
    {"NSimSun"},
    {"Palatino Linotype"},
    {"PMingLiU-ExtB"},
    {"Segoe MDL2 Assets"},
    {"Segoe Print"},
    {"Segoe Script"},
    {"Segoe UI"},
    {"Segoe UI Black"},
    {"Segoe UI Emoji"},
    {"Segoe UI Historic"},
    {"Segoe UI Light"},
    {"Segoe UI Semibold"},
    {"Segoe UI Semilight"},
    {"Segoe UI Symbol"},
    {"SimSun"},
    {"SimSun-ExtB"},
    {"Sitka Banner"},
    {"Sitka Display"},
    {"Sitka Heading"},
    {"Sitka Small"},
    {"Sitka Subheading"},
    {"Sitka Text"},
    {"Sylfaen"},
    {"Symbol"},
    {"Tahoma"},
    {"Times New Roman"},
    {"Trebuchet MS"},
    {"Verdana"},
    {"Webdings"},
    {"Wingdings"},
    {"Yu Gothic"},
    {"Yu Gothic Light"},
    {"Yu Gothic Medium"},
    {"Yu Gothic UI"},
    {"Yu Gothic UI Light"},
    {"Yu Gothic UI Semibold"},
    {"Yu Gothic UI Semilight"},
    // Localized family names of the CJK fonts above (East Asian sites name
    // them this way; DirectWrite matches any localized name).
    {"\uFF2D\uFF33 \u30B4\u30B7\u30C3\u30AF"},        // MS Gothic, ja
    {"\uFF2D\uFF33 \uFF30\u30B4\u30B7\u30C3\u30AF"},  // MS PGothic, ja
    {"\u6E38\u30B4\u30B7\u30C3\u30AF"},               // Yu Gothic, ja
    {"\u6E38\u30B4\u30B7\u30C3\u30AF Light"},         // Yu Gothic Light, ja
    {"\u6E38\u30B4\u30B7\u30C3\u30AF Medium"},        // Yu Gothic Medium, ja
    {"\u5B8B\u4F53"},                                 // SimSun, zh-Hans
    {"\u65B0\u5B8B\u4F53"},                           // NSimSun, zh-Hans
    {"\u5FAE\u8F6F\u96C5\u9ED1"},        // Microsoft YaHei, zh-Hans
    {"\u5FAE\u8EDF\u6B63\u9ED1\u9AD4"},  // Microsoft JhengHei, zh-Hant
    {"\u7D30\u660E\u9AD4-ExtB"},         // MingLiU-ExtB, zh-Hant
    {"\u65B0\u7D30\u660E\u9AD4-ExtB"},   // PMingLiU-ExtB, zh-Hant
    {"\u7D30\u660E\u9AD4_HKSCS-ExtB"},   // MingLiU_HKSCS-ExtB, zh-Hant
    {"\uB9D1\uC740 \uACE0\uB515"},       // Malgun Gothic, ko
    // Blink's own substitutions on Windows (alternate_font_family.h): looked
    // up as their listed equivalents, also on machines that installed the
    // real Helvetica.
    {"Helvetica", "Arial"},
    {"Times", "Times New Roman"},
    {"Courier", "Courier New"},
    {"MS Sans Serif", "Microsoft Sans Serif"},
    {"MS Serif", "Times New Roman"},
};

// macOS 13 Ventura, 14 Sonoma and 15 Sequoia: the families installed in
// /System/Library/Fonts and /System/Library/Fonts/Supplemental by every
// install, per Apple's published font lists for those releases. Fonts that
// macOS downloads on demand ("available for download": Athelas, Iowan Old
// Style, Canela, most CJK extras, ...) are not here, nor are app fonts (SF
// Pro, SF Mono, New York; the system UI font is reached through system-ui).
// The long tail of rarely named script fonts (Noto Sans <script>, ...) is
// left out: system fallback still uses them for their scripts.
constexpr FontListEntry kMacFonts[] = {
    {"American Typewriter"},
    {"Andale Mono"},
    {"Apple Braille"},
    {"Apple Chancery"},
    {"Apple Color Emoji"},
    {"Apple SD Gothic Neo"},
    {"Apple Symbols"},
    {"AppleGothic"},
    {"AppleMyungjo"},
    {"Arial"},
    {"Arial Black"},
    {"Arial Hebrew"},
    {"Arial Hebrew Scholar"},
    {"Arial Narrow"},
    {"Arial Rounded MT Bold"},
    {"Arial Unicode MS"},
    {"Avenir"},
    {"Avenir Next"},
    {"Avenir Next Condensed"},
    {"Ayuthaya"},
    {"Baghdad"},
    {"Bangla MN"},
    {"Bangla Sangam MN"},
    {"Baskerville"},
    {"Beirut"},
    {"Big Caslon"},
    {"Bodoni 72"},
    {"Bodoni 72 Oldstyle"},
    {"Bodoni 72 Smallcaps"},
    {"Bodoni Ornaments"},
    {"Bradley Hand"},
    {"Brush Script MT"},
    {"Chalkboard"},
    {"Chalkboard SE"},
    {"Chalkduster"},
    {"Charter"},
    {"Cochin"},
    {"Comic Sans MS"},
    {"Copperplate"},
    {"Corsiva Hebrew"},
    {"Courier"},
    {"Courier New"},
    {"Damascus"},
    {"DecoType Naskh"},
    {"Devanagari MT"},
    {"Devanagari Sangam MN"},
    {"Didot"},
    {"DIN Alternate"},
    {"DIN Condensed"},
    {"Diwan Kufi"},
    {"Diwan Thuluth"},
    {"Euphemia UCAS"},
    {"Farah"},
    {"Farisi"},
    {"Futura"},
    {"Geeza Pro"},
    {"Geneva"},
    {"Georgia"},
    {"Gill Sans"},
    {"Gujarati MT"},
    {"Gujarati Sangam MN"},
    {"Gurmukhi MN"},
    {"Gurmukhi MT"},
    {"Gurmukhi Sangam MN"},
    {"Heiti SC"},
    {"Heiti TC"},
    {"Helvetica"},
    {"Helvetica Neue"},
    {"Herculanum"},
    {"Hiragino Kaku Gothic ProN"},
    {"Hiragino Maru Gothic ProN"},
    {"Hiragino Mincho ProN"},
    {"Hiragino Sans"},
    {"Hiragino Sans GB"},
    {"Hoefler Text"},
    {"Impact"},
    {"InaiMathi"},
    {"ITF Devanagari"},
    {"ITF Devanagari Marathi"},
    {"Kailasa"},
    {"Kannada MN"},
    {"Kannada Sangam MN"},
    {"Kefa"},
    {"Khmer MN"},
    {"Khmer Sangam MN"},
    {"Kohinoor Bangla"},
    {"Kohinoor Devanagari"},
    {"Kohinoor Telugu"},
    {"Kokonor"},
    {"Krungthep"},
    {"KufiStandardGK"},
    {"Lao MN"},
    {"Lao Sangam MN"},
    {"Lucida Grande"},
    {"Luminari"},
    {"Malayalam MN"},
    {"Malayalam Sangam MN"},
    {"Marker Felt"},
    {"Menlo"},
    {"Microsoft Sans Serif"},
    {"Mishafi"},
    {"Mishafi Gold"},
    {"Monaco"},
    {"Mshtakan"},
    {"Mukta Mahee"},
    {"Muna"},
    {"Myanmar MN"},
    {"Myanmar Sangam MN"},
    {"Nadeem"},
    {"New Peninim MT"},
    {"Noteworthy"},
    {"Noto Nastaliq Urdu"},
    {"Noto Sans Kannada"},
    {"Noto Sans Myanmar"},
    {"Noto Sans Oriya"},
    {"Optima"},
    {"Oriya MN"},
    {"Oriya Sangam MN"},
    {"Palatino"},
    {"Papyrus"},
    {"Party LET"},
    {"Phosphate"},
    {"PingFang HK"},
    {"PingFang SC"},
    {"PingFang TC"},
    {"Plantagenet Cherokee"},
    {"PT Mono"},
    {"PT Sans"},
    {"PT Sans Caption"},
    {"PT Sans Narrow"},
    {"PT Serif"},
    {"PT Serif Caption"},
    {"Raanana"},
    {"Rockwell"},
    {"Sana"},
    {"Sathu"},
    {"Savoye LET"},
    {"Shree Devanagari 714"},
    {"SignPainter"},
    {"Silom"},
    {"Sinhala MN"},
    {"Sinhala Sangam MN"},
    {"Skia"},
    {"Snell Roundhand"},
    {"Songti SC"},
    {"Songti TC"},
    {"STIX Two Math"},
    {"STIX Two Text"},
    {"STIXGeneral"},
    {"Sukhumvit Set"},
    {"Symbol"},
    {"Tahoma"},
    {"Tamil MN"},
    {"Tamil Sangam MN"},
    {"Telugu MN"},
    {"Telugu Sangam MN"},
    {"Thonburi"},
    {"Times"},
    {"Times New Roman"},
    {"Trattatello"},
    {"Trebuchet MS"},
    {"Verdana"},
    {"Waseem"},
    {"Webdings"},
    {"Wingdings"},
    {"Wingdings 2"},
    {"Wingdings 3"},
    {"Zapf Dingbats"},
    {"Zapfino"},
    // PostScript names of listed faces that sites commonly put in their
    // stacks (Blink on macOS also matches family requests by PostScript
    // name; these resolve to the same face on every Mac). "HelveticaNeue"
    // and "LucidaGrande" already match their families' entries.
    {"HelveticaNeue-Bold"},
    {"HelveticaNeue-Light"},
    {"HelveticaNeue-Medium"},
    {"HelveticaNeue-Thin"},
    {"HelveticaNeue-UltraLight"},
    {"HiraKakuProN-W3"},
    {"HiraKakuProN-W6"},
    {"HiraginoSans-W3"},
    {"HiraMinProN-W3"},
    // Localized family names of the CJK fonts above.
    {"\u30D2\u30E9\u30AE\u30CE\u89D2\u30B4 ProN"},  // Hiragino Kaku Gothic ProN
    {"\u30D2\u30E9\u30AE\u30CE\u89D2\u30B4\u30B7\u30C3\u30AF"},  // Hiragino
                                                                 // Sans
    {"\u30D2\u30E9\u30AE\u30CE\u660E\u671D ProN"},  // Hiragino Mincho ProN
    {"\u30D2\u30E9\u30AE\u30CE\u4E38\u30B4 ProN"},  // Hiragino Maru Gothic
    {"\u51AC\u9752\u9ED1\u4F53\u7B80\u4F53\u4E2D\u6587"},  // Hiragino Sans GB
    {"\u82F9\u65B9-\u7B80"},                               // PingFang SC
    {"\u860B\u65B9-\u7E41"},                               // PingFang TC
    {"\u860B\u65B9-\u6E2F"},                               // PingFang HK
    {"\u9ED1\u4F53-\u7B80"},                               // Heiti SC
    {"\u9ED1\u9AD4-\u7E41"},                               // Heiti TC
    {"\u5B8B\u4F53-\u7B80"},                               // Songti SC
    {"\u5B8B\u9AD4-\u7E41"},                               // Songti TC
    {"Apple SD \uC0B0\uB3CC\uACE0\uB515 Neo"},  // Apple SD Gothic Neo
    {"\uC560\uD50C\uACE0\uB515"},               // AppleGothic
    {"\uC560\uD50C\uBA85\uC870"},               // AppleMyungjo
};

// Linux: a deliberately small set that mainstream desktop distributions
// (Debian/Ubuntu, Fedora, openSUSE, Arch with a desktop) all install:
// DejaVu, Liberation 2 and the core Noto families. fontconfig substitutes the
// metric-compatible Liberation fonts for Arial / Times New Roman / Courier
// New, but a machine with the real (msttcorefonts) fonts gets those instead,
// so protected contexts always ask for the Liberation family directly.
constexpr FontListEntry kLinuxFonts[] = {
    {"DejaVu Sans"},
    {"DejaVu Sans Condensed"},
    {"DejaVu Sans Light"},
    {"DejaVu Sans Mono"},
    {"DejaVu Serif"},
    {"DejaVu Serif Condensed"},
    {"Liberation Mono"},
    {"Liberation Sans"},
    {"Liberation Serif"},
    {"Noto Color Emoji"},
    {"Noto Sans"},
    {"Noto Sans Mono"},
    {"Noto Serif"},
    {"Arial", "Liberation Sans"},
    {"Helvetica", "Liberation Sans"},
    {"Times New Roman", "Liberation Serif"},
    {"Times", "Liberation Serif"},
    {"Courier New", "Liberation Mono"},
    {"Courier", "Liberation Mono"},
};

// Android: the named families and aliases of AOSP's fonts.xml (the system
// resolves the aliases itself, the same way on every device of a release).
// OEM additions are not listed. Users can't install system fonts.
constexpr FontListEntry kAndroidFonts[] = {
    {"sans-serif"},
    {"sans-serif-thin"},
    {"sans-serif-light"},
    {"sans-serif-medium"},
    {"sans-serif-black"},
    {"sans-serif-condensed"},
    {"sans-serif-condensed-light"},
    {"sans-serif-condensed-medium"},
    {"sans-serif-smallcaps"},
    {"sans-serif-monospace"},
    {"serif"},
    {"serif-bold"},
    {"serif-monospace"},
    {"monospace"},
    {"casual"},
    {"cursive"},
    {"fantasy"},
    {"source-sans-pro"},
    {"source-sans-pro-semi-bold"},
    {"arial"},
    {"helvetica"},
    {"tahoma"},
    {"verdana"},
    {"times"},
    {"times new roman"},
    {"palatino"},
    {"georgia"},
    {"baskerville"},
    {"goudy"},
    {"ITC Stone Serif"},
    {"monaco"},
    {"courier"},
    {"courier new"},
};

}  // namespace

base::span<const FontListEntry> FontListFor(FontPlatform platform) {
  switch (platform) {
    case FontPlatform::kWindows:
      return kWindowsFonts;
    case FontPlatform::kMac:
      return kMacFonts;
    case FontPlatform::kLinux:
      return kLinuxFonts;
    case FontPlatform::kAndroid:
      return kAndroidFonts;
  }
  return kLinuxFonts;
}

}  // namespace blink::molt_fingerprinting
