// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/helper_sandbox.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/common/helper_limits.h"
#include "chrome/browser/molt_ai/common/helper_sandbox_win_policy.h"
#include "chrome/browser/molt_ai/common/seatbelt_profile.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_POSIX)
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
#include "base/path_service.h"
#endif

#if BUILDFLAG(IS_WIN)
// winsock2.h before windows.h, which would otherwise bring in winsock.h.
#include <winsock2.h>

#include <windows.h>

#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/security_descriptor.h"
#include "base/win/security_util.h"
#include "base/win/sid.h"
#endif

#if BUILDFLAG(IS_LINUX)
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "chrome/browser/molt_ai/common/helper_sandbox_linux.h"
#endif

namespace molt_ai {
namespace {

// The parenthesis depth at the end of an SBPL profile, ignoring strings
// and comments; -1 if it closes more than it opened or a string never ends.
int SbplParenthesisBalance(std::string_view profile) {
  int depth = 0;
  bool in_string = false;
  bool in_comment = false;
  bool escaped = false;
  for (char c : profile) {
    if (in_comment) {
      in_comment = c != '\n';
    } else if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
    } else if (c == ';') {
      in_comment = true;
    } else if (c == '"') {
      in_string = true;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')' && --depth < 0) {
      return -1;
    }
  }
  return in_string ? -1 : depth;
}

size_t CountOf(std::string_view text, std::string_view needle) {
  size_t count = 0;
  for (size_t pos = text.find(needle); pos != std::string_view::npos;
       pos = text.find(needle, pos + needle.size())) {
    ++count;
  }
  return count;
}

TEST(SeatbeltProfileTest, DeniesByDefault) {
  const std::string profile = seatbelt::BuildProfile(2);
  EXPECT_EQ(profile.rfind("(version 1)\n(deny default)\n", 0), 0u);
  EXPECT_NE(profile.find("(deny network*)"), std::string::npos);
  EXPECT_EQ(profile.find("(allow network"), std::string::npos);
  EXPECT_EQ(profile.find("(allow mach-lookup"), std::string::npos);
  EXPECT_EQ(profile.find("(allow iokit"), std::string::npos);
  EXPECT_EQ(profile.find("(allow default"), std::string::npos);
  // Writes: /dev/null, and creating and writing regular files beneath the
  // scratch directory, nothing else (no unlink, rename, xattr, mode or
  // flags: all it writes stays where the launcher measures it).
  EXPECT_EQ(CountOf(profile, "file-write"), 3u);
  EXPECT_EQ(profile.find("file-write*"), std::string::npos);
  EXPECT_NE(profile.find("(allow file-write-data (literal \"/dev/null\"))"),
            std::string::npos);
  EXPECT_NE(profile.find("(allow file-write-create file-write-data\n"
                         "  (require-all\n"
                         "    (prefix (string-append (param \"SCRATCH\") "
                         "\"/\"))\n"
                         "    (vnode-type REGULAR-FILE)))"),
            std::string::npos);
  EXPECT_NE(profile.find("(allow file-read* (prefix (string-append (param "
                         "\"SCRATCH\") \"/\")))"),
            std::string::npos);
  // Exec of the helper only, and no other process.
  EXPECT_EQ(CountOf(profile, "process-exec"), 1u);
  EXPECT_NE(profile.find("(allow process-exec (literal (param \"HELPER\")))"),
            std::string::npos);
  EXPECT_EQ(profile.find("process-fork"), std::string::npos);
}

TEST(SeatbeltProfileTest, IsWellFormed) {
  for (size_t count = 0; count < 5; ++count) {
    EXPECT_EQ(SbplParenthesisBalance(seatbelt::BuildProfile(count)), 0)
        << count;
  }
}

TEST(SeatbeltProfileTest, OneParameterPerReadOnlyPath) {
  const std::string profile = seatbelt::BuildProfile(3);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(CountOf(profile, "(subpath (param \"" +
                                   seatbelt::ReadOnlyParam(i) + "\"))"),
              1u);
  }
  EXPECT_EQ(profile.find("READ_3"), std::string::npos);
  EXPECT_EQ(seatbelt::BuildProfile(0).find("READ_"), std::string::npos);
  EXPECT_NE(profile.find("(subpath (param \"HELPER_DIR\"))"),
            std::string::npos);
}

TEST(SeatbeltProfileTest, PathsOnlyAppearAsParameters) {
  // Characters that would break out of an SBPL string or list.
  const std::string helper = "/Apps/M (\"x\").app/ocr/tesseract";
  const std::string scratch =
      "/private/var/folders/T/molt_ocr\")(allow default";
  const std::vector<std::string> read_only = {
      "/Apps/M (\"x\").app/ocr/tessdata", scratch + "/input.png"};
  const std::vector<std::string> argv = seatbelt::BuildSandboxExecArgv(
      helper, {scratch + "/input.png", scratch + "/input", "-l", "eng"},
      read_only, scratch);
  const std::vector<std::string> expected = {
      "/usr/bin/sandbox-exec",
      "-p",
      seatbelt::BuildProfile(2),
      "-D",
      "HELPER=" + helper,
      "-D",
      "HELPER_DIR=/Apps/M (\"x\").app/ocr",
      "-D",
      "READ_0=" + read_only[0],
      "-D",
      "READ_1=" + read_only[1],
      "-D",
      "SCRATCH=" + scratch,
      helper,
      scratch + "/input.png",
      scratch + "/input",
      "-l",
      "eng",
  };
  EXPECT_EQ(argv, expected);
  EXPECT_EQ(argv[2].find("/Apps"), std::string::npos);
  EXPECT_EQ(argv[2].find("molt_ocr"), std::string::npos);
}

TEST(SeatbeltProfileTest, RecognizesSandboxExecFailures) {
  EXPECT_TRUE(seatbelt::IsSandboxExecFailure(
      65, "sandbox-exec: unbound variable: foo\n"));
  EXPECT_TRUE(seatbelt::IsSandboxExecFailure(
      71,
      "sandbox-exec: execvp() of '/x/tesseract' failed: Operation not "
      "permitted\n"));
  // The helper's own failures.
  EXPECT_FALSE(seatbelt::IsSandboxExecFailure(1, "sandbox-exec: x"));
  EXPECT_FALSE(seatbelt::IsSandboxExecFailure(65, "Error opening data file"));
  EXPECT_FALSE(seatbelt::IsSandboxExecFailure(0, ""));
}

TEST(HelperSandboxTest, DescribesEachFailure) {
  HelperRunResult run;
  EXPECT_EQ(
      DescribeHelperRunFailure(run, "tesseract", "failed to launch tesseract"),
      "failed to launch tesseract");

  run.sandbox_failed = true;
  run.error = "/usr/bin/sandbox-exec is missing";
  EXPECT_EQ(DescribeHelperRunFailure(run, "tesseract", "x"),
            "tesseract sandbox unavailable: /usr/bin/sandbox-exec is missing");

  run = HelperRunResult();
  run.launched = true;
  run.timed_out = true;
  run.error = "timed out after 120 s";
  EXPECT_EQ(DescribeHelperRunFailure(run, "tesseract", "x"),
            "tesseract timed out after 120 s");

  // Past a limit, whatever it exited with.
  run = HelperRunResult();
  run.launched = true;
  run.limit_exceeded = true;
  run.exit_code = 0;
  run.error = "it wrote 64 MiB or more to its scratch directory";
  EXPECT_EQ(DescribeHelperRunFailure(run, "whisper", "x"),
            "whisper was stopped: it wrote 64 MiB or more to its scratch "
            "directory");

  run = HelperRunResult();
  run.launched = true;
  run.exit_code = 1;
  run.output = "Error \xFF" + std::string(500, 'x');
  const std::string failure = DescribeHelperRunFailure(run, "whisper", "x");
  EXPECT_EQ(failure.rfind("whisper exit=1: Error \xEF\xBF\xBD", 0), 0u);
  EXPECT_EQ(failure.size(), std::string("whisper exit=1: ").size() + 200);

  run = HelperRunResult();
  run.launched = true;
  run.exit_code = 0;
  EXPECT_EQ(DescribeHelperRunFailure(run, "whisper", "x"), "");
}

TEST(HelperScratchDirTest, AFreshPrivateDirectoryPerRun) {
  base::FilePath path;
  {
    HelperScratchDir scratch;
    EXPECT_FALSE(scratch.IsValid());
    ASSERT_TRUE(scratch.Create("molt_test"));
    EXPECT_FALSE(scratch.Create("molt_test"));  // Once only.
    path = scratch.path();
    EXPECT_TRUE(path.IsAbsolute());
    EXPECT_TRUE(base::DirectoryExists(path));
    EXPECT_TRUE(path.BaseName().MaybeAsASCII().starts_with("molt_test"));
    ASSERT_TRUE(base::WriteFile(path.AppendASCII("input.png"), "image"));

    const HelperSandboxPolicy policy = scratch.MakePolicy();
    EXPECT_EQ(policy.writable_dir, path);
    EXPECT_TRUE(policy.read_only_paths.empty());
#if BUILDFLAG(IS_WIN)
    EXPECT_TRUE(helper_sandbox_win::IsAppContainerSid(policy.app_container_sid))
        << policy.app_container_sid;
#else
    EXPECT_TRUE(policy.app_container_sid.empty());
    struct stat info = {};
    ASSERT_EQ(stat(path.value().c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 07777, 0700u);
#endif

    // Another run gets another directory (and on Windows another
    // AppContainer).
    HelperScratchDir other;
    ASSERT_TRUE(other.Create("molt_test"));
    EXPECT_NE(other.path(), path);
#if BUILDFLAG(IS_WIN)
    EXPECT_NE(other.MakePolicy().app_container_sid, policy.app_container_sid);
#endif
  }
  // Gone, with what was in it.
  EXPECT_FALSE(base::PathExists(path));

  for (const char* prefix : {"", "..", "a/b", "a\\b", "molt ocr", "molt-ocr",
                             "abcdefghijklmnopqrstuvwxyz0123456"}) {
    HelperScratchDir scratch;
    EXPECT_FALSE(scratch.Create(prefix)) << prefix;
    EXPECT_FALSE(scratch.IsValid());
  }
}

TEST(HelperScratchDirTest, DataDirectories) {
  base::ScopedTempDir parent;
  ASSERT_TRUE(parent.CreateUniqueTempDir());
  const base::FilePath data = parent.GetPath().AppendASCII("models");
  ASSERT_TRUE(CreateHelperDataDirectory(data));
  EXPECT_TRUE(base::DirectoryExists(data));
  // Never one that is already there.
  EXPECT_FALSE(CreateHelperDataDirectory(data));
  EXPECT_TRUE(base::WriteFile(data.AppendASCII("model.bin"), "weights"));
}

#if BUILDFLAG(IS_POSIX)
TEST(HelperSandboxTest, ReadsOnlyPlainFilesTheHelperMade) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath text = dir.GetPath().AppendASCII("input.txt");
  ASSERT_TRUE(base::WriteFile(text, "recognized text"));
  std::string contents;
  EXPECT_TRUE(ReadHelperOutputFile(text, 1024, &contents));
  EXPECT_EQ(contents, "recognized text");
  EXPECT_TRUE(ReadHelperOutputFile(text, 4, &contents));
  EXPECT_EQ(contents, "reco");

  EXPECT_FALSE(ReadHelperOutputFile(dir.GetPath().AppendASCII("missing"), 1024,
                                    &contents));

  // A symlink or a hard link to a file outside the scratch directory.
  const base::FilePath secret = dir.GetPath().AppendASCII("secret");
  ASSERT_TRUE(base::WriteFile(secret, "secret"));
  const base::FilePath symlink = dir.GetPath().AppendASCII("symlink.txt");
  ASSERT_TRUE(base::CreateSymbolicLink(secret, symlink));
  EXPECT_FALSE(ReadHelperOutputFile(symlink, 1024, &contents));
  EXPECT_EQ(contents, "");
  const base::FilePath hardlink = dir.GetPath().AppendASCII("hardlink.txt");
  ASSERT_EQ(link(secret.value().c_str(), hardlink.value().c_str()), 0);
  EXPECT_FALSE(ReadHelperOutputFile(hardlink, 1024, &contents));

  // A FIFO would block a plain open().
  const base::FilePath fifo = dir.GetPath().AppendASCII("fifo.txt");
  ASSERT_EQ(mkfifo(fifo.value().c_str(), 0600), 0);
  EXPECT_FALSE(ReadHelperOutputFile(fifo, 1024, &contents));
  EXPECT_FALSE(ReadHelperOutputFile(dir.GetPath(), 1024, &contents));
}
#endif  // BUILDFLAG(IS_POSIX)

#if BUILDFLAG(IS_LINUX)
namespace hsl = helper_sandbox_linux;

// Enough of a classic BPF interpreter for what BuildSeccompProgram() emits.
uint32_t RunFilter(const std::vector<sock_filter>& program,
                   const seccomp_data& data) {
  constexpr uint32_t kArgs = offsetof(seccomp_data, args);
  uint32_t a = 0;
  size_t pc = 0;
  while (pc < program.size()) {
    const sock_filter& insn = program[pc];
    switch (insn.code) {
      case BPF_LD | BPF_W | BPF_ABS:
        if (insn.k == offsetof(seccomp_data, nr)) {
          a = static_cast<uint32_t>(data.nr);
        } else if (insn.k == offsetof(seccomp_data, arch)) {
          a = data.arch;
        } else if (insn.k >= kArgs && insn.k < kArgs + 6 * 8 &&
                   (insn.k - kArgs) % 8 == 0) {
          // The low half of an argument.
          a = static_cast<uint32_t>(
              std::to_array(data.args).at((insn.k - kArgs) / 8));
        } else {
          ADD_FAILURE() << "unexpected load at offset " << insn.k;
          return 0;
        }
        ++pc;
        break;
      case BPF_ALU | BPF_AND | BPF_K:
        a &= insn.k;
        ++pc;
        break;
      case BPF_JMP | BPF_JEQ | BPF_K:
        pc += 1u + (a == insn.k ? insn.jt : insn.jf);
        break;
      case BPF_JMP | BPF_JGE | BPF_K:
        pc += 1u + (a >= insn.k ? insn.jt : insn.jf);
        break;
      case BPF_JMP | BPF_JSET | BPF_K:
        pc += 1u + ((a & insn.k) != 0 ? insn.jt : insn.jf);
        break;
      case BPF_RET | BPF_K:
        return insn.k;
      default:
        ADD_FAILURE() << "unexpected instruction " << insn.code;
        return 0;
    }
  }
  ADD_FAILURE() << "the program has no return on this path";
  return 0;
}

uint32_t NativeArch() {
#if defined(__x86_64__)
  return AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
  return AUDIT_ARCH_AARCH64;
#else
  return 0;
#endif
}

seccomp_data Call(int nr, uint64_t arg0 = 0) {
  seccomp_data data = {};
  data.nr = nr;
  data.arch = NativeArch();
  data.args[0] = arg0;
  return data;
}

// A call with |value| as argument |index| (1 to 3).
seccomp_data CallWithArg(int nr, size_t index, uint64_t value) {
  seccomp_data data = Call(nr);
  switch (index) {
    case 1:
      data.args[1] = value;
      break;
    case 2:
      data.args[2] = value;
      break;
    case 3:
      data.args[3] = value;
      break;
    default:
      ADD_FAILURE() << "argument " << index;
  }
  return data;
}

constexpr uint32_t Errno(int error) {
  return SECCOMP_RET_ERRNO | static_cast<uint32_t>(error);
}

// socket() or socketpair() (|nr|) of |family| and |type|.
seccomp_data SocketCall(int nr, uint64_t family, uint64_t type) {
  seccomp_data data = Call(nr, family);
  data.args[1] = type;
  return data;
}

TEST(HelperSandboxLinuxTest, SeccompProgramShape) {
  const std::vector<sock_filter> program = hsl::BuildSeccompProgram();
  if (hsl::SeccompArchName().empty()) {
    EXPECT_TRUE(program.empty());
    return;
  }
  ASSERT_GE(program.size(), 4u);
  EXPECT_LE(program.size(), static_cast<size_t>(BPF_MAXINSNS));
  // Architecture first, then the system call number.
  EXPECT_EQ(program[0].code, BPF_LD | BPF_W | BPF_ABS);
  EXPECT_EQ(program[0].k, offsetof(seccomp_data, arch));
  EXPECT_EQ(program[1].k, NativeArch());
  EXPECT_EQ(program[2].code, BPF_RET | BPF_K);
  EXPECT_EQ(program[2].k, SECCOMP_RET_KILL_PROCESS);
  EXPECT_EQ(program[3].k, offsetof(seccomp_data, nr));
  // Allow by default.
  EXPECT_EQ(program.back().code, BPF_RET | BPF_K);
  EXPECT_EQ(program.back().k, SECCOMP_RET_ALLOW);
  // Every jump lands inside the program.
  for (size_t i = 0; i < program.size(); ++i) {
    if (BPF_CLASS(program[i].code) == BPF_JMP) {
      EXPECT_LT(i + 1 + program[i].jt, program.size()) << i;
      EXPECT_LT(i + 1 + program[i].jf, program.size()) << i;
    }
  }
}

TEST(HelperSandboxLinuxTest, SeccompVerdicts) {
  const std::vector<sock_filter> program = hsl::BuildSeccompProgram();
  if (program.empty()) {
    GTEST_SKIP() << "no seccomp filter for this architecture";
  }
  // Ordinary work.
  EXPECT_EQ(RunFilter(program, Call(__NR_read)), SECCOMP_RET_ALLOW);
  EXPECT_EQ(RunFilter(program, Call(__NR_write)), SECCOMP_RET_ALLOW);
  EXPECT_EQ(RunFilter(program, Call(__NR_mmap)), SECCOMP_RET_ALLOW);
  EXPECT_EQ(RunFilter(program, Call(__NR_execve)), SECCOMP_RET_ALLOW);
  // Threads (pthread_create) still work; processes don't (fork, posix_spawn
  // and vfork).
  EXPECT_EQ(RunFilter(program, Call(__NR_clone, 0x003d0f00)),
            SECCOMP_RET_ALLOW);
  EXPECT_EQ(RunFilter(program, Call(__NR_clone, SIGCHLD)), Errno(EPERM));
  EXPECT_EQ(
      RunFilter(program, Call(__NR_clone, CLONE_VM | CLONE_VFORK | SIGCHLD)),
      Errno(EPERM));
#if defined(__NR_fork)
  EXPECT_EQ(RunFilter(program, Call(__NR_fork)), Errno(EPERM));
  EXPECT_EQ(RunFilter(program, Call(__NR_vfork)), Errno(EPERM));
#endif

  // Network: only AF_UNIX stream and seqpacket sockets, and no connect().
  for (int nr : {__NR_socket, __NR_socketpair}) {
    for (uint64_t type :
         {uint64_t{SOCK_STREAM}, uint64_t{SOCK_STREAM | SOCK_CLOEXEC},
          uint64_t{SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC}}) {
      EXPECT_EQ(RunFilter(program, SocketCall(nr, AF_UNIX, type)),
                SECCOMP_RET_ALLOW)
          << nr << " " << type;
    }
    // A datagram socket sends to a named one without connect() (the
    // system log, the journal); AF_UNIX takes SOCK_RAW as one too.
    for (uint64_t type :
         {uint64_t{SOCK_DGRAM}, uint64_t{SOCK_DGRAM | SOCK_CLOEXEC},
          uint64_t{SOCK_DGRAM | SOCK_NONBLOCK}, uint64_t{SOCK_RAW},
          uint64_t{SOCK_RDM}, uint64_t{0}}) {
      EXPECT_EQ(RunFilter(program, SocketCall(nr, AF_UNIX, type)),
                Errno(EACCES))
          << nr << " " << type;
    }
    for (int family : {AF_INET, AF_INET6, AF_NETLINK, AF_PACKET, AF_UNSPEC}) {
      EXPECT_EQ(RunFilter(program, SocketCall(nr, family, SOCK_STREAM)),
                Errno(EACCES))
          << nr << " " << family;
    }
  }
  // Only the low 32 bits are the family and the type, as in the kernel.
  EXPECT_EQ(RunFilter(program, SocketCall(__NR_socket, (1ULL << 32) | AF_UNIX,
                                          SOCK_STREAM)),
            SECCOMP_RET_ALLOW);
  EXPECT_EQ(RunFilter(program, SocketCall(__NR_socket, AF_UNIX,
                                          (1ULL << 32) | SOCK_DGRAM)),
            Errno(EACCES));
  EXPECT_EQ(RunFilter(program, Call(__NR_connect)), Errno(EACCES));

  for (int nr : {__NR_io_uring_setup,
                 __NR_io_uring_enter,
                 __NR_io_uring_register,
                 __NR_setsid,
                 __NR_setpgid,
                 __NR_ptrace,
                 __NR_process_vm_readv,
                 __NR_process_vm_writev,
                 __NR_pidfd_getfd,
                 __NR_bpf,
                 __NR_perf_event_open,
                 __NR_userfaultfd,
                 __NR_keyctl,
                 __NR_add_key,
                 __NR_request_key,
                 __NR_unshare,
                 __NR_setns,
                 __NR_mount,
                 __NR_umount2,
                 __NR_pivot_root,
                 __NR_fsopen,
                 __NR_move_mount}) {
    EXPECT_EQ(RunFilter(program, Call(nr)), Errno(EPERM)) << nr;
  }
  // New namespaces through clone(); clone3() hides its flags.
  EXPECT_EQ(RunFilter(program, Call(__NR_clone, 0x10000000 | SIGCHLD)),
            Errno(EPERM));  // CLONE_NEWUSER
  EXPECT_EQ(RunFilter(program, Call(__NR_clone, 0x40000000 | SIGCHLD)),
            Errno(EPERM));  // CLONE_NEWNET
  EXPECT_EQ(RunFilter(program, Call(__NR_clone3)), Errno(ENOSYS));

  // A system call through another ABI is killed.
  seccomp_data foreign = Call(__NR_read);
  foreign.arch = AUDIT_ARCH_I386;
  EXPECT_EQ(RunFilter(program, foreign), SECCOMP_RET_KILL_PROCESS);
#if defined(__x86_64__)
  EXPECT_EQ(RunFilter(program, Call(__NR_socket | 0x40000000, AF_INET)),
            SECCOMP_RET_KILL_PROCESS);  // x32
#endif
}

TEST(HelperSandboxLinuxTest, SeccompKeepsWritesAndMemoryCounted) {
  const std::vector<sock_filter> program = hsl::BuildSeccompProgram();
  if (program.empty()) {
    GTEST_SKIP() << "no seccomp filter for this architecture";
  }
  // Space reserved without writing it, past RLIMIT_FSIZE.
  EXPECT_EQ(RunFilter(program, Call(__NR_fallocate)), Errno(EOPNOTSUPP));
  // Files: opened and created as usual, but not in no directory.
  for (uint64_t flags : {uint64_t{O_RDONLY}, uint64_t{O_WRONLY | O_CREAT},
                         uint64_t{O_RDWR | O_TRUNC | O_CLOEXEC},
                         uint64_t{O_RDONLY | O_DIRECTORY}}) {
    EXPECT_EQ(RunFilter(program, CallWithArg(__NR_openat, 2, flags)),
              SECCOMP_RET_ALLOW)
        << flags;
#if defined(__NR_open)
    EXPECT_EQ(RunFilter(program, CallWithArg(__NR_open, 1, flags)),
              SECCOMP_RET_ALLOW)
        << flags;
#endif
  }
  EXPECT_EQ(RunFilter(program, CallWithArg(__NR_openat, 2, O_TMPFILE | O_RDWR)),
            Errno(EOPNOTSUPP));
#if defined(__NR_open)
  EXPECT_EQ(RunFilter(program, CallWithArg(__NR_open, 1, O_TMPFILE | O_RDWR)),
            Errno(EOPNOTSUPP));
#endif
  EXPECT_EQ(RunFilter(program, Call(437)), Errno(ENOSYS));  // openat2
  for (int nr : {__NR_setxattr, __NR_lsetxattr, __NR_fsetxattr, 463}) {
    EXPECT_EQ(RunFilter(program, Call(nr)), Errno(EOPNOTSUPP)) << nr;
  }
  // Reading extended attributes still works.
  EXPECT_EQ(RunFilter(program, Call(__NR_getxattr)), SECCOMP_RET_ALLOW);

  // Private memory and file mappings, as malloc() and the loader make them.
  for (uint64_t flags :
       {uint64_t{MAP_PRIVATE | MAP_ANONYMOUS},
        uint64_t{MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK},
        uint64_t{MAP_PRIVATE}, uint64_t{MAP_PRIVATE | MAP_FIXED},
        uint64_t{MAP_SHARED}}) {
    EXPECT_EQ(RunFilter(program, CallWithArg(__NR_mmap, 3, flags)),
              SECCOMP_RET_ALLOW)
        << flags;
  }
  // Shared anonymous memory, which RLIMIT_DATA doesn't count.
  for (uint64_t flags : {uint64_t{MAP_SHARED | MAP_ANONYMOUS},
                         uint64_t{MAP_SHARED_VALIDATE | MAP_ANONYMOUS},
                         uint64_t{MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED}}) {
    EXPECT_EQ(RunFilter(program, CallWithArg(__NR_mmap, 3, flags)),
              Errno(EPERM))
        << flags;
  }
  EXPECT_EQ(RunFilter(program, Call(__NR_memfd_create)), Errno(ENOSYS));
  EXPECT_EQ(RunFilter(program, Call(447)), Errno(ENOSYS));  // memfd_secret
  for (int nr : {__NR_shmget, __NR_shmat, __NR_shmctl, __NR_shmdt, __NR_msgget,
                 __NR_msgsnd, __NR_msgrcv, __NR_msgctl, __NR_semget, __NR_semop,
                 __NR_semtimedop, __NR_semctl}) {
    EXPECT_EQ(RunFilter(program, Call(nr)), Errno(EPERM)) << nr;
  }
  EXPECT_EQ(RunFilter(program, Call(__NR_munmap)), SECCOMP_RET_ALLOW);
  EXPECT_EQ(RunFilter(program, Call(__NR_mremap)), SECCOMP_RET_ALLOW);
}

// A call with |arg0| and |arg1|, and |arg2| as its third argument.
seccomp_data CallWithArgs(int nr, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
  seccomp_data data = Call(nr, arg0);
  data.args[1] = arg1;
  data.args[2] = arg2;
  return data;
}

TEST(HelperSandboxLinuxTest, SeccompOwnPidSlots) {
  std::vector<sock_filter> program = hsl::BuildSeccompProgram();
  if (program.empty()) {
    GTEST_SKIP() << "no seccomp filter for this architecture";
  }
  auto count = [&program](uint32_t k) {
    return std::ranges::count_if(program, [k](const sock_filter& insn) {
      return insn.code == (BPF_JMP | BPF_JEQ | BPF_K) && insn.k == k;
    });
  };
  // kill, tgkill, rt_sigqueueinfo, rt_tgsigqueueinfo, fcntl(F_SETOWN),
  // prlimit64; the six scheduling and memory placement calls, setpriority
  // and ioprio_set; kill and fcntl(F_SETOWN) also take the process group.
  EXPECT_EQ(count(hsl::kSeccompOwnPidSlot), 14);
  EXPECT_EQ(count(hsl::kSeccompOwnGroupSlot), 2);
  hsl::SetSeccompOwnPid(4242, &program);
  EXPECT_EQ(count(hsl::kSeccompOwnPidSlot), 0);
  EXPECT_EQ(count(hsl::kSeccompOwnGroupSlot), 0);
  EXPECT_EQ(count(4242), 14);
  EXPECT_EQ(count(static_cast<uint32_t>(-4242)), 2);
}

TEST(HelperSandboxLinuxTest, SeccompKeepsSignalsToTheHelper) {
  std::vector<sock_filter> program = hsl::BuildSeccompProgram();
  if (program.empty()) {
    GTEST_SKIP() << "no seccomp filter for this architecture";
  }
  constexpr int32_t kSelf = 4242;
  constexpr int32_t kOther = 4243;
  auto as_arg = [](int64_t value) { return static_cast<uint64_t>(value); };
  // Before the child put its ID in, only its process group (0) matches.
  EXPECT_EQ(RunFilter(program, Call(__NR_kill, as_arg(kSelf))), Errno(EPERM));
  EXPECT_EQ(RunFilter(program, Call(__NR_kill, 0)), SECCOMP_RET_ALLOW);
  hsl::SetSeccompOwnPid(kSelf, &program);

  // kill(): itself, its process group (0 or minus its ID).
  for (int64_t pid : {int64_t{kSelf}, int64_t{0}, int64_t{-kSelf}}) {
    EXPECT_EQ(RunFilter(program, Call(__NR_kill, as_arg(pid))),
              SECCOMP_RET_ALLOW)
        << pid;
  }
  // Anyone else: another process, another group, every process (-1),
  // init. The kernel reads a 32-bit pid_t, and so does the filter.
  for (int64_t pid : {int64_t{kOther}, int64_t{-kOther}, int64_t{-1},
                      int64_t{1}, int64_t{kSelf} + (int64_t{1} << 32) - 1}) {
    EXPECT_EQ(RunFilter(program, Call(__NR_kill, as_arg(pid))), Errno(EPERM))
        << pid;
  }
  EXPECT_EQ(RunFilter(program, Call(__NR_kill, (uint64_t{1} << 32) | kSelf)),
            SECCOMP_RET_ALLOW);
  // Its own threads (raise(), abort(), pthread_kill()), not another
  // process's.
  EXPECT_EQ(RunFilter(program, CallWithArgs(__NR_tgkill, kSelf, 4250, 6)),
            SECCOMP_RET_ALLOW);
  EXPECT_EQ(RunFilter(program, CallWithArgs(__NR_rt_tgsigqueueinfo, kSelf,
                                            4250, 10)),
            SECCOMP_RET_ALLOW);
  EXPECT_EQ(RunFilter(program, CallWithArgs(__NR_rt_sigqueueinfo, kSelf, 10,
                                            0)),
            SECCOMP_RET_ALLOW);
  for (int nr : {__NR_tgkill, __NR_rt_tgsigqueueinfo, __NR_rt_sigqueueinfo}) {
    for (int64_t pid : {int64_t{kOther}, int64_t{0}, int64_t{-1}}) {
      EXPECT_EQ(RunFilter(program, CallWithArgs(nr, as_arg(pid), kOther, 0)),
                Errno(EPERM))
          << nr << " " << pid;
    }
  }
  // A thread of any process, and a pidfd: never checked, so refused.
  EXPECT_EQ(RunFilter(program, Call(__NR_tkill, kSelf)), Errno(EPERM));
  EXPECT_EQ(RunFilter(program, Call(424)), Errno(EPERM));  // pidfd_send_signal

  // Whom the kernel signals when a file is ready.
  constexpr uint64_t kSetOwner = 8;     // F_SETOWN
  constexpr uint64_t kSetOwnerEx = 15;  // F_SETOWN_EX
  for (int64_t pid : {int64_t{kSelf}, int64_t{-kSelf}, int64_t{0}}) {
    EXPECT_EQ(RunFilter(program, CallWithArgs(__NR_fcntl, 3, kSetOwner,
                                              as_arg(pid))),
              SECCOMP_RET_ALLOW)
        << pid;
  }
  for (int64_t pid : {int64_t{kOther}, int64_t{-kOther}, int64_t{1}}) {
    EXPECT_EQ(RunFilter(program, CallWithArgs(__NR_fcntl, 3, kSetOwner,
                                              as_arg(pid))),
              Errno(EPERM))
        << pid;
  }
  EXPECT_EQ(RunFilter(program, CallWithArgs(__NR_fcntl, 3, kSetOwnerEx, 0)),
            Errno(EPERM));
  // Other commands, whatever their argument.
  for (uint64_t command :
       {uint64_t{F_GETFD}, uint64_t{F_SETFD}, uint64_t{F_GETFL},
        uint64_t{F_SETFL}, uint64_t{F_GETOWN}, uint64_t{F_DUPFD_CLOEXEC}}) {
    EXPECT_EQ(
        RunFilter(program, CallWithArgs(__NR_fcntl, 3, command, kOther)),
        SECCOMP_RET_ALLOW)
        << command;
  }
  EXPECT_EQ(RunFilter(program, CallWithArgs(__NR_ioctl, 3, 0x8901, 0)),
            Errno(EPERM));  // FIOSETOWN
  EXPECT_EQ(RunFilter(program, CallWithArgs(__NR_ioctl, 3, 0x8902, 0)),
            Errno(EPERM));  // SIOCSPGRP
  for (uint64_t request : {uint64_t{0x5401}, uint64_t{0x541B},
                           uint64_t{0x5421}, uint64_t{0x8903}}) {
    // TCGETS, FIONREAD, FIONBIO; SIOCGPGRP (reading the owner).
    EXPECT_EQ(RunFilter(program, CallWithArgs(__NR_ioctl, 3, request, 0)),
              SECCOMP_RET_ALLOW)
        << request;
  }

  // Its own resource limits (0 is itself), not another process's.
  EXPECT_EQ(RunFilter(program, Call(__NR_prlimit64, 0)), SECCOMP_RET_ALLOW);
  EXPECT_EQ(RunFilter(program, Call(__NR_prlimit64, kSelf)),
            SECCOMP_RET_ALLOW);
  EXPECT_EQ(RunFilter(program, Call(__NR_prlimit64, kOther)), Errno(EPERM));
  EXPECT_EQ(RunFilter(program, Call(__NR_getpid)), SECCOMP_RET_ALLOW);
}

TEST(HelperSandboxLinuxTest, SeccompKeepsSchedulingToTheHelper) {
  std::vector<sock_filter> program = hsl::BuildSeccompProgram();
  if (program.empty()) {
    GTEST_SKIP() << "no seccomp filter for this architecture";
  }
  constexpr int32_t kSelf = 4242;
  constexpr int32_t kOther = 4243;
  hsl::SetSeccompOwnPid(kSelf, &program);
  // Its own affinity, policy and memory placement (0 is itself); not
  // another process's, nor a thread's named by its ID.
  for (int nr : {__NR_sched_setaffinity, __NR_sched_setscheduler,
                 __NR_sched_setparam, __NR_sched_setattr, __NR_migrate_pages,
                 __NR_move_pages}) {
    EXPECT_EQ(RunFilter(program, Call(nr, 0)), SECCOMP_RET_ALLOW) << nr;
    EXPECT_EQ(RunFilter(program, Call(nr, kSelf)), SECCOMP_RET_ALLOW) << nr;
    EXPECT_EQ(RunFilter(program, Call(nr, kOther)), Errno(EPERM)) << nr;
    EXPECT_EQ(RunFilter(program, Call(nr, 1)), Errno(EPERM)) << nr;
  }
  // Its own nice value and I/O priority, or its process group's; not
  // another process's or group's, nor every process of the user's.
  constexpr uint64_t kPrioProcess = 0;
  constexpr uint64_t kPrioGroup = 1;
  constexpr uint64_t kPrioUser = 2;
  for (uint64_t which : {kPrioProcess, kPrioGroup}) {
    for (uint64_t who : {uint64_t{0}, uint64_t{kSelf}}) {
      EXPECT_EQ(RunFilter(program, CallWithArgs(__NR_setpriority, which, who,
                                                19)),
                SECCOMP_RET_ALLOW)
          << which << " " << who;
    }
    EXPECT_EQ(
        RunFilter(program, CallWithArgs(__NR_setpriority, which, kOther, 19)),
        Errno(EPERM))
        << which;
  }
  EXPECT_EQ(RunFilter(program, CallWithArgs(__NR_setpriority, kPrioUser, 0,
                                            19)),
            Errno(EPERM));
  EXPECT_EQ(RunFilter(program, CallWithArgs(__NR_setpriority, kPrioUser, 1000,
                                            19)),
            Errno(EPERM));
  constexpr uint64_t kIoprioProcess = 1;
  constexpr uint64_t kIoprioGroup = 2;
  constexpr uint64_t kIoprioUser = 3;
  constexpr uint64_t kIdle = uint64_t{3} << 13;
  for (uint64_t which : {kIoprioProcess, kIoprioGroup}) {
    EXPECT_EQ(
        RunFilter(program, CallWithArgs(__NR_ioprio_set, which, 0, kIdle)),
        SECCOMP_RET_ALLOW)
        << which;
    EXPECT_EQ(
        RunFilter(program, CallWithArgs(__NR_ioprio_set, which, kOther, kIdle)),
        Errno(EPERM))
        << which;
  }
  EXPECT_EQ(
      RunFilter(program, CallWithArgs(__NR_ioprio_set, kIoprioUser, 0, kIdle)),
      Errno(EPERM));
  // Reading them stays allowed.
  for (int nr : {__NR_getpriority, __NR_sched_getaffinity,
                 __NR_sched_getscheduler, __NR_ioprio_get}) {
    EXPECT_EQ(RunFilter(program, Call(nr, kOther)), SECCOMP_RET_ALLOW) << nr;
  }
}

// On the real kernel: a confined child can't lower another process's CPU
// or I/O priority, change its policy or pin it to a CPU (Landlock has no
// rule for these, so the seccomp filter alone decides), and still changes
// its own.
TEST(HelperSandboxLinuxTest, ConfinedHelperKeepsSchedulingToItself) {
  hsl::Policy policy;
  policy.resident = true;
  policy.max_data_bytes = uint64_t{1} << 30;
  hsl::Prepared prepared;
  std::string error;
  ASSERT_TRUE(hsl::Prepare(policy, &prepared, &error)) << error;
  if (prepared.seccomp.empty()) {
    GTEST_SKIP() << "no seccomp filter here";
  }
  // Another process of the same user, outside the sandbox.
  const pid_t other = fork();
  ASSERT_GE(other, 0);
  if (other == 0) {
    pause();
    _exit(0);
  }
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    close(fds[0]);
    hsl::ApplyInChild(prepared);
    uint32_t bits = 0;
    bits |= setpriority(PRIO_PROCESS, other, 19) == 0 ? 1u : 0u;
    // A user without processes, so that nothing is changed if the filter
    // lets it through: the kernel would say ESRCH, the filter says EPERM.
    constexpr id_t kNoSuchUser = 0x7ffffff0;
    bits |= setpriority(PRIO_USER, kNoSuchUser, 19) == -1 && errno == EPERM
                ? 0u
                : 2u;
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(0, &cpus);
    bits |= sched_setaffinity(other, sizeof(cpus), &cpus) == 0 ? 4u : 0u;
    sched_param param = {};
    bits |= sched_setscheduler(other, SCHED_IDLE, &param) == 0 ? 8u : 0u;
    // IOPRIO_WHO_PROCESS, IOPRIO_CLASS_IDLE.
    bits |= syscall(__NR_ioprio_set, 1, other, 3 << 13) == 0 ? 16u : 0u;
    // Its own, which it may change.
    bits |= setpriority(PRIO_PROCESS, 0, 1) == 0 ? 0u : 32u;
    bits |= sched_getaffinity(0, sizeof(cpus), &cpus) == 0 &&
                    sched_setaffinity(0, sizeof(cpus), &cpus) == 0
                ? 0u
                : 64u;
    [[maybe_unused]] const ssize_t written = write(fds[1], &bits, sizeof(bits));
    _exit(0);
  }
  close(fds[1]);
  uint32_t bits = ~0u;
  EXPECT_EQ(read(fds[0], &bits, sizeof(bits)),
            static_cast<ssize_t>(sizeof(bits)));
  close(fds[0]);
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  EXPECT_EQ(bits, 0u) << std::hex << bits;
  errno = 0;
  EXPECT_EQ(getpriority(PRIO_PROCESS, other), getpriority(PRIO_PROCESS, 0));
  EXPECT_EQ(sched_getscheduler(other), SCHED_OTHER);
  kill(other, SIGKILL);
  ASSERT_EQ(waitpid(other, &status, 0), other);
}

// What a confined child could do with signals, as bits: kSignalled* ways
// of reaching its parent that worked, kSelf* ways of signalling itself
// that failed.
enum SignalProbe : uint32_t {
  kSignalledKill = 1u << 0,
  kSignalledTgkill = 1u << 1,
  kSignalledTkill = 1u << 2,
  kSignalledQueue = 1u << 3,
  kSignalledEveryone = 1u << 4,
  kSignalledPidfd = 1u << 5,
  kSignalledOwner = 1u << 6,
  kSignalledLimits = 1u << 7,
  kSelfKillFailed = 1u << 8,
  kSelfGroupFailed = 1u << 9,
  kSelfRaiseFailed = 1u << 10,
  kSelfQueueFailed = 1u << 11,
  kSelfNotReceived = 1u << 12,
};

volatile sig_atomic_t g_probe_received = 0;

void ProbeHandler(int) {
  g_probe_received = g_probe_received + 1;
}

// In the forked child, after ApplyInChild(): raw calls only. |fd| is a
// file of its own.
uint32_t ProbeSignals(pid_t parent, int fd) {
  uint32_t bits = 0;
  struct sigaction action = {};
  action.sa_handler = ProbeHandler;
  sigaction(SIGUSR1, &action, nullptr);
  const pid_t self = getpid();
  bits |= kill(self, SIGUSR1) == 0 ? 0 : kSelfKillFailed;
  bits |= kill(0, SIGUSR1) == 0 && kill(-self, SIGUSR1) == 0 ? 0
                                                              : kSelfGroupFailed;
  bits |= raise(SIGUSR1) == 0 ? 0 : kSelfRaiseFailed;
  union sigval value = {};
  bits |= sigqueue(self, SIGUSR1, value) == 0 ? 0 : kSelfQueueFailed;
  bits |= g_probe_received == 5 ? 0 : kSelfNotReceived;

  // Signal 0: the permission check only.
  bits |= kill(parent, 0) == 0 ? kSignalledKill : 0;
  bits |= syscall(__NR_tgkill, parent, parent, 0) == 0 ? kSignalledTgkill : 0;
  bits |= syscall(__NR_tkill, parent, 0) == 0 ? kSignalledTkill : 0;
  siginfo_t info = {};
  info.si_code = SI_QUEUE;
  info.si_pid = self;
  bits |= syscall(__NR_rt_sigqueueinfo, parent, 0, &info) == 0
              ? kSignalledQueue
              : 0;
  bits |= kill(-1, 0) == 0 ? kSignalledEveryone : 0;
  const long pidfd = syscall(434, parent, 0);  // pidfd_open
  bits |= pidfd >= 0 && syscall(424, pidfd, 0, nullptr, 0) == 0
              ? kSignalledPidfd
              : 0;
  bits |= fcntl(fd, F_SETOWN, parent) == 0 ? kSignalledOwner : 0;
  struct rlimit limit = {};
  bits |= syscall(__NR_prlimit64, parent, RLIMIT_NOFILE, nullptr, &limit) == 0
              ? kSignalledLimits
              : 0;
  return bits;
}

// On a kernel whose Landlock doesn't scope signals (before 6.12, e.g.
// Ubuntu 22.04's and 24.04's): the seccomp filter alone keeps a confined
// child's signals to itself, and abort() still ends it with SIGABRT.
TEST(HelperSandboxLinuxTest, SeccompAloneKeepsSignalsToTheHelper) {
  hsl::Policy policy;
  policy.resident = true;
  policy.max_data_bytes = uint64_t{1} << 30;
  hsl::Prepared prepared;
  std::string error;
  ASSERT_TRUE(hsl::Prepare(policy, &prepared, &error)) << error;
  if (prepared.seccomp.empty()) {
    GTEST_SKIP() << "no seccomp filter here";
  }
  // Without Landlock, as on such a kernel.
  prepared.CloseRuleset();
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  const pid_t parent = getpid();
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    close(fds[0]);
    hsl::ApplyInChild(prepared);
    const uint32_t bits = ProbeSignals(parent, fds[1]);
    [[maybe_unused]] const ssize_t written = write(fds[1], &bits, sizeof(bits));
    abort();
  }
  close(fds[1]);
  uint32_t bits = ~0u;
  EXPECT_EQ(read(fds[0], &bits, sizeof(bits)),
            static_cast<ssize_t>(sizeof(bits)));
  close(fds[0]);
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  EXPECT_EQ(bits, 0u) << std::hex << bits;
  ASSERT_TRUE(WIFSIGNALED(status)) << status;
  EXPECT_EQ(WTERMSIG(status), SIGABRT);
}

TEST(HelperSandboxLinuxTest, PathRules) {
  hsl::Policy policy;
  policy.read_execute = {"/opt/molt/ocr"};
  policy.read_only = {"/opt/molt/ocr/tessdata", "/tmp/molt_ocr.x/input.png"};
  policy.writable = "/tmp/molt_ocr.x";
  const std::vector<hsl::PathRule> rules = hsl::BuildPathRules(policy);
  auto access_of = [&rules](std::string_view path) -> uint64_t {
    for (const hsl::PathRule& rule : rules) {
      if (rule.path == path) {
        return rule.access;
      }
    }
    ADD_FAILURE() << "no rule for " << path;
    return 0;
  };
  constexpr uint64_t kChanges =
      hsl::kFsWriteFile | hsl::kFsRemoveDir | hsl::kFsRemoveFile |
      hsl::kFsMakeChar | hsl::kFsMakeDir | hsl::kFsMakeReg | hsl::kFsMakeSock |
      hsl::kFsMakeFifo | hsl::kFsMakeBlock | hsl::kFsMakeSym | hsl::kFsRefer |
      hsl::kFsTruncate;

  EXPECT_EQ(access_of("/usr"), hsl::kReadExecuteAccess);
  EXPECT_EQ(access_of("/lib"), hsl::kReadExecuteAccess);
  EXPECT_EQ(access_of("/lib64"), hsl::kReadExecuteAccess);
  EXPECT_EQ(access_of("/etc"), hsl::kReadAccess);
  EXPECT_EQ(access_of("/sys/devices/system/cpu"), hsl::kReadAccess);
  EXPECT_EQ(access_of("/dev/urandom"), hsl::kFsReadFile);
  EXPECT_EQ(access_of("/dev/null"),
            hsl::kFsReadFile | hsl::kFsWriteFile | hsl::kFsTruncate);
  EXPECT_EQ(access_of("/opt/molt/ocr"), hsl::kReadExecuteAccess);
  EXPECT_EQ(access_of("/opt/molt/ocr/tessdata"), hsl::kReadAccess);
  EXPECT_EQ(access_of("/tmp/molt_ocr.x/input.png"), hsl::kReadAccess);
  const uint64_t scratch = access_of("/tmp/molt_ocr.x");
  EXPECT_EQ(scratch, hsl::kScratchAccess);
  // Files are read, made, written and truncated there.
  EXPECT_EQ(scratch, hsl::kFsReadFile | hsl::kFsReadDir | hsl::kFsWriteFile |
                         hsl::kFsMakeReg | hsl::kFsTruncate);
  // Nothing written to the scratch directory can be run, and it can't hold
  // links out of it, sockets, FIFOs, devices or directories. Nothing in it
  // is deleted or renamed, so all the helper writes stays where the
  // launcher measures it (helper_limits.h).
  EXPECT_EQ(scratch & (hsl::kFsExecute | hsl::kFsMakeSym | hsl::kFsMakeSock |
                       hsl::kFsMakeFifo | hsl::kFsMakeChar | hsl::kFsMakeBlock |
                       hsl::kFsMakeDir | hsl::kFsRemoveFile |
                       hsl::kFsRemoveDir | hsl::kFsRefer),
            0u);

  for (const hsl::PathRule& rule : rules) {
    // Only the scratch directory must exist; only it and /dev/null change.
    EXPECT_EQ(rule.required, rule.path == policy.writable) << rule.path;
    if (rule.path != policy.writable && rule.path != "/dev/null") {
      EXPECT_EQ(rule.access & kChanges, 0u) << rule.path;
    }
  }
}

TEST(HelperSandboxLinuxTest, HandledAccessFollowsTheAbi) {
  EXPECT_EQ(hsl::HandledFsAccess(0), 0u);
  EXPECT_EQ(hsl::HandledFsAccess(1), 0x1FFFu);
  EXPECT_EQ(hsl::HandledFsAccess(2), 0x3FFFu);
  EXPECT_EQ(hsl::HandledFsAccess(3), 0x7FFFu);
  EXPECT_EQ(hsl::HandledFsAccess(4), 0x7FFFu);
  EXPECT_EQ(hsl::HandledFsAccess(5), 0xFFFFu);
  EXPECT_EQ(hsl::HandledFsAccess(7), 0xFFFFu);
}

TEST(HelperSandboxLinuxTest, PrepareRefusesRelativePaths) {
  hsl::Policy policy;
  policy.writable = "scratch";
  hsl::Prepared prepared;
  std::string error;
  EXPECT_FALSE(hsl::Prepare(policy, &prepared, &error));
  EXPECT_FALSE(error.empty());

  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  policy.writable = dir.GetPath().value();
  policy.read_only = {"tessdata"};
  EXPECT_FALSE(hsl::Prepare(policy, &prepared, &error));
  EXPECT_EQ(prepared.ruleset_fd, -1);
}

// End to end through base::LaunchProcess(), with /bin/sh as the helper.
base::CommandLine Shell(const std::string& script) {
  base::CommandLine cmd(base::FilePath("/bin/sh"));
  cmd.AppendArg("-c");
  cmd.AppendArg(script);
  return cmd;
}

TEST(HelperSandboxLinuxTest, RunsAndCapturesOutput) {
  base::ScopedTempDir scratch;
  ASSERT_TRUE(scratch.CreateUniqueTempDir());
  HelperSandboxPolicy policy;
  policy.writable_dir = scratch.GetPath();
  const HelperRunResult run = RunSandboxedHelper(
      Shell("echo out; echo err >&2; echo text > out.txt; exit 3"), policy,
      base::Seconds(30), 4096);
  EXPECT_TRUE(run.launched);
  EXPECT_FALSE(run.sandbox_failed);
  EXPECT_FALSE(run.timed_out);
  EXPECT_EQ(run.exit_code, 3);
  EXPECT_EQ(run.output, "out\nerr\n");
  std::string text;
  EXPECT_TRUE(ReadHelperOutputFile(scratch.GetPath().AppendASCII("out.txt"),
                                   1024, &text));
  EXPECT_EQ(text, "text\n");
}

TEST(HelperSandboxLinuxTest, KillsAtTheDeadline) {
  base::ScopedTempDir scratch;
  ASSERT_TRUE(scratch.CreateUniqueTempDir());
  HelperSandboxPolicy policy;
  policy.writable_dir = scratch.GetPath();
  const base::TimeTicks start = base::TimeTicks::Now();
  // Also closes its output early. (A shell's builtins only: the sandbox
  // lets the helper start no process.)
  const HelperRunResult run =
      RunSandboxedHelper(Shell("exec >/dev/null 2>&1; while :; do :; done"),
                         policy, base::Milliseconds(300), 4096);
  EXPECT_TRUE(run.launched);
  EXPECT_TRUE(run.timed_out);
  EXPECT_EQ(run.error, "timed out after 300 ms");
  EXPECT_LT(base::TimeTicks::Now() - start, base::Seconds(10));
}

TEST(HelperSandboxLinuxTest, CapsOutput) {
  base::ScopedTempDir scratch;
  ASSERT_TRUE(scratch.CreateUniqueTempDir());
  HelperSandboxPolicy policy;
  policy.writable_dir = scratch.GetPath();
  // Once the pipe is closed, echo fails (EPIPE, or SIGPIPE ends the shell).
  const base::TimeTicks start = base::TimeTicks::Now();
  const HelperRunResult run =
      RunSandboxedHelper(Shell("while echo 0123456789abcdef; do :; done"),
                         policy, base::Seconds(30), 1000);
  EXPECT_TRUE(run.launched);
  EXPECT_TRUE(run.output_truncated);
  EXPECT_EQ(run.output.size(), 1000u);
  EXPECT_FALSE(run.timed_out);
  EXPECT_LT(base::TimeTicks::Now() - start, base::Seconds(10));
}

TEST(HelperSandboxLinuxTest, WritesNoFileLargerThanTheLimit) {
  base::ScopedTempDir scratch;
  ASSERT_TRUE(scratch.CreateUniqueTempDir());
  HelperSandboxPolicy policy;
  policy.writable_dir = scratch.GetPath();
  // 1 KiB lines (shell builtins only), up to 4 MiB past the limit.
  const uint64_t lines = (hsl::kMaxHelperFileBytes >> 10) + 4096;
  const HelperRunResult run = RunSandboxedHelper(
      Shell("i=0; while [ $i -lt " + base::NumberToString(lines) +
            " ]; do printf '%1023s\\n' x || exit 7; i=$((i+1)); done > big; "
            "echo done"),
      policy, base::Seconds(60), 4096);
  ASSERT_TRUE(run.launched) << run.error;
  EXPECT_FALSE(run.timed_out);
  // SIGXFSZ ended the shell, or its write failed (EFBIG): never "done".
  EXPECT_EQ(run.output.find("done"), std::string::npos);
  // Never larger than the limit. (It is the limit unless the launcher's
  // check, which also counts space the file system reserved, ended it
  // first.)
  const std::optional<int64_t> size =
      base::GetFileSize(scratch.GetPath().AppendASCII("big"));
  ASSERT_TRUE(size.has_value());
  EXPECT_LE(static_cast<uint64_t>(*size), hsl::kMaxHelperFileBytes);
  EXPECT_GT(static_cast<uint64_t>(*size), hsl::kMaxHelperFileBytes / 2);
  // And the run says so.
  EXPECT_TRUE(run.limit_exceeded);
  EXPECT_EQ(run.error, "it wrote 64 MiB or more to its scratch directory");
}

TEST(HelperSandboxLinuxTest, ConfinesWhenTheKernelCan) {
  HelperScratchDir scratch;
  base::ScopedTempDir elsewhere;
  ASSERT_TRUE(scratch.Create("molt_test"));
  ASSERT_TRUE(elsewhere.CreateUniqueTempDir());
  const base::FilePath secret = elsewhere.GetPath().AppendASCII("secret");
  // A whole line, which the shell's read builtin reads.
  ASSERT_TRUE(base::WriteFile(secret, "secret\n"));
  const HelperSandboxPolicy policy = scratch.MakePolicy();
  const HelperRunResult run =
      RunSandboxedHelper(Shell("read line < '" + secret.value() +
                               "' && echo READ; "
                               "echo x > '" +
                               elsewhere.GetPath().value() +
                               "/new' && "
                               "echo WROTE; echo ok"),
                         policy, base::Seconds(30), 4096);
  ASSERT_TRUE(run.launched) << run.error;
  EXPECT_NE(run.output.find("ok"), std::string::npos);
  if (hsl::LandlockAbi() > 0) {
    EXPECT_EQ(run.output.find("READ"), std::string::npos);
    EXPECT_EQ(run.output.find("WROTE"), std::string::npos);
    EXPECT_FALSE(base::PathExists(elsewhere.GetPath().AppendASCII("new")));
  }
}
#endif  // BUILDFLAG(IS_LINUX)

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)
// The confinement and the limits (helper_limits.h) with a stand-in helper
// (helper_sandbox_test_helper_posix.cc) under the real sandbox.
class HelperSandboxLimitsTest : public testing::Test {
 protected:
  void SetUp() override {
    base::FilePath exe_dir;
    ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &exe_dir));
    helper_ = exe_dir.AppendASCII("molt_helper_sandbox_test_helper");
    ASSERT_TRUE(base::PathExists(helper_)) << helper_;
    ASSERT_TRUE(scratch_.Create("molt_test"));
  }

  HelperRunResult Run(const std::vector<std::string>& args,
                      base::TimeDelta timeout = base::Seconds(30)) {
    base::CommandLine cmd(helper_);
    for (const std::string& arg : args) {
      cmd.AppendArg(arg);
    }
    return RunSandboxedHelper(cmd, scratch_.MakePolicy(), timeout, 4096);
  }

  // What a run within the limits printed.
  std::string Output(const std::vector<std::string>& args) {
    const HelperRunResult run = Run(args);
    EXPECT_TRUE(run.launched) << run.error;
    EXPECT_FALSE(run.limit_exceeded) << run.error;
    return run.output;
  }

  // A run that is stopped past a limit, and why.
  std::string Stopped(const std::vector<std::string>& args) {
    const HelperRunResult run = Run(args);
    EXPECT_TRUE(run.launched) << run.error;
    EXPECT_FALSE(run.timed_out);
    EXPECT_TRUE(run.limit_exceeded) << run.output;
    return run.error;
  }

  std::string In(std::string_view name) const {
    return scratch_.path().AppendASCII(name).value();
  }

  helper_limits::ScratchUsage Usage() const {
    return helper_limits::MeasureScratch(scratch_.path().value(), 10000);
  }

  base::FilePath helper_;
  HelperScratchDir scratch_;
};

TEST_F(HelperSandboxLimitsTest, RunsAndCapturesOutput) {
  const HelperRunResult run = Run({"echo", "hello"});
  ASSERT_TRUE(run.launched) << run.error;
  EXPECT_FALSE(run.limit_exceeded);
  EXPECT_EQ(run.exit_code, 3);
  EXPECT_EQ(run.output, "hello\nhello\n");
}

TEST_F(HelperSandboxLimitsTest, WritesFilesAndNothingElse) {
#if BUILDFLAG(IS_LINUX)
  if (hsl::LandlockAbi() == 0) {
    GTEST_SKIP() << "no Landlock: files are not confined";
  }
#endif
  EXPECT_EQ(Output({"write", In("out.txt"), "1"}), "WROTE\n");
  std::string text;
  EXPECT_TRUE(
      ReadHelperOutputFile(scratch_.path().AppendASCII("out.txt"), 16, &text));
  EXPECT_EQ(text, std::string(16, 'x'));
  // A file outside the scratch directory, which the helper may neither read
  // nor reach: a hard link to it would give the helper a write handle to it
  // and so a way out of the one place it may write, so the link is refused
  // too (the scratch directory stays the only place the launcher measures).
  base::ScopedTempDir elsewhere;
  ASSERT_TRUE(elsewhere.CreateUniqueTempDir());
  const base::FilePath outside = elsewhere.GetPath().AppendASCII("outside");
  ASSERT_TRUE(base::WriteFile(outside, "outside\n"));
  // What it wrote stays: not deleted, renamed or moved into a directory, nor
  // a hard link to a file outside the scratch directory made.
  for (const std::vector<std::string>& args :
       std::vector<std::vector<std::string>>{
           {"delete", In("out.txt")},
           {"rename", In("out.txt"), In("moved.txt")},
           {"mkdir", In("dir")},
           {"symlink", "/etc/hosts", In("link")},
           {"link", outside.value(), In("outlink")},
           {"xattr", In("out.txt")}}) {
    const std::string output = Output(args);
    EXPECT_TRUE(output.starts_with("DENIED")) << args[0] << ": " << output;
  }
  EXPECT_TRUE(base::PathExists(scratch_.path().AppendASCII("out.txt")));
  EXPECT_FALSE(base::PathExists(scratch_.path().AppendASCII("moved.txt")));
  EXPECT_FALSE(base::PathExists(scratch_.path().AppendASCII("dir")));
  EXPECT_FALSE(base::PathExists(scratch_.path().AppendASCII("outlink")));
  // The outside file is untouched.
  std::string outside_text;
  EXPECT_TRUE(base::ReadFileToString(outside, &outside_text));
  EXPECT_EQ(outside_text, "outside\n");
}

TEST_F(HelperSandboxLimitsTest, StopsAHelperThatWritesTooMuch) {
  // 16 MiB files (each below Linux's file size limit) up to 128 MiB.
  const helper_limits::ScratchUsage before = Usage();
  EXPECT_EQ(Stopped({"spread", scratch_.path().value(), "8", "16"}),
            "it wrote 64 MiB or more to its scratch directory");
  // Stopped soon after: well below the 128 MiB it would have written.
  const helper_limits::ScratchUsage after = Usage();
  EXPECT_GE(after.bytes, before.bytes + helper_limits::kMaxHelperWriteBytes);
  EXPECT_LT(after.bytes,
            before.bytes + 2 * helper_limits::kMaxHelperWriteBytes - 1);
}

TEST_F(HelperSandboxLimitsTest, StopsAHelperThatMakesTooManyFiles) {
  EXPECT_EQ(Stopped({"files", scratch_.path().value(), "100"}),
            "it made more than 64 files in its scratch directory");
}

TEST_F(HelperSandboxLimitsTest, CountsWhatItLeftWhenItExited) {
  // Written at once, and it exits: if the run's checks missed it, the one
  // after it exited doesn't.
  const HelperRunResult run =
      Run({"spread", scratch_.path().value(), "2", "40"});
  ASSERT_TRUE(run.launched) << run.error;
  EXPECT_TRUE(run.limit_exceeded);
  EXPECT_EQ(DescribeHelperRunFailure(run, "test", "x"),
            "test was stopped: it wrote 64 MiB or more to its scratch "
            "directory");
}

TEST_F(HelperSandboxLimitsTest, StartsNoOtherProcess) {
  // A child would be a process the launcher doesn't count or kill.
  const std::string output = Output({"fork"});
  EXPECT_TRUE(output.starts_with("DENIED")) << output;
}

TEST_F(HelperSandboxLimitsTest, SendsNoDatagramToANamedSocket) {
  // A named UNIX socket outside the scratch directory, as the system log's
  // is: a datagram reaches it without connect().
  base::ScopedTempDir elsewhere;
  ASSERT_TRUE(elsewhere.CreateUniqueTempDir());
  const std::string path = elsewhere.GetPath().AppendASCII("log").value();
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    GTEST_SKIP() << "the temporary directory is too deep for a socket's name";
  }
  std::ranges::copy(path, std::begin(address.sun_path));
  const int listener = socket(AF_UNIX, SOCK_DGRAM, 0);
  ASSERT_GE(listener, 0);
  ASSERT_EQ(
      bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)),
      0);
  const std::string output = Output({"dgram", path});
  char received[16] = {};
  const ssize_t got =
      recv(listener, received, sizeof(received), MSG_DONTWAIT);
  close(listener);
  EXPECT_TRUE(output.starts_with("DENIED")) << output;
  EXPECT_LT(got, 0);
}

#if BUILDFLAG(IS_LINUX)
// The ways the stand-in helper tries to signal its parent (the test).
constexpr std::string_view kSignalWays[] = {
    "kill",       "everyone", "tgkill", "tkill", "sigqueue",
    "tgsigqueue", "pidfd",    "owner",  "limits"};

// Signals go to the helper itself only, on every kernel (the seccomp
// filter; Landlock's scoping too from ABI 6).
TEST_F(HelperSandboxLimitsTest, SignalsOnlyItself) {
  const std::string output = Output({"signals"});
  EXPECT_TRUE(output.starts_with("SELF 5 5\n")) << output;
  for (std::string_view way : kSignalWays) {
    EXPECT_NE(output.find(std::string(way) + " DENIED "), std::string::npos)
        << way << ": " << output;
  }
  EXPECT_EQ(output.find("SENT"), std::string::npos) << output;
}

// abort() (raise(SIGABRT), tgkill()) still reaches its own handler.
TEST_F(HelperSandboxLimitsTest, AbortStillWorks) {
  const HelperRunResult run = Run({"abort"});
  ASSERT_TRUE(run.launched) << run.error;
  EXPECT_EQ(run.output, "ABORTED\n");
  EXPECT_EQ(run.exit_code, 0);
}
#endif  // BUILDFLAG(IS_LINUX)

TEST_F(HelperSandboxLimitsTest, AnInputDoesntCount) {
  // A 100 MiB input the browser wrote, and a small output.
  ASSERT_TRUE(base::WriteFile(scratch_.path().AppendASCII("input.pnm"),
                              std::string(100 << 20, 'p')));
  EXPECT_EQ(Output({"write", In("out.txt"), "8"}), "WROTE\n");
}

#if BUILDFLAG(IS_LINUX)
TEST_F(HelperSandboxLimitsTest, ResourceLimits) {
  const std::string output = Output({"limits"});
  EXPECT_NE(output.find("FSIZE 67108864 67108864\n"), std::string::npos)
      << output;
  EXPECT_NE(output.find("CORE 0 0\n"), std::string::npos) << output;
  EXPECT_NE(output.find("DATA 4294967296 4294967296\n"), std::string::npos)
      << output;
  EXPECT_NE(output.find("NOFILE 256 256\n"), std::string::npos) << output;
}

TEST_F(HelperSandboxLimitsTest, MemoryLimit) {
  EXPECT_EQ(Output({"alloc", "256"}), "ALLOCATED\n");
  // Past RLIMIT_DATA (refused when mapped, not when touched).
  EXPECT_EQ(Output({"reserve", "3072"}), "RESERVED\n");
  EXPECT_TRUE(Output({"reserve", "5120"}).starts_with("FAILED"));
  // Memory RLIMIT_DATA wouldn't count is refused.
  for (const std::vector<std::string>& args :
       std::vector<std::vector<std::string>>{
           {"shared", "64"}, {"memfd"}, {"shm", "64"}}) {
    const std::string output = Output(args);
    EXPECT_TRUE(output.starts_with("DENIED")) << args[0] << ": " << output;
  }
}

TEST(HelperSandboxLinuxLimitsTest, AScratchDirectoryItLockedIsStillDeleted) {
  base::FilePath exe_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &exe_dir));
  base::FilePath path;
  {
    HelperScratchDir scratch;
    ASSERT_TRUE(scratch.Create("molt_test"));
    path = scratch.path();
    // Landlock doesn't cover chmod: the helper can take the browser's own
    // access away.
    base::CommandLine cmd(
        exe_dir.AppendASCII("molt_helper_sandbox_test_helper"));
    cmd.AppendArg("lock");
    cmd.AppendArg(path.value());
    const HelperRunResult run =
        RunSandboxedHelper(cmd, scratch.MakePolicy(), base::Seconds(30), 4096);
    ASSERT_TRUE(run.launched) << run.error;
    if (geteuid() != 0) {
      // What it wrote can't be measured any more: it is stopped.
      EXPECT_TRUE(run.limit_exceeded);
      EXPECT_EQ(run.error,
                "what it wrote to its scratch directory couldn't be measured");
    }
  }
  EXPECT_FALSE(base::PathExists(path));
}

TEST_F(HelperSandboxLimitsTest, NothingHiddenFromTheCheck) {
  if (hsl::BuildSeccompProgram().empty()) {
    GTEST_SKIP() << "no seccomp filter for this architecture";
  }
  // Space reserved without writing it, a file in no directory.
  std::string output = Output({"preallocate", In("big"), "1024"});
  EXPECT_TRUE(output.starts_with("DENIED")) << output;
  EXPECT_LT(Usage().bytes, 1u << 20);
  output = Output({"tmpfile", scratch_.path().value()});
  EXPECT_TRUE(output.starts_with("DENIED")) << output;
}
#endif  // BUILDFLAG(IS_LINUX)

#if BUILDFLAG(IS_MAC)
TEST_F(HelperSandboxLimitsTest, StopsAHelperThatReservesSpace) {
  // F_PREALLOCATE reserves space without writing it; the next check sees it.
  EXPECT_EQ(Stopped({"preallocate", In("big"), "1024"}),
            "it wrote 64 MiB or more to its scratch directory");
}

TEST_F(HelperSandboxLimitsTest, CountsAResourceFork) {
  EXPECT_EQ(Output({"write", In("out.txt"), "1"}), "WROTE\n");
  const HelperRunResult run = Run({"rsrc", In("out.txt"), "100"});
  ASSERT_TRUE(run.launched) << run.error;
  // Either the sandbox refused it, or the check stopped it.
  EXPECT_TRUE(run.limit_exceeded || run.output.starts_with("FAILED"))
      << run.output;
}

TEST_F(HelperSandboxLimitsTest, StopsAHelperThatUsesTooMuchMemory) {
  EXPECT_EQ(Output({"alloc", "256"}), "ALLOCATED\n");
  EXPECT_EQ(Stopped({"alloc", "6144"}), "it used more than 4 GiB of memory");
}
#endif  // BUILDFLAG(IS_MAC)
#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)

#if BUILDFLAG(IS_WIN)
namespace hsw = helper_sandbox_win;

std::vector<base::win::Sid> HelperFilesCapability() {
  std::vector<base::win::Sid> sids;
  sids.push_back(*base::win::Sid::FromSddlString(
      base::ASCIIToWide(hsw::kHelperFilesCapabilitySid)));
  return sids;
}

// The confinement Windows really applies, with a stand-in helper
// (helper_sandbox_test_helper_win.cc) in a folder set up as the installer
// sets up <install>\ocr and <install>\whisper.
class HelperSandboxWinTest : public testing::Test {
 protected:
  void SetUp() override {
    base::FilePath exe_dir;
    ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &exe_dir));
    const base::FilePath source =
        exe_dir.AppendASCII("molt_helper_sandbox_test_helper.exe");
    ASSERT_TRUE(base::PathExists(source)) << source;
    // moltbrowser.nsi: icacls <dir> /grant *<capability>:(OI)(CI)(RX).
    ASSERT_TRUE(bundle_.CreateUniqueTempDir());
    ASSERT_TRUE(base::win::GrantAccessToPath(
        bundle_.GetPath(), HelperFilesCapability(),
        FILE_GENERIC_READ | FILE_GENERIC_EXECUTE,
        OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE));
    helper_ = bundle_.GetPath().AppendASCII("helper.exe");
    ASSERT_TRUE(base::CopyFile(source, helper_));
    ASSERT_TRUE(scratch_.Create("molt_test"));
    // Somewhere else in the user's profile (%TEMP% is in it).
    ASSERT_TRUE(elsewhere_.CreateUniqueTempDir());
  }

  HelperRunResult Run(const std::vector<std::wstring>& args,
                      base::TimeDelta timeout = base::Seconds(30)) {
    base::CommandLine cmd(helper_);
    for (const std::wstring& arg : args) {
      cmd.AppendArgNative(arg);
    }
    HelperSandboxPolicy policy = scratch_.MakePolicy();
    policy.read_only_paths.push_back(bundle_.GetPath());
    return RunSandboxedHelper(cmd, policy, timeout, 4096);
  }

  // What a run printed; empty (and a failure) if it didn't run.
  std::string Output(const std::vector<std::wstring>& args) {
    const HelperRunResult run = Run(args);
    EXPECT_TRUE(run.launched) << run.error;
    return run.output;
  }

  base::ScopedTempDir bundle_;
  base::ScopedTempDir elsewhere_;
  base::FilePath helper_;
  HelperScratchDir scratch_;
};

TEST_F(HelperSandboxWinTest, TheCapabilityIsWindows) {
  EXPECT_EQ(base::win::Sid::FromNamedCapability(
                base::ASCIIToWide(hsw::kHelperFilesCapability)),
            HelperFilesCapability()[0]);
}

TEST_F(HelperSandboxWinTest, RunsAndCapturesOutput) {
  const HelperRunResult run = Run({L"echo", L"hello"});
  ASSERT_TRUE(run.launched) << run.error;
  EXPECT_FALSE(run.sandbox_failed);
  EXPECT_FALSE(run.timed_out);
  EXPECT_EQ(run.exit_code, 3);
  // stdout, then stderr: one pipe.
  EXPECT_EQ(run.output, "hello\nhello\n");
}

TEST_F(HelperSandboxWinTest, ReadsItsOwnFilesOnly) {
  const base::FilePath input = scratch_.path().AppendASCII("input.txt");
  ASSERT_TRUE(base::WriteFile(input, "scratch text"));
  EXPECT_EQ(Output({L"read", input.value()}), "READ scratch text\n");
  // Its own folder, through the capability.
  EXPECT_TRUE(Output({L"read", helper_.value()}).starts_with("READ "));
  // A file of the user's anywhere else.
  const base::FilePath secret = elsewhere_.GetPath().AppendASCII("secret.txt");
  ASSERT_TRUE(base::WriteFile(secret, "secret"));
  const std::string denied = Output({L"read", secret.value()});
  EXPECT_TRUE(denied.starts_with("DENIED")) << denied;
  EXPECT_EQ(denied.find("secret"), std::string::npos);
}

TEST_F(HelperSandboxWinTest, WritesItsScratchDirectoryOnly) {
  const base::FilePath out = scratch_.path().AppendASCII("out.txt");
  EXPECT_EQ(Output({L"write", out.value()}), "WROTE\n");
  std::string text;
  EXPECT_TRUE(ReadHelperOutputFile(out, 1024, &text));
  EXPECT_EQ(text, "written\n");
  for (const base::FilePath& path :
       {elsewhere_.GetPath().AppendASCII("new.txt"),
        bundle_.GetPath().AppendASCII("new.txt")}) {
    const std::string output = Output({L"write", path.value()});
    EXPECT_TRUE(output.starts_with("DENIED")) << path << ": " << output;
    EXPECT_FALSE(base::PathExists(path)) << path;
  }
}

TEST_F(HelperSandboxWinTest, WritesFilesAndNothingElse) {
  const base::FilePath out = scratch_.path().AppendASCII("out.txt");
  EXPECT_EQ(Output({L"write", out.value()}), "WROTE\n");
  // What it wrote stays: not deleted, renamed or moved into a directory.
  for (const std::vector<std::wstring>& args :
       std::vector<std::vector<std::wstring>>{
           {L"delete", out.value()},
           {L"rename", out.value(),
            scratch_.path().AppendASCII("moved.txt").value()},
           {L"mkdir", scratch_.path().AppendASCII("dir").value()}}) {
    const std::string output = Output(args);
    EXPECT_TRUE(output.starts_with("DENIED")) << args[0] << ": " << output;
  }
  EXPECT_TRUE(base::PathExists(out));
  EXPECT_FALSE(base::PathExists(scratch_.path().AppendASCII("moved.txt")));
  EXPECT_FALSE(base::PathExists(scratch_.path().AppendASCII("dir")));
}

TEST_F(HelperSandboxWinTest, StopsAHelperThatWritesTooMuch) {
  // 16 MiB files up to 128 MiB.
  const HelperRunResult run =
      Run({L"spread", scratch_.path().value(), L"8", L"16"});
  ASSERT_TRUE(run.launched) << run.error;
  EXPECT_FALSE(run.timed_out);
  EXPECT_TRUE(run.limit_exceeded) << run.output;
  EXPECT_EQ(run.error, "it wrote 64 MiB or more to its scratch directory");
  // Stopped soon after: well below the 128 MiB it would have written.
  EXPECT_LT(helper_limits::MeasureScratch(scratch_.path().value(), 10000).bytes,
            2 * helper_limits::kMaxHelperWriteBytes - 1);
}

TEST_F(HelperSandboxWinTest, StopsAHelperThatMakesTooManyFiles) {
  const HelperRunResult run = Run({L"files", scratch_.path().value(), L"100"});
  ASSERT_TRUE(run.launched) << run.error;
  EXPECT_TRUE(run.limit_exceeded) << run.output;
  EXPECT_EQ(run.error, "it made more than 64 files in its scratch directory");
}

TEST_F(HelperSandboxWinTest, CountsAStream) {
  const base::FilePath out = scratch_.path().AppendASCII("out.txt");
  EXPECT_EQ(Output({L"write", out.value()}), "WROTE\n");
  const HelperRunResult run = Run({L"stream", out.value(), L"100"});
  ASSERT_TRUE(run.launched) << run.error;
  // Either the sandbox refused the stream, or the check stopped it.
  EXPECT_TRUE(run.limit_exceeded || run.output.starts_with("FAILED"))
      << run.output;
}

TEST_F(HelperSandboxWinTest, StopsAHelperThatReservesSpace) {
  // SetEndOfFile() gives a file space without writing it.
  const HelperRunResult run =
      Run({L"extend", scratch_.path().AppendASCII("big").value(), L"1024"});
  ASSERT_TRUE(run.launched) << run.error;
  EXPECT_TRUE(run.limit_exceeded) << run.output;
  EXPECT_EQ(run.error, "it wrote 64 MiB or more to its scratch directory");
}

TEST_F(HelperSandboxWinTest, CantConnect) {
  WSADATA data;
  ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
  const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_NE(listener, INVALID_SOCKET);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(
      bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)),
      0);
  ASSERT_EQ(listen(listener, 1), 0);
  int length = sizeof(address);
  ASSERT_EQ(
      getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);

  const std::string output =
      Output({L"connect", base::NumberToWString(ntohs(address.sin_port))});
  EXPECT_TRUE(output.starts_with("REFUSED")) << output;
  // Nothing reached the listener.
  u_long non_blocking = 1;
  ASSERT_EQ(ioctlsocket(listener, FIONBIO, &non_blocking), 0);
  EXPECT_EQ(accept(listener, nullptr, nullptr), INVALID_SOCKET);
  closesocket(listener);
  WSACleanup();
}

TEST_F(HelperSandboxWinTest, CantStartAProcess) {
  base::FilePath system;
  ASSERT_TRUE(base::PathService::Get(base::DIR_SYSTEM, &system));
  for (const base::FilePath& program :
       {system.AppendASCII("whoami.exe"), helper_}) {
    const std::string output = Output({L"spawn", program.value()});
    EXPECT_TRUE(output.starts_with("DENIED")) << program << ": " << output;
  }
}

TEST_F(HelperSandboxWinTest, MemoryLimit) {
  EXPECT_EQ(Output({L"alloc", L"64"}), "ALLOCATED\n");
  // More than the job's limit.
  EXPECT_TRUE(Output({L"alloc", L"5120"}).starts_with("FAILED"));
}

TEST_F(HelperSandboxWinTest, OnlyItsOwnEnvironment) {
  const std::string output = Output({L"env"});
  EXPECT_NE(output.find("TEMP=" + scratch_.path().AsUTF8Unsafe() + "\n"),
            std::string::npos)
      << output;
  for (const char* gone :
       {"USERPROFILE=", "APPDATA=", "LOCALAPPDATA=", "USERNAME="}) {
    EXPECT_EQ(output.find(gone), std::string::npos) << gone;
  }
}

TEST_F(HelperSandboxWinTest, KillsAtTheDeadline) {
  const base::TimeTicks start = base::TimeTicks::Now();
  const HelperRunResult run = Run({L"spin"}, base::Milliseconds(300));
  EXPECT_TRUE(run.launched);
  EXPECT_TRUE(run.timed_out);
  EXPECT_EQ(run.error, "timed out after 300 ms");
  EXPECT_LT(base::TimeTicks::Now() - start, base::Seconds(10));
}

TEST_F(HelperSandboxWinTest, ThePortableZipCantRunIt) {
  // A folder without the installer's grant, as a ZIP unpacks to.
  base::ScopedTempDir portable;
  ASSERT_TRUE(portable.CreateUniqueTempDir());
  const base::FilePath helper = portable.GetPath().AppendASCII("helper.exe");
  ASSERT_TRUE(base::CopyFile(helper_, helper));
  base::CommandLine cmd(helper);
  cmd.AppendArg("echo");
  cmd.AppendArg("ran");
  const HelperRunResult run =
      RunSandboxedHelper(cmd, scratch_.MakePolicy(), base::Seconds(30), 4096);
  EXPECT_TRUE(run.sandbox_failed);
  EXPECT_FALSE(run.launched);
  EXPECT_TRUE(run.output.empty());
  EXPECT_NE(run.error.find("installer"), std::string::npos) << run.error;
  EXPECT_NE(run.error.find(helper.AsUTF8Unsafe()), std::string::npos);
}

TEST_F(HelperSandboxWinTest, OnlyItsOwnScratchDirectory) {
  base::CommandLine cmd(helper_);
  cmd.AppendArg("echo");
  cmd.AppendArg("ran");
  // A directory not made for a helper.
  HelperSandboxPolicy policy;
  policy.writable_dir = elsewhere_.GetPath();
  HelperRunResult run =
      RunSandboxedHelper(cmd, policy, base::Seconds(30), 4096);
  EXPECT_TRUE(run.sandbox_failed);
  EXPECT_FALSE(run.launched);
  // Another run's: this run's AppContainer may not write there.
  HelperScratchDir other;
  ASSERT_TRUE(other.Create("molt_test"));
  policy = scratch_.MakePolicy();
  policy.writable_dir = other.path();
  run = RunSandboxedHelper(cmd, policy, base::Seconds(30), 4096);
  EXPECT_TRUE(run.sandbox_failed);
  EXPECT_NE(run.error.find("can't write to"), std::string::npos) << run.error;
}

TEST_F(HelperSandboxWinTest, TheScratchDirectoryIsTheRunsAlone) {
  std::optional<base::win::SecurityDescriptor> descriptor =
      base::win::SecurityDescriptor::FromFile(
          scratch_.path(),
          DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION);
  ASSERT_TRUE(descriptor);
  EXPECT_TRUE(descriptor->dacl_protected());
  const std::optional<std::wstring> sddl = descriptor->ToSddl(
      DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION);
  ASSERT_TRUE(sddl);
  EXPECT_NE(
      sddl->find(base::ASCIIToWide(scratch_.MakePolicy().app_container_sid)),
      std::wstring::npos)
      << *sddl;
  EXPECT_NE(sddl->find(L"(ML;OICI;NW;;;LW)"), std::wstring::npos) << *sddl;
  EXPECT_EQ(sddl->find(L";WD)"), std::wstring::npos) << *sddl;
  EXPECT_EQ(sddl->find(L";AC)"), std::wstring::npos) << *sddl;
}

TEST_F(HelperSandboxWinTest, ReadsADataDirectory) {
  const base::FilePath data = elsewhere_.GetPath().AppendASCII("models");
  ASSERT_TRUE(CreateHelperDataDirectory(data));
  const base::FilePath model = data.AppendASCII("model.bin");
  ASSERT_TRUE(base::WriteFile(model, "weights"));
  base::CommandLine cmd(helper_);
  cmd.AppendArg("read");
  cmd.AppendArgPath(model);
  HelperSandboxPolicy policy = scratch_.MakePolicy();
  policy.read_only_paths.push_back(model);
  const HelperRunResult run =
      RunSandboxedHelper(cmd, policy, base::Seconds(30), 4096);
  ASSERT_TRUE(run.launched) << run.error;
  EXPECT_EQ(run.output, "READ weights\n");
  // Read only.
  const std::string output =
      Output({L"write", data.AppendASCII("new.bin").value()});
  EXPECT_TRUE(output.starts_with("DENIED")) << output;

  // A model anywhere else is refused before the helper starts.
  const base::FilePath stray = elsewhere_.GetPath().AppendASCII("stray.bin");
  ASSERT_TRUE(base::WriteFile(stray, "weights"));
  policy.read_only_paths.back() = stray;
  const HelperRunResult refused =
      RunSandboxedHelper(cmd, policy, base::Seconds(30), 4096);
  EXPECT_TRUE(refused.sandbox_failed);
  EXPECT_NE(refused.error.find("can't read"), std::string::npos)
      << refused.error;
}

TEST(HelperSandboxWinOutputTest, ReadsOnlyPlainFilesTheHelperMade) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath text = dir.GetPath().AppendASCII("input.txt");
  ASSERT_TRUE(base::WriteFile(text, "recognized text"));
  std::string contents;
  EXPECT_TRUE(ReadHelperOutputFile(text, 1024, &contents));
  EXPECT_EQ(contents, "recognized text");
  EXPECT_TRUE(ReadHelperOutputFile(text, 4, &contents));
  EXPECT_EQ(contents, "reco");
  EXPECT_FALSE(ReadHelperOutputFile(dir.GetPath().AppendASCII("missing"), 1024,
                                    &contents));
  EXPECT_FALSE(ReadHelperOutputFile(dir.GetPath(), 1024, &contents));
  // A hard link to a file outside the scratch directory.
  const base::FilePath secret = dir.GetPath().AppendASCII("secret");
  ASSERT_TRUE(base::WriteFile(secret, "secret"));
  const base::FilePath hardlink = dir.GetPath().AppendASCII("hardlink.txt");
  ASSERT_TRUE(base::CreateWinHardLink(hardlink, secret));
  EXPECT_FALSE(ReadHelperOutputFile(hardlink, 1024, &contents));
  EXPECT_EQ(contents, "");
}
#endif  // BUILDFLAG(IS_WIN)

}  // namespace
}  // namespace molt_ai
