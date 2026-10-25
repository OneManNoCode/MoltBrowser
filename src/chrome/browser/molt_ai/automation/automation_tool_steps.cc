// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_tool_steps.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai {
namespace automation {
namespace tool_steps {

namespace {

using agent_tools::ArgType;
using agent_tools::SiteTool;
using agent_tools::ToolArgument;

// The arguments' JSON as the tool got it: an object, nested at most this
// deep (object, list).
constexpr int kMaxArgumentsDepth = 4;
constexpr size_t kMaxHostBytes = 253;
constexpr size_t kMaxVariableNameChars = 24;

bool IsContinuation(char c) {
  return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

size_t CountChars(std::string_view s) {
  size_t n = 0;
  for (char c : s) {
    if (!IsContinuation(c))
      ++n;
  }
  return n;
}

// C0 controls (but \n and \t when |breaks|) and DEL.
bool HasControlChars(std::string_view s, bool breaks) {
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (breaks && (c == '\n' || c == '\t'))
      continue;
    if (c < 0x20 || c == 0x7F)
      return true;
  }
  return false;
}

bool HasTemplateBraces(std::string_view s) {
  return s.find("{{") != std::string_view::npos ||
         s.find("}}") != std::string_view::npos;
}

bool IsHttpOrigin(std::string_view s) {
  if (s.empty() || s.size() > kMaxOriginBytes)
    return false;
  const GURL url{std::string(s)};
  return url.is_valid() && url.SchemeIsHTTPOrHTTPS() &&
         url::Origin::Create(url).Serialize() == s;
}

std::string HostOf(std::string_view origin) {
  return std::string(GURL(std::string(origin)).host());
}

// A frame's path as the recorder keeps it: "" (no address of its own) or
// an http(s) URL's path, one line, within its cap.
bool IsFramePath(std::string_view path) {
  return path.empty() ||
         (path.size() <= kMaxFramePathBytes && path[0] == '/' &&
          !HasControlChars(path, /*breaks=*/false));
}

// A whole "{{name}}" variable reference the recorder writes.
bool IsVariableRef(std::string_view s) {
  if (s.size() < 5 || !s.starts_with("{{") || !s.ends_with("}}"))
    return false;
  const std::string_view name = s.substr(2, s.size() - 4);
  return !name.empty() && name.size() <= kMaxVariableNameChars &&
         std::all_of(name.begin(), name.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
         });
}

// A value of a stored argument: what ReadArguments() writes (a string
// within the agent's caps, a finite number, a boolean, a list of those),
// or a whole {{variable}}.
bool IsStoredScalar(const base::Value& v, size_t max_chars) {
  if (v.is_string()) {
    const std::string& s = v.GetString();
    if (IsVariableRef(s))
      return true;
    return s.size() <= 4 * max_chars && CountChars(s) <= max_chars &&
           !HasControlChars(s, /*breaks=*/true) && !HasTemplateBraces(s);
  }
  if (v.is_double())
    return std::isfinite(v.GetDouble());
  return v.is_int() || v.is_bool();
}

bool IsStoredValue(const base::Value& v) {
  if (!v.is_list())
    return IsStoredScalar(v, agent_tools::kMaxToolStringChars);
  const base::ListValue& l = v.GetList();
  if (l.size() > agent_tools::kMaxToolListItems)
    return false;
  for (const base::Value& item : l) {
    if (item.is_list() || !IsStoredScalar(item, agent_tools::kMaxToolItemChars))
      return false;
  }
  return true;
}

const ToolArgument* FindArgument(const SiteTool& tool, std::string_view name) {
  for (const ToolArgument& a : tool.arguments) {
    if (a.name == name)
      return &a;
  }
  return nullptr;
}

// Whether |v| (a stored value) is of |arg|'s kind.
bool SameKind(const base::Value& v, const ToolArgument& arg) {
  if (v.is_list() != arg.list)
    return false;
  auto scalar = [&](const base::Value& x) {
    switch (arg.type) {
      case ArgType::kString:
        return x.is_string();
      case ArgType::kInteger:
      case ArgType::kNumber:
        return x.is_int() || x.is_double();
      case ArgType::kBoolean:
        return x.is_bool();
    }
    return false;
  };
  if (!v.is_list())
    return scalar(v);
  for (const base::Value& item : v.GetList()) {
    if (!scalar(item))
      return false;
  }
  return true;
}

// A variable name for a withheld value of argument |argument|: [a-z0-9_],
// at most kMaxVariableNameChars, starting with a letter, not taken.
std::string VariableNameFor(std::string_view argument,
                            const std::map<std::string, std::string>& taken) {
  std::string slug = "tool_";
  for (char c : argument) {
    if (slug.size() >= kMaxVariableNameChars - 3)
      break;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
      slug.push_back(c);
    else if (c >= 'A' && c <= 'Z')
      slug.push_back(static_cast<char>(c - 'A' + 'a'));
    else if (slug.back() != '_')
      slug.push_back('_');
  }
  while (slug.back() == '_')
    slug.pop_back();
  if (!taken.count(slug))
    return slug;
  for (int n = 2;; ++n) {
    const std::string candidate = slug + "_" + base::NumberToString(n);
    if (!taken.count(candidate))
      return candidate;
  }
}

}  // namespace

RecordedCall::RecordedCall() = default;
RecordedCall::~RecordedCall() = default;
RecordedCall::RecordedCall(const RecordedCall&) = default;
RecordedCall& RecordedCall::operator=(const RecordedCall&) = default;

ToolStep::ToolStep() = default;
ToolStep::~ToolStep() = default;
ToolStep::ToolStep(ToolStep&&) = default;
ToolStep& ToolStep::operator=(ToolStep&&) = default;

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

std::optional<Step> BuildToolStep(
    const RecordedCall& call,
    const ValuePolicy& values,
    std::map<std::string, std::string>* variables) {
  if (!IsHttpOrigin(call.origin) ||
      !agent_tools::IsValidToolName(call.tool.name)) {
    return std::nullopt;
  }
  if (call.in_frame &&
      (!IsHttpOrigin(call.frame_origin) || !IsFramePath(call.frame_path))) {
    return std::nullopt;
  }
  std::optional<base::DictValue> args = base::JSONReader::ReadDict(
      call.arguments_json, base::JSON_PARSE_RFC, kMaxArgumentsDepth);
  if (!args || args->size() > agent_tools::kMaxToolArguments)
    return std::nullopt;

  // A tool that takes a card or a one-time code (by its name, an
  // argument's or its form's fields): none of its values is stored.
  const bool sensitive_tool = agent_safety::IsSensitive(call.tool.sensitive);
  base::DictValue stored;
  base::ListValue secret;
  bool withheld = false;
  for (const auto [name, value] : *args) {
    const ToolArgument* arg = FindArgument(call.tool, name);
    if (!arg || !agent_tools::IsValidArgumentName(name) ||
        !SameKind(value, *arg)) {
      return std::nullopt;  // not what ReadArguments() writes for the tool
    }
    if (sensitive_tool ||
        agent_safety::IsSensitive(agent_safety::ClassifySelector(name))) {
      secret.Append(name);
      continue;
    }
    if (value.is_string()) {
      const std::string& text = value.GetString();
      if (values.Withhold(name, text) || HasTemplateBraces(text) ||
          !IsStoredScalar(value, agent_tools::kMaxToolStringChars)) {
        const std::string var = VariableNameFor(name, *variables);
        (*variables)[var] = std::string();
        stored.Set(name, "{{" + var + "}}");
        withheld = true;
        continue;
      }
    } else if (value.is_list()) {
      // A list holding a secret-looking string can't be asked as one
      // value: it isn't stored, and the step can't replay.
      bool secret_item = false;
      for (const base::Value& item : value.GetList()) {
        if (item.is_string() &&
            (values.Withhold(name, item.GetString()) ||
             HasTemplateBraces(item.GetString()))) {
          secret_item = true;
        }
      }
      if (secret_item || !IsStoredValue(value)) {
        secret.Append(name);
        continue;
      }
    } else if (!IsStoredValue(value)) {
      return std::nullopt;
    }
    stored.Set(name, value.Clone());
  }

  Step s;
  s.type = StepType::CALL_TOOL;
  s.target = call.tool.name;
  const std::string host = HostOf(call.origin);
  base::DictValue tool;
  tool.Set("origin", call.origin);
  if (call.in_frame) {
    base::DictValue frame;
    frame.Set("origin", call.frame_origin);
    frame.Set("path", call.frame_path);
    tool.Set("frame", std::move(frame));
  }
  tool.Set("args", std::move(stored));
  const bool has_secret = !secret.empty();
  if (has_secret)
    tool.Set("secret_args", std::move(secret));
  tool.Set("read_only", call.tool.read_only && !call.tool.consequential);
  tool.Set("form", call.tool.form_backed);
  base::DictValue extra;
  extra.Set(kStepHostKey, host);
  extra.Set(kToolKey, std::move(tool));
  if (has_secret)
    extra.Set("review", kReviewSecretArgument);
  else if (withheld)
    extra.Set("review", kReviewWithheld);
  s.extra = base::Value(std::move(extra));
  // The browser's words: the tool's name (Blink's charset) and the sites.
  s.description = "Run the site's tool \xE2\x80\x9C" + call.tool.name +
                  "\xE2\x80\x9D on " + host;
  if (call.in_frame && call.frame_origin != call.origin)
    s.description += " (in its frame from " + HostOf(call.frame_origin) + ")";
  return s;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

std::optional<ToolStep> ParseToolStep(const Step& step) {
  if (step.type != StepType::CALL_TOOL ||
      !agent_tools::IsValidToolName(step.target) || !step.extra.is_dict()) {
    return std::nullopt;
  }
  const base::DictValue& extra = step.extra.GetDict();
  const std::string* host = extra.FindString(kStepHostKey);
  const base::DictValue* tool = extra.FindDict(kToolKey);
  if (!host || host->empty() || host->size() > kMaxHostBytes || !tool)
    return std::nullopt;
  ToolStep out;
  out.name = step.target;
  const std::string* origin = tool->FindString("origin");
  if (!origin || !IsHttpOrigin(*origin) || HostOf(*origin) != *host)
    return std::nullopt;
  out.origin = *origin;
  out.host = *host;
  if (const base::Value* frame = tool->Find("frame")) {
    const base::DictValue* f = frame->GetIfDict();
    const std::string* fo = f ? f->FindString("origin") : nullptr;
    const std::string* fp = f ? f->FindString("path") : nullptr;
    if (!fo || !fp || !IsHttpOrigin(*fo) || !IsFramePath(*fp))
      return std::nullopt;
    out.in_frame = true;
    out.frame_origin = *fo;
    out.frame_path = *fp;
  }
  const base::DictValue* args = tool->FindDict("args");
  if (!args || args->size() > agent_tools::kMaxToolArguments)
    return std::nullopt;
  for (const auto [name, value] : *args) {
    if (!agent_tools::IsValidArgumentName(name) || !IsStoredValue(value))
      return std::nullopt;
  }
  out.arguments = args->Clone();
  if (const base::Value* secret = tool->Find("secret_args")) {
    const base::ListValue* l = secret->GetIfList();
    if (!l || l->empty() || l->size() > agent_tools::kMaxToolArguments)
      return std::nullopt;
    for (const base::Value& n : *l) {
      if (!n.is_string() || !agent_tools::IsValidArgumentName(n.GetString()) ||
          out.arguments.contains(n.GetString()) ||
          std::find(out.secret_arguments.begin(), out.secret_arguments.end(),
                    n.GetString()) != out.secret_arguments.end()) {
        return std::nullopt;
      }
      out.secret_arguments.push_back(n.GetString());
    }
    if (out.arguments.size() + out.secret_arguments.size() >
        agent_tools::kMaxToolArguments) {
      return std::nullopt;
    }
  }
  for (const char* key : {"read_only", "form"}) {
    if (const base::Value* b = tool->Find(key); b && !b->is_bool())
      return std::nullopt;
  }
  out.recorded_read_only = tool->FindBool("read_only").value_or(false);
  out.recorded_form = tool->FindBool("form").value_or(false);
  return out;
}

bool IsCrossOriginFrame(const ToolStep& step) {
  return step.in_frame && step.frame_origin != step.origin;
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

std::optional<size_t> FindListedTool(
    const ToolStep& step,
    const std::vector<SiteTool>& tools,
    const std::vector<ListedFrame>& frames) {
  std::vector<size_t> in_origin;
  std::vector<size_t> at_path;
  for (size_t i = 0; i < tools.size(); ++i) {
    const SiteTool& t = tools[i];
    if (t.name != step.name || t.frame >= frames.size())
      continue;
    // Another site's frame only for a step recorded in one, at its origin.
    if (t.cross_origin != IsCrossOriginFrame(step) ||
        (t.cross_origin && t.frame_origin != step.frame_origin)) {
      continue;
    }
    if (!step.in_frame) {
      if (t.frame == 0)
        return i;  // a name is unique in its document
      continue;
    }
    if (t.frame == 0 || frames[t.frame].origin != step.frame_origin)
      continue;
    in_origin.push_back(i);
    if (frames[t.frame].path == step.frame_path)
      at_path.push_back(i);
  }
  if (at_path.size() == 1)
    return at_path[0];
  if (at_path.empty() && in_origin.size() == 1)
    return in_origin[0];
  return std::nullopt;
}

Compat CheckReplay(const ToolStep& step,
                   const SiteTool& live,
                   const base::DictValue& arguments,
                   agent_tools::ToolArguments* out) {
  if (!step.secret_arguments.empty())
    return Compat::kSecretArgument;
  if (live.form_backed && !step.recorded_form)
    return Compat::kChangedKind;
  for (const auto [name, value] : arguments) {
    const ToolArgument* arg = FindArgument(live, name);
    if (!arg || !SameKind(value, *arg))
      return Compat::kMissingArgument;
  }
  base::DictValue call = arguments.Clone();
  for (const ToolArgument& arg : live.arguments) {
    if (arguments.contains(arg.name))
      continue;
    if (arg.required)
      return Compat::kNewRequirement;
    call.Set(arg.name, base::Value());  // left out
  }
  if (agent_tools::ReadArguments(base::Value(std::move(call)), live, out) !=
      agent_tools::ArgumentsError::kNone) {
    return Compat::kBadValue;
  }
  return Compat::kOk;
}

std::string_view CompatText(Compat compat) {
  switch (compat) {
    case Compat::kOk:
      return "";
    case Compat::kSecretArgument:
      return "This step gives the site's tool a card number or one-time "
             "code, which MoltBrowser never saves, so it can't be replayed. "
             "Remove the step, or do it yourself and run the rest.";
    case Compat::kChangedKind:
      return "Not run: the site's tool now fills a form, which it didn't "
             "when this step was recorded.";
    case Compat::kMissingArgument:
      return "Not run: the site's tool no longer takes what this step "
             "recorded.";
    case Compat::kNewRequirement:
      return "Not run: the site's tool now needs something this step "
             "didn't record.";
    case Compat::kBadValue:
      return "Not run: a value this step recorded no longer fits the site's "
             "tool.";
  }
  return "";
}

Approval ApprovalFor(const ToolStep& step,
                     const SiteTool& live,
                     bool approved_for_site) {
  if (IsCrossOriginFrame(step) || live.cross_origin ||
      agent_safety::IsSensitive(live.sensitive)) {
    return Approval::kAsk;
  }
  if (agent_tools::ToolCallEffects(live).empty())
    return Approval::kRun;  // the site says it only reads
  return approved_for_site ? Approval::kRun : Approval::kAsk;
}

bool TrustWouldAllow(const ToolStep& step, const SiteTool& live) {
  return !IsCrossOriginFrame(step) && !live.cross_origin &&
         !agent_safety::IsSensitive(live.sensitive);
}

}  // namespace tool_steps
}  // namespace automation
}  // namespace molt_ai
