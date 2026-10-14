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
//
// Enforcement: AutomationRunner checks every NAVIGATE / CLICK / TYPE /
// KEYPRESS step against EffectivePolicy(script) before (and, for the
// element-level checks, right after resolving) the step. A denied step
// fails the run with a message naming the rule; there is no in-run
// approval prompt — "approval" is the user raising the script's trust
// level via promoteTrust in molt://ai-automation. Saving or importing a
// script never raises trust (ClampTrustForSave).

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SECURITY_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SECURITY_H_

#include <string>
#include <vector>

#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"

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

  // True if the script may submit a form (submit-button click, Enter in a
  // form field). "form_submit" in ApprovalRequired() means the user must
  // have approved the script, i.e. trust >= APPROVED.
  bool AllowSubmit(const SecurityPolicy& policy) const;

  // True if the script may type into a field of |kind|. Passwords and
  // one-time codes fall under "login", payment cards under "payment";
  // either needs trust >= APPROVED while listed in ApprovalRequired().
  bool AllowSensitiveField(const SecurityPolicy& policy,
                           agent_safety::FieldKind kind) const;

  // The policy a run of |script| is checked against: the stored policy,
  // with the domain whitelist widened by HostsReferencedBy(script.steps)
  // (older / studio-saved scripts were stored with an empty whitelist) and
  // require_approval_for defaulted to {form_submit, payment, login} when
  // empty.
  static SecurityPolicy EffectivePolicy(const Script& script);

  // Hosts a script's steps visibly target: static http(s) NAVIGATE URLs
  // ({{variable}} URLs are excluded — those are what the whitelist guards)
  // and the page host the recorder stored in each step's extra["host"].
  static std::vector<std::string> HostsReferencedBy(
      const std::vector<Step>& steps);

  // Saving must never raise trust: |incoming| (a script about to be saved
  // from UI-supplied JSON) keeps |existing|'s trust if the script is
  // already on disk, else starts CASUAL. Only promoteTrust changes trust.
  static void ClampTrustForSave(Script& incoming, const Script* existing);

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
