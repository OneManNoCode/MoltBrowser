// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's .onion helpers (tor/onion_util.h) on what web sites send: the
// Onion-Location response header (every occurrence) and page hosts.
//
// Checks: no memory or UB error; a header is honoured only when it occurs
// once, and then names an http(s) scheme and a lowercase v3 onion service
// host that is also in the .onion domain (the narrow check implies the
// broad one), and the host occurs in the header value; the broad check
// ignores case and trailing dots; Onion-Location is never honoured on a
// page that is not https or is already an onion service.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/browser/molt_ai/tor/onion_util.h"

namespace {

std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai::tor;
  FuzzedDataProvider p(data, size);

  std::vector<std::string> values;
  const size_t count = p.ConsumeIntegralInRange<size_t>(0, 3);
  for (size_t i = 0; i < count; ++i) {
    values.push_back(p.ConsumeRandomLengthString(2100));
  }
  const std::string page_scheme = p.ConsumeRandomLengthString(8);
  const std::string page_host = p.ConsumeRandomLengthString(300);

  const std::optional<OnionLocation> location =
      ParseOnionLocationHeaderValues(values);
  if (location) {
    CHECK(values.size() == 1) << "honoured a repeated header";
    CHECK(location->scheme == "http" || location->scheme == "https");
    CHECK(IsV3OnionServiceHost(location->host));
    CHECK(IsOnionHost(location->host));
    CHECK(location->host == Lower(location->host));
    CHECK(values[0].size() <= 2048 + 64);
    CHECK(Lower(values[0]).find(location->host) != std::string::npos);
  }

  if (IsV3OnionServiceHost(page_host)) CHECK(IsOnionHost(page_host));
  if (IsOnionHost(page_host)) {
    CHECK(IsOnionHost(Lower(page_host)));
    CHECK(IsOnionHost(page_host + "."));
  }
  if (ShouldHonorOnionLocation(page_scheme, page_host)) {
    CHECK(page_scheme == "https");
    CHECK(!IsOnionHost(page_host));
  }
  return 0;
}
