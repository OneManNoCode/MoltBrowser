// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/helper_sandbox_linux.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// System calls newer than Chromium's sysroot headers. From Linux 5.1 on,
// system call numbers are the same on every architecture.
#ifndef __NR_pidfd_send_signal
#define __NR_pidfd_send_signal 424
#endif
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register 427
#endif
#ifndef __NR_open_tree
#define __NR_open_tree 428
#endif
#ifndef __NR_move_mount
#define __NR_move_mount 429
#endif
#ifndef __NR_fsopen
#define __NR_fsopen 430
#endif
#ifndef __NR_fsconfig
#define __NR_fsconfig 431
#endif
#ifndef __NR_fsmount
#define __NR_fsmount 432
#endif
#ifndef __NR_fspick
#define __NR_fspick 433
#endif
#ifndef __NR_clone3
#define __NR_clone3 435
#endif
#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd 438
#endif
#ifndef __NR_mount_setattr
#define __NR_mount_setattr 442
#endif
#ifndef __NR_openat2
#define __NR_openat2 437
#endif
#ifndef __NR_memfd_secret
#define __NR_memfd_secret 447
#endif
#ifndef __NR_setxattrat
#define __NR_setxattrat 463
#endif
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif

namespace molt_ai {
namespace helper_sandbox_linux {

namespace {

constexpr long kSysLandlockCreateRuleset = 444;
constexpr long kSysLandlockAddRule = 445;
constexpr long kSysLandlockRestrictSelf = 446;
constexpr unsigned int kLandlockCreateRulesetVersion = 1U << 0;
constexpr int kLandlockRulePathBeneath = 1;

// struct landlock_ruleset_attr as of ABI 6. The kernel accepts a larger
// struct than it knows as long as the fields it doesn't know are zero.
struct LandlockRulesetAttr {
  uint64_t handled_access_fs;
  uint64_t handled_access_net;  // ABI 4
  uint64_t scoped;              // ABI 6
};

// struct landlock_path_beneath_attr.
struct __attribute__((packed)) LandlockPathBeneathAttr {
  uint64_t allowed_access;
  int32_t parent_fd;
};

// The rights a rule on a file (rather than a directory) may carry.
constexpr uint64_t kFileRights =
    kFsExecute | kFsWriteFile | kFsReadFile | kFsTruncate | kFsIoctlDev;

bool IsAbsolute(const std::string& path) {
  return !path.empty() && path.front() == '/';
}

std::string ErrnoText(int error) {
  return "errno " + std::to_string(error);
}

// Opens |rule.path| and adds it to the ruleset with the rights the kernel
// handles (file rights only, for a file).
bool AddPathRule(int ruleset_fd,
                 const PathRule& rule,
                 uint64_t handled_fs,
                 std::string* error) {
  const int fd = open(rule.path.c_str(), O_PATH | O_CLOEXEC);
  if (fd < 0) {
    if (!rule.required) {
      return true;  // e.g. no /lib64 on this system.
    }
    *error = rule.path + ": " + ErrnoText(errno);
    return false;
  }
  struct stat info = {};
  if (fstat(fd, &info) != 0) {
    *error = rule.path + ": " + ErrnoText(errno);
    close(fd);
    return false;
  }
  const bool is_directory = S_ISDIR(info.st_mode);
  if (rule.required && !is_directory) {
    *error = rule.path + " is not a directory";
    close(fd);
    return false;
  }
  uint64_t access = rule.access & handled_fs;
  if (!is_directory) {
    access &= kFileRights;
  }
  bool ok = true;
  if (access != 0) {
    LandlockPathBeneathAttr attr = {};
    attr.allowed_access = access;
    attr.parent_fd = fd;
    if (syscall(kSysLandlockAddRule, ruleset_fd, kLandlockRulePathBeneath,
                &attr, 0U) != 0) {
      *error = "landlock_add_rule(" + rule.path + "): " + ErrnoText(errno);
      ok = false;
    }
  }
  close(fd);
  return ok;
}

#if defined(__x86_64__) || defined(__aarch64__)
#if defined(__x86_64__)
// x86_64 x32 system calls have this bit set in their number.
constexpr uint32_t kX32SyscallBit = 0x40000000U;
#endif

// clone() flags that create namespaces.
constexpr uint32_t kCloneNamespaceFlags =
    CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC |
    CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET;
// The clone() flag that makes a thread rather than a process.
constexpr uint32_t kCloneThread = CLONE_THREAD;

// open() flags: O_TMPFILE without O_DIRECTORY (the same on x86_64 and
// aarch64), the bit that makes a file in no directory.
constexpr uint32_t kOpenTmpfileBit = 020000000;

// mmap() flags (the same on x86_64 and aarch64).
constexpr uint32_t kMapTypeMask = 0x0f;   // MAP_TYPE
constexpr uint32_t kMapPrivate = 0x02;    // MAP_PRIVATE
constexpr uint32_t kMapAnonymous = 0x20;  // MAP_ANONYMOUS

// socket() and socketpair(): the type's bits in their second argument;
// the rest are SOCK_NONBLOCK and SOCK_CLOEXEC (the kernel's
// SOCK_TYPE_MASK).
constexpr uint32_t kSocketTypeMask = 0x0f;

// fcntl() commands and ioctl() requests that choose whom the kernel signals
// when a file is ready (SIGIO, SIGURG, F_SETSIG's signal): the same on
// x86_64 and aarch64 (asm-generic/fcntl.h, asm-generic/sockios.h).
constexpr uint32_t kFcntlSetOwner = 8;     // F_SETOWN
constexpr uint32_t kFcntlSetOwnerEx = 15;  // F_SETOWN_EX
constexpr uint32_t kIoctlSetOwner = 0x8901;         // FIOSETOWN
constexpr uint32_t kIoctlSetProcessGroup = 0x8902;  // SIOCSPGRP

// BPF instructions.
sock_filter Statement(uint16_t code, uint32_t k) {
  return {code, 0, 0, k};
}

sock_filter Jump(uint16_t code,
                 uint32_t k,
                 uint8_t jump_true,
                 uint8_t jump_false) {
  return {code, jump_true, jump_false, k};
}

sock_filter LoadWord(uint32_t offset) {
  return Statement(BPF_LD | BPF_W | BPF_ABS, offset);
}

sock_filter Return(uint32_t action) {
  return Statement(BPF_RET | BPF_K, action);
}

constexpr uint32_t ReturnErrno(int error) {
  return SECCOMP_RET_ERRNO | (static_cast<uint32_t>(error) & SECCOMP_RET_DATA);
}

constexpr uint32_t kNrOffset = offsetof(struct seccomp_data, nr);
constexpr uint32_t kArchOffset = offsetof(struct seccomp_data, arch);

// The low 32 bits of system call argument |index|. Both supported
// architectures are little-endian, and the arguments checked here are
// ints (or, for clone(), flags the kernel reads from the low 32 bits).
constexpr uint32_t ArgLow(uint32_t index) {
  return static_cast<uint32_t>(offsetof(struct seccomp_data, args)) +
         8U * index;
}

// Appends "if (nr == |nr|) { |body| }" to |program|. With the system call
// number in the accumulator. |body| must return on every path.
void OnSyscall(long nr,
               const std::vector<sock_filter>& body,
               std::vector<sock_filter>* program) {
  program->push_back(Jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr),
                          0, static_cast<uint8_t>(body.size())));
  program->insert(program->end(), body.begin(), body.end());
}

void Deny(long nr, int error, std::vector<sock_filter>* program) {
  OnSyscall(nr, {Return(ReturnErrno(error))}, program);
}

// Allowed when argument |index| is one of |values|; EPERM otherwise. The
// values may be kSeccompOwnPidSlot and kSeccompOwnGroupSlot.
std::vector<sock_filter> OnlyWhenArgIs(uint32_t index,
                                       std::initializer_list<uint32_t> values) {
  std::vector<sock_filter> body = {LoadWord(ArgLow(index))};
  // From the first comparison, past the rest and the EPERM, to the allow.
  uint8_t to_allow = static_cast<uint8_t>(values.size());
  for (uint32_t value : values) {
    body.push_back(Jump(BPF_JMP | BPF_JEQ | BPF_K, value, to_allow, 0));
    --to_allow;
  }
  body.push_back(Return(ReturnErrno(EPERM)));
  body.push_back(Return(SECCOMP_RET_ALLOW));
  return body;
}

// An open() or openat() with O_TMPFILE in its flags (argument
// |flags_index|): EOPNOTSUPP.
std::vector<sock_filter> NoTmpfile(uint32_t flags_index) {
  return {
      LoadWord(ArgLow(flags_index)),
      Jump(BPF_JMP | BPF_JSET | BPF_K, kOpenTmpfileBit, 0, 1),
      Return(ReturnErrno(EOPNOTSUPP)),
      Return(SECCOMP_RET_ALLOW),
  };
}
#endif  // defined(__x86_64__) || defined(__aarch64__)

// Async-signal-safe output for ApplyInChild().
void WriteToStderr(std::string_view text) {
  [[maybe_unused]] const ssize_t written =
      write(STDERR_FILENO, text.data(), text.size());
}

void WriteNumberToStderr(int value) {
  std::array<char, 12> digits = {};
  size_t count = 0;
  unsigned int rest = value < 0 ? 0U : static_cast<unsigned int>(value);
  do {
    digits[count++] = static_cast<char>('0' + rest % 10);
    rest /= 10;
  } while (rest != 0 && count < digits.size());
  for (size_t i = 0; i < count / 2; ++i) {
    std::swap(digits[i], digits[count - 1 - i]);
  }
  [[maybe_unused]] const ssize_t written =
      write(STDERR_FILENO, digits.data(), count);
}

[[noreturn]] void FailInChild(std::string_view step) {
  const int error = errno;
  WriteToStderr(kSetupFailedMarker);
  WriteToStderr(step);
  WriteToStderr(" failed, errno ");
  WriteNumberToStderr(error);
  WriteToStderr("\n");
  _exit(kSetupFailedExitCode);
}

// Sets both limits of |resource| to |value|, or to the hard limit already
// in place if that is lower: a limit is never raised. Async-signal-safe.
bool LowerLimit(int resource, uint64_t value) {
  struct rlimit limit = {};
  if (getrlimit(resource, &limit) != 0) {
    return false;
  }
  const rlim_t target =
      limit.rlim_max != RLIM_INFINITY && limit.rlim_max < value
          ? limit.rlim_max
          : static_cast<rlim_t>(value);
  limit.rlim_cur = target;
  limit.rlim_max = target;
  return setrlimit(resource, &limit) == 0;
}

}  // namespace

Policy::Policy() = default;
Policy::Policy(const Policy&) = default;
Policy& Policy::operator=(const Policy&) = default;
Policy::~Policy() = default;

std::vector<PathRule> BuildPathRules(const Policy& policy) {
  std::vector<PathRule> rules;
  // The dynamic loader, shared libraries and data files of a helper
  // installed by the distribution (e.g. /usr/bin/tesseract and its
  // tessdata under /usr/share).
  for (const char* path : {"/usr", "/lib", "/lib64"}) {
    rules.push_back({path, kReadExecuteAccess, false});
  }
  // ld.so.cache and other configuration; CPU topology for thread counts.
  for (const char* path : {"/etc", "/sys/devices/system/cpu"}) {
    rules.push_back({path, kReadAccess, false});
  }
  rules.push_back(
      {"/dev/null", kFsReadFile | kFsWriteFile | kFsTruncate, false});
  for (const char* path : {"/dev/zero", "/dev/random", "/dev/urandom"}) {
    rules.push_back({path, kFsReadFile, false});
  }
  if (policy.resident) {
    for (const char* path : kResidentSystemPaths) {
      rules.push_back({path, kReadAccess, false});
    }
  }
  for (const std::string& path : policy.read_execute) {
    rules.push_back({path, kReadExecuteAccess, false});
  }
  for (const std::string& path : policy.read_only) {
    rules.push_back({path, kReadAccess, false});
  }
  if (!policy.resident) {
    rules.push_back({policy.writable, kScratchAccess, true});
  }
  return rules;
}

int LandlockAbi() {
  const long abi = syscall(kSysLandlockCreateRuleset, nullptr, size_t{0},
                           kLandlockCreateRulesetVersion);
  return abi > 0 ? static_cast<int>(abi) : 0;
}

uint64_t HandledFsAccess(int abi) {
  if (abi < 1) {
    return 0;
  }
  uint64_t handled = (kFsMakeSym << 1) - 1;  // ABI 1: execute .. make_sym
  if (abi >= 2) {
    handled |= kFsRefer;
  }
  if (abi >= 3) {
    handled |= kFsTruncate;
  }
  if (abi >= 5) {
    handled |= kFsIoctlDev;
  }
  return handled;
}

std::string_view SeccompArchName() {
#if defined(__x86_64__)
  return "x86_64";
#elif defined(__aarch64__)
  return "aarch64";
#else
  return std::string_view();
#endif
}

std::vector<sock_filter> BuildSeccompProgram() {
#if defined(__x86_64__)
  constexpr uint32_t kArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
  constexpr uint32_t kArch = AUDIT_ARCH_AARCH64;
#else
  return {};
#endif
#if defined(__x86_64__) || defined(__aarch64__)
  std::vector<sock_filter> program;
  // A system call made through another ABI (i386 int 0x80, x32) has other
  // numbers: kill rather than check it against the wrong table.
  program.push_back(LoadWord(kArchOffset));
  program.push_back(Jump(BPF_JMP | BPF_JEQ | BPF_K, kArch, 1, 0));
  program.push_back(Return(SECCOMP_RET_KILL_PROCESS));
  program.push_back(LoadWord(kNrOffset));
#if defined(__x86_64__)
  program.push_back(Jump(BPF_JMP | BPF_JGE | BPF_K, kX32SyscallBit, 0, 1));
  program.push_back(Return(SECCOMP_RET_KILL_PROCESS));
#endif

  // No network: sockets other than AF_UNIX fail with EACCES, so libraries
  // that probe fail gracefully. connect() fails too, which also keeps the
  // helper away from named UNIX-socket services (the session bus and
  // through it systemd, X11, ssh-agent, docker); socketpair() still works.
  // A datagram socket reaches a named one without connect() (sendto() to
  // the system log or the journal), so AF_UNIX sockets are stream or
  // seqpacket only (SOCK_RAW is a datagram socket there too).
  const std::vector<sock_filter> unix_only = {
      LoadWord(ArgLow(0)),
      Jump(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 0, 4),
      LoadWord(ArgLow(1)),
      Statement(BPF_ALU | BPF_AND | BPF_K, kSocketTypeMask),
      Jump(BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 2, 0),
      Jump(BPF_JMP | BPF_JEQ | BPF_K, SOCK_SEQPACKET, 1, 0),
      Return(ReturnErrno(EACCES)),
      Return(SECCOMP_RET_ALLOW),
  };
  OnSyscall(__NR_socket, unix_only, &program);
  OnSyscall(__NR_socketpair, unix_only, &program);
  Deny(__NR_connect, EACCES, &program);
  // io_uring can open and connect sockets without the calls above.
  Deny(__NR_io_uring_setup, EPERM, &program);
  Deny(__NR_io_uring_enter, EPERM, &program);
  Deny(__NR_io_uring_register, EPERM, &program);

  // Whatever the helper starts stays in its process group, which the
  // launcher kills (the helper itself already leads a session of its own).
  Deny(__NR_setsid, EPERM, &program);
  Deny(__NR_setpgid, EPERM, &program);

  // Other processes' memory and file descriptors.
  Deny(__NR_ptrace, EPERM, &program);
  Deny(__NR_process_vm_readv, EPERM, &program);
  Deny(__NR_process_vm_writev, EPERM, &program);
  Deny(__NR_pidfd_getfd, EPERM, &program);

  // Signals to the helper itself only: its own process ID, its threads
  // and its process group (0 or minus its ID: it leads a session of its
  // own and starts no process), on kernels whose Landlock doesn't scope
  // signals (before ABI 6) as well. tkill() names a thread of any process
  // and pidfd_send_signal() a pidfd, neither of which BPF can check;
  // glibc's raise(), abort() and pthread_kill() use tgkill() with the
  // process's own ID.
  OnSyscall(__NR_kill,
            OnlyWhenArgIs(0, {kSeccompOwnPidSlot, 0, kSeccompOwnGroupSlot}),
            &program);
  OnSyscall(__NR_tgkill, OnlyWhenArgIs(0, {kSeccompOwnPidSlot}), &program);
  OnSyscall(__NR_rt_sigqueueinfo, OnlyWhenArgIs(0, {kSeccompOwnPidSlot}),
            &program);
  OnSyscall(__NR_rt_tgsigqueueinfo, OnlyWhenArgIs(0, {kSeccompOwnPidSlot}),
            &program);
  Deny(__NR_tkill, EPERM, &program);
  Deny(__NR_pidfd_send_signal, EPERM, &program);
  // Whom the kernel signals when a file is ready (O_ASYNC's SIGIO, SIGURG,
  // or F_SETSIG's signal, of any number): the helper itself only.
  // F_SETOWN_EX passes its target in memory BPF can't read, and so do
  // FIOSETOWN and SIOCSPGRP.
  const std::vector<sock_filter> owner =
      OnlyWhenArgIs(2, {kSeccompOwnPidSlot, 0, kSeccompOwnGroupSlot});
  std::vector<sock_filter> fcntl_rule = {
      LoadWord(ArgLow(1)),
      Jump(BPF_JMP | BPF_JEQ | BPF_K, kFcntlSetOwnerEx, 0, 1),
      Return(ReturnErrno(EPERM)),
      // Any other command: to the allow at the end of |owner|.
      Jump(BPF_JMP | BPF_JEQ | BPF_K, kFcntlSetOwner, 0,
           static_cast<uint8_t>(owner.size() - 1)),
  };
  fcntl_rule.insert(fcntl_rule.end(), owner.begin(), owner.end());
  OnSyscall(__NR_fcntl, fcntl_rule, &program);
  OnSyscall(__NR_ioctl,
            {
                LoadWord(ArgLow(1)),
                Jump(BPF_JMP | BPF_JEQ | BPF_K, kIoctlSetOwner, 1, 0),
                Jump(BPF_JMP | BPF_JEQ | BPF_K, kIoctlSetProcessGroup, 0, 1),
                Return(ReturnErrno(EPERM)),
                Return(SECCOMP_RET_ALLOW),
            },
            &program);
  // Its own resource limits only (0 is itself): another process's
  // RLIMIT_CPU, lowered below the time it has used, has the kernel kill it.
  OnSyscall(__NR_prlimit64, OnlyWhenArgIs(0, {0, kSeccompOwnPidSlot}),
            &program);

  // Kernel attack surface no OCR or speech helper needs.
  Deny(__NR_bpf, EPERM, &program);
  Deny(__NR_perf_event_open, EPERM, &program);
  Deny(__NR_userfaultfd, EPERM, &program);

  // The user's kernel keyrings.
  Deny(__NR_keyctl, EPERM, &program);
  Deny(__NR_add_key, EPERM, &program);
  Deny(__NR_request_key, EPERM, &program);

  // Threads only, in no new namespace: the helper is one process, so
  // RLIMIT_DATA (per process) bounds all its memory, and the launcher's
  // kill all of it. clone() makes a thread or a process; clone3() passes
  // its flags in memory BPF can't read, so it reports ENOSYS and libc falls
  // back to clone().
  OnSyscall(__NR_clone,
            {
                LoadWord(ArgLow(0)),
                Jump(BPF_JMP | BPF_JSET | BPF_K, kCloneNamespaceFlags, 1, 0),
                Jump(BPF_JMP | BPF_JSET | BPF_K, kCloneThread, 1, 0),
                Return(ReturnErrno(EPERM)),
                Return(SECCOMP_RET_ALLOW),
            },
            &program);
  Deny(__NR_clone3, ENOSYS, &program);
#if defined(__NR_fork)
  Deny(__NR_fork, EPERM, &program);
#endif
#if defined(__NR_vfork)
  Deny(__NR_vfork, EPERM, &program);
#endif
  Deny(__NR_unshare, EPERM, &program);
  Deny(__NR_setns, EPERM, &program);
  Deny(__NR_mount, EPERM, &program);
  Deny(__NR_umount2, EPERM, &program);
  Deny(__NR_pivot_root, EPERM, &program);
  Deny(__NR_open_tree, EPERM, &program);
  Deny(__NR_move_mount, EPERM, &program);
  Deny(__NR_fsopen, EPERM, &program);
  Deny(__NR_fsconfig, EPERM, &program);
  Deny(__NR_fsmount, EPERM, &program);
  Deny(__NR_fspick, EPERM, &program);
  Deny(__NR_mount_setattr, EPERM, &program);

  // What the helper writes stays in files the launcher measures
  // (helper_limits.h). fallocate() reserves space without writing, which
  // FALLOC_FL_KEEP_SIZE does past RLIMIT_FSIZE; posix_fallocate() writes
  // instead when the system call is refused.
  Deny(__NR_fallocate, EOPNOTSUPP, &program);
  // An O_TMPFILE file is in no directory. openat2() passes its flags in
  // memory BPF can't read; libc uses openat() without it.
#if defined(__NR_open)
  OnSyscall(__NR_open, NoTmpfile(1), &program);
#endif
  OnSyscall(__NR_openat, NoTmpfile(2), &program);
  Deny(__NR_openat2, ENOSYS, &program);
  // Extended attributes hold data that no file's size shows.
  Deny(__NR_setxattr, EOPNOTSUPP, &program);
  Deny(__NR_lsetxattr, EOPNOTSUPP, &program);
  Deny(__NR_fsetxattr, EOPNOTSUPP, &program);
  Deny(__NR_setxattrat, EOPNOTSUPP, &program);

  // Its memory stays where RLIMIT_DATA counts it: no shared memory, which
  // it doesn't count. That is a MAP_SHARED anonymous mapping, a memfd, and
  // SysV IPC, whose objects also outlive the process (and other processes'
  // are theirs).
  OnSyscall(__NR_mmap,
            {
                LoadWord(ArgLow(3)),
                Jump(BPF_JMP | BPF_JSET | BPF_K, kMapAnonymous, 0, 3),
                Statement(BPF_ALU | BPF_AND | BPF_K, kMapTypeMask),
                Jump(BPF_JMP | BPF_JEQ | BPF_K, kMapPrivate, 1, 0),
                Return(ReturnErrno(EPERM)),
                Return(SECCOMP_RET_ALLOW),
            },
            &program);
  Deny(__NR_memfd_create, ENOSYS, &program);
  Deny(__NR_memfd_secret, ENOSYS, &program);
  for (long nr : {__NR_shmget, __NR_shmat, __NR_shmctl, __NR_shmdt, __NR_msgget,
                  __NR_msgsnd, __NR_msgrcv, __NR_msgctl, __NR_semget,
                  __NR_semop, __NR_semtimedop, __NR_semctl}) {
    Deny(nr, EPERM, &program);
  }

  program.push_back(Return(SECCOMP_RET_ALLOW));
  return program;
#endif
}

void SetSeccompOwnPid(int32_t pid, std::vector<sock_filter>* program) {
  const uint32_t own = static_cast<uint32_t>(pid);
  // Minus the ID, as a 32-bit argument holds it (no overflow: pid > 0).
  const uint32_t group = 0U - own;
  for (sock_filter& insn : *program) {
    if (insn.code != (BPF_JMP | BPF_JEQ | BPF_K)) {
      continue;
    }
    if (insn.k == kSeccompOwnPidSlot) {
      insn.k = own;
    } else if (insn.k == kSeccompOwnGroupSlot) {
      insn.k = group;
    }
  }
}

Prepared::Prepared() = default;

Prepared::Prepared(Prepared&& other)
    : landlock_abi(other.landlock_abi),
      file_size_limit(other.file_size_limit),
      data_limit(other.data_limit),
      ruleset_fd(std::exchange(other.ruleset_fd, -1)),
      handled_fs(other.handled_fs),
      restrict_flags(other.restrict_flags),
      seccomp(std::move(other.seccomp)) {}

Prepared& Prepared::operator=(Prepared&& other) {
  if (this != &other) {
    CloseRuleset();
    landlock_abi = other.landlock_abi;
    file_size_limit = other.file_size_limit;
    data_limit = other.data_limit;
    ruleset_fd = std::exchange(other.ruleset_fd, -1);
    handled_fs = other.handled_fs;
    restrict_flags = other.restrict_flags;
    seccomp = std::move(other.seccomp);
  }
  return *this;
}

Prepared::~Prepared() {
  CloseRuleset();
}

void Prepared::CloseRuleset() {
  if (ruleset_fd >= 0) {
    close(ruleset_fd);
    ruleset_fd = -1;
  }
}

std::string Prepared::Describe() const {
  std::string out;
  if (landlock_abi > 0) {
    out = "Landlock ABI " + std::to_string(landlock_abi) + " (files";
    if (landlock_abi >= 4) {
      out += ", TCP";
    }
    if (landlock_abi >= 6) {
      out += ", abstract sockets, signals";
    }
    out += ")";
  } else {
    out = "no Landlock (kernel without it: files are not confined)";
  }
  if (seccomp.empty()) {
    out += ", no seccomp-bpf (unsupported here: network is not blocked)";
  } else {
    out += ", seccomp-bpf (" + std::string(SeccompArchName()) + ")";
  }
  return out;
}

bool Prepare(const Policy& policy, Prepared* prepared, std::string* error) {
  *prepared = Prepared();
  if (policy.resident) {
    // Nothing writable, and its memory bounded.
    if (!policy.writable.empty()) {
      *error = "a resident helper has no writable directory";
      return false;
    }
    if (policy.max_data_bytes == 0) {
      *error = "a resident helper needs a memory limit";
      return false;
    }
    prepared->file_size_limit = 0;
    prepared->data_limit = policy.max_data_bytes;
  } else if (!IsAbsolute(policy.writable)) {
    *error = "the scratch directory is not an absolute path";
    return false;
  }
  for (const std::vector<std::string>* paths :
       {&policy.read_only, &policy.read_execute}) {
    for (const std::string& path : *paths) {
      if (!IsAbsolute(path)) {
        *error = "\"" + path + "\" is not an absolute path";
        return false;
      }
    }
  }

  const int abi = LandlockAbi();
  if (abi >= 1) {
    LandlockRulesetAttr attr = {};
    attr.handled_access_fs = HandledFsAccess(abi);
    if (abi >= 4) {
      // No rule grants a port: no TCP bind or connect at all.
      attr.handled_access_net = kNetBindTcp | kNetConnectTcp;
    }
    if (abi >= 6) {
      attr.scoped = kScopeAbstractUnixSocket | kScopeSignal;
    }
    const long fd = syscall(kSysLandlockCreateRuleset, &attr, sizeof(attr), 0U);
    if (fd < 0) {
      *error = "landlock_create_ruleset: " + ErrnoText(errno);
      return false;
    }
    prepared->ruleset_fd = static_cast<int>(fd);
    prepared->landlock_abi = abi;
    prepared->handled_fs = attr.handled_access_fs;
    prepared->restrict_flags = abi >= 7 ? kRestrictSelfLogNewExecOn : 0U;
    for (const PathRule& rule : BuildPathRules(policy)) {
      if (!AddPathRule(prepared->ruleset_fd, rule, prepared->handled_fs,
                       error)) {
        *prepared = Prepared();
        return false;
      }
    }
  }

  // PR_GET_SECCOMP fails only on a kernel built without seccomp.
  if (!SeccompArchName().empty() &&
      prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL) >= 0) {
    prepared->seccomp = BuildSeccompProgram();
  }
  return true;
}

void ApplyInChild(Prepared& prepared) {
  // A session of its own: no controlling terminal to push input into
  // (TIOCSTI), and a process group the launcher can kill as a whole. Fails
  // only for a process group leader, which a forked child is not.
  setsid();

  // No file past kMaxHelperFileBytes (none at all for a resident helper)
  // and no core dump: a helper a hostile input took over can't fill the
  // disk. At most kMaxHelperDataBytes of private memory (a resident
  // helper's own limit) and kMaxHelperOpenFiles open files. (Nothing below
  // allocates memory before exec().)
  if (!LowerLimit(RLIMIT_FSIZE, prepared.file_size_limit)) {
    FailInChild("setrlimit(RLIMIT_FSIZE)");
  }
  if (!LowerLimit(RLIMIT_CORE, 0)) {
    FailInChild("setrlimit(RLIMIT_CORE)");
  }
  if (!LowerLimit(RLIMIT_DATA, prepared.data_limit)) {
    FailInChild("setrlimit(RLIMIT_DATA)");
  }
  if (!LowerLimit(RLIMIT_NOFILE, kMaxHelperOpenFiles)) {
    FailInChild("setrlimit(RLIMIT_NOFILE)");
  }

  // base::LaunchProcess() sets this too; seccomp and Landlock require it.
  if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0) {
    FailInChild("prctl(PR_SET_NO_NEW_PRIVS)");
  }

  if (prepared.ruleset_fd >= 0) {
    // /proc/self names this process only here, in the child. Best effort:
    // without it the helper can't read its own /proc entries, which only
    // some libraries probe.
    const int proc_fd = open("/proc/self", O_PATH | O_CLOEXEC | O_DIRECTORY);
    if (proc_fd >= 0) {
      LandlockPathBeneathAttr attr = {};
      attr.allowed_access = kReadAccess & prepared.handled_fs;
      attr.parent_fd = proc_fd;
      syscall(kSysLandlockAddRule, kChildRulesetFd, kLandlockRulePathBeneath,
              &attr, 0U);
      close(proc_fd);
    }
    if (syscall(kSysLandlockRestrictSelf, kChildRulesetFd,
                prepared.restrict_flags) != 0) {
      FailInChild("landlock_restrict_self");
    }
    close(kChildRulesetFd);
  }

  if (!prepared.seccomp.empty()) {
    // Signals to this process only: its ID, which exec() keeps, written
    // into this child's own copy of the program (fork() copied it).
    SetSeccompOwnPid(getpid(), &prepared.seccomp);
    sock_fprog program = {};
    program.len = static_cast<unsigned short>(prepared.seccomp.size());
    program.filter = prepared.seccomp.data();
    if (syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0U, &program) != 0) {
      FailInChild("seccomp(SECCOMP_SET_MODE_FILTER)");
    }
  }
}

}  // namespace helper_sandbox_linux
}  // namespace molt_ai
