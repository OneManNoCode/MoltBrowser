// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/molt_ai_manager.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_conversation.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_prompts.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_sessions.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_translation.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_translator_session.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/mojom/ai/ai_proofreader.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_rewriter.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_semantic_embedder.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_summarizer.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_writer.mojom.h"
#include "third_party/blink/public/mojom/on_device_translation/translator.mojom.h"

namespace molt_ai::web_ai {

namespace {

using blink::mojom::AIManagerCreateClientError;
using blink::mojom::CanCreateTranslatorResult;
using blink::mojom::CreateTranslatorError;
using blink::mojom::ModelAvailabilityCheckResult;

ModelAvailabilityCheckResult ToMojo(const AvailabilityDecision& decision) {
  switch (decision.availability) {
    case Availability::kAvailable:
      return ModelAvailabilityCheckResult::kAvailable;
    case Availability::kDownloadable:
      return ModelAvailabilityCheckResult::kDownloadable;
    case Availability::kUnavailable:
      break;
  }
  // availability() turns every kUnavailable* value into "unavailable", but
  // create() rejects with the value's own message, which page script reads
  // (ConvertModelAvailabilityCheckResultToDebugString at 152). So only the
  // reasons that follow from the request alone, the same for every user of
  // this build, get their own value. A policy, a device that can't run a
  // model, the switches, MoltNet and the user's block or a paused prompt
  // all answer alike (WEB_AI_APIS.md §4.3), and so does audio input without
  // a model that hears it (what the user downloaded).
  switch (decision.reason) {
    case UnavailableReason::kUnsupportedOptions:
      return ModelAvailabilityCheckResult::
          kUnavailableModelAdaptationNotAvailable;
    case UnavailableReason::kUnsupportedLanguage:
      return ModelAvailabilityCheckResult::kUnavailableUnsupportedLanguage;
    case UnavailableReason::kNone:
    case UnavailableReason::kDisabled:
    case UnavailableReason::kEnterprisePolicy:
    case UnavailableReason::kBlockedByUser:
    case UnavailableReason::kDeviceNotEligible:
    case UnavailableReason::kNoAudioModel:
      break;
  }
  return ModelAvailabilityCheckResult::kUnavailableFeatureNotEnabled;
}

void AnswerModelAvailability(
    base::OnceCallback<void(ModelAvailabilityCheckResult)> callback,
    AvailabilityDecision decision) {
  std::move(callback).Run(ToMojo(decision));
}

// Translator.availability(). "downloadable" is always
// kAfterDownloadTranslatorCreationRequired, the value upstream uses to mask
// installed language packs: nothing is downloaded here, and the value says
// nothing about what is installed. Blink turns every kNo* answer into
// "unavailable"; the reason only reaches the console.
CanCreateTranslatorResult ToTranslatorResult(
    const AvailabilityDecision& decision) {
  switch (decision.availability) {
    case Availability::kAvailable:
      return CanCreateTranslatorResult::kReadily;
    case Availability::kDownloadable:
      return CanCreateTranslatorResult::
          kAfterDownloadTranslatorCreationRequired;
    case Availability::kUnavailable:
      break;
  }
  return decision.reason == UnavailableReason::kUnsupportedLanguage
             ? CanCreateTranslatorResult::kNoNotSupportedLanguage
             : CanCreateTranslatorResult::kNoDisallowedByPolicy;
}

void AnswerTranslationAvailability(
    base::OnceCallback<void(CanCreateTranslatorResult)> callback,
    AvailabilityDecision decision) {
  std::move(callback).Run(ToTranslatorResult(decision));
}

// Empty means "not specified".
bool IsValidCode(const blink::mojom::AILanguageCodePtr& code) {
  return !code || code->code.empty() || IsValidLanguageTag(code->code);
}

bool AreValidCodes(const std::vector<blink::mojom::AILanguageCodePtr>& codes) {
  for (const auto& code : codes) {
    if (!IsValidCode(code)) {
      return false;
    }
  }
  return true;
}

std::string OutputLanguage(const blink::mojom::AILanguageCodePtr& code) {
  return code && IsValidLanguageTag(code->code) ? code->code : std::string();
}

template <typename OptionsPtr>
bool WritingLanguagesValid(const OptionsPtr& options) {
  return !options || (IsValidCode(options->output_language) &&
                      AreValidCodes(options->expected_input_languages) &&
                      AreValidCodes(options->expected_context_languages));
}

// What a LanguageModel's create options ask for.
struct LanguageModelOptionCheck {
  // Text (and, with the multimodal-input feature, images and audio) in,
  // text out, no tools.
  bool supported = true;
  bool languages_valid = true;
  // expectedInputs has "image": the session takes images, and create()
  // needs a model that sees them. availability() doesn't look at this: its
  // answer is the same with or without image input (WEB_AI_APIS.md §5.1).
  bool images = false;
  // expectedInputs has "audio": the session takes audio, and create() needs
  // a model that hears it. availability() looks at this
  // (AvailabilityInput::wants_audio, WEB_AI_APIS.md §5.2).
  bool audio = false;
  // The one output language asked for, if exactly one (by base language).
  std::string output_language;
};

// |context|: ContextFacts::image_input_enabled and audio_input_enabled.
LanguageModelOptionCheck CheckLanguageModelOptions(
    const blink::mojom::AILanguageModelCreateOptionsPtr& options,
    const ContextFacts& context) {
  const bool image_input_enabled = context.image_input_enabled;
  const bool audio_input_enabled = context.audio_input_enabled;
  LanguageModelOptionCheck check;
  if (!options) {
    return check;
  }
  if (options->tools.has_value() && !options->tools->empty()) {
    check.supported = false;
  }
  auto check_expected =
      [&check, image_input_enabled, audio_input_enabled](
          const std::optional<
              std::vector<blink::mojom::AILanguageModelExpectedPtr>>& list,
          bool inputs, std::vector<std::string>* languages) {
        if (!list.has_value()) {
          return;
        }
        for (const auto& expected : *list) {
          if (!expected) {
            check.supported = false;
            continue;
          }
          if (inputs && image_input_enabled &&
              expected->type ==
                  blink::mojom::AILanguageModelPromptType::kImage) {
            check.images = true;
          } else if (inputs && audio_input_enabled &&
                     expected->type ==
                         blink::mojom::AILanguageModelPromptType::kAudio) {
            check.audio = true;
          } else if (expected->type !=
                     blink::mojom::AILanguageModelPromptType::kText) {
            // Tool calls and results; any output but text.
            check.supported = false;
            continue;
          }
          if (!expected->languages.has_value()) {
            continue;
          }
          for (const auto& code : *expected->languages) {
            if (!IsValidCode(code)) {
              check.languages_valid = false;
            } else if (languages && code && !code->code.empty()) {
              languages->push_back(code->code);
            }
          }
        }
      };
  std::vector<std::string> outputs;
  check_expected(options->expected_inputs, /*inputs=*/true, nullptr);
  check_expected(options->expected_outputs, /*inputs=*/false, &outputs);
  if (!outputs.empty()) {
    const std::string base = BaseLanguage(outputs.front());
    bool one = true;
    for (const std::string& tag : outputs) {
      one = one && BaseLanguage(tag) == base;
    }
    if (one) {
      check.output_language = outputs.front();
    }
  }
  return check;
}

// The initial prompts: an optional leading system message, then text turns.
// False if they hold anything else.
bool ParseInitialPrompts(
    const std::vector<blink::mojom::AILanguageModelPromptPtr>& prompts,
    std::string* system_prompt,
    std::vector<Turn>* turns) {
  for (size_t i = 0; i < prompts.size(); ++i) {
    const auto& prompt = prompts[i];
    if (!prompt || prompt->is_prefix) {
      return false;
    }
    std::string text;
    for (const auto& content : prompt->content) {
      if (!content || !content->is_text()) {
        return false;
      }
      if (!text.empty()) {
        text += "\n";
      }
      text += content->get_text();
    }
    text = molt_ai::ToDisplayText(text);
    switch (prompt->role) {
      case blink::mojom::AILanguageModelPromptRole::kSystem:
        if (i != 0) {
          return false;
        }
        *system_prompt = std::move(text);
        break;
      case blink::mojom::AILanguageModelPromptRole::kUser:
        turns->push_back(MakeTurn(Role::kUser, std::move(text)));
        break;
      case blink::mojom::AILanguageModelPromptRole::kAssistant:
        turns->push_back(MakeTurn(Role::kAssistant, std::move(text)));
        break;
    }
  }
  return true;
}

size_t InitialPromptBytes(
    const std::vector<blink::mojom::AILanguageModelPromptPtr>& prompts) {
  size_t bytes = 0;
  for (const auto& prompt : prompts) {
    if (!prompt) {
      continue;
    }
    for (const auto& content : prompt->content) {
      if (content && content->is_text()) {
        bytes += content->get_text().size();
      }
    }
  }
  return bytes;
}

Sampling SamplingFor(
    const blink::mojom::AILanguageModelCreateOptionsPtr& options) {
  if (options && options->sampling_mode.has_value()) {
    switch (*options->sampling_mode) {
      case blink::mojom::AILanguageModelSamplingMode::kMostPredictable:
        return SamplingForMode(SamplingMode::kMostPredictable);
      case blink::mojom::AILanguageModelSamplingMode::kPredictable:
        return SamplingForMode(SamplingMode::kPredictable);
      case blink::mojom::AILanguageModelSamplingMode::kSlightlyPredictable:
        return SamplingForMode(SamplingMode::kSlightlyPredictable);
      case blink::mojom::AILanguageModelSamplingMode::kBalanced:
        return SamplingForMode(SamplingMode::kBalanced);
      case blink::mojom::AILanguageModelSamplingMode::kSlightlyCreative:
        return SamplingForMode(SamplingMode::kSlightlyCreative);
      case blink::mojom::AILanguageModelSamplingMode::kCreative:
        return SamplingForMode(SamplingMode::kCreative);
      case blink::mojom::AILanguageModelSamplingMode::kMostCreative:
        return SamplingForMode(SamplingMode::kMostCreative);
    }
  }
  if (options && options->sampling_params) {
    return ClampSampling(options->sampling_params->top_k,
                         options->sampling_params->temperature);
  }
  return kDefaultSampling;
}

SummarizerOptions ToSummarizerOptions(
    const blink::mojom::AISummarizerCreateOptions& o) {
  SummarizerOptions out;
  switch (o.type) {
    case blink::mojom::AISummarizerType::kTLDR:
      out.type = SummarizerType::kTldr;
      break;
    case blink::mojom::AISummarizerType::kKeyPoints:
      out.type = SummarizerType::kKeyPoints;
      break;
    case blink::mojom::AISummarizerType::kTeaser:
      out.type = SummarizerType::kTeaser;
      break;
    case blink::mojom::AISummarizerType::kHeadline:
      out.type = SummarizerType::kHeadline;
      break;
  }
  out.format = o.format == blink::mojom::AISummarizerFormat::kPlainText
                   ? TextFormat::kPlainText
                   : TextFormat::kMarkdown;
  switch (o.length) {
    case blink::mojom::AISummarizerLength::kShort:
      out.length = SummarizerLength::kShort;
      break;
    case blink::mojom::AISummarizerLength::kMedium:
      out.length = SummarizerLength::kMedium;
      break;
    case blink::mojom::AISummarizerLength::kLong:
      out.length = SummarizerLength::kLong;
      break;
  }
  out.output_language = OutputLanguage(o.output_language);
  return out;
}

WriterOptions ToWriterOptions(const blink::mojom::AIWriterCreateOptions& o) {
  WriterOptions out;
  switch (o.tone) {
    case blink::mojom::AIWriterTone::kFormal:
      out.tone = WriterTone::kFormal;
      break;
    case blink::mojom::AIWriterTone::kNeutral:
      out.tone = WriterTone::kNeutral;
      break;
    case blink::mojom::AIWriterTone::kCasual:
      out.tone = WriterTone::kCasual;
      break;
  }
  out.format = o.format == blink::mojom::AIWriterFormat::kPlainText
                   ? TextFormat::kPlainText
                   : TextFormat::kMarkdown;
  switch (o.length) {
    case blink::mojom::AIWriterLength::kShort:
      out.length = WriterLength::kShort;
      break;
    case blink::mojom::AIWriterLength::kMedium:
      out.length = WriterLength::kMedium;
      break;
    case blink::mojom::AIWriterLength::kLong:
      out.length = WriterLength::kLong;
      break;
  }
  out.output_language = OutputLanguage(o.output_language);
  return out;
}

RewriterOptions ToRewriterOptions(
    const blink::mojom::AIRewriterCreateOptions& o) {
  RewriterOptions out;
  switch (o.tone) {
    case blink::mojom::AIRewriterTone::kAsIs:
      out.tone = RewriterTone::kAsIs;
      break;
    case blink::mojom::AIRewriterTone::kMoreFormal:
      out.tone = RewriterTone::kMoreFormal;
      break;
    case blink::mojom::AIRewriterTone::kMoreCasual:
      out.tone = RewriterTone::kMoreCasual;
      break;
  }
  switch (o.format) {
    case blink::mojom::AIRewriterFormat::kAsIs:
      out.format = RewriterFormat::kAsIs;
      break;
    case blink::mojom::AIRewriterFormat::kPlainText:
      out.format = RewriterFormat::kPlainText;
      break;
    case blink::mojom::AIRewriterFormat::kMarkdown:
      out.format = RewriterFormat::kMarkdown;
      break;
  }
  switch (o.length) {
    case blink::mojom::AIRewriterLength::kAsIs:
      out.length = RewriterLength::kAsIs;
      break;
    case blink::mojom::AIRewriterLength::kShorter:
      out.length = RewriterLength::kShorter;
      break;
    case blink::mojom::AIRewriterLength::kLonger:
      out.length = RewriterLength::kLonger;
      break;
  }
  out.output_language = OutputLanguage(o.output_language);
  return out;
}

// The shared context a writing-assistance create() carries: sanitized, and
// nullopt (with the requested count) if it exceeds the quota by itself.
std::optional<std::string> SharedContext(
    const std::optional<std::string>& shared_context,
    uint32_t* requested) {
  *requested = 0;
  if (!shared_context.has_value() || shared_context->empty()) {
    return std::string();
  }
  if (ExceedsQuotaBySize(shared_context->size(),
                         kWritingAssistanceInputQuota)) {
    *requested = TokensForSize(shared_context->size());
    return std::nullopt;
  }
  *requested = EstimateTokens(*shared_context);
  if (*requested > kWritingAssistanceInputQuota) {
    return std::nullopt;
  }
  return molt_ai::ToDisplayText(*shared_context);
}

}  // namespace

// ---------------------------------------------------------------------------
// Create requests
// ---------------------------------------------------------------------------

class MoltAIManager::CreateRequest {
 public:
  CreateRequest(WebAIApi api, bool options_supported, bool languages_supported)
      : api_(api),
        options_supported_(options_supported),
        languages_supported_(languages_supported) {}
  CreateRequest(const CreateRequest&) = delete;
  CreateRequest& operator=(const CreateRequest&) = delete;
  virtual ~CreateRequest() = default;

  // The page still waits (create() not aborted, document alive).
  virtual bool IsWanted() const = 0;
  virtual void Fail(AIManagerCreateClientError error,
                    blink::mojom::QuotaErrorInfoPtr quota) = 0;
  // The API is unavailable to the page for |reason| (DecideAvailability()).
  // The renderer asks for availability before create() and rejects an
  // unsupported language itself (NotSupportedError, as availability()
  // reported it), so a create() that still gets here fails like any other.
  virtual void Reject(UnavailableReason reason) {
    Fail(AIManagerCreateClientError::kUnableToCreateSession, nullptr);
  }
  // The user allowed the site and the model is loaded: make the session.
  // |sees_images|: the loaded model takes images; |hears_audio|: audio.
  virtual void Finish(MoltAIManager& manager,
                      uint32_t context_tokens,
                      bool sees_images,
                      bool hears_audio) = 0;

  // The session will take images: a model that sees them is preferred
  // when one has to be loaded.
  virtual bool wants_images() const { return false; }
  // The session will take audio: availability() looks at it, and a model
  // that hears audio is preferred when one has to be loaded.
  virtual bool wants_audio() const { return false; }

  WebAIApi api() const { return api_; }
  bool options_supported() const { return options_supported_; }
  bool languages_supported() const { return languages_supported_; }
  const DeviceFacts& device() const { return device_; }
  void set_device(const DeviceFacts& device) { device_ = device; }

 private:
  const WebAIApi api_;
  const bool options_supported_;
  const bool languages_supported_;
  DeviceFacts device_;
};

class MoltAIManager::LanguageModelCreate : public CreateRequest {
 public:
  using Client = blink::mojom::AIManagerCreateLanguageModelClient;

  LanguageModelCreate(mojo::PendingRemote<Client> client,
                      blink::mojom::AILanguageModelCreateOptionsPtr options,
                      const LanguageModelOptionCheck& check)
      : CreateRequest(WebAIApi::kLanguageModel,
                      check.supported,
                      check.languages_valid),
        client_(std::move(client)),
        options_(std::move(options)),
        output_language_(check.output_language),
        images_(check.images),
        audio_(check.audio) {}

  bool IsWanted() const override { return client_.is_connected(); }
  bool wants_images() const override { return images_; }
  bool wants_audio() const override { return audio_; }

  void Fail(AIManagerCreateClientError error,
            blink::mojom::QuotaErrorInfoPtr quota) override {
    client_->OnError(error, std::move(quota));
  }

  void Finish(MoltAIManager& manager,
              uint32_t context_tokens,
              bool sees_images,
              bool hears_audio) override {
    // Image input on a model that can't see images. Only a site the user
    // allowed gets this far, and it learns no more than its first prompt
    // with an image would tell it; availability() said the same either way.
    // Audio input on a model that can't hear it: the model loaded isn't the
    // one that hears audio (the chat's, loaded first).
    if ((images_ && !sees_images) || (audio_ && !hears_audio)) {
      Fail(AIManagerCreateClientError::kUnableToCreateSession, nullptr);
      return;
    }
    const LanguageModelLimits limits = LanguageModelLimitsFor(context_tokens);
    const uint32_t quota = limits.input_quota;
    const size_t bytes = InitialPromptBytes(options_->initial_prompts);
    if (ExceedsQuotaBySize(bytes, quota)) {
      Fail(AIManagerCreateClientError::kInitialInputTooLarge,
           blink::mojom::QuotaErrorInfo::New(TokensForSize(bytes), quota));
      return;
    }
    std::string system_prompt;
    std::vector<Turn> turns;
    if (!ParseInitialPrompts(options_->initial_prompts, &system_prompt,
                             &turns)) {
      Fail(AIManagerCreateClientError::kUnableToCreateSession, nullptr);
      return;
    }
    Conversation conversation(quota);
    if (!system_prompt.empty()) {
      const uint32_t system_tokens = EstimateMessageTokens(system_prompt);
      if (!conversation.SetSystemPrompt(std::move(system_prompt))) {
        Fail(AIManagerCreateClientError::kInitialInputTooLarge,
             blink::mojom::QuotaErrorInfo::New(system_tokens, quota));
        return;
      }
    }
    const Conversation::Plan plan = conversation.PlanAdd(turns);
    if (!plan.fits) {
      Fail(AIManagerCreateClientError::kInitialInputTooLarge,
           blink::mojom::QuotaErrorInfo::New(plan.requested, quota));
      return;
    }
    conversation.Add(plan, std::move(turns));

    LanguageModelSession::Config config;
    config.sampling = SamplingFor(options_);
    config.sampling_mode = options_->sampling_mode;
    config.max_reply_tokens = limits.max_reply_tokens;
    config.system_prompt = BuildLanguageModelSystemPrompt(output_language_);
    config.image_input = images_;
    config.audio_input = audio_;

    const uint64_t id = manager.ReserveSession();
    if (id == 0) {
      Fail(AIManagerCreateClientError::kUnableToCreateSession, nullptr);
      return;
    }
    mojo::PendingRemote<blink::mojom::AILanguageModel> remote;
    auto session = std::make_unique<LanguageModelSession>(
        manager, id, std::move(config), std::move(conversation),
        remote.InitWithNewPipeAndPassReceiver());
    blink::mojom::AILanguageModelInstanceInfoPtr info = session->GetInfo();
    manager.AdoptSession(std::move(session));
    client_->OnResult(std::move(remote), std::move(info));
  }

 private:
  mojo::Remote<Client> client_;
  blink::mojom::AILanguageModelCreateOptionsPtr options_;
  const std::string output_language_;
  const bool images_;
  const bool audio_;
};

// Summarizer, Writer and Rewriter.
template <typename Client,
          typename SessionInterface,
          typename Session,
          typename Options>
class MoltAIManager::WritingCreate : public CreateRequest {
 public:
  WritingCreate(WebAIApi api,
                mojo::PendingRemote<Client> client,
                std::string shared_context,
                Options options,
                bool languages_valid)
      : CreateRequest(api, /*options_supported=*/true, languages_valid),
        client_(std::move(client)),
        shared_context_(std::move(shared_context)),
        options_(std::move(options)) {}

  bool IsWanted() const override { return client_.is_connected(); }

  void Fail(AIManagerCreateClientError error,
            blink::mojom::QuotaErrorInfoPtr quota) override {
    client_->OnError(error, std::move(quota));
  }

  void Finish(MoltAIManager& manager,
              uint32_t context_tokens,
              bool sees_images,
              bool hears_audio) override {
    const uint64_t id = manager.ReserveSession();
    if (id == 0) {
      Fail(AIManagerCreateClientError::kUnableToCreateSession, nullptr);
      return;
    }
    mojo::PendingRemote<SessionInterface> remote;
    manager.AdoptSession(std::make_unique<Session>(
        manager, id, std::move(shared_context_), std::move(options_),
        remote.InitWithNewPipeAndPassReceiver()));
    // The page's inputQuota: the same fixed number for every model and
    // every page, never the loaded model's context window.
    client_->OnResult(std::move(remote), kWritingAssistanceInputQuota);
  }

 private:
  mojo::Remote<Client> client_;
  std::string shared_context_;
  Options options_;
};

// Translator.create(). The languages were resolved against the fixed table
// before the request was made.
class MoltAIManager::TranslatorCreate : public CreateRequest {
 public:
  using Client = blink::mojom::TranslationManagerCreateTranslatorClient;

  TranslatorCreate(mojo::PendingRemote<Client> client, TranslatorPair pair)
      : CreateRequest(WebAIApi::kTranslator,
                      /*options_supported=*/true,
                      /*languages_supported=*/true),
        client_(std::move(client)),
        pair_(pair) {}

  bool IsWanted() const override { return client_.is_connected(); }

  void Fail(AIManagerCreateClientError error,
            blink::mojom::QuotaErrorInfoPtr quota) override {
    FailWith(CreateTranslatorError::kFailedToCreateTranslator);
  }

  void Reject(UnavailableReason reason) override {
    switch (reason) {
      case UnavailableReason::kEnterprisePolicy:
        FailWith(CreateTranslatorError::kDisallowedByPolicy);
        return;
      case UnavailableReason::kUnsupportedLanguage:
        FailWith(CreateTranslatorError::kNotSupportedLanguage);
        return;
      case UnavailableReason::kNone:
      case UnavailableReason::kDisabled:
      case UnavailableReason::kUnsupportedOptions:
      case UnavailableReason::kBlockedByUser:
      case UnavailableReason::kDeviceNotEligible:
      case UnavailableReason::kNoAudioModel:
        break;
    }
    FailWith(CreateTranslatorError::kFailedToCreateTranslator);
  }

  void Finish(MoltAIManager& manager,
              uint32_t context_tokens,
              bool sees_images,
              bool hears_audio) override {
    const uint64_t id = manager.ReserveSession();
    if (id == 0) {
      FailWith(CreateTranslatorError::kExceedsServiceCountLimitation);
      return;
    }
    mojo::PendingRemote<blink::mojom::Translator> remote;
    manager.AdoptSession(std::make_unique<TranslatorSession>(
        manager, id, pair_, remote.InitWithNewPipeAndPassReceiver()));
    // The page's Translator reports the table's codes ("en" for "en-US").
    client_->OnResult(
        blink::mojom::CreateTranslatorResult::NewTranslator(std::move(remote)),
        blink::mojom::TranslatorLanguageCode::New(pair_.source->code),
        blink::mojom::TranslatorLanguageCode::New(pair_.target->code));
  }

 private:
  // Blink CHECKs that an error comes without language codes.
  void FailWith(CreateTranslatorError error) {
    client_->OnResult(blink::mojom::CreateTranslatorResult::NewError(error),
                      nullptr, nullptr);
  }

  mojo::Remote<Client> client_;
  const TranslatorPair pair_;
};

// ---------------------------------------------------------------------------
// MoltAIManager
// ---------------------------------------------------------------------------

MoltAIManager::MoltAIManager(std::unique_ptr<WebAIEnvironment> environment,
                             WebAIBackend* backend)
    : environment_(std::move(environment)), backend_(backend) {
  environment_->ObserveChanges(base::BindRepeating(
      &MoltAIManager::OnAccessChanged, weak_factory_.GetWeakPtr()));
}

MoltAIManager::~MoltAIManager() {
  // Sessions cancel their generations and release their site slots while
  // the backend and the environment are still here.
  sessions_.clear();
  creates_.clear();
}

void MoltAIManager::AddReceiver(
    mojo::PendingReceiver<blink::mojom::AIManager> receiver) {
  receivers_.Add(this, std::move(receiver));
}

void MoltAIManager::AddTranslationReceiver(
    mojo::PendingReceiver<blink::mojom::TranslationManager> receiver) {
  translation_receivers_.Add(this, std::move(receiver));
}

void MoltAIManager::OnAccessChanged() {
  if (sessions_.empty()) {
    return;
  }
  // Not now: the change may come from code a session is called from.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&MoltAIManager::CloseSessionsNoLongerServing,
                                weak_factory_.GetWeakPtr()));
}

void MoltAIManager::CloseSessionsNoLongerServing() {
  // Turned off (the answer taken back, a switch, a policy; the Translator's
  // own policy ends only Translators): each such session cancels its
  // generation and closes its pipe, so the page's calls fail; create() is
  // decided again.
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (MayServe(it->second->api())) {
      ++it;
    } else {
      it = sessions_.erase(it);
    }
  }
}

const std::string& MoltAIManager::site_key() {
  if (!site_key_) {
    site_key_ = environment_->GetSiteKey();
  }
  return *site_key_;
}

bool MoltAIManager::IsForeground() {
  return environment_->IsForeground();
}

bool MoltAIManager::MayServe(WebAIApi api) {
  return MaySessionServe(environment_->GetContextFacts(api),
                         backend_->WebApisSwitchOn(),
                         environment_->GetConsent());
}

// ---- availability() ----

void MoltAIManager::CheckAvailability(WebAIApi api,
                                      bool options_supported,
                                      bool languages_supported,
                                      bool wants_audio,
                                      AvailabilityCallback callback) {
  const ContextFacts context = environment_->GetContextFacts(api);
  if (!IsContextEligible(context)) {
    // Answered without asking the runtime anything.
    AvailabilityInput input;
    input.context = context;
    std::move(callback).Run(DecideAvailability(input));
    return;
  }
  // The site's own device facts: how long this takes doesn't depend on
  // other sites' calls.
  backend_->GetDeviceFactsForSite(
      site_key(),
      base::BindOnce(&MoltAIManager::OnAvailabilityDeviceFacts,
                     weak_factory_.GetWeakPtr(), api, options_supported,
                     languages_supported, wants_audio, std::move(callback)));
}

void MoltAIManager::OnAvailabilityDeviceFacts(WebAIApi api,
                                              bool options_supported,
                                              bool languages_supported,
                                              bool wants_audio,
                                              AvailabilityCallback callback,
                                              DeviceFacts device) {
  AvailabilityInput input;
  input.context = environment_->GetContextFacts(api);
  input.device = device;
  input.options_supported = options_supported;
  input.languages_supported = languages_supported;
  input.wants_audio = wants_audio;
  input.consent = environment_->GetConsent();
  std::move(callback).Run(DecideAvailability(input));
}

void MoltAIManager::CanCreateLanguageModel(
    blink::mojom::AILanguageModelCreateOptionsPtr options,
    CanCreateLanguageModelCallback callback) {
  if (!environment_->IsAllowedByPermissionsPolicy(WebAIApi::kLanguageModel)) {
    receivers_.ReportBadMessage("Permissions policy disabled");
    return;
  }
  const LanguageModelOptionCheck check = CheckLanguageModelOptions(
      options, environment_->GetContextFacts(WebAIApi::kLanguageModel));
  CheckAvailability(
      WebAIApi::kLanguageModel, check.supported, check.languages_valid,
      check.audio,
      base::BindOnce(&AnswerModelAvailability, std::move(callback)));
}

void MoltAIManager::CanCreateSummarizer(
    blink::mojom::AISummarizerCreateOptionsPtr options,
    CanCreateSummarizerCallback callback) {
  if (!environment_->IsAllowedByPermissionsPolicy(WebAIApi::kSummarizer)) {
    receivers_.ReportBadMessage("Permissions policy disabled");
    return;
  }
  CheckAvailability(
      WebAIApi::kSummarizer, /*options_supported=*/true,
      WritingLanguagesValid(options), /*wants_audio=*/false,
      base::BindOnce(&AnswerModelAvailability, std::move(callback)));
}

void MoltAIManager::CanCreateWriter(
    blink::mojom::AIWriterCreateOptionsPtr options,
    CanCreateWriterCallback callback) {
  if (!environment_->IsAllowedByPermissionsPolicy(WebAIApi::kWriter)) {
    receivers_.ReportBadMessage("Permissions policy disabled");
    return;
  }
  CheckAvailability(
      WebAIApi::kWriter, /*options_supported=*/true,
      WritingLanguagesValid(options), /*wants_audio=*/false,
      base::BindOnce(&AnswerModelAvailability, std::move(callback)));
}

void MoltAIManager::CanCreateRewriter(
    blink::mojom::AIRewriterCreateOptionsPtr options,
    CanCreateRewriterCallback callback) {
  if (!environment_->IsAllowedByPermissionsPolicy(WebAIApi::kRewriter)) {
    receivers_.ReportBadMessage("Permissions policy disabled");
    return;
  }
  CheckAvailability(
      WebAIApi::kRewriter, /*options_supported=*/true,
      WritingLanguagesValid(options), /*wants_audio=*/false,
      base::BindOnce(&AnswerModelAvailability, std::move(callback)));
}

void MoltAIManager::CanCreateProofreader(
    blink::mojom::AIProofreaderCreateOptionsPtr options,
    CanCreateProofreaderCallback callback) {
  std::move(callback).Run(
      ModelAvailabilityCheckResult::kUnavailableFeatureNotEnabled);
}

void MoltAIManager::CanCreateSemanticEmbedder(
    CanCreateSemanticEmbedderCallback callback) {
  std::move(callback).Run(
      ModelAvailabilityCheckResult::kUnavailableFeatureNotEnabled);
}

void MoltAIManager::GetLanguageModelParams(
    GetLanguageModelParamsCallback callback) {
  if (!IsContextEligible(
          environment_->GetContextFacts(WebAIApi::kLanguageModel))) {
    std::move(callback).Run(nullptr);
    return;
  }
  // The same for every user and model.
  std::move(callback).Run(blink::mojom::AILanguageModelParams::New(
      blink::mojom::AILanguageModelSamplingParams::New(
          kDefaultSampling.top_k, kDefaultSampling.temperature),
      blink::mojom::AILanguageModelSamplingParams::New(
          kMaxSampling.top_k, kMaxSampling.temperature)));
}

// ---- create() ----

void MoltAIManager::CreateLanguageModel(
    mojo::PendingRemote<blink::mojom::AIManagerCreateLanguageModelClient>
        client,
    blink::mojom::AILanguageModelCreateOptionsPtr options,
    mojo::PendingRemote<on_device_model::mojom::DownloadObserver> monitor) {
  // Blink checks these before create(); failing them is a bad renderer.
  if (!environment_->GetContextFacts(WebAIApi::kLanguageModel)
           .feature_enabled) {
    receivers_.ReportBadMessage("Feature not enabled");
    return;
  }
  if (!environment_->IsAllowedByPermissionsPolicy(WebAIApi::kLanguageModel)) {
    receivers_.ReportBadMessage("Permissions policy disabled");
    return;
  }
  if (!options) {
    options = blink::mojom::AILanguageModelCreateOptions::New();
  }
  const LanguageModelOptionCheck check = CheckLanguageModelOptions(
      options, environment_->GetContextFacts(WebAIApi::kLanguageModel));
  // Requests that can never succeed fail before anyone is asked anything.
  std::string system_prompt;
  std::vector<Turn> turns;
  const size_t bytes = InitialPromptBytes(options->initial_prompts);
  if (ExceedsQuotaBySize(bytes, kLargeInputQuota)) {
    mojo::Remote<blink::mojom::AIManagerCreateLanguageModelClient> remote(
        std::move(client));
    remote->OnError(
        AIManagerCreateClientError::kInitialInputTooLarge,
        blink::mojom::QuotaErrorInfo::New(TokensForSize(bytes),
                                          kLargeInputQuota));
    return;
  }
  if (!ParseInitialPrompts(options->initial_prompts, &system_prompt,
                           &turns)) {
    mojo::Remote<blink::mojom::AIManagerCreateLanguageModelClient> remote(
        std::move(client));
    remote->OnError(AIManagerCreateClientError::kUnableToCreateSession,
                    nullptr);
    return;
  }
  // The renderer reports download progress itself (0 and 1 on success);
  // nothing is downloaded on a page's behalf, so |monitor| stays unused.
  StartCreate(std::make_unique<LanguageModelCreate>(
      std::move(client), std::move(options), check));
}

void MoltAIManager::CreateSummarizer(
    mojo::PendingRemote<blink::mojom::AIManagerCreateSummarizerClient> client,
    blink::mojom::AISummarizerCreateOptionsPtr options,
    mojo::PendingRemote<on_device_model::mojom::DownloadObserver> monitor) {
  if (!environment_->GetContextFacts(WebAIApi::kSummarizer).feature_enabled) {
    receivers_.ReportBadMessage("Feature not enabled");
    return;
  }
  if (!environment_->IsAllowedByPermissionsPolicy(WebAIApi::kSummarizer)) {
    receivers_.ReportBadMessage("Permissions policy disabled");
    return;
  }
  if (!options) {
    options = blink::mojom::AISummarizerCreateOptions::New();
  }
  uint32_t requested = 0;
  std::optional<std::string> shared =
      SharedContext(options->shared_context, &requested);
  if (!shared) {
    mojo::Remote<blink::mojom::AIManagerCreateSummarizerClient> remote(
        std::move(client));
    remote->OnError(AIManagerCreateClientError::kInitialInputTooLarge,
                    blink::mojom::QuotaErrorInfo::New(
                        requested, kWritingAssistanceInputQuota));
    return;
  }
  using Request =
      WritingCreate<blink::mojom::AIManagerCreateSummarizerClient,
                    blink::mojom::AISummarizer, SummarizerSession,
                    SummarizerOptions>;
  StartCreate(std::make_unique<Request>(
      WebAIApi::kSummarizer, std::move(client), std::move(*shared),
      ToSummarizerOptions(*options), WritingLanguagesValid(options)));
}

void MoltAIManager::CreateWriter(
    mojo::PendingRemote<blink::mojom::AIManagerCreateWriterClient> client,
    blink::mojom::AIWriterCreateOptionsPtr options,
    mojo::PendingRemote<on_device_model::mojom::DownloadObserver> monitor) {
  if (!environment_->GetContextFacts(WebAIApi::kWriter).feature_enabled) {
    receivers_.ReportBadMessage("Feature not enabled");
    return;
  }
  if (!environment_->IsAllowedByPermissionsPolicy(WebAIApi::kWriter)) {
    receivers_.ReportBadMessage("Permissions policy disabled");
    return;
  }
  if (!options) {
    options = blink::mojom::AIWriterCreateOptions::New();
  }
  uint32_t requested = 0;
  std::optional<std::string> shared =
      SharedContext(options->shared_context, &requested);
  if (!shared) {
    mojo::Remote<blink::mojom::AIManagerCreateWriterClient> remote(
        std::move(client));
    remote->OnError(AIManagerCreateClientError::kInitialInputTooLarge,
                    blink::mojom::QuotaErrorInfo::New(
                        requested, kWritingAssistanceInputQuota));
    return;
  }
  using Request =
      WritingCreate<blink::mojom::AIManagerCreateWriterClient,
                    blink::mojom::AIWriter, WriterSession, WriterOptions>;
  StartCreate(std::make_unique<Request>(
      WebAIApi::kWriter, std::move(client), std::move(*shared),
      ToWriterOptions(*options), WritingLanguagesValid(options)));
}

void MoltAIManager::CreateRewriter(
    mojo::PendingRemote<blink::mojom::AIManagerCreateRewriterClient> client,
    blink::mojom::AIRewriterCreateOptionsPtr options,
    mojo::PendingRemote<on_device_model::mojom::DownloadObserver> monitor) {
  if (!environment_->GetContextFacts(WebAIApi::kRewriter).feature_enabled) {
    receivers_.ReportBadMessage("Feature not enabled");
    return;
  }
  if (!environment_->IsAllowedByPermissionsPolicy(WebAIApi::kRewriter)) {
    receivers_.ReportBadMessage("Permissions policy disabled");
    return;
  }
  if (!options) {
    options = blink::mojom::AIRewriterCreateOptions::New();
  }
  uint32_t requested = 0;
  std::optional<std::string> shared =
      SharedContext(options->shared_context, &requested);
  if (!shared) {
    mojo::Remote<blink::mojom::AIManagerCreateRewriterClient> remote(
        std::move(client));
    remote->OnError(AIManagerCreateClientError::kInitialInputTooLarge,
                    blink::mojom::QuotaErrorInfo::New(
                        requested, kWritingAssistanceInputQuota));
    return;
  }
  using Request =
      WritingCreate<blink::mojom::AIManagerCreateRewriterClient,
                    blink::mojom::AIRewriter, RewriterSession,
                    RewriterOptions>;
  StartCreate(std::make_unique<Request>(
      WebAIApi::kRewriter, std::move(client), std::move(*shared),
      ToRewriterOptions(*options), WritingLanguagesValid(options)));
}

void MoltAIManager::CreateProofreader(
    mojo::PendingRemote<blink::mojom::AIManagerCreateProofreaderClient> client,
    blink::mojom::AIProofreaderCreateOptionsPtr options,
    mojo::PendingRemote<on_device_model::mojom::DownloadObserver> monitor) {
  mojo::Remote<blink::mojom::AIManagerCreateProofreaderClient> remote(
      std::move(client));
  remote->OnError(AIManagerCreateClientError::kUnableToCreateSession,
                  nullptr);
}

void MoltAIManager::CreateSemanticEmbedder(
    mojo::PendingRemote<blink::mojom::AIManagerCreateSemanticEmbedderClient>
        client,
    mojo::PendingRemote<on_device_model::mojom::DownloadObserver> monitor) {
  mojo::Remote<blink::mojom::AIManagerCreateSemanticEmbedderClient> remote(
      std::move(client));
  remote->OnError(AIManagerCreateClientError::kUnableToCreateSession);
}

// ---- Translator ----

void MoltAIManager::TranslationAvailable(
    blink::mojom::TranslatorLanguageCodePtr source_lang,
    blink::mojom::TranslatorLanguageCodePtr target_lang,
    TranslationAvailableCallback callback) {
  // Blink answers "unavailable" itself where the "translator" permissions
  // policy is off.
  if (!environment_->IsAllowedByPermissionsPolicy(WebAIApi::kTranslator)) {
    translation_receivers_.ReportBadMessage("Permissions policy disabled");
    return;
  }
  // The pair decides by itself, from the fixed table: never the installed
  // models, the OS language or Accept-Language.
  const bool supported =
      source_lang && target_lang &&
      ResolveTranslatorPair(source_lang->code, target_lang->code).has_value();
  CheckAvailability(
      WebAIApi::kTranslator, /*options_supported=*/true, supported,
      /*wants_audio=*/false,
      base::BindOnce(&AnswerTranslationAvailability, std::move(callback)));
}

void MoltAIManager::CreateTranslator(
    mojo::PendingRemote<blink::mojom::TranslationManagerCreateTranslatorClient>
        client,
    blink::mojom::TranslatorCreateOptionsPtr options) {
  // Blink checks these before create(); failing them is a bad renderer.
  if (!environment_->GetContextFacts(WebAIApi::kTranslator).feature_enabled) {
    translation_receivers_.ReportBadMessage("Feature not enabled");
    return;
  }
  if (!environment_->IsAllowedByPermissionsPolicy(WebAIApi::kTranslator)) {
    translation_receivers_.ReportBadMessage("Permissions policy disabled");
    return;
  }
  std::optional<TranslatorPair> pair;
  if (options && options->source_lang && options->target_lang) {
    pair = ResolveTranslatorPair(options->source_lang->code,
                                 options->target_lang->code);
  }
  if (!pair) {
    // Blink asked availability() first, so only a racing or bad renderer
    // gets here; nobody is asked anything.
    mojo::Remote<blink::mojom::TranslationManagerCreateTranslatorClient>
        remote(std::move(client));
    remote->OnResult(blink::mojom::CreateTranslatorResult::NewError(
                         CreateTranslatorError::kNotSupportedLanguage),
                     nullptr, nullptr);
    return;
  }
  // options->observer_remote stays unused: nothing is downloaded on a
  // page's behalf, and the renderer reports progress 0 and 1 itself.
  StartCreate(std::make_unique<TranslatorCreate>(std::move(client), *pair));
}

void MoltAIManager::StartCreate(std::unique_ptr<CreateRequest> request) {
  if (creates_.size() >= kMaxPendingCreates) {
    request->Fail(AIManagerCreateClientError::kUnableToCreateSession,
                  nullptr);
    return;
  }
  const uint64_t id = next_create_id_++;
  creates_[id] = std::move(request);
  backend_->GetDeviceFactsForSite(
      site_key(), base::BindOnce(&MoltAIManager::OnCreateDeviceFacts,
                                 weak_factory_.GetWeakPtr(), id));
}

MoltAIManager::CreateRequest* MoltAIManager::FindCreate(uint64_t id) {
  auto it = creates_.find(id);
  if (it == creates_.end()) {
    return nullptr;
  }
  if (!it->second->IsWanted()) {
    // create() was aborted or the page went away: stop here (no prompt, no
    // model load).
    creates_.erase(it);
    return nullptr;
  }
  return it->second.get();
}

std::unique_ptr<MoltAIManager::CreateRequest> MoltAIManager::TakeCreate(
    uint64_t id) {
  auto it = creates_.find(id);
  if (it == creates_.end()) {
    return nullptr;
  }
  std::unique_ptr<CreateRequest> request = std::move(it->second);
  creates_.erase(it);
  return request;
}

void MoltAIManager::OnCreateDeviceFacts(uint64_t id, DeviceFacts device) {
  CreateRequest* request = FindCreate(id);
  if (!request) {
    return;
  }
  request->set_device(device);
  AvailabilityInput input;
  input.context = environment_->GetContextFacts(request->api());
  input.device = device;
  input.options_supported = request->options_supported();
  input.languages_supported = request->languages_supported();
  input.wants_audio = request->wants_audio();
  input.consent = environment_->GetConsent();
  switch (DecideCreateAction(input)) {
    case CreateAction::kReject:
      TakeCreate(id)->Reject(DecideAvailability(input).reason);
      return;
    case CreateAction::kAskUser:
    case CreateAction::kNeedsModel:
      // Audio input without a model that hears it: the prompt offers the
      // model picker too.
      environment_->RequestConsent(
          /*needs_model=*/!device.model_installed ||
              (request->wants_audio() && !device.audio_model_installed),
          base::BindOnce(&MoltAIManager::OnCreateConsent,
                         weak_factory_.GetWeakPtr(), id));
      return;
    case CreateAction::kCreate:
      backend_->PrepareModel(request->wants_images(), request->wants_audio(),
                             base::BindOnce(&MoltAIManager::OnCreateModelReady,
                                            weak_factory_.GetWeakPtr(), id));
      return;
  }
}

void MoltAIManager::OnCreateConsent(uint64_t id, SiteConsent consent) {
  CreateRequest* request = FindCreate(id);
  if (!request) {
    return;
  }
  AvailabilityInput input;
  input.context = environment_->GetContextFacts(request->api());
  input.device = request->device();
  input.options_supported = request->options_supported();
  input.languages_supported = request->languages_supported();
  input.wants_audio = request->wants_audio();
  input.consent = consent;
  if (DecideCreateAction(input) != CreateAction::kCreate) {
    // Blocked, closed without an answer, or allowed without a model (one
    // that hears audio, for audio input) yet: the prompt offered the model
    // picker; the page can try again later.
    TakeCreate(id)->Fail(AIManagerCreateClientError::kUnableToCreateSession,
                         nullptr);
    return;
  }
  backend_->PrepareModel(request->wants_images(), request->wants_audio(),
                         base::BindOnce(&MoltAIManager::OnCreateModelReady,
                                        weak_factory_.GetWeakPtr(), id));
}

void MoltAIManager::OnCreateModelReady(uint64_t id,
                                       bool ok,
                                       uint32_t context_tokens,
                                       bool sees_images,
                                       bool hears_audio) {
  if (!FindCreate(id)) {
    return;
  }
  std::unique_ptr<CreateRequest> request = TakeCreate(id);
  // The answer may have been taken back, or the APIs turned off, while the
  // model loaded.
  if (!MayServe(request->api())) {
    request->Fail(AIManagerCreateClientError::kUnableToCreateSession,
                  nullptr);
    return;
  }
  if (!ok) {
    request->Fail(AIManagerCreateClientError::kUnableToCreateSession,
                  nullptr);
    return;
  }
  request->Finish(*this, context_tokens, sees_images, hears_audio);
}

// ---- Sessions ----

uint64_t MoltAIManager::ReserveSession() {
  if (!backend_->TryAddSession(site_key())) {
    return 0;
  }
  return next_session_id_++;
}

void MoltAIManager::AdoptSession(std::unique_ptr<WebAISession> session) {
  const uint64_t id = session->id();
  sessions_[id] = std::move(session);
}

void MoltAIManager::CloseSession(uint64_t session_id) {
  // Not now: the session may be on the stack (destroy(), its pipe's
  // disconnect handler).
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&MoltAIManager::DeleteSession,
                                weak_factory_.GetWeakPtr(), session_id));
}

void MoltAIManager::DeleteSession(uint64_t session_id) {
  sessions_.erase(session_id);
}

void MoltAIManager::ForkLanguageModel(
    const LanguageModelSession& source,
    mojo::PendingRemote<blink::mojom::AIManagerCreateLanguageModelClient>
        client) {
  mojo::Remote<blink::mojom::AIManagerCreateLanguageModelClient> remote(
      std::move(client));
  const uint64_t id = ReserveSession();
  if (id == 0) {
    remote->OnError(AIManagerCreateClientError::kUnableToCreateSession,
                    nullptr);
    return;
  }
  mojo::PendingRemote<blink::mojom::AILanguageModel> pending;
  // The clone gets the committed context (a prompt still running isn't in
  // it) and its own KV-cache key.
  auto session = std::make_unique<LanguageModelSession>(
      *this, id, source.config(), source.conversation(),
      pending.InitWithNewPipeAndPassReceiver());
  // Its copy of the context's images and audio counts against the caps.
  if (!session->HoldContextImages()) {
    remote->OnError(AIManagerCreateClientError::kUnableToCreateSession,
                    nullptr);
    return;
  }
  blink::mojom::AILanguageModelInstanceInfoPtr info = session->GetInfo();
  AdoptSession(std::move(session));
  remote->OnResult(std::move(pending), std::move(info));
}

}  // namespace molt_ai::web_ai
