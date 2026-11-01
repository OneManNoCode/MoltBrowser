// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_learning_loop.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/spoken_answers.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"
#include "chrome/browser/molt_ai/voice/voice_learning.h"
#include "chrome/browser/molt_ai/voice/voice_reader.h"
#include "chrome/browser/molt_ai/voice/voice_routine_sites.h"
#include "chrome/browser/molt_ai/voice/voice_routines.h"
#include "chrome/browser/molt_ai/voice/voice_speech.h"
#include "chrome/browser/molt_ai/voice/voice_speech_text.h"
#include "chrome/browser/molt_ai/voice/voice_vocabulary.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace molt_ai {
namespace voice {

namespace {

using K = VoiceCommandKind;

// What the loop says, in code points.
constexpr size_t kMaxSaidChars = 400;

// Why the planner's steps can't be kept while learning is off.
constexpr std::string_view kLearningOff = SaidLater(
    "the AI's steps aren't kept while Learn from what I say is off.");

// Whether |step| is the planner's step that opens a page.
bool OpensPage(const RoutineStep& step) {
  return IsAgentStep(step) &&
         (step.agent.action == PlannerStep::Action::kNavigate ||
          step.agent.action == PlannerStep::Action::kSearch);
}

// Any reading of the page's text, "continue reading" included: a barge-in
// stops the reader before it is planned, so the reader can't tell then.
bool IsReadingKind(K kind) {
  return ReadsPageText(kind);
}

// Whether |turn| did what a command of |kind| asks: a step that acts, a
// question asked, or (for reading) something said.
bool Acted(K kind, const VoiceTurn& turn) {
  if (turn.state.confirming) {
    return true;
  }
  for (const VoiceStep& step : turn.steps) {
    if (step.kind != VoiceStepKind::kSpeak &&
        step.kind != VoiceStepKind::kSetCurrentItem) {
      return true;
    }
  }
  switch (kind) {
    case K::kReadPage:
    case K::kReadHeadings:
    case K::kReadLinks:
    case K::kReadResults:
    case K::kReadTitle:
    case K::kDescribeScreen:
      return !turn.steps.empty();
    default:
      return false;
  }
}

// The command acts on the page it is said on (not only opening one).
bool ActsOnPage(const VoiceCommand& command) {
  switch (command.kind) {
    case K::kOpenSite:
    case K::kHome:
      return false;
    case K::kSearch:
      return command.scope != SearchScope::kNamedSite &&
             command.scope != SearchScope::kDefaultEngine;
    default:
      return true;
  }
}

// What the user said |folded| (folded words) as, in |heard|, case kept;
// |folded| itself when it isn't there.
std::string CasedPhrase(const std::string& heard, const std::string& folded) {
  NormalizeOptions options;
  const NormalizedText text = NormalizeSpokenText(heard, options);
  std::vector<std::string> want;
  size_t start = 0;
  while (start < folded.size()) {
    size_t space = folded.find(' ', start);
    if (space == std::string::npos) {
      space = folded.size();
    }
    if (space > start) {
      want.push_back(folded.substr(start, space - start));
    }
    start = space + 1;
  }
  const size_t n = text.words.size();
  for (size_t i = 0; !want.empty() && i + want.size() <= n; ++i) {
    bool match = true;
    for (size_t j = 0; j < want.size() && match; ++j) {
      match = text.Word(i + j) == want[j];
    }
    if (match) {
      return SourceText(heard, text, i, i + want.size() - 1, {});
    }
  }
  return folded;
}

std::string JoinWords(const std::vector<std::string>& words) {
  std::string out;
  for (const std::string& word : words) {
    out += (out.empty() ? "" : " ") + word;
  }
  return out;
}

}  // namespace

VoiceLearningLoop::Playing::Playing() = default;
VoiceLearningLoop::Playing::~Playing() = default;
VoiceLearningLoop::Playing::Playing(Playing&&) = default;
VoiceLearningLoop::Playing& VoiceLearningLoop::Playing::operator=(Playing&&) =
    default;

VoiceLearningLoop::VoiceLearningLoop(VoiceController* controller)
    : controller_(controller), confirmer_(CreateRoutineSiteConfirmer()) {}

VoiceLearningLoop::~VoiceLearningLoop() = default;

void VoiceLearningLoop::SetSiteConfirmerForTesting(
    std::unique_ptr<RoutineSiteConfirmer> confirmer) {
  confirmer_ = std::move(confirmer);
}

VoiceLearning* VoiceLearningLoop::Learning() const {
  BrowserWindowInterface* browser = controller_->browser_;
  // Null for off-the-record profiles, MoltNet's included.
  return browser ? VoiceLearning::Get(browser->GetProfile()) : nullptr;
}

bool VoiceLearningLoop::HeardWhileReading() const {
  // The reader (voice_reader.h) reads between its chunks too, and after
  // "continue reading" or a heading's jump.
  return reading_ || controller_->reader_->reading() ||
         base::TimeTicks::Now() < reading_until_;
}

void VoiceLearningLoop::Say(const std::string& text, bool interrupt) {
  // A constant sentence in the window's language (the others come through
  // Said()).
  const std::string said =
      SpeechSafeText(Localized(controller_->language(), text), kMaxSaidChars);
  if (said.empty() || !controller_->speech_) {
    return;
  }
  for (VoiceController::Observer& observer : controller_->observers_) {
    observer.OnVoiceSpeech(said);
  }
  controller_->speech_->Speak(said, controller_->state_.speech_rate,
                              interrupt);
  controller_->SetStatus(said);
}

LearningEnvironment VoiceLearningLoop::Environment(
    const std::string& host) const {
  LearningEnvironment env;
  VoiceLearning* learning = Learning();
  if (!learning) {
    return env;
  }
  env.routines_available = true;
  env.enabled = learning->enabled();
  for (const RoutineSummary& routine : learning->routines()) {
    env.routines.push_back(routine.name);
  }
  for (const RoutineSummary& routine : learning->unclaimed()) {
    env.unclaimed.push_back(routine.name);
  }
  env.unclaimed_version = learning->unclaimed_version();
  for (const RoutineStep& step : recorder_.steps()) {
    env.this_steps.push_back(DescribeRoutineStep(step, controller_->language()));
  }
  env.this_planned = recorder_.planned();
  env.this_refusal = Relocalized(controller_->language(), recorder_.refusal());
  env.this_version = recorder_.version();
  for (const VocabularyEntry& entry : learning->vocabulary().entries()) {
    env.words.push_back(FoldForComparison(entry.phrase));
    if (entry.correction()) {
      env.words.push_back(entry.heard);
    }
  }
  if (env.enabled) {
    if (const SiteHint* hint = learning->hints().Find(host)) {
      env.hint = *hint;
    }
  }
  return env;
}

std::optional<VoiceCommand> VoiceLearningLoop::BeforePlanning(
    const std::string& utterance,
    VoiceCommand command,
    const ParseContext& context) {
  const base::TimeTicks now = base::TimeTicks::Now();
  heard_while_reading_ = HeardWhileReading();
  parts_.clear();
  failed_ = false;
  last_added_ = false;

  // The routine that runs: the answer it waits for keeps it going, the
  // text it asked for is taken, anything else ends it.
  if (playing_) {
    Playing& p = *playing_;
    if (p.asking_value && command.kind == K::kType && !command.text.empty() &&
        p.next < p.steps.size()) {
      p.values[p.steps[p.next].variable] = command.text;
      p.asking_value = false;
      answer_timer_.Stop();
      ScheduleNextStep();
      return std::nullopt;
    }
    // The routine's own question, or the planner's while it replays a
    // step.
    const bool asked = p.waiting_answer ||
                       (p.replaying && controller_->planner_question_ != 0);
    const bool answers = asked && context.expecting_yes_no &&
                         (command.kind == K::kYes || command.kind == K::kNo);
    if (!answers) {
      StopRoutine(std::string());
    } else {
      answer_timer_.Stop();
      p.waiting_answer = false;
      p.stop_after = command.kind == K::kNo;
    }
  }

  VoiceLearning* learning = Learning();
  const bool learn = learning && learning->enabled();

  // "No, I said ...": what is meant, in the utterance it corrects.
  if (command.kind == K::kCorrection) {
    if (previous_ && now - previous_time_ < kCorrectionWindow) {
      if (std::optional<AlignedCorrection> aligned =
              AlignCorrection(*previous_, command.text)) {
        VoiceCommand corrected =
            ParseVoiceCommand(aligned->corrected, context);
        if (corrected.kind != K::kYes && corrected.kind != K::kNo &&
            corrected.kind != K::kCorrection &&
            corrected.kind != K::kRefused) {
          if (learn && !heard_while_reading_ && !previous_while_reading_ &&
              !aligned->heard.empty()) {
            learning->NoteCorrection(aligned->heard, aligned->meant);
          }
          previous_ = aligned->corrected;
          previous_time_ = now;
          return corrected;
        }
      }
    }
    return command;
  }

  // The corrections learned, applied; never to an answer, nor while one
  // is asked for (the loop's question, or a browser prompt's).
  if (learn && !context.expecting_yes_no &&
      controller_->state_.prompt.id == 0 && command.kind != K::kYes &&
      command.kind != K::kNo) {
    std::vector<std::string> used;
    if (std::optional<std::string> applied =
            learning->vocabulary().ApplyCorrections(utterance, &used)) {
      VoiceCommand reparsed = ParseVoiceCommand(*applied, context);
      if (reparsed.kind != K::kYes && reparsed.kind != K::kNo &&
          reparsed.kind != K::kRefused) {
        for (const std::string& heard : used) {
          learning->NoteCorrectionUsed(heard);
        }
        command = std::move(reparsed);
      }
    }
  }

  switch (command.kind) {
    case K::kYes:
    case K::kNo:
    case K::kStop:
    case K::kStopSpeaking:
    case K::kCancel:
    case K::kRepeat:
    case K::kHelp:
    case K::kRefused:
      break;
    default:
      previous_ = command.heard;
      previous_time_ = now;
      previous_while_reading_ = heard_while_reading_;
      break;
  }
  return command;
}

std::optional<RoutineStep> VoiceLearningLoop::StepFromTurn(
    const VoiceCommand& command,
    const VoiceTurn& turn,
    const ConversationState& before,
    const PageMap& map,
    const VoiceEnvironment& env) const {
  int position = 0;
  std::string label;
  if (command.kind == K::kChoose) {
    if (turn.state.list == ListContext::kPageList) {
      position = turn.state.current_position;
    }
    for (const VoiceStep& step : turn.steps) {
      if (step.kind == VoiceStepKind::kSetCurrentItem && step.index > 0) {
        if (const PageItem* item = map.ItemByNumber(step.index)) {
          label = item->label;
        }
      }
    }
  }
  std::optional<RoutineStep> step =
      RoutineStepFor(command, env.web_page ? env.host : std::string(),
                     position, label, !before.offered.empty());
  if (!step) {
    return std::nullopt;
  }
  for (const VoiceStep& s : turn.steps) {
    if (s.kind == VoiceStepKind::kNavigate) {
      const GURL url(s.url);
      if (command.kind == K::kOpenSite) {
        step->url = s.url;
        step->host.clear();
      } else if (url.SchemeIsHTTPOrHTTPS()) {
        // A search on another site: the site it goes to.
        step->host = HintHost(url.host());
      }
    } else if (s.kind == VoiceStepKind::kSearchWeb) {
      const GURL url = controller_->DefaultSearchURL(s.text);
      if (url.SchemeIsHTTPOrHTTPS()) {
        step->host = HintHost(url.host());
      }
    }
  }
  return step;
}

void VoiceLearningLoop::NoteNames(const VoiceCommand& command,
                                  const VoiceTurn& turn) {
  VoiceLearning* learning = Learning();
  if (!learning || !learning->enabled() || heard_while_reading_ ||
      !Acted(command.kind, turn)) {
    return;
  }
  switch (command.kind) {
    case K::kSearch:
      learning->NoteName(command.text);
      if (command.scope == SearchScope::kNamedSite) {
        learning->NoteName(command.site_words);
      }
      break;
    case K::kOpenSite:
      for (const VoiceStep& step : turn.steps) {
        if (step.kind == VoiceStepKind::kNavigate) {
          learning->NoteName(command.site_words);
          break;
        }
      }
      break;
    case K::kChoose:
      if (command.choice.kind == ChoiceRef::Kind::kDescription) {
        for (const auto* words :
             {&command.choice.by_words, &command.choice.from_words}) {
          if (!words->empty()) {
            learning->NoteName(CasedPhrase(command.heard, JoinWords(*words)));
          }
        }
      }
      break;
    default:
      // Never what is typed into a field, nor anything else.
      break;
  }
}

void VoiceLearningLoop::OnPlanned(const VoiceCommand& command,
                                  const VoiceTurn& turn,
                                  const ConversationState& before,
                                  const PageMap& map,
                                  const VoiceEnvironment& env) {
  if (!env.learning.routines_available || env.moltnet) {
    return;
  }
  if (IsReadingKind(command.kind)) {
    reading_ = true;
  }
  parts_.insert(parts_.end(), turn.site_parts.begin(), turn.site_parts.end());
  if (playing_) {
    // An answer to the routine's question isn't a step of a new one.
    return;
  }
  // A request for the planner: its steps join "this" as they run.
  for (const VoiceStep& step : turn.steps) {
    if (step.kind == VoiceStepKind::kToPlanner) {
      planner_goal_ = step.text;
      content::WebContents* contents = controller_->ActiveContents();
      planner_start_url_ =
          contents ? contents->GetLastCommittedURL().spec() : std::string();
      planner_first_ = true;
    }
  }
  // "This": what the user carried out. A step that asked first counts once
  // its "yes" carried it out.
  if (command.kind == K::kYes && pending_) {
    if (Acted(K::kYes, turn)) {
      recorder_.Add(std::move(*pending_));
      last_added_ = true;
    }
    pending_.reset();
  } else {
    pending_.reset();
    if (IsRoutineKind(command.kind) && Acted(command.kind, turn)) {
      if (std::optional<RoutineStep> step =
              StepFromTurn(command, turn, before, map, env)) {
        if (turn.state.confirming) {
          pending_ = std::move(step);
        } else {
          recorder_.Add(std::move(*step));
          last_added_ = true;
        }
      }
    }
  }
  NoteNames(command, turn);
}

void VoiceLearningLoop::OnPlannerCarriedOut(const PlannerStep& carried) {
  VoiceLearning* learning = Learning();
  // Nothing in MoltNet or private windows (no learning there), nor from a
  // routine that runs.
  if (!learning || controller_->IsMoltNet() || playing_) {
    return;
  }
  const bool first = std::exchange(planner_first_, false);
  content::WebContents* contents = controller_->ActiveContents();
  const GURL page = contents ? contents->GetLastCommittedURL() : GURL();
  RoutineStep step = learning->enabled()
                         ? RoutineStepForPlanner(carried, planner_goal_,
                                                 page.SchemeIsHTTPOrHTTPS()
                                                     ? std::string(page.host())
                                                     : std::string())
                         : UnkeptPlannerStep(std::string(kLearningOff));
  if (first && OpensPage(step)) {
    // A request the planner began by opening a page is a new task.
    recorder_.Clear();
  } else if (first && IsAgentStep(step) && !IsUnkeptStep(step)) {
    // Else it began on the page it was asked on: a routine that starts
    // with it opens that page first (BuildRoutine()).
    step.url = planner_start_url_;
  }
  pending_.reset();
  recorder_.Add(std::move(step));
  last_added_ = false;
}

void VoiceLearningLoop::OnPlannerReplayed(PlannerReplay outcome) {
  if (!playing_ || !playing_->replaying) {
    return;
  }
  Playing& p = *playing_;
  p.replaying = false;
  const RoutineStep* step =
      p.next >= 1 && p.next <= p.steps.size() ? &p.steps[p.next - 1] : nullptr;
  const Language language = controller_->language();
  const std::string element =
      step && !step->agent.label.empty() ? step->agent.label : std::string();
  switch (outcome) {
    case PlannerReplay::kDone:
      return;
    case PlannerReplay::kDeclined:
      // "Okay, I won't." was said for the "no".
      p.stop_after = true;
      return;
    case PlannerReplay::kNotFound:
      p.why = element.empty()
                  ? Said(language, "I couldn't find it on this page.", {})
                  : Said(language, "I couldn't find {0} on this page.",
                         {element});
      break;
    case PlannerReplay::kAmbiguous:
      p.why = element.empty()
                  ? Said(language, "I found more than one it on this page.",
                         {})
                  : Said(language, "I found more than one {0} on this page.",
                         {element});
      break;
    case PlannerReplay::kFailed:
      p.why = Said(language, "That step didn't work.", {});
      break;
  }
  p.failed = true;
}

void VoiceLearningLoop::OnFollowUp(const VoiceTurn& turn) {
  parts_.insert(parts_.end(), turn.site_parts.begin(), turn.site_parts.end());
}

void VoiceLearningLoop::OnStepFailed() {
  failed_ = true;
  if (last_added_) {
    recorder_.DropLast();
    last_added_ = false;
  }
  if (playing_) {
    playing_->failed = true;
  }
}

void VoiceLearningLoop::CommitSiteParts() {
  VoiceLearning* learning = Learning();
  if (learning && learning->enabled()) {
    for (const SitePartUse& use : parts_) {
      if (failed_) {
        if (use.source == PartSource::kHint || use.hint_missed) {
          learning->HintFailed(use.host, use.part);
        }
      } else if (use.source == PartSource::kGeneric ||
                 use.source == PartSource::kHint) {
        learning->HintWorked(use.host, use.part, use.name);
      }
    }
  }
  parts_.clear();
}

void VoiceLearningLoop::OnIdle() {
  if (controller_->busy()) {
    return;
  }
  CommitSiteParts();
  if (!playing_ || playing_->scheduled) {
    return;
  }
  Playing& p = *playing_;
  if (p.replaying) {
    // The planner replays the step, or asks before it does.
    if (controller_->planner_running()) {
      return;
    }
    // Its run ended unanswered (the loop said so): the routine ends too.
    StopRoutine(std::string());
    return;
  }
  if (p.failed) {
    StopRoutine((p.why.empty() ? std::string() : p.why + " ") +
                Said(controller_->language(), "I stopped {0} at step {1}.",
                     {p.name, std::to_string(p.next)}));
    return;
  }
  if (p.stop_after) {
    StopRoutine(Said(controller_->language(), "I stopped {0}.", {p.name}));
    return;
  }
  if (p.asking_value || (p.waiting_answer && controller_->state_.confirming)) {
    return;
  }
  p.waiting_answer = false;
  // The step's last words are heard out first.
  if (controller_->speech_ && controller_->speech_->speaking()) {
    return;
  }
  ScheduleNextStep();
}

void VoiceLearningLoop::OnSpeakingChanged(bool speaking) {
  if (!speaking && (reading_ || controller_->reader_->reading())) {
    reading_ = false;
    reading_until_ = base::TimeTicks::Now() + kReadingGrace;
  }
  if (!speaking && playing_ && !controller_->busy()) {
    OnIdle();
  }
}

void VoiceLearningLoop::Stop() {
  answer_timer_.Stop();
  playing_.reset();
  pending_.reset();
  reading_ = false;
  planner_first_ = false;
}

// ---- kLearning steps ---------------------------------------------------------

void VoiceLearningLoop::RunStep(const VoiceStep& step) {
  VoiceLearning* learning = Learning();
  if (!learning) {
    Say("I can't do that here.", /*interrupt=*/false);
    return;
  }
  base::WeakPtr<VoiceLearningLoop> self = weak_factory_.GetWeakPtr();
  const std::string name = PageText(step.text);
  switch (static_cast<LearningOp>(step.index)) {
    case LearningOp::kSaveRoutine:
      SaveRoutine(step.text, step.count, /*replace=*/false);
      return;
    case LearningOp::kReplaceRoutine:
      SaveRoutine(step.text, step.count, /*replace=*/true);
      return;
    case LearningOp::kClaimRoutines:
      ClaimRoutines(step.count);
      return;
    case LearningOp::kDeclineRoutines:
      DeclineRoutines(step.count);
      return;
    case LearningOp::kRunRoutine:
      RunRoutine(step.text, std::string());
      return;
    case LearningOp::kForgetRoutine:
      learning->DeleteRoutine(
          step.text, std::string(),
          base::BindOnce(
              [](base::WeakPtr<VoiceLearningLoop> self, std::string name,
                 bool deleted) {
                if (self) {
                  const Language language = self->controller_->language();
                  self->Say(deleted
                                ? Said(language, "I forgot {0}.", {name})
                                : Said(language, "I couldn't forget {0}.",
                                       {name}),
                            /*interrupt=*/false);
                }
              },
              self, name));
      return;
    case LearningOp::kForgetWord:
      Say(learning->ForgetWord(step.text)
              ? Said(controller_->language(), "I forgot the word {0}.",
                     {name})
              : Said(controller_->language(),
                     "I haven't learned the word {0}.", {name}),
          /*interrupt=*/false);
      return;
    case LearningOp::kForgetEverything:
      recorder_.Clear();
      pending_.reset();
      learning->ForgetEverything(base::BindOnce(
          [](base::WeakPtr<VoiceLearningLoop> self) {
            if (self) {
              self->Say("Done. I forgot everything I learned from you.",
                        /*interrupt=*/false);
            }
          },
          self));
      return;
  }
}

void VoiceLearningLoop::SaveRoutine(const std::string& name,
                                    int version,
                                    bool replace) {
  VoiceLearning* learning = Learning();
  if (!learning) {
    return;
  }
  if (version != recorder_.version()) {
    Say(Said(controller_->language(),
             "What I'd remember changed. Say remember this again.", {}),
        /*interrupt=*/false);
    return;
  }
  learning->SaveRoutine(
      name, recorder_.steps(), replace,
      base::BindOnce(
          [](base::WeakPtr<VoiceLearningLoop> self, std::string name,
             RoutineSaveResult result) {
            if (!self) {
              return;
            }
            const Language language = self->controller_->language();
            if (!result.saved) {
              // The store's words, made in English.
              self->Say(Relocalized(language, result.said),
                        /*interrupt=*/false);
              return;
            }
            self->recorder_.Clear();
            const std::string spoken = PageText(name);
            // The planner's steps: the sites it uses, which the user
            // confirms the first time it runs.
            if (result.sites.empty()) {
              self->Say(result.replaced
                            ? Said(language,
                                   "Replaced {0}. Say run {0} to do it again.",
                                   {spoken})
                            : Said(language,
                                   "Saved. Say run {0} to do it again.",
                                   {spoken}),
                        /*interrupt=*/false);
              return;
            }
            const std::string sites =
                JoinSpokenList(result.sites, language);
            self->Say(result.replaced
                          ? Said(language,
                                 "Replaced {0}. It uses {1}. Its sites need "
                                 "your confirmation the first time you run "
                                 "it.",
                                 {spoken, sites})
                          : Said(language,
                                 "Saved {0}. It uses {1}. Its sites need "
                                 "your confirmation the first time you run "
                                 "it.",
                                 {spoken, sites}),
                      /*interrupt=*/false);
          },
          weak_factory_.GetWeakPtr(), name));
}

void VoiceLearningLoop::ClaimRoutines(int version) {
  VoiceLearning* learning = Learning();
  if (!learning) {
    return;
  }
  // Exactly the routines the question named: if they changed since (one
  // kept by another profile, say), the "yes" keeps nothing.
  if (version != learning->unclaimed_version() ||
      learning->unclaimed().empty()) {
    Say(Said(controller_->language(),
             "Those routines changed since I asked. Say what are my routines "
             "again.",
             {}),
        /*interrupt=*/false);
    return;
  }
  std::vector<std::string> ids;
  for (const RoutineSummary& routine : learning->unclaimed()) {
    ids.push_back(routine.id);
  }
  learning->ClaimRoutines(
      std::move(ids),
      base::BindOnce(
          [](base::WeakPtr<VoiceLearningLoop> self, ClaimResult result) {
            if (!self) {
              return;
            }
            const Language language = self->controller_->language();
            if (result.kept.empty()) {
              self->Say(Said(language,
                             "I couldn't keep them here. Another profile may "
                             "have kept them first.",
                             {}),
                        /*interrupt=*/false);
              return;
            }
            std::string said =
                result.kept.size() == 1
                    ? Said(language, "Kept it here.", {})
                    : Said(language, "Kept {0} routines here.",
                           {std::to_string(result.kept.size())});
            for (const ClaimedRoutine& kept : result.kept) {
              if (!kept.renamed_from.empty()) {
                said += " " + Said(language,
                                   "You already had a routine called {0}, so "
                                   "this one is {1}.",
                                   {PageText(kept.renamed_from),
                                    PageText(kept.name)});
              }
            }
            if (result.not_kept) {
              const std::string count = std::to_string(result.not_kept);
              said += " " + (result.not_kept == 1
                                 ? Said(language, "{0} wasn't there any more.",
                                        {count})
                                 : Said(language,
                                        "{0} weren't there any more.",
                                        {count}));
            }
            self->Say(said, /*interrupt=*/false);
          },
          weak_factory_.GetWeakPtr()));
}

void VoiceLearningLoop::DeclineRoutines(int version) {
  VoiceLearning* learning = Learning();
  // Only the ones the question named (Declined() said why already).
  if (!learning || version != learning->unclaimed_version()) {
    return;
  }
  std::vector<std::string> ids;
  for (const RoutineSummary& routine : learning->unclaimed()) {
    ids.push_back(routine.id);
  }
  learning->DeclineRoutines(ids);
}

void VoiceLearningLoop::RunRoutine(const std::string& name,
                                   const std::string& id) {
  VoiceLearning* learning = Learning();
  if (!learning) {
    return;
  }
  StopRoutine(std::string());
  // By its id when it has one (the routine whose sites were just
  // confirmed): another routine of the same name can't stand in for it.
  learning->LoadRoutine(id.empty() ? name : std::string(), id,
                        base::BindOnce(&VoiceLearningLoop::OnRoutineLoaded,
                                       weak_factory_.GetWeakPtr(), name));
}

void VoiceLearningLoop::OnRoutineLoaded(std::string name,
                                        std::optional<RoutineToRun> routine) {
  const std::string spoken = PageText(name);
  if (!routine) {
    Say(Said(controller_->language(), "I can't find the routine {0}.",
             {spoken}),
        /*interrupt=*/false);
    return;
  }
  if (!routine->ok) {
    // ReadRoutine()'s words, made in English.
    Say(Relocalized(controller_->language(), routine->why),
        /*interrupt=*/false);
    return;
  }
  if (!routine->needs_sites_confirmed) {
    StartRoutine(std::move(*routine));
    return;
  }
  // The planner's steps, or a routine brought in from elsewhere: its sites
  // need the user's confirmation first (voice_routine_sites.h).
  const std::string id = routine->id;
  const std::vector<std::string> sites = routine->sites;
  auto [by_voice, by_confirmer] = base::SplitOnceCallback(base::BindOnce(
      [](base::WeakPtr<VoiceLearningLoop> self, std::string name,
         std::string id, std::vector<std::string> sites, bool confirmed,
         std::string said) {
        if (!self) {
          return;
        }
        const std::string spoken = PageText(name);
        if (!confirmed && said.empty()) {
          // The prompt said why already (a wrong word, no answer).
          return;
        }
        if (!confirmed) {
          // The confirmer's words (voice_routine_sites.h), made in English.
          const Language language = self->controller_->language();
          self->Say(Said(language,
                         "Before {0} runs, its sites need your "
                         "confirmation. {1}",
                         {spoken, Relocalized(language, said)}),
                    /*interrupt=*/false);
          return;
        }
        VoiceLearning* learning = self->Learning();
        if (!learning) {
          return;
        }
        learning->ConfirmRoutineSites(
            id, sites,
            base::BindOnce(
                [](base::WeakPtr<VoiceLearningLoop> self, std::string name,
                   std::string id, bool ok) {
                  if (!self) {
                    return;
                  }
                  if (ok) {
                    // The workflow confirmed, by its id.
                    self->RunRoutine(name, id);
                  } else {
                    self->Say(Said(self->controller_->language(),
                                   "I couldn't confirm its sites.", {}),
                              /*interrupt=*/false);
                  }
                },
                self, name, id));
      },
      weak_factory_.GetWeakPtr(), routine->name, id, sites));
  // By voice, with the voice gesture, while the window listens
  // (VoiceBrowserPrompts::ConfirmDraftSites(): the sites said and shown, a
  // confirmation word heard cleanly, bound to exactly this workflow and
  // these sites); else the confirmer says what to do instead. Only sites
  // the prompt can say in full: the word confirms every one.
  if (RoutineSitesSayable(sites) &&
      controller_->ConfirmDraftSitesByVoice(
          id, sites,
          base::BindOnce(
              [](RoutineSiteConfirmer::Done done, bool confirmed) {
                std::move(done).Run(confirmed, std::string());
              },
              std::move(by_voice)))) {
    return;
  }
  confirmer_->ConfirmSites(controller_->ActiveContents(), routine->name, sites,
                           std::move(by_confirmer));
}

void VoiceLearningLoop::StartRoutine(RoutineToRun routine) {
  if (!controller_->active_ || routine.steps.empty()) {
    return;
  }
  Playing p;
  p.id = std::move(routine.id);
  p.name = PageText(routine.name);
  p.steps = std::move(routine.steps);
  p.policy = std::move(routine.policy);
  playing_ = std::move(p);
  NextStep();
}

void VoiceLearningLoop::ScheduleNextStep() {
  if (!playing_ || playing_->scheduled) {
    return;
  }
  playing_->scheduled = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&VoiceLearningLoop::NextStep,
                                weak_factory_.GetWeakPtr()));
}

void VoiceLearningLoop::NextStep() {
  if (!playing_) {
    return;
  }
  Playing& p = *playing_;
  p.scheduled = false;
  if (controller_->busy() || !controller_->active_) {
    return;
  }
  const Language language = controller_->language();
  if (p.next >= p.steps.size()) {
    const std::string done = Said(language, "{0} is done.", {p.name});
    playing_.reset();
    Say(done, /*interrupt=*/false);
    return;
  }
  const size_t index = p.next;
  const RoutineStep& step = p.steps[index];
  const std::string number = std::to_string(index + 1);
  const std::string described = DescribeRoutineStep(step, language);
  std::string said =
      index == 0 ? Said(language, "Running {0}. Step {1}: {2}.",
                        {p.name, number, described})
                 : Said(language, "Step {0}: {1}.", {number, described});
  if (!step.variable.empty() && !p.values.count(step.variable)) {
    p.asking_value = true;
    Say(said + " " +
            Said(language,
                 "I didn't keep the text for it. Say type, then the text, or "
                 "anything else to stop.",
                 {}),
        /*interrupt=*/index == 0);
    StartAnswerTimer();
    return;
  }
  if (IsAgentStep(step)) {
    ReplayStep(index, said);
    return;
  }
  VoiceCommand command = CommandForStep(
      step, step.variable.empty() ? std::string() : p.values[step.variable]);
  // Shown as what was heard, in the window's language.
  command.heard = described;
  // Only on the routine's own sites.
  automation::AutomationSecurity security;
  content::WebContents* contents = controller_->ActiveContents();
  const GURL page = contents ? contents->GetLastCommittedURL() : GURL();
  if (ActsOnPage(command) && !security.AllowNavigation(p.policy, page.spec())) {
    StopRoutine(Said(language,
                     "I stopped {0}: step {1} would act on {2}, which isn't "
                     "one of its sites.",
                     {p.name, number, PageText(page.host())}));
    return;
  }
  const PageMap map = controller_->CurrentMap();
  const VoiceEnvironment env = controller_->Environment();
  VoiceTurn turn = PlanCommand(command, map, env, controller_->state_);
  for (const VoiceStep& s : turn.steps) {
    GURL target;
    if (s.kind == VoiceStepKind::kNavigate) {
      target = GURL(s.url);
    } else if (s.kind == VoiceStepKind::kSearchWeb) {
      target = controller_->DefaultSearchURL(s.text);
    } else {
      continue;
    }
    if (!security.AllowNavigation(p.policy, target.spec())) {
      StopRoutine(Said(language,
                       "I stopped {0}: step {1} would open {2}, which isn't "
                       "one of its sites.",
                       {p.name, number, PageText(target.host())}));
      return;
    }
  }
  p.next = index + 1;
  p.waiting_answer = turn.state.confirming;
  // A step that finds nothing to act on says why; then the routine stops.
  p.failed = !Acted(command.kind, turn);
  VoiceStep intro;
  intro.kind = VoiceStepKind::kSpeak;
  intro.text = SpeechSafeText(said, kMaxSaidChars);
  turn.steps.insert(turn.steps.begin(), std::move(intro));
  parts_ = turn.site_parts;
  failed_ = false;
  if (p.waiting_answer) {
    StartAnswerTimer();
  }
  controller_->heard_ = SpeechSafeText(command.heard, kMaxSaidChars);
  controller_->RunTurn(std::move(turn));
}

void VoiceLearningLoop::ReplayStep(size_t index, const std::string& said) {
  Playing& p = *playing_;
  const RoutineStep& step = p.steps[index];
  const Language language = controller_->language();
  // Only on the routine's own sites: the page it opens, or the page it
  // acts on.
  const bool opens = OpensPage(step);
  automation::AutomationSecurity security;
  content::WebContents* contents = controller_->ActiveContents();
  const GURL page = contents ? contents->GetLastCommittedURL() : GURL();
  const GURL target = opens ? GURL(step.url) : page;
  if (!security.AllowNavigation(p.policy, target.spec())) {
    const std::string number = std::to_string(index + 1);
    const std::string host = PageText(target.host());
    StopRoutine(opens ? Said(language,
                             "I stopped {0}: step {1} would open {2}, which "
                             "isn't one of its sites.",
                             {p.name, number, host})
                      : Said(language,
                             "I stopped {0}: step {1} would act on {2}, which "
                             "isn't one of its sites.",
                             {p.name, number, host}));
    return;
  }
  PlannerStep replay = step.agent;
  replay.url = step.url;
  replay.host = step.host;
  if (!step.variable.empty()) {
    replay.text = p.values[step.variable];
  }
  p.next = index + 1;
  p.waiting_answer = false;
  p.failed = false;
  p.why.clear();
  p.replaying = true;
  const std::string name = p.name;
  Say(said, /*interrupt=*/index == 0);
  controller_->heard_ =
      SpeechSafeText(DescribeRoutineStep(step, language), kMaxSaidChars);
  // The planner may answer at once: |p| isn't used after this.
  if (!controller_->ReplayPlannerStep(replay)) {
    StopRoutine(Said(language,
                     "I can't run the AI's steps here, so I stopped {0}.",
                     {name}));
  }
}

void VoiceLearningLoop::StopRoutine(const std::string& said) {
  answer_timer_.Stop();
  if (!playing_) {
    return;
  }
  playing_.reset();
  if (!said.empty()) {
    Say(said, /*interrupt=*/false);
  }
}

void VoiceLearningLoop::StartAnswerTimer() {
  answer_timer_.Start(FROM_HERE, base::Milliseconds(kConfirmationTimeoutMs),
                      base::BindOnce(&VoiceLearningLoop::OnAnswerTimeout,
                                     base::Unretained(this)));
}

void VoiceLearningLoop::OnAnswerTimeout() {
  if (playing_ && (playing_->waiting_answer || playing_->asking_value)) {
    StopRoutine(Said(controller_->language(),
                     "I didn't hear an answer, so I stopped {0}.",
                     {playing_->name}));
  }
}

}  // namespace voice
}  // namespace molt_ai
