// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/seatbelt_profile.h"

#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace seatbelt {

namespace {

// Everything but the read-only paths. Paths only ever appear as (param ...).
// Operation and filter names as in Chromium's sandbox/policy/mac/common.sb.
constexpr std::string_view kProfileHead = R"sb((version 1)
(deny default)
(deny network*)

; The helper: exec of its own binary only, its threads and children,
; signals to and information about itself, sysctl reads.
(allow process-exec (literal (param "HELPER")))
(allow process-fork)
(allow signal (target self))
(allow process-info-pidinfo (target self))
(allow sysctl-read)

; stat() and realpath() anywhere; file contents stay unreadable.
(allow file-read-metadata)

; dyld: the helper's bundled libraries, system libraries and frameworks,
; the shared cache and the OS cryptexes (as dyld-support.sb).
(allow file-read* file-map-executable
  (subpath (param "HELPER_DIR"))
  (subpath "/usr/lib")
  (subpath "/System/Library")
  (subpath "/Library/Apple")
  (subpath "/System/Cryptexes/App")
  (subpath "/System/Cryptexes/OS")
  (subpath "/System/Volumes/Preboot/Cryptexes/App/System")
  (subpath "/System/Volumes/Preboot/Cryptexes/OS"))

; System data and devices.
(allow file-read*
  (subpath "/private/var/db/dyld")
  (subpath "/private/var/db/timezone")
  (subpath "/usr/share/locale")
  (literal "/dev/null")
  (literal "/dev/random")
  (literal "/dev/urandom"))
(allow file-write-data (literal "/dev/null"))

; The scratch directory: list it; read and write beneath it. Not the
; directory itself, so it can't be replaced by a symlink.
(allow file-read* (literal (param "SCRATCH")))
(allow file-read* file-write*
  (prefix (string-append (param "SCRATCH") "/")))
)sb";

}  // namespace

std::string ReadOnlyParam(size_t index) {
  return "READ_" + std::to_string(index);
}

std::string BuildProfile(size_t read_only_count) {
  std::string profile(kProfileHead);
  if (read_only_count == 0) {
    return profile;
  }
  profile +=
      "\n; Read-only inputs: model or tessdata directory, the input file.\n"
      "(allow file-read*";
  for (size_t i = 0; i < read_only_count; ++i) {
    profile += "\n  (subpath (param \"" + ReadOnlyParam(i) + "\"))";
  }
  profile += ")\n";
  return profile;
}

std::vector<std::string> BuildSandboxExecArgv(
    const std::string& helper,
    const std::vector<std::string>& helper_args,
    const std::vector<std::string>& read_only,
    const std::string& scratch) {
  std::string helper_dir = helper.substr(0, helper.find_last_of('/'));
  if (helper_dir.empty()) {
    helper_dir = "/";
  }
  std::vector<std::string> argv = {
      kSandboxExecPath,
      "-p",
      BuildProfile(read_only.size()),
      "-D",
      std::string(kHelperParam) + "=" + helper,
      "-D",
      std::string(kHelperDirParam) + "=" + helper_dir,
  };
  for (size_t i = 0; i < read_only.size(); ++i) {
    argv.push_back("-D");
    argv.push_back(ReadOnlyParam(i) + "=" + read_only[i]);
  }
  argv.push_back("-D");
  argv.push_back(std::string(kScratchParam) + "=" + scratch);
  argv.push_back(helper);
  argv.insert(argv.end(), helper_args.begin(), helper_args.end());
  return argv;
}

bool IsSandboxExecFailure(int exit_code, std::string_view output) {
  // sysexits.h: EX_USAGE (64) .. EX_CONFIG (78). sandbox-exec reports a
  // profile error with EX_DATAERR (65) and a failed exec with EX_OSERR (71).
  constexpr std::string_view kPrefix = "sandbox-exec:";
  return exit_code >= 64 && exit_code <= 78 &&
         output.substr(0, kPrefix.size()) == kPrefix;
}

}  // namespace seatbelt
}  // namespace molt_ai
