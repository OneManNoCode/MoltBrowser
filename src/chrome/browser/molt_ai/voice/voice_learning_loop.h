// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's learning in one window (docs/design/TOUCHLESS_BROWSING.md
// §6.8), beside its voice loop (voice_controller.h), which calls it at a
// few points:
//   - Environment(): what the loop's planner knows (VoiceEnvironment::
//     learning): the routines, what "remember this" would save, the learned
//     words, the site's hint.
//   - BeforePlanning(): a "no, I said ..." lined up with the utterance it
//     corrects (the corrected command is planned, and the misheard words
//     are learned); the corrections learned applied to a new utterance
//     (never while a yes-or-no question waits, never to make or unmake an
//     answer); the routine that runs kept going by the answer it waits for,
//     and ended by anything else.
//   - OnPlanned(), OnFollowUp(), OnStepFailed(), OnIdle(): what the user
//     carried out becomes "this" (voice_routines_logic.h); names said are
//     noted; the site's parts that worked become its hints, and a hint
//     whose part failed is dropped.
//   - OnPlannerCarriedOut(): a step the on-device planner carried out for
//     the user's request joins "this", in order with the user's own steps
//     (voice_routines.h's RoutineStepForPlanner()); one a routine can't
//     repeat is a gap, and while "Learn from what I say" is off every such
//     step is one.
//   - RunStep(): a kLearning step: save, run or forget a routine, forget a
//     word or everything.
//
// A routine runs through the loop's own steps, one command at a time:
// each step is said as it starts ("Step 2: search for news."), planned
// with PlanCommand() on the page as it is then, and run as any utterance's
// steps are, so every check stays: agent_safety on every address, the
// page controller's on every node, the activation guard on what the page
// does with the activation, and the questions before anything that can't
// be undone (the routine waits for the "yes"). Besides, a step acts only
// on the routine's own sites (AutomationSecurity::EffectivePolicy()). A
// step that fails or finds nothing to act on, a "no", 30 s without an
// answer, or anything else the user says ends it.
//
// A step the planner carried out is replayed by the planner
// (VoicePlanner::Replay(), through the web agent, without the model): said
// as it starts, on the routine's own sites, with the agent's checks and
// questions; the routine waits for it (OnPlannerReplayed()), and stops at
// it when its element isn't on the page ("I couldn't find Search on this
// page.").
//
// Nothing is learned in MoltNet windows or private ones, and nothing
// learned is used there (VoiceLearning::Get() is null), nor from what is
// typed into a field, nor from what is said while voice mode reads a page
// aloud (any reading of the page's text, voice_reader.h) or within
// kReadingGrace after.
//
// UI thread; owned by the VoiceController.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LEARNING_LOOP_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LEARNING_LOOP_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"
#include "chrome/browser/molt_ai/voice/voice_routines_logic.h"

namespace molt_ai {
namespace voice {

class RoutineSiteConfirmer;
class VoiceController;
class VoiceLearning;
struct RoutineToRun;

class VoiceLearningLoop {
 public:
  // What is said within this long after voice mode read a page aloud is
  // not learned from.
  static constexpr base::TimeDelta kReadingGrace = base::Seconds(3);
  // "No, I said ..." corrects the utterance before it within this long.
  static constexpr base::TimeDelta kCorrectionWindow = base::Minutes(2);

  explicit VoiceLearningLoop(VoiceController* controller);
  VoiceLearningLoop(const VoiceLearningLoop&) = delete;
  VoiceLearningLoop& operator=(const VoiceLearningLoop&) = delete;
  ~VoiceLearningLoop();

  // VoiceEnvironment::learning for the active tab's |host|.
  LearningEnvironment Environment(const std::string& host) const;

  // The utterance |utterance| the grammar parsed as |command| in |context|:
  // what to plan instead, or nullopt when the routine that runs took it
  // (the text it asked for).
  std::optional<VoiceCommand> BeforePlanning(const std::string& utterance,
                                             VoiceCommand command,
                                             const ParseContext& context);
  // The turn planned for the user's |command|, on |before|, |map| and
  // |env|.
  void OnPlanned(const VoiceCommand& command,
                 const VoiceTurn& turn,
                 const ConversationState& before,
                 const PageMap& map,
                 const VoiceEnvironment& env);
  // A follow-up turn, once a page loaded.
  void OnFollowUp(const VoiceTurn& turn);
  void OnStepFailed();
  // Nothing more runs for now.
  void OnIdle();
  void OnSpeakingChanged(bool speaking);
  // A kLearning step.
  void RunStep(const VoiceStep& step);
  // The planner's run carried out |step|; a routine's step the planner
  // replayed ended so.
  void OnPlannerCarriedOut(const PlannerStep& step);
  void OnPlannerReplayed(PlannerReplay outcome);
  // Voice mode stopped: the routine ends, unsaid.
  void Stop();

  bool routine_running() const { return playing_.has_value(); }

  // Tests: the site confirmer (voice_routine_sites.h).
  void SetSiteConfirmerForTesting(
      std::unique_ptr<RoutineSiteConfirmer> confirmer);

 private:
  struct Playing {
    Playing();
    ~Playing();
    Playing(Playing&&);
    Playing& operator=(Playing&&);

    std::string id;
    std::string name;
    std::vector<RoutineStep> steps;
    automation::SecurityPolicy policy;
    size_t next = 0;
    // The step asked a question, and waits for its "yes".
    bool waiting_answer = false;
    // The step needs a value that wasn't kept: "type ..." gives it.
    bool asking_value = false;
    std::map<std::string, std::string> values;
    // The step failed, or found nothing to act on; a "no" was said.
    bool failed = false;
    bool stop_after = false;
    bool scheduled = false;
    // The planner replays the step (it may be waiting for an answer).
    bool replaying = false;
    // Why the step failed, said before "I stopped ...".
    std::string why;
  };

  VoiceLearning* Learning() const;
  bool HeardWhileReading() const;
  // Says |text| (cleaned), after what is being said unless |interrupt|.
  void Say(const std::string& text, bool interrupt);

  std::optional<RoutineStep> StepFromTurn(const VoiceCommand& command,
                                          const VoiceTurn& turn,
                                          const ConversationState& before,
                                          const PageMap& map,
                                          const VoiceEnvironment& env) const;
  void NoteNames(const VoiceCommand& command, const VoiceTurn& turn);
  void CommitSiteParts();

  void SaveRoutine(const std::string& name, int version);
  // The routine called |name|, or the one with id |id| when that isn't
  // empty (|name| is then only said).
  void RunRoutine(const std::string& name, const std::string& id);
  void OnRoutineLoaded(std::string name, std::optional<RoutineToRun> routine);
  void StartRoutine(RoutineToRun routine);
  void ScheduleNextStep();
  void NextStep();
  void StopRoutine(const std::string& said);
  void StartAnswerTimer();
  void OnAnswerTimeout();
  // NextStep() for a step the planner carried out.
  void ReplayStep(size_t index, const std::string& said);

  raw_ptr<VoiceController> controller_;
  std::unique_ptr<RoutineSiteConfirmer> confirmer_;
  RoutineRecorder recorder_;
  // A step that waits for its "yes" before it counts.
  std::optional<RoutineStep> pending_;
  bool last_added_ = false;
  // The utterance before, for "no, I said ...".
  std::optional<std::string> previous_;
  base::TimeTicks previous_time_;
  bool previous_while_reading_ = false;
  bool heard_while_reading_ = false;
  // Voice mode reads a page aloud, or did until |reading_until_|.
  bool reading_ = false;
  base::TimeTicks reading_until_;
  // The site's parts the current utterance's turns used, and whether a
  // step failed since.
  std::vector<SitePartUse> parts_;
  bool failed_ = false;
  // The planner's run that the user's last request started: what was said
  // (its secrets are withheld from typed text), the page it began on, and
  // whether no step of it has been carried out yet.
  std::string planner_goal_;
  std::string planner_start_url_;
  bool planner_first_ = false;
  std::optional<Playing> playing_;
  base::OneShotTimer answer_timer_;
  base::WeakPtrFactory<VoiceLearningLoop> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LEARNING_LOOP_H_
