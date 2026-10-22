// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Site tools for the web agent: the tools a page registers with WebMCP
// (document.modelContext, Chromium 152; docs/design/AGENT_ACTIONS.md §2.7).
//
// The agent reaches them through the web API itself, from its isolated
// world: BuildToolListScript() lists the page's own tools
// (ModelContext.getTools()), BuildToolCallScript() calls one
// (ModelContext.executeTool()). Blink and the browser apply the platform's
// rules (secure context, the `tools` permissions policy, origin checks); the
// page can't see or patch the isolated world's calls. Isolated-world
// scripts don't wait for promises in 152, so each script starts the work,
// keeps its state in the isolated world, and a poll script reports it.
//
// Everything a page lists is untrusted:
//  * a tool is offered only if its name is valid, its description isn't
//    blank, and its input schema converts to the agent's subset
//    (ConvertInputSchema(): flat objects of strings, whole numbers,
//    numbers, booleans and lists of them, with the bounds the inference
//    service's grammar can express). Anything else skips the tool, never
//    approximates it;
//  * a tool whose name, an argument's name or the fields of its <form>
//    look like a password (or that has a file field) is never offered;
//    payment-card and one-time-code ones always ask the user;
//  * names, descriptions and results reach the model only inside the
//    untrusted page fence, on single lines, capped;
//  * the model picks a tool by id (t1, t2, ...) and writes arguments
//    under a grammar built here; ReadArguments() checks them again and
//    writes the JSON the tool gets, with base::JSONWriter;
//  * a call runs only on the page the run is on (the script checks the
//    origin), only if the tool is still the one listed (same description,
//    schema, read-only hint and form fields), and a tool that isn't
//    annotated read-only asks the user first (CheckToolCall()).
//
// Pure logic (base::Value, no content/), unit-tested in molt_ai_unittests
// and fuzzed (molt_agent_tools_fuzzer).

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_TOOLS_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_TOOLS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/security/agent_safety.h"

namespace base {
class Value;
}  // namespace base

namespace molt_ai {
namespace agent_tools {

// ---------------------------------------------------------------------------
// Caps. Lengths count characters (code points), as the grammar does.
// ---------------------------------------------------------------------------

// Tools offered to the model per observation (t1 ... t8).
inline constexpr size_t kMaxSiteTools = 8;
// Entries read from the page's list; the rest are ignored.
inline constexpr size_t kMaxListedTools = 32;
// Blink's own limit on a tool name: [A-Za-z0-9_.-]{1,128}.
inline constexpr size_t kMaxToolNameChars = 128;
// A longer description or input schema skips the tool.
inline constexpr size_t kMaxToolDescriptionChars = 4096;
inline constexpr size_t kMaxToolSchemaBytes = 8192;
// What the prompt shows of a description.
inline constexpr size_t kMaxShownDescriptionChars = 200;
inline constexpr size_t kMaxShownArgumentDescriptionChars = 80;
// One tool's lines in the prompt, and all of TOOLS.
inline constexpr size_t kMaxToolTextChars = 700;
inline constexpr size_t kMaxToolsTextChars = 4000;
// Arguments of one tool, and their names: 1-64 of [A-Za-z0-9_-], not
// starting with "-" or "__".
inline constexpr size_t kMaxToolArguments = 8;
inline constexpr size_t kMaxArgumentNameBytes = 64;
// A string argument, and a string in a list argument.
inline constexpr size_t kMaxToolStringChars = 500;
inline constexpr size_t kMaxToolItemChars = 100;
inline constexpr size_t kMaxToolListItems = 16;
inline constexpr size_t kMaxToolEnumValues = 16;
inline constexpr size_t kMaxToolEnumValueChars = 64;
// Whole-number bounds: within +-10^15, and a range of at most this many
// values (as json_schema_grammar.h's kMaxRangeSize).
inline constexpr int64_t kMaxToolBound = 1000000000000000;
inline constexpr int64_t kMaxToolRangeSize = 1001;
// Controls of a <form toolname> read; a form with more isn't offered.
inline constexpr size_t kMaxFormControls = 64;
// The page's answer, as the model sees it.
inline constexpr size_t kMaxToolResultChars = 1500;
// How long the agent waits for the list, and for a call.
inline constexpr int kListTimeoutMs = 3000;
inline constexpr int kCallTimeoutMs = 20000;
inline constexpr int kPollIntervalMs = 150;

// ---------------------------------------------------------------------------
// A tool and its arguments
// ---------------------------------------------------------------------------

enum class ArgType { kString, kInteger, kNumber, kBoolean };

struct ToolArgument {
  ToolArgument();
  ~ToolArgument();
  ToolArgument(const ToolArgument&);
  ToolArgument& operator=(const ToolArgument&);
  ToolArgument(ToolArgument&&);
  ToolArgument& operator=(ToolArgument&&);

  // [A-Za-z0-9_-]{1,64} (page text, checked).
  std::string name;
  // Page text on one line, for the prompt only.
  std::string description;
  ArgType type = ArgType::kString;
  // A list of |type| values (never of lists).
  bool list = false;
  // The page requires it; the model may write null for the others.
  bool required = false;
  // Strings (each string of a list): characters.
  size_t min_length = 0;
  size_t max_length = kMaxToolStringChars;
  // Strings: the allowed values, or empty for any.
  std::vector<std::string> enum_values;
  // Whole numbers and numbers: inclusive whole-number bounds.
  std::optional<int64_t> minimum;
  std::optional<int64_t> maximum;
  // Lists.
  size_t min_items = 0;
  size_t max_items = kMaxToolListItems;
};

struct SiteTool {
  SiteTool();
  ~SiteTool();
  SiteTool(const SiteTool&);
  SiteTool& operator=(const SiteTool&);
  SiteTool(SiteTool&&);
  SiteTool& operator=(SiteTool&&);

  // "t1", "t2", ...: assigned by agent_actions::AddSiteTools().
  std::string ref;
  // [A-Za-z0-9_.-]{1,128} (page text, checked).
  std::string name;
  // The description on one line, capped, for the prompt.
  std::string description;
  std::vector<ToolArgument> arguments;
  // annotations.readOnlyHint === true: the page says it changes nothing.
  bool read_only = false;
  // annotations.untrustedContentHint === true: its answers may hold
  // other people's content.
  bool untrusted_output = false;
  // A <form toolname> tool: a call fills (and may submit) that form.
  bool form_backed = false;
  // It takes a payment card or a one-time code (by its name, an
  // argument's name or its form's fields): every call asks.
  agent_safety::FieldKind sensitive = agent_safety::FieldKind::kOrdinary;

  // The tool exactly as the page listed it. A call runs only if the page
  // still lists the same tool (BuildToolCallScript()).
  std::string listed_description;
  std::optional<std::string> listed_input_schema;
  // (type, autocomplete) of each control of its form; only if form_backed.
  std::vector<std::pair<std::string, std::string>> listed_form_fields;
};

// ---------------------------------------------------------------------------
// Listing the page's tools
// ---------------------------------------------------------------------------

// Whether to list tools at this observation: the switch
// (#molt-agent-site-tools) is on, the page is in the run's scope, and the
// element snapshot ran in the page the tab is on.
struct ListFacts {
  bool switched_on = false;
  bool origin_allowed = false;
  bool snapshot_is_current = false;
};
bool ShouldListTools(const ListFacts& facts);

// Starts ModelContext.getTools() in the agent's isolated world, unless
// the page has no document.modelContext (no WebMCP, or not a secure
// context). Evaluates to {state: 'pending' | 'unavailable' | 'failed'}.
// |request_id| tells this request's state apart from an older one's.
std::string BuildToolListScript(std::string_view request_id);
// Reports it: {state: 'pending' | 'failed' | 'gone'} or {state: 'done',
// origin, tools: [{name, description, schema, readOnly, untrusted,
// form}]}, the tools the document itself registered (same window and
// origin), at most kMaxListedTools.
std::string BuildToolListPollScript(std::string_view request_id);

enum class ListState { kPending, kDone, kUnavailable, kFailed, kGone };

// One entry of the list, as the page gave it (sizes bounded, not checked).
struct ListedTool {
  ListedTool();
  ~ListedTool();
  ListedTool(const ListedTool&);
  ListedTool& operator=(const ListedTool&);
  ListedTool(ListedTool&&);
  ListedTool& operator=(ListedTool&&);

  std::string name;
  std::string description;
  std::optional<std::string> input_schema;
  bool read_only = false;
  bool untrusted_output = false;
  bool form_backed = false;
  // More than kMaxFormControls controls.
  bool form_overflow = false;
  std::vector<std::pair<std::string, std::string>> form_fields;
};

struct ToolList {
  ToolList();
  ~ToolList();
  ToolList(ToolList&&);
  ToolList& operator=(ToolList&&);

  ListState state = ListState::kFailed;
  // location.origin of the document listed; set when kDone.
  std::string origin;
  std::vector<ListedTool> tools;
};

// Parses either script's result. Malformed entries are skipped; a
// malformed result is kFailed.
ToolList ParseToolList(const base::Value& result);

// The tool as the agent may offer it, or nullopt (skipped): see the top
// of this file.
std::optional<SiteTool> MakeSiteTool(const ListedTool& listed);

// The page's inputSchema (JSON text) as an argument list, in name order
// (base::Value sorts keys), or nullopt if it is outside the subset. {}
// and {"type": "object"} have no arguments (nor has a tool registered
// without a schema: MakeSiteTool() doesn't call this then).
//   root:      {"type": "object", "properties": {...}, "required": [...],
//              "additionalProperties": bool}
//   argument:  "type": "string" (minLength, maxLength, enum of strings),
//              "integer" / "number" (minimum, maximum; exclusive bounds
//              for integers), "boolean", or "array" with "items" of those
//              (minItems, maxItems); "enum" of strings without "type".
//   ignored:   title, description (kept for the prompt), default,
//              examples, $schema, $id, $comment, deprecated, readOnly,
//              writeOnly, format (an annotation in JSON Schema 2020-12),
//              and $defs / definitions at the root.
// Refused: everything else ($ref, pattern, nested objects, lists of
// lists, type lists, const, oneOf, ...), and bounds the grammar can't
// express: whole numbers bounded other than by a range of at most
// kMaxToolRangeSize values or a minimum of 0 or 1 alone; numbers other
// than a whole, non-negative range of that size or a minimum of 0 alone.
// String caps are lowered to kMaxToolStringChars (kMaxToolItemChars in a
// list) and list caps to kMaxToolListItems; a minimum above them refuses.
std::optional<std::vector<ToolArgument>> ConvertInputSchema(
    std::string_view json);

// The JSON Schema of a CALL_TOOL's "arguments" for |tool|: an object with
// every argument required and no others; an argument the page doesn't
// require may be null. Inside the inference service's subset.
std::string BuildArgumentsSchema(const SiteTool& tool);

// The TOOLS lines of the prompt (page text: the caller fences it), or ""
// without tools. One line per tool, its arguments on the next.
std::string DescribeTools(const std::vector<SiteTool>& tools);
// One tool's lines, at most kMaxToolTextChars characters.
std::string DescribeTool(const SiteTool& tool);

// ---------------------------------------------------------------------------
// Calling a tool
// ---------------------------------------------------------------------------

enum class ArgumentsError {
  kNone,
  kWrongKeys,      // not an object, a key missing or extra
  kBadValue,       // a type, an enum value, a bound, a required null
  kTooLong,        // a string or a list over its cap
  kBadCharacters,  // control characters other than \n and \t
};

struct ToolArguments {
  // The object the tool gets (executeTool's inputArguments): the non-null
  // arguments, written by base::JSONWriter.
  std::string json;
  // `name = value; ...` for the step trace and the user, with hidden and
  // reordering characters shown.
  std::string text;
};

// Checks the model's "arguments" (untrusted) against |tool|.
ArgumentsError ReadArguments(const base::Value& arguments,
                             const SiteTool& tool,
                             ToolArguments* out);

// Whether a call asks the user first: unless the page annotated the tool
// read-only, it isn't a <form> tool and it takes no payment card or
// one-time code. |message| says what the user approves.
struct CallCheck {
  bool ask = false;
  std::string message;
};
CallCheck CheckToolCall(const SiteTool& tool,
                        std::string_view arguments_text,
                        std::string_view origin_text);

// Starts the call in the agent's isolated world: on the page at |origin|
// only, after getTools() shows the same tool as |tool| was listed. The
// arguments go in as one JSON string. Evaluates to 'started',
// 'origin_changed', 'unavailable' or 'error'.
std::string BuildToolCallScript(std::string_view request_id,
                                std::string_view origin,
                                const SiteTool& tool,
                                std::string_view arguments_json);
// Reports it: {state: 'pending' | 'done' | 'failed' | 'not_found' |
// 'changed' | 'origin_changed' | 'gone', result: string or null}.
std::string BuildToolCallPollScript(std::string_view request_id);

enum class CallState {
  kStarted,
  kPending,
  kDone,
  kFailed,
  kNotFound,       // the page no longer lists the tool
  kChanged,        // it lists a different tool under that name
  kOriginChanged,  // the page moved to another origin
  kUnavailable,    // no document.modelContext any more
  kGone,           // no state for this call: another document
  kTimedOut,       // set by the caller
};

struct CallOutcome {
  CallOutcome();
  ~CallOutcome();
  CallOutcome(const CallOutcome&);
  CallOutcome& operator=(const CallOutcome&);
  CallOutcome(CallOutcome&&);
  CallOutcome& operator=(CallOutcome&&);

  CallState state = CallState::kFailed;
  // kDone: the page's answer, cleaned and capped (nullopt: none).
  std::optional<std::string> result;
  bool result_cut = false;
};

CallOutcome ParseToolCallStart(const base::Value& result);
CallOutcome ParseToolCallPoll(const base::Value& result);

// The step's note for an outcome other than kDone (fixed text).
std::string_view CallFailureNote(CallState state);

// What the next observation shows of a finished call (page text: the
// caller fences it with the page).
std::string DescribeToolResult(const SiteTool& tool,
                               const CallOutcome& outcome);

}  // namespace agent_tools
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_TOOLS_H_
