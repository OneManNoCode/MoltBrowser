// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/profile/molt_profile_store.h"

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "components/os_crypt/sync/os_crypt.h"

namespace molt_ai {
namespace profile {

namespace {

// Subdirectory under the user's home; matches the layout already used
// by Personal Vector Memory (~/.moltbrowser/memory/) and Automation
// (~/.moltbrowser/automations/).
constexpr char kRootDirName[]     = ".moltbrowser";
constexpr char kProfileFilename[] = "profile.enc";

}  // namespace

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
  base::FilePath path = GetProfilePath();
  std::string ciphertext;
  if (!base::ReadFileToString(path, &ciphertext) || ciphertext.empty()) {
    return base::DictValue();
  }
  std::string plaintext;
  if (!OSCrypt::DecryptString(ciphertext, &plaintext)) {
    LOG(WARNING) << "[MoltProfileStore] decrypt failed; returning empty.";
    return base::DictValue();
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(plaintext, /*options=*/0);
  if (!parsed || !parsed->is_dict()) {
    LOG(WARNING) << "[MoltProfileStore] parse failed; returning empty.";
    return base::DictValue();
  }
  return std::move(parsed->GetDict());
}

bool MoltProfileStore::Save(const base::DictValue& dict) {
  base::FilePath path = GetProfilePath();
  // Make sure ~/.moltbrowser/ exists.
  base::FilePath dir = path.DirName();
  if (!base::DirectoryExists(dir) && !base::CreateDirectory(dir)) {
    LOG(WARNING) << "[MoltProfileStore] cannot create dir " << dir.value();
    return false;
  }
  std::string plaintext;
  if (!base::JSONWriter::Write(dict, &plaintext)) {
    return false;
  }
  std::string ciphertext;
  if (!OSCrypt::EncryptString(plaintext, &ciphertext)) {
    LOG(WARNING) << "[MoltProfileStore] encrypt failed";
    return false;
  }
  // Atomic write: stage to .tmp then rename.
  base::FilePath tmp_path = path.AddExtensionASCII(".tmp");
  if (!base::WriteFile(tmp_path, ciphertext)) {
    return false;
  }
  if (!base::ReplaceFile(tmp_path, path, /*error=*/nullptr)) {
    base::DeleteFile(tmp_path);
    return false;
  }
  return true;
}

bool MoltProfileStore::Clear() {
  base::FilePath path = GetProfilePath();
  if (!base::PathExists(path)) return true;
  return base::DeleteFile(path);
}

}  // namespace profile
}  // namespace molt_ai
