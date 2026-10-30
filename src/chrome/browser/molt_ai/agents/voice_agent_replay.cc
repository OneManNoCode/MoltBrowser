// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/voice_agent_replay.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "base/json/json_writer.h"
#include "base/values.h"
#include "url/gurl.h"

namespace molt_ai {
namespace voice_agent {

namespace {

using agent_actions::ObservedElement;
using automation::agent_draft::RunElement;
using automation::agent_draft::RunStep;
using Action = voice::PlannerStep::Action;

// The fixed words a gap says (after "I can't remember this: ").
constexpr char kSiteTool[] =
    "the AI used a site's own tool, which a routine can't repeat.";
constexpr char kPartialForm[] = "the AI filled a form only in part.";
constexpr char kNoElement[] = "one of the AI's steps can't be repeated.";

// The reason every replayed action gives (the step trace shows it).
constexpr char kReason[] = "replaying a step of a routine";

voice::PlannerStep Gap(const char* why) {
  voice::PlannerStep step;
  step.action = Action::kUnkept;
  step.why = why;
  return step;
}

std::string HostOf(const std::string& origin) {
  const GURL url(origin);
  return url.is_valid() && url.SchemeIsHTTPOrHTTPS() ? std::string(url.host())
                                                     : std::string();
}

voice::PlannerStep ElementStep(Action action,
                               const RunElement& element,
                               const std::string& host) {
  voice::PlannerStep step;
  step.action = action;
  step.host = host;
  step.kind = element.kind;
  step.label = element.label;
  step.input_type = element.input_type;
  step.credential_field =
      element.field_kind != agent_safety::FieldKind::kOrdinary;
  if (action == Action::kType) {
    step.text = element.text;
  } else if (action == Action::kSelect) {
    step.option = element.text;
  }
  return step;
}

std::string ToJson(const base::DictValue& reply) {
  std::string json;
  base::JSONWriter::Write(reply, &json);
  return json;
}

base::DictValue NewReply(std::string_view name) {
  base::DictValue reply;
  reply.Set("action", name);
  reply.Set("reason", kReason);
  return reply;
}

ReplayPick Stop(ReplayPick::Outcome outcome) {
  ReplayPick pick;
  pick.outcome = outcome;
  base::DictValue reply = NewReply("ERROR");
  reply.Set("result", outcome == ReplayPick::Outcome::kAmbiguous
                          ? "More than one element on the page has the "
                            "step's name."
                          : "No element on the page has the step's name.");
  pick.reply = ToJson(reply);
  return pick;
}

}  // namespace

std::vector<voice::PlannerStep> PlannerStepsFor(const RunStep& step) {
  std::vector<voice::PlannerStep> out;
  if (!step.success) {
    return out;
  }
  const std::string& a = step.action;
  if (a == "NAVIGATE" || a == "SEARCH") {
    voice::PlannerStep navigate;
    navigate.action = a == "SEARCH" ? Action::kSearch : Action::kNavigate;
    navigate.url = step.url;
    navigate.query = step.query;
    out.push_back(std::move(navigate));
    return out;
  }
  if (a == "SCROLL") {
    voice::PlannerStep scroll;
    scroll.action = Action::kScroll;
    scroll.host = HostOf(step.origin);
    scroll.direction = step.direction;
    out.push_back(std::move(scroll));
    return out;
  }
  if (a == "CALL_TOOL") {
    out.push_back(Gap(kSiteTool));
    return out;
  }
  const std::string host = HostOf(step.origin);
  if (a == "CLICK" || a == "TYPE" || a == "SELECT") {
    if (step.elements.size() != 1) {
      out.push_back(Gap(kNoElement));
      return out;
    }
    out.push_back(ElementStep(a == "CLICK"  ? Action::kClick
                              : a == "TYPE" ? Action::kType
                                            : Action::kSelect,
                              step.elements[0], host));
    return out;
  }
  if (a == "FILL_FORM") {
    // The agent keeps no fields for a form it filled only in part.
    if (step.elements.empty()) {
      out.push_back(Gap(kPartialForm));
      return out;
    }
    for (const RunElement& element : step.elements) {
      out.push_back(ElementStep(Action::kType, element, host));
    }
    return out;
  }
  // OBSERVE, REPLAN, DONE, ERROR: no page changed.
  return out;
}

ReplayPick PickReplayAction(const voice::PlannerStep& step,
                            const agent_actions::Observation& observation) {
  ReplayPick pick;
  base::DictValue reply;
  switch (step.action) {
    case Action::kNavigate:
    case Action::kSearch:
      // The page it opened (a search's results page): the agent loads it
      // under its own checks, as any NAVIGATE.
      reply = NewReply("NAVIGATE");
      reply.Set("url", step.url);
      break;
    case Action::kScroll:
      reply = NewReply("SCROLL");
      reply.Set("direction", step.direction == "up" ? "up" : "down");
      break;
    case Action::kClick:
    case Action::kType:
    case Action::kSelect: {
      // The element of that kind and name, exactly as the observation
      // shows it; a text field to type into; a select that lists the
      // option.
      std::vector<const ObservedElement*> found;
      for (const ObservedElement& element : observation.elements) {
        if (element.kind != step.kind || element.label != step.label ||
            (step.action == Action::kType && !element.typeable) ||
            (step.action == Action::kSelect &&
             (!element.selectable ||
              std::find(element.options.begin(), element.options.end(),
                        step.option) == element.options.end()))) {
          continue;
        }
        found.push_back(&element);
      }
      if (found.size() != 1) {
        return Stop(found.empty() ? ReplayPick::Outcome::kNotFound
                                  : ReplayPick::Outcome::kAmbiguous);
      }
      if (step.action == Action::kClick) {
        reply = NewReply("CLICK");
      } else if (step.action == Action::kType) {
        reply = NewReply("TYPE");
        reply.Set("text", step.text);
      } else {
        reply = NewReply("SELECT");
        reply.Set("option", step.option);
      }
      reply.Set("target", found[0]->ref);
      break;
    }
    case Action::kNone:
    case Action::kUnkept:
      return Stop(ReplayPick::Outcome::kNotFound);
  }
  pick.outcome = ReplayPick::Outcome::kAct;
  pick.reply = ToJson(reply);
  return pick;
}

std::string EndReplayReply(bool done) {
  base::DictValue reply = NewReply(done ? "DONE" : "ERROR");
  reply.Set("result", done ? "The step ran." : "The step didn't work.");
  return ToJson(reply);
}

}  // namespace voice_agent
}  // namespace molt_ai
