// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Windows helper sandbox's decisions, on every platform
// (helper_sandbox_win_policy.h). What Windows does with them is tested in
// helper_sandbox_unittest.cc, on Windows.

#include "chrome/browser/molt_ai/common/helper_sandbox_win_policy.h"

#include <stdint.h>

#include <array>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crypto/hash.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace helper_sandbox_win {
namespace {

// base::win::Sid::FromNamedCapability()'s derivation, from first
// principles.
std::string CapabilitySid(std::string_view name) {
  std::vector<uint8_t> utf16le;
  for (char c : name) {
    const char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    utf16le.push_back(static_cast<uint8_t>(upper));
    utf16le.push_back(0);
  }
  const std::array<uint8_t, 32> digest = crypto::hash::Sha256(utf16le);
  std::string sid = "S-1-15-3-1024";
  for (size_t i = 0; i < 32; i += 4) {
    const uint32_t word = static_cast<uint32_t>(digest[i]) |
                          static_cast<uint32_t>(digest[i + 1]) << 8 |
                          static_cast<uint32_t>(digest[i + 2]) << 16 |
                          static_cast<uint32_t>(digest[i + 3]) << 24;
    sid += "-" + std::to_string(word);
  }
  return sid;
}

constexpr char kPackageSid[] = "S-1-15-2-1-22-333-4444-55555-666666-4294967295";
constexpr char kUserSid[] = "S-1-5-21-1004336348-1177238915-682003330-1001";

TEST(HelperSandboxWinPolicyTest, TheCapabilitySidIsItsNames) {
  // The derivation gives Chrome's own capability SIDs, which the installer
  // already grants for the on-device AI sandbox...
  EXPECT_EQ(CapabilitySid("lpacChromeInstallFiles"),
            "S-1-15-3-1024-2302894289-466761758-1166120688-1039016420-"
            "2430351297-4240214049-4028510897-3317428798");
  EXPECT_EQ(CapabilitySid("registryRead"),
            "S-1-15-3-1024-1065365936-1281604716-3511738428-1654721687-"
            "432734479-3232135806-4053264122-3456934681");
  // ... and the SID moltbrowser.nsi grants on the helpers' folders.
  EXPECT_EQ(CapabilitySid(kHelperFilesCapability), kHelperFilesCapabilitySid);
  EXPECT_TRUE(IsSddlSid(kHelperFilesCapabilitySid));
}

TEST(HelperSandboxWinPolicyTest, LaunchPlan) {
  const std::optional<LaunchPlan> plan = PlanLaunch(kPackageSid);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->package_sid, kPackageSid);
  // The helper-files capability only: no network, nothing else.
  EXPECT_EQ(plan->capability_sids,
            std::vector<std::string>{kHelperFilesCapabilitySid});
  EXPECT_TRUE(plan->low_privilege_app_container);
  EXPECT_TRUE(plan->restrict_child_processes);
  EXPECT_TRUE(plan->output_pipe_only);
  EXPECT_TRUE(plan->assign_job);
  EXPECT_EQ(plan->mitigations, kMitigations);
  EXPECT_EQ(plan->active_process_limit, 1u);
  EXPECT_EQ(plan->process_memory_limit, uint64_t{4} << 30);

  EXPECT_FALSE(PlanLaunch(""));
  EXPECT_FALSE(PlanLaunch(kUserSid));
  EXPECT_FALSE(PlanLaunch(kHelperFilesCapabilitySid));
}

TEST(HelperSandboxWinPolicyTest, Mitigations) {
  for (uint64_t on :
       {kMitigationHeapTerminate, kMitigationBottomUpAslr,
        kMitigationExtensionPointDisable, kMitigationNonSystemFontDisable,
        kMitigationImageLoadNoRemote, kMitigationImageLoadNoLowLabel,
        kMitigationKtmComponent}) {
    EXPECT_EQ(kMitigations & on, on) << on;
  }
  for (uint64_t off :
       {kMitigationRelocateImage, kMitigationHighEntropyAslr,
        kMitigationStrictHandleChecks, kMitigationWin32kDisable,
        kMitigationDynamicCodeDisable, kMitigationForceMsSignedBins,
        kMitigationImageLoadPreferSys32, kMitigationCetStrictMode,
        kMitigationFsctlDisabled}) {
    EXPECT_EQ(kMitigations & off, 0u) << off;
  }
}

TEST(HelperSandboxWinPolicyTest, PackageSids) {
  const std::string sid =
      AppContainerSid({1, 22, 333, 4444, 55555, 666666, 4294967295u});
  EXPECT_EQ(sid, kPackageSid);
  EXPECT_TRUE(IsAppContainerSid(sid));
  EXPECT_TRUE(IsAppContainerSid(AppContainerSid({})));
  EXPECT_TRUE(IsSddlSid(sid));

  for (const char* bad :
       {"", "S-1-15-2", "S-1-15-2-", "S-1-15-2-1-2-3-4-5-6",
        "S-1-15-2-1-2-3-4-5-6-7-8", "S-1-15-2-1-2-3-4-5-6-4294967296",
        "S-1-15-2-1-2-3-4-5-6-07x", "S-1-15-2-1-2-3-4-5--6",
        "S-1-15-2-1-2-3-4-5-6-7-", "S-1-15-3-1-2-3-4-5-6-7",
        "s-1-15-2-1-2-3-4-5-6-7", "S-1-15-2-1-2-3-4-5-6-7)(A;;FA;;;WD)",
        "S-1-15-2-1-2-3-4-5-6-12345678901"}) {
    EXPECT_FALSE(IsAppContainerSid(bad)) << bad;
  }
}

TEST(HelperSandboxWinPolicyTest, SddlSids) {
  EXPECT_TRUE(IsSddlSid(kUserSid));
  EXPECT_TRUE(IsSddlSid("S-1-5-18"));
  EXPECT_TRUE(IsSddlSid("S-1-1-0"));
  for (const char* bad :
       {"", "S-1-", "S-1-5-", "SY", "WD", "S-1-5-21-1)(A;;FA;;;WD",
        "S-1-5-21-1;", "S-1-5-21-1 ", "S-2-5-18",
        "S-1-1-2-3-4-5-6-7-8-9-10-11-12-13-14-15-16-17"}) {
    EXPECT_FALSE(IsSddlSid(bad)) << bad;
  }
  // An authority and 15 sub-authorities at most.
  EXPECT_TRUE(IsSddlSid("S-1-1-2-3-4-5-6-7-8-9-10-11-12-13-14-15-16"));
}

TEST(HelperSandboxWinPolicyTest, ScratchDirectorySecurityDescriptor) {
  const std::string sddl = ScratchDirectorySddl(kUserSid, kPackageSid);
  const std::string pkg = kPackageSid;
  EXPECT_EQ(sddl, "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" +
                      std::string(kUserSid) + ")(A;;0x1200ef;;;" + pkg +
                      ")(A;CIIO;0x1300ef;;;" + pkg + ")(A;OIIO;0x13019f;;;" +
                      pkg + ")S:(ML;OICI;NW;;;LW)");
  // Nothing for everyone, any app package, the helpers' capability or
  // another run.
  for (const char* other :
       {";WD)", ";AC)", "S-1-15-2-1)", "S-1-15-2-2)", "S-1-15-3-", "S-1-1-0"}) {
    EXPECT_EQ(sddl.find(other), std::string::npos) << other;
  }
  // The run may not run what it wrote, change security, or remove or
  // rename the directory itself.
  EXPECT_EQ(kScratchFileRights & kFileTraverse, 0u);
  for (uint32_t rights : {kScratchDirectoryRights, kScratchSubdirectoryRights,
                          kScratchFileRights}) {
    EXPECT_EQ(rights & 0x000C0000u, 0u);  // WRITE_DAC, WRITE_OWNER
    EXPECT_EQ(rights & 0xF0000000u, 0u);  // generic rights
  }
  EXPECT_EQ(kScratchDirectoryRights & kDelete, 0u);
  EXPECT_EQ(kScratchSubdirectoryRights & kDelete, kDelete);

  EXPECT_EQ(ScratchDirectorySddl("", kPackageSid), "");
  EXPECT_EQ(ScratchDirectorySddl(kUserSid, kUserSid), "");
  EXPECT_EQ(ScratchDirectorySddl("S-1-5-21-1)(A;;FA;;;WD", kPackageSid), "");
}

TEST(HelperSandboxWinPolicyTest, DataDirectorySecurityDescriptor) {
  EXPECT_EQ(DataDirectorySddl(kUserSid),
            "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" +
                std::string(kUserSid) + ")(A;OICI;0x120089;;;" +
                kHelperFilesCapabilitySid + ")");
  EXPECT_EQ(DataDirectorySddl("WD"), "");
}

TEST(HelperSandboxWinPolicyTest, ScratchDirectoryNames) {
  EXPECT_EQ(ScratchDirectoryName("molt_ocr", 0x0123456789abcdefull),
            "molt_ocr_0123456789abcdef");
  EXPECT_EQ(ScratchDirectoryName("molt_voice", 0),
            "molt_voice_0000000000000000");
  for (const char* bad : {"", "..", "a/b", "a\\b", "molt ocr", "molt-ocr",
                          "abcdefghijklmnopqrstuvwxyz0123456"}) {
    EXPECT_EQ(ScratchDirectoryName(bad, 1), "") << bad;
  }
}

TEST(HelperSandboxWinPolicyTest, AccessChecks) {
  const std::vector<AccessCheck> checks = PlanAccessChecks(2);
  ASSERT_EQ(checks.size(), 4u);
  EXPECT_EQ(checks[0].target, AccessTarget::kProgram);
  EXPECT_EQ(checks[0].desired_access, 0xA0000000u);  // read and execute
  EXPECT_EQ(checks[1].target, AccessTarget::kReadOnlyPath);
  EXPECT_EQ(checks[1].index, 0u);
  EXPECT_EQ(checks[2].index, 1u);
  EXPECT_EQ(checks[2].desired_access, 0x80000000u);  // read
  EXPECT_EQ(checks[3].target, AccessTarget::kScratchDirectory);
  EXPECT_EQ(checks[3].desired_access, 0x23u);  // list, add file, traverse
  // The scratch directory grants all of that to the run.
  EXPECT_EQ(kScratchDirectoryRights & checks[3].desired_access,
            checks[3].desired_access);
  EXPECT_EQ(PlanAccessChecks(0).size(), 2u);

  const std::string program = DescribeAccessFailure(
      AccessTarget::kProgram, "C:\\x\\ocr\\tesseract.exe");
  EXPECT_NE(program.find("installer"), std::string::npos);
  EXPECT_NE(program.find("portable ZIP"), std::string::npos);
  EXPECT_NE(program.find("C:\\x\\ocr\\tesseract.exe"), std::string::npos);
  EXPECT_EQ(DescribeAccessFailure(AccessTarget::kReadOnlyPath, "C:\\m.bin"),
            "its sandbox can't read C:\\m.bin");
  EXPECT_EQ(DescribeAccessFailure(AccessTarget::kScratchDirectory, "C:\\t"),
            "its sandbox can't write to C:\\t");
}

TEST(HelperSandboxWinPolicyTest, Environment) {
  const EnvironmentVariables variables = HelperEnvironment(
      L"C:\\Windows", L"C:\\Users\\u\\AppData\\Local\\Temp\\s");
  const EnvironmentVariables expected = {
      {L"Path", L"C:\\Windows\\System32;C:\\Windows"},
      {L"SystemDrive", L"C:"},
      {L"SystemRoot", L"C:\\Windows"},
      {L"TEMP", L"C:\\Users\\u\\AppData\\Local\\Temp\\s"},
      {L"TMP", L"C:\\Users\\u\\AppData\\Local\\Temp\\s"},
      {L"windir", L"C:\\Windows"},
  };
  EXPECT_EQ(variables, expected);
  // No drive letter: no SystemDrive.
  EXPECT_EQ(HelperEnvironment(L"\\\\?\\X", L"t").size(), 5u);

  static constexpr wchar_t kBlock[] =
      L"Path=C:\\Windows\\System32;C:\\Windows\0"
      L"SystemDrive=C:\0SystemRoot=C:\\Windows\0"
      L"TEMP=C:\\Users\\u\\AppData\\Local\\Temp\\s\0"
      L"TMP=C:\\Users\\u\\AppData\\Local\\Temp\\s\0"
      L"windir=C:\\Windows\0";
  // The literal's own terminator is the block's final NUL.
  EXPECT_EQ(EnvironmentBlock(variables),
            std::wstring(kBlock, std::size(kBlock)));
}

TEST(HelperSandboxWinPolicyTest, EnvironmentBlocks) {
  EXPECT_EQ(EnvironmentBlock({}), std::wstring(1, L'\0'));
  // Sorted ignoring case, whatever the order given.
  static constexpr wchar_t kSorted[] = L"A=1\0b=2\0c=\0";
  EXPECT_EQ(EnvironmentBlock({{L"b", L"2"}, {L"A", L"1"}, {L"c", L""}}),
            std::wstring(kSorted, std::size(kSorted)));
  for (const EnvironmentVariables& bad :
       {EnvironmentVariables{{L"", L"x"}}, EnvironmentVariables{{L"A=B", L"x"}},
        EnvironmentVariables{{std::wstring(L"A\0B", 3), L"x"}},
        EnvironmentVariables{{L"A", std::wstring(L"x\0y", 3)}}}) {
    EXPECT_EQ(EnvironmentBlock(bad), L"");
  }
}

TEST(HelperSandboxWinPolicyTest, StartFailures) {
  for (uint32_t status :
       {0xC0000135u, 0xC0000142u, 0xC0000022u, 0xC000007Bu, 0xC0000139u}) {
    EXPECT_FALSE(DescribeStartFailure(status).empty()) << status;
  }
  // Exits and crashes of the helper itself.
  for (uint32_t status : {0u, 1u, 3u, 0xC0000005u, 0xC0000409u, 0x40010004u}) {
    EXPECT_EQ(DescribeStartFailure(status), "") << status;
  }
}

}  // namespace
}  // namespace helper_sandbox_win
}  // namespace molt_ai
