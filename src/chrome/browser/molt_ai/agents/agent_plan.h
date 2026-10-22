// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The web agent's planner (docs/design/AGENT_ACTIONS.md §2.8).
//
// Before its first action the web agent asks the on-device model for a
// short plan: at most kMaxPlanSteps steps, in order, each a goal in plain
// words and how to tell it is done. The model writes it under the grammar
// of BuildPlanSchema() and ParsePlan() reads it back strictly. While the
// run follows the plan, every action says which plan step it works on (the
// current one or, once that is done, the next: agent_actions::PlanSteps),
// and the prompt shows the plan and the current step. The plan is rewritten
// when the model asks (REPLAN), when actions keep failing on one step, or
// when one step takes too many actions; at most kMaxReplans times per run
// (PlanTracker), and the next time the run stops.
//
// A plan grants nothing. It is model output, written from the user's goal
// and from page text, so it is untrusted: it reaches the model fenced and
// never in the GOAL line, and the user as text. The browser never reads
// it for URLs, origins, element or tool ids; its only effect on the
// browser is which step is marked current. Every action is still chosen,
// parsed and checked one at a time (origin scope, confirmations,
// agent_safety, CheckToolCall), exactly as without a plan.
//
// Pure logic (base::Value, no content/), unit-tested in molt_ai_unittests
// and fuzzed by molt_agent_plan_fuzzer.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_PLAN_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_PLAN_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/agents/agent_actions.h"

namespace molt_ai {
namespace agent_plan {

// Caps. Lengths count characters (Unicode code points), as the grammar's
// maxLength does.
inline constexpr size_t kMaxPlanSteps = 6;
inline constexpr size_t kMaxGoalChars = 100;
inline constexpr size_t kMaxDoneWhenChars = 100;
// Visible-token budget for a plan.
inline constexpr int kMaxPlanReplyTokens = 512;
// A plan is asked for at most this many times; after the last unusable
// reply the run goes on without a plan.
inline constexpr int kMaxPlanAttempts = 2;
// A run's plan is rewritten at most this many times; the next time it
// fails, the run stops (kReplanLimitMessage).
inline constexpr int kMaxReplans = 2;
// The plan is rewritten when this many actions in a row fail on one step,
inline constexpr int kMaxFailuresInARow = 2;
// ... or when one step has taken this many actions.
inline constexpr int kMaxActionsPerStep = 8;

struct PlanStep {
  // What the step achieves, in plain words (model text: one line, no
  // control characters, at most kMaxGoalChars characters).
  std::string goal;
  // What the page shows once it is done (the same, kMaxDoneWhenChars).
  std::string done_when;
};

// 1 to kMaxPlanSteps steps, in order.
using Plan = std::vector<PlanStep>;

// The JSON Schema of the plan:
//   {"steps": [{"goal": "...", "done_when": "..."}, ...]}
// 1 to kMaxPlanSteps steps, every key required and no others, each string
// 1 to its cap. Inside the inference service's supported subset (the unit
// tests run it through the service's converter).
std::string BuildPlanSchema();

enum class PlanError {
  kNone,
  kNotJsonObject,  // not one JSON object (or nested too deep)
  kWrongKeys,      // a key missing, an extra key, or a value's type
  kStepCount,      // no step, or more than kMaxPlanSteps
  kTooLong,
  kEmpty,          // a blank goal or done_when
  kBadCharacters,  // control characters (a line break too)
};

// Fixed text, for the retry note the model sees.
std::string_view PlanErrorText(PlanError error);

struct ParsedPlan {
  ParsedPlan();
  ~ParsedPlan();
  ParsedPlan(ParsedPlan&&);
  ParsedPlan& operator=(ParsedPlan&&);

  std::optional<Plan> plan;
  PlanError error = PlanError::kNone;
};

// Parses and validates a plan (untrusted: whatever the service sent).
// Exactly {"steps": [...]} with 1 to kMaxPlanSteps objects of exactly
// "goal" and "done_when"; each a string within its cap in characters, not
// blank, without control characters (\n and \t included: one line each).
// Characters that reorder or hide text (C1 controls, bidi controls,
// zero-width characters, U+2028 / U+2029, U+FEFF) become U+FFFD, and the
// ends are trimmed, so the user and the model read the same visible text.
ParsedPlan ParsePlan(std::string_view json);

enum class PlanDecision {
  kUse,          // |plan| is valid
  kRetry,        // ask again, adding |message| to the prompt
  kWithoutPlan,  // go on without a plan; |message| is for the user
  kStop,         // end the run with |message| (user-facing)
};

struct PlanOutcome {
  PlanOutcome();
  ~PlanOutcome();
  PlanOutcome(PlanOutcome&&);
  PlanOutcome& operator=(PlanOutcome&&);

  PlanDecision decision = PlanDecision::kStop;
  Plan plan;
  std::string message;
};

// What to do with a plan reply. |bad_replies| counts the unusable replies
// before this one for this plan. A cut-off or malformed reply, or one
// ParsePlan() refuses, is asked for again until kMaxPlanAttempts; then the
// run goes on without a plan (a plan only helps; without one the agent is
// what it was before the planner). A cancelled request, an unsupported
// constraint (the schema is ours: a bug or a misbehaving service) and a
// failed generation stop the run, as for an action.
PlanOutcome InterpretPlanReply(const agent_actions::ModelReply& reply,
                               int bad_replies);

// The plan for a prompt, one line per step:
//   1. Search for flights to Tokyo (done when: the results list flights)
// Model text: agent_prompt fences it.
std::string DescribePlan(const Plan& plan);

enum class ReplanReason {
  kNone,
  kAsked,           // the model chose REPLAN
  kFailures,        // kMaxFailuresInARow failed actions in a row
  kTooManyActions,  // kMaxActionsPerStep actions on one step
};

// The browser's words for why the plan at |step| of |count| (1-based) is
// rewritten, for the planning prompt ("2 actions in a row failed at step 2
// of 4"). Empty for kNone.
std::string ReplanReasonText(ReplanReason reason, int step, int count);

// The run stops with this when its plan fails after kMaxReplans rewrites.
inline constexpr char kReplanLimitMessage[] =
    "The agent stopped: its plan failed again after it had been rewritten "
    "twice. Try a simpler task or a larger model.";

// The run's plan and how far it got. Changed only by the browser: the
// model's "step" (checked by ParseAction against Steps()) and each action's
// outcome.
class PlanTracker {
 public:
  PlanTracker();
  ~PlanTracker();
  PlanTracker(const PlanTracker&);
  PlanTracker& operator=(const PlanTracker&);

  // Follows |plan| from its first step (the first plan or a new one).
  void Adopt(Plan plan);
  // No plan: the run goes on one action at a time.
  void Drop();

  bool has_plan() const { return !plan_.empty(); }
  const Plan& plan() const { return plan_; }
  // The current step, 1-based; 0 without a plan.
  int current_step() const;
  // The rewrites used so far (UseReplan()).
  int replans() const { return replans_; }

  // What the next action's "step" may be. Only with has_plan().
  agent_actions::PlanSteps Steps() const;

  // The model's "step" for the action it chose: the next step marks the
  // current one done and makes it current. Anything else changes nothing.
  // Returns whether the current step changed.
  bool Report(int step);

  // One of the plan's actions finished (|ok|: it worked): why the plan
  // must be rewritten now (kFailures, kTooManyActions), or kNone. Without a
  // plan, always kNone.
  ReplanReason AfterAction(bool ok);

  // Uses one of the run's kMaxReplans rewrites; false, changing nothing,
  // when none is left.
  bool UseReplan();

  // The browser's line about the current step, for the prompt:
  //   CURRENT STEP: 2 of 4. Write "step": 2 while you work on it, or 3
  //   once it is done.
  // Empty without a plan.
  std::string DescribeProgress() const;

 private:
  Plan plan_;
  size_t current_ = 0;  // index into |plan_|
  int actions_in_step_ = 0;
  int failures_in_a_row_ = 0;
  int replans_ = 0;
};

}  // namespace agent_plan
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_PLAN_H_
