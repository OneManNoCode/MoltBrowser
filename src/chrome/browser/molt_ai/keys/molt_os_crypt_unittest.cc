// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/keys/molt_os_crypt.h"

#include <memory>
#include <string>

#include "base/strings/string_number_conversions.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/keys/molt_os_crypt_test_util.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/browser/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace molt_os_crypt {
namespace {

using os_crypt_async::Encryptor;

class MoltOSCryptTest : public testing::Test {
 protected:
  base::test::TaskEnvironment task_environment_;
  // Starts "not ready", whatever an earlier test left behind.
  ScopedEncryptorForTesting encryptor_{nullptr};
};

TEST_F(MoltOSCryptTest, FailsClosedUntilReady) {
  EXPECT_FALSE(IsReady());
  EXPECT_FALSE(IsEncryptionAvailable());

  std::string ciphertext = "stale";
  EXPECT_FALSE(EncryptString("secret", &ciphertext));
  EXPECT_TRUE(ciphertext.empty());

  std::string plaintext = "stale";
  Encryptor::DecryptFlags flags;
  EXPECT_FALSE(DecryptString("v10whatever", &plaintext, &flags));
  EXPECT_TRUE(plaintext.empty());
  EXPECT_TRUE(flags.temporarily_unavailable);
}

TEST_F(MoltOSCryptTest, RoundTripsWithTheOsKey) {
  encryptor_.Set(test::MakeOsKeyEncryptor());
  EXPECT_TRUE(IsReady());
  EXPECT_TRUE(IsEncryptionAvailable());

  std::string ciphertext;
  ASSERT_TRUE(EncryptString("secret", &ciphertext));
  EXPECT_EQ(ciphertext.compare(0, 3, test::kOsKeyTag), 0);
  EXPECT_EQ(ciphertext.find("secret"), std::string::npos);

  std::string plaintext;
  Encryptor::DecryptFlags flags;
  ASSERT_TRUE(DecryptString(ciphertext, &plaintext, &flags));
  EXPECT_EQ(plaintext, "secret");
  EXPECT_FALSE(flags.should_reencrypt);
  EXPECT_FALSE(flags.temporarily_unavailable);
}

// As with OSCrypt: empty in, empty out.
TEST_F(MoltOSCryptTest, EmptyStringStaysEmpty) {
  encryptor_.Set(test::MakeOsKeyEncryptor());
  std::string ciphertext = "stale";
  ASSERT_TRUE(EncryptString("", &ciphertext));
  EXPECT_TRUE(ciphertext.empty());
  std::string plaintext = "stale";
  ASSERT_TRUE(DecryptString("", &plaintext));
  EXPECT_TRUE(plaintext.empty());
}

TEST_F(MoltOSCryptTest, AnotherKeyCannotDecrypt) {
  encryptor_.Set(test::MakeOsKeyEncryptor(/*key_seed=*/1));
  std::string ciphertext;
  ASSERT_TRUE(EncryptString("secret", &ciphertext));

  encryptor_.Set(test::MakeOsKeyEncryptor(/*key_seed=*/2));
  std::string plaintext;
  Encryptor::DecryptFlags flags;
  EXPECT_FALSE(DecryptString(ciphertext, &plaintext, &flags));
  EXPECT_FALSE(flags.temporarily_unavailable);  // gone for good
}

TEST_F(MoltOSCryptTest, LockedKeyIsTemporarilyUnavailable) {
  encryptor_.Set(test::MakeOsKeyEncryptor());
  std::string ciphertext;
  ASSERT_TRUE(EncryptString("secret", &ciphertext));

  encryptor_.Set(test::MakeLockedOsKeyEncryptor());
  std::string plaintext;
  Encryptor::DecryptFlags flags;
  EXPECT_FALSE(DecryptString(ciphertext, &plaintext, &flags));
  EXPECT_TRUE(flags.temporarily_unavailable);
}

TEST_F(MoltOSCryptTest, UnknownTagDoesNotDecrypt) {
  encryptor_.Set(test::MakeOsKeyEncryptor());
  std::string plaintext;
  EXPECT_FALSE(DecryptString("plain text, no tag", &plaintext));
}

TEST_F(MoltOSCryptTest, NoKeysCannotEncrypt) {
  encryptor_.Set(test::MakeEncryptorWithoutKeys());
  EXPECT_TRUE(IsReady());
  EXPECT_FALSE(IsEncryptionAvailable());
  std::string ciphertext;
  EXPECT_FALSE(EncryptString("secret", &ciphertext));
  EXPECT_TRUE(ciphertext.empty());
}

TEST_F(MoltOSCryptTest, RunWhenReadyWaitsForTheEncryptor) {
  int runs = 0;
  RunWhenReady(base::BindLambdaForTesting([&] { ++runs; }));
  EXPECT_EQ(runs, 0);
  encryptor_.Set(test::MakeOsKeyEncryptor());
  EXPECT_EQ(runs, 1);
  // Already ready: runs straight away.
  RunWhenReady(base::BindLambdaForTesting([&] { ++runs; }));
  EXPECT_EQ(runs, 2);
}

TEST_F(MoltOSCryptTest, InitTakesTheEncryptorFromOSCryptAsync) {
  std::unique_ptr<os_crypt_async::OSCryptAsync> os_crypt_async =
      os_crypt_async::GetTestOSCryptAsyncForTesting(
          /*is_sync_for_unittests=*/true);
  int runs = 0;
  RunWhenReady(base::BindLambdaForTesting([&] { ++runs; }));
  Init(os_crypt_async.get());
  EXPECT_TRUE(IsReady());
  EXPECT_EQ(runs, 1);
  std::string ciphertext;
  ASSERT_TRUE(EncryptString("secret", &ciphertext));
  std::string plaintext;
  ASSERT_TRUE(DecryptString(ciphertext, &plaintext));
  EXPECT_EQ(plaintext, "secret");

  // Set once: a second Init() doesn't replace it.
  Init(os_crypt_async::GetTestOSCryptAsyncForTesting(true).get());
  ASSERT_TRUE(DecryptString(ciphertext, &plaintext));
}

TEST_F(MoltOSCryptTest, InitWithoutOSCryptAsyncIsANoOp) {
  Init(nullptr);
  EXPECT_FALSE(IsReady());
}

#if BUILDFLAG(IS_LINUX)
// The real PosixKeyProvider still reads what the old OSCrypt wrote under the
// hard-coded key: AES-128-CBC("hello") under "peanuts", tag "v10".
TEST_F(MoltOSCryptTest, DecryptsOldHardCodedKeyData) {
  encryptor_.Set(test::MakeOsKeyEncryptor());
  // "v10" + AES-128-CBC(key = PBKDF2-HMAC-SHA1("peanuts", "saltysalt", 1),
  // IV = 16 spaces, "hello"): what os_crypt_linux.cc wrote without a keyring.
  std::string legacy;
  ASSERT_TRUE(base::HexStringToString("763130ce9b849d984a1cedb8ba4eea782c2680",
                                      &legacy));
  std::string plaintext;
  Encryptor::DecryptFlags flags;
  ASSERT_TRUE(DecryptString(legacy, &plaintext, &flags));
  EXPECT_EQ(plaintext, "hello");
  // The keyring key is there, so callers may move the data to it.
  EXPECT_TRUE(flags.should_reencrypt);
}
#endif  // BUILDFLAG(IS_LINUX)

}  // namespace
}  // namespace molt_os_crypt
}  // namespace molt_ai
