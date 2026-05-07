// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_security.h"

#include <algorithm>

#include "base/strings/string_util.h"
#include "url/gurl.h"

namespace molt_ai {
namespace automation {

namespace {

// Heuristics for sensitive form fields by selector or value content.
bool LooksLikePasswordSelector(const std::string& selector) {
  std::string lower = base::ToLowerASCII(selector);
  return lower.find("password") != std::string::npos ||
         lower.find("type=\"password\"") != std::string::npos ||
         lower.find("type='password'") != std::string::npos;
}

bool LooksLikePaymentCardValue(const std::string& value) {
  // Strip spaces, then check for 13–19 digits (covers most card formats).
  std::string digits;
  for (char c : value) {
    if (c >= '0' && c <= '9')
      digits.push_back(c);
  }
  if (digits.size() < 13 || digits.size() > 19) return false;
  // Cheap Luhn check.
  int sum = 0;
  bool alt = false;
  for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
    int n = *it - '0';
    if (alt) {
      n *= 2;
      if (n > 9) n -= 9;
    }
    sum += n;
    alt = !alt;
  }
  return (sum % 10) == 0;
}

bool DomainMatchesWhitelist(const std::vector<std::string>& whitelist,
                             const std::string& host) {
  std::string h = base::ToLowerASCII(host);
  for (const auto& entry : whitelist) {
    std::string e = base::ToLowerASCII(entry);
    if (h == e) return true;
    // Suffix match: 'kayak.com' allows 'www.kayak.com'.
    if (h.size() > e.size() &&
        h.compare(h.size() - e.size() - 1, e.size() + 1, "." + e) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

AutomationSecurity::AutomationSecurity() = default;
AutomationSecurity::~AutomationSecurity() = default;

bool AutomationSecurity::AllowNavigation(const SecurityPolicy& policy,
                                          const std::string& url) const {
  if (policy.trust >= TrustLevel::TRUSTED)
    return true;
  GURL g(url);
  if (!g.is_valid())
    return false;
  return DomainMatchesWhitelist(policy.domain_whitelist, g.host());
}

bool AutomationSecurity::AllowClick(const SecurityPolicy& policy,
                                     const std::string& url,
                                     const std::string& selector) const {
  if (policy.trust >= TrustLevel::TRUSTED)
    return true;
  GURL g(url);
  if (!g.is_valid())
    return false;
  if (!DomainMatchesWhitelist(policy.domain_whitelist, g.host()))
    return false;
  // CASUAL trust forbids submit-shaped clicks unless explicitly approved.
  if (policy.trust == TrustLevel::CASUAL) {
    std::string lower = base::ToLowerASCII(selector);
    if (lower.find("submit") != std::string::npos)
      return false;
  }
  return true;
}

bool AutomationSecurity::AllowType(const SecurityPolicy& policy,
                                    const std::string& url,
                                    const std::string& selector,
                                    const std::string& value) const {
  if (policy.trust >= TrustLevel::TRUSTED)
    return true;
  GURL g(url);
  if (!g.is_valid())
    return false;
  if (!DomainMatchesWhitelist(policy.domain_whitelist, g.host()))
    return false;
  // Block password / card auto-fill below APPROVED.
  if (policy.trust < TrustLevel::APPROVED) {
    if (LooksLikePasswordSelector(selector))
      return false;
    if (LooksLikePaymentCardValue(value))
      return false;
  }
  return true;
}

bool AutomationSecurity::AllowHeadless(const SecurityPolicy& policy) const {
  return policy.trust >= TrustLevel::TRUSTED;
}

std::vector<std::string> AutomationSecurity::ApprovalRequired(
    const SecurityPolicy& policy) const {
  if (policy.trust >= TrustLevel::TRUSTED) return {};
  return policy.require_approval_for;
}

TrustLevel AutomationSecurity::ConsiderPromotion(TrustLevel current,
                                                  const Stats& stats) const {
  switch (current) {
    case TrustLevel::CASUAL:
      // After the first successful run, propose APPROVED.
      if (stats.successes >= 1) return TrustLevel::APPROVED;
      return TrustLevel::CASUAL;
    case TrustLevel::APPROVED:
      // After 5 successful runs, propose TRUSTED.
      if (stats.successes >= 5) return TrustLevel::TRUSTED;
      return TrustLevel::APPROVED;
    case TrustLevel::TRUSTED:
    case TrustLevel::ADMIN:
      // ADMIN requires explicit user opt-in, never auto-promoted.
      return current;
  }
  return current;
}

}  // namespace automation
}  // namespace molt_ai
