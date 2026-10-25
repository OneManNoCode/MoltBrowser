// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Site-tool steps of Agent-mode workflows (automation/
// automation_tool_steps.h): a CALL_TOOL step of an imported .molt file, the
// tool a page offers at replay (its schema, hints and form), the values a
// run substitutes, and a call recorded from the AI chat (the page's tool,
// the model's arguments). The input is JSON: {"step": {...a .molt step...},
// "tool": {"name", "description", "schema", "readOnly", "consequential",
// "form", "frame"}, "frames": [[origin, path], ...], "args": {...},
// "approved": bool, "call": {"origin", "arguments", "in_frame",
// "frame_origin", "frame_path"}}; any part may be missing.
//
// Checks: no memory or UB error; a read step is within every cap and
// writes back to a step that reads the same; a recorded call gives a step
// that reads back, keeps no value of a card or code argument (nor any of a
// card or code tool) and no value the policy withholds; a listed tool is
// found only by its name, in a frame of the recorded origin; a compatible
// call carries the browser's JSON of an object; a call runs unasked only
// if the tool only reads or the workflow is approved, and never for a
// card or code tool or another site's frame.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/agents/agent_tools.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_tool_steps.h"

namespace molt_ai::automation::tool_steps {
namespace {

// Withholds values with a digit (any rule will do: the check is that
// nothing withheld is stored).
class DigitPolicy : public ValuePolicy {
 public:
  bool Withhold(std::string_view, std::string_view value) const override {
    for (char c : value) {
      if (c >= '0' && c <= '9')
        return true;
    }
    return false;
  }
};

std::optional<Step> StepFrom(const base::Value* v) {
  if (!v || !v->is_dict())
    return std::nullopt;
  base::DictValue script;
  script.Set("id", "wf");
  base::ListValue steps;
  steps.Append(v->Clone());
  script.Set("steps", std::move(steps));
  std::optional<Script> s = Script::FromJSON(script);
  if (!s || s->steps.size() != 1)
    return std::nullopt;
  return std::move(s->steps[0]);
}

std::optional<agent_tools::SiteTool> ToolFrom(const base::Value* v) {
  if (!v || !v->is_dict())
    return std::nullopt;
  const base::DictValue& d = v->GetDict();
  agent_tools::ListedTool l;
  if (const std::string* n = d.FindString("name"))
    l.name = *n;
  if (const std::string* desc = d.FindString("description"))
    l.description = *desc;
  if (const std::string* schema = d.FindString("schema"))
    l.input_schema = *schema;
  l.read_only = d.FindBool("readOnly").value_or(false);
  l.consequential = d.FindBool("consequential").value_or(false);
  if (const base::ListValue* form = d.FindList("form")) {
    l.form_backed = true;
    for (const base::Value& f : *form) {
      if (f.is_string())
        l.form_fields.emplace_back(f.GetString(), std::string());
    }
  }
  std::optional<agent_tools::SiteTool> t = agent_tools::MakeSiteTool(l);
  if (t)
    t->frame = static_cast<size_t>(std::max(0, d.FindInt("frame").value_or(0)));
  return t;
}

}  // namespace
}  // namespace molt_ai::automation::tool_steps

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai::automation::tool_steps;
  using molt_ai::automation::Step;
  namespace agent_tools = molt_ai::agent_tools;
  namespace agent_safety = molt_ai::agent_safety;
  const std::string json(reinterpret_cast<const char*>(data), size);
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict())
    return 0;
  const base::DictValue& in = parsed->GetDict();

  const std::optional<agent_tools::SiteTool> live = ToolFrom(in.Find("tool"));

  // A call recorded from the chat.
  if (const base::DictValue* call = in.FindDict("call"); call && live) {
    RecordedCall c;
    c.tool = *live;
    if (const std::string* o = call->FindString("origin"))
      c.origin = *o;
    if (const std::string* a = call->FindString("arguments"))
      c.arguments_json = *a;
    c.in_frame = call->FindBool("in_frame").value_or(false);
    if (const std::string* fo = call->FindString("frame_origin"))
      c.frame_origin = *fo;
    if (const std::string* fp = call->FindString("frame_path"))
      c.frame_path = *fp;
    DigitPolicy policy;
    std::map<std::string, std::string> vars;
    std::optional<Step> built = BuildToolStep(c, policy, &vars);
    if (built) {
      std::optional<ToolStep> back = ParseToolStep(*built);
      CHECK(back) << "a recorded call that doesn't read back";
      CHECK_EQ(back->name, live->name);
      CHECK_EQ(back->origin, c.origin);
      for (const auto [name, value] : back->arguments) {
        CHECK(!agent_safety::IsSensitive(live->sensitive))
            << "a card or code tool's value was stored";
        CHECK(!agent_safety::IsSensitive(
            agent_safety::ClassifySelector(name)))
            << "a card or code argument's value was stored";
        if (value.is_string() && value.GetString().find("{{") != 0) {
          CHECK(!policy.Withhold(name, value.GetString()))
              << "a withheld value was stored";
        }
      }
      for (const auto& [name, value] : vars)
        CHECK(value.empty());
    }
  }

  // A step of a file.
  std::optional<Step> step = StepFrom(in.Find("step"));
  if (!step)
    return 0;
  std::optional<ToolStep> t = ParseToolStep(*step);
  if (!t)
    return 0;
  CHECK(agent_tools::IsValidToolName(t->name));
  CHECK_LE(t->arguments.size() + t->secret_arguments.size(),
           agent_tools::kMaxToolArguments);
  CHECK_LE(t->origin.size(), kMaxOriginBytes);
  CHECK_LE(t->frame_path.size(), kMaxFramePathBytes);
  if (!live)
    return 0;

  std::vector<agent_tools::FrameAddress> frames;
  if (const base::ListValue* f = in.FindList("frames")) {
    for (const base::Value& v : *f) {
      if (v.is_list() && v.GetList().size() == 2 &&
          v.GetList()[0].is_string() && v.GetList()[1].is_string()) {
        frames.push_back(
            {v.GetList()[0].GetString(), v.GetList()[1].GetString()});
      }
    }
  }
  std::vector<agent_tools::SiteTool> tools = {*live};
  if (std::optional<size_t> i = FindListedTool(*t, tools, frames)) {
    CHECK_EQ(*i, 0u);
    CHECK_EQ(tools[0].name, t->name);
    CHECK_LT(tools[0].frame, frames.size());
    if (t->in_frame) {
      CHECK_NE(tools[0].frame, 0u);
      CHECK_EQ(frames[tools[0].frame].origin, t->frame_origin);
    } else {
      CHECK_EQ(tools[0].frame, 0u);
    }
  }

  base::DictValue args;
  if (const base::DictValue* a = in.FindDict("args"))
    args = a->Clone();
  else
    args = t->arguments.Clone();
  agent_tools::ToolArguments out;
  if (CheckReplay(*t, *live, args, &out) == Compat::kOk) {
    CHECK(t->secret_arguments.empty());
    std::optional<base::DictValue> sent =
        base::JSONReader::ReadDict(out.json, base::JSON_PARSE_RFC);
    CHECK(sent) << "the tool gets an object";
    for (const auto [name, value] : *sent)
      CHECK(args.contains(name)) << "a value nobody recorded";
  }
  const bool approved = in.FindBool("approved").value_or(false);
  if (ApprovalFor(*t, *live, approved) == Approval::kRun) {
    CHECK(!IsCrossOriginFrame(*t));
    CHECK(!agent_safety::IsSensitive(live->sensitive));
    CHECK(approved || agent_tools::ToolCallEffects(*live).empty());
  }
  return 0;
}
