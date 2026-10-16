// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/conversation_store.h"

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/keys/molt_os_crypt.h"
#include "chrome/browser/molt_ai/keys/molt_os_crypt_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace memory {
namespace {

constexpr char kMagic[] = "MOLTCONV1\n";
constexpr char kHistory[] =
    R"([{"role":"user","content":"my bank PIN is 4711"}])";

bool Succeeded(const base::DictValue& result) {
  return result.FindBool("success").value_or(false);
}

class ConversationStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    store_.emplace(temp_dir_.GetPath().AppendASCII("profile"));
  }

  base::FilePath FileFor(const std::string& id) const {
    return store_->dir().AppendASCII(id + ".mconv");
  }

  std::string ReadFile(const base::FilePath& path) const {
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(path, &contents));
    return contents;
  }

  base::ScopedTempDir temp_dir_;
  std::optional<ConversationStore> store_;
  // Starts "not ready": no Encryptor yet.
  molt_os_crypt::ScopedEncryptorForTesting encryptor_{nullptr};
};

TEST_F(ConversationStoreTest, SavesEncryptedAndLoadsBack) {
  encryptor_.Set(test::MakeOsKeyEncryptor());
  ASSERT_TRUE(Succeeded(store_->Save("c1", "Bank", kHistory)));

  const std::string on_disk = ReadFile(FileFor("c1"));
  ASSERT_EQ(on_disk.compare(0, sizeof(kMagic) - 1, kMagic), 0);
  EXPECT_EQ(on_disk.compare(sizeof(kMagic) - 1, 3, test::kOsKeyTag), 0);
  EXPECT_EQ(on_disk.find("4711"), std::string::npos);
  EXPECT_EQ(on_disk.find("Bank"), std::string::npos);

  base::DictValue loaded = store_->Load("c1");
  ASSERT_TRUE(Succeeded(loaded));
  const std::string* title = loaded.FindString("title");
  ASSERT_TRUE(title);
  EXPECT_EQ(*title, "Bank");
  const std::string* history = loaded.FindString("history_json");
  ASSERT_TRUE(history);
  EXPECT_NE(history->find("4711"), std::string::npos);
}

// Never plaintext: before the Encryptor is ready, or with no key at all,
// nothing is written.
TEST_F(ConversationStoreTest, NoEncryptionNoFile) {
  EXPECT_FALSE(Succeeded(store_->Save("c1", "Bank", kHistory)));
  EXPECT_FALSE(base::PathExists(FileFor("c1")));

  encryptor_.Set(test::MakeEncryptorWithoutKeys());
  EXPECT_FALSE(Succeeded(store_->Save("c1", "Bank", kHistory)));
  EXPECT_FALSE(base::PathExists(FileFor("c1")));
}

TEST_F(ConversationStoreTest, ListSkipsWhatItCannotDecrypt) {
  encryptor_.Set(test::MakeOsKeyEncryptor(/*key_seed=*/1));
  ASSERT_TRUE(Succeeded(store_->Save("c1", "Bank", kHistory)));
  base::DictValue listed = store_->List(10);
  const base::ListValue* list = listed.FindList("conversations");
  ASSERT_TRUE(list);
  EXPECT_EQ(list->size(), 1u);

  // Not ready yet, or another key: listed as nothing, the file kept.
  encryptor_.Set(nullptr);
  base::DictValue not_ready = store_->List(10);
  list = not_ready.FindList("conversations");
  ASSERT_TRUE(list);
  EXPECT_TRUE(list->empty());
  EXPECT_FALSE(Succeeded(store_->Load("c1")));

  encryptor_.Set(test::MakeOsKeyEncryptor(/*key_seed=*/2));
  base::DictValue other_key = store_->List(10);
  list = other_key.FindList("conversations");
  ASSERT_TRUE(list);
  EXPECT_TRUE(list->empty());
  EXPECT_TRUE(base::PathExists(FileFor("c1")));
}

// The legacy plaintext copy is only deleted once it is stored encrypted.
TEST_F(ConversationStoreTest, MigrationWaitsForEncryption) {
  const base::FilePath legacy = temp_dir_.GetPath().AppendASCII("legacy");
  ASSERT_TRUE(base::CreateDirectory(legacy));
  const base::FilePath legacy_file = legacy.AppendASCII("old1.json");
  ASSERT_TRUE(base::WriteFile(legacy_file,
                              R"({"title":"Old","messages":[{"role":"user",)"
                              R"("content":"hello"}]})"));

  store_->MigrateLegacy(legacy);  // not ready: nothing moves
  EXPECT_TRUE(base::PathExists(legacy_file));
  EXPECT_FALSE(base::PathExists(FileFor("old1")));

  encryptor_.Set(test::MakeOsKeyEncryptor());
  store_->MigrateLegacy(legacy);
  EXPECT_FALSE(base::PathExists(legacy_file));
  EXPECT_FALSE(base::PathExists(legacy));
  EXPECT_EQ(ReadFile(FileFor("old1")).find("hello"), std::string::npos);
  base::DictValue loaded = store_->Load("old1");
  ASSERT_TRUE(Succeeded(loaded));
  EXPECT_EQ(*loaded.FindString("title"), "Old");
}

#if BUILDFLAG(IS_LINUX)
// Chat history, unlike the secret stores, is kept on a Linux desktop without
// a keyring: under the hard-coded key, as the old OSCrypt did. Files written
// that way stay readable once a keyring shows up.
TEST_F(ConversationStoreTest, HardCodedKeyIsAcceptedForChatHistory) {
  encryptor_.Set(test::MakeHardCodedKeyEncryptor());
  ASSERT_TRUE(Succeeded(store_->Save("c1", "Bank", kHistory)));
  const std::string on_disk = ReadFile(FileFor("c1"));
  EXPECT_EQ(on_disk.compare(sizeof(kMagic) - 1, 3, "v10"), 0);
  EXPECT_EQ(on_disk.find("4711"), std::string::npos);

  encryptor_.Set(test::MakeOsKeyEncryptor());
  EXPECT_TRUE(Succeeded(store_->Load("c1")));
}
#endif  // BUILDFLAG(IS_LINUX)

}  // namespace
}  // namespace memory
}  // namespace molt_ai
