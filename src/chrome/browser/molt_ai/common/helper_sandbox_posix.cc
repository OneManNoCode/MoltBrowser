// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// RunSandboxedHelper() on Linux (Landlock + seccomp-bpf in a pre-exec
// delegate) and macOS (sandbox-exec), plus the deadline and output limit
// both share. Other POSIX platforms fail closed.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/posix/eintr_wrapper.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/threading/platform_thread.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/common/helper_sandbox.h"

#if BUILDFLAG(IS_LINUX)
#include <atomic>

#include "chrome/browser/molt_ai/common/helper_sandbox_linux.h"
#endif

#if BUILDFLAG(IS_MAC)
#include "chrome/browser/molt_ai/common/seatbelt_profile.h"
#endif

namespace molt_ai {

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)
namespace {

// Upper bound for |max_output_bytes| (and so for what one run buffers).
constexpr size_t kMaxOutputLimit = 64u << 20;

// How long to wait for a killed helper before leaving it to base's reaper.
constexpr base::TimeDelta kReapTimeout = base::Seconds(2);

// The helper's whole environment: nothing of the browser's (which may hold
// tokens or keys) reaches it.
base::EnvironmentMap HelperEnvironment(const base::FilePath& scratch) {
  base::EnvironmentMap environment;
  environment["HOME"] = scratch.value();
  environment["TMPDIR"] = scratch.value();
  environment["PATH"] = "/usr/bin:/bin";
  return environment;
}

std::string DescribeTimeout(base::TimeDelta timeout) {
  if (timeout >= base::Seconds(1)) {
    return "timed out after " + base::NumberToString(timeout.InSeconds()) +
           " s";
  }
  return "timed out after " + base::NumberToString(timeout.InMilliseconds()) +
         " ms";
}

enum class ReadEnd { kEof, kLimit, kDeadline, kError };

// Reads the helper's output from |fd| into |output| until end of file, the
// deadline, or more than |max_bytes| (then |output| keeps |max_bytes| and
// |*truncated| is set).
ReadEnd ReadOutput(int fd,
                   base::TimeTicks deadline,
                   size_t max_bytes,
                   std::string* output,
                   bool* truncated) {
  std::array<char, 16384> buffer;
  for (;;) {
    const base::TimeDelta remaining = deadline - base::TimeTicks::Now();
    if (!remaining.is_positive()) {
      return ReadEnd::kDeadline;
    }
    struct pollfd poll_fd = {};
    poll_fd.fd = fd;
    poll_fd.events = POLLIN;
    const int ready =
        poll(&poll_fd, 1,
             base::saturated_cast<int>(remaining.InMillisecondsRoundedUp()));
    if (ready < 0) {
      if (errno == EINTR) {
        continue;  // The loop recomputes the time left.
      }
      return ReadEnd::kError;
    }
    if (ready == 0) {
      continue;
    }
    // One byte past the limit tells "exactly the limit" from "more".
    const size_t wanted =
        std::min(buffer.size(), max_bytes + 1 - output->size());
    const ssize_t bytes = HANDLE_EINTR(read(fd, buffer.data(), wanted));
    if (bytes == 0) {
      return ReadEnd::kEof;
    }
    if (bytes < 0) {
      return ReadEnd::kError;
    }
    output->append(buffer.data(), static_cast<size_t>(bytes));
    if (output->size() > max_bytes) {
      output->resize(max_bytes);
      *truncated = true;
      return ReadEnd::kLimit;
    }
  }
}

// Waits for |process| to exit until |deadline|. Polls with a zero timeout:
// Process::WaitForExitWithTimeout() with a real timeout needs base sync
// primitives, which a plain MayBlock task doesn't have.
bool WaitForExitUntil(const base::Process& process,
                      base::TimeTicks deadline,
                      int* exit_code) {
  base::TimeDelta pause = base::Milliseconds(1);
  for (;;) {
    if (process.WaitForExitWithTimeout(base::TimeDelta(), exit_code)) {
      return true;
    }
    const base::TimeDelta remaining = deadline - base::TimeTicks::Now();
    if (!remaining.is_positive()) {
      return false;
    }
    base::PlatformThread::Sleep(std::min(pause, remaining));
    pause = std::min(pause * 2, base::Milliseconds(50));
  }
}

// SIGKILL for the helper's process group (its own session on Linux, its
// own group on macOS: whatever it started goes too) and the helper itself,
// in case it is killed before it had its group. Then reaps it.
void KillHelper(base::Process process) {
  const pid_t pid = process.Pid();
  kill(-pid, SIGKILL);
  kill(pid, SIGKILL);
  int ignored = 0;
  if (!WaitForExitUntil(process, base::TimeTicks::Now() + kReapTimeout,
                        &ignored)) {
    base::EnsureProcessTerminated(std::move(process));
  }
}

#if BUILDFLAG(IS_LINUX)
class SandboxPreExecDelegate : public base::LaunchOptions::PreExecDelegate {
 public:
  explicit SandboxPreExecDelegate(helper_sandbox_linux::Prepared prepared)
      : prepared_(std::move(prepared)) {}

  // Between fork and exec: raw system calls only.
  void RunAsyncSafe() override {
    helper_sandbox_linux::ApplyInChild(prepared_);
  }

  helper_sandbox_linux::Prepared& prepared() { return prepared_; }

 private:
  helper_sandbox_linux::Prepared prepared_;
};

// Which layers this kernel gives the helpers, logged once per run of the
// browser.
void LogLayersOnce(const helper_sandbox_linux::Prepared& prepared) {
  static std::atomic<bool> logged{false};
  if (logged.exchange(true)) {
    return;
  }
  if (prepared.landlock_abi > 0 && !prepared.seccomp.empty()) {
    LOG(INFO) << "[MoltHelperSandbox] " << prepared.Describe();
  } else {
    LOG(WARNING) << "[MoltHelperSandbox] reduced confinement for OCR and "
                    "voice helpers: "
                 << prepared.Describe();
  }
}

bool PrepareLinuxSandbox(const base::CommandLine& cmd,
                         const HelperSandboxPolicy& policy,
                         helper_sandbox_linux::Prepared* prepared,
                         std::string* error) {
  helper_sandbox_linux::Policy linux_policy;
  linux_policy.read_execute.push_back(cmd.GetProgram().DirName().value());
  for (const base::FilePath& path : policy.read_only_paths) {
    linux_policy.read_only.push_back(path.value());
  }
  linux_policy.writable = policy.writable_dir.value();
  if (!helper_sandbox_linux::Prepare(linux_policy, prepared, error)) {
    return false;
  }
  LogLayersOnce(*prepared);
  return true;
}
#endif  // BUILDFLAG(IS_LINUX)

#if BUILDFLAG(IS_MAC)
// Seatbelt matches resolved paths (/private/var/..., not /var/...). Empty
// if |path| doesn't exist.
std::string CanonicalPath(const base::FilePath& path) {
  return base::MakeAbsoluteFilePath(path).value();
}

bool WrapInSandboxExec(const base::CommandLine& cmd,
                       const HelperSandboxPolicy& policy,
                       std::vector<std::string>* argv,
                       std::string* error) {
  if (!base::PathExists(base::FilePath(seatbelt::kSandboxExecPath))) {
    *error = std::string(seatbelt::kSandboxExecPath) + " is missing";
    return false;
  }
  const std::string helper = CanonicalPath(cmd.GetProgram());
  const std::string scratch = CanonicalPath(policy.writable_dir);
  if (helper.empty() || scratch.empty()) {
    *error = "the helper or its scratch directory is missing";
    return false;
  }
  std::vector<std::string> read_only;
  for (const base::FilePath& path : policy.read_only_paths) {
    std::string canonical = CanonicalPath(path);
    if (!canonical.empty()) {
      read_only.push_back(std::move(canonical));
    }
  }
  const std::vector<std::string>& cmd_argv = cmd.argv();
  std::vector<std::string> helper_args;
  for (size_t i = 1; i < cmd_argv.size(); ++i) {
    helper_args.push_back(cmd_argv[i]);
  }
  *argv =
      seatbelt::BuildSandboxExecArgv(helper, helper_args, read_only, scratch);
  return true;
}
#endif  // BUILDFLAG(IS_MAC)

// Whether the run ended in the sandbox setup rather than in the helper.
bool SandboxSetupFailed(const HelperRunResult& run) {
#if BUILDFLAG(IS_LINUX)
  return run.exit_code == helper_sandbox_linux::kSetupFailedExitCode &&
         base::StartsWith(run.output, helper_sandbox_linux::kSetupFailedMarker);
#else
  return seatbelt::IsSandboxExecFailure(run.exit_code, run.output);
#endif
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
  if (!cmd.GetProgram().IsAbsolute() || !policy.writable_dir.IsAbsolute()) {
    result.sandbox_failed = true;
    result.error = "the helper and its scratch directory need absolute paths";
    return result;
  }

  base::LaunchOptions options;
  std::vector<std::string> argv = cmd.argv();
#if BUILDFLAG(IS_LINUX)
  helper_sandbox_linux::Prepared prepared;
  if (!PrepareLinuxSandbox(cmd, policy, &prepared, &result.error)) {
    result.sandbox_failed = true;
    LOG(ERROR) << "[MoltHelperSandbox] " << result.error;
    return result;
  }
  SandboxPreExecDelegate pre_exec(std::move(prepared));
  options.pre_exec_delegate = &pre_exec;
  if (pre_exec.prepared().ruleset_fd >= 0) {
    options.fds_to_remap.emplace_back(pre_exec.prepared().ruleset_fd,
                                      helper_sandbox_linux::kChildRulesetFd);
  }
  // If the browser dies, so does the helper.
  options.kill_on_parent_death = true;
#else
  if (!WrapInSandboxExec(cmd, policy, &argv, &result.error)) {
    result.sandbox_failed = true;
    LOG(ERROR) << "[MoltHelperSandbox] " << result.error;
    return result;
  }
  options.new_process_group = true;
#endif

  // stdout and stderr both go to one pipe; stdin is /dev/null.
  base::ScopedFD read_end;
  base::ScopedFD write_end;
  if (!base::CreatePipe(&read_end, &write_end) ||
      !base::SetCloseOnExec(read_end.get()) ||
      !base::SetCloseOnExec(write_end.get())) {
    result.error = "could not create a pipe";
    return result;
  }
  options.fds_to_remap.emplace_back(write_end.get(), STDOUT_FILENO);
  options.fds_to_remap.emplace_back(write_end.get(), STDERR_FILENO);
  options.current_directory = policy.writable_dir;
  options.clear_environment = true;
  options.environment = HelperEnvironment(policy.writable_dir);

  base::Process process = base::LaunchProcess(argv, options);
  write_end.reset();
#if BUILDFLAG(IS_LINUX)
  pre_exec.prepared().CloseRuleset();
#endif
  if (!process.IsValid()) {
    result.error = "could not start the helper";
    return result;
  }
  result.launched = true;

  const ReadEnd end = ReadOutput(read_end.get(), deadline, max_output_bytes,
                                 &result.output, &result.output_truncated);
  if (end != ReadEnd::kEof) {
    // Stop reading: the helper's next write gets EPIPE or SIGPIPE.
    read_end.reset();
  }
  bool exited = false;
  if (end == ReadEnd::kEof || end == ReadEnd::kLimit) {
    exited = WaitForExitUntil(process, deadline, &result.exit_code);
  }
  if (!exited) {
    KillHelper(std::move(process));
    result.exit_code = -1;
    if (end == ReadEnd::kError) {
      result.error = "lost the helper's output";
    } else {
      result.timed_out = true;
      result.error = DescribeTimeout(timeout);
    }
    return result;
  }

  // Anything the helper left running in its process group goes too (on
  // Linux seccomp keeps its children from leaving the group). While any
  // member is alive the group keeps its id; an empty group's id could only
  // be handed out again after the pid counter wraps around.
  kill(-process.Pid(), SIGKILL);

  if (SandboxSetupFailed(result)) {
    result.launched = false;
    result.sandbox_failed = true;
    result.error = result.output.substr(0, result.output.find('\n'));
    LOG(ERROR) << "[MoltHelperSandbox] " << result.error;
  }
  return result;
}

#else  // !(BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC))

HelperRunResult RunSandboxedHelper(const base::CommandLine& cmd,
                                   const HelperSandboxPolicy& policy,
                                   base::TimeDelta timeout,
                                   size_t max_output_bytes) {
  HelperRunResult result;
  result.sandbox_failed = true;
  result.error = "no helper sandbox on this platform";
  return result;
}

#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)

bool ReadHelperOutputFile(const base::FilePath& path,
                          size_t max_bytes,
                          std::string* contents) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  contents->clear();
  // O_NOFOLLOW: not a symlink. O_NONBLOCK: a FIFO can't block the open.
  base::ScopedFD fd(HANDLE_EINTR(open(
      path.value().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)));
  if (!fd.is_valid()) {
    return false;
  }
  // A regular file the helper created: one link (not a hard link to a file
  // outside the scratch directory).
  struct stat info = {};
  if (fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_nlink != 1) {
    return false;
  }
  std::array<char, 16384> buffer;
  while (contents->size() < max_bytes) {
    const size_t wanted = std::min(buffer.size(), max_bytes - contents->size());
    const ssize_t bytes = HANDLE_EINTR(read(fd.get(), buffer.data(), wanted));
    if (bytes < 0) {
      return false;
    }
    if (bytes == 0) {
      break;
    }
    contents->append(buffer.data(), static_cast<size_t>(bytes));
  }
  return true;
}

}  // namespace molt_ai
