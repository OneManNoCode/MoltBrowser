// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/security/webui_navigation_policy.h"

#include <string>

namespace molt_ai::security {

namespace {

// Spelled out rather than taken from content/ and chrome/common so this
// stays a std + //url target for the standalone unit-test binary.
constexpr char kChromeUIScheme[] = "chrome";     // content::kChromeUIScheme
constexpr char kDevToolsScheme[] = "devtools";   // content::kChromeDevToolsScheme
constexpr char kMoltScheme[] = "molt";           // chrome::kMoltScheme

}  // namespace

bool ShouldBlockWebInitiatedWebUINavigation(
    const GURL& url,
    bool renderer_initiated,
    bool is_history_or_reload,
    const std::optional<url::Origin>& initiator_origin) {
  if (!renderer_initiated || is_history_or_reload) {
    return false;
  }
  if (!url.SchemeIs(kChromeUIScheme) && !url.SchemeIs(kMoltScheme)) {
    return false;
  }
  if (!initiator_origin || initiator_origin->opaque()) {
    return true;
  }
  const std::string& scheme = initiator_origin->scheme();
  return scheme != kChromeUIScheme && scheme != kDevToolsScheme;
}

}  // namespace molt_ai::security
