// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_ai_answers.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/molt_ai/voice/ai_questions.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/spoken_answers.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"

namespace molt_ai {
namespace voice {

namespace {

// What the browser says (whole sentences of the catalog, in the window's
// language when said).
constexpr std::string_view kStart = SaidLater("Let me think.");
constexpr std::string_view kStillWorkingText =
    SaidLater("Still working on it.");
constexpr std::string_view kNoModel = SaidLater(
    "The on-device AI isn't set up. You can download a model in settings.");
constexpr std::string_view kUnsupported =
    SaidLater("The on-device AI can't run on this computer.");
constexpr std::string_view kLoadFailed = SaidLater(
    "I couldn't start the on-device AI. Try again in a moment, or choose "
    "another model in settings.");
constexpr std::string_view kTooSlow = SaidLater(
    "The on-device AI didn't answer in time, so I stopped. It may be busy.");
constexpr std::string_view kFailed =
    SaidLater("The on-device AI couldn't answer that. Try asking another way.");
constexpr std::string_view kNoQuestion =
    SaidLater("Ask what? Say ask the AI, then your question.");

constexpr float kTemperature = 0.3f;
constexpr float kTopP = 0.9f;

VoiceAiAnswers::ModelForTesting& TestingModel() {
  static base::NoDestructor<VoiceAiAnswers::ModelForTesting> model;
  return *model;
}

}  // namespace

struct VoiceAiAnswers::ModelCheck {
  bool ready = false;
  std::string_view problem;
};

// static
void VoiceAiAnswers::SetModelForTesting(ModelForTesting model) {
  TestingModel() = std::move(model);
}

// static
bool VoiceAiAnswers::HasModelForTesting() {
  return !TestingModel().is_null();
}

VoiceAiAnswers::VoiceAiAnswers() = default;

VoiceAiAnswers::~VoiceAiAnswers() {
  Cancel();
}

// static
VoiceAiAnswers::ModelCheck VoiceAiAnswers::EnsureModel() {
  // As questions about the page do (voice_page_questions.cc): the model
  // loaded, else the one pages would get (web_ai_policy.h).
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
    LOG(WARNING) << "[MoltVoice] ask the AI: the model didn't load";
    check.problem = kLoadFailed;
    return check;
  }
  check.ready = true;
  return check;
}

void VoiceAiAnswers::Ask(const std::string& question,
                         Language language,
                         ProgressCallback progress,
                         DoneCallback done) {
  Cancel();
  language_ = language;
  done_ = std::move(done);
  run_ = ++next_run_;
  const AiQuestionPrompt prompt = BuildAiQuestionPrompt(question, language);
  if (prompt.user_message.empty()) {
    FinishSoon(Localized(language, kNoQuestion));
    return;
  }
  progress_ = std::move(progress);
  if (progress_) {
    progress_.Run(Localized(language, kStart));
  }
  still_working_timer_.Start(FROM_HERE, kStillWorking,
                             base::BindOnce(&VoiceAiAnswers::OnStillWorking,
                                            weak_factory_.GetWeakPtr(), run_));
  ChatPrompt chat;
  chat.system_prompt = prompt.system_prompt;
  chat.user_message = prompt.user_message;
  PromptOptions options;
  options.max_tokens = kAiAnswerMaxTokens;
  options.temperature = kTemperature;
  options.top_p = kTopP;
  options.reasoning = ReasoningMode::kOff;
  // One bounded string (constrained decoding).
  options.json_schema = std::string(AiAnswerSchema());
  if (HasModelForTesting()) {
    ModelCheck ready;
    ready.ready = true;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&VoiceAiAnswers::OnModelChecked,
                       weak_factory_.GetWeakPtr(), run_, std::move(chat),
                       std::move(options), std::move(ready)));
    return;
  }
  BrowserAIRuntime& runtime = BrowserAIRuntime::GetInstance();
  runtime.Initialize();
  runtime.StartMemoryPressureMonitoring();
  request_id_ = runtime.NewRequestId();
  options.request_id = request_id_;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&VoiceAiAnswers::EnsureModel),
      base::BindOnce(&VoiceAiAnswers::OnModelChecked,
                     weak_factory_.GetWeakPtr(), run_, std::move(chat),
                     std::move(options)));
}

void VoiceAiAnswers::OnModelChecked(int run,
                                    ChatPrompt prompt,
                                    PromptOptions options,
                                    ModelCheck check) {
  if (run != run_ || !done_) {
    return;
  }
  if (!check.ready) {
    Finish(
        Localized(language_, check.problem.empty() ? kNoModel : check.problem),
        /*model=*/false);
    return;
  }
  time_limit_timer_.Start(FROM_HERE, kTimeLimit,
                          base::BindOnce(&VoiceAiAnswers::OnTimeLimit,
                                         weak_factory_.GetWeakPtr(), run));
  Generate(run, std::move(prompt), std::move(options));
}

void VoiceAiAnswers::Generate(int run,
                              ChatPrompt prompt,
                              PromptOptions options) {
  if (HasModelForTesting()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<VoiceAiAnswers> self, int run,
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
  generating_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(
          [](ChatPrompt prompt, PromptOptions options) {
            BrowserAIRuntime& runtime = BrowserAIRuntime::GetInstance();
            base::ScopedClosureRunner release(
                base::BindOnce(&BrowserAIRuntime::FinishRequest,
                               base::Unretained(&runtime), options.request_id));
            return runtime.StreamChat(prompt, TokenCallback(), options);
          },
          std::move(prompt), std::move(options)),
      base::BindOnce(&VoiceAiAnswers::OnReply, weak_factory_.GetWeakPtr(),
                     run));
}

void VoiceAiAnswers::OnReply(int run, GenerationResult result) {
  if (run != run_ || !done_) {
    return;
  }
  request_id_ = 0;
  generating_ = false;
  if (!result.success || result.was_cancelled) {
    Finish(Localized(language_, kFailed), /*model=*/false);
    return;
  }
  // Model output: hostile.
  AiAnswer answer = AiAnswerFromModel(result.text, language_);
  Finish(std::move(answer.text), answer.kind == AiAnswer::Kind::kAnswer);
}

void VoiceAiAnswers::OnStillWorking(int run) {
  if (run == run_ && progress_) {
    progress_.Run(Localized(language_, kStillWorkingText));
  }
}

void VoiceAiAnswers::OnTimeLimit(int run) {
  if (run != run_ || !done_) {
    return;
  }
  CancelRequest();
  Finish(Localized(language_, kTooSlow), /*model=*/false);
}

void VoiceAiAnswers::Finish(std::string text, bool model) {
  still_working_timer_.Stop();
  time_limit_timer_.Stop();
  run_ = 0;
  CancelRequest();
  progress_.Reset();
  DoneCallback done = std::move(done_);
  if (done) {
    std::move(done).Run(SpeechSafeText(text, 2 * kMaxAiAnswerChars), model);
  }
}

void VoiceAiAnswers::FinishSoon(std::string text) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<VoiceAiAnswers> self, int run, std::string said) {
            if (self && run == self->run_ && self->done_) {
              self->Finish(std::move(said), /*model=*/false);
            }
          },
          weak_factory_.GetWeakPtr(), run_, std::move(text)));
}

void VoiceAiAnswers::CancelRequest() {
  if (request_id_ == 0) {
    return;
  }
  BrowserAIRuntime& runtime = BrowserAIRuntime::GetInstance();
  runtime.CancelRequest(request_id_);
  if (!generating_) {
    runtime.FinishRequest(request_id_);
  }
  request_id_ = 0;
  generating_ = false;
}

void VoiceAiAnswers::Cancel() {
  CancelRequest();
  still_working_timer_.Stop();
  time_limit_timer_.Stop();
  run_ = 0;
  progress_.Reset();
  done_.Reset();
}

}  // namespace voice
}  // namespace molt_ai
