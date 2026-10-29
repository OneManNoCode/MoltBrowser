// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// RunSandboxedHelper: the one way MoltBrowser runs a short-lived helper
// program on untrusted input (tesseract on any website's PDF or an attached
// image, whisper-cli on recorded audio). The helper gets OS-level
// confinement and a deadline, so a malicious file that exploits it gets
// neither the user's files nor the network, and can't hang a browser worker.
// It also gets simple input: tesseract reads an uncompressed image the
// browser wrote from what Chromium's sandboxed image decoder decoded
// (ocr/ocr_image.h), whisper-cli a WAV the browser wrote from checked
// samples (voice/recorded_wav.h).
//
// Policy: the helper may read beneath |read_only_paths| (its own directory
// with its bundled libraries and data, the model or tessdata directory, the
// input file) and the system's libraries, create and write files in
// |writable_dir| (a fresh scratch directory per call, HelperScratchDir,
// which also holds the input and output and is the working directory), and
// nothing else: not delete, rename or link files there, nor make
// directories. Never the network. Its environment is reduced to a few
// variables naming the scratch directory and the system's (POSIX: HOME,
// TMPDIR, PATH; Windows: TEMP, TMP, SystemRoot, SystemDrive, windir, Path).
//
// Limits, the same on every OS (helper_limits.h): the scratch directory may
// grow by less than 64 MiB beyond what the browser put there, and gain at
// most 64 entries. The launcher measures it every 10 ms while the helper
// runs, and once more after it exits; past a limit it kills the helper and
// the run fails, saying which limit (HelperRunResult::limit_exceeded). As
// the helper can't delete what it wrote, nothing it writes leaves the
// directory unmeasured. Memory: at most 4 GiB.
//
//   Linux: no_new_privs, a new session, Landlock and seccomp-bpf, applied
//     between fork and exec (helper_sandbox_linux.h), and resource limits:
//     no file larger than 64 MiB, no core dump, at most 4 GiB of private
//     memory (RLIMIT_DATA; seccomp refuses the shared memory it doesn't
//     count) and 256 open files. A layer the kernel lacks is skipped and
//     logged once (e.g. Landlock before Linux 5.13: then files are not
//     confined); one the kernel has but that can't be set up fails the run.
//   macOS: /usr/bin/sandbox-exec with a deny-by-default Seatbelt profile
//     (seatbelt_profile.h); the helper starts no other process.
//     base::LaunchProcess() has no hook before exec here, so no resource
//     limits: the launcher checks the helper's memory footprint along with
//     its scratch directory. Fail-closed: without sandbox-exec, or if it
//     can't apply the profile, the helper does not run.
//   Windows: a less-privileged AppContainer (LPAC) of its own per run, with
//     no network capability, no child processes, a job (one process, a
//     memory limit, killed with the job, UI restrictions) it is in before
//     its first instruction, and process mitigations
//     (helper_sandbox_win_policy.h has the whole policy). It reads its own
//     folder through a capability the installer grants there
//     (lpacMoltBrowserHelperFiles), its scratch directory through that
//     directory's security descriptor, and Windows' system files.
//     Fail-closed: if the AppContainer can't run the program or reach a
//     path it needs (the portable ZIP has no installer to grant the
//     capability), or the attributes, job or scratch directory can't be
//     set up, the helper does not run and the error says why.
//   Other platforms: fail-closed.
//
// The deadline, or a limit, kills the helper and its process group
// (Windows: its job). Output (stdout and stderr together) is read up to
// |max_output_bytes|; past that, reading stops and the pipe is closed.
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
  // list it anyway when its libraries or data live there. On Windows a
  // path outside the helper's own folder and its scratch directory must be
  // in a directory made by CreateHelperDataDirectory().
  std::vector<base::FilePath> read_only_paths;
  // The one directory the helper may write: an existing, private, per-call
  // scratch directory (HelperScratchDir). Also its working directory and
  // temp directory (POSIX: HOME and TMPDIR too).
  base::FilePath writable_dir;
  // Windows: the AppContainer SID |writable_dir| was made for
  // (HelperScratchDir::MakePolicy() sets both); the helper runs as it.
  // Unused elsewhere.
  std::string app_container_sid;
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
  // Past a limit (helper_limits.h): killed when the launcher saw it, or
  // when it exited. Its output and exit code don't count then.
  bool limit_exceeded = false;
  // The exit code when the helper exited by itself; -1 if a signal ended it.
  int exit_code = -1;
  // stdout and stderr, at most |max_output_bytes|. Untrusted bytes.
  std::string output;
  bool output_truncated = false;
  // Why it did not launch, the sandbox failed, it timed out ("timed out
  // after 120 s") or which limit it passed ("it wrote 64 MiB or more to its
  // scratch directory").
  std::string error;
};

// A fresh, private scratch directory for one helper run, in the temp
// directory; deleted with what is in it when this goes. POSIX: mode 0700
// (set again before it is deleted, as a helper on Linux can change it).
// Windows: created with a security descriptor (never changed afterwards)
// that gives the user full control and an AppContainer of its own the
// rights a helper needs there, with a low integrity label so that
// AppContainer can write (helper_sandbox_win_policy.h,
// ScratchDirectorySddl()). Blocking: MayBlock thread-pool tasks only.
class HelperScratchDir {
 public:
  HelperScratchDir();
  HelperScratchDir(const HelperScratchDir&) = delete;
  HelperScratchDir& operator=(const HelperScratchDir&) = delete;
  ~HelperScratchDir();

  // Creates it, named after |prefix| (1 to 32 ASCII letters, digits or
  // underscores). False if it couldn't be, or already was.
  bool Create(std::string_view prefix);

  bool IsValid() const { return !path_.empty(); }
  const base::FilePath& path() const { return path_; }

  // A policy that writes here: |writable_dir| and, on Windows,
  // |app_container_sid|. The caller adds the read-only paths.
  HelperSandboxPolicy MakePolicy() const;

 private:
  base::FilePath path_;
  std::string app_container_sid_;
};

// Creates the directory |path| (its parent must exist) for data a helper
// reads from outside its own folder, e.g. a speech model downloaded later.
// Windows: its security descriptor, set when it is created, lets helpers
// (their capability) read what is put in it, as the installer does for
// their folders (helper_sandbox_win_policy.h, DataDirectorySddl()).
// Elsewhere a plain directory: a helper reads what its policy lists. False
// if it couldn't be created or already exists. Blocking.
bool CreateHelperDataDirectory(const base::FilePath& path);

// Runs |cmd| (absolute program path) under |policy|, killing it after
// |timeout|. Blocking: call from a ThreadPool task with base::MayBlock().
// It needs no base sync primitives (it polls for the exit).
HelperRunResult RunSandboxedHelper(const base::CommandLine& cmd,
                                   const HelperSandboxPolicy& policy,
                                   base::TimeDelta timeout,
                                   size_t max_output_bytes);

// Empty if |run| succeeded (launched, exited by itself with 0, within the
// limits). Otherwise:
//   sandbox failure:  "<name> sandbox unavailable: <why>"
//   launch failure:   |launch_error|
//   past a limit:     "<name> was stopped: <which limit>"
//   deadline:         "<name> timed out after N s"
//   non-zero exit:    "<name> exit=N: <its output, sanitized, 200 bytes>"
std::string DescribeHelperRunFailure(const HelperRunResult& run,
                                     std::string_view name,
                                     std::string_view launch_error);

// Reads a file the helper wrote into its scratch directory, at most
// |max_bytes|. It must be a regular file with a single link and not a
// symlink (Windows: not a reparse point), so a compromised helper can't make
// the browser read a file outside its sandbox (a link to ~/.ssh/...) and
// pass it on as "OCR text". False if it is missing or refused. Blocking.
bool ReadHelperOutputFile(const base::FilePath& path,
                          size_t max_bytes,
                          std::string* contents);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_HELPER_SANDBOX_H_
