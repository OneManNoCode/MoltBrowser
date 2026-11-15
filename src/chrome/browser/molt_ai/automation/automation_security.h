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
// KEYPRESS / CALL_TOOL step against EffectivePolicy(script) before (and,
// for the element-level checks, right after resolving) the step. A denied
// step fails the run with a message naming the rule; the policy itself is
// never widened in a run — "approval" is the user raising the script's
// trust level via promoteTrust in molt://ai-automation. Two things ask the
// user in the middle of a run instead (run_questions.h), and only ever for
// that one step: an element found only by a weak fallback locator, and a
// site-tool call the policy doesn't let run unasked
// (docs/design/AGENT_ACTIONS.md §3.4, §3.5). Saving or importing a script
// never raises trust (ClampTrustForSave).
//
// A workflow file changed outside the browser (its SecurityDigest() isn't
// the one the browser recorded for that file, workflow_record.h) starts
// CASUAL and unconfirmed, whatever it says, with only what the Agent
// studio's review shows (ClampUnvouched(), an allowlist): it runs, fires
// and is run by another workflow only once the user has reviewed it and
// confirmed its sites in the Agent studio, and climbs the staircase again
// from there (docs/design/AGENT_ACTIONS.md §3.6).

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
// AutomationSecurity::HostsReferencedBy. "text" (a recorded CLICK / TYPE)
// is the element's visible text, which the runner checks a selector
// against and looks for when every selector misses; "ref_shot" a picture
// of the element when it was recorded (shown in the editor).
inline constexpr char kSecretKey[] = "secret";
inline constexpr char kSecretVault[] = "vault";
inline constexpr char kSecretKindKey[] = "secret_kind";
inline constexpr char kSecretHostKey[] = "secret_host";
inline constexpr char kStepHostKey[] = "host";
inline constexpr char kStepTextAnchorKey[] = "text";
inline constexpr char kStepRefShotKey[] = "ref_shot";

// Why a draft from a web agent run (SecurityPolicy::sites_unconfirmed)
// doesn't run: the runner stops on it, the studio shows it.
inline constexpr char kDraftNeedsReviewMessage[] =
    "This draft was made by the AI agent from your run. Review its steps "
    "and confirm its sites in Agent mode before it can run.";

// Why a workflow whose file was changed outside the browser
// (SecurityPolicy::changed_outside) doesn't run, fire or get trusted.
inline constexpr char kChangedOutsideMessage[] =
    "This workflow's file was changed outside MoltBrowser. Review its steps, "
    "sites and schedule and confirm them in Agent mode before it can run.";

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
  // unconfirmed draft, so is |incoming| (only ConfirmDraftSites clears it),
  // with |existing|'s changed_outside.
  // And a routine taught by voice that a save brings in (|incoming| has
  // voice steps, |existing| is null or had none: an import, a copy) is
  // saved as an unconfirmed draft, with no sites.
  static void ClampTrustForSave(Script& incoming, const Script* existing);

  // The user confirms the sites of an unconfirmed draft (a real click in
  // the Agent studio). |shown|, the sites the studio showed them, must be
  // exactly HostsReferencedBy(script.steps) (in any order) and not empty,
  // so steps changed in between aren't confirmed blindly. The whitelist
  // becomes those sites and the draft mark (and changed_outside) is
  // cleared; trust is unchanged. Returns false, changing nothing,
  // otherwise. The caller saves.
  static bool ConfirmDraftSites(Script& script,
                                const std::vector<std::string>& shown);

  // ---- Workflow files the browser didn't write (workflow_record.h) ----

  // Lowercase hex SHA-256 of the parsed workflow without its run
  // bookkeeping: Script::ToJSON(), plaintext secrets scrubbed (so the
  // scrub a load does never changes it), then "id", "stats" and
  // "trigger.last_fired_unix" left out. Everything else counts: the name,
  // version, creation time, model, enabled flag, variables, the rest of
  // the trigger, every field of every step (in order) and the whole
  // security policy (changed_outside included). Taken over the parsed
  // script, so the file's formatting, key order and unknown keys don't
  // matter, and it covers exactly what the runner executes. "" if it can't
  // be computed; "" is never recorded nor matched.
  static std::string SecurityDigest(const Script& script);

  // The SecurityDigest() a reader computes from the file a save of
  // |script| writes: serialized as AutomationStorage writes it (scrubbed,
  // pretty-printed), read back and parsed. "" if that fails.
  static std::string DigestAsStored(const Script& script);

  // A file the browser didn't write (or that changed since) loads as an
  // unconfirmed import, with only what the Agent studio's review shows, as
  // the runner uses it (an allowlist; everything else is dropped, any key a
  // later build adds included):
  //   - the script: its id, name, creation time, the names of its variables
  //     (their default values ""), and its schedule's kind and expression
  //     (none when the file has it turned off or the expression holds a
  //     character the review can't show; no time zone, no end); the default
  //     policy (CASUAL, no sites, the default approvals and limits) with
  //     sites_unconfirmed and changed_outside set; no run history;
  //   - each step the review shows in full, with only the fields it shows:
  //     NAVIGATE, CLICK, WAIT_FOR (target); TYPE, ASSERT (target, value);
  //     SCROLL, KEYPRESS, WAIT, NOTIFY (value); EXTRACT (target, store_as);
  //     CALL_TOOL (target). Its extra keeps only "host" (printable ASCII);
  //     a TYPE step's "secret" as "vault" with its "secret_kind" as the
  //     runner reads it (and no value); a CALL_TOOL step's "tool" with only
  //     its "origin", "frame" ("origin", "path"), "args", "secret_args" and
  //     "form" (when true). Every other field is its default (no
  //     description, fallback selectors, locators, text anchor, picture,
  //     timeout, retries, transform or review note);
  //   - a step of a routine taught by voice (extra["voice"]): its type,
  //     target and value, and extra "voice" and "host" only (never a
  //     secret key); its claim keeps of extra["voice"] only what the review
  //     describes (voice::DescribeRoutineStepsAsRun());
  //   - every other step is dropped: IF, ELSE, END_IF, LOOP, END_LOOP,
  //     SCREENSHOT, RUN_JS, RUN_SCRIPT, AI_DECIDE, AI_EXTRACT, UNKNOWN, and
  //     a step whose shown target, value or store_as holds a control
  //     character (a line break, a tab), DEL or a character that reorders,
  //     hides or breaks text (agents/hidden_chars.h), and a CALL_TOOL step
  //     whose "args" hold a name or string (in a list too) with a control
  //     character or DEL.
  // Clamping twice is clamping once.
  static void ClampUnvouched(Script& script);

  // Whether the scheduler may fire |script| unattended: enabled, its sites
  // confirmed, and not a routine taught by voice (voice mode runs those).
  static bool MayFireUnattended(const Script& script);
  // Whether a RUN_SCRIPT step may run |script|'s steps inside another
  // workflow's run (at that run's trust): its sites confirmed, and not a
  // routine taught by voice.
  static bool MayRunAsChild(const Script& script);
  // What the user is told about an unconfirmed |script|:
  // kChangedOutsideMessage or kDraftNeedsReviewMessage.
  static const char* ReviewMessage(const Script& script);

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
