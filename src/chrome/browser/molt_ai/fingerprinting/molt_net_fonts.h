// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's bundled fonts, browser side (docs/FINGERPRINTING.md, "Fonts").
//
// MoltBrowser ships one font set for MoltNet windows (tools/fonts/
// moltnet_fonts.json; installed by scripts/bundle-fonts.sh) next to the
// browser:
//   macOS:          MoltBrowser.app/Contents/Resources/molt_fonts/
//   Windows, Linux: <the executable's folder>/molt_fonts/
// The browser opens every file of the set once, read-only, on a background
// sequence when the first renderer launches, and checks it against the
// table generated from the manifest (molt_net_fonts_table.h: the same files,
// in the same order, as Blink's table): its size, and that it starts like a
// font. Each MoltNet renderer launched after that gets --molt-net-fonts on
// its command line and, over its channel right after the session seed,
// read-only duplicates of the files (FingerprintingConfiguration.
// SetMoltNetFonts); Blink then takes every font from them and never opens a
// font file itself (core/molt_fingerprinting/moltnet_fonts.h).
//
// Without the set (a development build that didn't bundle it, a damaged
// install), MoltNet renderers get neither and keep MoltNet's per-OS font
// allow-lists, as before the set existed; the browser logs which file is
// missing or wrong. Packaging refuses to ship without it
// (scripts/bundle-fonts.sh).

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_FONTS_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_FONTS_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"

namespace molt_ai::fingerprinting {

// The set's folder next to the browser.
inline constexpr base::FilePath::CharType kMoltNetFontsFolder[] =
    FILE_PATH_LITERAL("molt_fonts");

// Where this installation keeps the set (see above); empty if the
// executable's path is unknown.
base::FilePath GetMoltNetFontsDirectory();

// Whether a file of the set looks right: `length` is the table's size for
// file `index`, and `head` (its first bytes) starts like an OpenType font or
// collection.
bool IsExpectedMoltNetFontFile(size_t index,
                               int64_t length,
                               base::span<const uint8_t> head);

// Opens every file of the table in `directory`, read-only, in table order,
// and checks each one (IsExpectedMoltNetFontFile). Blocks. nullopt, after
// logging which file failed, if any is missing or wrong.
std::optional<std::vector<base::File>> OpenMoltNetFonts(
    const base::FilePath& directory);

// UI thread. Starts opening the set in the background, once per browser
// process (later calls do nothing).
void StartLoadingMoltNetFonts();
// UI thread. Whether the set is open and checked.
bool AreMoltNetFontsReady();
// UI thread. Read-only duplicates of the set's files for one renderer, in
// table order; empty when the set isn't ready or a duplicate fails.
std::vector<base::File> DuplicateMoltNetFonts();

// Tests: replaces the loaded set (nullopt: not loaded).
void SetMoltNetFontsForTesting(std::optional<std::vector<base::File>> files);

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_FONTS_H_
