// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/helper_limits_watch.h"

#include <stdint.h>

#include <string>

#include "base/files/file_path.h"
#include "base/process/process_handle.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/common/helper_limits.h"

#if BUILDFLAG(IS_MAC)
#include <errno.h>
#include <libproc.h>
#include <sys/resource.h>
#endif

namespace molt_ai {

namespace hl = helper_limits;

namespace {

#if BUILDFLAG(IS_MAC)
// Empty while the process |pid| uses at most kMaxHelperMemoryBytes (its
// footprint: what Activity Monitor shows as its memory) or is gone;
// otherwise why not.
std::string CheckMemory(base::ProcessId pid) {
  rusage_info_v2 usage = {};
  if (proc_pid_rusage(pid, RUSAGE_INFO_V2,
                      reinterpret_cast<rusage_info_t*>(&usage)) != 0) {
    return errno == ESRCH ? std::string()
                          : "its memory use couldn't be measured";
  }
  return usage.ri_phys_footprint > hl::kMaxHelperMemoryBytes
             ? hl::DescribeMemoryLimit()
             : std::string();
}
#endif  // BUILDFLAG(IS_MAC)

}  // namespace

HelperLimitsWatch::HelperLimitsWatch(const base::FilePath& scratch)
    : scratch_(scratch) {}

HelperLimitsWatch::~HelperLimitsWatch() = default;

bool HelperLimitsWatch::Start() {
  before_ = hl::MeasureScratch(scratch_.value(), hl::kMaxScratchNewEntries);
  if (before_.gap != hl::ScratchUsage::Gap::kNone) {
    error_ = "its scratch directory couldn't be measured";
    return false;
  }
  return true;
}

void HelperLimitsWatch::WatchMemoryOf(base::ProcessId pid) {
  pid_ = pid;
}

bool HelperLimitsWatch::Check(base::TimeTicks now, bool force) {
  if (!error_.empty()) {
    return false;
  }
  if (!force && now < next_check_) {
    return true;
  }
  error_ = hl::DescribeScratchVerdict(hl::JudgeScratch(
      before_,
      hl::MeasureScratch(scratch_.value(), hl::MaxScratchEntries(before_))));
#if BUILDFLAG(IS_MAC)
  if (error_.empty() && pid_ != base::kNullProcessId) {
    error_ = CheckMemory(pid_);
  }
#endif
  next_check_ =
      base::TimeTicks::Now() + base::Milliseconds(hl::kScratchCheckIntervalMs);
  return error_.empty();
}

}  // namespace molt_ai
