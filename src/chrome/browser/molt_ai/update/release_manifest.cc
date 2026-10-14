// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/update/release_manifest.h"

#include <algorithm>
#include <set>
#include <utility>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "base/version.h"
#include "crypto/hash.h"
#include "third_party/boringssl/src/include/openssl/curve25519.h"

namespace molt_ai {

namespace {

constexpr int kSupportedSchema = 1;
constexpr char kProductName[] = "MoltBrowser";
// Largest integer a JSON number (double) holds exactly.
constexpr double kMaxExactJsonInteger = 9007199254740992.0;  // 2^53

std::optional<std::array<uint8_t, ED25519_PUBLIC_KEY_LEN>> DecodePublicKey(
    std::string_view base64) {
  std::string raw;
  if (base64.empty() ||
      !base::Base64Decode(base64, &raw, base::Base64DecodePolicy::kStrict) ||
      raw.size() != ED25519_PUBLIC_KEY_LEN) {
    return std::nullopt;
  }
  std::array<uint8_t, ED25519_PUBLIC_KEY_LEN> key;
  std::copy(raw.begin(), raw.end(), key.begin());
  return key;
}

bool IsLowerHex64(std::string_view s) {
  if (s.size() != 64) {
    return false;
  }
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// Asset names become file names on disk, so only plain names are allowed:
// [A-Za-z0-9._-], not starting with a dot, at most 128 characters.
bool IsPlainFileName(std::string_view name) {
  if (name.empty() || name.size() > 128 || name[0] == '.') {
    return false;
  }
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::string_view StripLeadingV(std::string_view tag) {
  if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) {
    tag.remove_prefix(1);
  }
  return tag;
}

}  // namespace

ReleaseManifest::ReleaseManifest() = default;
ReleaseManifest::ReleaseManifest(const ReleaseManifest&) = default;
ReleaseManifest& ReleaseManifest::operator=(const ReleaseManifest&) = default;
ReleaseManifest::~ReleaseManifest() = default;

const ReleaseManifest::Asset* ReleaseManifest::FindAsset(
    std::string_view name) const {
  for (const Asset& asset : assets) {
    if (asset.name == name) {
      return &asset;
    }
  }
  return nullptr;
}

std::string ReleaseManifestSignedMessage(std::string_view manifest_bytes) {
  return base::StrCat({kReleaseManifestSigningContext, manifest_bytes});
}

std::string UpdateSigningKeyId(base::span<const uint8_t, 32> public_key) {
  const std::array<uint8_t, crypto::hash::kSha256Size> digest =
      crypto::hash::Sha256(public_key);
  return base::HexEncodeLower(base::span<const uint8_t>(digest).first(8u));
}

bool HasUsableUpdateSigningKey(base::span<const std::string_view> keys) {
  for (std::string_view key : keys) {
    if (DecodePublicKey(key)) {
      return true;
    }
  }
  return false;
}

bool VerifyReleaseManifestSignature(
    std::string_view manifest_bytes,
    std::string_view signature_file,
    base::span<const std::string_view> trusted_keys_base64,
    std::string* error) {
  if (manifest_bytes.empty() ||
      manifest_bytes.size() > kMaxReleaseManifestBytes) {
    *error = "manifest missing or too large";
    return false;
  }
  if (signature_file.size() > kMaxReleaseSignatureBytes) {
    *error = "signature file too large";
    return false;
  }

  std::optional<base::Value> parsed =
      base::JSONReader::Read(signature_file, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    *error = "signature file is not valid JSON";
    return false;
  }
  const base::DictValue& sig_dict = parsed->GetDict();
  const std::string* algorithm = sig_dict.FindString("algorithm");
  const std::string* signature_b64 = sig_dict.FindString("signature");
  if (!algorithm || *algorithm != "ed25519" || !signature_b64) {
    *error = "unsupported signature algorithm";
    return false;
  }
  std::string signature;
  if (!base::Base64Decode(*signature_b64, &signature,
                          base::Base64DecodePolicy::kStrict) ||
      signature.size() != ED25519_SIGNATURE_LEN) {
    *error = "malformed signature";
    return false;
  }

  const std::string message = ReleaseManifestSignedMessage(manifest_bytes);
  const auto* message_data = reinterpret_cast<const uint8_t*>(message.data());
  const auto* signature_data =
      reinterpret_cast<const uint8_t*>(signature.data());

  bool any_key = false;
  for (std::string_view key_b64 : trusted_keys_base64) {
    std::optional<std::array<uint8_t, ED25519_PUBLIC_KEY_LEN>> key =
        DecodePublicKey(key_b64);
    if (!key) {
      continue;  // Placeholder or malformed entry.
    }
    any_key = true;
    if (ED25519_verify(message_data, message.size(), signature_data,
                       key->data()) == 1) {
      return true;
    }
  }
  *error = any_key ? "signature does not match any trusted key"
                   : "this build has no update-signing key";
  return false;
}

std::optional<ReleaseManifest> ParseReleaseManifest(
    std::string_view manifest_bytes,
    std::string* error) {
  std::optional<base::Value> parsed =
      base::JSONReader::Read(manifest_bytes, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    *error = "manifest is not valid JSON";
    return std::nullopt;
  }
  const base::DictValue& root = parsed->GetDict();

  if (root.FindInt("schema") != kSupportedSchema) {
    *error = "unsupported manifest schema";
    return std::nullopt;
  }
  const std::string* product = root.FindString("product");
  if (!product || *product != kProductName) {
    *error = "manifest is for a different product";
    return std::nullopt;
  }

  ReleaseManifest manifest;
  const std::string* version = root.FindString("version");
  const std::string* tag = root.FindString("tag");
  if (!version || !base::Version(*version).IsValid() || !tag || tag->empty()) {
    *error = "manifest version or tag is invalid";
    return std::nullopt;
  }
  manifest.version = *version;
  manifest.tag = *tag;

  const base::ListValue* assets = root.FindList("assets");
  if (!assets || assets->empty()) {
    *error = "manifest lists no assets";
    return std::nullopt;
  }
  std::set<std::string> seen;
  for (const base::Value& entry : *assets) {
    const base::DictValue* asset = entry.GetIfDict();
    const std::string* name = asset ? asset->FindString("name") : nullptr;
    const std::string* sha256 = asset ? asset->FindString("sha256") : nullptr;
    std::optional<double> size =
        asset ? asset->FindDouble("size") : std::nullopt;
    if (!name || !IsPlainFileName(*name) || !sha256 || !IsLowerHex64(*sha256) ||
        !size ||
        *size < 1 || *size > kMaxExactJsonInteger ||
        *size != static_cast<double>(static_cast<int64_t>(*size))) {
      *error = "manifest has a malformed asset entry";
      return std::nullopt;
    }
    if (!seen.insert(*name).second) {
      *error = "manifest lists an asset twice";
      return std::nullopt;
    }
    ReleaseManifest::Asset out;
    out.name = *name;
    out.size = static_cast<int64_t>(*size);
    if (!base::HexStringToSpan(*sha256, out.sha256)) {
      *error = "manifest has a malformed asset hash";
      return std::nullopt;
    }
    manifest.assets.push_back(std::move(out));
  }
  return manifest;
}

bool CheckReleaseManifestPolicy(const ReleaseManifest& manifest,
                                std::string_view expected_tag,
                                std::string_view current_version,
                                std::string* error) {
  if (manifest.tag != expected_tag) {
    *error = "manifest is for a different release";
    return false;
  }
  if (StripLeadingV(manifest.tag) != manifest.version) {
    *error = "manifest version does not match its tag";
    return false;
  }
  const base::Version current(current_version);
  const base::Version offered(manifest.version);
  if (!current.IsValid() || !offered.IsValid() || offered <= current) {
    *error = "manifest is not newer than the running version";
    return false;
  }
  return true;
}

}  // namespace molt_ai
