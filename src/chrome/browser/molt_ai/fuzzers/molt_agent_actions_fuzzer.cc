// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The web agent's and the AI chat's action format (agents/agent_actions.h,
// agents/chat_actions.h) on everything outside the browser: the page's
// answer to the element snapshot script and to the element probe and action
// scripts (any JavaScript value, here any JSON), the sandboxed model's reply
// (any text), and the user's request and page URL / title.
//
// Checks:
//  * the observation is capped and clean: at most kMaxObservedElements,
//    refs e1..eN, selectors and labels within their caps without control
//    characters, never a typeable password field;
//  * the action schema for any observation is inside the inference
//    service's supported subset (JsonSchemaToGrammar() accepts it);
//  * an action ParseAction() accepts is one of the allowed set, names only
//    elements of the observation (a text field for TYPE / FILL_FORM, each
//    once) and respects every cap;
//  * InterpretReply() acts only on such an action and otherwise says why;
//  * the chat's decision prompt keeps page text inside one fence that page
//    text can't forge; CheckProposal() never lets model text into a
//    password field and never opens a non-http(s) URL; MayRun() runs an
//    asked-about action only with the user's OK on that exact reason.

#include <algorithm>
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
#include "base/values.h"
#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/agents/chat_actions.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/services/molt_ai/inference/engine/json_schema_grammar.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai {
namespace {

using agent_actions::ActionSet;
using agent_actions::ActionType;
using agent_actions::AgentAction;
using agent_actions::Observation;
using agent_actions::ObservedElement;

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

size_t Count(std::string_view haystack, std::string_view needle) {
  size_t n = 0;
  for (size_t at = haystack.find(needle); at != std::string_view::npos;
       at = haystack.find(needle, at + 1)) {
    ++n;
  }
  return n;
}

// Any JavaScript value a page script can evaluate to, as JSON.
base::Value PageValue(const std::string& json) {
  std::optional<base::Value> v =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  return v ? std::move(*v) : base::Value(json);
}

void CheckObservation(const Observation& observation,
                      const std::string& origin) {
  using namespace agent_actions;
  CHECK(observation.elements.size() <= kMaxObservedElements);
  CHECK(origin.size() <= 512 && !HasControl(origin, false));
  for (size_t i = 0; i < observation.elements.size(); ++i) {
    const ObservedElement& e = observation.elements[i];
    CHECK(e.ref == "e" + std::to_string(i + 1));
    CHECK(observation.Find(e.ref) == &e);
    CHECK(!e.selector.empty() && e.selector.size() <= kMaxSelectorBytes);
    CHECK(!HasControl(e.selector, false));
    CHECK(CountChars(e.label) <= kMaxElementLabelChars);
    CHECK(!HasControl(e.label, false));
    CHECK(e.label.find('"') == std::string::npos);
    if (e.typeable) {
      CHECK(e.field_kind != agent_safety::FieldKind::kPassword)
          << "a password field the agent may type into";
    }
  }
  const std::string described = DescribeElements(observation);
  CHECK(!described.empty());
}

void CheckAction(const AgentAction& a,
                 const Observation& observation,
                 ActionSet allowed) {
  using namespace agent_actions;
  CHECK(HasAction(allowed, a.type)) << "an action outside the allowed set";
  CHECK(CountChars(a.reason) <= kMaxReasonChars && !HasControl(a.reason, false));
  switch (a.type) {
    case ActionType::kNavigate:
      CHECK(!a.url.empty() && CountChars(a.url) <= kMaxUrlChars);
      CHECK(!HasControl(a.url, false));
      break;
    case ActionType::kSearch:
      CHECK(!a.query.empty() && CountChars(a.query) <= kMaxQueryChars);
      CHECK(!HasControl(a.query, false));
      break;
    case ActionType::kClick:
      CHECK(observation.Find(a.ref)) << "clicks an element it wasn't shown";
      break;
    case ActionType::kType: {
      const ObservedElement* e = observation.Find(a.ref);
      CHECK(e && e->typeable) << "types into something that isn't a field";
      CHECK(CountChars(a.text) <= kMaxTypedChars && !HasControl(a.text, true));
      break;
    }
    case ActionType::kFillForm: {
      CHECK(!a.fields.empty() && a.fields.size() <= kMaxFormFields);
      std::set<std::string> refs;
      for (const FormFieldValue& f : a.fields) {
        const ObservedElement* e = observation.Find(f.ref);
        CHECK(e && e->typeable);
        CHECK(refs.insert(f.ref).second) << "a field filled twice";
        CHECK(CountChars(f.text) <= kMaxTypedChars && !HasControl(f.text, true));
      }
      break;
    }
    case ActionType::kScroll:
      CHECK(a.direction == "down" || a.direction == "up");
      break;
    case ActionType::kObserve:
      break;
    case ActionType::kDone:
    case ActionType::kError:
      CHECK(!a.result.empty() && CountChars(a.result) <= kMaxResultChars);
      CHECK(!HasControl(a.result, true));
      break;
    case ActionType::kCallTool:
      // Site tools have their own fuzzer (molt_agent_tools_fuzzer); this
      // observation has none, so no call can be accepted.
      CHECK(observation.FindTool(a.tool)) << "a call to a tool not offered";
      break;
    case ActionType::kReplan:
      // Only with a plan (molt_agent_plan_fuzzer); never in these sets, which
      // the first CHECK above enforces.
      break;
    case ActionType::kSelect: {
      // Only on a page map's select (molt_agent_page_map_fuzzer): the
      // element snapshot lists none, so no SELECT can be accepted here.
      const ObservedElement* e = observation.Find(a.ref);
      CHECK(e && e->selectable) << "a SELECT on something that isn't a select";
      CHECK(std::find(e->options.begin(), e->options.end(), a.option) !=
            e->options.end());
      break;
    }
  }
}

void CheckChat(FuzzedDataProvider* p,
               const AgentAction& action,
               const Observation& observation) {
  using namespace chat_actions;
  const std::string request = p->ConsumeRandomLengthString(2048);
  const std::string page_url = p->ConsumeRandomLengthString(512);
  const std::string page_title = p->ConsumeRandomLengthString(512);
  LooksLikePageActionRequest(request);

  const DecisionPrompt prompt = BuildDecisionPrompt(
      request, page_url, page_title, observation,
      p->ConsumeBool() ? agent_actions::ActionErrorText(
                             agent_actions::ActionError::kUnknownElement)
                       : std::string_view());
  // One fence, which page text can't forge or close early.
  CHECK(Count(prompt.context, agent_safety::kUntrustedBegin) == 1);
  CHECK(Count(prompt.context, agent_safety::kUntrustedEnd) == 1);
  CHECK(prompt.context.ends_with(agent_safety::kUntrustedEnd));
  CHECK(prompt.user_message.find("<<<") == std::string::npos);
  CHECK(prompt.system_prompt ==
        BuildDecisionPrompt("", "", "", Observation(), "").system_prompt)
      << "page or user text in the system prompt";

  const url::Origin page_origin = url::Origin::Create(GURL(page_url));
  const Scope scope = Scope::ForRequest(page_origin, request);
  const std::vector<std::string> targets = TargetSelectors(action, observation);
  // The page answers the probe script with anything.
  const base::Value probe_result = PageValue(p->ConsumeRandomLengthString(4096));
  const std::vector<agent_safety::ElementProbe> probes =
      agent_safety::ParseElementProbes(
          probe_result, p->ConsumeBool() ? targets.size()
                                         : p->ConsumeIntegralInRange<size_t>(0, 9));
  const Check check =
      CheckProposal(action, observation, scope, page_origin, probes);
  CHECK(check.verdict == Verdict::kAct || !check.message.empty());
  if (check.verdict != Verdict::kRefuse) {
    if (action.type == ActionType::kType ||
        action.type == ActionType::kFillForm) {
      for (const agent_safety::ElementProbe& probe : probes) {
        CHECK(probe.field_kind != agent_safety::FieldKind::kPassword)
            << "model text may go into a password field";
      }
    }
    if (action.type == ActionType::kNavigate ||
        action.type == ActionType::kSearch) {
      CHECK(agent_safety::IsAllowedNavigationURL(GURL(NavigationURL(action))));
    }
    CHECK(action.type != ActionType::kObserve &&
          action.type != ActionType::kError);
  }
  const bool confirmed = p->ConsumeBool();
  const std::string approved =
      p->ConsumeBool() ? check.message : p->ConsumeRandomLengthString(64);
  const bool runs = MayRun(check, confirmed, approved);
  switch (check.verdict) {
    case Verdict::kAct:
      CHECK(runs);
      break;
    case Verdict::kAsk:
      CHECK(runs == (confirmed && approved == check.message));
      break;
    case Verdict::kRefuse:
      CHECK(!runs);
      break;
  }
  const std::string description = DescribeProposal(action, observation);
  CHECK(!description.empty());
  const ScriptOutcome outcome = InterpretScriptResult(
      action, PageValue(p->ConsumeRandomLengthString(256)));
  CHECK(outcome.success || !outcome.message.empty());
}

}  // namespace
}  // namespace molt_ai

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai;
  using namespace molt_ai::agent_actions;
  FuzzedDataProvider p(data, size);
  const ActionSet allowed =
      p.ConsumeBool() ? kAgentActions : chat_actions::kChatActions;

  // The page's answer to BuildElementSnapshotScript().
  std::string origin;
  const Observation observation = ParseElementSnapshot(
      PageValue(p.ConsumeRandomLengthString(16384)), &origin);
  CheckObservation(observation, origin);

  // The schema the model's reply is decoded under: always one the
  // inference service supports, whatever the page put in the snapshot.
  const std::string schema = BuildActionSchema(observation, allowed);
  const SchemaGrammar grammar = JsonSchemaToGrammar(schema);
  CHECK(grammar.status == SchemaGrammarStatus::kOk)
      << "the service would refuse the action schema: " << grammar.reason;

  // The model's reply.
  ModelReply reply;
  reply.success = p.ConsumeBool();
  reply.cancelled = p.ConsumeIntegralInRange<int>(0, 15) == 0;
  reply.constraint_unsupported = p.ConsumeIntegralInRange<int>(0, 15) == 0;
  reply.invalid_output = p.ConsumeIntegralInRange<int>(0, 7) == 0;
  reply.truncated = p.ConsumeBool();
  reply.error_message = p.ConsumeRandomLengthString(64);
  reply.text = p.ConsumeRandomLengthString(4096);
  const ParsedAction parsed = ParseAction(reply.text, observation, allowed);
  if (parsed.action) {
    CHECK(parsed.error == ActionError::kNone);
    CheckAction(*parsed.action, observation, allowed);
  } else {
    CHECK(parsed.error != ActionError::kNone);
    CHECK(!ActionErrorText(parsed.error).empty());
  }
  const int bad_in_a_row = p.ConsumeIntegralInRange<int>(0, 5);
  const int bad_total = p.ConsumeIntegralInRange<int>(bad_in_a_row, 8);
  const ReplyOutcome outcome =
      InterpretReply(reply, observation, bad_in_a_row, bad_total, allowed);
  if (outcome.decision == ReplyDecision::kAct) {
    CHECK(reply.success && !reply.cancelled && parsed.action);
    CheckAction(outcome.action, observation, allowed);
  } else {
    CHECK(!outcome.message.empty());
  }
  chat_actions::InterpretDecision(reply, observation, bad_total);

  // What the action scripts are built from.
  const std::string url = BuildSearchURL(p.ConsumeRandomLengthString(512));
  CHECK(url.starts_with(kSearchURLPrefix));
  for (size_t i = sizeof(kSearchURLPrefix) - 1; i < url.size(); ++i) {
    const char c = url[i];
    CHECK((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
          c == '~' || c == '+' || c == '%');
  }
  if (!observation.elements.empty()) {
    const ObservedElement& e = observation.elements.front();
    BuildClickScript(origin, e.selector);
    BuildTypeScript(origin, e.selector, reply.text);
    BuildFillFormScript(origin, {{e.selector, reply.text}});
  }
  BuildScrollScript(reply.text);

  // The chat's checks on the parsed action, or on any action (they must
  // hold whatever the action is).
  AgentAction action;
  if (parsed.action) {
    action = *parsed.action;
  } else {
    action.type = static_cast<ActionType>(p.ConsumeIntegralInRange<int>(0, 8));
    action.url = p.ConsumeRandomLengthString(256);
    action.query = p.ConsumeRandomLengthString(64);
    action.text = p.ConsumeRandomLengthString(64);
    action.direction = p.ConsumeBool() ? "up" : "down";
    const size_t n = observation.elements.size();
    if (n > 0) {
      action.ref =
          observation.elements[p.ConsumeIntegralInRange<size_t>(0, n - 1)].ref;
      const size_t fields = p.ConsumeIntegralInRange<size_t>(0, 3);
      for (size_t i = 0; i < fields; ++i) {
        action.fields.push_back(
            {observation.elements[p.ConsumeIntegralInRange<size_t>(0, n - 1)]
                 .ref,
             "x"});
      }
    }
  }
  CheckChat(&p, action, observation);
  return 0;
}
