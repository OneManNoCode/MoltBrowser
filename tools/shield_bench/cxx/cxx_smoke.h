// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef SHIELD_BENCH_CXX_CXX_SMOKE_H_
#define SHIELD_BENCH_CXX_CXX_SMOKE_H_

#include <cstddef>
#include <cstdint>

#include "rust/cxx.h"

namespace shield_bench {

// Same resolver the bridge uses (test stand-in for Chromium's PSL), exposed
// so the benchmark computes third-party-ness exactly as the browser glue
// does (molt_shield_request_utils.cc).
size_t bench_domain_offset(rust::Str host);

// Drives the molt_adblock bridge through its *C++* API — the same calls
// MoltShieldEngine makes in the browser — and returns the number of failed
// checks (0 = pass). Prints a line per check.
int32_t run_cxx_smoke(rust::Str list_text, rust::Str resources_json);

}  // namespace shield_bench

#endif  // SHIELD_BENCH_CXX_CXX_SMOKE_H_
