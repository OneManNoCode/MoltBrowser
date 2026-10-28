// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/helper_sandbox.h"

#include <stddef.h>
#include <stdint.h>

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
#include "chrome/browser/molt_ai/common/helper_sandbox_win_policy.h"
#include "chrome/browser/molt_ai/common/seatbelt_profile.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_POSIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

#if BUILDFLAG(IS_WIN)
// winsock2.h before windows.h, which would otherwise bring in winsock.h.
#include <winsock2.h>

#include <windows.h>

#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/security_descriptor.h"
#include "base/win/security_util.h"
#include "base/win/sid.h"
#endif

#if BUILDFLAG(IS_LINUX)
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>

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
  // Writes: /dev/null and beneath the scratch directory, nothing else.
  EXPECT_EQ(CountOf(profile, "file-write"), 2u);
  EXPECT_NE(profile.find("(allow file-write-data (literal \"/dev/null\"))"),
            std::string::npos);
  EXPECT_NE(profile.find("(allow file-read* file-write*\n"
                         "  (prefix (string-append (param \"SCRATCH\") "
                         "\"/\")))"),
            std::string::npos);
  // Exec of the helper only.
  EXPECT_EQ(CountOf(profile, "process-exec"), 1u);
  EXPECT_NE(profile.find("(allow process-exec (literal (param \"HELPER\")))"),
            std::string::npos);
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
        } else if (insn.k == offsetof(seccomp_data, args)) {
          a = static_cast<uint32_t>(data.args[0]);
        } else {
          ADD_FAILURE() << "unexpected load at offset " << insn.k;
          return 0;
        }
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

constexpr uint32_t Errno(int error) {
  return SECCOMP_RET_ERRNO | static_cast<uint32_t>(error);
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
  // Threads (pthread_create) and processes (fork) still work.
  EXPECT_EQ(RunFilter(program, Call(__NR_clone, 0x003d0f00)),
            SECCOMP_RET_ALLOW);
  EXPECT_EQ(RunFilter(program, Call(__NR_clone, SIGCHLD)), SECCOMP_RET_ALLOW);

  // Network: only AF_UNIX sockets, and no connect().
  EXPECT_EQ(RunFilter(program, Call(__NR_socket, AF_UNIX)), SECCOMP_RET_ALLOW);
  EXPECT_EQ(RunFilter(program, Call(__NR_socketpair, AF_UNIX)),
            SECCOMP_RET_ALLOW);
  for (int family : {AF_INET, AF_INET6, AF_NETLINK, AF_PACKET, AF_UNSPEC}) {
    EXPECT_EQ(RunFilter(program, Call(__NR_socket, family)), Errno(EACCES))
        << family;
    EXPECT_EQ(RunFilter(program, Call(__NR_socketpair, family)), Errno(EACCES))
        << family;
  }
  // Only the low 32 bits are the family, as in the kernel.
  EXPECT_EQ(RunFilter(program, Call(__NR_socket, (1ULL << 32) | AF_UNIX)),
            SECCOMP_RET_ALLOW);
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
  // Nothing written to the scratch directory can be run, and it can't hold
  // links out of it, sockets, FIFOs or devices.
  EXPECT_EQ(scratch & (hsl::kFsExecute | hsl::kFsMakeSym | hsl::kFsMakeSock |
                       hsl::kFsMakeFifo | hsl::kFsMakeChar | hsl::kFsMakeBlock),
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
  // Also closes its output early, and leaves a child behind in its group.
  const HelperRunResult run =
      RunSandboxedHelper(Shell("exec >/dev/null 2>&1; (while :; do :; done) & "
                               "while :; do :; done"),
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
  const std::optional<int64_t> size =
      base::GetFileSize(scratch.GetPath().AppendASCII("big"));
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(static_cast<uint64_t>(*size), hsl::kMaxHelperFileBytes);
}

TEST(HelperSandboxLinuxTest, ConfinesWhenTheKernelCan) {
  HelperScratchDir scratch;
  base::ScopedTempDir elsewhere;
  ASSERT_TRUE(scratch.Create("molt_test"));
  ASSERT_TRUE(elsewhere.CreateUniqueTempDir());
  const base::FilePath secret = elsewhere.GetPath().AppendASCII("secret");
  ASSERT_TRUE(base::WriteFile(secret, "secret"));
  const HelperSandboxPolicy policy = scratch.MakePolicy();
  const HelperRunResult run =
      RunSandboxedHelper(Shell("cat '" + secret.value() +
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
