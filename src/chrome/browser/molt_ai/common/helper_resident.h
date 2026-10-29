// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ResidentHelper: a sandboxed helper program that keeps running while a
// feature is on and talks to the browser over its stdin and stdout, as the
// speech helper does while voice mode listens (voice/voice_listener.h,
// docs/design/SPEECH_HELPER_PROTOCOL.md). helper_sandbox.h runs short-lived
// helpers with a deadline and a scratch directory; a resident one has
// neither:
//
//   - No deadline: it runs until the browser stops it (or it exits).
//   - Nothing writable at all: it reads its own directory, the policy's
//     read-only paths (its model directory) and the system's libraries,
//     and writes nothing but its stdout (and /dev/null). Never the
//     network. No child processes.
//   - Its memory and CPU are bounded instead of its time: at most
//     |max_memory_bytes| (Linux RLIMIT_DATA, Windows its job's limit, macOS
//     its footprint, checked) and, on average, |max_cpu_cores| over the last
//     |cpu_window| (checked from its CPU time by CheckLimits(); Windows also
//     caps its job's CPU rate at kResidentCpuHardCapCores).
//   - It dies with the browser: Linux PR_SET_PDEATHSIG (and its stdin's
//     end of file), Windows its job (killed when the browser's handle
//     closes), macOS its stdin's end of file (the protocol says exit then).
//   - Its stderr goes nowhere (/dev/null on POSIX, no handle on Windows):
//     nothing it writes there can block it, or reach the browser.
//
//   Linux: the same Landlock and seccomp-bpf layers as a short-lived helper
//     (helper_sandbox_linux.h), with no writable directory, RLIMIT_FSIZE 0,
//     and /proc/cpuinfo readable (ONNX Runtime's CPU detection).
//   macOS: sandbox-exec with the same profile but no scratch directory
//     (seatbelt_profile.h, BuildResidentProfile()).
//   Windows: an LPAC AppContainer of its own, in a job (one process, its
//     memory limit, a hard CPU cap, killed with the job), as
//     helper_sandbox_win_policy.h's PlanResidentLaunch() says; its package
//     SID grants it nothing, so it can write nowhere.
//   Other platforms: Start() fails.
//
// Threads: Start() blocks (call it on a thread that may block). On Linux,
// call it on a thread that outlives the helper: some kernels send the
// parent-death signal when the thread that started the helper ends, so a
// thread-pool worker that goes away could take the helper with it. Read() and
// Write() block; call each from one thread of its own. Kill() may be called
// from any thread and makes a blocked Read() return 0 and Write() false.
// CheckLimits() may run on any one thread. Destroy it after those threads
// stopped using it.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_HELPER_RESIDENT_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_HELPER_RESIDENT_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/process/process_handle.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/common/helper_limits.h"

namespace base {
class CommandLine;
}  // namespace base

namespace molt_ai {

struct ResidentHelperPolicy {
  ResidentHelperPolicy();
  ResidentHelperPolicy(const ResidentHelperPolicy&);
  ResidentHelperPolicy& operator=(const ResidentHelperPolicy&);
  ~ResidentHelperPolicy();

  // Absolute paths (files or directories) it may read beneath, besides its
  // own directory (always readable). On Windows a path outside the
  // helper's own folder must be in a directory CreateHelperDataDirectory()
  // made, or one the installer grants helpers.
  std::vector<base::FilePath> read_only_paths;
  uint64_t max_memory_bytes = helper_limits::kMaxResidentMemoryBytes;
  double max_cpu_cores = helper_limits::kMaxResidentCpuCores;
  base::TimeDelta cpu_window =
      base::Milliseconds(helper_limits::kResidentCpuWindowMs);
};

class ResidentHelper {
 public:
  // Starts |cmd| (an absolute program path) under |policy|. Null, with
  // |error| set, if it couldn't be started confined. Blocking.
  static std::unique_ptr<ResidentHelper> Start(
      const base::CommandLine& cmd,
      const ResidentHelperPolicy& policy,
      std::string* error);

  ResidentHelper(const ResidentHelper&) = delete;
  ResidentHelper& operator=(const ResidentHelper&) = delete;
  // Kills it (and, on POSIX, its process group) and reaps it. Blocking,
  // briefly.
  ~ResidentHelper();

  // Writes all of |data| to its stdin, blocking while the pipe is full.
  // False once it is gone, or after CloseStdin() or Kill().
  bool Write(base::span<const uint8_t> data);
  // Its stdin ends: it reads end of file.
  void CloseStdin();
  // Reads what it wrote to its stdout into |buffer|, blocking until there
  // is something. The number of bytes; 0 at end of file (it exited or was
  // killed); -1 on an error.
  int Read(base::span<uint8_t> buffer);

  // Checks its memory and CPU against the policy at |now|. False, with
  // |why| ("it used more than 0.75 of a CPU core for 30 s"), once it is
  // past a limit: kill it then. Blocking, briefly.
  bool CheckLimits(base::TimeTicks now, std::string* why);

  // Kills it now. Any thread.
  void Kill();

  // Waits up to |timeout| for it to exit: its exit code (-1 if a signal
  // ended it), or nullopt if it is still running. Blocking.
  std::optional<int> WaitForExit(base::TimeDelta timeout);

  base::ProcessId pid() const;

  // e.g. "Landlock ABI 6 (files, TCP, abstract sockets, signals),
  // seccomp-bpf (x86_64)": the confinement it got, for logs.
  const std::string& confinement() const { return confinement_; }

 private:
  class Platform;

  ResidentHelper(std::unique_ptr<Platform> platform,
                 const ResidentHelperPolicy& policy,
                 std::string confinement);

  std::unique_ptr<Platform> platform_;
  helper_limits::CpuUsageWindow cpu_;
  const uint64_t max_memory_bytes_;
  const std::string confinement_;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_HELPER_RESIDENT_H_
