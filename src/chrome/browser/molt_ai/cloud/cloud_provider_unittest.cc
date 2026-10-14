// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/cloud/cloud_provider.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

bool Valid(const std::string& url) {
  std::string error;
  const bool ok = ValidateCloudBaseUrl(url, &error);
  // A rejection always explains itself; an accept never sets an error.
  EXPECT_EQ(ok, error.empty()) << url;
  return ok;
}

TEST(CloudBaseUrlTest, EmptyMeansProviderDefault) {
  EXPECT_TRUE(Valid(""));
}

TEST(CloudBaseUrlTest, HttpsIsAllowed) {
  EXPECT_TRUE(Valid("https://api.openai.com/v1"));
  EXPECT_TRUE(Valid("https://openrouter.ai/api/v1/"));
  EXPECT_TRUE(Valid("https://my-proxy.example.com:8443/openai/v1"));
}

TEST(CloudBaseUrlTest, HttpOnlyForLoopback) {
  EXPECT_TRUE(Valid("http://localhost:11434/v1"));
  EXPECT_TRUE(Valid("http://127.0.0.1:1234/v1"));
  EXPECT_TRUE(Valid("http://127.3.4.5/v1"));
  EXPECT_TRUE(Valid("http://[::1]:8080/v1"));

  EXPECT_FALSE(Valid("http://api.openai.com/v1"));
  EXPECT_FALSE(Valid("http://192.168.1.20:11434/v1"));
  EXPECT_FALSE(Valid("http://10.0.0.1/v1"));
  // Not loopback, despite the name.
  EXPECT_FALSE(Valid("http://localhost.evil.com/v1"));
  EXPECT_FALSE(Valid("http://127.0.0.1.nip.io/v1"));
}

TEST(CloudBaseUrlTest, OtherSchemesRejected) {
  EXPECT_FALSE(Valid("ftp://example.com/v1"));
  EXPECT_FALSE(Valid("file:///etc/passwd"));
  EXPECT_FALSE(Valid("ws://localhost/v1"));
  EXPECT_FALSE(Valid("javascript:alert(1)"));
  EXPECT_FALSE(Valid("api.openai.com/v1"));  // no scheme
  EXPECT_FALSE(Valid("not a url"));
}

TEST(CloudBaseUrlTest, CredentialsQueryAndFragmentRejected) {
  EXPECT_FALSE(Valid("https://user:pass@api.example.com/v1"));
  EXPECT_FALSE(Valid("https://user@api.example.com/v1"));
  EXPECT_FALSE(Valid("https://api.example.com/v1?key=abc"));
  EXPECT_FALSE(Valid("https://api.example.com/v1#frag"));
}

TEST(CloudBaseUrlTest, NullErrorPointerIsAllowed) {
  EXPECT_TRUE(ValidateCloudBaseUrl("https://api.anthropic.com", nullptr));
  EXPECT_FALSE(ValidateCloudBaseUrl("http://api.anthropic.com", nullptr));
}

TEST(CloudBaseUrlTest, BuiltInProviderDefaultsAreValid) {
  for (const CloudProviderInfo& info : GetCloudProviders()) {
    EXPECT_TRUE(Valid(info.default_base_url)) << info.id;
  }
}

}  // namespace
}  // namespace molt_ai
