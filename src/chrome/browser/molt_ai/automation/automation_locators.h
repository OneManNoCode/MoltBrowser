// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Fallback locators for a workflow step's element (docs/design/
// AGENT_ACTIONS.md §3.4).
//
// When a step is recorded (AutomationRecorder) or saved from a web agent run
// (agent_run_draft.h), the browser captures, from the element the step acted
// on, a small ordered list of other ways to find it again: its accessible
// role and name, its label, stable attributes, its text and a structural
// path, strongest first. They are stored on the step (Step::locators, the
// optional "locators" key of a .molt step) with what the element was: its
// kind (tag, input type, role), whether it was a credential field, and the
// origin of the page.
//
// At replay, when the step's recorded selector doesn't find the element as
// recorded, AutomationRunner runs one probe script that evaluates every
// locator on the page and reports what each found; DecideLocatorMatch()
// then picks the element, in order:
//   * never an element of another kind (another tag, input type class or
//     role), never one on another origin, and never a credential field
//     (password, payment card, one-time code) unless the step's element
//     was one of that kind;
//   * the first locator, in order, that finds exactly one such element: if
//     it is a strong one (role and name, label, id / name / data-testid
//     style attributes, the recorded selector) and no other strong locator
//     finds a different element, that element; if strong locators disagree,
//     or only weak ones (text, path, label-like attributes) find it, the
//     runner asks the user before acting on it.
// The run's log records which locator found the element.
//
// Everything here is page data: the capture report and the probe report
// come from scripts that run in the page (in an isolated world), and
// locators come from .molt files, which may be imported. Parsing checks
// every field and caps every size; an entry that fails a check is dropped,
// a block that fails one at its root is dropped whole, and the step keeps
// its recorded selector. Old files without locators replay as before.
//
// Pure logic (base::Value; no content/), unit-tested in molt_ai_unittests
// and fuzzed (molt_automation_locators_fuzzer).

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_LOCATORS_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_LOCATORS_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"

namespace molt_ai {
namespace automation {
namespace locators {

// ---------------------------------------------------------------------------
// Caps
// ---------------------------------------------------------------------------

// Locators kept per step.
inline constexpr size_t kMaxLocators = 8;
// A role's name, a label, a text, an attribute's value: characters.
inline constexpr size_t kMaxLocatorTextChars = 120;
// A structural path (a CSS selector the capture script built): bytes, and
// its steps.
inline constexpr size_t kMaxPathBytes = 512;
inline constexpr size_t kMaxPathSteps = 12;
inline constexpr size_t kMaxTagChars = 32;
inline constexpr size_t kMaxRoleChars = 32;
inline constexpr size_t kMaxTypeChars = 24;
inline constexpr size_t kMaxAutocompleteChars = 64;
inline constexpr size_t kMaxOriginBytes = 256;
// The probe: candidate elements reported in all, matches per locator.
inline constexpr size_t kMaxProbeCandidates = 32;
inline constexpr size_t kMaxMatchesPerLocator = 8;
// A candidate's label as the question shows it.
inline constexpr size_t kMaxShownLabelChars = 60;
// The probe's and pin's tokens: 1-64 of [A-Za-z0-9].
inline constexpr size_t kMaxTokenChars = 64;

// ---------------------------------------------------------------------------
// Locators
// ---------------------------------------------------------------------------

enum class By {
  kRole,       // an accessible role and name: role, value = the name
  kLabel,      // a form field's <label> text: value
  kAttribute,  // name = one of kAttributes, value
  kText,       // a clickable element's visible text: value
  kPath,       // a structural CSS path: value
  // The step's recorded selector (Step::target), probed first. Never
  // stored in "locators"; built by the runner.
  kSelector,
};

// The attributes a locator may name: the strong ones (an element's stable
// identity), then weak ones (text a page also shows).
inline constexpr std::string_view kStrongAttributes[] = {
    "id", "name", "data-testid", "data-test", "data-cy", "data-qa"};
inline constexpr std::string_view kWeakAttributes[] = {
    "aria-label", "placeholder", "title", "alt"};

struct Locator {
  Locator();
  ~Locator();
  Locator(const Locator&);
  Locator& operator=(const Locator&);
  Locator(Locator&&);
  Locator& operator=(Locator&&);

  By by = By::kText;
  // kRole: [a-z]{1,32}.
  std::string role;
  // kAttribute: one of kStrongAttributes / kWeakAttributes.
  std::string name;
  // kRole: the accessible name; kLabel / kText: the text; kAttribute: the
  // value; kPath / kSelector: the selector. One line, no control
  // characters, within its cap.
  std::string value;

  friend bool operator==(const Locator&, const Locator&) = default;
};

// What the element was: lower-case tag, the class of an <input>'s type
// (TypeClass()), and its effective role (explicit, else implicit).
struct ElementKind {
  std::string tag;
  std::string type;
  std::string role;

  friend bool operator==(const ElementKind&, const ElementKind&) = default;
};

struct StepLocators {
  StepLocators();
  ~StepLocators();
  StepLocators(const StepLocators&);
  StepLocators& operator=(const StepLocators&);
  StepLocators(StepLocators&&);
  StepLocators& operator=(StepLocators&&);

  // url::Origin serialization of the page the element was on: http(s).
  std::string origin;
  ElementKind element;
  // Whether it was a credential field, and which kind.
  agent_safety::FieldKind field = agent_safety::FieldKind::kOrdinary;
  // At most kMaxLocators, strongest first, none twice.
  std::vector<Locator> list;

  friend bool operator==(const StepLocators&, const StepLocators&) = default;
};

// An <input>'s type as a kind: text-like types (text, search, email, url,
// tel, number, and an empty or unknown type) are "text", button-like ones
// (submit, button, reset, image) "button", the others themselves
// (password, checkbox, radio, date, ...). "" for other elements.
std::string TypeClass(std::string_view tag, std::string_view input_type);

// Whether |live| is the same kind of element as |recorded|.
bool KindMatches(const ElementKind& recorded, const ElementKind& live);

// A strong locator: role and name, label, a strong attribute, the
// recorded selector. Text, path and weak attributes are weak.
bool IsStrong(const Locator& locator);

// How the run's log and the user's question say how the element was found
// ("its role and name", "its label", "its data-testid", "its text", "its
// position on the page", "the recorded selector"). Fixed text.
std::string Describe(const Locator& locator);

// The .molt form ({"origin", "element": {tag, type, role}, "field",
// "list": [{"by", "role"?, "name"?, "value"}]}) and back. Parse() returns
// nullopt if the root is malformed (not an object, an origin that isn't
// http(s), a bad element kind, no usable locator); entries that fail a
// check are dropped, the first kMaxLocators good ones kept.
base::Value ToValue(const StepLocators& locators);
std::optional<StepLocators> Parse(const base::Value& value);

// The capture script's report for one element ({"origin", "tag", "type",
// "role", "input_type", "autocomplete", "list"}; see CaptureFunction()):
// the same checks as Parse(), the field kind from the reported input type
// and autocomplete (agent_safety::ClassifyField).
std::optional<StepLocators> FromCaptureReport(const base::Value& report);

// ---------------------------------------------------------------------------
// Capture (scripts run in the page's isolated world)
// ---------------------------------------------------------------------------

// A script that defines `__moltLocatorLib` (a var, so it can run again)
// with capture(el) -> the report above, and the helpers the probe uses.
// The recorder runs it before its own script.
std::string LibraryScript();

// LibraryScript() and a capture of document.querySelector(|selector|) in
// the same script: the report, or null if nothing matches. For the web
// agent, right after it acted on |selector|.
std::string BuildCaptureScript(std::string_view selector);

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

// One element the probe found.
struct Candidate {
  Candidate();
  ~Candidate();
  Candidate(const Candidate&);
  Candidate& operator=(const Candidate&);
  Candidate(Candidate&&);
  Candidate& operator=(Candidate&&);

  ElementKind kind;
  agent_safety::FieldKind field = agent_safety::FieldKind::kOrdinary;
  // Its name as the user reads it (page text, cleaned: CleanLabel()).
  std::string label;
};

struct Probe {
  Probe();
  ~Probe();
  Probe(const Probe&);
  Probe& operator=(const Probe&);
  Probe(Probe&&);
  Probe& operator=(Probe&&);

  // location.origin of the document probed.
  std::string origin;
  std::vector<Candidate> candidates;
  // For each locator probed, in order: the visible candidates it found
  // (indices into |candidates|), and whether it found more than
  // kMaxMatchesPerLocator.
  std::vector<std::vector<size_t>> matches;
  std::vector<bool> overflow;
};

// The locators the runner probes, in order: the step's recorded selector
// first (if any; |selector| is what the runner resolved it to), then the
// stored ones.
std::vector<Locator> ProbeOrder(std::string_view selector,
                                const StepLocators& stored);

// Evaluates every locator of |order| in the document and keeps what it
// found in the isolated world under |probe_id| (for the pin script). The
// recorded selector's matches must agree with |text_hint| (the step's text
// anchor) as the runner's selector check requires. Evaluates to the probe
// report ParseProbe() reads.
std::string BuildProbeScript(std::string_view probe_id,
                             const std::vector<Locator>& order,
                             std::string_view text_hint);

// Reads the probe's report; nullopt unless it has exactly |locators|
// entries, at most kMaxProbeCandidates candidates and valid indices.
std::optional<Probe> ParseProbe(const base::Value& report, size_t locators);

enum class Strength {
  kNone,      // no locator found exactly one eligible element
  kStrong,    // a strong locator did, and no other strong one disagrees
  kWeak,      // only a weak locator did: ask the user
  kConflict,  // strong locators found different elements: ask the user
};

struct Decision {
  Strength strength = Strength::kNone;
  // Valid unless kNone: the element (an index into Probe::candidates) and
  // the locator that found it (an index into the probe order).
  size_t candidate = 0;
  size_t locator = 0;
};

// Whether |candidate| may stand in for the recorded element: the same kind,
// and a credential field only if the recorded element was one of the same
// kind.
bool Eligible(const StepLocators& recorded, const Candidate& candidate);

// Picks the element, as at the top of this file. |page_origin| is the
// browser's committed origin of the document probed: both it and the
// probe's must be the recorded origin.
Decision Decide(const std::vector<Locator>& order,
                const Probe& probe,
                const StepLocators& recorded,
                std::string_view page_origin);

// Marks the candidate |index| of probe |probe_id| with the attribute
// data-molt-pick="|token|" (1-64 of [A-Za-z0-9], unguessable) and keeps it
// in the isolated world as the pinned element. Evaluates to PinSelector()
// on success, '' otherwise.
std::string BuildPinScript(std::string_view probe_id,
                           size_t index,
                           std::string_view token);
std::string PinSelector(std::string_view token);

// The check the runner's click / type / fill scripts make on the element
// they found by PinSelector(|token|): it must be the very element pinned.
// A JS boolean expression of `el`.
std::string PinnedCheck(std::string_view token);

// One clean line of page text for the user: invalid UTF-8 as U+FFFD,
// control characters as spaces, characters that hide or reorder text as
// U+FFFD (hidden_chars.h), quotes as single quotes, runs of spaces as one,
// at most |max_chars| characters (cut with "…").
std::string CleanLabel(std::string_view text, size_t max_chars);

// What the element is, in words for the user: "button", "link", "text
// field", "checkbox", ... (from its kind). Fixed text.
std::string KindWord(const ElementKind& kind);

// The question the runner asks for a weak or conflicting match: names the
// step (its description, cleaned), the element found (kind and label) and
// how it was found.
std::string WeakMatchQuestion(std::string_view step_description,
                              const Candidate& candidate,
                              const Locator& found_by,
                              Strength strength);

}  // namespace locators
}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_LOCATORS_H_
