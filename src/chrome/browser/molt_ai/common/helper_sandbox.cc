// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/helper_sandbox.h"

#include <string>
#include <string_view>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
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
