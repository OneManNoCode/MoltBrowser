// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser's own directory in the user's home, ~/.moltbrowser: its
// settings.json (molt_settings_file.h) and the shared workflows
// (automations/, automation_storage.h), among others. The voice routines
// and the learning file are in each profile's directory, which tests
// already make their own; the routines from before routines belonged to a
// profile are in the shared workflows.
//
// Tests: SetMoltHomeDirForTesting() (or a ScopedMoltHomeDirForTesting,
// molt_home_dir_test_util.h) makes another directory this one before any
// profile exists, so a browser test never reads or changes the real
// ~/.moltbrowser (a profile's first listing of its routines settles claims
// cut short there, which changes files). No I/O; any thread.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_MOLT_HOME_DIR_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_MOLT_HOME_DIR_H_

#include <optional>

#include "base/files/file_path.h"

namespace molt_ai {

// <home>/.moltbrowser, or the test's directory; nullopt without a home
// directory (base::DIR_HOME unknown).
std::optional<base::FilePath> MoltHomeDir();

// Tests: MoltHomeDir() is |dir| from now on (an empty path: the real one
// again). Set it before any profile, window or storage reads it.
void SetMoltHomeDirForTesting(const base::FilePath& dir);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_MOLT_HOME_DIR_H_
