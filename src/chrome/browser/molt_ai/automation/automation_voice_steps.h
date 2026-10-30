// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Steps of a routine taught by voice (docs/design/TOUCHLESS_BROWSING.md
// §6.8; molt_ai/voice/voice_routines.h): an ordinary workflow step whose
// Step::extra["voice"] holds the voice command it replays. Voice mode runs
// such a routine through its own loop (the page map, the activation guard
// and its spoken confirmations), never through AutomationRunner, whose
// steps act through CSS selectors these steps don't have. So the runner,
// the scheduler and the Agent studio's Run and Schedule refuse a workflow
// with one, saying how to run it.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_VOICE_STEPS_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_VOICE_STEPS_H_

#include <algorithm>

#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"

namespace molt_ai {
namespace automation {

// Step::extra's key for the voice command a step replays.
inline constexpr char kVoiceStepKey[] = "voice";

// Why a routine taught by voice doesn't run here.
inline constexpr char kVoiceRoutineMessage[] =
    "This routine was taught by voice. Run it in voice mode: say \"run\" "
    "and its name.";

// Whether |step| replays a voice command.
inline bool IsVoiceStep(const Step& step) {
  return step.extra.is_dict() &&
         step.extra.GetDict().FindDict(kVoiceStepKey) != nullptr;
}

// Whether any step of |script| does.
inline bool HasVoiceSteps(const Script& script) {
  return std::any_of(script.steps.begin(), script.steps.end(), IsVoiceStep);
}

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_VOICE_STEPS_H_
