// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/keys/molt_keys_store.h"

#include <utility>

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/values.h"
#include "build/build_config.h"
#include "components/os_crypt/sync/os_crypt.h"

namespace molt_ai {

namespace {

constexpr char kRootDirName[] = ".moltbrowser";
constexpr char kKeysFile[] = "keys.enc";

// Process-wide test-root override. Empty in production. Mirrors the
// per-instance override on VaultStore, but MoltKeysStore is all-static
// so the override has to live at file scope.
base::FilePath& RootOverride() {
  static base::NoDestructor<base::FilePath> root;
  return *root;
}

base::FilePath GetKeysPath() {
  base::FilePath root;
  if (!RootOverride().empty()) {
    root = RootOverride();
  } else {
    base::PathService::Get(base::DIR_HOME, &root);
    root = root.AppendASCII(kRootDirName);
  }
  return root.AppendASCII(kKeysFile);
}

// Best-effort zero of a transient plaintext buffer. Same technique as
// chat_handler_helpers::SecureClear; duplicated here to avoid a
// ui/webui include from a browser/ leaf target.
void ScrubString(std::string& s) {
  if (s.empty()) {
    return;
  }
  volatile char* p = const_cast<volatile char*>(s.data());
  for (size_t i = 0; i < s.size(); ++i) {
    p[i] = 0;
  }
}

ProviderConfig ConfigFromDict(const base::DictValue& d) {
  ProviderConfig cfg;
  if (const std::string* s = d.FindString("api_key")) {
    cfg.api_key = *s;
  }
  if (const std::string* s = d.FindString("base_url")) {
    cfg.base_url = *s;
  }
  if (const base::ListValue* models = d.FindList("enabled_models")) {
    for (const base::Value& m : *models) {
      if (m.is_string()) {
        cfg.enabled_models.push_back(m.GetString());
      }
    }
  }
  return cfg;
}

base::DictValue ConfigToDict(const ProviderConfig& cfg) {
  base::DictValue d;
  d.Set("api_key", cfg.api_key);
  d.Set("base_url", cfg.base_url);
  base::ListValue models;
  for (const std::string& m : cfg.enabled_models) {
    models.Append(m);
  }
  d.Set("enabled_models", std::move(models));
  return d;
}

// Read+decrypt the whole file into a dict. Returns std::nullopt if the
// file exists but won't decrypt or parse (corrupt / wrong keychain). An
// absent file returns an empty dict — first-run, not an error.
std::optional<base::DictValue> ReadDecrypted() {
  base::FilePath path = GetKeysPath();
  if (!base::PathExists(path)) {
    return base::DictValue();
  }
  std::string ciphertext;
  if (!base::ReadFileToString(path, &ciphertext)) {
    return std::nullopt;
  }
  if (ciphertext.empty()) {
    return base::DictValue();
  }
  std::string plaintext;
  if (!OSCrypt::DecryptString(ciphertext, &plaintext)) {
    LOG(WARNING) << "[MoltKeysStore] decrypt failed";
    return std::nullopt;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(plaintext, /*options=*/0);
  // Scrub the transient plaintext buffer; the parsed dict now holds the
  // live copy the caller asked for.
  ScrubString(plaintext);
  if (!parsed || !parsed->is_dict()) {
    LOG(WARNING) << "[MoltKeysStore] parse failed or not a dict";
    return std::nullopt;
  }
  return std::move(*parsed).TakeDict();
}

// Whether a write may go ahead without a real OS-backed key. Only writes
// that remove data from an already-readable file may.
enum class WriteKind { kStoresSecrets, kRemovalOnly };

// Encrypt+write the whole dict. Atomic via .tmp + ReplaceFile.
bool WriteEncrypted(const base::DictValue& dict, WriteKind kind) {
  if (kind == WriteKind::kStoresSecrets && !IsSecureStorageAvailable()) {
    LOG(WARNING) << "[MoltKeysStore] refusing to store keys: no secure "
                    "OS key storage (OSCrypt would use a hard-coded key)";
    return false;
  }
  base::FilePath path = GetKeysPath();
  base::FilePath dir = path.DirName();
  if (!base::DirectoryExists(dir) && !base::CreateDirectory(dir)) {
    return false;
  }
  std::string plaintext;
  if (!base::JSONWriter::Write(dict, &plaintext)) {
    return false;
  }
  std::string ciphertext;
  bool encrypted = OSCrypt::EncryptString(plaintext, &ciphertext);
  ScrubString(plaintext);
  if (!encrypted) {
    LOG(WARNING) << "[MoltKeysStore] encrypt failed";
    return false;
  }
  base::FilePath tmp = path.AddExtensionASCII(".tmp");
  if (!base::WriteFile(tmp, ciphertext)) {
    base::DeleteFile(tmp);  // don't leave a partial temp behind
    return false;
  }
  if (!base::ReplaceFile(tmp, path, /*error=*/nullptr)) {
    base::DeleteFile(tmp);
    return false;
  }
  return true;
}

}  // namespace

bool IsSecureStorageAvailable() {
  // On Linux this is false when OSCrypt has no libsecret/KWallet backend
  // (or --password-store=basic) and would encrypt with the hard-coded v10
  // key; on macOS when the Keychain is unavailable; on Windows when only
  // a mock key exists.
  return OSCrypt::IsEncryptionAvailable();
}

std::string SecureStorageUnavailableMessage() {
#if BUILDFLAG(IS_LINUX)
  return "No system keyring found; install/unlock GNOME Keyring or KWallet "
         "to store keys securely.";
#elif BUILDFLAG(IS_MAC)
  return "The macOS Keychain is unavailable or locked; unlock it to store "
         "keys securely.";
#else
  return "Secure OS key storage is unavailable, so keys can't be stored "
         "securely.";
#endif
}

ProviderConfig::ProviderConfig() = default;
ProviderConfig::ProviderConfig(const ProviderConfig&) = default;
ProviderConfig::ProviderConfig(ProviderConfig&&) = default;
ProviderConfig& ProviderConfig::operator=(const ProviderConfig&) = default;
ProviderConfig& ProviderConfig::operator=(ProviderConfig&&) = default;
ProviderConfig::~ProviderConfig() = default;

// static
std::map<std::string, ProviderConfig> MoltKeysStore::LoadAll() {
  std::map<std::string, ProviderConfig> out;
  std::optional<base::DictValue> dict = ReadDecrypted();
  if (!dict) {
    return out;
  }
  for (const auto kv : *dict) {
    if (!kv.second.is_dict()) {
      continue;
    }
    out.emplace(kv.first, ConfigFromDict(kv.second.GetDict()));
  }
  return out;
}

// static
std::optional<ProviderConfig> MoltKeysStore::Get(
    const std::string& provider_id) {
  std::optional<base::DictValue> dict = ReadDecrypted();
  if (!dict) {
    return std::nullopt;
  }
  const base::DictValue* entry = dict->FindDict(provider_id);
  if (!entry) {
    return std::nullopt;
  }
  return ConfigFromDict(*entry);
}

// static
bool MoltKeysStore::Save(const std::string& provider_id,
                         const ProviderConfig& cfg) {
  if (provider_id.empty()) {
    return false;
  }
  std::optional<base::DictValue> dict = ReadDecrypted();
  // A decrypt failure must not silently wipe other providers' keys.
  if (!dict) {
    LOG(WARNING) << "[MoltKeysStore] Save aborted: existing store unreadable";
    return false;
  }
  dict->Set(provider_id, ConfigToDict(cfg));
  return WriteEncrypted(*dict, WriteKind::kStoresSecrets);
}

// static
void MoltKeysStore::Remove(const std::string& provider_id) {
  std::optional<base::DictValue> dict = ReadDecrypted();
  if (!dict) {
    return;
  }
  if (dict->Remove(provider_id)) {
    WriteEncrypted(*dict, WriteKind::kRemovalOnly);
  }
}

// static
void MoltKeysStore::SetRootForTesting(const base::FilePath& root) {
  RootOverride() = root;
}

}  // namespace molt_ai
