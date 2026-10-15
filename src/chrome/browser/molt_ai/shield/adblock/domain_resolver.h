// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// C++ half of the molt_adblock cxx bridge: lets adblock-rust resolve
// registrable domains (eTLD+1) with Chromium's public-suffix list instead of
// the crate's embedded copy, so MoltShield's notion of first/third party
// matches the rest of the browser and updates with Chromium.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_ADBLOCK_DOMAIN_RESOLVER_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_ADBLOCK_DOMAIN_RESOLVER_H_

#include <cstddef>

#include "third_party/rust/cxx/v1/cxx.h"

namespace molt_ai::shield::ffi {

// Byte offset within `host` at which its registrable domain starts
// (private registries included). 0 when there is none (IP literal, bare
// public suffix, single-label host), which adblock-rust treats as "the
// whole host is the domain".
size_t registrable_domain_offset(rust::Str host);

}  // namespace molt_ai::shield::ffi

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_ADBLOCK_DOMAIN_RESOLVER_H_
