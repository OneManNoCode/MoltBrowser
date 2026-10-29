// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The macOS confinement for helper programs: short-lived ones (tesseract,
// whisper-cli; helper_sandbox.h is their front end) and resident ones (the
// speech helper; helper_resident.h). The helper runs under
// /usr/bin/sandbox-exec with the Seatbelt profile below.
//
// The profile text is fixed: it depends only on how many read-only paths
// there are, never on the paths themselves. Paths are passed as
// "-D KEY=VALUE" parameters and referenced as (param "KEY"), so no path can
// inject profile syntax. The profile denies everything by default and
// allows:
//   - exec of the helper itself (HELPER), but no other process (no fork:
//     the launcher's kill and checks reach all of it), signals to and
//     information about itself, and sysctl reads;
//   - stat() anywhere (dyld and libc resolve paths), but no file contents
//     outside the paths below (Chromium's common.sb grants the same to
//     every sandboxed process);
//   - reading and mapping code from the helper's own directory (HELPER_DIR:
//     its bundled libraries and data) and the system's libraries,
//     frameworks, shared cache and OS cryptexes (/usr/lib, /System/Library,
//     /Library/Apple, /System/Cryptexes, /System/Volumes/Preboot/Cryptexes);
//   - reading /private/var/db/dyld, time zone and locale data, /dev/null
//     (also writing), /dev/random and /dev/urandom;
//   - reading beneath each READ_<i> (the model or tessdata directory, the
//     input file);
//   - reading beneath SCRATCH, and creating and writing regular files
//     there; not deleting or renaming them, making directories or symbolic
//     links, or changing modes, flags, owners, times or extended
//     attributes, so all it writes stays where the launcher measures it
//     (helper_limits.h); not replacing SCRATCH itself. A resident helper
//     has no SCRATCH: it may write nothing but /dev/null (and the pipes it
//     was started with).
// No network, no Mach service lookups, no IOKit (so no GPU): a helper that
// wants any of those fails rather than reaching them.
//
// Pure C++ without //base, compiled on every platform so the unit tests
// check the profile everywhere.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_SEATBELT_PROFILE_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_SEATBELT_PROFILE_H_

#include <stddef.h>

#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace seatbelt {

inline constexpr char kSandboxExecPath[] = "/usr/bin/sandbox-exec";

// Parameter names.
inline constexpr char kHelperParam[] = "HELPER";
inline constexpr char kHelperDirParam[] = "HELPER_DIR";
inline constexpr char kScratchParam[] = "SCRATCH";
// "READ_0", "READ_1", ...
std::string ReadOnlyParam(size_t index);

// The profile for a run with |read_only_count| read-only paths.
std::string BuildProfile(size_t read_only_count);
// The same without a scratch directory, for a resident helper.
std::string BuildResidentProfile(size_t read_only_count);

// The full command line:
//   sandbox-exec -p <profile> -D HELPER=<helper> -D HELPER_DIR=<its dir>
//       -D READ_<i>=<read_only[i]>... -D SCRATCH=<scratch>
//       <helper> <helper_args>...
// All paths must be absolute and canonical (symlinks resolved, e.g.
// /private/var rather than /var): Seatbelt matches resolved paths.
std::vector<std::string> BuildSandboxExecArgv(
    const std::string& helper,
    const std::vector<std::string>& helper_args,
    const std::vector<std::string>& read_only,
    const std::string& scratch);

// A resident helper's command line: the same without SCRATCH.
std::vector<std::string> BuildResidentSandboxExecArgv(
    const std::string& helper,
    const std::vector<std::string>& helper_args,
    const std::vector<std::string>& read_only);

// True when a run's exit status and output say sandbox-exec itself failed
// (bad profile, sandbox refused, or exec of the helper denied): it prints
// "sandbox-exec: ..." and exits with a sysexits code (64-78). The helper
// did not run.
bool IsSandboxExecFailure(int exit_code, std::string_view output);

}  // namespace seatbelt
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_SEATBELT_PROFILE_H_
