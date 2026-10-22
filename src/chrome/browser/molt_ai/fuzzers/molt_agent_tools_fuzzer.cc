// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The web agent's site tools (agents/agent_tools.h, and CALL_TOOL in
// agents/agent_actions.h) on everything outside the browser: the page's
// answer to the tool-list script (any JavaScript value, here any JSON: tool
// names, descriptions, input schemas, annotations, form fields), an input
// schema on its own, the sandboxed model's reply, and the page's answers to
// the call scripts.
//
// Checks:
//  * a tool the agent offers has Blink's kind of name, a one-line
//    description without double quotes within its cap, at most 8 arguments
//    with valid names and capped specs; never a password (by its name, an
//    argument's name or a form field) or a file field; a payment card or
//    one-time code is marked;
//  * its arguments' schema, and the agent's action schema with every tool
//    AddSiteTools() keeps, are accepted by the inference service's
//    converter; at most 8 tools, ids t1..tN, distinct names, the TOOLS text
//    within its cap and one line per tool and per argument list;
//  * a call asks the user unless the page annotated the tool read-only and
//    it is neither a <form> tool nor takes a payment card or one-time code;
//  * a CALL_TOOL that ParseAction() accepts names an offered tool, and its
//    arguments read back (ReadArguments()) to the very same JSON and text:
//    the tool's own arguments without nulls, each within its spec; the
//    call script carries them as one JSON string;
//  * a call's answer as the model sees it is capped, trimmed, and has no
//    control characters but line breaks and tabs.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/agents/agent_tools.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/services/molt_ai/inference/engine/json_schema_grammar.h"

namespace molt_ai {
namespace {

using agent_safety::FieldKind;
using agent_tools::ArgType;
using agent_tools::SiteTool;
using agent_tools::ToolArgument;

size_t CountChars(std::string_view s) {
  size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
  }
  return n;
}

bool HasControl(std::string_view s, bool allow_newlines) {
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (allow_newlines && (c == '\n' || c == '\t')) continue;
    if (c < 0x20 || c == 0x7F) return true;
  }
  return false;
}

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

bool ValidToolName(std::string_view s) {
  if (s.empty() || s.size() > agent_tools::kMaxToolNameChars) return false;
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!ok) return false;
  }
  return true;
}

bool ValidArgumentName(std::string_view s) {
  if (s.empty() || s.size() > agent_tools::kMaxArgumentNameBytes ||
      s[0] == '-' || s.starts_with("__")) {
    return false;
  }
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::string JsString(std::string_view s) {
  std::string out;
  base::JSONWriter::Write(base::Value(s), &out);
  return out;
}

// Any JavaScript value a page script can evaluate to, as JSON.
base::Value PageValue(const std::string& json) {
  std::optional<base::Value> v = base::JSONReader::Read(json);
  return v ? std::move(*v) : base::Value(json);
}

bool ServiceAccepts(const std::string& schema) {
  return JsonSchemaToGrammar(schema).status == SchemaGrammarStatus::kOk;
}

void CheckArgument(const ToolArgument& a) {
  CHECK(ValidArgumentName(a.name));
  CHECK(CountChars(a.description) <=
        agent_tools::kMaxShownArgumentDescriptionChars);
  CHECK(!HasControl(a.description, false));
  CHECK(a.description.find('"') == std::string::npos);
  CHECK(agent_safety::ClassifySelector(a.name) != FieldKind::kPassword)
      << "an argument named like a password";
  switch (a.type) {
    case ArgType::kString:
      CHECK(a.min_length <= a.max_length);
      CHECK(a.max_length <= (a.list ? agent_tools::kMaxToolItemChars
                                    : agent_tools::kMaxToolStringChars) ||
            !a.enum_values.empty());
      CHECK(a.enum_values.size() <= agent_tools::kMaxToolEnumValues);
      for (const std::string& v : a.enum_values) {
        CHECK(!v.empty() && CountChars(v) <= agent_tools::kMaxToolEnumValueChars);
        CHECK(!HasControl(v, false));
      }
      CHECK(!a.minimum && !a.maximum);
      break;
    case ArgType::kInteger:
    case ArgType::kNumber:
      CHECK(a.enum_values.empty());
      if (a.minimum && a.maximum) {
        CHECK(*a.minimum <= *a.maximum);
        CHECK(*a.maximum - *a.minimum < agent_tools::kMaxToolRangeSize);
        CHECK(a.type == ArgType::kInteger || *a.minimum >= 0);
      } else if (a.minimum) {
        CHECK(*a.minimum == 0 || (a.type == ArgType::kInteger && *a.minimum == 1));
      } else {
        CHECK(!a.maximum) << "a maximum alone isn't expressible";
      }
      break;
    case ArgType::kBoolean:
      CHECK(a.enum_values.empty() && !a.minimum && !a.maximum);
      break;
  }
  if (a.list) {
    CHECK(a.min_items <= a.max_items);
    CHECK(a.max_items <= agent_tools::kMaxToolListItems);
  }
}

void CheckTool(const SiteTool& tool, const agent_tools::ListedTool& listed) {
  CHECK(tool.name == listed.name);
  CHECK(ValidToolName(tool.name));
  CHECK(!tool.description.empty());
  CHECK(CountChars(tool.description) <=
        agent_tools::kMaxShownDescriptionChars);
  CHECK(!HasControl(tool.description, false));
  CHECK(tool.description.find('"') == std::string::npos);
  CHECK(tool.arguments.size() <= agent_tools::kMaxToolArguments);
  std::set<std::string> names;
  for (const ToolArgument& a : tool.arguments) {
    CheckArgument(a);
    CHECK(names.insert(a.name).second) << "an argument twice";
  }
  CHECK(agent_safety::ClassifySelector(tool.name) != FieldKind::kPassword);
  CHECK(tool.sensitive != FieldKind::kPassword);
  CHECK(tool.form_backed == listed.form_backed);
  if (tool.form_backed) {
    CHECK(!listed.form_overflow);
    CHECK(tool.listed_form_fields.size() <= agent_tools::kMaxFormControls);
    for (const auto& [type, autocomplete] : tool.listed_form_fields) {
      CHECK(type != "file") << "a form tool with a file field";
      CHECK(agent_safety::ClassifyField(type, autocomplete) !=
            FieldKind::kPassword)
          << "a form tool with a password field";
    }
  }
  CHECK(tool.listed_description == listed.description);
  CHECK(tool.listed_input_schema == listed.input_schema);

  const agent_tools::CallCheck check =
      agent_tools::CheckToolCall(tool, "", "https://shop.example");
  const bool reads_only = tool.read_only && !tool.form_backed &&
                          tool.sensitive == FieldKind::kOrdinary;
  CHECK(check.ask == !reads_only) << "a call that changes things ran unasked";
  CHECK(check.message.find("\"" + tool.name + "\"") != std::string::npos);

  SiteTool alone = tool;
  alone.ref = "t1";
  CHECK(ServiceAccepts(agent_tools::BuildArgumentsSchema(alone)))
      << agent_tools::BuildArgumentsSchema(alone);
}

void CheckOfferedTools(const agent_actions::Observation& o) {
  CHECK(o.tools.size() <= agent_tools::kMaxSiteTools);
  std::set<std::string> names;
  for (size_t i = 0; i < o.tools.size(); ++i) {
    CHECK(o.tools[i].ref == "t" + std::to_string(i + 1));
    CHECK(o.FindTool(o.tools[i].ref) == &o.tools[i]);
    CHECK(names.insert(o.tools[i].name).second) << "a tool name twice";
  }
  const std::string text = agent_tools::DescribeTools(o.tools);
  CHECK(CountChars(text) <= agent_tools::kMaxToolsTextChars);
  CHECK(!HasControl(text, true) && text.find('\t') == std::string::npos);
  // Two lines per tool: "tN name ..." and "   arguments: ...".
  size_t lines = 0;
  for (size_t at = 0; at < text.size();) {
    const size_t end = text.find('\n', at);
    CHECK(end != std::string::npos) << "TOOLS text ends without a line break";
    const std::string_view line(text.data() + at, end - at);
    if (lines % 2 == 0) {
      const std::string& ref = o.tools[lines / 2].ref;
      CHECK(line.starts_with(ref + " " + o.tools[lines / 2].name + " ["));
    } else {
      CHECK(line.starts_with("   arguments: "));
    }
    ++lines;
    at = end + 1;
  }
  CHECK(lines == 2 * o.tools.size());
  CHECK(ServiceAccepts(agent_actions::BuildActionSchema(o)))
      << "the service would refuse the action schema with these tools";
}

void CheckCall(const agent_actions::AgentAction& action,
               const agent_actions::Observation& o) {
  const SiteTool* tool = o.FindTool(action.tool);
  CHECK(tool) << "a call to a tool that wasn't offered";
  std::optional<base::Value> args = base::JSONReader::Read(action.tool_arguments);
  CHECK(args && args->is_dict());
  std::set<std::string> given;
  for (const auto& [key, value] : args->GetDict()) {
    CHECK(!value.is_none()) << "a null sent to the tool";
    given.insert(key);
  }
  for (const ToolArgument& a : tool->arguments) {
    CHECK(!a.required || given.count(a.name));
    given.erase(a.name);
  }
  CHECK(given.empty()) << "an argument the tool doesn't have";
  // What the tool gets reads back to itself (with the nulls it leaves out
  // put back).
  for (const ToolArgument& a : tool->arguments) {
    if (!args->GetDict().Find(a.name))
      args->GetDict().Set(a.name, base::Value());
  }
  agent_tools::ToolArguments again;
  CHECK(agent_tools::ReadArguments(*args, *tool, &again) ==
        agent_tools::ArgumentsError::kNone);
  CHECK(again.json == action.tool_arguments);
  CHECK(again.text == action.tool_arguments_text);
  CHECK(!HasControl(action.tool_arguments_text, false));

  const std::string script = agent_tools::BuildToolCallScript(
      "c1", "https://shop.example", *tool, action.tool_arguments);
  CHECK(script.find("args = " + JsString(action.tool_arguments) + ";") !=
        std::string::npos)
      << "the arguments must go in as one JSON string";
}

void CheckOutcome(const agent_tools::CallOutcome& outcome) {
  if (outcome.state != agent_tools::CallState::kDone) {
    CHECK(!outcome.result);
    CHECK(!agent_tools::CallFailureNote(outcome.state).empty() ||
          outcome.state == agent_tools::CallState::kPending ||
          outcome.state == agent_tools::CallState::kStarted);
    return;
  }
  if (!outcome.result) return;
  const std::string& r = *outcome.result;
  CHECK(CountChars(r) <= agent_tools::kMaxToolResultChars);
  CHECK(!HasControl(r, true)) << "control characters in a tool's answer";
  CHECK(r.find('\r') == std::string::npos);
  if (!r.empty()) {
    CHECK(!IsSpace(r.front()) && !IsSpace(r.back()));
  }
}

agent_actions::Observation SampleObservation() {
  static constexpr char kSnapshot[] =
      R"({"origin":"https://shop.example","elements":[)"
      R"({"sel":"#q","kind":"input","type":"search","ac":"","label":"Search"},)"
      R"({"sel":"#go","kind":"button","type":"","ac":"","label":"Go"}]})";
  std::string origin;
  return agent_actions::ParseElementSnapshot(PageValue(kSnapshot), &origin);
}

}  // namespace
}  // namespace molt_ai

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai;
  FuzzedDataProvider p(data, size);

  // The page's answer to the list scripts.
  const agent_tools::ToolList list =
      agent_tools::ParseToolList(PageValue(p.ConsumeRandomLengthString(32768)));
  CHECK(list.tools.size() <= agent_tools::kMaxListedTools);
  if (list.state != agent_tools::ListState::kDone) {
    CHECK(list.tools.empty() && list.origin.empty());
  }
  std::vector<SiteTool> tools;
  for (const agent_tools::ListedTool& listed : list.tools) {
    std::optional<SiteTool> tool = agent_tools::MakeSiteTool(listed);
    if (!tool) continue;
    CheckTool(*tool, listed);
    tools.push_back(std::move(*tool));
  }

  // An input schema on its own.
  agent_tools::ListedTool direct;
  direct.name = "direct_tool";
  direct.description = "Direct.";
  direct.read_only = p.ConsumeBool();
  direct.input_schema = p.ConsumeRandomLengthString(8192);
  if (std::optional<std::vector<ToolArgument>> args =
          agent_tools::ConvertInputSchema(*direct.input_schema)) {
    std::optional<SiteTool> tool = agent_tools::MakeSiteTool(direct);
    CHECK(tool || direct.input_schema->size() > agent_tools::kMaxToolSchemaBytes ||
          [&] {
            for (const ToolArgument& a : *args) {
              if (agent_safety::ClassifySelector(a.name) == FieldKind::kPassword)
                return true;
            }
            return false;
          }());
    if (tool) {
      CheckTool(*tool, direct);
      tools.insert(tools.begin(), std::move(*tool));
    }
  }

  // What the model is offered.
  agent_actions::Observation observation =
      p.ConsumeBool() ? SampleObservation() : agent_actions::Observation();
  agent_actions::AddSiteTools(tools, &observation);
  CheckOfferedTools(observation);

  // The model's reply.
  const std::string reply = p.ConsumeRandomLengthString(4096);
  const agent_actions::ParsedAction parsed =
      agent_actions::ParseAction(reply, observation);
  if (parsed.action && parsed.action->type == agent_actions::ActionType::kCallTool)
    CheckCall(*parsed.action, observation);
  // The chat never takes one.
  const agent_actions::ParsedAction chat = agent_actions::ParseAction(
      reply, observation,
      agent_actions::kAgentActions &
          ~agent_actions::ActionBit(agent_actions::ActionType::kCallTool));
  CHECK(!chat.action || chat.action->type != agent_actions::ActionType::kCallTool);

  // The page's answers to the call scripts.
  const agent_tools::CallOutcome start =
      agent_tools::ParseToolCallStart(PageValue(p.ConsumeRandomLengthString(64)));
  CHECK(!start.result);
  const agent_tools::CallOutcome poll =
      agent_tools::ParseToolCallPoll(PageValue(p.ConsumeRandomLengthString(16384)));
  CheckOutcome(poll);
  if (!observation.tools.empty()) {
    const std::string shown =
        agent_tools::DescribeToolResult(observation.tools.front(), poll);
    CHECK(!HasControl(shown, true));
    CHECK(shown.starts_with("TOOL RESULT (the page's tool "));
  }
  return 0;
}
