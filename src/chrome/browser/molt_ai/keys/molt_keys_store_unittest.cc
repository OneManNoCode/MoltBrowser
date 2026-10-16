// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/keys/molt_keys_store.h"

#include <map>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/keys/molt_os_crypt.h"
#include "chrome/browser/molt_ai/keys/molt_os_crypt_test_util.h"
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
  }

  base::FilePath KeysPath() const {
    return temp_dir_.GetPath().AppendASCII("keys.enc");
  }

  std::string ReadKeysFile() const {
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(KeysPath(), &contents));
    return contents;
  }

  // A real OS-backed key (a fake keyring / Keychain / DPAPI key).
  void UseOsKey() { encryptor_.Set(test::MakeOsKeyEncryptor()); }

  base::ScopedTempDir temp_dir_;
  // Starts "not ready": no Encryptor yet.
  molt_os_crypt::ScopedEncryptorForTesting encryptor_{nullptr};
};

TEST_F(MoltKeysStoreTest, SavesAndReadsBackWithOsKey) {
  UseOsKey();
  ASSERT_TRUE(IsSecureStorageAvailable());
  ASSERT_TRUE(MoltKeysStore::Save("openai", MakeConfig("sk-test")));
  std::optional<ProviderConfig> cfg = MoltKeysStore::Get("openai");
  ASSERT_TRUE(cfg);
  EXPECT_EQ(cfg->api_key, "sk-test");
  // The key never hits the disk in the clear, and the OS key encrypted it.
  const std::string on_disk = ReadKeysFile();
  EXPECT_EQ(on_disk.find("sk-test"), std::string::npos);
  EXPECT_EQ(on_disk.compare(0, 3, test::kOsKeyTag), 0);
}

// A keys.enc that can't be read (keyring locked or reset, another key,
// corrupt file) is never rewritten: not by a save, not by a removal.
TEST_F(MoltKeysStoreTest, NeverOverwritesAnUnreadableStore) {
  UseOsKey();
  const std::string corrupt = "\x01\x02 not a key store";
  ASSERT_TRUE(base::WriteFile(KeysPath(), corrupt));
  EXPECT_TRUE(MoltKeysStore::LoadAll().empty());
  EXPECT_FALSE(MoltKeysStore::Save("openai", MakeConfig("sk-test")));
  MoltKeysStore::Remove("openai");
  EXPECT_EQ(ReadKeysFile(), corrupt);
}

// Before the Encryptor has arrived (early startup, or a keyring unlock
// prompt still open), nothing is read or written.
TEST_F(MoltKeysStoreTest, NotReadyYetFailsClosed) {
  UseOsKey();
  ASSERT_TRUE(MoltKeysStore::Save("openai", MakeConfig("sk-test")));
  const std::string saved = ReadKeysFile();

  encryptor_.Set(nullptr);
  EXPECT_FALSE(IsSecureStorageAvailable());
  EXPECT_NE(SecureStorageUnavailableMessage().find("starting up"),
            std::string::npos);
  EXPECT_TRUE(MoltKeysStore::LoadAll().empty());
  EXPECT_FALSE(MoltKeysStore::Get("openai"));
  EXPECT_FALSE(MoltKeysStore::Save("anthropic", MakeConfig("sk-new")));
  MoltKeysStore::Remove("openai");
  EXPECT_EQ(ReadKeysFile(), saved);
  std::string ciphertext = "stale";
  EXPECT_FALSE(EncryptSecretForStorage("secret", &ciphertext));
  EXPECT_TRUE(ciphertext.empty());

  UseOsKey();
  std::optional<ProviderConfig> cfg = MoltKeysStore::Get("openai");
  ASSERT_TRUE(cfg);
  EXPECT_EQ(cfg->api_key, "sk-test");
}

// The OS key store is locked: what it protects is unreadable for now and is
// left alone; no new secret goes anywhere else.
TEST_F(MoltKeysStoreTest, LockedKeyStoreRefusesAndKeepsTheFile) {
  UseOsKey();
  ASSERT_TRUE(MoltKeysStore::Save("openai", MakeConfig("sk-test")));
  const std::string saved = ReadKeysFile();

  encryptor_.Set(test::MakeLockedOsKeyEncryptor());
  EXPECT_FALSE(IsSecureStorageAvailable());
  EXPECT_TRUE(MoltKeysStore::LoadAll().empty());
  EXPECT_FALSE(MoltKeysStore::Save("anthropic", MakeConfig("sk-new")));
  MoltKeysStore::Remove("openai");
  EXPECT_EQ(ReadKeysFile(), saved);
}

TEST_F(MoltKeysStoreTest, NoKeysAtAllRefuses) {
  encryptor_.Set(test::MakeEncryptorWithoutKeys());
  EXPECT_FALSE(IsSecureStorageAvailable());
  EXPECT_FALSE(MoltKeysStore::Save("openai", MakeConfig("sk-test")));
  EXPECT_FALSE(base::PathExists(KeysPath()));
}

TEST(MoltSecretCiphertextTest, LinuxKeyringPrefix) {
  EXPECT_TRUE(IsLinuxKeyringCiphertext("v11\x01\x02"));
  EXPECT_TRUE(IsLinuxKeyringCiphertext("v12\x01\x02"));  // secret portal
  EXPECT_FALSE(IsLinuxKeyringCiphertext("v10\x01\x02"));
  EXPECT_FALSE(IsLinuxKeyringCiphertext("v20\x01\x02"));
  EXPECT_FALSE(IsLinuxKeyringCiphertext("k1\x01\x02"));
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

// What a Linux desktop without libsecret / KWallet (or with
// --password-store=basic) gets: only the hard-coded key.
void UseNoKeyring(molt_os_crypt::ScopedEncryptorForTesting& encryptor) {
  encryptor.Set(test::MakeHardCodedKeyEncryptor());
}

TEST_F(MoltKeysStoreTest, RefusesNewSecretsWithoutKeyring) {
  UseNoKeyring(encryptor_);
  // The Encryptor can encrypt, just not with a key worth the name.
  EXPECT_TRUE(molt_os_crypt::IsEncryptionAvailable());
  EXPECT_FALSE(IsSecureStorageAvailable());
  EXPECT_NE(SecureStorageUnavailableMessage().find("keyring"),
            std::string::npos);
  EXPECT_FALSE(MoltKeysStore::Save("openai", MakeConfig("sk-test")));
  EXPECT_FALSE(base::PathExists(KeysPath()));

  std::string ciphertext = "stale";
  EXPECT_FALSE(EncryptSecretForStorage("secret", &ciphertext));
  EXPECT_TRUE(ciphertext.empty());
}

// Decrypted by the real PosixKeyProvider: files the old OSCrypt wrote under
// the hard-coded key still read.
TEST_F(MoltKeysStoreTest, LegacyHardCodedKeyFileStaysReadable) {
  UseNoKeyring(encryptor_);
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
  UseOsKey();

  ASSERT_TRUE(MoltKeysStore::Save("anthropic", MakeConfig("sk-new")));
  EXPECT_TRUE(IsLinuxKeyringCiphertext(ReadKeysFile()));
  // Nothing was lost on the way.
  std::map<std::string, ProviderConfig> all = MoltKeysStore::LoadAll();
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all["openai"].api_key, "sk-legacy");
  EXPECT_EQ(all["anthropic"].api_key, "sk-new");
}

TEST_F(MoltKeysStoreTest, SecretsUseTheKeyringKey) {
  UseOsKey();
  std::string ciphertext;
  ASSERT_TRUE(EncryptSecretForStorage("secret", &ciphertext));
  EXPECT_TRUE(IsLinuxKeyringCiphertext(ciphertext));
  EXPECT_EQ(ciphertext.compare(0, 3, "v11"), 0);
}

// Flatpak / Snap: the XDG secret portal key counts as a keyring key.
TEST_F(MoltKeysStoreTest, SecretPortalKeyCountsAsSecure) {
  encryptor_.Set(test::MakeSecretPortalEncryptor());
  EXPECT_TRUE(IsSecureStorageAvailable());
  ASSERT_TRUE(MoltKeysStore::Save("openai", MakeConfig("sk-test")));
  EXPECT_EQ(ReadKeysFile().compare(0, 3, "v12"), 0);
  std::optional<ProviderConfig> cfg = MoltKeysStore::Get("openai");
  ASSERT_TRUE(cfg);
  EXPECT_EQ(cfg->api_key, "sk-test");
}

#endif  // BUILDFLAG(IS_LINUX)

}  // namespace
}  // namespace molt_ai
