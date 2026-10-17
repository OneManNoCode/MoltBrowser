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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// System calls newer than Chromium's sysroot headers. From Linux 5.1 on,
// system call numbers are the same on every architecture.
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
  for (const std::string& path : policy.read_execute) {
    rules.push_back({path, kReadExecuteAccess, false});
  }
  for (const std::string& path : policy.read_only) {
    rules.push_back({path, kReadAccess, false});
  }
  rules.push_back({policy.writable, kScratchAccess, true});
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
  const std::vector<sock_filter> unix_only = {
      LoadWord(ArgLow(0)),
      Jump(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 0, 1),
      Return(SECCOMP_RET_ALLOW),
      Return(ReturnErrno(EACCES)),
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

  // Kernel attack surface no OCR or speech helper needs.
  Deny(__NR_bpf, EPERM, &program);
  Deny(__NR_perf_event_open, EPERM, &program);
  Deny(__NR_userfaultfd, EPERM, &program);

  // The user's kernel keyrings.
  Deny(__NR_keyctl, EPERM, &program);
  Deny(__NR_add_key, EPERM, &program);
  Deny(__NR_request_key, EPERM, &program);

  // Namespaces and mounts. clone() is allowed for threads and processes
  // but not for new namespaces; clone3() passes its flags in memory BPF
  // can't read, so it reports ENOSYS and libc falls back to clone().
  OnSyscall(__NR_clone,
            {
                LoadWord(ArgLow(0)),
                Jump(BPF_JMP | BPF_JSET | BPF_K, kCloneNamespaceFlags, 0, 1),
                Return(ReturnErrno(EPERM)),
                Return(SECCOMP_RET_ALLOW),
            },
            &program);
  Deny(__NR_clone3, ENOSYS, &program);
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

  program.push_back(Return(SECCOMP_RET_ALLOW));
  return program;
#endif
}

Prepared::Prepared() = default;

Prepared::Prepared(Prepared&& other)
    : landlock_abi(other.landlock_abi),
      ruleset_fd(std::exchange(other.ruleset_fd, -1)),
      handled_fs(other.handled_fs),
      restrict_flags(other.restrict_flags),
      seccomp(std::move(other.seccomp)) {}

Prepared& Prepared::operator=(Prepared&& other) {
  if (this != &other) {
    CloseRuleset();
    landlock_abi = other.landlock_abi;
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
  if (!IsAbsolute(policy.writable)) {
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

void ApplyInChild(const Prepared& prepared) {
  // A session of its own: no controlling terminal to push input into
  // (TIOCSTI), and a process group the launcher can kill as a whole. Fails
  // only for a process group leader, which a forked child is not.
  setsid();

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
    sock_fprog program = {};
    program.len = static_cast<unsigned short>(prepared.seccomp.size());
    // The kernel only reads the program.
    program.filter = const_cast<sock_filter*>(prepared.seccomp.data());
    if (syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0U, &program) != 0) {
      FailInChild("seccomp(SECCOMP_SET_MODE_FILTER)");
    }
  }
}

}  // namespace helper_sandbox_linux
}  // namespace molt_ai
