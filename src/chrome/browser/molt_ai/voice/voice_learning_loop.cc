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

bool IsReadingKind(K kind) {
  return kind == K::kReadPage || kind == K::kReadFromHere ||
         kind == K::kReadSelection || kind == K::kReadSection ||
         kind == K::kNextParagraph || kind == K::kPreviousParagraph;
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
  const std::string said = SpeechSafeText(text, kMaxSaidChars);
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
  for (const RoutineStep& step : recorder_.steps()) {
    env.this_steps.push_back(DescribeRoutineStep(step));
  }
  env.this_planned = recorder_.planned();
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
    const bool answers = p.waiting_answer && context.expecting_yes_no &&
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
  // is asked for.
  if (learn && !context.expecting_yes_no && command.kind != K::kYes &&
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
  if (p.failed) {
    StopRoutine("I stopped " + p.name + " at step " + std::to_string(p.next) +
                ".");
    return;
  }
  if (p.stop_after) {
    StopRoutine("I stopped " + p.name + ".");
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
      SaveRoutine(step.text, step.count);
      return;
    case LearningOp::kRunRoutine:
      RunRoutine(step.text);
      return;
    case LearningOp::kForgetRoutine:
      learning->DeleteRoutine(
          step.text, std::string(),
          base::BindOnce(
              [](base::WeakPtr<VoiceLearningLoop> self, std::string name,
                 bool deleted) {
                if (self) {
                  self->Say(deleted ? "I forgot " + name + "."
                                    : "I couldn't forget " + name + ".",
                            /*interrupt=*/false);
                }
              },
              self, name));
      return;
    case LearningOp::kForgetWord:
      Say(learning->ForgetWord(step.text)
              ? "I forgot the word " + name + "."
              : "I haven't learned the word " + name + ".",
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

void VoiceLearningLoop::SaveRoutine(const std::string& name, int version) {
  VoiceLearning* learning = Learning();
  if (!learning) {
    return;
  }
  if (version != recorder_.version()) {
    Say("What I'd remember changed. Say remember this again.",
        /*interrupt=*/false);
    return;
  }
  const bool planned = recorder_.planned();
  learning->SaveRoutine(
      name, recorder_.steps(),
      base::BindOnce(
          [](base::WeakPtr<VoiceLearningLoop> self, std::string name,
             bool planned, bool saved, std::string said) {
            if (!self) {
              return;
            }
            if (!saved) {
              self->Say(said, /*interrupt=*/false);
              return;
            }
            self->recorder_.Clear();
            const std::string spoken = PageText(name);
            self->Say(planned ? "Saved " + spoken +
                                    ". Some of its steps are the AI's, so "
                                    "you'll confirm its sites before it runs."
                              : "Saved. Say run " + spoken +
                                    " to do it again.",
                      /*interrupt=*/false);
          },
          weak_factory_.GetWeakPtr(), name, planned));
}

void VoiceLearningLoop::RunRoutine(const std::string& name) {
  VoiceLearning* learning = Learning();
  if (!learning) {
    return;
  }
  StopRoutine(std::string());
  learning->LoadRoutine(
      name, base::BindOnce(&VoiceLearningLoop::OnRoutineLoaded,
                           weak_factory_.GetWeakPtr(), name));
}

void VoiceLearningLoop::OnRoutineLoaded(std::string name,
                                        std::optional<RoutineToRun> routine) {
  const std::string spoken = PageText(name);
  if (!routine) {
    Say("I can't find the routine " + spoken + ".", /*interrupt=*/false);
    return;
  }
  if (!routine->ok) {
    Say(routine->why, /*interrupt=*/false);
    return;
  }
  if (!routine->needs_sites_confirmed) {
    StartRoutine(std::move(*routine));
    return;
  }
  // The planner's steps: its sites need the user's confirmation first
  // (voice_routine_sites.h).
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
          self->Say(spoken +
                        " has steps the AI planned, so the sites it uses "
                        "need your confirmation. " +
                        said,
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
                   bool ok) {
                  if (!self) {
                    return;
                  }
                  if (ok) {
                    self->RunRoutine(name);
                  } else {
                    self->Say("I couldn't confirm its sites.",
                              /*interrupt=*/false);
                  }
                },
                self, name));
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
  if (p.next >= p.steps.size()) {
    const std::string done = p.name + " is done.";
    playing_.reset();
    Say(done, /*interrupt=*/false);
    return;
  }
  const size_t index = p.next;
  const RoutineStep& step = p.steps[index];
  std::string said = "Step " + std::to_string(index + 1) + ": " +
                     DescribeRoutineStep(step) + ".";
  if (index == 0) {
    said = "Running " + p.name + ". " + said;
  }
  if (!step.variable.empty() && !p.values.count(step.variable)) {
    p.asking_value = true;
    Say(said + " I didn't keep the text for it. Say type, then the text, or "
               "anything else to stop.",
        /*interrupt=*/index == 0);
    StartAnswerTimer();
    return;
  }
  const VoiceCommand command = CommandForStep(
      step, step.variable.empty() ? std::string() : p.values[step.variable]);
  // Only on the routine's own sites.
  automation::AutomationSecurity security;
  content::WebContents* contents = controller_->ActiveContents();
  const GURL page = contents ? contents->GetLastCommittedURL() : GURL();
  if (ActsOnPage(command) && !security.AllowNavigation(p.policy, page.spec())) {
    StopRoutine("I stopped " + p.name + ": step " +
                std::to_string(index + 1) + " would act on " +
                PageText(page.host()) + ", which isn't one of its sites.");
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
      StopRoutine("I stopped " + p.name + ": step " +
                  std::to_string(index + 1) + " would open " +
                  PageText(target.host()) + ", which isn't one of its sites.");
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
    StopRoutine("I didn't hear an answer, so I stopped " + playing_->name +
                ".");
  }
}

}  // namespace voice
}  // namespace molt_ai
