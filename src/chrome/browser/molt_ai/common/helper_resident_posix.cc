// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ResidentHelper on Linux (Landlock and seccomp-bpf in a pre-exec delegate)
// and macOS (sandbox-exec). Other POSIX platforms fail closed.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/posix/eintr_wrapper.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/synchronization/lock.h"
#include "base/threading/platform_thread.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/common/helper_limits.h"
#include "chrome/browser/molt_ai/common/helper_resident.h"

#if BUILDFLAG(IS_LINUX)
#include "chrome/browser/molt_ai/common/helper_sandbox_linux.h"
#endif

#if BUILDFLAG(IS_MAC)
#include <libproc.h>
#include <mach/mach_time.h>
#include <sys/resource.h>

#include "chrome/browser/molt_ai/common/seatbelt_profile.h"
#endif

namespace molt_ai {

ResidentHelperPolicy::ResidentHelperPolicy() = default;
ResidentHelperPolicy::ResidentHelperPolicy(const ResidentHelperPolicy&) =
    default;
ResidentHelperPolicy& ResidentHelperPolicy::operator=(
    const ResidentHelperPolicy&) = default;
ResidentHelperPolicy::~ResidentHelperPolicy() = default;

namespace {

// How long the destructor waits for a killed helper before leaving it to
// base's reaper.
constexpr base::TimeDelta kReapTimeout = base::Seconds(2);

#if BUILDFLAG(IS_LINUX)
class ResidentPreExecDelegate : public base::LaunchOptions::PreExecDelegate {
 public:
  explicit ResidentPreExecDelegate(helper_sandbox_linux::Prepared prepared)
      : prepared_(std::move(prepared)) {}

  // Between fork and exec: raw system calls only.
  void RunAsyncSafe() override {
    helper_sandbox_linux::ApplyInChild(prepared_);
  }

  helper_sandbox_linux::Prepared& prepared() { return prepared_; }

 private:
  helper_sandbox_linux::Prepared prepared_;
};

// Its user and system CPU time, from /proc/<pid>/stat; nullopt if it is
// gone or that can't be read.
std::optional<int64_t> CpuTimeMs(base::ProcessId pid) {
  const std::string path = "/proc/" + std::to_string(pid) + "/stat";
  base::ScopedFD fd(HANDLE_EINTR(open(path.c_str(), O_RDONLY | O_CLOEXEC)));
  if (!fd.is_valid()) {
    return std::nullopt;
  }
  std::array<char, 1024> buffer;
  const ssize_t bytes =
      HANDLE_EINTR(read(fd.get(), buffer.data(), buffer.size()));
  if (bytes <= 0) {
    return std::nullopt;
  }
  const std::optional<uint64_t> ticks = helper_limits::ParseProcStatCpuTicks(
      std::string_view(buffer.data(), static_cast<size_t>(bytes)));
  const auto per_second = sysconf(_SC_CLK_TCK);
  if (!ticks || per_second <= 0) {
    return std::nullopt;
  }
  return static_cast<int64_t>(*ticks * 1000 /
                              static_cast<uint64_t>(per_second));
}
#endif  // BUILDFLAG(IS_LINUX)

#if BUILDFLAG(IS_MAC)
// Its CPU time and memory footprint (proc_pid_rusage(), which needs no
// task port); nullopt if it is gone.
struct MacUsage {
  int64_t cpu_ms = 0;
  uint64_t footprint = 0;
};

std::optional<MacUsage> UsageOf(base::ProcessId pid) {
  rusage_info_v2 usage = {};
  if (proc_pid_rusage(pid, RUSAGE_INFO_V2,
                      reinterpret_cast<rusage_info_t*>(&usage)) != 0) {
    return std::nullopt;
  }
  // The CPU times are in Mach absolute time units (nanoseconds on Intel).
  mach_timebase_info_data_t timebase = {};
  if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0) {
    return std::nullopt;
  }
  const uint64_t units = usage.ri_user_time + usage.ri_system_time;
  MacUsage result;
  result.cpu_ms =
      static_cast<int64_t>(units / timebase.denom * timebase.numer / 1000000);
  result.footprint = usage.ri_phys_footprint;
  return result;
}

// Seatbelt matches resolved paths. Empty if |path| doesn't exist.
std::string CanonicalPath(const base::FilePath& path) {
  return base::MakeAbsoluteFilePath(path).value();
}
#endif  // BUILDFLAG(IS_MAC)

}  // namespace

class ResidentHelper::Platform {
 public:
  Platform(base::Process process,
           base::ScopedFD stdin_fd,
           base::ScopedFD stdout_fd)
      : process_(std::move(process)),
        pid_(process_.Pid()),
        stdin_fd_(std::move(stdin_fd)),
        stdout_fd_(std::move(stdout_fd)) {}

  ~Platform() {
    Kill();
    if (!WaitForExit(kReapTimeout)) {
      base::AutoLock lock(lock_);
      if (!reaped_) {
        reaped_ = true;
        base::EnsureProcessTerminated(std::move(process_));
      }
    }
  }

  bool Write(base::span<const uint8_t> data) {
    while (!data.empty()) {
      if (!stdin_fd_.is_valid()) {
        return false;
      }
      const ssize_t written =
          HANDLE_EINTR(write(stdin_fd_.get(), data.data(), data.size()));
      if (written <= 0) {
        return false;  // EPIPE: it is gone.
      }
      data = data.subspan(static_cast<size_t>(written));
    }
    return true;
  }

  void CloseStdin() { stdin_fd_.reset(); }

  int Read(base::span<uint8_t> buffer) {
    const ssize_t bytes =
        HANDLE_EINTR(read(stdout_fd_.get(), buffer.data(), buffer.size()));
    if (bytes < 0) {
      return -1;
    }
    return static_cast<int>(bytes);
  }

  void Kill() {
    base::AutoLock lock(lock_);
    if (reaped_) {
      return;  // its pid may be someone else's by now
    }
    // Its process group too: a Linux helper leads a session of its own,
    // a macOS one a process group.
    kill(-pid_, SIGKILL);
    kill(pid_, SIGKILL);
  }

  std::optional<int> WaitForExit(base::TimeDelta timeout) {
    const base::TimeTicks deadline = base::TimeTicks::Now() + timeout;
    base::TimeDelta pause = base::Milliseconds(1);
    for (;;) {
      {
        base::AutoLock lock(lock_);
        if (reaped_) {
          return exit_code_;
        }
        int exit_code = -1;
        // A zero timeout: base sync primitives aren't needed.
        if (process_.WaitForExitWithTimeout(base::TimeDelta(), &exit_code)) {
          reaped_ = true;
          exit_code_ = exit_code;
          return exit_code_;
        }
      }
      const base::TimeTicks now = base::TimeTicks::Now();
      if (now >= deadline) {
        return std::nullopt;
      }
      base::PlatformThread::Sleep(std::min(pause, deadline - now));
      pause = std::min(pause * 2, base::Milliseconds(50));
    }
  }

  // Its CPU time so far and, on macOS, its memory footprint.
  std::optional<int64_t> CpuMs(uint64_t* footprint) {
    *footprint = 0;
#if BUILDFLAG(IS_LINUX)
    return CpuTimeMs(pid_);
#elif BUILDFLAG(IS_MAC)
    const std::optional<MacUsage> usage = UsageOf(pid_);
    if (!usage) {
      return std::nullopt;
    }
    *footprint = usage->footprint;
    return usage->cpu_ms;
#else
    return std::nullopt;
#endif
  }

  base::ProcessId pid() const { return pid_; }

 private:
  base::Lock lock_;
  base::Process process_;
  const base::ProcessId pid_;
  bool reaped_ = false;
  int exit_code_ = -1;
  base::ScopedFD stdin_fd_;
  base::ScopedFD stdout_fd_;
};

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)
// static
std::unique_ptr<ResidentHelper> ResidentHelper::Start(
    const base::CommandLine& cmd,
    const ResidentHelperPolicy& policy,
    std::string* error) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  const base::FilePath program = cmd.GetProgram();
  if (!program.IsAbsolute()) {
    *error = "the helper needs an absolute path";
    return nullptr;
  }
  for (const base::FilePath& path : policy.read_only_paths) {
    if (!path.IsAbsolute()) {
      *error = "\"" + path.value() + "\" is not an absolute path";
      return nullptr;
    }
  }
  if (policy.max_memory_bytes == 0 || !(policy.max_cpu_cores > 0)) {
    *error = "a resident helper needs memory and CPU limits";
    return nullptr;
  }

  base::LaunchOptions options;
  std::vector<std::string> argv = cmd.argv();
  std::string confinement;
#if BUILDFLAG(IS_LINUX)
  helper_sandbox_linux::Policy linux_policy;
  linux_policy.read_execute.push_back(program.DirName().value());
  for (const base::FilePath& path : policy.read_only_paths) {
    linux_policy.read_only.push_back(path.value());
  }
  linux_policy.resident = true;
  linux_policy.max_data_bytes = policy.max_memory_bytes;
  helper_sandbox_linux::Prepared prepared;
  if (!helper_sandbox_linux::Prepare(linux_policy, &prepared, error)) {
    LOG(ERROR) << "[MoltHelperSandbox] resident: " << *error;
    return nullptr;
  }
  confinement = prepared.Describe();
  ResidentPreExecDelegate pre_exec(std::move(prepared));
  options.pre_exec_delegate = &pre_exec;
  if (pre_exec.prepared().ruleset_fd >= 0) {
    options.fds_to_remap.emplace_back(pre_exec.prepared().ruleset_fd,
                                      helper_sandbox_linux::kChildRulesetFd);
  }
  // If the browser (the thread that started it) dies, so does the helper.
  options.kill_on_parent_death = true;
#elif BUILDFLAG(IS_MAC)
  if (!base::PathExists(base::FilePath(seatbelt::kSandboxExecPath))) {
    *error = std::string(seatbelt::kSandboxExecPath) + " is missing";
    return nullptr;
  }
  const std::string helper = CanonicalPath(program);
  if (helper.empty()) {
    *error = "the helper is missing";
    return nullptr;
  }
  std::vector<std::string> read_only;
  for (const base::FilePath& path : policy.read_only_paths) {
    std::string canonical = CanonicalPath(path);
    if (!canonical.empty()) {
      read_only.push_back(std::move(canonical));
    }
  }
  const std::vector<std::string>& cmd_argv = cmd.argv();
  argv = seatbelt::BuildResidentSandboxExecArgv(
      helper, std::vector<std::string>(cmd_argv.begin() + 1, cmd_argv.end()),
      read_only);
  options.new_process_group = true;
  confinement = "sandbox-exec (Seatbelt)";
#endif

  // stdin and stdout are pipes to the browser; stderr goes nowhere.
  base::ScopedFD stdin_read;
  base::ScopedFD stdin_write;
  base::ScopedFD stdout_read;
  base::ScopedFD stdout_write;
  if (!base::CreatePipe(&stdin_read, &stdin_write) ||
      !base::CreatePipe(&stdout_read, &stdout_write)) {
    *error = "could not create its pipes";
    return nullptr;
  }
  base::ScopedFD null_fd(HANDLE_EINTR(open("/dev/null", O_WRONLY | O_CLOEXEC)));
  if (!null_fd.is_valid() || !base::SetCloseOnExec(stdin_write.get()) ||
      !base::SetCloseOnExec(stdout_read.get())) {
    *error = "could not create its pipes";
    return nullptr;
  }
#if BUILDFLAG(IS_MAC)
  // A write after it went away fails with EPIPE instead of a signal.
  fcntl(stdin_write.get(), F_SETNOSIGPIPE, 1);
#endif
  options.fds_to_remap.emplace_back(stdin_read.get(), STDIN_FILENO);
  options.fds_to_remap.emplace_back(stdout_write.get(), STDOUT_FILENO);
  options.fds_to_remap.emplace_back(null_fd.get(), STDERR_FILENO);
  options.current_directory = program.DirName();
  options.clear_environment = true;
  options.environment["PATH"] = "/usr/bin:/bin";

  base::Process process = base::LaunchProcess(argv, options);
  stdin_read.reset();
  stdout_write.reset();
  null_fd.reset();
#if BUILDFLAG(IS_LINUX)
  pre_exec.prepared().CloseRuleset();
#endif
  if (!process.IsValid()) {
    *error = "could not start the helper";
    return nullptr;
  }
  auto platform = std::make_unique<Platform>(
      std::move(process), std::move(stdin_write), std::move(stdout_read));
  return base::WrapUnique(
      new ResidentHelper(std::move(platform), policy, std::move(confinement)));
}
#else   // !(BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC))
// static
std::unique_ptr<ResidentHelper> ResidentHelper::Start(
    const base::CommandLine& cmd,
    const ResidentHelperPolicy& policy,
    std::string* error) {
  *error = "no helper sandbox on this platform";
  return nullptr;
}
#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)

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
  uint64_t footprint = 0;
  const std::optional<int64_t> cpu_ms = platform_->CpuMs(&footprint);
  if (!cpu_ms) {
    return true;  // gone: its exit is reported where it is read
  }
  if (footprint > max_memory_bytes_) {
    *why = "it used more than " + std::to_string(max_memory_bytes_ >> 20) +
           " MiB of memory";
    return false;
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
