// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Security policy enforcement for automation scripts.
//
// The Trust Staircase:
//   CASUAL   → recorded once. Read-only DOM, navigate within whitelist.
//   APPROVED → user clicked Approve once. Plus form_submit / clicks off
//              whitelist on the same domain.
//   TRUSTED  → 5+ successful runs and user marked Trusted. Plus headless
//              runs, persists cookies between runs.
//   ADMIN    → user explicitly toggled. Plus access local AI for arbitrary
//              decisions and chain other automations.
//
// AutomationSecurity wraps the existing ActionValidator (used by the
// agent engine) with script-aware checks.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SECURITY_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SECURITY_H_

#include <string>
#include <vector>

#include "chrome/browser/molt_ai/automation/automation_script.h"

namespace molt_ai {
namespace automation {

class AutomationSecurity {
 public:
  AutomationSecurity();
  ~AutomationSecurity();

  // Returns true if a navigation to |url| is permitted by |policy|.
  // - CASUAL: only the domain whitelist
  // - APPROVED: whitelist + same-eTLD as any whitelist entry
  // - TRUSTED+: any URL
  bool AllowNavigation(const SecurityPolicy& policy,
                       const std::string& url) const;

  // Returns true if a click on |selector| in |url| is permitted.
  bool AllowClick(const SecurityPolicy& policy,
                  const std::string& url,
                  const std::string& selector) const;

  // Returns true if typing |value| into |selector| is permitted.
  // Auto-blocks password / payment-card heuristics for trust < APPROVED.
  bool AllowType(const SecurityPolicy& policy,
                 const std::string& url,
                 const std::string& selector,
                 const std::string& value) const;

  // Returns true if running headless is allowed (TRUSTED+).
  bool AllowHeadless(const SecurityPolicy& policy) const;

  // Returns the list of action types that need user approval right now,
  // given the current trust level. The runner can use this to synchronously
  // pop a confirmation prompt before executing.
  std::vector<std::string> ApprovalRequired(
      const SecurityPolicy& policy) const;

  // Promote the trust level after a successful run, following the rules:
  // - CASUAL → APPROVED after first successful run
  // - APPROVED → TRUSTED after 5 successful runs
  // - TRUSTED → ADMIN only by explicit user toggle (NOT auto-promoted)
  // Returns the new trust level (may equal the old one).
  TrustLevel ConsiderPromotion(TrustLevel current,
                                const Stats& stats) const;
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SECURITY_H_
