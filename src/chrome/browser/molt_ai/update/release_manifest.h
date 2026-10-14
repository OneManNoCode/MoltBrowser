// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Signed release manifests: the root of trust for in-app updates.
//
// Every release carries two extra assets next to the installers:
//   molt-release-manifest.json      what this release contains
//   molt-release-manifest.json.sig  an Ed25519 signature over it
//
// The manifest is signed OFFLINE with a key that never touches CI or GitHub
// (tools/release_signing/molt_release_sign.py). The browser only installs an
// installer whose size and SHA-256 match a manifest that verifies against a
// public key compiled into this build (update_signing_keys.cc), that names
// the release tag being installed, and whose version is newer than the
// running one. A compromised GitHub account, release asset, or CI runner can
// therefore no longer push code to users.
//
// Manifest (UTF-8 JSON, signed byte-for-byte as published):
//   {
//     "schema": 1,
//     "product": "MoltBrowser",
//     "version": "0.3.0",
//     "tag": "v0.3.0",
//     "assets": [
//       {"name": "MoltBrowser-macOS-arm64.dmg", "size": 929292681,
//        "sha256": "<64 lowercase hex>"},
//       ...
//     ]
//   }
// Signature file (JSON):
//   {"algorithm": "ed25519", "key_id": "<16 hex>", "signature": "<base64>"}
// The signed message is kReleaseManifestSigningContext followed by the
// manifest bytes, so a signature can never be replayed as a signature over
// some other kind of MoltBrowser data.

#ifndef CHROME_BROWSER_MOLT_AI_UPDATE_RELEASE_MANIFEST_H_
#define CHROME_BROWSER_MOLT_AI_UPDATE_RELEASE_MANIFEST_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"

namespace molt_ai {

inline constexpr char kReleaseManifestAssetName[] =
    "molt-release-manifest.json";
inline constexpr char kReleaseSignatureAssetName[] =
    "molt-release-manifest.json.sig";

// Domain-separation prefix for the signed message. Must match
// tools/release_signing/molt_release_sign.py exactly.
inline constexpr char kReleaseManifestSigningContext[] =
    "MoltBrowser release manifest v1\n";

// Download caps for the two small metadata files.
inline constexpr size_t kMaxReleaseManifestBytes = 64 * 1024;
inline constexpr size_t kMaxReleaseSignatureBytes = 4 * 1024;

struct ReleaseManifest {
  struct Asset {
    std::string name;
    int64_t size = 0;
    std::array<uint8_t, 32> sha256 = {};
  };

  ReleaseManifest();
  ReleaseManifest(const ReleaseManifest&);
  ReleaseManifest& operator=(const ReleaseManifest&);
  ~ReleaseManifest();

  // Returns the asset named |name|, or nullptr.
  const Asset* FindAsset(std::string_view name) const;

  std::string version;
  std::string tag;
  std::vector<Asset> assets;
};

// The exact bytes the release key signs for |manifest_bytes|.
std::string ReleaseManifestSignedMessage(std::string_view manifest_bytes);

// Short identifier of an Ed25519 public key: the first 8 bytes of its
// SHA-256, as 16 lowercase hex characters. Only used for diagnostics and to
// pick the key to try first; trust comes from the key list itself.
std::string UpdateSigningKeyId(base::span<const uint8_t, 32> public_key);

// Base64 (standard alphabet, padded) 32-byte Ed25519 public keys trusted to
// sign release manifests. Defined in update_signing_keys.cc. Empty entries
// are placeholders and are ignored.
base::span<const std::string_view> TrustedUpdateSigningKeys();

// True if |keys| contains at least one well-formed key.
bool HasUsableUpdateSigningKey(base::span<const std::string_view> keys);

// Verifies that |signature_file| is a valid Ed25519 signature over
// |manifest_bytes| by one of |trusted_keys_base64|. On failure returns false
// and sets |error| to a short reason.
bool VerifyReleaseManifestSignature(
    std::string_view manifest_bytes,
    std::string_view signature_file,
    base::span<const std::string_view> trusted_keys_base64,
    std::string* error);

// Parses and structurally validates a manifest. Rejects unknown schema
// versions, a product other than MoltBrowser, invalid versions, and assets
// without a positive size or a 64-hex-character SHA-256.
std::optional<ReleaseManifest> ParseReleaseManifest(
    std::string_view manifest_bytes,
    std::string* error);

// Release policy for a verified manifest: it must describe |expected_tag|
// (the release being installed), its version must match that tag, and it
// must be strictly newer than |current_version| (no downgrades, no replays of
// older signed manifests).
bool CheckReleaseManifestPolicy(const ReleaseManifest& manifest,
                                std::string_view expected_tag,
                                std::string_view current_version,
                                std::string* error);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_UPDATE_RELEASE_MANIFEST_H_
