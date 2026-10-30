// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_page_questions.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"

namespace molt_ai {
namespace voice {

namespace {

// What the browser says.
constexpr char kStart[] = "Let me look at the page.";
constexpr char kStartMore[] = "Let me look further.";
constexpr char kStillWorkingText[] = "Still working on it.";
constexpr char kNoModel[] =
    "The on-device AI isn't set up. You can download a model in settings.";
constexpr char kUnsupported[] =
    "The on-device AI can't run on this computer, so I can't answer "
    "questions about pages.";
constexpr char kLoadFailed[] =
    "I couldn't start the on-device AI. Try again in a moment, or choose "
    "another model in settings.";
constexpr char kTooSlow[] =
    "The on-device AI didn't answer in time, so I stopped. It may be busy, "
    "or the page too long for this computer.";
constexpr char kFailed[] =
    "The on-device AI couldn't answer that. Try asking another way.";
constexpr char kNoText[] = "There's no text on this page I can answer from.";
constexpr char kAskFirst[] =
    "Ask me something about this page first, then say tell me more.";
constexpr char kNoMorePage[] = "That's all of the page.";
constexpr char kNoMoreTopic[] = "That's all the page says about that.";
constexpr char kPageChanged[] = "The page changed, so I stopped.";

// The answer's sampling: low temperature, for answers that stay close to
// the page's words.
constexpr float kTemperature = 0.2f;
constexpr float kTopP = 0.9f;

VoicePageQuestions::ModelForTesting& TestingModel() {
  static base::NoDestructor<VoicePageQuestions::ModelForTesting> model;
  return *model;
}

}  // namespace

// The last question and its page's text, for "tell me more".
struct VoicePageQuestions::Followed {
  int64_t page_id = -1;
  PageQuestion question;
  ReadingDocument document;
  GivenBlocks given;
};

struct VoicePageQuestions::ModelCheck {
  bool ready = false;
  // When not ready: the words the browser says.
  std::string problem;
};

// static
void VoicePageQuestions::SetModelForTesting(ModelForTesting model) {
  TestingModel() = std::move(model);
}

// static
bool VoicePageQuestions::HasModelForTesting() {
  return !TestingModel().is_null();
}

VoicePageQuestions::VoicePageQuestions() = default;

VoicePageQuestions::~VoicePageQuestions() {
  Cancel();
}

// static
VoicePageQuestions::ModelCheck VoicePageQuestions::EnsureModel() {
  // As the planner fallback does (agents/voice_agent_planner.cc): the
  // model loaded, else the one pages would get (web_ai_policy.h).
  ModelCheck check;
  BrowserAIRuntime& runtime = BrowserAIRuntime::GetInstance();
  if (!runtime.GetLocalInferenceUnsupportedReason().empty()) {
    check.problem = kUnsupported;
    return check;
  }
  if (!runtime.GetLoadedModelId().empty()) {
    check.ready = true;
    return check;
  }
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
    check.problem = kNoModel;
    return check;
  }
  std::string error;
  if (!runtime.LoadModel(model_id, &error)) {
    // The runtime's reason goes to the log, not to the voice: the user
    // hears what to do.
    LOG(WARNING) << "[MoltVoice] page questions: the model didn't load";
    check.problem = kLoadFailed;
    return check;
  }
  check.ready = true;
  return check;
}

void VoicePageQuestions::Ask(PageQuestionKind kind,
                             const std::string& text,
                             int64_t page_id,
                             const PageTree& tree,
                             const SiteSkill* skill,
                             ProgressCallback progress,
                             DoneCallback done) {
  // One question at a time.
  Cancel();
  if (last_ && last_->page_id != page_id) {
    last_.reset();
  }
  question_ = PageQuestion();
  page_id_ = page_id;
  done_ = std::move(done);
  run_ = ++next_run_;
  const bool more = kind == PageQuestionKind::kMore;
  if (more && !last_) {
    FinishSoon(kAskFirst);
    return;
  }
  if (!more) {
    auto followed = std::make_unique<Followed>();
    followed->page_id = page_id;
    followed->question = MakePageQuestion(kind, text);
    followed->document = QuestionDocument(tree, skill);
    last_.reset();
    if (followed->document.blocks.empty()) {
      FinishSoon(kNoText);
      return;
    }
    last_ = std::move(followed);
  }
  // The loaded model's window decides how much of the page fits (the
  // default until one is loaded).
  const uint32_t context_tokens =
      HasModelForTesting()
          ? 0
          : BrowserAIRuntime::GetInstance().GetLoadedContextTokens();
  const PageExcerpt excerpt =
      SelectPageText(last_->document, last_->question,
                     PageTextBudgetFor(context_tokens), &last_->given, more);
  if (excerpt.parts.empty()) {
    const bool topic = last_->question.kind == PageQuestionKind::kTopic ||
                       last_->question.kind == PageQuestionKind::kQuestion;
    FinishSoon(more ? (topic ? kNoMoreTopic : kNoMorePage) : kNoText);
    return;
  }
  const PageQuestionPrompt prompt =
      BuildPageQuestionPrompt(last_->question, excerpt, more);
  DVLOG(1) << "[MoltVoice] page question: " << excerpt.parts.size()
           << " part(s), " << excerpt.bytes << " bytes"
           << (excerpt.capped ? ", capped" : "");

  question_ = last_->question;
  progress_ = std::move(progress);
  if (progress_) {
    progress_.Run(more ? kStartMore : kStart);
  }
  still_working_timer_.Start(FROM_HERE, kStillWorking,
                             base::BindOnce(&VoicePageQuestions::OnStillWorking,
                                            weak_factory_.GetWeakPtr(), run_));

  ChatPrompt chat;
  chat.system_prompt = prompt.system_prompt;
  chat.user_message = prompt.user_message;
  PromptOptions options;
  options.max_tokens = kPageAnswerMaxTokens;
  options.temperature = kTemperature;
  options.top_p = kTopP;
  options.reasoning = ReasoningMode::kOff;
  // One bounded string and a flag (constrained decoding): no lists, no
  // markup to speak, no room for more than the answer.
  options.json_schema = std::string(PageAnswerSchema());

  if (HasModelForTesting()) {
    ModelCheck ready;
    ready.ready = true;
    // After the words said first, as a real model would answer.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&VoicePageQuestions::OnModelChecked,
                       weak_factory_.GetWeakPtr(), run_, std::move(chat),
                       std::move(options), std::move(ready)));
    return;
  }
  // The runtime is the process's (as the chat's): initialized on the UI
  // thread, the model loaded on a worker.
  BrowserAIRuntime& runtime = BrowserAIRuntime::GetInstance();
  runtime.Initialize();
  runtime.StartMemoryPressureMonitoring();
  request_id_ = runtime.NewRequestId();
  options.request_id = request_id_;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&VoicePageQuestions::EnsureModel),
      base::BindOnce(&VoicePageQuestions::OnModelChecked,
                     weak_factory_.GetWeakPtr(), run_, std::move(chat),
                     std::move(options)));
}

void VoicePageQuestions::OnModelChecked(int run,
                                        ChatPrompt prompt,
                                        PromptOptions options,
                                        ModelCheck check) {
  if (run != run_ || !done_) {
    return;
  }
  if (!check.ready) {
    Finish(check.problem.empty() ? std::string(kNoModel) : check.problem,
           /*from_page=*/false);
    return;
  }
  // The time limit is on the generation, from here.
  time_limit_timer_.Start(FROM_HERE, kTimeLimit,
                          base::BindOnce(&VoicePageQuestions::OnTimeLimit,
                                         weak_factory_.GetWeakPtr(), run));
  Generate(run, std::move(prompt), std::move(options));
}

void VoicePageQuestions::Generate(int run,
                                  ChatPrompt prompt,
                                  PromptOptions options) {
  if (HasModelForTesting()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<VoicePageQuestions> self, int run,
                          ChatPrompt prompt, PromptOptions options) {
                         if (self && HasModelForTesting()) {
                           self->OnReply(run,
                                         TestingModel().Run(prompt, options));
                         }
                       },
                       weak_factory_.GetWeakPtr(), run, std::move(prompt),
                       std::move(options)));
    return;
  }
  // From here the worker holds the request id, and releases it.
  generating_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(
          [](ChatPrompt prompt, PromptOptions options) {
            BrowserAIRuntime& runtime = BrowserAIRuntime::GetInstance();
            // The request id is ours to release whatever happens.
            base::ScopedClosureRunner release(
                base::BindOnce(&BrowserAIRuntime::FinishRequest,
                               base::Unretained(&runtime), options.request_id));
            // Nothing is streamed: the answer is said once it is whole
            // and checked.
            return runtime.StreamChat(prompt, TokenCallback(), options);
          },
          std::move(prompt), std::move(options)),
      base::BindOnce(&VoicePageQuestions::OnReply, weak_factory_.GetWeakPtr(),
                     run));
}

void VoicePageQuestions::OnReply(int run, GenerationResult result) {
  if (run != run_ || !done_) {
    return;
  }
  // The worker released the request.
  request_id_ = 0;
  generating_ = false;
  // Counts only, never text.
  DVLOG(1) << "[MoltVoice] page question answered: " << result.tokens_prompt
           << " prompt and " << result.tokens_generated
           << " answer token(s) in " << result.generation_time_ms << " ms";
  if (!result.success || result.was_cancelled) {
    Finish(kFailed, /*from_page=*/false);
    return;
  }
  // The model wrote it from the page's text: hostile.
  const PageAnswer answer = PageAnswerFromModel(result.text, question_);
  Finish(answer.text, answer.kind == PageAnswer::Kind::kAnswer);
}

void VoicePageQuestions::OnStillWorking(int run) {
  if (run == run_ && progress_) {
    progress_.Run(kStillWorkingText);
  }
}

void VoicePageQuestions::OnTimeLimit(int run) {
  if (run != run_ || !done_) {
    return;
  }
  CancelRequest();
  Finish(kTooSlow, /*from_page=*/false);
}

void VoicePageQuestions::Finish(std::string text, bool from_page) {
  still_working_timer_.Stop();
  time_limit_timer_.Stop();
  run_ = 0;
  // A request no worker got (no model) is released.
  CancelRequest();
  progress_.Reset();
  DoneCallback done = std::move(done_);
  if (done) {
    std::move(done).Run(
        {SpeechSafeText(text, 2 * kMaxPageAnswerChars), from_page});
  }
}

void VoicePageQuestions::FinishSoon(const char* text) {
  // Never inside Ask(): its caller hears the outcome as it hears an
  // answer, and "stop" before then drops it.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<VoicePageQuestions> self, int run,
                        std::string said) {
                       if (self && run == self->run_ && self->done_) {
                         self->Finish(std::move(said), /*from_page=*/false);
                       }
                     },
                     weak_factory_.GetWeakPtr(), run_, std::string(text)));
}

void VoicePageQuestions::CancelRequest() {
  if (request_id_ == 0) {
    return;
  }
  BrowserAIRuntime& runtime = BrowserAIRuntime::GetInstance();
  runtime.CancelRequest(request_id_);
  // The worker generating it releases it; one no worker holds (the model
  // still loading, or none) is released here.
  if (!generating_) {
    runtime.FinishRequest(request_id_);
  }
  request_id_ = 0;
  generating_ = false;
}

void VoicePageQuestions::Cancel() {
  CancelRequest();
  still_working_timer_.Stop();
  time_limit_timer_.Stop();
  run_ = 0;
  progress_.Reset();
  done_.Reset();
}

void VoicePageQuestions::PageChanged(int64_t page_id) {
  if (last_ && last_->page_id != page_id) {
    last_.reset();
  }
  if (running() && page_id_ != page_id) {
    CancelRequest();
    Finish(kPageChanged, /*from_page=*/false);
  }
}

void VoicePageQuestions::Clear() {
  Cancel();
  last_.reset();
}

}  // namespace voice
}  // namespace molt_ai
