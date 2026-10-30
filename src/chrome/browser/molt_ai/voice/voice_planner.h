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
//   - anything else the user says, "stop" included, cancels the run.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PLANNER_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PLANNER_H_

#include <string>

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {
namespace voice {

struct PageMap;

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
  // The user's answer to question |question|; stale ones are ignored.
  virtual void Answer(int question, bool yes) = 0;
  // Ends the run at once; the delegate hears nothing more from it.
  virtual void Cancel() = 0;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PLANNER_H_
