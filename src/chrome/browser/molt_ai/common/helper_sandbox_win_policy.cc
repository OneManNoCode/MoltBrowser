// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/helper_sandbox_win_policy.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace molt_ai {
namespace helper_sandbox_win {

namespace {

// Whether |text| is |count| (or, with |at_most|, 1 to |count|) decimal
// numbers of at most 32 bits, separated by '-'.
bool IsDecimalParts(std::string_view text, size_t count, bool at_most) {
  size_t parts = 0;
  size_t start = 0;
  for (;;) {
    const size_t end = std::min(text.find('-', start), text.size());
    const std::string_view part = text.substr(start, end - start);
    if (part.empty() || part.size() > 10) {
      return false;
    }
    uint64_t value = 0;
    for (char c : part) {
      if (c < '0' || c > '9') {
        return false;
      }
      value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    if (value > UINT32_MAX || ++parts > count) {
      return false;
    }
    if (end == text.size()) {
      return at_most || parts == count;
    }
    start = end + 1;
  }
}

constexpr std::string_view kHexDigits = "0123456789abcdef";

std::string Hex(uint32_t value) {
  std::string digits;
  do {
    digits.insert(digits.begin(), kHexDigits[value & 0xf]);
    value >>= 4;
  } while (value != 0);
  return "0x" + digits;
}

wchar_t AsciiLower(wchar_t c) {
  return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

bool LessIgnoringAsciiCase(const std::wstring& a, const std::wstring& b) {
  return std::lexicographical_compare(
      a.begin(), a.end(), b.begin(), b.end(),
      [](wchar_t x, wchar_t y) { return AsciiLower(x) < AsciiLower(y); });
}

// "(A;<flags>;<rights>;;;<sid>)".
std::string AllowAce(std::string_view flags,
                     std::string_view rights,
                     std::string_view sid) {
  std::string ace = "(A;";
  ace += flags;
  ace += ';';
  ace += rights;
  ace += ";;;";
  ace += sid;
  ace += ')';
  return ace;
}

// SYSTEM, Administrators and the user: full control, inherited.
std::string OwnersAces(std::string_view user_sid) {
  return AllowAce("OICI", "FA", "SY") + AllowAce("OICI", "FA", "BA") +
         AllowAce("OICI", "FA", user_sid);
}

}  // namespace

LaunchPlan::LaunchPlan() = default;
LaunchPlan::LaunchPlan(const LaunchPlan&) = default;
LaunchPlan& LaunchPlan::operator=(const LaunchPlan&) = default;
LaunchPlan::~LaunchPlan() = default;

std::optional<LaunchPlan> PlanLaunch(std::string_view package_sid) {
  if (!IsAppContainerSid(package_sid)) {
    return std::nullopt;
  }
  LaunchPlan plan;
  plan.package_sid = std::string(package_sid);
  plan.capability_sids = {kHelperFilesCapabilitySid};
  plan.low_privilege_app_container = true;
  plan.restrict_child_processes = true;
  plan.output_pipe_only = true;
  plan.assign_job = true;
  plan.mitigations = kMitigations;
  plan.active_process_limit = kActiveProcessLimit;
  plan.process_memory_limit = kProcessMemoryLimit;
  return plan;
}

std::string AppContainerSid(const std::array<uint32_t, 7>& words) {
  std::string sid = "S-1-15-2";
  for (uint32_t word : words) {
    sid += '-';
    sid += std::to_string(word);
  }
  return sid;
}

bool IsAppContainerSid(std::string_view sid) {
  constexpr std::string_view kPrefix = "S-1-15-2-";
  return sid.substr(0, kPrefix.size()) == kPrefix &&
         IsDecimalParts(sid.substr(kPrefix.size()), 7, /*at_most=*/false);
}

bool IsSddlSid(std::string_view sid) {
  // The identifier authority, then up to 15 sub-authorities.
  constexpr std::string_view kPrefix = "S-1-";
  return sid.substr(0, kPrefix.size()) == kPrefix &&
         IsDecimalParts(sid.substr(kPrefix.size()), 16, /*at_most=*/true);
}

std::string ScratchDirectorySddl(std::string_view user_sid,
                                 std::string_view package_sid) {
  if (!IsSddlSid(user_sid) || !IsAppContainerSid(package_sid)) {
    return std::string();
  }
  return "D:P" + OwnersAces(user_sid) +
         AllowAce("", Hex(kScratchDirectoryRights), package_sid) +
         AllowAce("CIIO", Hex(kScratchSubdirectoryRights), package_sid) +
         AllowAce("OIIO", Hex(kScratchFileRights), package_sid) +
         "S:(ML;OICI;NW;;;LW)";
}

std::string DataDirectorySddl(std::string_view user_sid) {
  if (!IsSddlSid(user_sid)) {
    return std::string();
  }
  return "D:P" + OwnersAces(user_sid) +
         AllowAce("OICI", Hex(kFileGenericRead), kHelperFilesCapabilitySid);
}

std::string ScratchDirectoryName(std::string_view prefix, uint64_t random) {
  if (prefix.empty() || prefix.size() > 32 ||
      !std::all_of(prefix.begin(), prefix.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
      })) {
    return std::string();
  }
  std::string name(prefix);
  name += '_';
  for (int shift = 60; shift >= 0; shift -= 4) {
    name += kHexDigits[(random >> shift) & 0xf];
  }
  return name;
}

std::vector<AccessCheck> PlanAccessChecks(size_t read_only_count) {
  std::vector<AccessCheck> checks;
  checks.push_back({AccessTarget::kProgram, 0, kProgramAccess});
  for (size_t i = 0; i < read_only_count; ++i) {
    checks.push_back({AccessTarget::kReadOnlyPath, i, kReadOnlyPathAccess});
  }
  checks.push_back(
      {AccessTarget::kScratchDirectory, 0, kScratchDirectoryAccess});
  return checks;
}

std::string DescribeAccessFailure(AccessTarget target, std::string_view path) {
  switch (target) {
    case AccessTarget::kProgram:
      return "on Windows, OCR and voice need MoltBrowser's installer, which "
             "lets their sandbox read their folders (the portable ZIP "
             "can't); its sandbox can't run " +
             std::string(path);
    case AccessTarget::kReadOnlyPath:
      return "its sandbox can't read " + std::string(path);
    case AccessTarget::kScratchDirectory:
      return "its sandbox can't write to " + std::string(path);
  }
  return "its sandbox can't reach " + std::string(path);
}

EnvironmentVariables HelperEnvironment(const std::wstring& system_root,
                                       const std::wstring& scratch) {
  EnvironmentVariables variables = {
      {L"Path", system_root + L"\\System32;" + system_root},
      {L"SystemRoot", system_root},
      {L"TEMP", scratch},
      {L"TMP", scratch},
      {L"windir", system_root},
  };
  if (system_root.size() >= 2 && system_root[1] == L':' &&
      ((system_root[0] >= L'A' && system_root[0] <= L'Z') ||
       (system_root[0] >= L'a' && system_root[0] <= L'z'))) {
    variables.emplace_back(L"SystemDrive", system_root.substr(0, 2));
  }
  std::sort(variables.begin(), variables.end(),
            [](const auto& a, const auto& b) {
              return LessIgnoringAsciiCase(a.first, b.first);
            });
  return variables;
}

std::wstring EnvironmentBlock(const EnvironmentVariables& variables) {
  EnvironmentVariables sorted = variables;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const auto& a, const auto& b) {
                     return LessIgnoringAsciiCase(a.first, b.first);
                   });
  std::wstring block;
  for (const auto& [name, value] : sorted) {
    if (name.empty() || name.find(L'=') != std::wstring::npos ||
        name.find(L'\0') != std::wstring::npos ||
        value.find(L'\0') != std::wstring::npos) {
      return std::wstring();
    }
    block += name;
    block += L'=';
    block += value;
    block += L'\0';
  }
  block += L'\0';
  return block;
}

std::string DescribeStartFailure(uint32_t exit_code) {
  switch (exit_code) {
    case 0xC0000135:  // STATUS_DLL_NOT_FOUND
      return "a library it needs couldn't be loaded (0xC0000135)";
    case 0xC0000142:  // STATUS_DLL_INIT_FAILED
      return "a library it needs failed to start (0xC0000142)";
    case 0xC0000022:  // STATUS_ACCESS_DENIED
      return "it was denied access to its own files (0xC0000022)";
    case 0xC000007B:  // STATUS_INVALID_IMAGE_FORMAT
      return "a library it needs is damaged or for another system "
             "(0xC000007B)";
    case 0xC0000139:  // STATUS_ENTRYPOINT_NOT_FOUND
      return "a library it needs is the wrong version (0xC0000139)";
    default:
      return std::string();
  }
}

}  // namespace helper_sandbox_win
}  // namespace molt_ai
