// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// RunSandboxedHelper: the one way MoltBrowser runs a short-lived helper
// program on untrusted input (tesseract on any website's PDF or an attached
// image, whisper-cli on recorded audio). The helper gets OS-level
// confinement and a deadline, so a malicious file that exploits it gets
// neither the user's files nor the network, and can't hang a browser worker.
//
// Policy: the helper may read beneath |read_only_paths| (its own directory
// with its bundled libraries and data, the model or tessdata directory, the
// input file) and the system's libraries, read and write beneath
// |writable_dir| (a fresh scratch directory per call, which also holds the
// input and output and is the working directory), and nothing else. Never
// the network. Its environment is reduced to HOME and TMPDIR (both the
// scratch directory) and PATH.
//
//   Linux: no_new_privs, a new session, Landlock and seccomp-bpf, applied
//     between fork and exec (helper_sandbox_linux.h). A layer the kernel
//     lacks is skipped and logged once (e.g. Landlock before Linux 5.13:
//     then files are not confined); one the kernel has but that can't be
//     set up fails the run.
//   macOS: /usr/bin/sandbox-exec with a deny-by-default Seatbelt profile
//     (seatbelt_profile.h). Fail-closed: without sandbox-exec, or if it
//     can't apply the profile, the helper does not run.
//   Windows: a job object (killed with its job, UI restrictions) and the
//     deadline only. No file system or network confinement yet; that needs
//     an AppContainer and ACLs on the bundle and scratch directories.
//   Other platforms: fail-closed.
//
// The deadline kills the helper and its process group. Output (stdout and
// stderr together) is read up to |max_output_bytes|; past that, reading
// stops and the pipe is closed (POSIX; on Windows the rest is discarded).
//
// Not for tor: it is long-running and needs the network.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_HELPER_SANDBOX_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_HELPER_SANDBOX_H_

#include <stddef.h>

#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/time/time.h"

namespace base {
class CommandLine;
}  // namespace base

namespace molt_ai {

struct HelperSandboxPolicy {
  HelperSandboxPolicy();
  HelperSandboxPolicy(const HelperSandboxPolicy&);
  HelperSandboxPolicy& operator=(const HelperSandboxPolicy&);
  ~HelperSandboxPolicy();

  // Absolute paths (files or directories) the helper may read beneath. The
  // helper's own directory is always readable (and its code executable);
  // list it anyway when its libraries or data live there.
  std::vector<base::FilePath> read_only_paths;
  // The one directory the helper may write: an existing, private, per-call
  // scratch directory. Also its working directory, HOME and TMPDIR.
  base::FilePath writable_dir;
  // Network: none, always.
};

struct HelperRunResult {
  HelperRunResult();
  HelperRunResult(const HelperRunResult&);
  HelperRunResult& operator=(const HelperRunResult&);
  ~HelperRunResult();

  // The helper started under its sandbox.
  bool launched = false;
  // The confinement could not be set up; the helper did not run.
  bool sandbox_failed = false;
  // Killed at the deadline.
  bool timed_out = false;
  // The exit code when the helper exited by itself; -1 if a signal ended it.
  int exit_code = -1;
  // stdout and stderr, at most |max_output_bytes|. Untrusted bytes.
  std::string output;
  bool output_truncated = false;
  // Why it did not launch, the sandbox failed, or it timed out
  // ("timed out after 120 s").
  std::string error;
};

// Runs |cmd| (absolute program path) under |policy|, killing it after
// |timeout|. Blocking: call from a ThreadPool task with base::MayBlock().
// It needs no base sync primitives (it polls for the exit).
HelperRunResult RunSandboxedHelper(const base::CommandLine& cmd,
                                   const HelperSandboxPolicy& policy,
                                   base::TimeDelta timeout,
                                   size_t max_output_bytes);

// Empty if |run| succeeded (launched, exited by itself with 0). Otherwise:
//   sandbox failure:  "<name> sandbox unavailable: <why>"
//   launch failure:   |launch_error|
//   deadline:         "<name> timed out after N s"
//   non-zero exit:    "<name> exit=N: <its output, sanitized, 200 bytes>"
std::string DescribeHelperRunFailure(const HelperRunResult& run,
                                     std::string_view name,
                                     std::string_view launch_error);

// Reads a file the helper wrote into its scratch directory, at most
// |max_bytes|. On POSIX it must be a regular file with a single link and
// not a symlink, so a compromised helper can't make the browser read a
// file outside its sandbox (a symlink or hard link to ~/.ssh/...) and pass
// it on as "OCR text". False if it is missing or refused. Blocking.
bool ReadHelperOutputFile(const base::FilePath& path,
                          size_t max_bytes,
                          std::string* contents);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_HELPER_SANDBOX_H_
