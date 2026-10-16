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
#include "chrome/browser/molt_ai/keys/molt_os_crypt.h"
#include "chrome/browser/molt_ai/keys/molt_os_crypt_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace profile {
namespace {

// Not an encrypted JSON dict on any platform: a truncated or corrupt file.
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

  base::FilePath ProfilePath() const {
    return temp_dir_.GetPath().AppendASCII("profile.enc");
  }

  std::string ReadProfileFile() const {
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(ProfilePath(), &contents));
    return contents;
  }

  // A real OS-backed key (a fake keyring / Keychain / DPAPI key).
  void UseOsKey() { encryptor_.Set(test::MakeOsKeyEncryptor()); }

  base::ScopedTempDir temp_dir_;
  MoltProfileStore store_;
  // Starts "not ready": no Encryptor yet.
  molt_os_crypt::ScopedEncryptorForTesting encryptor_{nullptr};
};

TEST_F(MoltProfileStoreTest, SavesAndLoadsWithOsKey) {
  UseOsKey();
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
  UseOsKey();
  ASSERT_TRUE(base::WriteFile(ProfilePath(), ""));
  EXPECT_EQ(store_.LoadWithStatus().status, ProfileReadStatus::kAbsent);
  EXPECT_EQ(store_.Save(MakeProfile("Ada")), ProfileSaveResult::kSaved);
  EXPECT_EQ(NameOf(store_.Load()), "Ada");
}

// The data-loss bug: a profile.enc that can't be read was replaced by
// whatever the (empty) editor saved.
TEST_F(MoltProfileStoreTest, NeverOverwritesAnUnreadableProfile) {
  UseOsKey();
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
  UseOsKey();
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
  UseOsKey();
  ASSERT_EQ(store_.Save(MakeProfile("Ada")), ProfileSaveResult::kSaved);
  EXPECT_TRUE(store_.Clear());
  EXPECT_FALSE(base::PathExists(ProfilePath()));
  EXPECT_TRUE(store_.Clear());
}

// The keyring goes away (locked, or not answering at startup), or the
// Encryptor isn't there yet: the profile reads as unreadable, saving is
// refused, and the file is left alone until the key is back.
TEST_F(MoltProfileStoreTest, LockedKeyringLeavesTheProfileAlone) {
  UseOsKey();
  ASSERT_EQ(store_.Save(MakeProfile("Ada")), ProfileSaveResult::kSaved);
  const std::string saved = ReadProfileFile();

  for (bool locked : {true, false}) {
    SCOPED_TRACE(locked ? "locked" : "not ready");
    encryptor_.Set(locked ? test::MakeLockedOsKeyEncryptor()
                          : scoped_refptr<os_crypt_async::Encryptor>());
    ASSERT_FALSE(IsSecureStorageAvailable());
    EXPECT_EQ(store_.LoadWithStatus().status, ProfileReadStatus::kUnreadable);
    EXPECT_EQ(store_.Save(MakeProfile("Grace")),
              ProfileSaveResult::kNoSecureStorage);
    base::FilePath backup;
    EXPECT_EQ(
        store_.SaveKeepingUnreadableAsBackup(MakeProfile("Grace"), &backup),
        ProfileSaveResult::kNoSecureStorage);
    EXPECT_TRUE(backup.empty());
    EXPECT_EQ(ReadProfileFile(), saved);
  }

  // Unlocked again: the profile is back.
  UseOsKey();
  EXPECT_EQ(NameOf(store_.Load()), "Ada");
}

#if BUILDFLAG(IS_LINUX)

// profile.enc written on a machine (or keyring) with another key: OSCrypt
// "v11" = AES-128-CBC under PBKDF2("another machine's keyring") of
// {"full_name":"Ada Lovelace","email":"ada@example.com"}. Under the test
// keyring key (and the empty-password fallback key) it fails to decrypt.
constexpr char kOtherKeyringProfileHex[] =
    "76313151f8cf32405da2507c26b783b8f6368b34e70942c4636e3c333fb43d576749"
    "7ba5d3a2bf2247026bf736bd66657a3628d77bbb828f94e9bf5a655894ee315cd1";

TEST_F(MoltProfileStoreTest, ProfileFromAnotherKeyringIsNotOverwritten) {
  std::string other;
  ASSERT_TRUE(base::HexStringToString(kOtherKeyringProfileHex, &other));
  ASSERT_TRUE(base::WriteFile(ProfilePath(), other));
  UseOsKey();

  EXPECT_EQ(store_.LoadWithStatus().status, ProfileReadStatus::kUnreadable);
  EXPECT_EQ(store_.Save(MakeProfile("Grace")),
            ProfileSaveResult::kExistingUnreadable);
  EXPECT_EQ(ReadProfileFile(), other);
}

// Without a keyring the profile isn't saved under the hard-coded key.
TEST_F(MoltProfileStoreTest, NoKeyringRefusesToSave) {
  encryptor_.Set(test::MakeHardCodedKeyEncryptor());
  EXPECT_EQ(store_.Save(MakeProfile("Ada")),
            ProfileSaveResult::kNoSecureStorage);
  EXPECT_FALSE(base::PathExists(ProfilePath()));
}

#endif  // BUILDFLAG(IS_LINUX)

}  // namespace
}  // namespace profile
}  // namespace molt_ai
