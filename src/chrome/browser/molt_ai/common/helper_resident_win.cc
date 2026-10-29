// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ResidentHelper on Windows: a less-privileged AppContainer of its own (its
// package SID, random per start, grants it nothing), in a job (one process,
// a memory limit, a hard CPU cap, killed with the job), created with a
// STARTUPINFOEX attribute list whose handle list holds only its stdin's
// and stdout's pipe ends. helper_sandbox_win_policy.h
// (PlanResidentLaunch()) holds the decisions; helper_sandbox_win.cc runs
// short-lived helpers the same way.

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
#include "base/memory/ptr_util.h"
#include "base/numerics/safe_conversions.h"
#include "base/path_service.h"
#include "base/process/kill.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/system/sys_info.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/time/time.h"
#include "base/win/scoped_handle.h"
#include "base/win/scoped_process_information.h"
#include "base/win/sid.h"
#include "chrome/browser/molt_ai/common/helper_limits.h"
#include "chrome/browser/molt_ai/common/helper_resident.h"
#include "chrome/browser/molt_ai/common/helper_sandbox_win_policy.h"
#include "sandbox/win/src/app_container.h"
#include "sandbox/win/src/app_container_base.h"
#include "sandbox/win/src/job.h"
#include "sandbox/win/src/security_level.h"
#include "sandbox/win/src/startup_information_helper.h"

namespace molt_ai {

namespace hsw = helper_sandbox_win;

ResidentHelperPolicy::ResidentHelperPolicy() = default;
ResidentHelperPolicy::ResidentHelperPolicy(const ResidentHelperPolicy&) =
    default;
ResidentHelperPolicy& ResidentHelperPolicy::operator=(
    const ResidentHelperPolicy&) = default;
ResidentHelperPolicy::~ResidentHelperPolicy() = default;

namespace {

// How long the destructor waits for a killed helper.
constexpr DWORD kKillWaitMs = 2000;

// Shown only in tools that list AppContainers; the SID is what counts.
constexpr wchar_t kPackageName[] = L"MoltBrowser.ResidentHelper";

std::string WithError(std::string_view what, DWORD error) {
  return std::string(what) + " (error " + base::NumberToString(error) + ")";
}

// The run's AppContainer, as |plan| says (as helper_sandbox_win.cc makes
// one).
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

// What it would be denied under its own token: empty if nothing.
std::string CheckAccess(sandbox::AppContainerBase& container,
                        const base::FilePath& program,
                        const ResidentHelperPolicy& policy) {
  for (const hsw::AccessCheck& check :
       hsw::PlanResidentAccessChecks(policy.read_only_paths.size())) {
    base::FilePath path = program;
    if (check.target == hsw::AccessTarget::kReadOnlyPath) {
      path = policy.read_only_paths[check.index];
      if (!base::PathExists(path)) {
        continue;  // the helper's to report
      }
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

}  // namespace

class ResidentHelper::Platform {
 public:
  Platform(base::win::ScopedProcessInformation process,
           std::unique_ptr<sandbox::Job> job,
           base::win::ScopedHandle stdin_write,
           base::win::ScopedHandle stdout_read)
      : process_(std::move(process)),
        job_(std::move(job)),
        stdin_write_(std::move(stdin_write)),
        stdout_read_(std::move(stdout_read)) {}

  ~Platform() {
    Kill();
    ::WaitForSingleObject(process_.process_handle(), kKillWaitMs);
  }

  bool Write(base::span<const uint8_t> data) {
    while (!data.empty()) {
      if (!stdin_write_.is_valid()) {
        return false;
      }
      DWORD written = 0;
      const DWORD wanted = base::saturated_cast<DWORD>(data.size());
      if (!::WriteFile(stdin_write_.get(), data.data(), wanted, &written,
                       nullptr) ||
          written == 0) {
        return false;  // ERROR_NO_DATA, ERROR_BROKEN_PIPE: it is gone.
      }
      data = data.subspan(written);
    }
    return true;
  }

  void CloseStdin() { stdin_write_.Close(); }

  int Read(base::span<uint8_t> buffer) {
    DWORD bytes = 0;
    const DWORD wanted =
        base::saturated_cast<DWORD>(std::min<size_t>(buffer.size(), 1u << 30));
    if (!::ReadFile(stdout_read_.get(), buffer.data(), wanted, &bytes,
                    nullptr)) {
      return ::GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    }
    return static_cast<int>(bytes);
  }

  void Kill() {
    // Everything in its job, which holds only it.
    ::TerminateJobObject(job_->GetHandle(), base::win::kProcessKilledExitCode);
  }

  std::optional<int> WaitForExit(base::TimeDelta timeout) {
    const DWORD waited = ::WaitForSingleObject(
        process_.process_handle(),
        base::saturated_cast<DWORD>(timeout.InMillisecondsRoundedUp()));
    if (waited != WAIT_OBJECT_0) {
      return std::nullopt;
    }
    DWORD exit_code = 0;
    if (!::GetExitCodeProcess(process_.process_handle(), &exit_code)) {
      return -1;
    }
    return static_cast<int>(exit_code);
  }

  // The most committed memory it has used (its job holds only it).
  uint64_t PeakMemory() {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    if (!::QueryInformationJobObject(job_->GetHandle(),
                                     JobObjectExtendedLimitInformation, &info,
                                     sizeof(info), nullptr)) {
      return 0;
    }
    return info.PeakProcessMemoryUsed;
  }

  std::optional<int64_t> CpuMs() {
    FILETIME created = {};
    FILETIME exited = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (!::GetProcessTimes(process_.process_handle(), &created, &exited,
                           &kernel, &user)) {
      return std::nullopt;
    }
    auto ticks = [](const FILETIME& time) {
      return (static_cast<uint64_t>(time.dwHighDateTime) << 32) |
             time.dwLowDateTime;
    };
    // 100 ns units.
    return static_cast<int64_t>((ticks(kernel) + ticks(user)) / 10000);
  }

  base::ProcessId pid() const { return process_.process_id(); }

 private:
  base::win::ScopedProcessInformation process_;
  std::unique_ptr<sandbox::Job> job_;
  base::win::ScopedHandle stdin_write_;
  base::win::ScopedHandle stdout_read_;
};

// static
std::unique_ptr<ResidentHelper> ResidentHelper::Start(
    const base::CommandLine& cmd,
    const ResidentHelperPolicy& policy,
    std::string* error) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  const base::FilePath program = cmd.GetProgram();
  auto fail = [&](std::string why) -> std::unique_ptr<ResidentHelper> {
    *error = std::move(why);
    LOG(ERROR) << "[MoltHelperSandbox] resident: " << *error;
    return nullptr;
  };
  if (!program.IsAbsolute()) {
    return fail("the helper needs an absolute path");
  }
  for (const base::FilePath& path : policy.read_only_paths) {
    if (!path.IsAbsolute()) {
      return fail("a read-only path is not absolute");
    }
  }
  if (!base::PathExists(program)) {
    return fail("could not start the helper");
  }

  // An AppContainer of its own: a package SID nothing grants anything.
  std::array<uint32_t, 7> words = {};
  base::RandBytes(base::as_writable_byte_span(words));
  const std::optional<hsw::LaunchPlan> plan = hsw::PlanResidentLaunch(
      hsw::AppContainerSid(words), policy.max_memory_bytes,
      helper_limits::kResidentCpuHardCapCores,
      base::checked_cast<uint32_t>(
          std::max(base::SysInfo::NumberOfProcessors(), 1)));
  if (!plan) {
    return fail("a resident helper needs a memory limit");
  }
  std::unique_ptr<sandbox::AppContainerBase> container =
      MakeAppContainer(*plan);
  if (!container) {
    return fail("could not set up its AppContainer");
  }
  const std::string denied = CheckAccess(*container, program, policy);
  if (!denied.empty()) {
    return fail(denied);
  }

  // One process, a memory limit, killed with the job, no UI outside
  // itself; and at most kResidentCpuHardCapCores of CPU.
  auto job = std::make_unique<sandbox::Job>();
  const DWORD job_error =
      job->Init(sandbox::JobLevel::kLockdown, /*ui_exceptions=*/0,
                base::saturated_cast<size_t>(plan->process_memory_limit));
  if (job_error != ERROR_SUCCESS) {
    return fail(WithError("could not set up its job", job_error));
  }
  JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu = {};
  cpu.ControlFlags =
      JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
  cpu.CpuRate = plan->cpu_rate;
  if (!::SetInformationJobObject(job->GetHandle(),
                                 JobObjectCpuRateControlInformation, &cpu,
                                 sizeof(cpu))) {
    return fail(WithError("could not cap its CPU", ::GetLastError()));
  }

  // Its stdin and stdout: two pipes, whose ends only it inherits (the
  // handle list), and only until it is started. No stderr.
  SECURITY_ATTRIBUTES inheritable = {};
  inheritable.nLength = sizeof(inheritable);
  inheritable.bInheritHandle = TRUE;
  HANDLE in_read = nullptr;
  HANDLE in_write = nullptr;
  if (!::CreatePipe(&in_read, &in_write, &inheritable, 0)) {
    return fail(WithError("could not create a pipe", ::GetLastError()));
  }
  base::win::ScopedHandle stdin_read(in_read);
  base::win::ScopedHandle stdin_write(in_write);
  HANDLE out_read = nullptr;
  HANDLE out_write = nullptr;
  if (!::CreatePipe(&out_read, &out_write, &inheritable, 0)) {
    return fail(WithError("could not create a pipe", ::GetLastError()));
  }
  base::win::ScopedHandle stdout_read(out_read);
  base::win::ScopedHandle stdout_write(out_write);
  if (!::SetHandleInformation(stdin_write.get(), HANDLE_FLAG_INHERIT, 0) ||
      !::SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0)) {
    return fail(WithError("could not create a pipe", ::GetLastError()));
  }

  base::FilePath windows_dir;
  if (!base::PathService::Get(base::DIR_WINDOWS, &windows_dir)) {
    return fail("could not find the Windows directory");
  }
  std::wstring environment = hsw::EnvironmentBlock(
      hsw::ResidentHelperEnvironment(windows_dir.value()));
  if (environment.empty()) {
    return fail("could not set up its environment");
  }

  sandbox::StartupInformationHelper startup;
  startup.UpdateFlags(STARTF_FORCEOFFFEEDBACK);
  startup.SetMitigations(plan->mitigations);
  startup.SetRestrictChildProcessCreation(plan->restrict_child_processes);
  startup.SetStdHandles(stdout_write.get(), /*stderr_handle=*/nullptr);
  startup.AddInheritedHandle(stdin_read.get());
  startup.SetAppContainer(container.get());
  startup.AddJobToAssociate(job->GetHandle());
  startup.SetEnvironment(std::move(environment));
  if (!startup.BuildStartupInformation()) {
    return fail(
        WithError("could not set up its process attributes", ::GetLastError()));
  }
  // The handle list holds stdin's read end; it is the helper's stdin.
  startup.GetStartupInformation()->startup_info()->hStdInput = stdin_read.get();

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
      program.DirName().value().c_str(),
      startup.GetStartupInformation()->startup_info(), &raw_info);
  const DWORD create_error = ::GetLastError();
  // Only the helper holds these ends now: its exit ends its stdout.
  stdin_read.Close();
  stdout_write.Close();
  if (!created) {
    return fail(WithError("Windows refused its sandbox", create_error));
  }
  auto platform = std::make_unique<Platform>(
      base::win::ScopedProcessInformation(raw_info), std::move(job),
      std::move(stdin_write), std::move(stdout_read));
  return base::WrapUnique(new ResidentHelper(std::move(platform), policy,
                                             "an LPAC AppContainer in a job"));
}

ResidentHelper::ResidentHelper(std::unique_ptr<Platform> platform,
                               const ResidentHelperPolicy& policy,
                               std::string confinement)
    : platform_(std::move(platform)),
      cpu_(policy.max_cpu_cores, policy.cpu_window.InMilliseconds()),
      max_memory_bytes_(policy.max_memory_bytes),
      confinement_(std::move(confinement)) {}

ResidentHelper::~ResidentHelper() {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  platform_.reset();
}

bool ResidentHelper::Write(base::span<const uint8_t> data) {
  return platform_->Write(data);
}

void ResidentHelper::CloseStdin() {
  platform_->CloseStdin();
}

int ResidentHelper::Read(base::span<uint8_t> buffer) {
  return platform_->Read(buffer);
}

bool ResidentHelper::CheckLimits(base::TimeTicks now, std::string* why) {
  // The job refuses memory past its limit; reaching it is a reason to stop.
  if (platform_->PeakMemory() >= max_memory_bytes_) {
    *why =
        "it used " + std::to_string(max_memory_bytes_ >> 20) + " MiB of memory";
    return false;
  }
  const std::optional<int64_t> cpu_ms = platform_->CpuMs();
  if (!cpu_ms) {
    return true;
  }
  if (!cpu_.Add((now - base::TimeTicks()).InMilliseconds(), *cpu_ms)) {
    *why = cpu_.Describe();
    return false;
  }
  return true;
}

void ResidentHelper::Kill() {
  platform_->Kill();
}

std::optional<int> ResidentHelper::WaitForExit(base::TimeDelta timeout) {
  return platform_->WaitForExit(timeout);
}

base::ProcessId ResidentHelper::pid() const {
  return platform_->pid();
}

}  // namespace molt_ai
