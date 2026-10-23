// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The web agent's planner (agents/agent_plan.h, the plan's part of
// agents/agent_actions.h and agents/agent_prompt.h) on everything outside
// the browser: the sandboxed model's plan and its next action (any text),
// the page's element snapshot (any JSON), the page text and the user's goal
// in the prompts, and any order of the run's events (steps reported,
// actions that work or fail, rewrites).
//
// Checks:
//  * the plan schema is one the inference service converts; a plan
//    ParsePlan() accepts has 1 to kMaxPlanSteps steps, each goal and
//    done_when non-empty, within its cap in characters, on one line, with
//    no control, bidi, zero-width, BOM, soft hyphen, Hangul filler,
//    variation selector or tag character (decoded independently of
//    agent_plan.cc) and no space at either end; DescribePlan() writes
//    exactly one line per step;
//  * InterpretPlanReply() uses a plan only from a successful, constrained,
//    complete reply that ParsePlan() accepts, asks again at most
//    kMaxPlanAttempts times, then goes on without a plan, and otherwise
//    says why;
//  * PlanTracker matches a reference model whatever the run does: the
//    current step moves only to the next one, the rewrites stop at
//    kMaxReplans, the triggers fire exactly after kMaxFailuresInARow
//    failures in a row or kMaxActionsPerStep actions on one step;
//  * the action schema with any plan the tracker offers converts in the
//    service; an action ParseAction() accepts with a plan names a step
//    AllowedPlanSteps() lists (and REPLAN only exists with a plan), and
//    without one never has a step;
//  * the action prompt and the planner's prompt keep the plan and the page
//    inside their own fences, which neither can forge; the rules never
//    depend on the plan, the page or the goal; the GOAL line comes first.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/agents/agent_plan.h"
#include "chrome/browser/molt_ai/agents/agent_prompt.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/services/molt_ai/inference/engine/json_schema_grammar.h"

namespace molt_ai {
namespace {

using agent_actions::ActionType;
using agent_actions::PlanSteps;
using agent_plan::Plan;
using agent_plan::PlanTracker;
using agent_plan::ReplanReason;

size_t CountChars(std::string_view s) {
  size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
  }
  return n;
}

size_t Count(std::string_view haystack, std::string_view needle) {
  size_t n = 0;
  for (size_t at = haystack.find(needle); at != std::string_view::npos;
       at = haystack.find(needle, at + 1)) {
    ++n;
  }
  return n;
}

// A control, or a character that reorders, hides or breaks text, or shows
// nothing (agent_plan.cc's list), by code point.
bool Hidden(uint32_t c) {
  return c < 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x9F) || c == 0xAD ||
         c == 0x61C || c == 0x115F || c == 0x1160 || c == 0x180E ||
         (c >= 0x200B && c <= 0x200F) || (c >= 0x2028 && c <= 0x202E) ||
         (c >= 0x2060 && c <= 0x2064) || (c >= 0x2066 && c <= 0x2069) ||
         c == 0x3164 || (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFEFF ||
         c == 0xFFA0 || (c >= 0xE0000 && c <= 0xE007F) ||
         (c >= 0xE0100 && c <= 0xE01EF);
}

// Valid UTF-8 without a Hidden() character. Decoded here, independently of
// agent_plan.cc's byte patterns.
bool CleanLine(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    size_t len = 1;
    uint32_t c = b0;
    if (b0 >= 0xF0) {
      len = 4;
      c = b0 & 0x07;
    } else if (b0 >= 0xE0) {
      len = 3;
      c = b0 & 0x0F;
    } else if (b0 >= 0xC0) {
      len = 2;
      c = b0 & 0x1F;
    } else if (b0 >= 0x80) {
      return false;  // A stray continuation byte.
    }
    if (i + len > s.size()) return false;
    for (size_t k = 1; k < len; ++k) {
      const auto b = static_cast<unsigned char>(s[i + k]);
      if ((b & 0xC0) != 0x80) return false;
      c = (c << 6) | (b & 0x3F);
    }
    if (Hidden(c)) return false;
    i += len;
  }
  return true;
}

void CheckPlan(const Plan& plan) {
  using namespace agent_plan;
  CHECK(!plan.empty() && plan.size() <= kMaxPlanSteps);
  auto check = [](const std::string& text, size_t cap) {
    CHECK(!text.empty()) << "a blank plan step";
    CHECK(CountChars(text) <= cap) << "a plan step over its cap";
    CHECK(CleanLine(text)) << "a plan step with hidden or control text";
    CHECK(text.front() != ' ' && text.back() != ' ');
  };
  for (const agent_plan::PlanStep& step : plan) {
    check(step.goal, kMaxGoalChars);
    check(step.done_when, kMaxDoneWhenChars);
  }
  const std::string described = DescribePlan(plan);
  CHECK(Count(described, "\n") == plan.size());
  CHECK(described.starts_with("1. "));
}

// The tracker against a reference model of the same rules.
struct Reference {
  size_t size = 0;
  size_t current = 0;
  int actions = 0;
  int failures = 0;
  int replans = 0;
};

void Adopt(PlanTracker* t, Reference* r, const Plan& plan) {
  t->Adopt(plan);
  r->size = std::min(plan.size(), agent_plan::kMaxPlanSteps);
  r->current = 0;
  r->actions = 0;
  r->failures = 0;
}

void CheckTracker(const PlanTracker& t, const Reference& r) {
  CHECK(t.has_plan() == (r.size > 0));
  CHECK(t.plan().size() == r.size);
  CHECK(t.current_step() == (r.size ? static_cast<int>(r.current) + 1 : 0));
  CHECK(t.replans() == r.replans && t.replans() <= agent_plan::kMaxReplans);
  if (t.has_plan()) {
    const std::vector<int> steps = agent_actions::AllowedPlanSteps(t.Steps());
    CHECK(steps.front() == t.current_step());
    CHECK(steps.size() == (r.current + 1 < r.size ? 2u : 1u));
    CHECK(!t.DescribeProgress().empty());
  } else {
    CHECK(t.DescribeProgress().empty());
  }
}

base::Value PageValue(const std::string& json) {
  std::optional<base::Value> v = base::JSONReader::Read(json);
  return v ? std::move(*v) : base::Value(json);
}

std::string ConsumeText(FuzzedDataProvider* p, size_t max) {
  return p->ConsumeRandomLengthString(max);
}

}  // namespace
}  // namespace molt_ai

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai;
  FuzzedDataProvider p(data, size);

  static const bool schema_ok = [] {
    const SchemaGrammar g = JsonSchemaToGrammar(agent_plan::BuildPlanSchema());
    return g.status == SchemaGrammarStatus::kOk;
  }();
  CHECK(schema_ok) << "the service would refuse the plan schema";

  // The model's plan.
  const std::string plan_text = ConsumeText(&p, 8192);
  const agent_plan::ParsedPlan parsed = agent_plan::ParsePlan(plan_text);
  if (parsed.plan) {
    CHECK(parsed.error == agent_plan::PlanError::kNone);
    CheckPlan(*parsed.plan);
  } else {
    CHECK(parsed.error != agent_plan::PlanError::kNone);
    CHECK(!agent_plan::PlanErrorText(parsed.error).empty());
  }
  agent_actions::ModelReply reply;
  reply.success = p.ConsumeBool();
  reply.cancelled = p.ConsumeIntegralInRange<int>(0, 15) == 0;
  reply.constraint_unsupported = p.ConsumeIntegralInRange<int>(0, 15) == 0;
  reply.invalid_output = p.ConsumeIntegralInRange<int>(0, 7) == 0;
  reply.truncated = p.ConsumeBool();
  reply.text = plan_text;
  const int bad = p.ConsumeIntegralInRange<int>(0, 3);
  const agent_plan::PlanOutcome outcome =
      agent_plan::InterpretPlanReply(reply, bad);
  switch (outcome.decision) {
    case agent_plan::PlanDecision::kUse:
      CHECK(reply.success && !reply.cancelled &&
            !reply.constraint_unsupported && !reply.invalid_output &&
            parsed.plan);
      CHECK(outcome.plan.size() == parsed.plan->size());
      break;
    case agent_plan::PlanDecision::kRetry:
      CHECK(bad + 1 < agent_plan::kMaxPlanAttempts);
      CHECK(!reply.cancelled && !reply.constraint_unsupported);
      CHECK(outcome.message.starts_with("Your previous "));
      CHECK(outcome.plan.empty());
      break;
    case agent_plan::PlanDecision::kWithoutPlan:
      CHECK(bad + 1 >= agent_plan::kMaxPlanAttempts);
      CHECK(!outcome.message.empty() && outcome.plan.empty());
      break;
    case agent_plan::PlanDecision::kStop:
      CHECK(!outcome.message.empty() && outcome.plan.empty());
      CHECK(reply.cancelled || reply.constraint_unsupported ||
            (!reply.invalid_output && !reply.success));
      break;
  }

  // The page's elements, for the action schema and parser.
  std::string origin;
  const agent_actions::Observation observation =
      agent_actions::ParseElementSnapshot(PageValue(ConsumeText(&p, 8192)),
                                          &origin);

  // The run: any order of events, against the reference model.
  PlanTracker tracker;
  Reference ref;
  Plan fallback = {{"Goal", "Check"}, {"Goal 2", "Check 2"}};
  if (parsed.plan)
    Adopt(&tracker, &ref, *parsed.plan);
  CheckTracker(tracker, ref);
  const std::string action_text = ConsumeText(&p, 4096);
  const int events = p.ConsumeIntegralInRange<int>(0, 40);
  for (int i = 0; i < events; ++i) {
    switch (p.ConsumeIntegralInRange<int>(0, 5)) {
      case 0: {
        const int step = p.ConsumeIntegralInRange<int>(-2, 8);
        const bool expected =
            ref.size && ref.current + 1 < ref.size &&
            step == static_cast<int>(ref.current) + 2;
        CHECK(tracker.Report(step) == expected);
        if (expected) {
          ++ref.current;
          ref.actions = 0;
          ref.failures = 0;
        }
        break;
      }
      case 1:
      case 2: {
        const bool ok = p.ConsumeBool();
        const ReplanReason why = tracker.AfterAction(ok);
        if (!ref.size) {
          CHECK(why == ReplanReason::kNone);
          break;
        }
        ++ref.actions;
        ref.failures = ok ? 0 : ref.failures + 1;
        const ReplanReason expected =
            ref.failures >= agent_plan::kMaxFailuresInARow
                ? ReplanReason::kFailures
            : ref.actions >= agent_plan::kMaxActionsPerStep
                ? ReplanReason::kTooManyActions
                : ReplanReason::kNone;
        CHECK(why == expected);
        break;
      }
      case 3: {
        const bool expected = ref.replans < agent_plan::kMaxReplans;
        CHECK(tracker.UseReplan() == expected);
        if (expected)
          ++ref.replans;
        break;
      }
      case 4:
        Adopt(&tracker, &ref, parsed.plan && p.ConsumeBool() ? *parsed.plan
                                                              : fallback);
        break;
      case 5:
        tracker.Drop();
        ref.size = 0;
        ref.current = 0;
        break;
    }
    CheckTracker(tracker, ref);
  }

  // The next action, with the plan the tracker offers (or none).
  const std::optional<PlanSteps> steps =
      tracker.has_plan() ? std::optional<PlanSteps>(tracker.Steps())
                         : std::nullopt;
  const agent_actions::ActionSet allowed =
      steps ? agent_actions::kPlannedAgentActions
            : agent_actions::kAgentActions;
  const PlanSteps* plan_steps = steps ? &*steps : nullptr;
  const SchemaGrammar g = JsonSchemaToGrammar(
      agent_actions::BuildActionSchema(observation, allowed, plan_steps));
  CHECK(g.status == SchemaGrammarStatus::kOk)
      << "the service would refuse the action schema: " << g.reason;
  const agent_actions::ParsedAction action =
      agent_actions::ParseAction(action_text, observation, allowed,
                                 plan_steps);
  if (action.action) {
    CHECK(agent_actions::HasAction(allowed, action.action->type));
    if (steps) {
      const std::vector<int> ok = agent_actions::AllowedPlanSteps(*steps);
      CHECK(std::find(ok.begin(), ok.end(), action.action->plan_step) !=
            ok.end())
          << "an action on a plan step it may not name";
    } else {
      CHECK(action.action->plan_step == 0);
      CHECK(action.action->type != ActionType::kReplan);
    }
    if (action.action->type == ActionType::kClick) {
      CHECK(observation.Find(action.action->ref));
    }
  }

  // The prompts.
  // The goal is the user's own text (trusted, not fenced); here it is any
  // text that holds no fence marker, so the fence counts below are exact.
  const std::string goal =
      agent_safety::SanitizeUntrustedText(ConsumeText(&p, 512));
  const std::string page = ConsumeText(&p, 4096);
  const std::string plan_lines =
      tracker.has_plan() ? agent_plan::DescribePlan(tracker.plan())
                         : std::string();
  agent_prompt::PromptInput in;
  in.goal = goal;
  in.page_state = page;
  in.max_steps = 20;
  in.steps_remaining = 20;
  in.with_tools = p.ConsumeBool();
  in.with_screenshot = p.ConsumeBool();
  in.plan = plan_lines;
  in.plan_progress = tracker.DescribeProgress();
  const agent_prompt::AgentPrompt prompt = agent_prompt::BuildAgentPrompt(in);
  const size_t fences = tracker.has_plan() ? 2 : 1;
  CHECK(Count(prompt.user, agent_safety::kUntrustedBegin) == fences);
  CHECK(Count(prompt.user, agent_safety::kUntrustedEnd) == fences);
  CHECK(prompt.user.starts_with("GOAL: " + goal + "\n"));
  // The rules depend on whether there is a plan, never on its text, the
  // page or the goal.
  agent_prompt::PromptInput other = in;
  other.goal = "g";
  other.page_state = "p";
  if (tracker.has_plan())
    other.plan = "1. a (done when: b)\n";
  CHECK(agent_prompt::BuildAgentPrompt(other).system == prompt.system)
      << "plan, page or goal text in the rules";
  if (tracker.has_plan()) {
    // The plan sits between the GOAL line and the history, in its fence.
    const size_t plan_begin = prompt.user.find(agent_safety::kUntrustedBegin);
    const size_t plan_end = prompt.user.find(agent_safety::kUntrustedEnd);
    CHECK(plan_begin < plan_end);
    CHECK(prompt.user.find("\nHISTORY:\n", plan_end) != std::string::npos);
  }

  agent_prompt::PlanPromptInput plan_in;
  plan_in.goal = goal;
  plan_in.page_state = page;
  plan_in.max_plan_steps = static_cast<int>(agent_plan::kMaxPlanSteps);
  plan_in.earlier_plan = plan_lines;
  plan_in.replan_reason =
      plan_lines.empty() ? std::string()
                         : agent_plan::ReplanReasonText(
                               ReplanReason::kAsked, tracker.current_step(),
                               static_cast<int>(tracker.plan().size()));
  const agent_prompt::AgentPrompt plan_prompt =
      agent_prompt::BuildPlanPrompt(plan_in);
  CHECK(Count(plan_prompt.user, agent_safety::kUntrustedBegin) == fences);
  CHECK(Count(plan_prompt.user, agent_safety::kUntrustedEnd) == fences);
  CHECK(plan_prompt.user.starts_with("GOAL: " + goal + "\n"));
  agent_prompt::PlanPromptInput plan_other = plan_in;
  plan_other.goal = "g";
  plan_other.page_state = "p";
  if (!plan_lines.empty())
    plan_other.earlier_plan = "1. a (done when: b)\n";
  CHECK(agent_prompt::BuildPlanPrompt(plan_other).system == plan_prompt.system)
      << "plan, page or goal text in the planner's rules";
  return 0;
}
