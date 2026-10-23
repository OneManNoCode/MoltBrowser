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
// AutomationSecurity adds script-aware checks on top of the shared page-
// driving rules in molt_ai/security/agent_safety.h.
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
#include <string_view>
#include <vector>

#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"

namespace molt_ai {
namespace automation {

// Step::extra keys for a TYPE step whose value is a secret. Its "value" is
// never stored in the .molt file:
//   "secret": "vault"        — replay fills it from the encrypted vault
//                              (vault_store.h), matching the EXACT host of
//                              the page being filled
//   "secret_kind": "password" | "payment_card" | "one_time_code"
//                            — only passwords can be replayed; the vault
//                              holds no cards or one-time codes
//   "secret_host": host the field was recorded on (informational)
// "host" (any recorded step) is the page host the user acted on; it feeds
// AutomationSecurity::HostsReferencedBy.
inline constexpr char kSecretKey[] = "secret";
inline constexpr char kSecretVault[] = "vault";
inline constexpr char kSecretKindKey[] = "secret_kind";
inline constexpr char kSecretHostKey[] = "secret_host";
inline constexpr char kStepHostKey[] = "host";

// Why a draft from a web agent run (SecurityPolicy::sites_unconfirmed)
// doesn't run: the runner stops on it, the studio shows it.
inline constexpr char kDraftNeedsReviewMessage[] =
    "This draft was made by the AI agent from your run. Review its steps "
    "and confirm its sites in Agent mode before it can run.";

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
  // empty. An unconfirmed draft (sites_unconfirmed) gets neither the
  // widening nor its stored trust: its own whitelist (empty) at CASUAL. The
  // runner refuses to run one at all; this keeps any other caller from
  // reading it as allowed.
  static SecurityPolicy EffectivePolicy(const Script& script);

  // Hosts a script's steps visibly target: static http(s) NAVIGATE URLs
  // ({{variable}} URLs are excluded — those are what the whitelist guards)
  // and the page host the recorder stored in each step's extra["host"].
  static std::vector<std::string> HostsReferencedBy(
      const std::vector<Step>& steps);

  // Saving must never raise trust: |incoming| (a script about to be saved
  // from UI-supplied JSON) keeps |existing|'s trust if the script is
  // already on disk, else starts CASUAL. Only promoteTrust changes trust.
  // Nor does saving confirm a draft's sites: if |existing| is an
  // unconfirmed draft, so is |incoming| (only ConfirmDraftSites clears it).
  static void ClampTrustForSave(Script& incoming, const Script* existing);

  // The user confirms the sites of an unconfirmed draft (a real click in
  // the Agent studio). |shown|, the sites the studio showed them, must be
  // exactly HostsReferencedBy(script.steps) (in any order) and not empty,
  // so steps changed in between aren't confirmed blindly. The whitelist
  // becomes those sites and the draft mark is cleared; trust is unchanged.
  // Returns false, changing nothing, otherwise. The caller saves.
  static bool ConfirmDraftSites(Script& script,
                                const std::vector<std::string>& shown);

  // ---- Secret TYPE steps (see kSecretKey) ----

  // Turns |step| into a secret step of |kind| recorded on |host|: clears
  // its value and sets the extra keys above.
  static void MarkStepSecret(Step& step,
                             agent_safety::FieldKind kind,
                             std::string_view host);

  // The secret kind of a TYPE step, or kOrdinary if it is a plain one.
  static agent_safety::FieldKind SecretStepKind(const Step& step);

  // Removes plaintext secrets from a serialized script (Script::ToJSON()
  // shape). A "type" step with a non-empty value loses it and becomes a
  // secret step when it is already marked secret, was recorded on a
  // password field by the old recorder (its description shows the
  // "••••••" mask), or its selector names a password / card / one-time-code
  // field (agent_safety::ClassifySelector). Descriptions and run-history
  // entries that quote a removed value are redacted too. Returns true if
  // anything changed. AutomationStorage runs this on every save, and on
  // load (re-saving the file) so old plaintext does not linger.
  static bool ScrubPlaintextSecrets(base::DictValue& script_json);

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
