// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's planner fallback through the web agent
// (docs/design/TOUCHLESS_BROWSING.md §6.2, docs/design/AGENT_ACTIONS.md
// §2.10): an open-ended request ("find a train to Lyon tomorrow morning")
// becomes a WebAgent run in the active tab, with what was heard as its
// goal, the on-device model as its planner, and the voice loop as its user
// interface:
//   - it says "Working on it.", then each plan step as it begins ("I'll
//     search the site for trains to Lyon."), or each action before it runs
//     when there is no plan, then the result (voice_agent_speech.h);
//   - the agent's questions (another site, a form submit, a site's tool)
//     are asked through the loop's confirmation and wait for a spoken
//     "yes"; a "no" stops the run; a frame of another site offering tools
//     is left out ("Not now"), never asked about by voice;
//   - the voice loop cancels the run on anything else the user says, and
//     on "stop";
//   - without a model, or when the model can't load, it says so plainly,
//     with what the user can do instead.
// The agent observes and acts through the page map, as voice mode does
// (outside MoltNet windows, where voice mode is off anyway); every check
// the agent makes still applies.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_VOICE_AGENT_PLANNER_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_VOICE_AGENT_PLANNER_H_

#include <cstddef>
#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/molt_ai/agents/voice_agent_speech.h"
#include "chrome/browser/molt_ai/agents/web_agent.h"
#include "chrome/browser/molt_ai/voice/voice_planner.h"

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {

class VoiceAgentPlanner : public voice::VoicePlanner {
 public:
  // The goal's cap, in code points (the grammar takes 1,000 bytes).
  static constexpr size_t kMaxGoalChars = 1000;

  VoiceAgentPlanner();
  VoiceAgentPlanner(const VoiceAgentPlanner&) = delete;
  VoiceAgentPlanner& operator=(const VoiceAgentPlanner&) = delete;
  ~VoiceAgentPlanner() override;

  // voice::VoicePlanner:
  void Start(content::WebContents* contents,
             const std::string& heard,
             const voice::PageMap& map,
             Delegate* delegate) override;
  void Answer(int question, bool yes) override;
  void Cancel() override;

 private:
  // Whether the on-device model is there (loaded, or loaded now), from a
  // worker.
  struct ModelCheck {
    bool ready = false;
    voice_agent::NoPlanner reason = voice_agent::NoPlanner::kNoModel;
    std::string detail;
  };
  static ModelCheck EnsureModel();

  void OnModelChecked(int run,
                      base::WeakPtr<content::WebContents> contents,
                      std::string goal,
                      ModelCheck check);
  void OnAction(const WebAgent::Step& step);
  void OnPlan(const WebAgent::PlanUpdate& update);
  void OnConfirm(const WebAgent::ConfirmRequest& request);
  void OnDone(int run, bool success, std::string result);
  void Say(const std::string& text);
  // The run is over: the agent goes (after its callback returns) and the
  // delegate hears it.
  void Finish();

  raw_ptr<Delegate> delegate_ = nullptr;
  std::unique_ptr<WebAgent> agent_;
  // Changes with every Start() and Cancel(): late answers are dropped.
  int run_ = 0;
  bool has_plan_ = false;
  int question_ = 0;
  bool declined_ = false;
  base::WeakPtrFactory<VoiceAgentPlanner> weak_factory_{this};
};

// The planner voice mode uses (voice_controller.h).
std::unique_ptr<voice::VoicePlanner> CreateVoiceAgentPlanner();

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_VOICE_AGENT_PLANNER_H_
