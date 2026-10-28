// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Windows confinement for short-lived helper programs (tesseract,
// whisper-cli), as data: everything helper_sandbox_win.cc decides, kept
// here as plain C++ without <windows.h> or //base so it compiles and is
// unit-tested on every platform. The Windows file applies it with thin
// calls and checks these values against the SDK's and the sandbox
// library's with static_asserts.
//
// A helper runs as a less-privileged AppContainer (LPAC) process:
//   - with a package SID of its own for each run (AppContainerSid()), which
//     only that run's scratch directory grants (ScratchDirectorySddl());
//   - with one capability, lpacMoltBrowserHelperFiles, which the installer
//     grants read and execute on the helpers' own folders (<install>\ocr,
//     <install>\whisper) and nothing else;
//   - with no network capability: an AppContainer without one has no
//     network, loopback included;
//   - opted out of ALL APPLICATION PACKAGES (that is what makes it LPAC),
//     so it reads only what grants its package SID, its capability or ALL
//     RESTRICTED APPLICATION PACKAGES (Windows' own system files);
//   - created with a STARTUPINFOEX attribute list (PlanLaunch()): the
//     security capabilities, the all-application-packages opt-out, no child
//     processes, a handle list holding only the output pipe (its stdout and
//     stderr; it has no stdin and no console), its job (so it is in the job
//     before its first instruction) and kMitigations;
//   - in a job that allows one process and kProcessMemoryLimit of committed
//     memory, kills it when the job closes or an exception goes unhandled,
//     and keeps it off the clipboard, global atoms, other processes' user
//     handles, the desktop and system settings (sandbox::JobLevel::
//     kLockdown);
//   - with an environment of only SystemRoot, SystemDrive, windir, Path
//     (System32 and the Windows directory), TEMP and TMP (the scratch
//     directory).
//
// Traversal: an AppContainer token keeps SeChangeNotifyPrivilege ("bypass
// traverse checking"), so a helper opens a path it is granted whatever the
// directories above it grant (its scratch directory sits in the user's
// %TEMP%, which grants AppContainers nothing).
//
// Before a helper starts, the launcher checks under that token (the
// sandbox library's AppContainer AccessCheck) that it may run the program,
// read each read-only path that exists and write the scratch directory
// (PlanAccessChecks()). If not, the helper doesn't start, and the error
// says what is missing (DescribeAccessFailure()). That is what a user of
// the portable ZIP sees, which has no installer to grant the capability.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_HELPER_SANDBOX_WIN_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_HELPER_SANDBOX_WIN_POLICY_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace molt_ai {
namespace helper_sandbox_win {

// The capability the installer grants (icacls "(OI)(CI)(RX)") on the
// helpers' folders, and its SID: S-1-15-3-1024- and the SHA-256 of the
// upper-cased UTF-16LE name as eight little-endian words, as
// base::win::Sid::FromNamedCapability() derives it. installer/windows/
// moltbrowser.nsi spells out the same SID.
inline constexpr char kHelperFilesCapability[] = "lpacMoltBrowserHelperFiles";
inline constexpr char kHelperFilesCapabilitySid[] =
    "S-1-15-3-1024-3499754957-3417772683-1255750666-3341737774-2438675945-"
    "3280382303-1781483004-2555713060";

// sandbox::MitigationFlags (sandbox/win/src/security_level.h).
inline constexpr uint64_t kMitigationRelocateImage = 0x00000008;
inline constexpr uint64_t kMitigationHeapTerminate = 0x00000020;
inline constexpr uint64_t kMitigationBottomUpAslr = 0x00000040;
inline constexpr uint64_t kMitigationHighEntropyAslr = 0x00000080;
inline constexpr uint64_t kMitigationStrictHandleChecks = 0x00000100;
inline constexpr uint64_t kMitigationWin32kDisable = 0x00000800;
inline constexpr uint64_t kMitigationExtensionPointDisable = 0x00001000;
inline constexpr uint64_t kMitigationDynamicCodeDisable = 0x00002000;
inline constexpr uint64_t kMitigationNonSystemFontDisable = 0x00010000;
inline constexpr uint64_t kMitigationForceMsSignedBins = 0x00020000;
inline constexpr uint64_t kMitigationImageLoadNoRemote = 0x00040000;
inline constexpr uint64_t kMitigationImageLoadNoLowLabel = 0x00080000;
inline constexpr uint64_t kMitigationImageLoadPreferSys32 = 0x00100000;
inline constexpr uint64_t kMitigationKtmComponent = 0x00800000;
inline constexpr uint64_t kMitigationCetStrictMode = 0x02000000;
inline constexpr uint64_t kMitigationFsctlDisabled = 0x04000000;

// What helpers get at creation: what any console program lives with.
// Terminate on heap corruption; bottom-up ASLR; no legacy extension points
// (AppInit DLLs, Winsock LSPs, global hooks); no fonts but the system's;
// no images from network shares or labelled low integrity (so nothing
// written to the scratch directory, which is, can be loaded); no Kernel
// Transaction Manager (Windows 11).
//
// Left out, because a third-party program built with MinGW (as the
// Windows tesseract and whisper builds are) may not start or may crash
// under them, and they can't be tried here: strict handle checks (a
// harmless double close becomes a crash), win32k lockdown (a DLL that
// links user32 fails to initialize), arbitrary code guard (MinGW's
// runtime applies pseudo-relocations by making code writable), forced
// image relocation and forced high-entropy ASLR (for an image that
// doesn't ask for them), Microsoft-signed binaries only, preferring
// System32's DLLs (an older system C++ runtime under a newer helper), CET
// strict mode and the FSCTL lockdown.
inline constexpr uint64_t kMitigations =
    kMitigationHeapTerminate | kMitigationBottomUpAslr |
    kMitigationExtensionPointDisable | kMitigationNonSystemFontDisable |
    kMitigationImageLoadNoRemote | kMitigationImageLoadNoLowLabel |
    kMitigationKtmComponent;

// The job: the helper only, and at most this much committed memory (a
// runaway or malicious input can't exhaust the machine's commit).
inline constexpr uint32_t kActiveProcessLimit = 1;
inline constexpr uint64_t kProcessMemoryLimit = uint64_t{4} << 30;

// What the launcher puts in the STARTUPINFOEX attribute list, and the job.
struct LaunchPlan {
  LaunchPlan();
  LaunchPlan(const LaunchPlan&);
  LaunchPlan& operator=(const LaunchPlan&);
  ~LaunchPlan();

  // PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES.
  std::string package_sid;
  std::vector<std::string> capability_sids;
  // PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY: opted out.
  bool low_privilege_app_container = false;
  // PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY: restricted.
  bool restrict_child_processes = false;
  // PROC_THREAD_ATTRIBUTE_HANDLE_LIST: only the output pipe's write end,
  // which is the helper's stdout and stderr; stdin is null.
  bool output_pipe_only = false;
  // PROC_THREAD_ATTRIBUTE_JOB_LIST: the run's job.
  bool assign_job = false;
  // PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY (and the component filter).
  uint64_t mitigations = 0;
  // The job's limits.
  uint32_t active_process_limit = 0;
  uint64_t process_memory_limit = 0;
};

// The plan for a run with package SID |package_sid|; nullopt unless it is
// an AppContainer SID (IsAppContainerSid()).
std::optional<LaunchPlan> PlanLaunch(std::string_view package_sid);

// "S-1-15-2-" and seven 32-bit words: a package SID, as
// DeriveAppContainerSidFromAppContainerName() makes one from a name. Each
// run takes random words.
std::string AppContainerSid(const std::array<uint32_t, 7>& words);
bool IsAppContainerSid(std::string_view sid);

// A SID in SDDL form ("S-1-" and up to 15 decimal parts), as
// base::win::Sid::ToSddlString() writes one: what the security descriptors
// below accept, so nothing else can reach their SDDL.
bool IsSddlSid(std::string_view sid);

// Access masks (winnt.h).
inline constexpr uint32_t kGenericRead = 0x80000000;
inline constexpr uint32_t kGenericExecute = 0x20000000;
inline constexpr uint32_t kFileListDirectory = 0x0001;
inline constexpr uint32_t kFileAddFile = 0x0002;
inline constexpr uint32_t kFileAddSubdirectory = 0x0004;
inline constexpr uint32_t kFileReadEa = 0x0008;
inline constexpr uint32_t kFileWriteEa = 0x0010;
inline constexpr uint32_t kFileTraverse = 0x0020;  // FILE_EXECUTE on files
inline constexpr uint32_t kFileDeleteChild = 0x0040;
inline constexpr uint32_t kFileReadAttributes = 0x0080;
inline constexpr uint32_t kFileWriteAttributes = 0x0100;
inline constexpr uint32_t kDelete = 0x00010000;
inline constexpr uint32_t kReadControl = 0x00020000;
inline constexpr uint32_t kSynchronize = 0x00100000;
// FILE_GENERIC_READ.
inline constexpr uint32_t kFileGenericRead = kReadControl | kFileListDirectory |
                                             kFileReadAttributes | kFileReadEa |
                                             kSynchronize;

// The scratch directory's rights for the run's package SID: on the
// directory itself, list, create files and directories, delete what is in
// it, traverse, read its attributes; not delete or rename it, and not
// change its security. On directories made in it, the same and delete. On
// files in it, read, write and delete, never execute.
inline constexpr uint32_t kScratchDirectoryRights =
    kFileListDirectory | kFileAddFile | kFileAddSubdirectory | kFileReadEa |
    kFileTraverse | kFileDeleteChild | kFileReadAttributes | kReadControl |
    kSynchronize;
inline constexpr uint32_t kScratchSubdirectoryRights =
    kScratchDirectoryRights | kDelete;
inline constexpr uint32_t kScratchFileRights =
    kFileListDirectory /* FILE_READ_DATA */ | kFileAddFile /* WRITE_DATA */ |
    kFileAddSubdirectory /* APPEND_DATA */ | kFileReadEa | kFileWriteEa |
    kFileReadAttributes | kFileWriteAttributes | kDelete | kReadControl |
    kSynchronize;

// The security descriptor the scratch directory is created with (never
// changed later): a protected DACL that gives SYSTEM, Administrators and
// the user (|user_sid|, the browser's) full control, and the run's package
// SID the rights above, inherited by what is created in it; and a low
// mandatory label, inherited too, without which a low-integrity
// AppContainer process can't write there. Empty if either SID isn't one.
std::string ScratchDirectorySddl(std::string_view user_sid,
                                 std::string_view package_sid);

// The security descriptor of a directory for data helpers read from
// outside their own folders (CreateHelperDataDirectory(); e.g. a speech
// model downloaded later): SYSTEM, Administrators and the user have full
// control, helpers (their capability) may read; inherited by what is put
// in it, set when it is created. Empty if |user_sid| isn't a SID.
std::string DataDirectorySddl(std::string_view user_sid);

// "<prefix>_<16 hex digits of |random|>"; empty unless |prefix| is 1 to 32
// ASCII letters, digits or underscores.
std::string ScratchDirectoryName(std::string_view prefix, uint64_t random);

// What the launcher checks under the helper's token before starting it, in
// this order: the program (read and execute), each of the
// |read_only_count| read-only paths that exist (read), the scratch
// directory (list, create files, traverse).
enum class AccessTarget { kProgram, kReadOnlyPath, kScratchDirectory };
struct AccessCheck {
  AccessTarget target = AccessTarget::kProgram;
  // Into the policy's read_only_paths, for kReadOnlyPath.
  size_t index = 0;
  uint32_t desired_access = 0;
};
inline constexpr uint32_t kProgramAccess = kGenericRead | kGenericExecute;
inline constexpr uint32_t kReadOnlyPathAccess = kGenericRead;
inline constexpr uint32_t kScratchDirectoryAccess =
    kFileListDirectory | kFileAddFile | kFileTraverse;
std::vector<AccessCheck> PlanAccessChecks(size_t read_only_count);

// Why the helper didn't start when a check failed for |path| (UTF-8).
std::string DescribeAccessFailure(AccessTarget target, std::string_view path);

// The helper's environment, sorted by name: SystemDrive and SystemRoot,
// windir, Path, TEMP and TMP. |system_root| is the Windows directory
// ("C:\Windows"), |scratch| the scratch directory. SystemDrive is left out
// unless |system_root| starts with a drive letter and colon.
using EnvironmentVariables = std::vector<std::pair<std::wstring, std::wstring>>;
EnvironmentVariables HelperEnvironment(const std::wstring& system_root,
                                       const std::wstring& scratch);

// The environment block CreateProcess takes with
// CREATE_UNICODE_ENVIRONMENT: "NAME=value\0" for each variable, sorted by
// name ignoring ASCII case, and a final "\0". Empty if a name is empty or
// holds '=' or NUL, or a value holds NUL.
std::wstring EnvironmentBlock(const EnvironmentVariables& variables);

// For a helper that exited, with nothing on its output, with one of the
// NTSTATUS codes Windows' loader ends a process with when it couldn't
// start it (a library missing or unreadable, or failing to initialize):
// why, e.g. "a library it needs couldn't be loaded (0xC0000135)".
// Empty for any other exit code.
std::string DescribeStartFailure(uint32_t exit_code);

}  // namespace helper_sandbox_win
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_HELPER_SANDBOX_WIN_POLICY_H_
