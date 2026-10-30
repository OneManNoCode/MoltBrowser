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
//   "voice": the command, only the fields its kind uses (below);
//   "host":  the page it was said on.
// A step the on-device planner carried out is a recorded agent step, as an
// agent draft keeps it (agent_run_draft.h): navigate (its address as the
// target; a search's results page, with the query), click, type, a choice
// in a select (click) or scroll, the page's host, and in "voice" the
// action ("agent": "navigate", "search", "click", "type", "select",
// "scroll"), what the web agent knew of the element (its kind and name,
// page text, cleaned and capped as the agent's observation shows it: the
// replay finds it again by them), the text typed or the option chosen,
// and "planned": true. No selector, no page text but the element's name
// and the option, nothing the model said but the text typed and the query.
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
// no sites, sites_unconfirmed, until the user confirms its sites, with the
// voice gesture (VoiceController::ConfirmDraftSitesByVoice()) or a real
// click in the Agent studio (AutomationSecurity::ConfirmDraftSites(), on
// exactly the sites its steps use). The confirmation holds for those
// sites: once the steps use others (a step added or changed in the file),
// the routine needs its sites confirmed again (RoutineSitesConfirmed()).
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

// A page address a routine keeps, at most; the planner's (which the web
// agent opens again, under its own cap), at most.
inline constexpr size_t kMaxRoutineUrlBytes = 2048;
inline constexpr size_t kMaxPlannerUrlBytes = 1024;
// The planner's step: an element's name and a select's option, in code
// points (the agent's observation's caps), the text it typed and its
// search, in code points (the agent's).
inline constexpr size_t kMaxPlannerLabelChars = 60;
inline constexpr size_t kMaxPlannerOptionChars = 80;
inline constexpr size_t kMaxPlannerTextChars = 500;
inline constexpr size_t kMaxPlannerQueryChars = 256;
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

// Whether a routine's |sites| (AutomationSecurity::HostsReferencedBy())
// can be confirmed by voice: the prompt that asks (the voice gesture's,
// VoiceBrowserPrompts::ConfirmDraftSites()) says every one of them, each
// in full as the browser says a site, within what a prompt says of its
// text. Otherwise they're confirmed in the Agent studio, where they're
// shown: the word confirms them all, so the user must have heard them all.
bool RoutineSitesSayable(const std::vector<std::string>& sites);

// The routine step for |step|, a step the planner carried out for the
// request |goal| (what the user said) on a page of |page_host| (the active
// tab's, for a step whose own host the agent didn't say): checked and
// cleaned as an agent draft keeps a step (agent_run_draft.h), with typed
// text that looks secret (by the field's name, the text, or because the
// goal gave it as one) withheld, to be asked for at each run. A step a
// routine can't repeat faithfully is a gap (IsUnkeptStep()) saying why.
RoutineStep RoutineStepForPlanner(const PlannerStep& step,
                                  std::string_view goal,
                                  std::string_view page_host);
// A gap: a step of the planner's that "this" can't keep, for |why| (the
// browser's words, said after "I can't remember this: ").
RoutineStep UnkeptPlannerStep(std::string why);

// Whether a routine with the planner's steps may run: its sites were
// confirmed (not sites_unconfirmed), and they are exactly the sites its
// steps use now (AutomationSecurity::HostsReferencedBy(), as sets). A
// routine of the user's own steps only needs the first.
bool RoutineSitesConfirmed(const automation::Script& script);
// A routine with the planner's steps whose steps no longer use exactly the
// sites confirmed: made a draft again (sites_unconfirmed, no sites,
// Casual), so it is confirmed again before it runs. Returns whether it
// changed |script| (the caller saves it).
bool UnconfirmChangedRoutine(automation::Script& script);

// The extra["voice"] of |step|'s command (the wire format), and back
// (nullopt for anything invalid). Exposed for the tests and the fuzzer.
base::DictValue VoiceCommandToDict(const RoutineStep& step);
std::optional<RoutineStep> VoiceCommandFromDict(const base::DictValue& dict);
// The workflow step for a planner's |step| (IsAgentStep()), and the step
// read back from one (nullopt for anything invalid).
automation::Step PlannerStepToWorkflowStep(const RoutineStep& step);
std::optional<RoutineStep> PlannerStepFromWorkflowStep(
    const automation::Step& step);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_ROUTINES_H_
