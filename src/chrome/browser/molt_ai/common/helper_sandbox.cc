// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/helper_sandbox.h"

#include <string>
#include <string_view>

#include "base/files/file_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/common/helper_output.h"

namespace molt_ai {

HelperSandboxPolicy::HelperSandboxPolicy() = default;
HelperSandboxPolicy::HelperSandboxPolicy(const HelperSandboxPolicy&) = default;
HelperSandboxPolicy& HelperSandboxPolicy::operator=(
    const HelperSandboxPolicy&) = default;
HelperSandboxPolicy::~HelperSandboxPolicy() = default;

HelperRunResult::HelperRunResult() = default;
HelperRunResult::HelperRunResult(const HelperRunResult&) = default;
HelperRunResult& HelperRunResult::operator=(const HelperRunResult&) = default;
HelperRunResult::~HelperRunResult() = default;

HelperScratchDir::HelperScratchDir() = default;

HelperScratchDir::~HelperScratchDir() {
  if (path_.empty()) {
    return;
  }
#if BUILDFLAG(IS_POSIX)
  // A helper on Linux may have taken away the browser's own access to it
  // (Landlock doesn't cover chmod), which would keep it from being listed,
  // so emptied. A helper makes no directories in it.
  base::SetPosixFilePermissions(path_, base::FILE_PERMISSION_USER_MASK);
#endif
  base::DeletePathRecursively(path_);
}

HelperSandboxPolicy HelperScratchDir::MakePolicy() const {
  HelperSandboxPolicy policy;
  policy.writable_dir = path_;
  policy.app_container_sid = app_container_sid_;
  return policy;
}

std::string DescribeHelperRunFailure(const HelperRunResult& run,
                                     std::string_view name,
                                     std::string_view launch_error) {
  if (run.sandbox_failed) {
    return base::StrCat(
        {name, " sandbox unavailable: ", SanitizeHelperText(run.error, 300)});
  }
  if (!run.launched) {
    return std::string(launch_error);
  }
  if (run.limit_exceeded) {
    return base::StrCat({name, " was stopped: ", run.error});
  }
  if (run.timed_out) {
    return base::StrCat({name, " ", run.error});
  }
  if (run.exit_code != 0) {
    // The helper read an untrusted file: its output need not be UTF-8.
    return base::StrCat({name, " exit=", base::NumberToString(run.exit_code),
                         ": ", SanitizeHelperText(run.output, 200)});
  }
  return std::string();
}

}  // namespace molt_ai
