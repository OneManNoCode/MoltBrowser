// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// "Teach once, run again": a successful web agent run (agents/web_agent.h,
// started from the AI chat) saved as a DRAFT Agent-mode workflow
// (automation_script.h) that AutomationRunner can replay
// (docs/design/AGENT_ACTIONS.md §2.9).
//
// The run is model-made: the model chose every action, from page text a
// site controls. So the draft
//   * keeps only the steps that ran and succeeded, in order: NAVIGATE and
//     SEARCH as navigations, CLICK, TYPE and each FILL_FORM field as the
//     recorder's click / type steps (the observation's selector, the
//     element's label as the runner's text anchor, the page's host),
//     SCROLL as a scroll; OBSERVE, REPLAN, DONE and ERROR change nothing on
//     a page and are left out;
//   * never stores a secret: a value the agent typed that looks like a
//     password, key, card number or one-time code (by the field's label,
//     the text itself, or because the user's goal gave it as one) becomes a
//     {{variable}} asked at each run; a payment-card or one-time-code field
//     becomes a secret step (automation_security.h), which replay refuses;
//     a run whose page addresses look like they hold one isn't saved;
//   * starts CASUAL with an empty site whitelist and
//     SecurityPolicy::sites_unconfirmed: it can't run until the user has
//     reviewed it and confirmed its sites in the Agent studio, with a real
//     click (AutomationSecurity::ConfirmDraftSites);
//   * isn't made from a run that called a site's own tool (CALL_TOOL,
//     WebMCP), which no workflow step can replay, or from a MoltNet run.
// Nothing else of the run is kept: no page text, screenshot, plan or
// model reasoning, and the goal only as the draft's name when it holds
// nothing secret-looking.
//
// Pure logic (base::Value, GURL; no content/), unit-tested in
// molt_ai_unittests and fuzzed by molt_agent_run_draft_fuzzer. Every
// string here is untrusted: labels and selectors are page data, typed
// text, URLs and queries the model's.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AGENT_RUN_DRAFT_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AGENT_RUN_DRAFT_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"

namespace molt_ai {
namespace automation {
namespace agent_draft {

// ---------------------------------------------------------------------------
// The run, as the browser saw it (filled from WebAgent::Step).
// ---------------------------------------------------------------------------

// One element a CLICK / TYPE / FILL_FORM step acted on.
struct RunElement {
  RunElement();
  ~RunElement();
  RunElement(const RunElement&);
  RunElement& operator=(const RunElement&);
  RunElement(RunElement&&);
  RunElement& operator=(RunElement&&);

  // The observation's selector for it (an #id or an nth-of-type path the
  // browser's snapshot script built; page-derived).
  std::string selector;
  // link, button, input, textarea, select, checkbox, radio, editable.
  std::string kind;
  // The <input>'s type, lower case; "" for other elements.
  std::string input_type;
  // Its visible name, as the model was shown it (page text).
  std::string label;
  // What the probe found right before the action.
  agent_safety::FieldKind field_kind = agent_safety::FieldKind::kOrdinary;
  // TYPE / FILL_FORM: what the agent typed (model text).
  std::string text;
};

struct RunStep {
  RunStep();
  ~RunStep();
  RunStep(const RunStep&);
  RunStep& operator=(const RunStep&);
  RunStep(RunStep&&);
  RunStep& operator=(RunStep&&);

  // The action's wire name ("NAVIGATE", "SEARCH", "CLICK", "TYPE",
  // "FILL_FORM", "SCROLL", "OBSERVE", "DONE", "ERROR", "CALL_TOOL",
  // "REPLAN"), as WebAgent::Step::action.
  std::string action;
  bool success = false;
  // NAVIGATE / SEARCH: the URL loaded.
  std::string url;
  // SEARCH: the query (model text).
  std::string query;
  // SCROLL: "down" or "up".
  std::string direction;
  // CLICK / TYPE / FILL_FORM: the origin of the page acted on.
  std::string origin;
  // CLICK / TYPE: one element. FILL_FORM: the fields filled, in order. A
  // successful element step without them can't be replayed.
  std::vector<RunElement> elements;
};

struct Run {
  Run();
  ~Run();
  Run(const Run&);
  Run& operator=(const Run&);
  Run(Run&&);
  Run& operator=(Run&&);

  // The user's goal (their own words).
  std::string goal;
  // The tab's URL when the run started.
  std::string start_url;
  // The run ended with DONE.
  bool succeeded = false;
  // The chat or the tab it drove is MoltNet's.
  bool in_moltnet = false;
  // Every step the agent reported, in order (failed ones included).
  std::vector<RunStep> steps;
};

// ---------------------------------------------------------------------------
// The draft
// ---------------------------------------------------------------------------

enum class Refusal {
  kNone,
  kNotSucceeded,      // the run didn't end with DONE
  kMoltNet,           // saved workflows are off in MoltNet
  kUsedSiteTool,      // a CALL_TOOL succeeded
  kNothingToSave,     // no step that changes a page
  kUnusableStep,      // a step the draft can't replay faithfully
  kSecretInAddress,   // a page address looks like it holds a secret
  kTemplateText,      // "{{" in text a workflow would read as a variable
};

// Step::extra["review"]: what the studio points out on a step (fixed codes,
// never page or model text).
inline constexpr char kReviewKey[] = "review";
// The value typed was withheld; Step::value is a {{variable}} asked at
// each run.
inline constexpr char kReviewWithheld[] = "value_withheld";
// A payment-card / one-time-code field: a secret step (never replayed).
inline constexpr char kReviewSecretField[] = "secret_field";

// Caps. A step the agent couldn't have produced (over the agent's own caps)
// makes the run unusable rather than being cut down.
inline constexpr size_t kMaxDraftSteps = 200;
inline constexpr size_t kMaxTypedChars = 500;     // agent_actions' cap
inline constexpr size_t kMaxQueryChars = 256;     // agent_actions' cap
inline constexpr size_t kMaxUrlBytes = 2048;
inline constexpr size_t kMaxSelectorBytes = 1024; // agent_actions' cap
inline constexpr size_t kMaxLabelChars = 60;      // the snapshot's cap
inline constexpr size_t kMaxNameChars = 60;
inline constexpr size_t kMaxVariableNameChars = 24;

struct Draft {
  Draft();
  ~Draft();
  Draft(Draft&&);
  Draft& operator=(Draft&&);

  // Set iff |refusal| is kNone. Its id is empty: the caller assigns one.
  std::optional<Script> script;
  Refusal refusal = Refusal::kNone;
  // Fixed user-facing text: why it was refused, or what the draft needs.
  std::string message;
  // Values withheld (asked at each run) and secret-field steps.
  int withheld = 0;
  int secret_steps = 0;
};

// Builds the draft for |run|. |saved_workflows_allowed| is the profile's
// MoltNet access (MoltNetRequestAccess::saved_workflows). |now_unix| is the
// draft's creation time.
Draft BuildDraft(const Run& run, bool saved_workflows_allowed,
                 int64_t now_unix);

// The fixed text for a refusal ("" for kNone and kNotSucceeded, which the
// chat doesn't explain).
std::string_view RefusalText(Refusal refusal);

// ---------------------------------------------------------------------------
// The pieces (exposed for tests and the fuzzer)
// ---------------------------------------------------------------------------

// A field label that names a secret: password, PIN, CVV, security /
// verification code, one-time code, SSN, API key, card number, IBAN, ...
bool LabelSuggestsSecret(std::string_view label);

// Text that looks like a secret on its own: a password-like or key-like
// token, a card number (Luhn), an SSN, an IBAN, a one-time code (6-8
// digits), or words such as "password".
bool TextLooksSecret(std::string_view text);

// The tokens the user's goal gives as secrets: the word after "password",
// "PIN", "code", "card", ... ("my password is hunter2" -> "hunter2"), and
// tokens that TextLooksSecret() on their own. At least 3 bytes each.
std::vector<std::string> GoalSecretTokens(std::string_view goal);

// A URL that looks like it carries a credential: user info, a query or
// fragment parameter named like one (password, token, code, key, session,
// ...), a card number, SSN, JWT or API key anywhere, or a long token in a
// sign-in / reset / verify path.
bool UrlLooksSecret(std::string_view url);

// One line of display text: invalid UTF-8 and characters that reorder,
// hide or break text (C0 / C1 controls, bidi controls, zero-width
// characters, U+2028 / U+2029, U+FEFF) become U+FFFD or a space, runs of
// spaces one, no space at either end, at most |max_chars| code points (cut
// with "…").
std::string CleanDisplayText(std::string_view text, size_t max_chars);

// A {{variable}} name for a value withheld from a field labelled |label|:
// [a-z0-9_], at most kMaxVariableNameChars, starting with a letter, not in
// |taken| ("value" when the label gives nothing).
std::string VariableNameFor(std::string_view label,
                            const std::set<std::string>& taken);

}  // namespace agent_draft
}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AGENT_RUN_DRAFT_H_
