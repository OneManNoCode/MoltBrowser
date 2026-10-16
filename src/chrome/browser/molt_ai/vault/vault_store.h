// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// VaultStore — encrypted on-device password vault for MoltBrowser.
//
// Stores a JSON array of credential entries at
//   ~/.moltbrowser/vault.enc
// encrypted with the os_crypt_async Encryptor through molt_os_crypt (OS
// Keychain on macOS / DPAPI on Windows / Secret Service or KWallet on
// Linux). The blob is small so we read/write the whole thing on every
// change; no streaming or partial-update path.
//
// Why a separate file from profile.enc:
//   - Credentials are the highest-sensitivity blob the user owns.
//     Isolating them in their own file means a future change (e.g.
//     adding a second crypto layer on top of the OS key, or backing
//     this with a hardware security module) only touches one path.
//   - The form-filler profile is already a different shape (single
//     dict of name/email/etc); credentials are an array.
//
// Threading: any thread; synchronous file I/O, so wrap UI-thread callers in
// ScopedAllowBlockingForMolt. Encryption doesn't block on the OS key store.
// Until molt_os_crypt has its Encryptor (early startup), List() is empty and
// writes are refused, as for a locked keyring.

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
// when the entry is in scope — the on-disk form is encrypted at file
// granularity (not field granularity: the OS key is symmetric, and the
// cost-benefit of a second crypto layer for a single field is marginal vs.
// holding the whole vault open in RAM during a form fill).
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

  // Full on-disk form, password included. Never send this to a WebUI.
  base::DictValue ToDict() const;
  static std::optional<VaultEntry> FromDict(const base::DictValue& d);

  // What a WebUI may see when listing entries: id, site_host, username,
  // timestamps and whether there are notes. No password, no notes text.
  base::DictValue ToMetadataDict() const;
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
  // the assigned id, empty string on failure — including when there is
  // no real OS key store (molt_ai::IsSecureStorageAvailable()), since the
  // Encryptor would then use a hard-coded key (or isn't ready yet).
  std::string Add(VaultEntry entry);

  // Replace an existing entry by id. Bumps last_changed_unix if the
  // password changed. Returns false if the id isn't found, or (like
  // Add) when there is no real OS key store.
  bool Update(const std::string& id, VaultEntry entry);

  // Remove the entry with this id. True if it was there. Allowed
  // without a secure backend: it only drops data already on disk.
  bool Delete(const std::string& id);

  // Bump last_used_unix on the entry with this id. Cheap convenience
  // for the autofill path — caller doesn't have to round-trip a
  // full Update. Rewrites only data that is already stored, so like
  // Delete it doesn't need a secure backend.
  bool TouchLastUsed(const std::string& id);

  // Every entry that may be offered for a page on |host| (canonical,
  // e.g. url::Origin::host()): exact host matches first, then entries
  // for the same registrable domain (see vault_host_match.h — those
  // need user confirmation before filling). Within each group, newest
  // last_used_unix first.
  std::vector<VaultEntry> FindForHost(const std::string& host) const;

  // The entry with this id, or std::nullopt.
  std::optional<VaultEntry> Get(const std::string& id) const;

  // Wipe the vault file. True if the file is gone after the call.
  bool Clear();

  // Test seam.
  void SetRootForTesting(const base::FilePath& root);

 private:
  base::FilePath GetVaultPath() const;
  // Read+decrypt the whole vault. Returns std::nullopt if the file
  // exists but won't decrypt (corrupt / wrong keychain).
  std::optional<std::vector<VaultEntry>> ReadDecrypted() const;
  // Encrypt+write the whole vault. Atomic via .tmp + rename. When
  // |adds_secrets| is true the write is refused unless a real OS key
  // store backs the Encryptor (molt_ai::EncryptSecretForStorage()).
  bool WriteEncrypted(const std::vector<VaultEntry>& entries,
                      bool adds_secrets);

  base::FilePath root_override_;
};

}  // namespace vault
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VAULT_VAULT_STORE_H_
