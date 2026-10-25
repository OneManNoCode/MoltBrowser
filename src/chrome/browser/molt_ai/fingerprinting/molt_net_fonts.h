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
// sequence: at startup, once startup-critical work is done, or at the first
// renderer launch if that comes first. It checks each file against the table
// generated from the manifest (molt_net_fonts_table.h: the same files, in the
// same order, as Blink's table): its size, and that it starts like a font.
//
// Every MoltNet renderer gets --molt-net-fonts on its command line unless the
// browser already knows this installation has no set (no molt_fonts folder:
// a development build that didn't bundle it). Blink then takes every font
// from the set and never from the system (core/molt_fingerprinting/
// moltnet_fonts.h). The files follow over the renderer's channel as
// read-only duplicates (FingerprintingConfiguration.SetMoltNetFonts): at once
// when the set is open, else as soon as it is. Until they arrive the
// renderer draws text with an empty font; it never chooses its fonts by
// timing. A damaged set (the folder is there but a file is missing or wrong)
// is never sent: MoltNet renderers then stay without fonts, and the browser
// logs which file is wrong. Packaging refuses to ship without the set
// (scripts/bundle-fonts.sh, scripts/release.sh).

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_FONTS_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_FONTS_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"

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

// What opening the set found.
enum class MoltNetFontsStatus {
  kReady,   // Every file is there and checked.
  kAbsent,  // No set folder: an installation without the set.
  kBroken,  // The folder is there, but a file is missing or wrong.
};
struct MoltNetFontsLoad {
  MoltNetFontsLoad();
  MoltNetFontsLoad(MoltNetFontsStatus status, std::vector<base::File> files);
  MoltNetFontsLoad(MoltNetFontsLoad&&);
  MoltNetFontsLoad& operator=(MoltNetFontsLoad&&);
  ~MoltNetFontsLoad();

  MoltNetFontsStatus status = MoltNetFontsStatus::kAbsent;
  std::vector<base::File> files;  // The set, when kReady.
};
// Opens the set in `directory` (OpenMoltNetFonts). Blocks.
MoltNetFontsLoad LoadMoltNetFonts(const base::FilePath& directory);

// UI thread. Starts opening the set at browser startup: posts the start as a
// best-effort UI task, which runs once startup-critical work is done.
void StartLoadingMoltNetFontsAfterStartup();
// UI thread. Starts opening the set in the background now, once per browser
// process (later calls, and the call above if it comes later, do nothing).
void StartLoadingMoltNetFonts();
// UI thread. Whether the set is open and checked.
bool AreMoltNetFontsReady();
// UI thread. Whether a MoltNet renderer launching now is to expect the set
// (--molt-net-fonts): always, unless the set is known to be absent.
bool MoltNetRenderersExpectFonts();
// UI thread. Read-only duplicates of the set's files for one renderer, in
// table order; empty when the set isn't ready or a duplicate fails.
std::vector<base::File> DuplicateMoltNetFonts();
// UI thread. Runs `deliver` with read-only duplicates of the set: now if it
// is ready, else once it has been opened. Never if it is absent or damaged.
using MoltNetFontsCallback = base::OnceCallback<void(std::vector<base::File>)>;
void WhenMoltNetFontsReady(MoltNetFontsCallback deliver);

// Tests: replaces the loaded set (nullopt: not loaded, nothing waiting).
void SetMoltNetFontsForTesting(std::optional<std::vector<base::File>> files);
// Tests: as if opening had started and not finished (nothing is opened).
void SetMoltNetFontsLoadingForTesting();
// Tests: as if opening had finished with `load`.
void FinishMoltNetFontsLoadForTesting(MoltNetFontsLoad load);

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_FONTS_H_
