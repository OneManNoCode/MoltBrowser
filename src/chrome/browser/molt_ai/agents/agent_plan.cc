// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/agent_plan.h"

#include <algorithm>
#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/agents/hidden_chars.h"

namespace molt_ai {
namespace agent_plan {

namespace {

// The reply's nesting: object > "steps" list > step object > its strings.
// base::JSONReader counts every level, scalars included, so a deeper reply
// is refused by the reader itself.
constexpr size_t kMaxPlanJsonDepth = 4;

// Characters in valid UTF-8 (what base::JSONReader returns): every byte
// that isn't a continuation byte starts one.
size_t CountChars(std::string_view s) {
  size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
      ++n;
  }
  return n;
}

bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

// C0 controls (line breaks and tabs included) and DEL.
bool HasControlChars(std::string_view s) {
  return std::any_of(s.begin(), s.end(), [](char ch) {
    const unsigned char c = static_cast<unsigned char>(ch);
    return c < 0x20 || c == 0x7F;
  });
}

// |s| as the user and the model read it: characters that reorder, hide or
// break text (hidden_chars.h: C1 controls, bidi and zero-width characters,
// line separators, variation selectors, tag characters, ...) shown as
// U+FFFD (one character each, so the length in characters stays the same),
// ASCII spaces trimmed from the ends.
std::string Shown(std::string_view s) {
  const std::string out = hidden_chars::ShowHidden(s);
  const size_t begin = out.find_first_not_of(' ');
  if (begin == std::string::npos)
    return std::string();
  const size_t end = out.find_last_not_of(' ');
  return out.substr(begin, end - begin + 1);
}

ParsedPlan Fail(PlanError error) {
  ParsedPlan p;
  p.error = error;
  return p;
}

// Reads |key| of a step into |out|: a string of at most |max_chars|
// characters, without control characters, not blank.
PlanError ReadStepText(const base::DictValue& step,
                       std::string_view key,
                       size_t max_chars,
                       std::string* out) {
  const std::string* s = step.FindString(key);
  if (!s)
    return PlanError::kWrongKeys;
  if (CountChars(*s) > max_chars)
    return PlanError::kTooLong;
  if (HasControlChars(*s))
    return PlanError::kBadCharacters;
  if (std::all_of(s->begin(), s->end(), IsAsciiSpace))
    return PlanError::kEmpty;
  *out = Shown(*s);
  return PlanError::kNone;
}

constexpr char kCutOffNote[] =
    "Your previous plan was cut off before the JSON object was complete. "
    "Write fewer and shorter steps.";
constexpr char kNotJsonNote[] =
    "Your previous reply was not one complete JSON object in the required "
    "format.";
constexpr char kRetryTail[] =
    " Reply with one JSON object in the format above.";

}  // namespace

ParsedPlan::ParsedPlan() = default;
ParsedPlan::~ParsedPlan() = default;
ParsedPlan::ParsedPlan(ParsedPlan&&) = default;
ParsedPlan& ParsedPlan::operator=(ParsedPlan&&) = default;

PlanOutcome::PlanOutcome() = default;
PlanOutcome::~PlanOutcome() = default;
PlanOutcome::PlanOutcome(PlanOutcome&&) = default;
PlanOutcome& PlanOutcome::operator=(PlanOutcome&&) = default;

std::string BuildPlanSchema() {
  auto text = [](size_t max_chars) {
    return "{\"type\":\"string\",\"minLength\":1,\"maxLength\":" +
           base::NumberToString(max_chars) + "}";
  };
  return "{\"type\":\"object\",\"properties\":{\"steps\":{\"type\":\"array\","
         "\"items\":{\"type\":\"object\",\"properties\":{\"goal\":" +
         text(kMaxGoalChars) + ",\"done_when\":" + text(kMaxDoneWhenChars) +
         "},\"required\":[\"goal\",\"done_when\"],"
         "\"additionalProperties\":false},\"minItems\":1,\"maxItems\":" +
         base::NumberToString(kMaxPlanSteps) +
         "}},\"required\":[\"steps\"],\"additionalProperties\":false}";
}

std::string_view PlanErrorText(PlanError error) {
  switch (error) {
    case PlanError::kNone:
      return "no error";
    case PlanError::kNotJsonObject:
      return "it was not one JSON object";
    case PlanError::kWrongKeys:
      return "its keys or value types did not match the format";
    case PlanError::kStepCount:
      return "a plan has 1 to 6 steps";
    case PlanError::kTooLong:
      return "a goal or done_when was too long";
    case PlanError::kEmpty:
      return "a goal or done_when was empty";
    case PlanError::kBadCharacters:
      return "a goal or done_when contained line breaks or control "
             "characters";
  }
  return "";
}

ParsedPlan ParsePlan(std::string_view json) {
  static_assert(kMaxPlanSteps == 6, "update PlanErrorText(kStepCount)");
  std::optional<base::DictValue> dict = base::JSONReader::ReadDict(
      json, base::JSON_PARSE_RFC, kMaxPlanJsonDepth);
  if (!dict)
    return Fail(PlanError::kNotJsonObject);
  const base::ListValue* steps = dict->FindList("steps");
  if (!steps || dict->size() != 1)
    return Fail(PlanError::kWrongKeys);
  if (steps->empty() || steps->size() > kMaxPlanSteps)
    return Fail(PlanError::kStepCount);
  Plan plan;
  for (const base::Value& item : *steps) {
    if (!item.is_dict() || item.GetDict().size() != 2)
      return Fail(PlanError::kWrongKeys);
    PlanStep step;
    PlanError e =
        ReadStepText(item.GetDict(), "goal", kMaxGoalChars, &step.goal);
    if (e == PlanError::kNone) {
      e = ReadStepText(item.GetDict(), "done_when", kMaxDoneWhenChars,
                       &step.done_when);
    }
    if (e != PlanError::kNone)
      return Fail(e);
    plan.push_back(std::move(step));
  }
  ParsedPlan parsed;
  parsed.plan = std::move(plan);
  return parsed;
}

PlanOutcome InterpretPlanReply(const agent_actions::ModelReply& reply,
                               int bad_replies) {
  PlanOutcome out;
  if (reply.cancelled) {
    out.message = "Stopped.";
    return out;
  }
  if (reply.constraint_unsupported) {
    out.message =
        "The on-device model couldn't use the planner's format, so the agent "
        "stopped.";
    return out;
  }
  std::string note;
  if (reply.invalid_output) {
    note = reply.truncated ? kCutOffNote : kNotJsonNote;
  } else if (!reply.success) {
    out.message = reply.error_message.empty()
                      ? std::string("The on-device model didn't answer.")
                      : "The on-device model failed: " + reply.error_message;
    return out;
  } else {
    ParsedPlan parsed = ParsePlan(reply.text);
    if (parsed.plan) {
      out.decision = PlanDecision::kUse;
      out.plan = std::move(*parsed.plan);
      return out;
    }
    note = "Your previous plan was not usable: " +
           std::string(PlanErrorText(parsed.error)) + ".";
  }
  if (bad_replies + 1 >= kMaxPlanAttempts) {
    out.decision = PlanDecision::kWithoutPlan;
    out.message =
        "The model couldn't write a usable plan, so the agent goes on one "
        "step at a time.";
    return out;
  }
  out.decision = PlanDecision::kRetry;
  out.message = note + kRetryTail;
  return out;
}

std::string DescribePlan(const Plan& plan) {
  std::string out;
  for (size_t i = 0; i < plan.size(); ++i) {
    out += base::NumberToString(i + 1) + ". " + plan[i].goal +
           " (done when: " + plan[i].done_when + ")\n";
  }
  return out;
}

std::string ReplanReasonText(ReplanReason reason, int step, int count) {
  static_assert(kMaxFailuresInARow == 2 && kMaxActionsPerStep == 8,
                "update ReplanReasonText");
  const std::string where =
      base::NumberToString(step) + " of " + base::NumberToString(count);
  switch (reason) {
    case ReplanReason::kNone:
      return std::string();
    case ReplanReason::kAsked:
      return "at step " + where +
             " the agent found that the plan no longer fits the page";
    case ReplanReason::kFailures:
      return "2 actions in a row failed at step " + where;
    case ReplanReason::kTooManyActions:
      return "8 actions did not finish step " + where;
  }
  return std::string();
}

PlanTracker::PlanTracker() = default;
PlanTracker::~PlanTracker() = default;
PlanTracker::PlanTracker(const PlanTracker&) = default;
PlanTracker& PlanTracker::operator=(const PlanTracker&) = default;

void PlanTracker::Adopt(Plan plan) {
  plan_ = std::move(plan);
  if (plan_.size() > kMaxPlanSteps)
    plan_.resize(kMaxPlanSteps);
  current_ = 0;
  actions_in_step_ = 0;
  failures_in_a_row_ = 0;
}

void PlanTracker::Drop() {
  Adopt(Plan());
}

int PlanTracker::current_step() const {
  return has_plan() ? static_cast<int>(current_) + 1 : 0;
}

agent_actions::PlanSteps PlanTracker::Steps() const {
  agent_actions::PlanSteps steps;
  steps.current = current_step();
  steps.count = static_cast<int>(plan_.size());
  return steps;
}

bool PlanTracker::Report(int step) {
  if (!has_plan() || current_ + 1 >= plan_.size() ||
      step != current_step() + 1) {
    return false;
  }
  ++current_;
  actions_in_step_ = 0;
  failures_in_a_row_ = 0;
  return true;
}

ReplanReason PlanTracker::AfterAction(bool ok) {
  if (!has_plan())
    return ReplanReason::kNone;
  ++actions_in_step_;
  failures_in_a_row_ = ok ? 0 : failures_in_a_row_ + 1;
  if (failures_in_a_row_ >= kMaxFailuresInARow)
    return ReplanReason::kFailures;
  if (actions_in_step_ >= kMaxActionsPerStep)
    return ReplanReason::kTooManyActions;
  return ReplanReason::kNone;
}

bool PlanTracker::UseReplan() {
  if (replans_ >= kMaxReplans)
    return false;
  ++replans_;
  return true;
}

std::string PlanTracker::DescribeProgress() const {
  if (!has_plan())
    return std::string();
  const std::string step = base::NumberToString(current_step());
  const std::string count = base::NumberToString(plan_.size());
  if (current_ + 1 >= plan_.size()) {
    return "CURRENT STEP: " + step + " of " + count + ", the last. Write " +
           "\"step\": " + step + ".";
  }
  return "CURRENT STEP: " + step + " of " + count + ". Write \"step\": " +
         step + " while you work on it, or " +
         base::NumberToString(current_step() + 1) + " once it is done.";
}

}  // namespace agent_plan
}  // namespace molt_ai
