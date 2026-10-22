// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The web agent's action format (docs/design/AGENT_ACTIONS.md).
//
// WebAgent asks the on-device model for its next step with grammar-
// constrained decoding (PromptOptions::json_schema). BuildActionSchema()
// describes exactly the actions WebAgent::RunAction() performs, with the
// element ids of the last observation as enums and tight length caps, so
// an honest model can only write one well-formed action.
//
// The grammar is not a security boundary. The reply comes from the
// sandboxed inference process, which the browser treats as hostile, so
// ParseAction() checks it again here with base::JSONReader: the action
// name, the exact set of keys, their types and lengths, the characters,
// and element ids that must exist in the observation the model was shown.
//
// None of the page-facing checks move here: every action still goes
// through WebAgent's origin scope and confirmations and agent_safety's
// element probe (http(s) only, never a password field, approval for form
// submits, other sites, payment and one-time-code fields).
//
// The AI chat's page actions (chat_actions.h) use the same format with a
// smaller set of actions (ActionSet).
//
// CALL_TOOL calls a tool the page itself offers (WebMCP; agent_tools.h,
// docs/design/AGENT_ACTIONS.md §2.7): a tool id from the observation and
// its arguments, under a schema built from the page's own, which
// agent_tools converts only when it fits the grammar's subset.
//
// With a plan (the planner, agent_plan.h, §2.8), every action also says
// which plan step it works on ("step", PlanSteps), and REPLAN asks for a
// new plan. Without one, the schema and the parser are what they were.
//
// Pure logic (base::Value, no content/), unit-tested in molt_ai_unittests.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_ACTIONS_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_ACTIONS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/agents/agent_tools.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"

namespace base {
class Value;
}  // namespace base

namespace molt_ai {
namespace agent_actions {

enum class ActionType {
  kNavigate,
  kSearch,
  kClick,
  kType,
  kFillForm,
  kScroll,
  kObserve,
  kDone,
  kError,
  kCallTool,  // a site tool (WebMCP): the web agent only
  kReplan,    // ask for a new plan (agent_plan.h): the web agent only
};

// The wire name ("NAVIGATE", "FILL_FORM", ...), used in the schema, the
// reply and WebAgent::Step::action.
std::string_view ActionName(ActionType type);
// Exact (case-sensitive) inverse of ActionName().
std::optional<ActionType> ActionFromName(std::string_view name);

// A set of actions: a bit mask of ActionBit()s. The schema offers only the
// set's actions and ParseAction() refuses the others.
using ActionSet = uint32_t;
constexpr ActionSet ActionBit(ActionType type) {
  return ActionSet{1} << static_cast<int>(type);
}
constexpr bool HasAction(ActionSet set, ActionType type) {
  return (set & ActionBit(type)) != 0;
}
// The web agent's set: every action but REPLAN.
inline constexpr ActionSet kAgentActions =
    ActionBit(ActionType::kNavigate) | ActionBit(ActionType::kSearch) |
    ActionBit(ActionType::kClick) | ActionBit(ActionType::kType) |
    ActionBit(ActionType::kFillForm) | ActionBit(ActionType::kScroll) |
    ActionBit(ActionType::kObserve) | ActionBit(ActionType::kDone) |
    ActionBit(ActionType::kError) | ActionBit(ActionType::kCallTool);
// ... and while it follows a plan (agent_plan.h): REPLAN too.
inline constexpr ActionSet kPlannedAgentActions =
    kAgentActions | ActionBit(ActionType::kReplan);

// Caps. Lengths count characters (Unicode code points), as the grammar's
// maxLength does.
inline constexpr size_t kMaxUrlChars = 1024;
inline constexpr size_t kMaxQueryChars = 256;
inline constexpr size_t kMaxTypedChars = 500;
inline constexpr size_t kMaxReasonChars = 160;
inline constexpr size_t kMaxResultChars = 1000;
inline constexpr size_t kMaxFormFields = 8;
// Interactive elements listed per observation (e1 ... e40).
inline constexpr size_t kMaxObservedElements = 40;
inline constexpr size_t kMaxElementLabelChars = 60;
inline constexpr size_t kMaxSelectorBytes = 1024;
// Visible-token budget for one reply.
inline constexpr int kMaxReplyTokens = 512;
// A step's reply is requested at most this many times; the run stops at
// the last unusable one (cut off, not JSON, or refused by ParseAction).
inline constexpr int kMaxAttemptsPerStep = 3;
// ... and a run stops at its kMaxBadRepliesPerRun-th unusable reply.
inline constexpr int kMaxBadRepliesPerRun = 6;

// ---------------------------------------------------------------------------
// Observation: the interactive elements the model may act on.
// ---------------------------------------------------------------------------

struct ObservedElement {
  ObservedElement();
  ~ObservedElement();
  ObservedElement(const ObservedElement&);
  ObservedElement& operator=(const ObservedElement&);
  ObservedElement(ObservedElement&&);
  ObservedElement& operator=(ObservedElement&&);

  // "e1", "e2", ...: assigned by ParseElementSnapshot, in page order.
  std::string ref;
  // CSS selector the snapshot script built (an #id, or an nth-of-type path
  // from <html>). Page-derived: only ever handed to querySelector as a
  // JSON string, never spliced into script source.
  std::string selector;
  // link, button, input, textarea, select, checkbox, radio, editable.
  std::string kind;
  // The <input>'s type, lower case; "" for other elements.
  std::string input_type;
  // Visible name (page text): no quotes, line breaks or control
  // characters, at most kMaxElementLabelChars characters.
  std::string label;
  agent_safety::FieldKind field_kind = agent_safety::FieldKind::kOrdinary;
  // A text field TYPE / FILL_FORM may write: a text-like <input> or a
  // <textarea>; never a password field.
  bool typeable = false;
};

struct Observation {
  Observation();
  ~Observation();
  Observation(const Observation&);
  Observation& operator=(const Observation&);
  Observation(Observation&&);
  Observation& operator=(Observation&&);

  const ObservedElement* Find(std::string_view ref) const;
  bool HasTypeable() const;
  const agent_tools::SiteTool* FindTool(std::string_view ref) const;

  std::vector<ObservedElement> elements;
  // The page's own tools (t1 ... t8), added by AddSiteTools().
  std::vector<agent_tools::SiteTool> tools;
};

// A script (run it in an isolated world) that lists the page's visible
// interactive elements: in-viewport ones first, then form fields, buttons
// and links further down, at most kMaxObservedElements, back in page order.
// It evaluates to {origin: location.origin, elements: [{sel, kind, type,
// ac, label}, ...]}.
std::string BuildElementSnapshotScript();

// Parses the script's result. Entries that are malformed or over a cap
// are skipped; at most kMaxObservedElements are kept. |origin| gets the
// origin the script ran in ("" if missing).
Observation ParseElementSnapshot(const base::Value& result,
                                 std::string* origin);

// `button "Search"`, `input[email] "Email" (text field)`: for the step
// trace and confirmation messages.
std::string DescribeElement(const ObservedElement& element);

// One `e3 button "Search"` line per element, for the prompt. The caller
// fences it with the rest of the page text.
std::string DescribeElements(const Observation& observation);

// Adds the page's tools (agent_tools::MakeSiteTool()) to |observation|, in
// order, as t1, t2, ...: at most agent_tools::kMaxSiteTools, names not
// repeated, the TOOLS text within agent_tools::kMaxToolsTextChars, and
// each kept only if the web agent's action schema with it is still one the
// inference service converts (JsonSchemaToGrammar(), the service's own
// converter). A tool that doesn't fit is left out, never cut down. With
// |with_plan| (the run has the planner on) the schema checked is the one
// with a plan (kPlannedAgentActions, kWidestPlanSteps), which is the
// largest the run sends.
void AddSiteTools(std::vector<agent_tools::SiteTool> tools,
                  Observation* observation,
                  bool with_plan = false);

// ---------------------------------------------------------------------------
// The action and its schema
// ---------------------------------------------------------------------------

// With a plan (agent_plan.h), every action also carries "step", after
// "reason": the number (1-based) of the plan step it works on. That is
// |current|, or |current| + 1 when |current| < |count|, which reports the
// current step done. Nothing else: a plan step is never skipped, and the
// number only moves the browser's marker of the current step.
struct PlanSteps {
  int current = 1;
  int count = 1;
};

// The step numbers |plan| allows, in order (|current| clamped to
// 1..|count|).
std::vector<int> AllowedPlanSteps(const PlanSteps& plan);

// The PlanSteps AddSiteTools() checks a tool against when the run may have
// a plan: the widest "step" (two numbers) and REPLAN.
inline constexpr PlanSteps kWidestPlanSteps = {1, 2};

struct FormFieldValue {
  std::string ref;
  std::string text;
};

struct AgentAction {
  AgentAction();
  ~AgentAction();
  AgentAction(const AgentAction&);
  AgentAction& operator=(const AgentAction&);
  AgentAction(AgentAction&&);
  AgentAction& operator=(AgentAction&&);

  ActionType type = ActionType::kObserve;
  std::string reason;
  std::string url;                     // kNavigate
  std::string query;                   // kSearch
  std::string ref;                     // kClick, kType
  std::string text;                    // kType
  std::vector<FormFieldValue> fields;  // kFillForm
  std::string direction;               // kScroll: "down" or "up"
  std::string result;                  // kDone, kError
  std::string tool;                    // kCallTool: the tool's id ("t1")
  // kCallTool: the arguments the tool gets (JSON, written by the browser)
  // and as the user reads them (agent_tools::ToolArguments).
  std::string tool_arguments;
  std::string tool_arguments_text;
  // With a plan: the plan step it works on (1-based, PlanSteps); 0 without.
  int plan_step = 0;
};

// The JSON Schema for the next reply, given what the model was shown.
// An anyOf of one object per action in |allowed|, every key required and
// no others, "reason" first. CLICK needs an element and TYPE / FILL_FORM a
// text field; their branches are left out when the observation has none.
// CALL_TOOL is an anyOf of one object per tool of the observation (its id
// as a const, its arguments' schema), left out without tools.
// With |plan|, every object also has "step" (an enum of
// AllowedPlanSteps()) after "reason"; null means no plan (and the schema is
// exactly what it was before the planner).
// Always inside the inference service's supported subset and caps
// (json_schema_grammar.h; checked by the unit tests, and for tools by
// AddSiteTools()).
std::string BuildActionSchema(const Observation& observation,
                              ActionSet allowed = kAgentActions,
                              const PlanSteps* plan = nullptr);

enum class ActionError {
  kNone,
  kNotJsonObject,  // not one JSON object (or nested too deep)
  kUnknownAction,  // "action" missing, or not an action of the set
  kWrongKeys,      // a key missing, an extra key, or a value's type
  kTooLong,
  kEmpty,           // a value that must not be blank
  kBadCharacters,   // control characters where none are allowed
  kUnknownElement,  // an element id that isn't in the observation
  kNotTypeable,     // TYPE / FILL_FORM on something that isn't a text field
  kDuplicateField,  // FILL_FORM names the same element twice
  kTooManyFields,   // FILL_FORM with none, or more than kMaxFormFields
  kBadDirection,    // SCROLL other than "down" / "up"
  kUnknownTool,     // CALL_TOOL names a tool that isn't in the observation
  kBadArguments,    // CALL_TOOL's arguments don't match the tool's
  kBadPlanStep,     // "step" is neither the current plan step nor the next
};

// Fixed text, for the retry note the model sees ("it named an element
// that isn't in ELEMENTS").
std::string_view ActionErrorText(ActionError error);

struct ParsedAction {
  ParsedAction();
  ~ParsedAction();
  ParsedAction(ParsedAction&&);
  ParsedAction& operator=(ParsedAction&&);

  std::optional<AgentAction> action;
  ActionError error = ActionError::kNone;
};

// Parses and validates a reply (untrusted: whatever the service sent)
// against |observation|, |allowed| and |plan|, what BuildActionSchema() was
// given. With |plan|, "step" must be a whole number AllowedPlanSteps()
// lists (AgentAction::plan_step); without, the key is refused like any
// other extra key.
ParsedAction ParseAction(std::string_view json,
                         const Observation& observation,
                         ActionSet allowed = kAgentActions,
                         const PlanSteps* plan = nullptr);

// ---------------------------------------------------------------------------
// What to do with a reply
// ---------------------------------------------------------------------------

// The parts of a GenerationResult (browser_ai_runtime.h) the decision
// needs.
struct ModelReply {
  ModelReply();
  ~ModelReply();
  ModelReply(const ModelReply&);
  ModelReply& operator=(const ModelReply&);
  ModelReply(ModelReply&&);
  ModelReply& operator=(ModelReply&&);

  bool success = false;
  bool cancelled = false;
  bool constraint_unsupported = false;
  bool invalid_output = false;
  bool truncated = false;
  std::string text;
  std::string error_message;
};

enum class ReplyDecision {
  kAct,    // |action| is valid for the observation
  kRetry,  // ask again, adding |message| to the prompt
  kStop,   // end the run with |message| (user-facing)
};

struct ReplyOutcome {
  ReplyOutcome();
  ~ReplyOutcome();
  ReplyOutcome(ReplyOutcome&&);
  ReplyOutcome& operator=(ReplyOutcome&&);

  ReplyDecision decision = ReplyDecision::kStop;
  AgentAction action;
  std::string message;
};

// |bad_in_a_row| and |bad_total| count the unusable replies before this
// one (in this step, and in the run). A cut-off or malformed reply, or one
// ParseAction() refuses, is retried until kMaxAttemptsPerStep or
// kMaxBadRepliesPerRun is reached. A cancelled request, an unsupported
// constraint (a bug, or a misbehaving service: the schema is built here)
// and a failed generation stop the run.
ReplyOutcome InterpretReply(const ModelReply& reply,
                            const Observation& observation,
                            int bad_in_a_row,
                            int bad_total,
                            ActionSet allowed = kAgentActions,
                            const PlanSteps* plan = nullptr);

// ---------------------------------------------------------------------------
// Carrying an action out
// ---------------------------------------------------------------------------

// SEARCH loads this engine's results page (Brave Search: no tracking);
// only the query is the model's.
inline constexpr char kSearchURLPrefix[] = "https://search.brave.com/search?q=";

// kSearchURLPrefix + |query|, form-encoded: ASCII letters, digits and
// "-_.~" stay, a space becomes '+', every other byte is %XX.
std::string BuildSearchURL(std::string_view query);

// Scripts that carry out an action in the page, to run in an isolated
// world. Selectors, text and the origin go in as JSON strings, never
// spliced as code. The element scripts first check that the page still has
// |origin| (location.origin, as url::Origin::Serialize() writes it) and
// evaluate to 'origin_changed' if not.
//
// Click: 'clicked', 'not_found' or 'error:...'.
std::string BuildClickScript(std::string_view origin,
                             std::string_view selector);
// Type: 'typed', 'not_found', 'refused_password' (re-checked in the same
// script as the write, so a field swapped in after the probe is still
// refused) or 'error:...'.
std::string BuildTypeScript(std::string_view origin,
                            std::string_view selector,
                            std::string_view text);
// Fill: the number of fields written; password fields and missing
// elements are skipped. |fields| are (selector, text) pairs.
std::string BuildFillFormScript(
    std::string_view origin,
    const std::vector<std::pair<std::string, std::string>>& fields);
// Scroll by 600 px: |direction| "up" scrolls up, anything else down.
// Evaluates to 'scrolled'.
std::string BuildScrollScript(std::string_view direction);

}  // namespace agent_actions
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_ACTIONS_H_
