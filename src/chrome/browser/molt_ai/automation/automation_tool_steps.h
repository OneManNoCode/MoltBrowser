// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Site-tool steps in Agent-mode workflows (docs/design/AGENT_ACTIONS.md
// §3.5): a WebMCP tool call (agents/agent_tools.h) the user approved while
// recording, kept as a CALL_TOOL step and called again at replay.
//
// Recording. The AI chat runs a site's tool only after the user's click on
// its proposal (§4.9); when that tab is being recorded, the call becomes a
// step: the tool's name, the page's origin, the frame it was in (its origin
// and path, never its number), and the arguments the tool got. An argument
// named like a payment card or one-time code is never stored (the step then
// can't replay, as a card or code TYPE step can't); a value that looks
// secret (agent_draft's heuristics) is stored as a {{variable}} asked at
// each run. Nothing the tool answered is kept.
//
// Replay (AutomationRunner). The tool is listed again on the recorded
// origin, in the recorded frame (by origin and path), and called only if:
//   * the page still offers it with a compatible schema: the recorded
//     arguments are each still an argument of the same kind, every argument
//     the tool now requires was recorded, the values pass the tool's
//     current bounds (agent_tools::ReadArguments()), and a tool that wasn't
//     a <form> tool hasn't become one;
//   * the workflow's policy allows it (automation_security.h): its page is
//     one of the workflow's sites, and a tool that may change something (not
//     read-only, consequential, a <form> tool, one taking a card or code)
//     asks the user unless the workflow is Approved (or above) for that
//     site; a card or code tool and a tool of another site's frame always
//     ask; a read-only one runs without asking.
// A call runs once: a failed or timed-out call is never retried.
//
// Everything in a step is untrusted when read: a .molt file may be
// imported. ParseToolStep() checks every field and caps every size.
//
// Pure logic (base::Value, GURL; no content/), unit-tested in
// molt_ai_unittests and fuzzed (molt_automation_tool_steps_fuzzer).

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_TOOL_STEPS_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_TOOL_STEPS_H_

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"
#include "chrome/browser/molt_ai/agents/agent_tools.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"

namespace molt_ai {
namespace automation {
namespace tool_steps {

// Step::extra keys of a CALL_TOOL step.
inline constexpr char kToolKey[] = "tool";
// Step::extra["review"] when a value was withheld (as agent_run_draft.h's).
inline constexpr char kReviewWithheld[] = "value_withheld";
// ... when a card or one-time-code argument wasn't stored.
inline constexpr char kReviewSecretArgument[] = "secret_argument";

// Caps (bytes for URLs and paths; characters as agent_tools counts them).
inline constexpr size_t kMaxOriginBytes = 256;
inline constexpr size_t kMaxFramePathBytes = 512;

// A tool call the user approved, as the AI chat ran it.
struct RecordedCall {
  RecordedCall();
  ~RecordedCall();
  RecordedCall(const RecordedCall&);
  RecordedCall& operator=(const RecordedCall&);

  // url::Origin::Serialize() of the page's main document (http(s)).
  std::string origin;
  // The tool as listed and called (agent_tools::SiteTool).
  agent_tools::SiteTool tool;
  // The tool's frame, unless it is the main document's: its origin and the
  // path of its http(s) URL ("" for a frame without an address of its own).
  bool in_frame = false;
  std::string frame_origin;
  std::string frame_path;
  // The arguments the tool got (agent_tools::ReadArguments()' JSON).
  std::string arguments_json;
};

// Which recorded values are withheld (stored as a {{variable}} asked at
// each run). The recorder's uses agent_run_draft's secret heuristics.
class ValuePolicy {
 public:
  virtual ~ValuePolicy() = default;
  virtual bool Withhold(std::string_view argument_name,
                        std::string_view value) const = 0;
};

// The step for |call|, or nullopt if it can't be one (not an http(s)
// origin, a tool name Blink wouldn't take, arguments that aren't the
// tool's). Withheld values become {{variables}} added to |variables| (a
// name not already in it, an empty default).
std::optional<Step> BuildToolStep(
    const RecordedCall& call,
    const ValuePolicy& values,
    std::map<std::string, std::string>* variables);

// A CALL_TOOL step, read back and checked.
struct ToolStep {
  ToolStep();
  ~ToolStep();
  ToolStep(ToolStep&&);
  ToolStep& operator=(ToolStep&&);

  std::string name;
  std::string origin;
  std::string host;
  bool in_frame = false;
  std::string frame_origin;
  std::string frame_path;
  // Argument name -> value as recorded (strings may be "{{variable}}").
  base::DictValue arguments;
  // Card or one-time-code arguments that weren't stored.
  std::vector<std::string> secret_arguments;
  // As recorded (shown to the user; replay goes by the live tool).
  bool recorded_read_only = false;
  bool recorded_form = false;
};

// nullopt if |step| isn't a CALL_TOOL step or any field fails a check.
std::optional<ToolStep> ParseToolStep(const Step& step);

// Whether the frame |frame_origin| is another site's frame than the page's
// (another origin): such a tool always asks.
bool IsCrossOriginFrame(const ToolStep& step);

// Which listed tool the step calls: |tools| as SiteToolHost listed them,
// |frames| each listed document's (origin, path), [0] the main document. The
// main document's tool of that name; else, in a frame of the recorded
// origin, the one at the recorded path or, if there is only one frame of
// that origin with such a tool, that one. nullopt: not offered, or more
// than one could be meant.
using ListedFrame = agent_tools::FrameAddress;
std::optional<size_t> FindListedTool(
    const ToolStep& step,
    const std::vector<agent_tools::SiteTool>& tools,
    const std::vector<ListedFrame>& frames);

enum class Compat {
  kOk,
  kSecretArgument,     // a card or code argument wasn't stored
  kChangedKind,        // a <form> tool now, or one the page lists unusably
  kMissingArgument,    // a recorded argument is gone, or of another kind
  kNewRequirement,     // the tool requires an argument that wasn't recorded
  kBadValue,           // a value fails the tool's current bounds
};

// |arguments| is the step's, with {{variables}} substituted (the runner's
// Resolve() on each string). On kOk, |json| / |text| are what the tool
// gets and how the user reads it (agent_tools::ToolArguments).
Compat CheckReplay(const ToolStep& step,
                   const agent_tools::SiteTool& live,
                   const base::DictValue& arguments,
                   agent_tools::ToolArguments* out);

// Fixed text for the run's log.
std::string_view CompatText(Compat compat);

enum class Approval { kRun, kAsk };

// Whether the call runs unasked: a tool that only reads (agent_tools'
// rule: read-only, not consequential, not a form, no card or code) of the
// page itself; or, for any other but a card or code tool, a workflow that
// is Approved or above (|approved_for_site|: the runner's check of its
// trust and that the page is one of its sites). A tool of another site's
// frame always asks.
Approval ApprovalFor(const ToolStep& step,
                     const agent_tools::SiteTool& live,
                     bool approved_for_site);

// Whether trusting the workflow would let the call run unasked (the
// studio's "Trust & re-run" when a question goes unanswered).
bool TrustWouldAllow(const ToolStep& step, const agent_tools::SiteTool& live);

}  // namespace tool_steps
}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_TOOL_STEPS_H_
