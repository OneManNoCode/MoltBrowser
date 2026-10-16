// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/profile/molt_profile_store.h"

#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/keys/molt_keys_store.h"
#include "components/os_crypt/sync/os_crypt_mocker.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace profile {
namespace {

// Not an OSCrypt blob of a JSON dict on any platform: a truncated or
// corrupt file.
constexpr char kCorrupt[] = "\x01\x02 not a profile";

base::DictValue MakeProfile(const std::string& name) {
  base::DictValue d;
  d.Set(kFieldFullName, name);
  d.Set(kFieldEmail, "someone@example.com");
  return d;
}

std::string NameOf(const base::DictValue& d) {
  const std::string* s = d.FindString(kFieldFullName);
  return s ? *s : std::string();
}

class MoltProfileStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    store_.SetRootForTesting(temp_dir_.GetPath());
  }

  void TearDown() override { OSCryptMocker::TearDown(); }

  base::FilePath ProfilePath() const {
    return temp_dir_.GetPath().AppendASCII("profile.enc");
  }

  std::string ReadProfileFile() const {
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(ProfilePath(), &contents));
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
  MoltProfileStore store_;
};

TEST_F(MoltProfileStoreTest, SavesAndLoadsWithOsKey) {
  if (!UseOsKey()) {
    GTEST_SKIP() << "no OSCrypt mock in this configuration";
  }
  EXPECT_EQ(store_.LoadWithStatus().status, ProfileReadStatus::kAbsent);
  ASSERT_EQ(store_.Save(MakeProfile("Ada")), ProfileSaveResult::kSaved);

  ProfileLoadResult loaded = store_.LoadWithStatus();
  EXPECT_EQ(loaded.status, ProfileReadStatus::kOk);
  EXPECT_EQ(NameOf(loaded.profile), "Ada");
  EXPECT_EQ(NameOf(store_.Load()), "Ada");
  // The profile never hits the disk in the clear.
  EXPECT_EQ(ReadProfileFile().find("Ada"), std::string::npos);

  // Replacing a readable profile needs no backup.
  base::FilePath backup;
  ASSERT_EQ(store_.SaveKeepingUnreadableAsBackup(MakeProfile("Grace"),
                                                 &backup),
            ProfileSaveResult::kSaved);
  EXPECT_TRUE(backup.empty());
  EXPECT_EQ(NameOf(store_.Load()), "Grace");
}

TEST_F(MoltProfileStoreTest, EmptyFileCountsAsNoProfile) {
  if (!UseOsKey()) {
    GTEST_SKIP() << "no OSCrypt mock in this configuration";
  }
  ASSERT_TRUE(base::WriteFile(ProfilePath(), ""));
  EXPECT_EQ(store_.LoadWithStatus().status, ProfileReadStatus::kAbsent);
  EXPECT_EQ(store_.Save(MakeProfile("Ada")), ProfileSaveResult::kSaved);
  EXPECT_EQ(NameOf(store_.Load()), "Ada");
}

// The data-loss bug: a profile.enc that can't be read was replaced by
// whatever the (empty) editor saved.
TEST_F(MoltProfileStoreTest, NeverOverwritesAnUnreadableProfile) {
  if (!UseOsKey()) {
    GTEST_SKIP() << "no OSCrypt mock in this configuration";
  }
  ASSERT_TRUE(base::WriteFile(ProfilePath(), kCorrupt));

  ProfileLoadResult loaded = store_.LoadWithStatus();
  EXPECT_EQ(loaded.status, ProfileReadStatus::kUnreadable);
  EXPECT_TRUE(loaded.profile.empty());
  EXPECT_TRUE(store_.Load().empty());

  EXPECT_EQ(store_.Save(MakeProfile("Ada")),
            ProfileSaveResult::kExistingUnreadable);
  EXPECT_EQ(ReadProfileFile(), kCorrupt);
  EXPECT_FALSE(base::PathExists(ProfilePath().AddExtensionASCII(".tmp")));
}

TEST_F(MoltProfileStoreTest, StartingOverKeepsTheUnreadableFileAsABackup) {
  if (!UseOsKey()) {
    GTEST_SKIP() << "no OSCrypt mock in this configuration";
  }
  ASSERT_TRUE(base::WriteFile(ProfilePath(), kCorrupt));

  base::FilePath backup;
  ASSERT_EQ(store_.SaveKeepingUnreadableAsBackup(MakeProfile("Ada"), &backup),
            ProfileSaveResult::kSaved);
  ASSERT_FALSE(backup.empty());
  EXPECT_EQ(backup.DirName(), ProfilePath().DirName());
  EXPECT_EQ(backup.AsUTF8Unsafe().find(
                ProfilePath().AsUTF8Unsafe() + ".unreadable-"),
            0u);
  std::string kept;
  ASSERT_TRUE(base::ReadFileToString(backup, &kept));
  EXPECT_EQ(kept, kCorrupt);
  EXPECT_EQ(NameOf(store_.Load()), "Ada");

  // A second unreadable file in the same second gets its own name.
  ASSERT_TRUE(base::WriteFile(ProfilePath(), "second"));
  base::FilePath backup2;
  ASSERT_EQ(
      store_.SaveKeepingUnreadableAsBackup(MakeProfile("Grace"), &backup2),
      ProfileSaveResult::kSaved);
  ASSERT_FALSE(backup2.empty());
  EXPECT_NE(backup2, backup);
  ASSERT_TRUE(base::ReadFileToString(backup, &kept));
  EXPECT_EQ(kept, kCorrupt);
  ASSERT_TRUE(base::ReadFileToString(backup2, &kept));
  EXPECT_EQ(kept, "second");
  EXPECT_EQ(NameOf(store_.Load()), "Grace");
}

TEST_F(MoltProfileStoreTest, ClearRemovesTheProfile) {
  if (!UseOsKey()) {
    GTEST_SKIP() << "no OSCrypt mock in this configuration";
  }
  ASSERT_EQ(store_.Save(MakeProfile("Ada")), ProfileSaveResult::kSaved);
  EXPECT_TRUE(store_.Clear());
  EXPECT_FALSE(base::PathExists(ProfilePath()));
  EXPECT_TRUE(store_.Clear());
}

#if BUILDFLAG(IS_LINUX)

// profile.enc written on a machine (or keyring) with another key: OSCrypt
// "v11" = AES-128-CBC under PBKDF2("another machine's keyring") of
// {"full_name":"Ada Lovelace","email":"ada@example.com"}. Under the mock
// keyring's key it fails to decrypt.
constexpr char kOtherKeyringProfileHex[] =
    "76313151f8cf32405da2507c26b783b8f6368b34e70942c4636e3c333fb43d576749"
    "7ba5d3a2bf2247026bf736bd66657a3628d77bbb828f94e9bf5a655894ee315cd1";

TEST_F(MoltProfileStoreTest, LockedKeyringLeavesTheProfileAlone) {
  if (!UseOsKey()) {
    GTEST_SKIP() << "no OSCrypt mock in this configuration";
  }
  ASSERT_EQ(store_.Save(MakeProfile("Ada")), ProfileSaveResult::kSaved);
  const std::string saved = ReadProfileFile();

  // The keyring goes away (locked, or not running at startup).
  OSCryptMocker::TearDown();
  ASSERT_FALSE(IsSecureStorageAvailable());
  EXPECT_EQ(store_.LoadWithStatus().status, ProfileReadStatus::kUnreadable);
  EXPECT_EQ(store_.Save(MakeProfile("Grace")),
            ProfileSaveResult::kNoSecureStorage);
  base::FilePath backup;
  EXPECT_EQ(store_.SaveKeepingUnreadableAsBackup(MakeProfile("Grace"),
                                                 &backup),
            ProfileSaveResult::kNoSecureStorage);
  EXPECT_TRUE(backup.empty());
  EXPECT_EQ(ReadProfileFile(), saved);

  // Unlocked again: the profile is back.
  ASSERT_TRUE(UseOsKey());
  EXPECT_EQ(NameOf(store_.Load()), "Ada");
}

TEST_F(MoltProfileStoreTest, ProfileFromAnotherKeyringIsNotOverwritten) {
  std::string other;
  ASSERT_TRUE(base::HexStringToString(kOtherKeyringProfileHex, &other));
  ASSERT_TRUE(base::WriteFile(ProfilePath(), other));
  if (!UseOsKey()) {
    GTEST_SKIP() << "no OSCrypt mock in this configuration";
  }

  EXPECT_EQ(store_.LoadWithStatus().status, ProfileReadStatus::kUnreadable);
  EXPECT_EQ(store_.Save(MakeProfile("Grace")),
            ProfileSaveResult::kExistingUnreadable);
  EXPECT_EQ(ReadProfileFile(), other);
}

#endif  // BUILDFLAG(IS_LINUX)

}  // namespace
}  // namespace profile
}  // namespace molt_ai
