// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// VaultStore — encrypted on-device password vault for MoltBrowser.
//
// Stores a JSON array of credential entries at
//   ~/.moltbrowser/vault.enc
// encrypted via OSCrypt (OS keychain on macOS / DPAPI on Windows /
// libsecret on Linux). The blob is small so we read/write the whole
// thing on every change; no streaming or partial-update path.
//
// Why a separate file from profile.enc:
//   - Credentials are the highest-sensitivity blob the user owns.
//     Isolating them in their own file means a future change (e.g.
//     adding a second crypto layer on top of OSCrypt, or backing
//     this with a hardware security module) only touches one path.
//   - The form-filler profile is already a different shape (single
//     dict of name/email/etc); credentials are an array.
//
// Threading: methods are UI-thread-safe but synchronous on disk +
// keychain. Wrap callers in ScopedAllowBlockingForMolt.

#ifndef CHROME_BROWSER_MOLT_AI_VAULT_VAULT_STORE_H_
#define CHROME_BROWSER_MOLT_AI_VAULT_VAULT_STORE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/values.h"

namespace molt_ai {
namespace vault {

// One stored credential. The plaintext password lives only in memory
// when the entry is in scope — the on-disk form is encrypted via
// OSCrypt at file granularity (not field granularity, since OSCrypt
// is symmetric, and the cost-benefit of a second crypto layer for a
// single field is marginal vs. holding the whole vault open in RAM
// during a form fill).
struct VaultEntry {
  VaultEntry();
  VaultEntry(const VaultEntry&);
  VaultEntry(VaultEntry&&);
  VaultEntry& operator=(const VaultEntry&);
  VaultEntry& operator=(VaultEntry&&);
  ~VaultEntry();

  std::string id;                  // opaque random id, set on Add()
  std::string site_host;           // e.g. "github.com" — used for match
  std::string username;
  std::string password;
  std::string notes;
  int64_t created_at_unix = 0;
  int64_t last_used_unix = 0;      // bumped on autofill
  int64_t last_changed_unix = 0;   // bumped on password rotation

  base::DictValue ToDict() const;
  static std::optional<VaultEntry> FromDict(const base::DictValue& d);
};

class VaultStore {
 public:
  VaultStore();
  ~VaultStore();

  VaultStore(const VaultStore&) = delete;
  VaultStore& operator=(const VaultStore&) = delete;

  // Read every entry from disk. Empty vector on first run or decrypt
  // failure. The caller decides whether decrypt failure should
  // surface as an error or be treated as "no vault yet".
  std::vector<VaultEntry> List() const;

  // Insert a new entry. Assigns a fresh id and timestamps. Returns
  // the assigned id, empty string on failure.
  std::string Add(VaultEntry entry);

  // Replace an existing entry by id. Bumps last_changed_unix if the
  // password changed. Returns false if the id isn't found.
  bool Update(const std::string& id, VaultEntry entry);

  // Remove the entry with this id. True if it was there.
  bool Delete(const std::string& id);

  // Bump last_used_unix on the entry with this id. Cheap convenience
  // for the autofill path — caller doesn't have to round-trip a
  // full Update.
  bool TouchLastUsed(const std::string& id);

  // Convenience: every entry whose site_host matches |host| or is a
  // suffix of it ("github.com" matches "gist.github.com"). Newest
  // last_used_unix first.
  std::vector<VaultEntry> FindForHost(const std::string& host) const;

  // Wipe the vault file. True if the file is gone after the call.
  bool Clear();

  // Test seam.
  void SetRootForTesting(const base::FilePath& root);

 private:
  base::FilePath GetVaultPath() const;
  // Read+decrypt the whole vault. Returns std::nullopt if the file
  // exists but won't decrypt (corrupt / wrong keychain).
  std::optional<std::vector<VaultEntry>> ReadDecrypted() const;
  // Encrypt+write the whole vault. Atomic via .tmp + rename.
  bool WriteEncrypted(const std::vector<VaultEntry>& entries);

  base::FilePath root_override_;
};

}  // namespace vault
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VAULT_VAULT_STORE_H_
