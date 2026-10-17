// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// RunSandboxedHelper() on Windows: a job object and the deadline. The job
// kills the helper (and anything it started) when the run ends or the
// browser dies, turns a crash into an exit instead of an error dialog, and
// keeps it off the clipboard, global atoms, other processes' windows and
// system settings. There is no file system or network confinement yet:
// that needs an AppContainer, plus ACLs granting it the bundle and the
// scratch directory (as the inference sandbox will).

#include <windows.h>

#include <string>
#include <string_view>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/time/time.h"
#include "base/win/scoped_handle.h"
#include "chrome/browser/molt_ai/common/helper_sandbox.h"

namespace molt_ai {

namespace {

// How long to wait for a killed helper to go away, so its scratch
// directory can be deleted.
constexpr DWORD kKillWaitMs = 5000;

bool ConfigureJob(HANDLE job) {
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
      JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
    return false;
  }
  JOBOBJECT_BASIC_UI_RESTRICTIONS ui = {};
  ui.UIRestrictionsClass =
      JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
      JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
      JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
      JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
  return ::SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui,
                                   sizeof(ui));
}

std::string DescribeTimeout(base::TimeDelta timeout) {
  if (timeout >= base::Seconds(1)) {
    return "timed out after " + base::NumberToString(timeout.InSeconds()) +
           " s";
  }
  return "timed out after " + base::NumberToString(timeout.InMilliseconds()) +
         " ms";
}

}  // namespace

HelperRunResult RunSandboxedHelper(const base::CommandLine& cmd,
                                   const HelperSandboxPolicy& policy,
                                   base::TimeDelta timeout,
                                   size_t max_output_bytes) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  HelperRunResult result;
  if (!cmd.GetProgram().IsAbsolute() || !policy.writable_dir.IsAbsolute()) {
    result.sandbox_failed = true;
    result.error = "the helper and its scratch directory need absolute paths";
    return result;
  }
  base::win::ScopedHandle job(::CreateJobObject(nullptr, nullptr));
  if (!job.is_valid() || !ConfigureJob(job.get())) {
    result.sandbox_failed = true;
    result.error = "could not set up a job object (error " +
                   base::NumberToString(::GetLastError()) + ")";
    return result;
  }

  base::LaunchOptions options;
  options.job_handle = job.get();
  options.start_hidden = true;
  options.current_directory = policy.writable_dir;
  options.environment[L"TEMP"] = policy.writable_dir.value();
  options.environment[L"TMP"] = policy.writable_dir.value();

  // base drains the pipe until the helper exits or |timeout| passes; this
  // keeps the first |max_output_bytes| and discards the rest.
  base::Process helper;
  std::string output;
  bool truncated = false;
  int exit_code = -1;
  base::TerminationStatus status = base::TERMINATION_STATUS_LAUNCH_FAILED;
  base::GetAppOutputWithExitCodeAndTimeout(
      cmd.GetCommandLineString(), /*include_stderr=*/true, /*output=*/nullptr,
      &exit_code, timeout, options,
      [&](const base::Process& process, std::string_view partial_output) {
        if (!helper.IsValid()) {
          helper = process.Duplicate();
        }
        const size_t room = max_output_bytes - output.size();
        if (partial_output.size() > room) {
          truncated = true;
        }
        output.append(partial_output.substr(0, room));
      },
      &status);

  result.output = std::move(output);
  result.output_truncated = truncated;
  if (status == base::TERMINATION_STATUS_LAUNCH_FAILED) {
    result.error = "could not start the helper";
    return result;
  }
  result.launched = true;
  if (status == base::TERMINATION_STATUS_STILL_RUNNING) {
    ::TerminateJobObject(job.get(), base::win::kProcessKilledExitCode);
    if (helper.IsValid()) {
      ::WaitForSingleObject(helper.Handle(), kKillWaitMs);
    }
    result.timed_out = true;
    result.error = DescribeTimeout(timeout);
    return result;
  }
  result.exit_code = exit_code;
  return result;
}

bool ReadHelperOutputFile(const base::FilePath& path,
                          size_t max_bytes,
                          std::string* contents) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  contents->clear();
  if (!base::PathExists(path) || base::DirectoryExists(path)) {
    return false;
  }
  // False for a longer file, but |contents| then holds its first
  // |max_bytes|.
  return base::ReadFileToStringWithMaxSize(path, contents, max_bytes) ||
         contents->size() == max_bytes;
}

}  // namespace molt_ai
