// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/keys/molt_os_crypt_test_util.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/types/expected.h"
#include "components/os_crypt/async/browser/key_provider.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/algorithm.mojom.h"

#if BUILDFLAG(IS_LINUX)
#include "components/os_crypt/async/browser/posix_key_provider.h"
#endif

namespace molt_ai {
namespace test {

#if BUILDFLAG(IS_LINUX)
const char kOsKeyTag[] = "v11";
#else
const char kOsKeyTag[] = "v10";
#endif

namespace {

using os_crypt_async::Encryptor;
using os_crypt_async::KeyProvider;
using os_crypt_async::OSCryptAsync;
using os_crypt_async::mojom::Algorithm;

using Providers = std::vector<
    std::pair<OSCryptAsync::Precedence, std::unique_ptr<KeyProvider>>>;

// Hands out a fixed key under |tag|, synchronously. An empty |key| is a
// locked key store: the provider reports the key temporarily unavailable.
class FixedKeyProvider : public KeyProvider {
 public:
  FixedKeyProvider(std::string tag,
                   std::vector<uint8_t> key,
                   Algorithm algorithm)
      : tag_(std::move(tag)), key_(std::move(key)), algorithm_(algorithm) {}
  ~FixedKeyProvider() override = default;

  void GetKey(KeyCallback callback) override {
    if (key_.empty()) {
      std::move(callback).Run(
          tag_, base::unexpected(KeyError::kTemporarilyUnavailable));
      return;
    }
    std::move(callback).Run(tag_, Encryptor::Key(key_, algorithm_));
  }

  bool UseForEncryption() override { return true; }

 private:
  const std::string tag_;
  const std::vector<uint8_t> key_;
  const Algorithm algorithm_;
};

std::vector<uint8_t> TestKeyBytes(size_t size, uint8_t seed) {
  std::vector<uint8_t> key(size);
  for (size_t i = 0; i < size; ++i) {
    key[i] = static_cast<uint8_t>(seed * 37u + i * 11u + 1u);
  }
  return key;
}

std::unique_ptr<KeyProvider> OsKeyProvider(std::vector<uint8_t> key) {
  // AES-128-CBC like the Linux Secret Service and macOS Keychain keys.
  return std::make_unique<FixedKeyProvider>(kOsKeyTag, std::move(key),
                                            Algorithm::kAES128CBC);
}

void AddHardCodedKeyProvider(Providers& providers) {
#if BUILDFLAG(IS_LINUX)
  // Precedence 5, as BrowserProcessImpl registers it.
  providers.emplace_back(5u,
                         std::make_unique<os_crypt_async::PosixKeyProvider>());
#endif
}

scoped_refptr<Encryptor> Build(Providers providers) {
  OSCryptAsync os_crypt_async(std::move(providers));
  scoped_refptr<Encryptor> encryptor;
  // Every provider here answers synchronously, so the Encryptor arrives
  // before GetInstance() returns.
  os_crypt_async.GetInstance(base::BindOnce(
      [](scoped_refptr<Encryptor>* out, scoped_refptr<Encryptor> result) {
        *out = std::move(result);
      },
      base::Unretained(&encryptor)));
  CHECK(encryptor);
  return encryptor;
}

}  // namespace

scoped_refptr<Encryptor> MakeOsKeyEncryptor(uint8_t key_seed) {
  Providers providers;
  AddHardCodedKeyProvider(providers);
  providers.emplace_back(10u, OsKeyProvider(TestKeyBytes(16, key_seed)));
  return Build(std::move(providers));
}

scoped_refptr<Encryptor> MakeLockedOsKeyEncryptor() {
  Providers providers;
  AddHardCodedKeyProvider(providers);
  providers.emplace_back(10u, OsKeyProvider(/*key=*/{}));
  return Build(std::move(providers));
}

scoped_refptr<Encryptor> MakeEncryptorWithoutKeys() {
  return Build(Providers());
}

#if BUILDFLAG(IS_LINUX)
scoped_refptr<Encryptor> MakeHardCodedKeyEncryptor() {
  Providers providers;
  AddHardCodedKeyProvider(providers);
  return Build(std::move(providers));
}

scoped_refptr<Encryptor> MakeSecretPortalEncryptor() {
  Providers providers;
  AddHardCodedKeyProvider(providers);
  providers.emplace_back(10u, OsKeyProvider(TestKeyBytes(16, 1)));
  providers.emplace_back(
      15u, std::make_unique<FixedKeyProvider>("v12", TestKeyBytes(32, 9),
                                              Algorithm::kAES256GCM));
  return Build(std::move(providers));
}
#endif  // BUILDFLAG(IS_LINUX)

}  // namespace test
}  // namespace molt_ai
