// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Linux confinement for helper programs: short-lived ones (tesseract,
// whisper-cli; helper_sandbox.h is their front end) and resident ones (the
// speech helper; helper_resident.h): Landlock and a seccomp-bpf filter,
// applied in the child between fork() and exec().
//
// Pure POSIX/Linux code without //base, so the exact code that ships is also
// exercised by a standalone harness against the real kernel, besides the
// unit tests. Everything is prepared in the parent; ApplyInChild() only makes
// raw system calls on what was prepared, because after fork() in a
// multi-threaded process the child may not allocate or take locks.
//
// Layers, each used when the kernel has it:
//   - no_new_privs (always; setuid binaries lose their privileges), a new
//     session, so the helper has no controlling terminal (no TIOCSTI), and
//     resource limits (always; helper_limits.h): no file past
//     kMaxHelperFileBytes, no core dump, at most kMaxHelperDataBytes of
//     private memory and kMaxHelperOpenFiles open files.
//   - Landlock: read (and execute) beneath the policy's read-only paths and
//     the system locations a dynamically linked helper needs, read, create
//     and write files beneath one scratch directory (not delete or rename
//     them, nor make directories), nothing else. A resident helper has no
//     scratch directory: it writes no file at all (RLIMIT_FSIZE 0 too).
//     ABI >= 4: no TCP bind or connect. ABI >= 6: no connecting to abstract
//     UNIX sockets or signalling processes outside the sandbox.
//   - seccomp-bpf (x86_64 and aarch64): an allow-by-default filter with a
//     short deny list (BuildSeccompProgram()). socket() for anything but
//     an AF_UNIX stream or seqpacket socket, and connect(), fail with
//     EACCES, so libraries that probe for the network fail gracefully
//     instead of being killed. Signals go to the helper itself only (its
//     threads and its process group): on every kernel, as Landlock's
//     scoping does from ABI 6; so do priority, affinity and scheduling
//     changes. It also keeps
//     what the helper writes in files the launcher can measure, and its
//     memory where RLIMIT_DATA counts it.
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

#include "chrome/browser/molt_ai/common/helper_limits.h"

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
// The scratch directory: read, create, write and truncate files. Not:
// delete or rename them (a file deleted while the helper holds it open
// would take space the launcher's measuring can't see), link them into or
// out of another directory, make directories, execute what was written
// there, or create symlinks, sockets, FIFOs or device nodes. (A hard link
// within it is another name the launcher counts.)
inline constexpr uint64_t kScratchAccess =
    kFsReadFile | kFsReadDir | kFsWriteFile | kFsMakeReg | kFsTruncate;

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
  // Read and write beneath this directory (it must exist). Empty, and only
  // empty, for a resident helper.
  std::string writable;
  // A resident helper (helper_resident.h): no writable directory, no file
  // written anywhere (RLIMIT_FSIZE 0), its memory capped at
  // |max_data_bytes| instead of kMaxHelperDataBytes, and the CPU topology
  // ONNX Runtime's cpuinfo reads (kResidentSystemPaths) readable.
  bool resident = false;
  uint64_t max_data_bytes = 0;
};

// What a resident helper may read besides its own directory and its
// policy's paths: ONNX Runtime's CPU detection (pytorch/cpuinfo) reads
// /proc/cpuinfo on arm64 (the CPU topology under /sys/devices/system/cpu is
// readable for every helper). VOICE_BENCH.md §5.3.
inline constexpr const char* kResidentSystemPaths[] = {"/proc/cpuinfo"};

struct PathRule {
  std::string path;
  uint64_t access = 0;
  // A missing required path fails the run; others are skipped.
  bool required = false;
};

// The Landlock rules for |policy|: the system locations every dynamically
// linked helper needs (/usr, /lib, /lib64 read+execute; /etc and
// /sys/devices/system/cpu read; /dev/null read+write; /dev/zero,
// /dev/random, /dev/urandom read), a resident helper's
// kResidentSystemPaths (read), then the policy's own paths (no writable
// one for a resident helper). /proc/self (read) is added in the child,
// where it names the child: the helper process itself, not processes it
// starts. Access bits are masked to what the kernel handles when the
// ruleset is built.
std::vector<PathRule> BuildPathRules(const Policy& policy);

// The Landlock ABI version of the running kernel, or 0 without Landlock.
int LandlockAbi();

// The filesystem rights Landlock ABI |abi| knows (0 for abi < 1).
uint64_t HandledFsAccess(int abi);

// The seccomp-bpf program for this build's CPU architecture (x86_64,
// aarch64), empty on any other. It checks the architecture first (a foreign
// syscall ABI, e.g. int 0x80 or x32, kills the process), then denies:
//   socket() / socketpair() other than an AF_UNIX stream or seqpacket
//     socket, and connect(): EACCES (no network; no talking to the session
//     bus, X11, ssh-agent or other named services through a UNIX socket,
//     nor sending datagrams to one, such as the system log, without
//     connect());
//   io_uring_*: EPERM (it can open sockets without socket());
//   setsid, setpgid: EPERM (what the helper starts stays in its process
//     group, which the launcher kills);
//   ptrace, process_vm_readv/writev, pidfd_getfd: EPERM (other processes);
//   signals to anything but the helper (Landlock scopes them only from
//     ABI 6, Linux 6.12): kill() of anything but its own process ID or its
//     own process group (0, or minus its ID: the child leads a session of
//     its own), tgkill() and rt_tgsigqueueinfo() outside its own thread
//     group, rt_sigqueueinfo() to another process, tkill() and
//     pidfd_send_signal() (they can't be checked; glibc's raise(),
//     abort() and pthread_kill() use tgkill()), fcntl(F_SETOWN) of another
//     process or group and F_SETOWN_EX, ioctl(FIOSETOWN, SIOCSPGRP) (whom
//     the kernel signals when a file is ready), and prlimit64() of
//     another process (RLIMIT_CPU would have the kernel kill it): EPERM.
//     These compare with the helper's own process ID, which only the child
//     knows: SetSeccompOwnPid() puts it in;
//   scheduling and memory placement of anything but the helper (Landlock
//     has no rule for these): setpriority() and ioprio_set() of another
//     process or group or of the user's processes, sched_setaffinity(),
//     sched_setscheduler(), sched_setparam(), sched_setattr(),
//     migrate_pages() and move_pages() of another process (or of a thread
//     named by its ID: it can't be told from a process): EPERM;
//   bpf, perf_event_open, userfaultfd: EPERM (kernel attack surface);
//   keyctl, add_key, request_key: EPERM (the user's kernel keyrings);
//   unshare, setns, clone() with namespace flags, mount and the new mount
//     API, umount2, pivot_root: EPERM; clone3: ENOSYS (its flags can't be
//     inspected; libc falls back to clone());
//   clone() for anything but a thread, fork, vfork: EPERM (the helper is
//     one process: RLIMIT_DATA, which is per process, bounds all its
//     memory, and the launcher's kill reaches all of it);
// and keeps what the helper writes where the launcher measures it, and its
// memory where RLIMIT_DATA counts it (helper_limits.h):
//   fallocate: EOPNOTSUPP (it reserves space past RLIMIT_FSIZE with
//     FALLOC_FL_KEEP_SIZE; posix_fallocate() writes instead);
//   open() / openat() with O_TMPFILE: EOPNOTSUPP (a file in no directory);
//     openat2: ENOSYS (its flags can't be inspected; libc uses openat());
//   setxattr, lsetxattr, fsetxattr, setxattrat: EOPNOTSUPP (data no file's
//     size shows);
//   mmap() of shared anonymous memory: EPERM; memfd_create, memfd_secret:
//     ENOSYS; SysV IPC (shm*, msg*, sem*): EPERM (memory RLIMIT_DATA
//     doesn't count; SysV objects also outlive the process).
// Everything else is allowed.
std::vector<sock_filter> BuildSeccompProgram();

// What BuildSeccompProgram() compares a process ID with where it means the
// helper's own, and minus its own (its process group), until
// SetSeccompOwnPid() puts the real one in: values no process ID can have
// (PID_MAX_LIMIT is 2^22), so until then those comparisons match nothing.
inline constexpr uint32_t kSeccompOwnPidSlot = 0x7fffffffU;
inline constexpr uint32_t kSeccompOwnGroupSlot = 0x80000001U;  // -0x7fffffff

// Puts |pid|, the helper's own process ID, into |program|'s comparisons
// with it (kSeccompOwnPidSlot, kSeccompOwnGroupSlot). Called in the child
// on its own copy of the program, before the filter is installed.
// Async-signal-safe: it only writes to the program in place.
void SetSeccompOwnPid(int32_t pid, std::vector<sock_filter>* program);

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
inline constexpr uint64_t kMaxHelperFileBytes =
    helper_limits::kMaxHelperWriteBytes;
// Its private memory (RLIMIT_DATA: heap, anonymous and thread stack
// mappings, not the read-only mappings of its code or a model).
inline constexpr uint64_t kMaxHelperDataBytes =
    helper_limits::kMaxHelperMemoryBytes;
// Its open files (RLIMIT_NOFILE), which bounds the kernel memory that
// descriptors (sockets, pipes) can hold. The helpers use a few.
inline constexpr uint64_t kMaxHelperOpenFiles = 256;

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
  // RLIMIT_FSIZE and RLIMIT_DATA in the child: kMaxHelperFileBytes and
  // kMaxHelperDataBytes, or for a resident helper 0 and its policy's
  // |max_data_bytes|.
  uint64_t file_size_limit = kMaxHelperFileBytes;
  uint64_t data_limit = kMaxHelperDataBytes;
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
// scratch directory; a resident policy with a writable directory or no
// memory cap): the caller must not run the helper. Blocking.
bool Prepare(const Policy& policy, Prepared* prepared, std::string* error);

// Child side, after fork() and before exec(), with the ruleset at
// kChildRulesetFd: new session, the resource limits (the prepared file
// size and data limits, no core dump, kMaxHelperOpenFiles; each soft and
// hard, never above the hard limit the browser already has),
// no_new_privs, Landlock (adding /proc/self), seccomp (with the child's own
// process ID put into |prepared|'s program: the child's copy, after
// fork()). Async-signal-safe: raw system calls only, no allocation. If a
// step fails it writes kSetupFailedMarker + the step to stderr and calls
// _exit(kSetupFailedExitCode).
void ApplyInChild(Prepared& prepared);

}  // namespace helper_sandbox_linux
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_HELPER_SANDBOX_LINUX_H_
