// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/voice_agent_planner.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/molt_ai/agents/frame_tools.h"
#include "chrome/browser/molt_ai/automation/agent_run_draft.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "content/public/browser/web_contents.h"

namespace molt_ai {

namespace {

// The agent's record of a step, in the terms a draft reads it
// (molt_ai_chat_handler_agent_run.cc records a chat's run the same way):
// nothing the model wrote about it, no page text but the element's name.
automation::agent_draft::RunStep ToRunStep(const WebAgent::Step& step) {
  automation::agent_draft::RunStep out;
  out.action = step.action;
  out.success = step.success;
  out.url = step.url;
  if (step.action == "SEARCH") {
    out.query = step.target;
  } else if (step.action == "SCROLL") {
    out.direction = step.target;
  }
  out.origin = step.origin;
  for (const WebAgent::Step::Element& e : step.elements) {
    automation::agent_draft::RunElement element;
    element.kind = e.kind;
    element.input_type = e.input_type;
    element.label = e.label;
    element.field_kind = e.field_kind;
    element.text = e.text;
    out.elements.push_back(std::move(element));
  }
  return out;
}

}  // namespace

VoiceAgentPlanner::VoiceAgentPlanner() = default;

VoiceAgentPlanner::~VoiceAgentPlanner() {
  if (agent_) {
    agent_->Cancel();
  }
}

// static
VoiceAgentPlanner::ModelCheck VoiceAgentPlanner::EnsureModel() {
  ModelCheck check;
  BrowserAIRuntime& runtime = BrowserAIRuntime::GetInstance();
  const std::string unsupported = runtime.GetLocalInferenceUnsupportedReason();
  if (!unsupported.empty()) {
    check.reason = voice_agent::NoPlanner::kUnsupported;
    check.detail = unsupported;
    return check;
  }
  if (!runtime.GetLoadedModelId().empty()) {
    check.ready = true;
    return check;
  }
  // The model pages would get (web_ai_policy.h): with none loaded, the
  // smallest one downloaded.
  runtime.RefreshModelStatus();
  std::vector<web_ai::ModelCandidate> candidates;
  for (const ModelInfo& info : runtime.GetAvailableModels()) {
    web_ai::ModelCandidate candidate;
    candidate.model_id = info.model_id;
    candidate.downloaded = info.is_downloaded;
    candidate.ram_required_bytes = info.ram_required_bytes;
    candidates.push_back(std::move(candidate));
  }
  const std::string model_id = web_ai::PickWebModel(
      /*loaded_model_id=*/std::string(), /*default_model_id=*/std::string(),
      candidates);
  if (model_id.empty()) {
    check.reason = voice_agent::NoPlanner::kNoModel;
    return check;
  }
  std::string error;
  if (!runtime.LoadModel(model_id, &error)) {
    check.reason = voice_agent::NoPlanner::kLoadFailed;
    check.detail = error;
    return check;
  }
  check.ready = true;
  return check;
}

void VoiceAgentPlanner::Start(content::WebContents* contents,
                              const std::string& heard,
                              const voice::PageMap& map,
                              Delegate* delegate) {
  Cancel();
  delegate_ = delegate;
  const int run = run_;
  // What the user said is the goal (the agent's trusted GOAL line): one
  // line, without control or invisible characters.
  std::string goal = voice::SpeechSafeText(heard, kMaxGoalChars);
  if (!contents || goal.empty()) {
    Say(voice_agent::NoPlannerSpeech(voice_agent::NoPlanner::kNoPage,
                                     std::string()));
    Finish();
    return;
  }
  // The map the utterance was planned on is the page the agent starts
  // from; the agent maps it again before it acts (the model may take a
  // while to load, and the page can change meanwhile).
  DVLOG(1) << "[VoiceAgentPlanner] Start on a page of " << map.items.size()
           << " item(s)";
  Say(voice_agent::StartSpeech());

  if (WebAgent::HasModelForTesting()) {
    ModelCheck ready;
    ready.ready = true;
    OnModelChecked(run, contents->GetWeakPtr(), std::move(goal),
                   std::move(ready));
    return;
  }
  // The runtime is the process's (as the chat's): initialized on the UI
  // thread, the model loaded on a worker.
  BrowserAIRuntime& runtime = BrowserAIRuntime::GetInstance();
  runtime.Initialize();
  runtime.StartMemoryPressureMonitoring();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&VoiceAgentPlanner::EnsureModel),
      base::BindOnce(&VoiceAgentPlanner::OnModelChecked,
                     weak_factory_.GetWeakPtr(), run, contents->GetWeakPtr(),
                     std::move(goal)));
}

void VoiceAgentPlanner::OnModelChecked(
    int run,
    base::WeakPtr<content::WebContents> contents,
    std::string goal,
    ModelCheck check) {
  if (run != run_ || !delegate_) {
    return;
  }
  if (!check.ready) {
    Say(voice_agent::NoPlannerSpeech(check.reason, check.detail));
    Finish();
    return;
  }
  if (!contents) {
    Say(voice_agent::NoPlannerSpeech(voice_agent::NoPlanner::kNoPage,
                                     std::string()));
    Finish();
    return;
  }
  agent_ = std::make_unique<WebAgent>(contents.get(),
                                      &BrowserAIRuntime::GetInstance());
  agent_->SetActionCallback(base::BindRepeating(&VoiceAgentPlanner::OnAction,
                                                weak_factory_.GetWeakPtr()));
  agent_->SetUsePageMap(true);
  agent_->Start(goal,
                base::BindRepeating(&VoiceAgentPlanner::OnStep,
                                    weak_factory_.GetWeakPtr()),
                base::BindOnce(&VoiceAgentPlanner::OnDone,
                               weak_factory_.GetWeakPtr(), run),
                base::BindRepeating(&VoiceAgentPlanner::OnConfirm,
                                    weak_factory_.GetWeakPtr()),
                base::BindRepeating(&VoiceAgentPlanner::OnPlan,
                                    weak_factory_.GetWeakPtr()));
}

void VoiceAgentPlanner::OnAction(const WebAgent::Step& step) {
  // With a plan its steps are said instead (OnPlan()).
  if (has_plan_) {
    return;
  }
  Say(voice_agent::ActionSpeech(step.action, step.target, step.value));
}

void VoiceAgentPlanner::OnStep(const WebAgent::Step& step) {
  if (replaying_) {
    return;
  }
  const base::WeakPtr<VoiceAgentPlanner> self = weak_factory_.GetWeakPtr();
  for (const voice::PlannerStep& carried :
       voice_agent::PlannerStepsFor(ToRunStep(step))) {
    if (!self || !delegate_) {
      return;
    }
    delegate_->PlannerCarriedOut(carried);
  }
}

void VoiceAgentPlanner::Replay(content::WebContents* contents,
                               const voice::PlannerStep& step,
                               Delegate* delegate) {
  Cancel();
  delegate_ = delegate;
  replaying_ = true;
  replay_step_ = step;
  if (!contents) {
    EndReplaySoon(voice::PlannerReplay::kFailed);
    return;
  }
  const int run = run_;
  agent_ =
      std::make_unique<WebAgent>(contents, &BrowserAIRuntime::GetInstance());
  agent_->SetUsePageMap(true);
  agent_->SetReplayPicker(base::BindRepeating(&VoiceAgentPlanner::PickReplay,
                                              weak_factory_.GetWeakPtr()));
  // The page a step opens is the run's to open (an address in the goal is
  // in its scope, as one the user said); any other step acts on the page
  // the tab is on, which the run starts on.
  const std::string goal =
      step.action == voice::PlannerStep::Action::kNavigate ||
              step.action == voice::PlannerStep::Action::kSearch
          ? step.url
          : std::string("Replay a step of a routine on this page");
  agent_->Start(goal,
                base::BindRepeating(&VoiceAgentPlanner::OnReplayStep,
                                    weak_factory_.GetWeakPtr()),
                base::BindOnce(&VoiceAgentPlanner::OnReplayDone,
                               weak_factory_.GetWeakPtr(), run),
                base::BindRepeating(&VoiceAgentPlanner::OnConfirm,
                                    weak_factory_.GetWeakPtr()),
                WebAgent::PlanCallback());
}

std::string VoiceAgentPlanner::PickReplay(
    const agent_actions::Observation& observation) {
  if (replay_picked_) {
    // The step has run, or failed: the run ends.
    return voice_agent::EndReplayReply(replay_ran_);
  }
  replay_picked_ = true;
  voice_agent::ReplayPick pick =
      voice_agent::PickReplayAction(replay_step_, observation);
  switch (pick.outcome) {
    case voice_agent::ReplayPick::Outcome::kAct:
      replay_outcome_ = voice::PlannerReplay::kFailed;
      break;
    case voice_agent::ReplayPick::Outcome::kNotFound:
      replay_outcome_ = voice::PlannerReplay::kNotFound;
      break;
    case voice_agent::ReplayPick::Outcome::kAmbiguous:
      replay_outcome_ = voice::PlannerReplay::kAmbiguous;
      break;
  }
  return std::move(pick.reply);
}

void VoiceAgentPlanner::OnReplayStep(const WebAgent::Step& step) {
  replay_ran_ = step.success;
}

void VoiceAgentPlanner::OnReplayDone(int run,
                                     bool success,
                                     std::string result) {
  if (run != run_) {
    return;
  }
  Delegate* delegate = delegate_;
  const voice::PlannerReplay outcome = success ? voice::PlannerReplay::kDone
                                       : declined_
                                           ? voice::PlannerReplay::kDeclined
                                           : replay_outcome_;
  // The delegate hears the outcome, then that the run is over.
  const base::WeakPtr<VoiceAgentPlanner> self = weak_factory_.GetWeakPtr();
  if (delegate) {
    delegate->PlannerReplayed(outcome);
  }
  if (self) {
    Finish();
  }
}

void VoiceAgentPlanner::EndReplaySoon(voice::PlannerReplay outcome) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<VoiceAgentPlanner> self, int run,
                        voice::PlannerReplay outcome) {
                       if (!self || self->run_ != run || !self->delegate_) {
                         return;
                       }
                       self->delegate_->PlannerReplayed(outcome);
                       if (self) {
                         self->Finish();
                       }
                     },
                     weak_factory_.GetWeakPtr(), run_, outcome));
}

void VoiceAgentPlanner::OnPlan(const WebAgent::PlanUpdate& update) {
  const int count = static_cast<int>(update.steps.size());
  const int current = update.current;
  const std::string goal = current >= 1 && current <= count
                               ? update.steps[current - 1].goal
                               : std::string();
  switch (update.event) {
    case WebAgent::PlanEvent::kPlanned:
      has_plan_ = count > 0;
      Say(voice_agent::PlanStepSpeech(goal, current, count));
      return;
    case WebAgent::PlanEvent::kAdvanced:
      Say(voice_agent::PlanStepSpeech(goal, current, count));
      return;
    case WebAgent::PlanEvent::kReplanned:
      has_plan_ = count > 0;
      Say(voice_agent::ReplanSpeech(goal));
      return;
    case WebAgent::PlanEvent::kWithoutPlan:
      // One action at a time: each is said before it runs.
      has_plan_ = false;
      return;
  }
}

void VoiceAgentPlanner::OnConfirm(const WebAgent::ConfirmRequest& request) {
  if (!agent_ || !delegate_) {
    return;
  }
  if (request.action == "PAGE_REQUEST") {
    // The activation guard's question: voice mode asks those itself when it
    // is on the tab; here it isn't, so the page's request is refused.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<VoiceAgentPlanner> self, int run, int id) {
              if (self && self->run_ == run && self->agent_) {
                self->agent_->ResolveConfirmation(id, false);
              }
            },
            weak_factory_.GetWeakPtr(), run_, request.id));
    return;
  }
  if (request.action == "FRAME_TOOLS") {
    // Another site's frame offering tools: "Not now", never asked by voice
    // (the run goes on without them).
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<VoiceAgentPlanner> self, int run, int id) {
              if (self && self->run_ == run && self->agent_) {
                self->agent_->ResolveFrameTools(id, frame_tools::Grant::kNone);
              }
            },
            weak_factory_.GetWeakPtr(), run_, request.id));
    return;
  }
  question_ = request.id;
  delegate_->PlannerAsks(request.id,
                         voice_agent::QuestionWhat(request.message));
}

void VoiceAgentPlanner::Answer(int question, bool yes) {
  if (!agent_ || question == 0 || question != question_) {
    return;
  }
  question_ = 0;
  declined_ = !yes;
  // A "no" stops the run (WebAgent: a denial ends it); the loop has said
  // "Okay, I won't."
  agent_->ResolveConfirmation(question, yes);
}

void VoiceAgentPlanner::OnDone(int run, bool success, std::string result) {
  if (run != run_) {
    return;
  }
  if (!declined_) {
    Say(voice_agent::ResultSpeech(success, result));
  }
  Finish();
}

void VoiceAgentPlanner::Cancel() {
  ++run_;
  weak_factory_.InvalidateWeakPtrs();
  if (agent_) {
    agent_->Cancel();
    // Not in its own callback's stack.
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(agent_));
  }
  delegate_ = nullptr;
  has_plan_ = false;
  question_ = 0;
  declined_ = false;
  ResetReplay();
}

void VoiceAgentPlanner::Say(const std::string& text) {
  if (delegate_ && !text.empty()) {
    delegate_->PlannerSays(text);
  }
}

void VoiceAgentPlanner::Finish() {
  Delegate* delegate = delegate_;
  ++run_;
  weak_factory_.InvalidateWeakPtrs();
  if (agent_) {
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(agent_));
  }
  delegate_ = nullptr;
  has_plan_ = false;
  question_ = 0;
  declined_ = false;
  ResetReplay();
  if (delegate) {
    delegate->PlannerFinished();
  }
}

void VoiceAgentPlanner::ResetReplay() {
  replaying_ = false;
  replay_step_ = voice::PlannerStep();
  replay_picked_ = false;
  replay_ran_ = false;
  replay_outcome_ = voice::PlannerReplay::kFailed;
}

std::unique_ptr<voice::VoicePlanner> CreateVoiceAgentPlanner() {
  return std::make_unique<VoiceAgentPlanner>();
}

}  // namespace molt_ai
