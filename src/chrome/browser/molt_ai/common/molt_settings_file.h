// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ~/.moltbrowser/settings.json, MoltBrowser's own settings file: one writer
// for the whole browser.
//
//   - Every change is a read-modify-write of named keys on one sequence
//     (MoltSettingsFileTaskRunner(): MayBlock, BLOCK_SHUTDOWN), so changes
//     made at the same time from different places (molt://ai-settings, the
//     voice loop's speech rate, voice mode's switches, the built-in AI
//     APIs' switch) never drop each other's keys.
//   - Written atomically (base::ImportantFileWriter::WriteFileAtomically():
//     a temporary file in the same directory, then a rename), so a crash
//     leaves the old file or the new one, never part of one.
//   - Read with a cap (kMaxMoltSettingsFileBytes). A file over it, or one
//     that isn't a JSON object, is left alone: nothing is written over it
//     (it is the user's to fix), and the change reports failure.
//   - Readers may read from any thread that may block: a write is a rename,
//     so they see the old file or the new one. Reading on the sequence also
//     sees every change posted before.
//
// Writers: voice_session_settings.h, voice_reading_settings.h,
// voice_language_settings.h,
// web_ai_settings_switch.h and molt://ai-settings (molt_ai_settings_ui.cc).

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_MOLT_SETTINGS_FILE_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_MOLT_SETTINGS_FILE_H_

#include <stddef.h>

#include <optional>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/functional/function_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/values.h"

namespace base {
class SequencedTaskRunner;
}  // namespace base

namespace molt_ai {

// settings.json is at most a few KiB; anything bigger isn't ours.
inline constexpr size_t kMaxMoltSettingsFileBytes = 1 << 20;

// ~/.moltbrowser/settings.json (or the test's path); nullopt without a home
// directory.
std::optional<base::FilePath> MoltSettingsFilePath();

// Tests: settings.json is |path| instead, until called with nullopt.
void SetMoltSettingsFilePathForTesting(std::optional<base::FilePath> path);

// The one sequence that changes settings.json.
scoped_refptr<base::SequencedTaskRunner> MoltSettingsFileTaskRunner();

enum class MoltSettingsRead {
  kMissing,      // no file: the defaults apply, and a change creates it
  kObject,       // a JSON object of at most kMaxMoltSettingsFileBytes
  kNotAnObject,  // anything else: left alone
};

// Blocking. Reads |path| into |out| (cleared unless kObject). JSON with
// trailing commas is accepted, as the user may have edited it.
MoltSettingsRead ReadMoltSettingsFileAt(const base::FilePath& path,
                                        base::DictValue* out);

// Blocking. The settings: empty when the file is missing, nullopt when it
// isn't a JSON object or there is no home directory.
std::optional<base::DictValue> ReadMoltSettings();

// |changes|' keys set over |settings| (a key whose value is none is
// removed); every other key of |settings| kept.
void MergeMoltSettings(base::DictValue& settings,
                       const base::DictValue& changes);

// Blocking: reads |path|, lets |update| change the settings (empty when the
// file is missing) and writes them atomically, creating the directory if
// needed. False, and nothing written, when the file isn't a JSON object,
// |update| returns false, or the write fails. Run on
// MoltSettingsFileTaskRunner(), or in a test on a file of its own.
using MoltSettingsUpdate = base::FunctionRef<bool(base::DictValue& settings)>;
bool UpdateMoltSettingsFileAt(const base::FilePath& path,
                              MoltSettingsUpdate update);

// The same for settings.json, on MoltSettingsFileTaskRunner() only.
bool UpdateMoltSettings(MoltSettingsUpdate update);
// Sets |changes|' keys (MergeMoltSettings()), on the sequence only.
bool SetMoltSettings(const base::DictValue& changes);

// From any thread: SetMoltSettings(|changes|) on the settings sequence,
// then |done| (if any; it needs a calling sequence) with its result on the
// calling sequence.
void PostSetMoltSettings(
    base::DictValue changes,
    base::OnceCallback<void(bool)> done = base::OnceCallback<void(bool)>());

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_MOLT_SETTINGS_FILE_H_
