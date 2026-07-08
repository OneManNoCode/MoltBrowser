// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltKeysStore — OSCrypt-encrypted store for cloud-provider API keys.
//
// Stores a JSON dict of provider configs at
//   ~/.moltbrowser/keys.enc
// encrypted via OSCrypt (OS keychain on macOS / DPAPI on Windows /
// libsecret on Linux). The blob is tiny (one entry per configured
// frontier provider) so we read/write the whole thing on every change;
// no streaming or partial-update path — the exact same idiom as
// molt_ai::vault::VaultStore.
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
// Threading: methods are UI-thread-safe but synchronous on disk +
// keychain. Wrap callers in ScopedAllowBlockingForMolt.

#ifndef CHROME_BROWSER_MOLT_AI_KEYS_MOLT_KEYS_STORE_H_
#define CHROME_BROWSER_MOLT_AI_KEYS_MOLT_KEYS_STORE_H_

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"

namespace molt_ai {

// One provider's stored configuration. `api_key` is plaintext only
// while this struct is in scope — the on-disk form is OSCrypt-encrypted
// at file granularity.
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
  // file.
  static void Save(const std::string& provider_id, const ProviderConfig& cfg);

  // Remove `provider_id` if present. No-op if absent.
  static void Remove(const std::string& provider_id);

  // Test seam — redirects the store root away from ~/.moltbrowser.
  static void SetRootForTesting(const base::FilePath& root);
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_KEYS_MOLT_KEYS_STORE_H_
