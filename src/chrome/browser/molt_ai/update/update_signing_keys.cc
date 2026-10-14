// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Public keys trusted to sign MoltBrowser release manifests (see
// release_manifest.h). The matching private keys are kept OFFLINE by the
// release owner and never stored on a CI runner or in this repository.
//
// Add or rotate a key with:
//   python3 tools/release_signing/molt_release_sign.py add-key <public key>
// which rewrites the block between the markers below. During a rotation keep
// both keys listed for at least one release so every installed version can
// verify the next update. A build with no key here cannot verify updates and
// will not install them automatically (it sends users to the release page).

#include <string_view>

#include "base/containers/span.h"
#include "chrome/browser/molt_ai/update/release_manifest.h"

namespace molt_ai {

namespace {

// Base64 of 32-byte Ed25519 public keys. Empty entries are ignored.
// MOLT_UPDATE_SIGNING_KEYS_BEGIN
constexpr std::string_view kTrustedUpdateSigningKeys[] = {
    "",  // No key yet: run `molt_release_sign.py keygen` offline.
};
// MOLT_UPDATE_SIGNING_KEYS_END

}  // namespace

base::span<const std::string_view> TrustedUpdateSigningKeys() {
  return kTrustedUpdateSigningKeys;
}

}  // namespace molt_ai
