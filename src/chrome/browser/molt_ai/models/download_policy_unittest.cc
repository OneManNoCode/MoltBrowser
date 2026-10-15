// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/models/download_policy.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace models {
namespace {

const char kHash[] =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

TEST(DownloadPolicyTest, UrlUsesPinnedRevision) {
  EXPECT_EQ(ModelDownloadUrl("Qwen/Qwen3-8B-GGUF",
                             "0123456789abcdef0123456789abcdef01234567",
                             "Qwen3-8B-Q4_K_M.gguf"),
            "https://huggingface.co/Qwen/Qwen3-8B-GGUF/resolve/"
            "0123456789abcdef0123456789abcdef01234567/Qwen3-8B-Q4_K_M.gguf");
  EXPECT_EQ(ModelDownloadUrl("a/b", "", "m.gguf"),
            "https://huggingface.co/a/b/resolve/main/m.gguf");
}

TEST(DownloadPolicyTest, Sha256HexValidation) {
  EXPECT_TRUE(IsValidSha256Hex(kHash));
  EXPECT_FALSE(IsValidSha256Hex(""));
  EXPECT_FALSE(IsValidSha256Hex(std::string(kHash).substr(1)));
  std::string upper = kHash;
  upper[0] = 'E';
  EXPECT_FALSE(IsValidSha256Hex(upper));
}

TEST(DownloadPolicyTest, FreshDownload) {
  EXPECT_EQ(DecideResumeAction(0, 200, -1), ResumeAction::kAppend);
  EXPECT_EQ(DecideResumeAction(0, 206, 0), ResumeAction::kAppend);
  EXPECT_EQ(DecideResumeAction(0, 404, -1), ResumeAction::kFail);
}

TEST(DownloadPolicyTest, ResumeAppendsOnlyAtTheRightOffset) {
  EXPECT_EQ(DecideResumeAction(1000, 206, 1000), ResumeAction::kAppend);
  // Server ignored the Range header and sent everything: the old code
  // (DownloadToFile) truncated the .partial and kept only this body, or
  // would have appended a full copy after the existing bytes.
  EXPECT_EQ(DecideResumeAction(1000, 200, -1), ResumeAction::kRestart);
  EXPECT_EQ(DecideResumeAction(1000, 206, 0), ResumeAction::kRestart);
  EXPECT_EQ(DecideResumeAction(1000, 206, 500), ResumeAction::kFail);
  EXPECT_EQ(DecideResumeAction(1000, 206, -1), ResumeAction::kFail);
  EXPECT_EQ(DecideResumeAction(1000, 416, -1), ResumeAction::kFail);
}

TEST(DownloadPolicyTest, VerifyPinnedEntries) {
  EXPECT_EQ(VerifyDownload(kHash, 10, kHash, 10), VerifyResult::kVerified);
  EXPECT_EQ(VerifyDownload(kHash, 10, kHash, 0), VerifyResult::kVerified);
  std::string other = kHash;
  other[5] = '0';
  EXPECT_EQ(VerifyDownload(other, 10, kHash, 10),
            VerifyResult::kHashMismatch);
  EXPECT_EQ(VerifyDownload(kHash, 9, kHash, 10), VerifyResult::kSizeMismatch);
}

TEST(DownloadPolicyTest, UnpinnedEntriesAreAllowedButUnverified) {
  EXPECT_EQ(VerifyDownload(kHash, 10, "", 0), VerifyResult::kUnverified);
  // A known size is still enforced without a hash.
  EXPECT_EQ(VerifyDownload(kHash, 10, "", 11), VerifyResult::kSizeMismatch);
}

}  // namespace
}  // namespace models
}  // namespace molt_ai
