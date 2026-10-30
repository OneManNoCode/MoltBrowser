// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's planner fallback (docs/design/TOUCHLESS_BROWSING.md §6.2,
// layer 3): what an utterance the direct-command grammar has no command for
// goes to ("find a train to Lyon tomorrow morning"). The web agent's
// on-device planner implements it (agents/voice_agent_planner.h); a stub
// does in tests.
//
// The voice loop (voice_controller.h) owns the planner and is its
// Delegate:
//   - what the planner says is spoken through the loop, cleaned with
//     SpeechSafeText() (model output and page text are in it);
//   - its questions are asked through the loop's confirmation rule: "Say
//     yes to <what>, or no to cancel.", answered only by a "yes" or "no"
//     heard in that context, on the same page, within the loop's timeout;
//   - anything else the user says, "stop" included, cancels the run;
//   - each step it carried out is reported (PlannerCarriedOut()), so a
//     routine taught by voice keeps it (§6.8, voice_routines_logic.h);
//   - a routine's step it carried out before is replayed by it (Replay()),
//     without the model: the recorded step runs, or the routine stops.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PLANNER_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PLANNER_H_

#include <string>

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {
namespace voice {

struct PageMap;

// A step the planner carried out: it ran and worked (not one it only
// proposed, nor one the user declined), as the web agent saw it. Every
// string is untrusted: |label| and |option| are page text, |text| and
// |query| the model's. Std-only; voice_routines.h checks and keeps it
// (RoutineStepForPlanner()), and reads it back as untrusted.
struct PlannerStep {
  PlannerStep();
  ~PlannerStep();
  PlannerStep(const PlannerStep&);
  PlannerStep& operator=(const PlannerStep&);
  PlannerStep(PlannerStep&&);
  PlannerStep& operator=(PlannerStep&&);

  enum class Action {
    kNone,
    kNavigate,  // |url|: the page it opened
    kSearch,    // |url|: the results page; |query|: what it searched for
    kClick,     // the element
    kType,      // the element and |text|
    kSelect,    // the element (a select) and |option|
    kScroll,    // |direction|
    // A step that worked but that a routine can't repeat (a site's own
    // tool, a form filled only in part): |why| (the browser's words).
    kUnkept,
  };

  Action action = Action::kNone;
  std::string url;
  std::string query;
  // The http(s) page acted on (kClick, kType, kSelect, kScroll): its host;
  // "" when the agent didn't say (the loop takes the active tab's).
  std::string host;
  // kClick, kType, kSelect: the element, as the agent's observation named
  // it: its kind ("link", "button", "input", ...), its name (page text,
  // cleaned and capped as the observation shows it, so the replay finds it
  // again by the same name), and an <input>'s type.
  std::string kind;
  std::string label;
  std::string input_type;
  // kType: the field is a password, payment card or one-time code field.
  bool credential_field = false;
  // kType: what was typed. kSelect: the option chosen.
  std::string text;
  std::string option;
  // kScroll: "down" or "up".
  std::string direction;
  std::string why;
};

// How a replayed step ended (Delegate::PlannerReplayed()).
enum class PlannerReplay {
  kDone,       // it ran
  kNotFound,   // no element of that kind and name on the page
  kAmbiguous,  // more than one
  kDeclined,   // the user said no to its question
  kFailed,     // the page changed, a check refused it, it didn't work
};

class VoicePlanner {
 public:
  class Delegate {
   public:
    // Says |text| (cleaned and capped by the loop).
    virtual void PlannerSays(const std::string& text) = 0;
    // Asks the user to confirm |what| ("open evil.example"); the answer
    // comes back as VoicePlanner::Answer(|question|, yes).
    virtual void PlannerAsks(int question, const std::string& what) = 0;
    // The run is over: done, failed, declined or unable to start. Nothing
    // more comes from it.
    virtual void PlannerFinished() = 0;
    // Start()'s run carried out |step| (it ran and worked), in order.
    virtual void PlannerCarriedOut(const PlannerStep& step) {}
    // Replay()'s step ended so, right before PlannerFinished().
    virtual void PlannerReplayed(PlannerReplay outcome) {}

   protected:
    virtual ~Delegate() = default;
  };

  virtual ~VoicePlanner() = default;

  // Starts a run for |heard| (what the user said, which is the run's goal)
  // in |contents| (the window's active tab; may be null), with |map| the
  // page map the utterance was planned on. |delegate| outlives the run.
  virtual void Start(content::WebContents* contents,
                     const std::string& heard,
                     const PageMap& map,
                     Delegate* delegate) = 0;
  // Replays |step|, a step a run carried out before (a routine's), in
  // |contents|, without the model: on the page as it is now, with every
  // check a run's own step gets, and its questions asked through the
  // delegate. It says nothing else. The delegate hears PlannerReplayed(),
  // then PlannerFinished(), maybe at once. |delegate| outlives the run.
  virtual void Replay(content::WebContents* contents,
                      const PlannerStep& step,
                      Delegate* delegate) = 0;
  // The user's answer to question |question|; stale ones are ignored.
  virtual void Answer(int question, bool yes) = 0;
  // Ends the run at once; the delegate hears nothing more from it.
  virtual void Cancel() = 0;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PLANNER_H_
