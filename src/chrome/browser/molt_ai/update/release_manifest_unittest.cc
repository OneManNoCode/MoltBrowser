// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/update/release_manifest.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "base/base64.h"
#include "base/strings/string_number_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

// Vectors produced by tools/release_signing/molt_release_sign.py with a
// PUBLIC TEST KEY (private seed 00 01 02 .. 1f). Never trust this key in a
// real build. Ed25519 is deterministic, so these never change.
constexpr char kTestPublicKey[] = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
constexpr char kTestKeyId[] = "56475aa75463474c";
constexpr char kTestManifest[] = "{\n  \"assets\": [\n    {\n      \"name\": \"MoltBrowser-Linux-x64.deb\",\n      \"sha256\": \"9cfa1468c93fc18652e34a000f0c6614b0fa18f6f4887477ad9b0d36ca6a7eaa\",\n      \"size\": 300000\n    },\n    {\n      \"name\": \"MoltBrowser-macOS-arm64.dmg\",\n      \"sha256\": \"00cbbd0ddbda2762798f7009838ed34ca1f12b93965813c7df22943bc62166d1\",\n      \"size\": 929292681\n    }\n  ],\n  \"product\": \"MoltBrowser\",\n  \"schema\": 1,\n  \"tag\": \"v0.3.0\",\n  \"version\": \"0.3.0\"\n}\n";
constexpr char kTestSignature[] = "{\"algorithm\": \"ed25519\", \"key_id\": \"56475aa75463474c\", \"signature\": \"8J2aMolzw6VOy0Zwx4MFCmRDGwbR2IJbR0uSCKJLAmRyVto+Ne3wFx/zbcaJl3cfYzGBofQhGsUJUBYZI1e4Aw==\"}";
constexpr char kTestSignatureNoContext[] = "{\"algorithm\": \"ed25519\", \"signature\": \"8MJGKdYNYGwD5uuuEO0TzATn9dh6/r1ubYsNvF01/uH99e5oQ9dH+dOP1wP/wJsShwmxL2rCcYifFHznb7lTCQ==\"}";
constexpr char kDebSha256[] = "9cfa1468c93fc18652e34a000f0c6614b0fa18f6f4887477ad9b0d36ca6a7eaa";

std::string Replace(std::string s, std::string_view from, std::string_view to) {
  const size_t pos = s.find(from);
  if (pos != std::string::npos) {
    s.replace(pos, from.size(), to);
  }
  return s;
}

const std::string_view kKeys[] = {"", kTestPublicKey};

TEST(ReleaseManifestTest, ValidSignatureVerifies) {
  std::string error;
  EXPECT_TRUE(VerifyReleaseManifestSignature(kTestManifest, kTestSignature,
                                             kKeys, &error))
      << error;
}

TEST(ReleaseManifestTest, WrongKeyIsRejected) {
  const std::string_view other[] = {
      "ZIVejD1GzFChzyYEDWuGMBD8cflD+59TCiVcAvg6rL0="};
  std::string error;
  EXPECT_FALSE(VerifyReleaseManifestSignature(kTestManifest, kTestSignature,
                                              other, &error));
  EXPECT_EQ(error, "signature does not match any trusted key");
}

TEST(ReleaseManifestTest, NoUsableKeyFailsClosed) {
  const std::string_view none[] = {""};
  std::string error;
  EXPECT_FALSE(VerifyReleaseManifestSignature(kTestManifest, kTestSignature,
                                              none, &error));
  EXPECT_EQ(error, "this build has no update-signing key");
}

TEST(ReleaseManifestTest, TamperedManifestIsRejected) {
  std::string error;
  EXPECT_FALSE(VerifyReleaseManifestSignature(
      Replace(kTestManifest, "929292681", "929292682"), kTestSignature, kKeys,
      &error));
}

TEST(ReleaseManifestTest, SignatureWithoutContextIsRejected) {
  std::string error;
  EXPECT_FALSE(VerifyReleaseManifestSignature(
      kTestManifest, kTestSignatureNoContext, kKeys, &error));
}

TEST(ReleaseManifestTest, MalformedSignatureFilesAreRejected) {
  std::string error;
  EXPECT_FALSE(
      VerifyReleaseManifestSignature(kTestManifest, "not json", kKeys, &error));
  EXPECT_FALSE(VerifyReleaseManifestSignature(
      kTestManifest, Replace(kTestSignature, "ed25519", "rsa"), kKeys, &error));
  EXPECT_FALSE(VerifyReleaseManifestSignature(
      kTestManifest, R"({"algorithm":"ed25519","signature":"AAAA"})", kKeys,
      &error));
  EXPECT_FALSE(VerifyReleaseManifestSignature(std::string(70000, ' '),
                                              kTestSignature, kKeys, &error));
}

TEST(ReleaseManifestTest, ParsesAssets) {
  std::string error;
  std::optional<ReleaseManifest> m = ParseReleaseManifest(kTestManifest, &error);
  ASSERT_TRUE(m) << error;
  EXPECT_EQ(m->version, "0.3.0");
  EXPECT_EQ(m->tag, "v0.3.0");
  ASSERT_EQ(m->assets.size(), 2u);
  const ReleaseManifest::Asset* deb = m->FindAsset("MoltBrowser-Linux-x64.deb");
  ASSERT_TRUE(deb);
  EXPECT_EQ(deb->size, 300000);
  EXPECT_EQ(base::HexEncodeLower(deb->sha256), kDebSha256);
  EXPECT_EQ(m->FindAsset("MoltBrowser-macOS-arm64.dmg")->size, 929292681);
  EXPECT_FALSE(m->FindAsset("nope"));
}

TEST(ReleaseManifestTest, PolicyRejectsDowngradesAndOtherReleases) {
  std::string error;
  std::optional<ReleaseManifest> m = ParseReleaseManifest(kTestManifest, &error);
  ASSERT_TRUE(m);
  EXPECT_TRUE(CheckReleaseManifestPolicy(*m, "v0.3.0", "0.2.5", &error));
  EXPECT_FALSE(CheckReleaseManifestPolicy(*m, "v0.3.0", "0.3.0", &error));
  EXPECT_FALSE(CheckReleaseManifestPolicy(*m, "v0.3.0", "0.4.0", &error));
  EXPECT_FALSE(CheckReleaseManifestPolicy(*m, "v0.3.1", "0.2.5", &error));
  ReleaseManifest mismatched = *m;
  mismatched.tag = "v0.9.9";
  EXPECT_FALSE(
      CheckReleaseManifestPolicy(mismatched, "v0.9.9", "0.2.5", &error));
}

TEST(ReleaseManifestTest, RejectsMalformedManifests) {
  const std::string base = kTestManifest;
  const std::string bad[] = {
      Replace(base, "\"schema\": 1", "\"schema\": 2"),
      Replace(base, "\"schema\": 1", "\"schema\": true"),
      Replace(base, "\"MoltBrowser\"", "\"OtherBrowser\""),
      Replace(base, "MoltBrowser-Linux-x64.deb", "../evil.deb"),
      Replace(base, "MoltBrowser-Linux-x64.deb", ".evil"),
      Replace(base, "MoltBrowser-Linux-x64.deb", "a/b.deb"),
      Replace(base, kDebSha256, "abcd"),
      Replace(base, "9cfa", "9CFA"),
      Replace(base, "300000", "0"),
      Replace(base, "300000", "1.5"),
      Replace(base, "300000", "-1"),
      Replace(base, "MoltBrowser-Linux-x64.deb", "MoltBrowser-macOS-arm64.dmg"),
      Replace(base, "\"version\": \"0.3.0\"", "\"version\": \"x\""),
      "{",
  };
  for (const std::string& text : bad) {
    std::string error;
    EXPECT_FALSE(ParseReleaseManifest(text, &error)) << text;
  }
}

TEST(ReleaseManifestTest, KeyHelpers) {
  const std::string_view junk[] = {"", "bad", "AAAA"};
  EXPECT_FALSE(HasUsableUpdateSigningKey(junk));
  EXPECT_TRUE(HasUsableUpdateSigningKey(kKeys));

  std::string raw;
  ASSERT_TRUE(base::Base64Decode(kTestPublicKey, &raw));
  std::array<uint8_t, 32> key;
  std::copy(raw.begin(), raw.end(), key.begin());
  EXPECT_EQ(UpdateSigningKeyId(key), kTestKeyId);
  EXPECT_EQ(ReleaseManifestSignedMessage("abc"),
            "MoltBrowser release manifest v1\nabc");
}

}  // namespace
}  // namespace molt_ai
