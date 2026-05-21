// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/vault/vault_store.h"

#include <algorithm>
#include <ctime>
#include <utility>

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "components/os_crypt/sync/os_crypt.h"

namespace molt_ai {
namespace vault {

namespace {

constexpr char kRootDirName[]  = ".moltbrowser";
constexpr char kVaultFile[]    = "vault.enc";

// Generate a 128-bit random id rendered as 32 hex chars.
std::string MintId() {
  uint8_t buf[16];
  base::RandBytes(buf);
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (uint8_t b : buf) {
    out += kHex[(b >> 4) & 0xF];
    out += kHex[b & 0xF];
  }
  return out;
}

int64_t Now() {
  return static_cast<int64_t>(time(nullptr));
}

}  // namespace

VaultEntry::VaultEntry() = default;
VaultEntry::VaultEntry(const VaultEntry&) = default;
VaultEntry::VaultEntry(VaultEntry&&) = default;
VaultEntry& VaultEntry::operator=(const VaultEntry&) = default;
VaultEntry& VaultEntry::operator=(VaultEntry&&) = default;
VaultEntry::~VaultEntry() = default;

base::DictValue VaultEntry::ToDict() const {
  base::DictValue d;
  d.Set("id", id);
  d.Set("site_host", site_host);
  d.Set("username", username);
  d.Set("password", password);
  d.Set("notes", notes);
  d.Set("created_at_unix", static_cast<double>(created_at_unix));
  d.Set("last_used_unix", static_cast<double>(last_used_unix));
  d.Set("last_changed_unix", static_cast<double>(last_changed_unix));
  return d;
}

// static
std::optional<VaultEntry> VaultEntry::FromDict(const base::DictValue& d) {
  VaultEntry e;
  const std::string* id = d.FindString("id");
  if (!id || id->empty()) return std::nullopt;
  e.id = *id;
  if (auto* s = d.FindString("site_host")) e.site_host = *s;
  if (auto* s = d.FindString("username"))  e.username = *s;
  if (auto* s = d.FindString("password"))  e.password = *s;
  if (auto* s = d.FindString("notes"))     e.notes = *s;
  auto created = d.FindDouble("created_at_unix");
  if (created) e.created_at_unix = static_cast<int64_t>(*created);
  auto last_used = d.FindDouble("last_used_unix");
  if (last_used) e.last_used_unix = static_cast<int64_t>(*last_used);
  auto last_changed = d.FindDouble("last_changed_unix");
  if (last_changed) e.last_changed_unix = static_cast<int64_t>(*last_changed);
  return e;
}

VaultStore::VaultStore() = default;
VaultStore::~VaultStore() = default;

void VaultStore::SetRootForTesting(const base::FilePath& root) {
  root_override_ = root;
}

base::FilePath VaultStore::GetVaultPath() const {
  base::FilePath root;
  if (!root_override_.empty()) {
    root = root_override_;
  } else {
    base::PathService::Get(base::DIR_HOME, &root);
    root = root.AppendASCII(kRootDirName);
  }
  return root.AppendASCII(kVaultFile);
}

std::optional<std::vector<VaultEntry>> VaultStore::ReadDecrypted() const {
  base::FilePath path = GetVaultPath();
  if (!base::PathExists(path)) {
    return std::vector<VaultEntry>{};
  }
  std::string ciphertext;
  if (!base::ReadFileToString(path, &ciphertext)) {
    return std::nullopt;
  }
  if (ciphertext.empty()) return std::vector<VaultEntry>{};
  std::string plaintext;
  if (!OSCrypt::DecryptString(ciphertext, &plaintext)) {
    LOG(WARNING) << "[VaultStore] decrypt failed";
    return std::nullopt;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(plaintext, /*options=*/0);
  if (!parsed || !parsed->is_list()) {
    LOG(WARNING) << "[VaultStore] parse failed or not a list";
    return std::nullopt;
  }
  std::vector<VaultEntry> out;
  for (const base::Value& v : parsed->GetList()) {
    if (!v.is_dict()) continue;
    auto e = VaultEntry::FromDict(v.GetDict());
    if (e) out.push_back(*std::move(e));
  }
  return out;
}

bool VaultStore::WriteEncrypted(const std::vector<VaultEntry>& entries) {
  base::FilePath path = GetVaultPath();
  base::FilePath dir = path.DirName();
  if (!base::DirectoryExists(dir) && !base::CreateDirectory(dir)) {
    return false;
  }
  base::ListValue list;
  for (const auto& e : entries) {
    list.Append(e.ToDict());
  }
  std::string plaintext;
  if (!base::JSONWriter::Write(list, &plaintext)) return false;
  std::string ciphertext;
  if (!OSCrypt::EncryptString(plaintext, &ciphertext)) {
    LOG(WARNING) << "[VaultStore] encrypt failed";
    return false;
  }
  base::FilePath tmp = path.AddExtensionASCII(".tmp");
  if (!base::WriteFile(tmp, ciphertext)) return false;
  if (!base::ReplaceFile(tmp, path, /*error=*/nullptr)) {
    base::DeleteFile(tmp);
    return false;
  }
  return true;
}

std::vector<VaultEntry> VaultStore::List() const {
  auto v = ReadDecrypted();
  if (!v) return {};
  return *std::move(v);
}

std::string VaultStore::Add(VaultEntry entry) {
  auto v = ReadDecrypted();
  if (!v) return {};
  entry.id = MintId();
  if (entry.created_at_unix == 0) entry.created_at_unix = Now();
  if (entry.last_changed_unix == 0) entry.last_changed_unix = entry.created_at_unix;
  std::string assigned = entry.id;
  v->push_back(std::move(entry));
  if (!WriteEncrypted(*v)) return {};
  return assigned;
}

bool VaultStore::Update(const std::string& id, VaultEntry entry) {
  auto v = ReadDecrypted();
  if (!v) return false;
  for (auto& e : *v) {
    if (e.id != id) continue;
    if (entry.password != e.password) {
      entry.last_changed_unix = Now();
    } else {
      entry.last_changed_unix = e.last_changed_unix;
    }
    entry.id = id;
    entry.created_at_unix = e.created_at_unix;
    e = std::move(entry);
    return WriteEncrypted(*v);
  }
  return false;
}

bool VaultStore::Delete(const std::string& id) {
  auto v = ReadDecrypted();
  if (!v) return false;
  auto it = std::remove_if(v->begin(), v->end(),
                           [&](const VaultEntry& e){ return e.id == id; });
  if (it == v->end()) return false;
  v->erase(it, v->end());
  return WriteEncrypted(*v);
}

bool VaultStore::TouchLastUsed(const std::string& id) {
  auto v = ReadDecrypted();
  if (!v) return false;
  for (auto& e : *v) {
    if (e.id != id) continue;
    e.last_used_unix = Now();
    return WriteEncrypted(*v);
  }
  return false;
}

std::vector<VaultEntry> VaultStore::FindForHost(
    const std::string& host) const {
  std::vector<VaultEntry> matches;
  if (host.empty()) return matches;
  for (const auto& e : List()) {
    if (e.site_host.empty()) continue;
    if (e.site_host == host) {
      matches.push_back(e);
      continue;
    }
    // Suffix match — "github.com" matches "gist.github.com".
    if (host.size() > e.site_host.size() + 1) {
      std::string dotted = "." + e.site_host;
      if (host.compare(host.size() - dotted.size(), dotted.size(),
                        dotted) == 0) {
        matches.push_back(e);
      }
    }
  }
  std::sort(matches.begin(), matches.end(),
            [](const VaultEntry& a, const VaultEntry& b) {
              return a.last_used_unix > b.last_used_unix;
            });
  return matches;
}

bool VaultStore::Clear() {
  base::FilePath path = GetVaultPath();
  if (!base::PathExists(path)) return true;
  return base::DeleteFile(path);
}

}  // namespace vault
}  // namespace molt_ai
