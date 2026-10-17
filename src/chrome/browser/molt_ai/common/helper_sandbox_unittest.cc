// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/helper_sandbox.h"

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/common/seatbelt_profile.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_POSIX)
#include <sys/stat.h>
#include <unistd.h>
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
  policy.read_only = {"/opt/molt/ocr/tessdata", "/tmp/molt_ocr.x/input.pdf"};
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
  EXPECT_EQ(access_of("/tmp/molt_ocr.x/input.pdf"), hsl::kReadAccess);
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

TEST(HelperSandboxLinuxTest, ConfinesWhenTheKernelCan) {
  base::ScopedTempDir scratch;
  base::ScopedTempDir elsewhere;
  ASSERT_TRUE(scratch.CreateUniqueTempDir());
  ASSERT_TRUE(elsewhere.CreateUniqueTempDir());
  const base::FilePath secret = elsewhere.GetPath().AppendASCII("secret");
  ASSERT_TRUE(base::WriteFile(secret, "secret"));
  HelperSandboxPolicy policy;
  policy.writable_dir = scratch.GetPath();
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

}  // namespace
}  // namespace molt_ai
