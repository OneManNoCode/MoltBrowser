// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// FindHelperBinary — the one way MoltBrowser locates a helper executable
// (tor, tesseract, whisper-cli). base::FilePath front end for the policy in
// trusted_binary.h:
//
//   1. the copy bundled with MoltBrowser (|bundle_dir| + one of
//      |bundled_names|): accepted if it stays inside the bundle, is a regular
//      executable file and is not writable by everyone;
//   2. on Linux only, fixed system paths (e.g. /usr/bin/tor): accepted only
//      if root owns the file and every directory above it and none of them
//      is group- or world-writable.
//
// Never PATH, Homebrew, /usr/local or /opt/local (audit S18). On Windows only
// the bundled copy next to the executable is accepted (no ACL checks).

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_HELPER_BINARY_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_HELPER_BINARY_H_

#include <string>
#include <vector>

#include "base/files/file_path.h"

namespace molt_ai {

struct HelperBinary {
  HelperBinary();
  HelperBinary(const HelperBinary&);
  HelperBinary& operator=(const HelperBinary&);
  ~HelperBinary();

  bool found() const { return !path.empty(); }

  // Canonical path to execute (symlinks resolved). Empty if nothing was
  // accepted.
  base::FilePath path;
  // True iff |path| is the bundled copy.
  bool bundled = false;
  // When !found(): why the most relevant candidate was refused (for logs and
  // UI), and whether that candidate existed at all.
  std::string error;
  bool rejected_existing = false;
};

// Blocking, but only a few stat()/realpath() calls, so it may run on the UI
// thread.
HelperBinary FindHelperBinary(
    const base::FilePath& bundle_dir,
    const std::vector<base::FilePath>& bundled_names,
    const std::vector<std::string>& linux_system_paths);

// An explicit, absolute command-line override (--moltnet-tor-binary). On
// POSIX the file and every directory above it must be owned by root or the
// current user and not be group- or world-writable.
HelperBinary CheckExplicitHelperBinary(const base::FilePath& path);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_HELPER_BINARY_H_
