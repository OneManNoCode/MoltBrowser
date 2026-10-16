// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltKeysStore — encrypted store for cloud-provider API keys.
//
// Stores a JSON dict of provider configs at
//   ~/.moltbrowser/keys.enc
// encrypted with the os_crypt_async Encryptor through molt_os_crypt (OS
// Keychain on macOS / DPAPI on Windows / Secret Service or KWallet on
// Linux). The blob is tiny (one entry per configured frontier provider) so
// we read/write the whole thing on every change; no streaming or
// partial-update path — the exact same idiom as molt_ai::vault::VaultStore.
//
// On-disk JSON shape:
//   {
//     "openai": {
//       "api_key": "sk-...",
//       "base_url": "https://api.openai.com/v1",
//       "enabled_models": ["gpt-5.1", ...]
//     },
//     "anthropic": { ... },
//     ...
//   }
//
// Security notes:
//   - API keys are the highest-sensitivity blob after the password
//     vault. They live in their own file so a future second crypto
//     layer only touches one path.
//   - NEVER log a key. NEVER write a key into settings.json — keys go
//     here and nowhere else.
//   - Callers should SecureClear() any transient plaintext key buffers
//     (molt_ai::chat_handler_helpers::SecureClear) after use.
//
// Threading: any thread; synchronous file I/O, so wrap UI-thread callers in
// ScopedAllowBlockingForMolt. Encryption never blocks on the OS key store
// (molt_os_crypt fetched the key at startup). Until that key has arrived,
// reads come back empty and writes are refused, as for a locked keyring.

#ifndef CHROME_BROWSER_MOLT_AI_KEYS_MOLT_KEYS_STORE_H_
#define CHROME_BROWSER_MOLT_AI_KEYS_MOLT_KEYS_STORE_H_

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"

namespace molt_ai {

// One provider's stored configuration. `api_key` is plaintext only
// while this struct is in scope — the on-disk form is encrypted at file
// granularity.
struct ProviderConfig {
  ProviderConfig();
  ProviderConfig(const ProviderConfig&);
  ProviderConfig(ProviderConfig&&);
  ProviderConfig& operator=(const ProviderConfig&);
  ProviderConfig& operator=(ProviderConfig&&);
  ~ProviderConfig();

  std::string api_key;
  std::string base_url;  // empty => use the provider's default_base_url
  std::vector<std::string> enabled_models;
};

// True when new secrets would be encrypted with a real OS-backed key (macOS
// Keychain, Windows DPAPI, the Secret Service / KWallet or the XDG secret
// portal on Linux). On Linux without a keyring the Encryptor falls back to a
// hard-coded key ("v10"), which is obfuscation, not encryption: anyone who
// can read the file can decrypt it. MoltBrowser's secret stores (keys.enc,
// vault.enc, profile.enc) refuse to write new secrets while this is false.
// Files written earlier stay readable, and removing entries is still allowed.
//
// Also false until molt_os_crypt has its Encryptor (early in startup; on
// Linux possibly until the user answers a keyring unlock prompt). Never
// blocks.
//
// Implementation: the Encryptor must be able to encrypt, and on Linux a probe
// encrypted with it must carry a keyring tag (see IsLinuxKeyringCiphertext).
// The Encryptor is fixed for the browser session, so a keyring that comes up
// later only counts after a restart.
bool IsSecureStorageAvailable();

// User-facing explanation to surface when IsSecureStorageAvailable() is
// false.
std::string SecureStorageUnavailableMessage();

// Encrypts |plaintext| (non-empty) for one of the secret stores. Succeeds
// only when the result is protected by an OS-backed key: the check above
// must pass and, on Linux, the output must carry a keyring tag ("v11" or
// "v12"), never the hard-coded-key tag ("v10"). On failure |ciphertext| is
// left empty and nothing should be written.
bool EncryptSecretForStorage(const std::string& plaintext,
                             std::string* ciphertext);

// Pure helper behind the Linux check above: true if |ciphertext| was made
// with a key from the user's keyring. On Linux, "v11" is the Secret Service /
// KWallet key (FreedesktopSecretKeyProvider, the old OSCrypt's v11 key) and
// "v12" the XDG secret portal key (SecretPortalKeyProvider). "v10" is the
// hard-coded key (PosixKeyProvider). Any other or unknown tag is rejected.
// Exposed for tests.
bool IsLinuxKeyringCiphertext(const std::string& ciphertext);

// All static — the store is stateless apart from the on-disk file and
// an optional test-root override. Every call reads or writes the whole
// keys.enc; no long-lived plaintext is held in memory.
class MoltKeysStore {
 public:
  // Read every provider config from disk. Empty map on first run or
  // decrypt failure.
  static std::map<std::string, ProviderConfig> LoadAll();

  // Read one provider's config. std::nullopt if not present (or on
  // decrypt failure).
  static std::optional<ProviderConfig> Get(const std::string& provider_id);

  // Insert-or-replace the config for `provider_id`. Rewrites the whole
  // file. Returns false (and writes nothing) if the existing store can't
  // be read, the write fails, or IsSecureStorageAvailable() is false.
  static bool Save(const std::string& provider_id, const ProviderConfig& cfg);

  // Remove `provider_id` if present. No-op if absent. Works without a
  // secure backend too: it only ever drops data that is already on disk.
  static void Remove(const std::string& provider_id);

  // Test seam — redirects the store root away from ~/.moltbrowser.
  static void SetRootForTesting(const base::FilePath& root);
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_KEYS_MOLT_KEYS_STORE_H_
