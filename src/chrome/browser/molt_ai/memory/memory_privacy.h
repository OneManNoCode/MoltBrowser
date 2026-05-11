// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryPrivacy — central policy gate that decides whether a given
// URL is eligible for capture.
//
// Defaults skip:
//   - Non-http(s) schemes (chrome://, molt://, file://, devtools://,
//     about:, data:, javascript:, view-source:)
//   - Known sensitive categories (banks, healthcare portals, password
//     managers, government, .onion)
//   - URLs that look like login/checkout/payment flows
//   - The user's own MoltBrowser internal pages

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_PRIVACY_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_PRIVACY_H_

#include <string>
#include <vector>

class GURL;

namespace molt_ai {
namespace memory {

class MemoryPrivacy {
 public:
  MemoryPrivacy();
  ~MemoryPrivacy();

  // True if the URL should be captured. False for any reason in the
  // skip list above OR if the user blocked the host explicitly.
  bool ShouldCapture(const GURL& url) const;

  // Per-user host blocklist. Persisted by MemoryService.
  void SetBlockedHosts(std::vector<std::string> hosts);
  const std::vector<std::string>& blocked_hosts() const {
    return blocked_hosts_;
  }

 private:
  std::vector<std::string> blocked_hosts_;
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_PRIVACY_H_
