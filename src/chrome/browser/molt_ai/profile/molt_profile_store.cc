// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/profile/molt_profile_store.h"

#include <optional>
#include <string>
#include <utility>

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/keys/molt_keys_store.h"
#include "chrome/browser/molt_ai/keys/molt_os_crypt.h"

namespace molt_ai {
namespace profile {

namespace {

// Subdirectory under the user's home; matches the layout already used
// by Personal Vector Memory (~/.moltbrowser/memory/) and Automation
// (~/.moltbrowser/automations/).
constexpr char kRootDirName[]     = ".moltbrowser";
constexpr char kProfileFilename[] = "profile.enc";
// Prefix of the name an unreadable profile.enc is moved to by
// SaveKeepingUnreadableAsBackup().
constexpr char kUnreadableBackupInfix[] = ".unreadable-";

// Reads |path|. |profile| (may be null) receives the dict on kOk.
ProfileReadStatus ReadProfile(const base::FilePath& path,
                              base::DictValue* profile) {
  if (!base::PathExists(path)) {
    return ProfileReadStatus::kAbsent;
  }
  std::string ciphertext;
  if (!base::ReadFileToString(path, &ciphertext)) {
    LOG(WARNING) << "[MoltProfileStore] profile exists but can't be read";
    return ProfileReadStatus::kUnreadable;
  }
  if (ciphertext.empty()) {
    return ProfileReadStatus::kAbsent;  // Nothing in it to lose.
  }
  std::string plaintext;
  os_crypt_async::Encryptor::DecryptFlags flags;
  if (!molt_os_crypt::DecryptString(ciphertext, &plaintext, &flags)) {
    // Either way the file is kept: a temporary failure (keyring locked, or
    // the Encryptor not ready yet) may clear up, and a permanent one (key
    // changed, corrupt file) is still the user's data.
    LOG(WARNING) << "[MoltProfileStore] decrypt failed ("
                 << (flags.temporarily_unavailable
                         ? "OS key store locked or not ready yet"
                         : "key changed or file corrupt?")
                 << ")";
    return ProfileReadStatus::kUnreadable;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(plaintext, /*options=*/0);
  if (!parsed || !parsed->is_dict()) {
    LOG(WARNING) << "[MoltProfileStore] parse failed";
    return ProfileReadStatus::kUnreadable;
  }
  if (profile) {
    *profile = std::move(*parsed).TakeDict();
  }
  return ProfileReadStatus::kOk;
}

// A name next to |path| for keeping an unreadable profile, e.g.
// profile.enc.unreadable-20261016T093000Z; empty if every candidate is
// taken.
base::FilePath UnusedBackupPath(const base::FilePath& path) {
  base::Time::Exploded t;
  base::Time::Now().UTCExplode(&t);
  const std::string base_name =
      std::string(kProfileFilename) + kUnreadableBackupInfix +
      base::StringPrintf("%04d%02d%02dT%02d%02d%02dZ", t.year, t.month,
                         t.day_of_month, t.hour, t.minute, t.second);
  for (int i = 0; i < 100; ++i) {
    const base::FilePath candidate = path.DirName().AppendASCII(
        i == 0 ? base_name : base_name + "-" + base::NumberToString(i));
    if (!base::PathExists(candidate)) {
      return candidate;
    }
  }
  return base::FilePath();
}

}  // namespace

ProfileLoadResult::ProfileLoadResult() = default;
ProfileLoadResult::ProfileLoadResult(ProfileLoadResult&&) = default;
ProfileLoadResult& ProfileLoadResult::operator=(ProfileLoadResult&&) =
    default;
ProfileLoadResult::~ProfileLoadResult() = default;

MoltProfileStore::MoltProfileStore() = default;
MoltProfileStore::~MoltProfileStore() = default;

void MoltProfileStore::SetRootForTesting(const base::FilePath& root) {
  root_override_ = root;
}

base::FilePath MoltProfileStore::GetProfilePath() const {
  base::FilePath root;
  if (!root_override_.empty()) {
    root = root_override_;
  } else {
    base::PathService::Get(base::DIR_HOME, &root);
    root = root.AppendASCII(kRootDirName);
  }
  return root.AppendASCII(kProfileFilename);
}

base::DictValue MoltProfileStore::Load() const {
  return LoadWithStatus().profile;
}

ProfileLoadResult MoltProfileStore::LoadWithStatus() const {
  ProfileLoadResult result;
  result.status = ReadProfile(GetProfilePath(), &result.profile);
  if (result.status != ProfileReadStatus::kOk) {
    result.profile = base::DictValue();
  }
  return result;
}

ProfileSaveResult MoltProfileStore::Save(const base::DictValue& dict) {
  return SaveImpl(dict, /*backup_unreadable=*/false, /*backup_path=*/nullptr);
}

ProfileSaveResult MoltProfileStore::SaveKeepingUnreadableAsBackup(
    const base::DictValue& dict,
    base::FilePath* backup_path) {
  return SaveImpl(dict, /*backup_unreadable=*/true, backup_path);
}

ProfileSaveResult MoltProfileStore::SaveImpl(const base::DictValue& dict,
                                             bool backup_unreadable,
                                             base::FilePath* backup_path) {
  if (backup_path) {
    *backup_path = base::FilePath();
  }
  if (!molt_ai::IsSecureStorageAvailable()) {
    LOG(WARNING) << "[MoltProfileStore] refusing to store profile: no "
                    "secure OS key storage (no keyring key, or not ready "
                    "yet)";
    return ProfileSaveResult::kNoSecureStorage;
  }
  const base::FilePath path = GetProfilePath();
  // Never replace a profile we couldn't read: it may only be locked away
  // for now (keyring locked at the time) and would be lost for good.
  const bool existing_unreadable =
      ReadProfile(path, /*profile=*/nullptr) == ProfileReadStatus::kUnreadable;
  if (existing_unreadable && !backup_unreadable) {
    LOG(WARNING) << "[MoltProfileStore] refusing to overwrite a profile it "
                    "can't read";
    return ProfileSaveResult::kExistingUnreadable;
  }
  // Make sure ~/.moltbrowser/ exists.
  base::FilePath dir = path.DirName();
  if (!base::DirectoryExists(dir) && !base::CreateDirectory(dir)) {
    LOG(WARNING) << "[MoltProfileStore] cannot create dir " << dir.value();
    return ProfileSaveResult::kFailed;
  }
  std::string plaintext;
  if (!base::JSONWriter::Write(dict, &plaintext)) {
    return ProfileSaveResult::kFailed;
  }
  std::string ciphertext;
  // Fails unless an OS-backed key encrypted it (never the hard-coded Linux
  // fallback key).
  if (!molt_ai::EncryptSecretForStorage(plaintext, &ciphertext)) {
    LOG(WARNING) << "[MoltProfileStore] encrypt failed";
    return ProfileSaveResult::kFailed;
  }
  // Atomic write: stage to .tmp then rename.
  base::FilePath tmp_path = path.AddExtensionASCII(".tmp");
  if (!base::WriteFile(tmp_path, ciphertext)) {
    base::DeleteFile(tmp_path);
    return ProfileSaveResult::kFailed;
  }
  base::FilePath backup;
  if (existing_unreadable) {
    // The user chose to start over: keep the old file under a new name.
    backup = UnusedBackupPath(path);
    if (backup.empty() || !base::ReplaceFile(path, backup, /*error=*/nullptr)) {
      LOG(WARNING) << "[MoltProfileStore] can't move the unreadable profile "
                      "aside; not saving";
      base::DeleteFile(tmp_path);
      return ProfileSaveResult::kFailed;
    }
  }
  if (!base::ReplaceFile(tmp_path, path, /*error=*/nullptr)) {
    if (!backup.empty()) {
      base::ReplaceFile(backup, path, /*error=*/nullptr);  // put it back
    }
    base::DeleteFile(tmp_path);
    return ProfileSaveResult::kFailed;
  }
  if (backup_path) {
    *backup_path = backup;
  }
  return ProfileSaveResult::kSaved;
}

bool MoltProfileStore::Clear() {
  base::FilePath path = GetProfilePath();
  if (!base::PathExists(path)) return true;
  return base::DeleteFile(path);
}

}  // namespace profile
}  // namespace molt_ai
