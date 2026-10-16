// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/keys/molt_keys_store.h"

#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "components/os_crypt/sync/os_crypt_mocker.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

ProviderConfig MakeConfig(const std::string& api_key) {
  ProviderConfig cfg;
  cfg.api_key = api_key;
  cfg.enabled_models = {"m1"};
  return cfg;
}

class MoltKeysStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    MoltKeysStore::SetRootForTesting(temp_dir_.GetPath());
  }

  void TearDown() override {
    MoltKeysStore::SetRootForTesting(base::FilePath());
    OSCryptMocker::TearDown();
  }

  base::FilePath KeysPath() const {
    return temp_dir_.GetPath().AppendASCII("keys.enc");
  }

  std::string ReadKeysFile() const {
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(KeysPath(), &contents));
    return contents;
  }

  // A real OS-backed key (a mock keyring / keychain / DPAPI key).
  // Returns false where this build has no OSCrypt mock to install.
  bool UseOsKey() {
    OSCryptMocker::TearDown();  // Linux: forget a cached "no backend".
    OSCryptMocker::SetUp();
    return IsSecureStorageAvailable();
  }

  base::ScopedTempDir temp_dir_;
};

TEST_F(MoltKeysStoreTest, SavesAndReadsBackWithOsKey) {
  if (!UseOsKey()) {
    GTEST_SKIP() << "no OSCrypt mock in this configuration";
  }
  ASSERT_TRUE(MoltKeysStore::Save("openai", MakeConfig("sk-test")));
  std::optional<ProviderConfig> cfg = MoltKeysStore::Get("openai");
  ASSERT_TRUE(cfg);
  EXPECT_EQ(cfg->api_key, "sk-test");
  // The key never hits the disk in the clear.
  EXPECT_EQ(ReadKeysFile().find("sk-test"), std::string::npos);
}

// A keys.enc that can't be read (keyring locked or reset, another key,
// corrupt file) is never rewritten: not by a save, not by a removal.
TEST_F(MoltKeysStoreTest, NeverOverwritesAnUnreadableStore) {
  if (!UseOsKey()) {
    GTEST_SKIP() << "no OSCrypt mock in this configuration";
  }
  const std::string corrupt = "\x01\x02 not a key store";
  ASSERT_TRUE(base::WriteFile(KeysPath(), corrupt));
  EXPECT_TRUE(MoltKeysStore::LoadAll().empty());
  EXPECT_FALSE(MoltKeysStore::Save("openai", MakeConfig("sk-test")));
  MoltKeysStore::Remove("openai");
  EXPECT_EQ(ReadKeysFile(), corrupt);
}

TEST(MoltSecretCiphertextTest, LinuxKeyringPrefix) {
  EXPECT_TRUE(IsLinuxKeyringCiphertext("v11\x01\x02"));
  EXPECT_FALSE(IsLinuxKeyringCiphertext("v10\x01\x02"));
  EXPECT_FALSE(IsLinuxKeyringCiphertext("v11"));  // prefix only, no payload
  EXPECT_FALSE(IsLinuxKeyringCiphertext(""));
  EXPECT_FALSE(IsLinuxKeyringCiphertext("plaintext"));
}

#if BUILDFLAG(IS_LINUX)

// keys.enc as an older build wrote it on a desktop with no keyring: OSCrypt
// v10, i.e. AES-128-CBC under the hard-coded "peanuts" key, of
// {"openai":{"api_key":"sk-legacy","base_url":"","enabled_models":[]}}.
constexpr char kLegacyV10KeysHex[] =
    "763130062e6329ea570e279f78c24beacd486ba8a6fecb7f35e5cb8c23fe5b43a1181f"
    "c4e936a36c604b9a0006eddc1c2ecf30c7bd73874c70f9de601783a4d6c3af6adab0f1"
    "b8ed119f40a1fbb69b35e98f71";

// OSCrypt with no key storage backend: what a Linux desktop without
// libsecret / KWallet (or with --password-store=basic) gets.
void UseNoKeyring() {
  OSCryptMocker::TearDown();
}

TEST_F(MoltKeysStoreTest, RefusesNewSecretsWithoutKeyring) {
  UseNoKeyring();
  EXPECT_FALSE(IsSecureStorageAvailable());
  EXPECT_FALSE(MoltKeysStore::Save("openai", MakeConfig("sk-test")));
  EXPECT_FALSE(base::PathExists(KeysPath()));

  std::string ciphertext = "stale";
  EXPECT_FALSE(EncryptSecretForStorage("secret", &ciphertext));
  EXPECT_TRUE(ciphertext.empty());
}

TEST_F(MoltKeysStoreTest, LegacyHardCodedKeyFileStaysReadable) {
  UseNoKeyring();
  std::string legacy;
  ASSERT_TRUE(base::HexStringToString(kLegacyV10KeysHex, &legacy));
  ASSERT_TRUE(base::WriteFile(KeysPath(), legacy));

  std::map<std::string, ProviderConfig> all = MoltKeysStore::LoadAll();
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all["openai"].api_key, "sk-legacy");

  // A new key is refused and the existing file is left untouched...
  EXPECT_FALSE(MoltKeysStore::Save("anthropic", MakeConfig("sk-new")));
  EXPECT_EQ(ReadKeysFile(), legacy);

  // ...but the user can still remove what is already there.
  MoltKeysStore::Remove("openai");
  EXPECT_TRUE(MoltKeysStore::LoadAll().empty());
}

TEST_F(MoltKeysStoreTest, LegacyFileMovesToKeyringKeyOnNextSave) {
  std::string legacy;
  ASSERT_TRUE(base::HexStringToString(kLegacyV10KeysHex, &legacy));
  ASSERT_TRUE(base::WriteFile(KeysPath(), legacy));
  if (!UseOsKey()) {
    GTEST_SKIP() << "no OSCrypt mock in this configuration";
  }

  ASSERT_TRUE(MoltKeysStore::Save("anthropic", MakeConfig("sk-new")));
  EXPECT_TRUE(IsLinuxKeyringCiphertext(ReadKeysFile()));
  // Nothing was lost on the way.
  std::map<std::string, ProviderConfig> all = MoltKeysStore::LoadAll();
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all["openai"].api_key, "sk-legacy");
  EXPECT_EQ(all["anthropic"].api_key, "sk-new");
}

TEST_F(MoltKeysStoreTest, SecretsUseTheKeyringKey) {
  if (!UseOsKey()) {
    GTEST_SKIP() << "no OSCrypt mock in this configuration";
  }
  std::string ciphertext;
  ASSERT_TRUE(EncryptSecretForStorage("secret", &ciphertext));
  EXPECT_TRUE(IsLinuxKeyringCiphertext(ciphertext));
}

#endif  // BUILDFLAG(IS_LINUX)

}  // namespace
}  // namespace molt_ai
