// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_privacy.h"

#include <algorithm>

#include "base/strings/string_util.h"
#include "url/gurl.h"

namespace molt_ai {
namespace memory {

namespace {

// Substring matches against the lowercase host. Conservative defaults —
// users can lift any of these by adding the domain to an allowlist (not
// yet exposed in v1 UI but the plumbing is here).
const char* const kSensitiveHostSubstrings[] = {
    "bank", "credit", "wellsfargo", "chase.com", "americanexpress",
    "amex.com", "citibank", "capitalone", "hsbc", "barclays",
    "paypal.com", "venmo.com", "stripe.com",
    "1password.com", "lastpass.com", "bitwarden.com",
    "myhealth", "patientportal", "kaiserpermanente", "mychart",
    "irs.gov", ".gov.uk", "ato.gov.au", "ssa.gov",
    ".onion",
};

// Path-based exclusions — login/checkout/payment pages, regardless of
// the rest of the site being safe to capture.
const char* const kSensitivePathSubstrings[] = {
    "/login", "/signin", "/sign-in", "/auth", "/oauth", "/checkout",
    "/payment", "/billing", "/account/security", "/password",
};

bool ContainsAny(const std::string& haystack,
                  const char* const* needles, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (haystack.find(needles[i]) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

MemoryPrivacy::MemoryPrivacy() = default;
MemoryPrivacy::~MemoryPrivacy() = default;

bool MemoryPrivacy::ShouldCapture(const GURL& url) const {
  if (!url.is_valid()) return false;
  if (!url.SchemeIsHTTPOrHTTPS()) return false;

  std::string host = base::ToLowerASCII(url.host());
  std::string path = base::ToLowerASCII(url.path());

  // Skip the user's own MoltBrowser internal pages and dev tools, even
  // if they somehow showed up as http(s).
  if (host == "moltbrowser.local") return false;

  // Hard-coded sensitive categories.
  if (ContainsAny(host, kSensitiveHostSubstrings,
                   sizeof(kSensitiveHostSubstrings) /
                       sizeof(kSensitiveHostSubstrings[0]))) {
    return false;
  }
  if (ContainsAny(path, kSensitivePathSubstrings,
                   sizeof(kSensitivePathSubstrings) /
                       sizeof(kSensitivePathSubstrings[0]))) {
    return false;
  }

  // User-specified blocklist.
  for (const auto& blocked : blocked_hosts_) {
    std::string b = base::ToLowerASCII(blocked);
    if (host == b) return false;
    if (host.size() > b.size() + 1 &&
        host.compare(host.size() - b.size() - 1, b.size() + 1,
                     "." + b) == 0) {
      return false;
    }
  }

  return true;
}

void MemoryPrivacy::SetBlockedHosts(std::vector<std::string> hosts) {
  blocked_hosts_ = std::move(hosts);
}

}  // namespace memory
}  // namespace molt_ai
