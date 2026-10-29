// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ResidentHelper (helper_resident.h) with the stand-in helper
// (helper_sandbox_test_helper_posix.cc) under the real sandbox: what it may
// read, that it writes nothing, has no network and starts no process, its
// limits, and its pipes.

#include "chrome/browser/molt_ai/common/helper_resident.h"

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/path_service.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/common/helper_limits.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if BUILDFLAG(IS_LINUX)
#include <sys/prctl.h>
#include <sys/wait.h>

#include "chrome/browser/molt_ai/common/helper_sandbox_linux.h"
#endif

namespace molt_ai {
namespace {

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)

class ResidentHelperTest : public testing::Test {
 protected:
  void SetUp() override {
    // The browser ignores SIGPIPE; a write to a helper that went away
    // fails instead.
    signal(SIGPIPE, SIG_IGN);
    base::FilePath exe_dir;
    ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &exe_dir));
    helper_ = exe_dir.AppendASCII("molt_helper_sandbox_test_helper");
    ASSERT_TRUE(base::PathExists(helper_)) << helper_;
    ASSERT_TRUE(models_.CreateUniqueTempDir());
    ASSERT_TRUE(elsewhere_.CreateUniqueTempDir());
    ASSERT_TRUE(base::WriteFile(models_.GetPath().AppendASCII("model.onnx"),
                                std::string(100, 'm')));
    ASSERT_TRUE(base::WriteFile(elsewhere_.GetPath().AppendASCII("secret"),
                                std::string(10, 's')));
    policy_.read_only_paths = {models_.GetPath()};
  }

  bool Confined() const {
#if BUILDFLAG(IS_LINUX)
    return helper_sandbox_linux::LandlockAbi() > 0;
#else
    return true;
#endif
  }

  std::unique_ptr<ResidentHelper> Start(const std::vector<std::string>& args) {
    base::CommandLine cmd(helper_);
    for (const std::string& arg : args) {
      cmd.AppendArg(arg);
    }
    std::string error;
    std::unique_ptr<ResidentHelper> helper =
        ResidentHelper::Start(cmd, policy_, &error);
    EXPECT_TRUE(helper) << error;
    return helper;
  }

  // Everything it writes until end of file.
  static std::string ReadAll(ResidentHelper& helper) {
    std::string out;
    std::vector<uint8_t> buffer(4096);
    for (;;) {
      const int bytes = helper.Read(buffer);
      if (bytes <= 0) {
        return out;
      }
      out.append(buffer.begin(), buffer.begin() + bytes);
    }
  }

  // What a one-action run printed.
  std::string Output(const std::vector<std::string>& args) {
    std::unique_ptr<ResidentHelper> helper = Start(args);
    if (!helper) {
      return std::string();
    }
    helper->CloseStdin();
    return ReadAll(*helper);
  }

  base::FilePath helper_;
  base::ScopedTempDir models_;
  base::ScopedTempDir elsewhere_;
  ResidentHelperPolicy policy_;
};

TEST_F(ResidentHelperTest, TalksOverItsPipes) {
  std::unique_ptr<ResidentHelper> helper = Start({"relay"});
  ASSERT_TRUE(helper);
  EXPECT_GT(helper->pid(), 0);
  EXPECT_FALSE(helper->confinement().empty());
  const std::string hello = "hello\n";
  ASSERT_TRUE(helper->Write(base::as_byte_span(hello)));
  std::vector<uint8_t> buffer(64);
  const int bytes = helper->Read(buffer);
  ASSERT_GT(bytes, 0);
  EXPECT_EQ(std::string(buffer.begin(), buffer.begin() + bytes), hello);
  // A lot more than a pipe holds, while it copies.
  const std::string block(256 * 1024, 'b');
  std::thread writer([&] {
    EXPECT_TRUE(helper->Write(base::as_byte_span(block)));
    helper->CloseStdin();
  });
  const std::string echoed = ReadAll(*helper);
  writer.join();
  EXPECT_EQ(echoed, block + "EOF\n");
  EXPECT_EQ(helper->WaitForExit(base::Seconds(5)), 0);
  // Gone: writes fail rather than block or raise a signal.
  EXPECT_FALSE(helper->Write(base::as_byte_span(hello)));
}

TEST_F(ResidentHelperTest, ReadsItsModelDirectoryOnly) {
  if (!Confined()) {
    GTEST_SKIP() << "no Landlock: files are not confined";
  }
  EXPECT_EQ(
      Output({"read", models_.GetPath().AppendASCII("model.onnx").value()}),
      "READ 100\n");
  const std::string secret =
      Output({"read", elsewhere_.GetPath().AppendASCII("secret").value()});
  EXPECT_TRUE(secret.starts_with("DENIED")) << secret;
}

TEST_F(ResidentHelperTest, WritesNothing) {
  if (!Confined()) {
    GTEST_SKIP() << "no Landlock: files are not confined";
  }
  for (const base::FilePath& path :
       {models_.GetPath().AppendASCII("out.txt"),
        elsewhere_.GetPath().AppendASCII("out.txt"),
        base::FilePath("/tmp/molt_resident_test_out.txt")}) {
    const std::string output = Output({"write", path.value(), "1"});
    EXPECT_TRUE(output.starts_with("FAILED")) << path << ": " << output;
    EXPECT_FALSE(base::PathExists(path)) << path;
  }
  const std::string made =
      Output({"mkdir", elsewhere_.GetPath().AppendASCII("dir").value()});
  EXPECT_TRUE(made.starts_with("DENIED")) << made;
}

TEST_F(ResidentHelperTest, HasNoNetwork) {
  // A listener it could otherwise reach.
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listener, 0);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(
      bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)),
      0);
  ASSERT_EQ(listen(listener, 1), 0);
  socklen_t length = sizeof(address);
  ASSERT_EQ(
      getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
  const std::string output =
      Output({"connect", std::to_string(ntohs(address.sin_port))});
  close(listener);
  EXPECT_TRUE(output.starts_with("DENIED")) << output;
}

TEST_F(ResidentHelperTest, StartsNoOtherProcess) {
  const std::string output = Output({"fork"});
  EXPECT_TRUE(output.starts_with("DENIED")) << output;
}

#if BUILDFLAG(IS_LINUX)
TEST_F(ResidentHelperTest, ResourceLimits) {
  policy_.max_memory_bytes = uint64_t{512} << 20;
  const std::string output = Output({"limits"});
  EXPECT_NE(output.find("FSIZE 0 0\n"), std::string::npos) << output;
  EXPECT_NE(output.find("CORE 0 0\n"), std::string::npos) << output;
  EXPECT_NE(output.find("DATA 536870912 536870912\n"), std::string::npos)
      << output;
  EXPECT_NE(output.find("NOFILE 256 256\n"), std::string::npos) << output;
}

TEST_F(ResidentHelperTest, MemoryLimit) {
  policy_.max_memory_bytes = uint64_t{256} << 20;
  EXPECT_EQ(Output({"alloc", "128"}), "ALLOCATED\n");
  EXPECT_TRUE(Output({"reserve", "512"}).starts_with("FAILED"));
  // Memory the limit wouldn't count is refused.
  EXPECT_TRUE(Output({"shared", "64"}).starts_with("DENIED"));
}

TEST_F(ResidentHelperTest, DiesWithTheBrowser) {
  // A "browser" (a child of the test) starts the helper and dies without
  // stopping it, as a crash would. The test adopts the orphan and sees it
  // killed by the parent-death signal. (Older kernels send that signal when
  // the thread that started the helper ends, which is why the listener
  // starts it on a thread that lives as long as it.)
  ASSERT_EQ(prctl(PR_SET_CHILD_SUBREAPER, 1UL, 0UL, 0UL, 0UL), 0);
  int fds[2] = {-1, -1};
  ASSERT_EQ(pipe(fds), 0);
  // A helper that never reads its stdin (so its end of file can't stop
  // it): only the kernel's parent-death signal does.
  base::CommandLine cmd(helper_);
  cmd.AppendArg("spin");
  const pid_t browser = fork();
  ASSERT_GE(browser, 0);
  if (browser == 0) {
    std::string error;
    std::unique_ptr<ResidentHelper> helper =
        ResidentHelper::Start(cmd, policy_, &error);
    const pid_t pid = helper ? helper->pid() : -1;
    [[maybe_unused]] const ssize_t written = write(fds[1], &pid, sizeof(pid));
    _exit(0);
  }
  close(fds[1]);
  pid_t helper = -1;
  ASSERT_EQ(read(fds[0], &helper, sizeof(helper)),
            static_cast<ssize_t>(sizeof(helper)));
  close(fds[0]);
  ASSERT_EQ(waitpid(browser, nullptr, 0), browser);
  ASSERT_GT(helper, 0);
  int status = 0;
  pid_t reaped = 0;
  for (int i = 0; i < 100 && reaped == 0; ++i) {
    reaped = waitpid(helper, &status, WNOHANG);
    if (reaped == 0) {
      base::PlatformThread::Sleep(base::Milliseconds(50));
    }
  }
  prctl(PR_SET_CHILD_SUBREAPER, 0UL, 0UL, 0UL, 0UL);
  if (reaped == 0) {
    kill(helper, SIGKILL);
    waitpid(helper, nullptr, 0);
  }
  ASSERT_EQ(reaped, helper);
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);
}
#endif  // BUILDFLAG(IS_LINUX)

TEST_F(ResidentHelperTest, StoppedPastItsCpuLimit) {
  policy_.max_cpu_cores = 0.5;
  policy_.cpu_window = base::Seconds(2);
  std::unique_ptr<ResidentHelper> helper = Start({"spin"});
  ASSERT_TRUE(helper);
  std::string why;
  bool stopped = false;
  const base::TimeTicks start = base::TimeTicks::Now();
  while (!stopped && base::TimeTicks::Now() - start < base::Seconds(10)) {
    base::PlatformThread::Sleep(base::Milliseconds(100));
    stopped = !helper->CheckLimits(base::TimeTicks::Now(), &why);
  }
  EXPECT_TRUE(stopped);
  EXPECT_EQ(why, "it used more than 0.5 of a CPU core for 2 s");
  helper->Kill();
  EXPECT_EQ(ReadAll(*helper), "");
  EXPECT_EQ(helper->WaitForExit(base::Seconds(5)), -1);
}

TEST_F(ResidentHelperTest, AQuietHelperIsWithinItsLimits) {
  policy_.cpu_window = base::Seconds(2);
  std::unique_ptr<ResidentHelper> helper = Start({"relay"});
  ASSERT_TRUE(helper);
  std::string why;
  for (int i = 0; i < 10; ++i) {
    base::PlatformThread::Sleep(base::Milliseconds(100));
    EXPECT_TRUE(helper->CheckLimits(base::TimeTicks::Now(), &why)) << why;
  }
}

TEST_F(ResidentHelperTest, KillEndsAReadInProgress) {
  std::unique_ptr<ResidentHelper> helper = Start({"relay"});
  ASSERT_TRUE(helper);
  std::thread killer([&] {
    base::PlatformThread::Sleep(base::Milliseconds(200));
    helper->Kill();
  });
  std::vector<uint8_t> buffer(64);
  EXPECT_EQ(helper->Read(buffer), 0);
  killer.join();
  EXPECT_EQ(helper->WaitForExit(base::Seconds(5)), -1);
  // Twice is fine.
  helper->Kill();
}

TEST_F(ResidentHelperTest, RefusesWhatIsntConfinable) {
  std::string error;
  base::CommandLine relative(base::FilePath("molt_helper"));
  EXPECT_FALSE(ResidentHelper::Start(relative, policy_, &error));
  EXPECT_FALSE(error.empty());
  ResidentHelperPolicy no_memory_limit = policy_;
  no_memory_limit.max_memory_bytes = 0;
  EXPECT_FALSE(ResidentHelper::Start(base::CommandLine(helper_),
                                     no_memory_limit, &error));
  ResidentHelperPolicy relative_path = policy_;
  relative_path.read_only_paths = {base::FilePath("models")};
  EXPECT_FALSE(
      ResidentHelper::Start(base::CommandLine(helper_), relative_path, &error));
}

#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)

}  // namespace
}  // namespace molt_ai
