// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// In-app updates' root of trust (update/release_manifest.h) on what a
// release page serves: the manifest and its signature file, both read
// before anything is trusted, and the manifest's contents once its
// signature checks out.
//
// The fuzzer signs every manifest with a fixed TEST key (never a release
// key) so the signed-manifest parser and the release policy are reached
// too. Checks: no memory or UB error; a valid signature by a listed key
// verifies, and stops verifying when one byte of the manifest changes or
// the signing context is left out; a parsed manifest's assets are plain,
// distinct names with positive sizes; the policy only accepts a manifest
// for the expected tag whose version matches it and is newer than the
// running one.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/base64.h"
#include "base/check.h"
#include "base/containers/span.h"
#include "base/version.h"
#include "chrome/browser/molt_ai/update/release_manifest.h"
#include "third_party/boringssl/src/include/openssl/curve25519.h"

namespace molt_ai {
namespace {

struct TestKey {
  TestKey() {
    std::array<uint8_t, 32> seed;
    for (size_t i = 0; i < seed.size(); ++i) {
      seed[i] = static_cast<uint8_t>(0xA5 ^ i);
    }
    ED25519_keypair_from_seed(public_key.data(), private_key.data(),
                              seed.data());
    base64 = base::Base64Encode(public_key);
  }
  std::array<uint8_t, ED25519_PUBLIC_KEY_LEN> public_key;
  std::array<uint8_t, ED25519_PRIVATE_KEY_LEN> private_key;
  std::string base64;
};

std::string SignatureFile(const TestKey& key, std::string_view message) {
  std::array<uint8_t, ED25519_SIGNATURE_LEN> signature;
  CHECK(ED25519_sign(signature.data(),
                     reinterpret_cast<const uint8_t*>(message.data()),
                     message.size(), key.private_key.data()) == 1);
  return R"({"algorithm":"ed25519","key_id":")" +
         UpdateSigningKeyId(key.public_key) + R"(","signature":")" +
         base::Base64Encode(signature) + "\"}";
}

bool IsPlainName(std::string_view name) {
  if (name.empty() || name.size() > 128 || name[0] == '.') return false;
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

}  // namespace
}  // namespace molt_ai

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai;
  static const TestKey* key = new TestKey();
  FuzzedDataProvider p(data, size);
  const std::string manifest =
      p.ConsumeRandomLengthString(kMaxReleaseManifestBytes + 16);
  const std::string signature_file =
      p.ConsumeRandomLengthString(kMaxReleaseSignatureBytes + 16);
  const std::string expected_tag = p.ConsumeRandomLengthString(32);
  const std::string current_version = p.ConsumeRandomLengthString(32);
  const std::string_view keys[] = {"", "not base64!", key->base64};

  // Whatever signature file the release page serves.
  std::string error;
  if (!VerifyReleaseManifestSignature(manifest, signature_file, keys, &error)) {
    CHECK(!error.empty());
  }
  CHECK(UpdateSigningKeyId(key->public_key).size() == 16);
  CHECK(HasUsableUpdateSigningKey(keys));

  // A valid signature by the listed test key.
  const bool in_size =
      !manifest.empty() && manifest.size() <= kMaxReleaseManifestBytes;
  const std::string good =
      SignatureFile(*key, ReleaseManifestSignedMessage(manifest));
  CHECK(VerifyReleaseManifestSignature(manifest, good, keys, &error) == in_size)
      << error;
  if (in_size) {
    std::string tampered = manifest;
    tampered[p.ConsumeIntegralInRange<size_t>(0, tampered.size() - 1)] ^=
        static_cast<char>(p.ConsumeIntegralInRange<int>(1, 255));
    CHECK(!VerifyReleaseManifestSignature(tampered, good, keys, &error))
        << "a changed manifest still verifies";
    // Domain separation: a signature over the bare bytes doesn't count.
    CHECK(!VerifyReleaseManifestSignature(
        manifest, SignatureFile(*key, manifest), keys, &error));
    // Nor does a valid signature by a key the build doesn't list.
    const std::string_view other_keys[] = {"", "not base64!"};
    CHECK(!VerifyReleaseManifestSignature(manifest, good, other_keys, &error));
  }

  // The verified manifest's contents and the release policy.
  const std::optional<ReleaseManifest> parsed =
      ParseReleaseManifest(manifest, &error);
  if (!parsed) {
    CHECK(!error.empty());
    return 0;
  }
  CHECK(!parsed->assets.empty());
  CHECK(base::Version(parsed->version).IsValid() && !parsed->tag.empty());
  for (size_t i = 0; i < parsed->assets.size(); ++i) {
    const ReleaseManifest::Asset& asset = parsed->assets[i];
    CHECK(IsPlainName(asset.name)) << "an asset name that isn't a plain file";
    CHECK(asset.size >= 1 && asset.size <= (int64_t{1} << 53));
    CHECK(parsed->FindAsset(asset.name) == &asset) << "a duplicate asset";
  }
  if (CheckReleaseManifestPolicy(*parsed, expected_tag, current_version,
                                 &error)) {
    CHECK(parsed->tag == expected_tag);
    std::string_view tag = parsed->tag;
    if (tag[0] == 'v' || tag[0] == 'V') tag.remove_prefix(1);
    CHECK(tag == parsed->version);
    CHECK(base::Version(parsed->version) > base::Version(current_version))
        << "a downgrade or replay passed the policy";
  }
  return 0;
}
