// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// RunSandboxedHelper()'s side of helper_limits.h: it measures the helper's
// scratch directory before the helper starts, then every
// kScratchCheckIntervalMs while it runs (on macOS also the helper's memory
// footprint), and once more after it exits. Only the helper sandbox's
// launchers use it.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_HELPER_LIMITS_WATCH_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_HELPER_LIMITS_WATCH_H_

#include <string>

#include "base/files/file_path.h"
#include "base/process/process_handle.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/common/helper_limits.h"

namespace molt_ai {

class HelperLimitsWatch {
 public:
  explicit HelperLimitsWatch(const base::FilePath& scratch);
  HelperLimitsWatch(const HelperLimitsWatch&) = delete;
  HelperLimitsWatch& operator=(const HelperLimitsWatch&) = delete;
  ~HelperLimitsWatch();

  // Measures what the browser put in the scratch directory, before the
  // helper starts. False, with error() set, if it can't be measured: the
  // helper must not start then. Blocking.
  bool Start();

  // macOS: the helper's process, whose memory footprint each check reads.
  // Unused elsewhere (Linux and Windows limit memory in the kernel).
  void WatchMemoryOf(base::ProcessId pid);

  // When the next check is due.
  base::TimeTicks next_check() const { return next_check_; }

  // Checks the limits if a check is due at |now|, or always if |force|.
  // False once the helper is past one; error() says which, e.g. "it wrote
  // 64 MiB or more to its scratch directory". Blocking.
  bool Check(base::TimeTicks now, bool force = false);

  const std::string& error() const { return error_; }

 private:
  const base::FilePath scratch_;
  helper_limits::ScratchUsage before_;
  base::ProcessId pid_ = base::kNullProcessId;
  base::TimeTicks next_check_;
  std::string error_;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_HELPER_LIMITS_WATCH_H_
