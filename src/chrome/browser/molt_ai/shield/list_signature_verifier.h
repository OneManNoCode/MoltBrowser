// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Slot for verifying signed filter-list bundles from a GenEye CDN.
//
// Long-term plan: MoltShield fetches one bundle of its default lists from a
// GenEye CDN, signed with Ed25519 under the same key scheme as app updates
// (being built in //chrome/browser/molt_ai/update). Until that lands no
// verifier is installed, FilterListManager never fetches the CDN bundle, and
// lists come from their publishers over HTTPS (integrity = TLS + content
// sanity checks + sha256 change tracking).
//
// TODO(molt-update): implement with BoringSSL's ED25519_verify against the
// pinned public key(s) from molt_ai/update, then set kSignedBundleUrl in
// filter_list_catalog.h and install the verifier via
// FilterListManager::SetSignatureVerifier().

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_LIST_SIGNATURE_VERIFIER_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_LIST_SIGNATURE_VERIFIER_H_

#include <cstdint>

#include "base/containers/span.h"

namespace molt_ai::shield {

class ListSignatureVerifier {
 public:
  virtual ~ListSignatureVerifier() = default;

  // True iff `signature` is a valid detached Ed25519 signature of `payload`
  // by a trusted GenEye list-signing key. Must be callable on any sequence.
  virtual bool Verify(base::span<const uint8_t> payload,
                      base::span<const uint8_t> signature) const = 0;
};

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_LIST_SIGNATURE_VERIFIER_H_
