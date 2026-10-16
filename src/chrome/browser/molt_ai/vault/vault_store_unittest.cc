// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/vault/vault_store.h"

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/keys/molt_keys_store.h"
#include "components/os_crypt/sync/os_crypt_mocker.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace vault {
namespace {

VaultEntry MakeEntry(const std::string& host, const std::string& password) {
  VaultEntry e;
  e.site_host = host;
  e.username = "bob";
  e.password = password;
  return e;
}

class VaultStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    store_.SetRootForTesting(temp_dir_.GetPath());
  }

  void TearDown() override { OSCryptMocker::TearDown(); }

  base::FilePath VaultPath() const {
    return temp_dir_.GetPath().AppendASCII("vault.enc");
  }

  bool UseOsKey() {
    OSCryptMocker::TearDown();
    OSCryptMocker::SetUp();
    return IsSecureStorageAvailable();
  }

  base::ScopedTempDir temp_dir_;
  VaultStore store_;
};

TEST_F(VaultStoreTest, AddsAndListsWithOsKey) {
  if (!UseOsKey()) {
    GTEST_SKIP() << "no OSCrypt mock in this configuration";
  }
  const std::string id = store_.Add(MakeEntry("github.com", "pw1"));
  ASSERT_FALSE(id.empty());
  std::vector<VaultEntry> all = store_.List();
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].password, "pw1");
  std::string on_disk;
  ASSERT_TRUE(base::ReadFileToString(VaultPath(), &on_disk));
  EXPECT_EQ(on_disk.find("pw1"), std::string::npos);
}

// A vault.enc that can't be read (keyring locked or reset, another key,
// corrupt file) is never rewritten, whatever the operation.
TEST_F(VaultStoreTest, NeverOverwritesAnUnreadableVault) {
  if (!UseOsKey()) {
    GTEST_SKIP() << "no OSCrypt mock in this configuration";
  }
  const std::string corrupt = "\x01\x02 not a vault";
  ASSERT_TRUE(base::WriteFile(VaultPath(), corrupt));
  EXPECT_TRUE(store_.List().empty());
  EXPECT_TRUE(store_.Add(MakeEntry("github.com", "pw1")).empty());
  EXPECT_FALSE(store_.Update("someid", MakeEntry("github.com", "pw2")));
  EXPECT_FALSE(store_.Delete("someid"));
  EXPECT_FALSE(store_.TouchLastUsed("someid"));
  std::string after;
  ASSERT_TRUE(base::ReadFileToString(VaultPath(), &after));
  EXPECT_EQ(after, corrupt);
}

#if BUILDFLAG(IS_LINUX)

// vault.enc as an older build wrote it with no keyring (OSCrypt v10,
// hard-coded key): one entry, example.com / alice / hunter2.
constexpr char kLegacyV10VaultHex[] =
    "763130011d0fcbb5e889610b49b8963f6f394b0a44de7e2ac454440bb5d4859cf3c525"
    "5b4e3b0f131b01a2b88a84e74633b011f6e9e363cde3663274c7041460311f91feebdc"
    "7247946f783da5a68fd0c1d94551b3f58e95a6d01635fb10e9dbfe0ec9dcd8d7e41732"
    "6e46a68c4519f5cf98c7efa431d7d31173d64633435d0bb89f327a7e0e10929da519be"
    "2c926fcd752b85817a40bc5ca24c8c1288aec5ed1e97dd";

TEST_F(VaultStoreTest, NoKeyringRefusesNewCredentialsButKeepsOldOnes) {
  OSCryptMocker::TearDown();  // no key storage backend
  ASSERT_FALSE(IsSecureStorageAvailable());

  std::string legacy;
  ASSERT_TRUE(base::HexStringToString(kLegacyV10VaultHex, &legacy));
  ASSERT_TRUE(base::WriteFile(VaultPath(), legacy));

  // Existing entries still read.
  std::vector<VaultEntry> all = store_.List();
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].site_host, "example.com");
  EXPECT_EQ(all[0].password, "hunter2");

  // New or changed credentials are refused; nothing on disk changes.
  EXPECT_TRUE(store_.Add(MakeEntry("github.com", "pw1")).empty());
  EXPECT_FALSE(store_.Update("legacyid", MakeEntry("example.com", "pw2")));
  std::string after;
  ASSERT_TRUE(base::ReadFileToString(VaultPath(), &after));
  EXPECT_EQ(after, legacy);

  // Deleting still works.
  EXPECT_TRUE(store_.Delete("legacyid"));
  EXPECT_TRUE(store_.List().empty());
}

#endif  // BUILDFLAG(IS_LINUX)

}  // namespace
}  // namespace vault
}  // namespace molt_ai
