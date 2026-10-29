// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// RunSandboxedHelper() on Windows: the helper runs as a less-privileged
// AppContainer of its own, in a job, created with a STARTUPINFOEX attribute
// list. helper_sandbox_win_policy.h holds every decision and says why; this
// file asks Windows for it with the sandbox library's own pieces: its
// AppContainer (with the access check that simulates an LPAC token), its
// attribute-list builder (which also leaves out mitigations an older
// Windows lacks) and its job levels.

#include <windows.h>

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/path_service.h"
#include "base/process/kill.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/time/time.h"
#include "base/win/access_token.h"
#include "base/win/scoped_handle.h"
#include "base/win/scoped_process_information.h"
#include "base/win/security_descriptor.h"
#include "base/win/sid.h"
#include "chrome/browser/molt_ai/common/helper_limits.h"
#include "chrome/browser/molt_ai/common/helper_limits_watch.h"
#include "chrome/browser/molt_ai/common/helper_sandbox.h"
#include "chrome/browser/molt_ai/common/helper_sandbox_win_policy.h"
#include "sandbox/win/src/app_container.h"
#include "sandbox/win/src/app_container_base.h"
#include "sandbox/win/src/job.h"
#include "sandbox/win/src/security_level.h"
#include "sandbox/win/src/startup_information_helper.h"

namespace molt_ai {

namespace hsw = helper_sandbox_win;

// The policy's numbers are the sandbox library's and the SDK's.
static_assert(hsw::kMitigationRelocateImage ==
              sandbox::MITIGATION_RELOCATE_IMAGE);
static_assert(hsw::kMitigationHeapTerminate ==
              sandbox::MITIGATION_HEAP_TERMINATE);
static_assert(hsw::kMitigationBottomUpAslr ==
              sandbox::MITIGATION_BOTTOM_UP_ASLR);
static_assert(hsw::kMitigationHighEntropyAslr ==
              sandbox::MITIGATION_HIGH_ENTROPY_ASLR);
static_assert(hsw::kMitigationStrictHandleChecks ==
              sandbox::MITIGATION_STRICT_HANDLE_CHECKS);
static_assert(hsw::kMitigationWin32kDisable ==
              sandbox::MITIGATION_WIN32K_DISABLE);
static_assert(hsw::kMitigationExtensionPointDisable ==
              sandbox::MITIGATION_EXTENSION_POINT_DISABLE);
static_assert(hsw::kMitigationDynamicCodeDisable ==
              sandbox::MITIGATION_DYNAMIC_CODE_DISABLE);
static_assert(hsw::kMitigationNonSystemFontDisable ==
              sandbox::MITIGATION_NONSYSTEM_FONT_DISABLE);
static_assert(hsw::kMitigationForceMsSignedBins ==
              sandbox::MITIGATION_FORCE_MS_SIGNED_BINS);
static_assert(hsw::kMitigationImageLoadNoRemote ==
              sandbox::MITIGATION_IMAGE_LOAD_NO_REMOTE);
static_assert(hsw::kMitigationImageLoadNoLowLabel ==
              sandbox::MITIGATION_IMAGE_LOAD_NO_LOW_LABEL);
static_assert(hsw::kMitigationImageLoadPreferSys32 ==
              sandbox::MITIGATION_IMAGE_LOAD_PREFER_SYS32);
static_assert(hsw::kMitigationKtmComponent ==
              sandbox::MITIGATION_KTM_COMPONENT);
static_assert(hsw::kMitigationCetStrictMode ==
              sandbox::MITIGATION_CET_STRICT_MODE);
static_assert(hsw::kMitigationFsctlDisabled ==
              sandbox::MITIGATION_FSCTL_DISABLED);
static_assert(hsw::kGenericRead == GENERIC_READ);
static_assert(hsw::kGenericExecute == GENERIC_EXECUTE);
static_assert(hsw::kFileListDirectory == FILE_LIST_DIRECTORY &&
              hsw::kFileListDirectory == FILE_READ_DATA);
static_assert(hsw::kFileAddFile == FILE_ADD_FILE &&
              hsw::kFileAddFile == FILE_WRITE_DATA);
static_assert(hsw::kFileAddSubdirectory == FILE_ADD_SUBDIRECTORY &&
              hsw::kFileAddSubdirectory == FILE_APPEND_DATA);
static_assert(hsw::kFileReadEa == FILE_READ_EA);
static_assert(hsw::kFileWriteEa == FILE_WRITE_EA);
static_assert(hsw::kFileTraverse == FILE_TRAVERSE &&
              hsw::kFileTraverse == FILE_EXECUTE);
static_assert(hsw::kFileDeleteChild == FILE_DELETE_CHILD);
static_assert(hsw::kFileReadAttributes == FILE_READ_ATTRIBUTES);
static_assert(hsw::kFileWriteAttributes == FILE_WRITE_ATTRIBUTES);
static_assert(hsw::kDelete == DELETE);
static_assert(hsw::kReadControl == READ_CONTROL);
static_assert(hsw::kSynchronize == SYNCHRONIZE);
static_assert(hsw::kFileGenericRead == FILE_GENERIC_READ);
// A file in the scratch directory: read and write, nothing more (not
// delete or rename it). The directory: no subdirectories, nothing deleted.
static_assert((hsw::kScratchFileRights &
               ~(FILE_GENERIC_READ | FILE_GENERIC_WRITE)) == 0);
static_assert((hsw::kScratchDirectoryRights &
               (FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD | DELETE)) == 0);
// sandbox::JobLevel::kLockdown allows one process.
static_assert(hsw::kActiveProcessLimit == 1);
// The job's memory limit is every OS's (helper_limits.h).
static_assert(hsw::kProcessMemoryLimit == helper_limits::kMaxHelperMemoryBytes);

namespace {

// Upper bound for |max_output_bytes| (and so for what one run buffers).
constexpr size_t kMaxOutputLimit = 64u << 20;

// How long to wait for a killed helper to go away, so its scratch
// directory can be deleted.
constexpr DWORD kKillWaitMs = 5000;

// Shown only in tools that list AppContainers; the SID is what counts.
constexpr wchar_t kPackageName[] = L"MoltBrowser.Helper";

std::string WithError(std::string_view what, DWORD error) {
  return std::string(what) + " (error " + base::NumberToString(error) + ")";
}

std::string DescribeTimeout(base::TimeDelta timeout) {
  if (timeout >= base::Seconds(1)) {
    return "timed out after " + base::NumberToString(timeout.InSeconds()) +
           " s";
  }
  return "timed out after " + base::NumberToString(timeout.InMilliseconds()) +
         " ms";
}

// The browser's user, for the security descriptors it creates.
std::optional<std::string> CurrentUserSid() {
  const std::optional<base::win::AccessToken> token =
      base::win::AccessToken::FromCurrentProcess();
  if (!token) {
    return std::nullopt;
  }
  const std::optional<std::wstring> sid = token->User().ToSddlString();
  if (!sid || !base::IsStringASCII(*sid)) {
    return std::nullopt;
  }
  return base::WideToASCII(*sid);
}

// Creates the directory |path| with the security descriptor |sddl|, which
// it has from the start. ERROR_SUCCESS or the error.
DWORD CreateDirectoryWithSddl(const base::FilePath& path,
                              const std::string& sddl) {
  std::optional<base::win::SecurityDescriptor> descriptor =
      base::win::SecurityDescriptor::FromSddl(base::ASCIIToWide(sddl));
  if (!descriptor) {
    return ERROR_INVALID_SECURITY_DESCR;
  }
  SECURITY_DESCRIPTOR absolute = descriptor->ToAbsolute();
  SECURITY_ATTRIBUTES attributes = {};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = &absolute;
  attributes.bInheritHandle = FALSE;
  return ::CreateDirectoryW(path.value().c_str(), &attributes)
             ? ERROR_SUCCESS
             : ::GetLastError();
}

// The run's AppContainer, as |plan| says.
std::unique_ptr<sandbox::AppContainerBase> MakeAppContainer(
    const hsw::LaunchPlan& plan) {
  std::optional<base::win::Sid> package =
      base::win::Sid::FromSddlString(base::ASCIIToWide(plan.package_sid));
  if (!package) {
    return nullptr;
  }
  auto container = std::make_unique<sandbox::AppContainerBase>(
      kPackageName, std::move(*package), sandbox::AppContainerType::kDerived);
  for (const std::string& capability : plan.capability_sids) {
    if (!container->AddCapabilitySddl(base::ASCIIToWide(capability))) {
      return nullptr;
    }
  }
  container->SetEnableLowPrivilegeAppContainer(
      plan.low_privilege_app_container);
  return container;
}

// What the helper would be denied, under its own token: empty if nothing.
std::string CheckAccess(sandbox::AppContainerBase& container,
                        const base::FilePath& program,
                        const HelperSandboxPolicy& policy) {
  for (const hsw::AccessCheck& check :
       hsw::PlanAccessChecks(policy.read_only_paths.size())) {
    base::FilePath path;
    switch (check.target) {
      case hsw::AccessTarget::kProgram:
        path = program;
        break;
      case hsw::AccessTarget::kReadOnlyPath:
        path = policy.read_only_paths[check.index];
        // As on Linux and macOS, a missing input is the helper's to report.
        if (!base::PathExists(path)) {
          continue;
        }
        break;
      case hsw::AccessTarget::kScratchDirectory:
        path = policy.writable_dir;
        break;
    }
    DWORD granted = 0;
    BOOL allowed = FALSE;
    if (!container.AccessCheck(path.value(),
                               base::win::SecurityObjectType::kFile,
                               check.desired_access, &granted, &allowed) ||
        !allowed) {
      return hsw::DescribeAccessFailure(check.target, path.AsUTF8Unsafe());
    }
  }
  return std::string();
}

enum class ReadEnd { kEof, kLimit, kDeadline, kError, kPastLimit };

// How long to wait at |now| before the deadline or the next check of
// |watch|, whichever comes first, in whole milliseconds. Never negative.
DWORD WaitMsBefore(base::TimeTicks deadline,
                   const HelperLimitsWatch& watch,
                   base::TimeTicks now) {
  const base::TimeDelta wait =
      std::max(std::min(deadline, watch.next_check()) - now, base::TimeDelta());
  return base::saturated_cast<DWORD>(wait.InMillisecondsRoundedUp());
}

// Reads the helper's output from |pipe| into |output| until it ends, the
// deadline, more than |max_bytes| (then |output| keeps |max_bytes| and
// |*truncated| is set), or a check of |watch| finds it past a limit. An
// anonymous pipe has no overlapped reads, so this peeks, and waits on
// |process| (which wakes it when the helper exits) while nothing is there.
ReadEnd ReadOutput(HANDLE pipe,
                   HANDLE process,
                   base::TimeTicks deadline,
                   size_t max_bytes,
                   HelperLimitsWatch* watch,
                   std::string* output,
                   bool* truncated) {
  std::array<char, 16384> buffer;
  base::TimeDelta pause = base::Milliseconds(1);
  bool exited = false;
  for (;;) {
    if (!watch->Check(base::TimeTicks::Now())) {
      return ReadEnd::kPastLimit;
    }
    DWORD available = 0;
    if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
      return ::GetLastError() == ERROR_BROKEN_PIPE ? ReadEnd::kEof
                                                   : ReadEnd::kError;
    }
    if (available == 0) {
      // Everything a helper wrote is in the pipe once it has exited, even
      // if a stray copy of the pipe's write end kept it open.
      if (exited) {
        return ReadEnd::kEof;
      }
      const base::TimeTicks now = base::TimeTicks::Now();
      if (!(deadline - now).is_positive()) {
        return ReadEnd::kDeadline;
      }
      exited = ::WaitForSingleObject(
                   process, std::min(base::saturated_cast<DWORD>(
                                         pause.InMillisecondsRoundedUp()),
                                     WaitMsBefore(deadline, *watch, now))) ==
               WAIT_OBJECT_0;
      pause = std::min(pause * 2, base::Milliseconds(50));
      continue;
    }
    pause = base::Milliseconds(1);
    // One byte past the limit tells "exactly the limit" from "more".
    const size_t wanted = std::min<size_t>(
        {buffer.size(), available, max_bytes + 1 - output->size()});
    DWORD bytes = 0;
    if (!::ReadFile(pipe, buffer.data(), static_cast<DWORD>(wanted), &bytes,
                    nullptr)) {
      return ::GetLastError() == ERROR_BROKEN_PIPE ? ReadEnd::kEof
                                                   : ReadEnd::kError;
    }
    output->append(buffer.data(), bytes);
    if (output->size() > max_bytes) {
      output->resize(max_bytes);
      *truncated = true;
      return ReadEnd::kLimit;
    }
  }
}

enum class WaitEnd { kExited, kDeadline, kPastLimit };

// Waits for |process| to exit until |deadline|, or until a check of |watch|
// finds it past a limit.
WaitEnd WaitForExitUntil(HANDLE process,
                         base::TimeTicks deadline,
                         HelperLimitsWatch* watch) {
  for (;;) {
    const base::TimeTicks now = base::TimeTicks::Now();
    if (!watch->Check(now)) {
      return WaitEnd::kPastLimit;
    }
    const DWORD waited =
        ::WaitForSingleObject(process, WaitMsBefore(deadline, *watch, now));
    if (waited == WAIT_OBJECT_0) {
      return WaitEnd::kExited;
    }
    if (waited != WAIT_TIMEOUT ||
        !(deadline - base::TimeTicks::Now()).is_positive()) {
      return WaitEnd::kDeadline;
    }
  }
}

}  // namespace

HelperRunResult RunSandboxedHelper(const base::CommandLine& cmd,
                                   const HelperSandboxPolicy& policy,
                                   base::TimeDelta timeout,
                                   size_t max_output_bytes) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  HelperRunResult result;
  const base::TimeTicks deadline = base::TimeTicks::Now() + timeout;
  max_output_bytes = std::min(max_output_bytes, kMaxOutputLimit);
  const base::FilePath program = cmd.GetProgram();
  const auto fail = [&result](std::string error) {
    result.sandbox_failed = true;
    result.error = std::move(error);
    LOG(ERROR) << "[MoltHelperSandbox] " << result.error;
    return result;
  };
  if (!program.IsAbsolute() || !policy.writable_dir.IsAbsolute()) {
    return fail("the helper and its scratch directory need absolute paths");
  }
  if (!base::PathExists(program)) {
    result.error = "could not start the helper";
    return result;
  }
  const std::optional<hsw::LaunchPlan> plan =
      hsw::PlanLaunch(policy.app_container_sid);
  if (!plan) {
    return fail("its scratch directory wasn't made for a helper sandbox");
  }
  std::unique_ptr<sandbox::AppContainerBase> container =
      MakeAppContainer(*plan);
  if (!container) {
    return fail("could not set up its AppContainer");
  }
  // Fail closed, and say why, before Windows would let it start and die.
  const std::string denied = CheckAccess(*container, program, policy);
  if (!denied.empty()) {
    return fail(denied);
  }
  // What the browser put in the scratch directory, which the helper's
  // limits are counted from.
  HelperLimitsWatch watch(policy.writable_dir);
  if (!watch.Start()) {
    return fail(watch.error());
  }

  // One process, a memory limit, killed when this returns (the job closes),
  // dies on an unhandled exception, no UI outside itself.
  sandbox::Job job;
  const DWORD job_error =
      job.Init(sandbox::JobLevel::kLockdown, /*ui_exceptions=*/0,
               base::saturated_cast<size_t>(plan->process_memory_limit));
  if (job_error != ERROR_SUCCESS) {
    return fail(WithError("could not set up its job", job_error));
  }

  // stdout and stderr: one pipe, whose write end only the helper inherits
  // (the handle list), and only until it is started.
  HANDLE read_raw = nullptr;
  HANDLE write_raw = nullptr;
  SECURITY_ATTRIBUTES inheritable = {};
  inheritable.nLength = sizeof(inheritable);
  inheritable.bInheritHandle = TRUE;
  if (!::CreatePipe(&read_raw, &write_raw, &inheritable, 0)) {
    result.error = WithError("could not create a pipe", ::GetLastError());
    return result;
  }
  base::win::ScopedHandle read_end(read_raw);
  base::win::ScopedHandle write_end(write_raw);
  if (!::SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0)) {
    result.error = WithError("could not create a pipe", ::GetLastError());
    return result;
  }

  base::FilePath windows_dir;
  if (!base::PathService::Get(base::DIR_WINDOWS, &windows_dir)) {
    return fail("could not find the Windows directory");
  }
  std::wstring environment = hsw::EnvironmentBlock(
      hsw::HelperEnvironment(windows_dir.value(), policy.writable_dir.value()));
  if (environment.empty()) {
    return fail("could not set up its environment");
  }

  sandbox::StartupInformationHelper startup;
  startup.UpdateFlags(STARTF_FORCEOFFFEEDBACK);
  startup.SetMitigations(plan->mitigations);
  startup.SetRestrictChildProcessCreation(plan->restrict_child_processes);
  if (plan->output_pipe_only) {
    startup.SetStdHandles(write_end.get(), write_end.get());
  }
  startup.SetAppContainer(container.get());
  if (plan->assign_job) {
    startup.AddJobToAssociate(job.GetHandle());
  }
  startup.SetEnvironment(std::move(environment));
  if (!startup.BuildStartupInformation()) {
    return fail(
        WithError("could not set up its process attributes", ::GetLastError()));
  }

  // No console of its own; the job holds it from its first instruction.
  DWORD flags = CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS;
  if (startup.GetStartupInformation()->has_extended_startup_info()) {
    flags |= EXTENDED_STARTUPINFO_PRESENT;
  }
  std::wstring command_line = cmd.GetCommandLineString();
  PROCESS_INFORMATION raw_info = {};
  const BOOL created = ::CreateProcessW(
      program.value().c_str(), command_line.data(),
      /*lpProcessAttributes=*/nullptr, /*lpThreadAttributes=*/nullptr,
      startup.ShouldInheritHandles(), flags, startup.GetEnvironment(),
      policy.writable_dir.value().c_str(),
      startup.GetStartupInformation()->startup_info(), &raw_info);
  const DWORD create_error = ::GetLastError();
  // Only the helper holds the write end now: its exit ends the output.
  write_end.Close();
  if (!created) {
    // The program was there: Windows refused the sandbox it asked for.
    return fail(WithError("Windows refused its sandbox", create_error));
  }
  base::win::ScopedProcessInformation process(raw_info);
  result.launched = true;

  const ReadEnd end = ReadOutput(read_end.get(), process.process_handle(),
                                 deadline, max_output_bytes, &watch,
                                 &result.output, &result.output_truncated);
  if (end != ReadEnd::kEof) {
    // Stop reading: the helper's next write fails.
    read_end.Close();
  }
  WaitEnd wait = WaitEnd::kDeadline;
  if (end == ReadEnd::kPastLimit) {
    wait = WaitEnd::kPastLimit;
  } else if (end == ReadEnd::kEof || end == ReadEnd::kLimit) {
    wait = WaitForExitUntil(process.process_handle(), deadline, &watch);
  }
  if (wait != WaitEnd::kExited) {
    ::TerminateJobObject(job.GetHandle(), base::win::kProcessKilledExitCode);
    ::WaitForSingleObject(process.process_handle(), kKillWaitMs);
    if (wait == WaitEnd::kPastLimit) {
      result.limit_exceeded = true;
      result.error = watch.error();
      LOG(WARNING) << "[MoltHelperSandbox] stopped: " << result.error;
    } else if (end == ReadEnd::kError) {
      result.error = "lost the helper's output";
    } else {
      result.timed_out = true;
      result.error = DescribeTimeout(timeout);
    }
    return result;
  }

  DWORD exit_code = 0;
  if (!::GetExitCodeProcess(process.process_handle(), &exit_code)) {
    result.error = "lost the helper's exit code";
    return result;
  }
  result.exit_code = static_cast<int>(exit_code);
  // Windows' loader ended it before its own code ran: a library it couldn't
  // load in its sandbox.
  if (result.output.empty()) {
    const std::string start_failure = hsw::DescribeStartFailure(exit_code);
    if (!start_failure.empty()) {
      result.launched = false;
      return fail("it couldn't start in its sandbox: " + start_failure);
    }
  }
  // What it wrote just before it exited counts too.
  if (!watch.Check(base::TimeTicks::Now(), /*force=*/true)) {
    result.limit_exceeded = true;
    result.error = watch.error();
    LOG(WARNING) << "[MoltHelperSandbox] stopped: " << result.error;
  }
  return result;
}

bool HelperScratchDir::Create(std::string_view prefix) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  if (!path_.empty()) {
    return false;
  }
  const std::optional<std::string> user = CurrentUserSid();
  base::FilePath temp;
  if (!user || !base::GetSecureTempDirectory(&temp)) {
    return false;
  }
  // The run's own AppContainer, which only this directory grants.
  std::array<uint32_t, 7> words = {};
  base::RandBytes(base::as_writable_byte_span(words));
  const std::string package_sid = hsw::AppContainerSid(words);
  const std::string sddl = hsw::ScratchDirectorySddl(*user, package_sid);
  if (sddl.empty()) {
    return false;
  }
  for (int attempt = 0; attempt < 8; ++attempt) {
    const std::string name =
        hsw::ScratchDirectoryName(prefix, base::RandUint64());
    if (name.empty()) {
      return false;
    }
    const base::FilePath path = temp.AppendASCII(name);
    const DWORD error = CreateDirectoryWithSddl(path, sddl);
    if (error == ERROR_SUCCESS) {
      path_ = path;
      app_container_sid_ = package_sid;
      return true;
    }
    // Never an existing directory: someone else's could be anything.
    if (error != ERROR_ALREADY_EXISTS) {
      LOG(ERROR) << "[MoltHelperSandbox] "
                 << WithError("could not create a scratch directory", error);
      return false;
    }
  }
  return false;
}

bool CreateHelperDataDirectory(const base::FilePath& path) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  const std::optional<std::string> user = CurrentUserSid();
  if (!user) {
    return false;
  }
  const std::string sddl = hsw::DataDirectorySddl(*user);
  return !sddl.empty() && CreateDirectoryWithSddl(path, sddl) == ERROR_SUCCESS;
}

bool ReadHelperOutputFile(const base::FilePath& path,
                          size_t max_bytes,
                          std::string* contents) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  contents->clear();
  // The file itself, not what a reparse point (a symlink or junction the
  // helper made) leads to; a directory doesn't open without
  // FILE_FLAG_BACKUP_SEMANTICS.
  base::win::ScopedHandle file(::CreateFileW(
      path.value().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!file.is_valid()) {
    return false;
  }
  // A regular file the helper created: one link (not a hard link to a file
  // outside the scratch directory), on a disk.
  BY_HANDLE_FILE_INFORMATION info = {};
  if (!::GetFileInformationByHandle(file.get(), &info) ||
      (info.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
      info.nNumberOfLinks != 1 || ::GetFileType(file.get()) != FILE_TYPE_DISK) {
    return false;
  }
  std::array<char, 16384> buffer;
  while (contents->size() < max_bytes) {
    const DWORD wanted = static_cast<DWORD>(
        std::min(buffer.size(), max_bytes - contents->size()));
    DWORD bytes = 0;
    if (!::ReadFile(file.get(), buffer.data(), wanted, &bytes, nullptr)) {
      return false;
    }
    if (bytes == 0) {
      break;
    }
    contents->append(buffer.data(), bytes);
  }
  return true;
}

}  // namespace molt_ai
