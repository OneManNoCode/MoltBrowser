// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/helper_binary.h"

#include "base/files/file_util.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"

#if BUILDFLAG(IS_POSIX)
#include "chrome/browser/molt_ai/common/trusted_binary.h"
#endif

namespace molt_ai {

HelperBinary::HelperBinary() = default;
HelperBinary::HelperBinary(const HelperBinary&) = default;
HelperBinary& HelperBinary::operator=(const HelperBinary&) = default;
HelperBinary::~HelperBinary() = default;

namespace {

#if BUILDFLAG(IS_POSIX)
HelperBinary FromResult(const trusted_binary::Result& result) {
  HelperBinary out;
  if (result.ok()) {
    out.path = base::FilePath(result.path);
    out.bundled = result.source == trusted_binary::Source::kBundled;
  } else {
    out.error = result.error;
    out.rejected_existing = result.rejected_existing;
  }
  return out;
}
#endif  // BUILDFLAG(IS_POSIX)

}  // namespace

HelperBinary FindHelperBinary(
    const base::FilePath& bundle_dir,
    const std::vector<base::FilePath>& bundled_names,
    [[maybe_unused]] const std::vector<std::string>& linux_system_paths) {
  // A handful of stat()/realpath() calls on local paths.
  ScopedAllowBlockingForMolt allow_blocking;
#if BUILDFLAG(IS_POSIX)
  std::vector<std::string> bundled;
  if (!bundle_dir.empty()) {
    for (const base::FilePath& name : bundled_names) {
      bundled.push_back(bundle_dir.Append(name).value());
    }
  }
#if BUILDFLAG(IS_LINUX)
  const std::vector<std::string>& system = linux_system_paths;
#else
  // macOS: /usr/local and /opt/homebrew are user-writable and nothing in
  // /usr/bin is ours to trust, so only the bundled copy counts.
  const std::vector<std::string> system;
#endif
  return FromResult(trusted_binary::Find(bundle_dir.value(), bundled, system,
                                         trusted_binary::RealFileSystem()));
#else   // !BUILDFLAG(IS_POSIX)
  HelperBinary out;
  if (bundle_dir.empty() || bundled_names.empty()) {
    out.error = "not bundled with this build";
    return out;
  }
  // Windows: only the copy next to the executable. No PATH search.
  const base::FilePath dir = base::MakeAbsoluteFilePath(bundle_dir);
  for (const base::FilePath& name : bundled_names) {
    const base::FilePath candidate =
        base::MakeAbsoluteFilePath(bundle_dir.Append(name));
    if (dir.empty() || candidate.empty() || !dir.IsParent(candidate) ||
        !base::PathExists(candidate) || base::DirectoryExists(candidate)) {
      continue;
    }
    out.path = candidate;
    out.bundled = true;
    return out;
  }
  out.error =
      bundle_dir.Append(bundled_names.front()).AsUTF8Unsafe() + " not found";
  return out;
#endif  // BUILDFLAG(IS_POSIX)
}

HelperBinary CheckExplicitHelperBinary(const base::FilePath& path) {
  ScopedAllowBlockingForMolt allow_blocking;
  HelperBinary out;
  if (!path.IsAbsolute()) {
    out.error = "\"" + path.AsUTF8Unsafe() + "\" is not an absolute path";
    return out;
  }
#if BUILDFLAG(IS_POSIX)
  return FromResult(
      trusted_binary::CheckExplicit(path.value(), trusted_binary::CurrentUid(),
                                    trusted_binary::RealFileSystem()));
#else
  if (!base::PathExists(path) || base::DirectoryExists(path)) {
    out.error = path.AsUTF8Unsafe() + " not found";
    out.rejected_existing = base::DirectoryExists(path);
    return out;
  }
  out.path = path;
  return out;
#endif
}

}  // namespace molt_ai
