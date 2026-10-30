// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A routine taught by voice as an Agent-mode workflow, and back
// (docs/design/TOUCHLESS_BROWSING.md §6.8): "remember this as my news"
// saves the steps (voice_routines_logic.h) as a .molt file through
// AutomationStorage, so the routine is listed in the Agent studio with the
// other workflows, and "run my news" reads it back.
//
// Each step is an ordinary workflow step (navigate for opening a site, so
// the studio shows the site and AutomationSecurity::HostsReferencedBy()
// finds it; click for a choice; type for a search or typing; scroll; else
// "unknown"), with a description the studio shows ("Open YouTube") and
// Step::extra:
//   "voice": the command, only the fields its kind uses (below), and
//            "planned": true for the on-device planner's steps;
//   "host":  the page it was said on.
// The runner, the scheduler and the studio's Run refuse such a workflow
// (automation_voice_steps.h): voice mode runs it, through its own loop.
//
// What is stored, as the agent's drafts (agent_run_draft.h) store it:
//   - no secret: a search or typed text that looks like one (a password,
//     key, card number, one-time code: agent_draft::TextLooksSecret())
//     becomes a {{variable}} the browser asks for at each run; a page
//     address that looks like it holds one, or "{{" anywhere, refuses the
//     routine;
//   - no page text: the user's own words, the site's address, and for a
//     choice made by number the chosen item's name (as a draft keeps a
//     text anchor);
//   - nothing from MoltNet (voice mode is off there; the caller checks).
//
// Trust (automation_security.h): steps the user said start Approved on
// their own sites, as a recording does: the user did each of them, on those
// sites. A routine with the planner's steps keeps a draft's rules: Casual,
// no sites, sites_unconfirmed, until the user confirms its sites
// (voice_learning.h's ConfirmSites seam; today a real click in the Agent
// studio).
//
// ReadRoutine() reads a .molt as untrusted (it may have been imported or
// edited): every step must be a known voice command with valid, capped
// fields, or the routine isn't run by voice. Fuzzed by
// molt_voice_learning_fuzzer.
//
// base::Value and GURL only: unit-tested in molt_ai_unittests.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_ROUTINES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_ROUTINES_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/voice/voice_routines_logic.h"

namespace molt_ai {
namespace voice {

// A page address a routine keeps, at most.
inline constexpr size_t kMaxRoutineUrlBytes = 2048;
// Words of a choice made by name, at most (the matcher's own cap).
inline constexpr size_t kMaxRoutineChoiceWords = 12;

struct BuiltRoutine {
  BuiltRoutine();
  ~BuiltRoutine();
  BuiltRoutine(BuiltRoutine&&);
  BuiltRoutine& operator=(BuiltRoutine&&);

  // Set unless refused. Its id is |id| as given.
  std::optional<automation::Script> script;
  // Why it was refused (the browser's words).
  std::string refusal;
  // Values withheld, asked for at each run.
  int withheld = 0;
};

// The workflow for |steps|, named |name| (RoutineDisplayName()), with id
// |id|, made at |now_unix|. When the first step doesn't open a site, the
// workflow first opens the site it was said on, as a recording does.
BuiltRoutine BuildRoutine(std::string_view id,
                          std::string_view name,
                          const std::vector<RoutineStep>& steps,
                          int64_t now_unix);

struct ReadRoutineResult {
  ReadRoutineResult();
  ~ReadRoutineResult();
  ReadRoutineResult(ReadRoutineResult&&);
  ReadRoutineResult& operator=(ReadRoutineResult&&);

  // Every step valid: |steps| is the routine.
  bool ok = false;
  std::vector<RoutineStep> steps;
  // Some steps are the planner's, or the sites were never confirmed: it
  // runs only once they are.
  bool needs_sites_confirmed = false;
  // Why not (the browser's words).
  std::string why;
};

// The voice routine in |script|, read as untrusted.
ReadRoutineResult ReadRoutine(const automation::Script& script);

// The extra["voice"] of |step|'s command (the wire format), and back
// (nullopt for anything invalid). Exposed for the tests and the fuzzer.
base::DictValue VoiceCommandToDict(const RoutineStep& step);
std::optional<RoutineStep> VoiceCommandFromDict(const base::DictValue& dict);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_ROUTINES_H_
