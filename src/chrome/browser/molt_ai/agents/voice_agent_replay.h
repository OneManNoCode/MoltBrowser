// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The on-device planner's steps in routines taught by voice
// (docs/design/TOUCHLESS_BROWSING.md §6.8), on the web agent's side:
//   - what a step the planner carried out is for a routine
//     (PlannerStepsFor()): the agent's record of it, as the AI chat records
//     a run for a draft (agent_run_draft.h's RunStep), in the voice side's
//     terms; voice_routines.h checks and keeps it;
//   - a routine's step replayed (agents/voice_agent_planner.h): the reply the
//     model would have written for it on the page as it is now
//     (PickReplayAction()). The web agent checks that reply as any model's
//     (agent_actions::ParseAction()) and carries it out with every check a
//     run's own step gets. The element is found again by its kind and name,
//     exactly as the observation shows them; when no element, or more than
//     one, has them, nothing is guessed: the replay stops.
// Pure logic (base, url; no content/): molt_ai_unittests links it.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_VOICE_AGENT_REPLAY_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_VOICE_AGENT_REPLAY_H_

#include <string>
#include <vector>

#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/automation/agent_run_draft.h"
#include "chrome/browser/molt_ai/voice/voice_planner.h"

namespace molt_ai {
namespace voice_agent {

// What a routine keeps of |step|, one of the agent's steps: nothing for a
// step that failed or changed no page (OBSERVE, REPLAN, DONE, ERROR); one
// step for NAVIGATE, SEARCH, CLICK, TYPE, SELECT and SCROLL; one per field
// for FILL_FORM; and a gap (PlannerStep::Action::kUnkept, saying why) for
// a site's tool (CALL_TOOL), a form filled only in part, or an element
// step the agent kept no element for.
std::vector<voice::PlannerStep> PlannerStepsFor(
    const automation::agent_draft::RunStep& step);

// The reply that replays |step| on |observation|.
struct ReplayPick {
  enum class Outcome {
    kAct,        // |reply| is the step's action
    kNotFound,   // no element of the step's kind and name (and a select's
                 // option, a text field for typing): |reply| is an ERROR
    kAmbiguous,  // more than one: an ERROR
  };
  Outcome outcome = Outcome::kNotFound;
  // One JSON object, as the model writes one (agent_actions.h).
  std::string reply;
};
ReplayPick PickReplayAction(const voice::PlannerStep& step,
                            const agent_actions::Observation& observation);

// The reply that ends a replay once its step has run: DONE, or ERROR when
// the step didn't work.
std::string EndReplayReply(bool done);

}  // namespace voice_agent
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_VOICE_AGENT_REPLAY_H_
