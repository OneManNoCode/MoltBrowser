// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Linux confinement for short-lived helper programs (tesseract,
// whisper-cli): Landlock and a seccomp-bpf filter, applied in the child
// between fork() and exec() (helper_sandbox.h is the front end callers use).
//
// Pure POSIX/Linux code without //base, so the exact code that ships is also
// exercised by a standalone harness against the real kernel, besides the
// unit tests. Everything is prepared in the parent; ApplyInChild() only makes
// raw system calls on what was prepared, because after fork() in a
// multi-threaded process the child may not allocate or take locks.
//
// Layers, each used when the kernel has it:
//   - no_new_privs (always; setuid binaries lose their privileges), and a
//     new session, so the helper has no controlling terminal (no TIOCSTI).
//   - Landlock: read (and execute) beneath the policy's read-only paths and
//     the system locations a dynamically linked helper needs, read and write
//     beneath one scratch directory, nothing else. ABI >= 4: no TCP bind or
//     connect. ABI >= 6: no connecting to abstract UNIX sockets or signalling
//     processes outside the sandbox.
//   - seccomp-bpf (x86_64 and aarch64): an allow-by-default filter with a
//     short deny list (BuildSeccompProgram()). socket() for anything but
//     AF_UNIX and connect() fail with EACCES, so libraries that probe for
//     the network fail gracefully instead of being killed.
// A layer the kernel lacks (Landlock before 5.13, or disabled at boot) is
// skipped and the front end logs it once. A layer the kernel offers but that
// can't be set up fails the run: it never silently runs with less.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_HELPER_SANDBOX_LINUX_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_HELPER_SANDBOX_LINUX_H_

#include <linux/filter.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace helper_sandbox_linux {

// Landlock access rights (<linux/landlock.h>; Chromium's sysroot predates
// most of them).
inline constexpr uint64_t kFsExecute = 1ULL << 0;
inline constexpr uint64_t kFsWriteFile = 1ULL << 1;
inline constexpr uint64_t kFsReadFile = 1ULL << 2;
inline constexpr uint64_t kFsReadDir = 1ULL << 3;
inline constexpr uint64_t kFsRemoveDir = 1ULL << 4;
inline constexpr uint64_t kFsRemoveFile = 1ULL << 5;
inline constexpr uint64_t kFsMakeChar = 1ULL << 6;
inline constexpr uint64_t kFsMakeDir = 1ULL << 7;
inline constexpr uint64_t kFsMakeReg = 1ULL << 8;
inline constexpr uint64_t kFsMakeSock = 1ULL << 9;
inline constexpr uint64_t kFsMakeFifo = 1ULL << 10;
inline constexpr uint64_t kFsMakeBlock = 1ULL << 11;
inline constexpr uint64_t kFsMakeSym = 1ULL << 12;
inline constexpr uint64_t kFsRefer = 1ULL << 13;                 // ABI 2
inline constexpr uint64_t kFsTruncate = 1ULL << 14;              // ABI 3
inline constexpr uint64_t kFsIoctlDev = 1ULL << 15;              // ABI 5
inline constexpr uint64_t kNetBindTcp = 1ULL << 0;               // ABI 4
inline constexpr uint64_t kNetConnectTcp = 1ULL << 1;            // ABI 4
inline constexpr uint64_t kScopeAbstractUnixSocket = 1ULL << 0;  // ABI 6
inline constexpr uint64_t kScopeSignal = 1ULL << 1;              // ABI 6
// landlock_restrict_self(): log denials after exec too (ABI 7; only when the
// audit subsystem is on), so a helper hitting the policy shows up there.
inline constexpr uint32_t kRestrictSelfLogNewExecOn = 1U << 1;

// What the rules grant.
inline constexpr uint64_t kReadAccess = kFsReadFile | kFsReadDir;
inline constexpr uint64_t kReadExecuteAccess = kReadAccess | kFsExecute;
// The scratch directory: read, write, create and delete files and
// directories, rename within it. Not: execute what was written there, or
// create symlinks, sockets, FIFOs or device nodes.
inline constexpr uint64_t kScratchAccess =
    kFsReadFile | kFsReadDir | kFsWriteFile | kFsRemoveDir | kFsRemoveFile |
    kFsMakeDir | kFsMakeReg | kFsRefer | kFsTruncate;

// What one helper run may touch. Absolute paths.
struct Policy {
  Policy();
  Policy(const Policy&);
  Policy& operator=(const Policy&);
  ~Policy();

  // Read beneath these (files or directories).
  std::vector<std::string> read_only;
  // Read and execute beneath these: the helper's own directory.
  std::vector<std::string> read_execute;
  // Read and write beneath this directory (it must exist).
  std::string writable;
};

struct PathRule {
  std::string path;
  uint64_t access = 0;
  // A missing required path fails the run; others are skipped.
  bool required = false;
};

// The Landlock rules for |policy|: the system locations every dynamically
// linked helper needs (/usr, /lib, /lib64 read+execute; /etc and
// /sys/devices/system/cpu read; /dev/null read+write; /dev/zero,
// /dev/random, /dev/urandom read), then the policy's own paths. /proc/self
// (read) is added in the child, where it names the child: the helper
// process itself, not processes it starts. Access bits are masked to what
// the kernel handles when the ruleset is built.
std::vector<PathRule> BuildPathRules(const Policy& policy);

// The Landlock ABI version of the running kernel, or 0 without Landlock.
int LandlockAbi();

// The filesystem rights Landlock ABI |abi| knows (0 for abi < 1).
uint64_t HandledFsAccess(int abi);

// The seccomp-bpf program for this build's CPU architecture (x86_64,
// aarch64), empty on any other. It checks the architecture first (a foreign
// syscall ABI, e.g. int 0x80 or x32, kills the process), then denies:
//   socket() / socketpair() other than AF_UNIX, and connect(): EACCES
//     (no network; no talking to the session bus, X11, ssh-agent or other
//     named services through a UNIX socket);
//   io_uring_*: EPERM (it can open sockets without socket());
//   setsid, setpgid: EPERM (what the helper starts stays in its process
//     group, which the launcher kills);
//   ptrace, process_vm_readv/writev, pidfd_getfd: EPERM (other processes);
//   bpf, perf_event_open, userfaultfd: EPERM (kernel attack surface);
//   keyctl, add_key, request_key: EPERM (the user's kernel keyrings);
//   unshare, setns, clone() with namespace flags, mount and the new mount
//     API, umount2, pivot_root: EPERM; clone3: ENOSYS (its flags can't be
//     inspected; libc falls back to clone()).
// Everything else is allowed.
std::vector<sock_filter> BuildSeccompProgram();

// "x86_64", "aarch64", or empty if BuildSeccompProgram() has no filter for
// this architecture.
std::string_view SeccompArchName();

// The ruleset's file descriptor number in the child. The launcher maps
// Prepared::ruleset_fd there (base::LaunchOptions::fds_to_remap); the child
// closes it before exec.
inline constexpr int kChildRulesetFd = 3;

// The largest file a helper may write (RLIMIT_FSIZE; past it the write
// fails and SIGXFSZ ends the helper), and it writes no core dump
// (RLIMIT_CORE 0): a helper a hostile input took over can't fill the disk
// before its deadline. tesseract and whisper-cli write a text file of at
// most a few hundred KiB.
inline constexpr uint64_t kMaxHelperFileBytes = 64ULL << 20;

// Exit status and stderr prefix of a child whose sandbox setup failed (the
// helper never ran).
inline constexpr int kSetupFailedExitCode = 125;
inline constexpr std::string_view kSetupFailedMarker = "molt-helper-sandbox: ";

// Everything ApplyInChild() needs, prepared in the parent. Move-only; owns
// |ruleset_fd|.
struct Prepared {
  Prepared();
  Prepared(Prepared&& other);
  Prepared& operator=(Prepared&& other);
  ~Prepared();

  // Closes |ruleset_fd| in this process (after the child was launched).
  void CloseRuleset();
  // e.g. "Landlock ABI 7 (files, TCP, scopes) + seccomp-bpf (x86_64)".
  std::string Describe() const;

  // 0 when the kernel has no Landlock.
  int landlock_abi = 0;
  // The Landlock ruleset (parent's descriptor), or -1.
  int ruleset_fd = -1;
  uint64_t handled_fs = 0;
  uint32_t restrict_flags = 0;
  // Empty when this architecture or kernel has no seccomp filter support.
  std::vector<sock_filter> seccomp;
};

// Parent side: builds the Landlock ruleset (opening every path) and the
// seccomp program. Returns false with |error| set when a layer the kernel
// offers can't be set up, or the policy is unusable (relative path, missing
// scratch directory): the caller must not run the helper. Blocking.
bool Prepare(const Policy& policy, Prepared* prepared, std::string* error);

// Child side, after fork() and before exec(), with the ruleset at
// kChildRulesetFd: new session, the file size and core dump limits
// (kMaxHelperFileBytes), no_new_privs, Landlock (adding /proc/self),
// seccomp. Async-signal-safe: raw system calls only, no allocation. If a
// step fails it writes kSetupFailedMarker + the step to stderr and calls
// _exit(kSetupFailedExitCode).
void ApplyInChild(const Prepared& prepared);

}  // namespace helper_sandbox_linux
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_HELPER_SANDBOX_LINUX_H_
