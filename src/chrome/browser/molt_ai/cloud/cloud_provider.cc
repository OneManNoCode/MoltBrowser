// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/cloud/cloud_provider.h"

#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "base/values.h"
#include "net/base/load_flags.h"
#include "net/http/http_request_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace molt_ai {

CloudChatRequest::CloudChatRequest() = default;
CloudChatRequest::CloudChatRequest(const CloudChatRequest&) = default;
CloudChatRequest::CloudChatRequest(CloudChatRequest&&) = default;
CloudChatRequest& CloudChatRequest::operator=(const CloudChatRequest&) =
    default;
CloudChatRequest& CloudChatRequest::operator=(CloudChatRequest&&) = default;
CloudChatRequest::~CloudChatRequest() = default;

namespace {

// ==================================================================
// Traffic annotations
// ==================================================================

constexpr net::NetworkTrafficAnnotationTag kCloudChatAnnotation =
    net::DefineNetworkTrafficAnnotation("molt_ai_cloud_chat", R"(
    semantics {
      sender: "MoltBrowser Cloud AI Chat"
      description:
        "Streams a chat completion from a user-configured frontier AI "
        "provider (OpenAI, Anthropic, Google Gemini, or an OpenAI-"
        "compatible endpoint). The request carries the conversation the "
        "user is having with the AI assistant and is authenticated with "
        "an API key the user entered in AI settings."
      trigger:
        "User sends a message to the AI assistant while a cloud model "
        "is selected."
      data:
        "The conversation messages and the user's own API key for the "
        "selected provider. Sent only to the provider endpoint the user "
        "configured."
      destination: OTHER
      destination_other: "The AI provider endpoint chosen by the user."
    }
    policy {
      cookies_allowed: NO
      setting:
        "Users choose whether to configure any cloud provider and which "
        "model to use in AI settings. With no key configured, no request "
        "is ever made."
    })");

constexpr net::NetworkTrafficAnnotationTag kCloudModelsAnnotation =
    net::DefineNetworkTrafficAnnotation("molt_ai_cloud_models", R"(
    semantics {
      sender: "MoltBrowser Cloud AI Model List"
      description:
        "Fetches the list of available models from a user-configured "
        "frontier AI provider, so the model picker can be populated. "
        "Authenticated with the user's API key."
      trigger:
        "User adds or refreshes a cloud provider in AI settings."
      data: "The user's API key for the selected provider."
      destination: OTHER
      destination_other: "The AI provider endpoint chosen by the user."
    }
    policy {
      cookies_allowed: NO
      setting:
        "Users choose whether to configure any cloud provider in AI "
        "settings."
    })");

// ==================================================================
// Provider registry
// ==================================================================

std::vector<CloudProviderInfo> BuildProviders() {
  return {
      {"openai", "OpenAI", "https://api.openai.com/v1", CloudFormat::kOpenAI,
       false},
      {"anthropic", "Anthropic", "https://api.anthropic.com",
       CloudFormat::kAnthropic, false},
      {"gemini", "Google Gemini",
       "https://generativelanguage.googleapis.com/v1beta",
       CloudFormat::kGemini, false},
      {"openrouter", "OpenRouter", "https://openrouter.ai/api/v1",
       CloudFormat::kOpenAI, false},
      {"xai", "xAI (Grok)", "https://api.x.ai/v1", CloudFormat::kOpenAI,
       false},
      {"deepseek", "DeepSeek", "https://api.deepseek.com/v1",
       CloudFormat::kOpenAI, false},
      {"groq", "Groq", "https://api.groq.com/openai/v1", CloudFormat::kOpenAI,
       false},
      {"mistral", "Mistral", "https://api.mistral.ai/v1", CloudFormat::kOpenAI,
       false},
      {"perplexity", "Perplexity", "https://api.perplexity.ai",
       CloudFormat::kOpenAI, false},
      {"custom", "Custom (OpenAI-compatible)", "", CloudFormat::kOpenAI, true},
  };
}

// Resolve the effective base url for a request: caller override wins,
// else the provider default. Trailing slashes are trimmed so we can
// concatenate path suffixes cleanly.
std::string ResolveBaseUrl(const CloudProviderInfo& info,
                           const std::string& override_url) {
  std::string base =
      override_url.empty() ? info.default_base_url : override_url;
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  return base;
}

// ==================================================================
// Request body builders
// ==================================================================

// OpenAI Chat Completions body: messages pass through untouched (system
// stays inline in the list).
std::string BuildOpenAIBody(const CloudChatRequest& req, bool stream) {
  base::DictValue root;
  root.Set("model", req.model);
  base::ListValue messages;
  for (const CloudMessage& m : req.messages) {
    base::DictValue msg;
    msg.Set("role", m.role);
    msg.Set("content", m.content);
    messages.Append(std::move(msg));
  }
  root.Set("messages", std::move(messages));
  root.Set("stream", stream);
  root.Set("max_tokens", req.max_tokens);
  root.Set("temperature", req.temperature);
  std::string out;
  base::JSONWriter::Write(root, &out);
  return out;
}

// Anthropic Messages body: the system message is lifted out of the
// list into a top-level "system"; only user/assistant remain.
std::string BuildAnthropicBody(const CloudChatRequest& req, bool stream) {
  base::DictValue root;
  root.Set("model", req.model);
  root.Set("max_tokens", req.max_tokens);
  root.Set("temperature", req.temperature);
  root.Set("stream", stream);

  std::string system_text;
  base::ListValue messages;
  for (const CloudMessage& m : req.messages) {
    if (m.role == "system") {
      if (!system_text.empty()) {
        system_text += "\n\n";
      }
      system_text += m.content;
      continue;
    }
    base::DictValue msg;
    // Anthropic only accepts user/assistant; anything else is coerced
    // to user.
    msg.Set("role", m.role == "assistant" ? "assistant" : "user");
    msg.Set("content", m.content);
    messages.Append(std::move(msg));
  }
  if (!system_text.empty()) {
    root.Set("system", system_text);
  }
  root.Set("messages", std::move(messages));
  std::string out;
  base::JSONWriter::Write(root, &out);
  return out;
}

// Gemini generateContent body: system lifted into systemInstruction;
// assistant role mapped to "model".
std::string BuildGeminiBody(const CloudChatRequest& req) {
  base::DictValue root;

  std::string system_text;
  base::ListValue contents;
  for (const CloudMessage& m : req.messages) {
    if (m.role == "system") {
      if (!system_text.empty()) {
        system_text += "\n\n";
      }
      system_text += m.content;
      continue;
    }
    base::DictValue content;
    content.Set("role", m.role == "assistant" ? "model" : "user");
    base::ListValue parts;
    base::DictValue part;
    part.Set("text", m.content);
    parts.Append(std::move(part));
    content.Set("parts", std::move(parts));
    contents.Append(std::move(content));
  }

  if (!system_text.empty()) {
    base::DictValue sys_instruction;
    base::ListValue sys_parts;
    base::DictValue sys_part;
    sys_part.Set("text", system_text);
    sys_parts.Append(std::move(sys_part));
    sys_instruction.Set("parts", std::move(sys_parts));
    root.Set("systemInstruction", std::move(sys_instruction));
  }
  root.Set("contents", std::move(contents));

  base::DictValue gen_config;
  gen_config.Set("maxOutputTokens", req.max_tokens);
  gen_config.Set("temperature", req.temperature);
  root.Set("generationConfig", std::move(gen_config));

  std::string out;
  base::JSONWriter::Write(root, &out);
  return out;
}

// ==================================================================
// SSE delta extractors — one JSON data line -> incremental text.
// ==================================================================

std::string ExtractOpenAIDelta(const base::Value& v) {
  if (!v.is_dict()) {
    return std::string();
  }
  const base::ListValue* choices = v.GetDict().FindList("choices");
  if (!choices || choices->empty()) {
    return std::string();
  }
  const base::Value& first = (*choices)[0];
  if (!first.is_dict()) {
    return std::string();
  }
  const base::DictValue* delta = first.GetDict().FindDict("delta");
  if (!delta) {
    return std::string();
  }
  const std::string* content = delta->FindString("content");
  return content ? *content : std::string();
}

std::string ExtractAnthropicDelta(const base::Value& v) {
  if (!v.is_dict()) {
    return std::string();
  }
  const std::string* type = v.GetDict().FindString("type");
  if (!type || *type != "content_block_delta") {
    return std::string();
  }
  const base::DictValue* delta = v.GetDict().FindDict("delta");
  if (!delta) {
    return std::string();
  }
  const std::string* text = delta->FindString("text");
  return text ? *text : std::string();
}

std::string ExtractGeminiDelta(const base::Value& v) {
  if (!v.is_dict()) {
    return std::string();
  }
  const base::ListValue* candidates = v.GetDict().FindList("candidates");
  if (!candidates || candidates->empty()) {
    return std::string();
  }
  const base::Value& first = (*candidates)[0];
  if (!first.is_dict()) {
    return std::string();
  }
  const base::DictValue* content = first.GetDict().FindDict("content");
  if (!content) {
    return std::string();
  }
  const base::ListValue* parts = content->FindList("parts");
  if (!parts) {
    return std::string();
  }
  // A single Gemini chunk can carry multiple text parts — concatenate them
  // all rather than dropping everything after parts[0].
  std::string out;
  for (const base::Value& part : *parts) {
    if (!part.is_dict()) {
      continue;
    }
    const std::string* text = part.GetDict().FindString("text");
    if (text) {
      out += *text;
    }
  }
  return out;
}

// ==================================================================
// CloudChatStreamImpl — the SSE consumer + loader owner.
// ==================================================================

class CloudChatStreamImpl : public CloudChatStream,
                            public network::SimpleURLLoaderStreamConsumer {
 public:
  CloudChatStreamImpl(
      CloudFormat format,
      base::RepeatingCallback<void(const std::string& delta)> on_token,
      base::OnceCallback<void(bool ok, const std::string& error)> on_done)
      : format_(format),
        on_token_(std::move(on_token)),
        on_done_(std::move(on_done)) {}

  ~CloudChatStreamImpl() override = default;

  void Start(scoped_refptr<network::SharedURLLoaderFactory> factory,
             std::unique_ptr<network::ResourceRequest> request,
             const std::string& body,
             const std::string& content_type) {
    factory_ = std::move(factory);
    loader_ = network::SimpleURLLoader::Create(std::move(request),
                                               kCloudChatAnnotation);
    loader_->AttachStringForUpload(body, content_type);
    // Surface provider error bodies (4xx/5xx) instead of a bare
    // net-error; the buffered body becomes the error text.
    loader_->SetAllowHttpErrorResults(true);
    loader_->SetTimeoutDuration(base::Seconds(120));
    // DownloadAsStream runs on this (UI) thread; all consumer callbacks
    // fire here.
    loader_->DownloadAsStream(factory_.get(), this);
  }

  // CloudChatStream:
  void Cancel() override {
    // Dropping the loader stops the request and prevents further
    // consumer callbacks. on_done_ is intentionally not run on an
    // explicit cancel.
    loader_.reset();
  }

  // network::SimpleURLLoaderStreamConsumer:
  void OnDataReceived(std::string_view chunk,
                      base::OnceClosure resume) override {
    buffer_.append(chunk.data(), chunk.size());
    ProcessBuffer();
    std::move(resume).Run();
  }

  void OnComplete(bool success) override {
    // Flush any trailing complete line without a terminating newline.
    if (!buffer_.empty()) {
      std::string leftover;
      leftover.swap(buffer_);
      HandleLine(leftover);
    }

    int response_code = 0;
    if (loader_ && loader_->ResponseInfo() &&
        loader_->ResponseInfo()->headers) {
      response_code = loader_->ResponseInfo()->headers->response_code();
    }

    bool http_ok = response_code == 0 || (response_code >= 200 &&
                                          response_code < 300);
    if (success && http_ok) {
      Finish(true, std::string());
      return;
    }

    // Build a useful error string: prefer the provider's response body
    // (accumulated verbatim for error responses), else the net error.
    std::string error;
    if (response_code >= 400) {
      error = base::StrCat({"HTTP ", base::NumberToString(response_code)});
      if (!error_body_.empty()) {
        error += ": " + error_body_;
      }
    } else if (loader_) {
      error = base::StrCat(
          {"Network error (", base::NumberToString(loader_->NetError()), ")"});
    } else {
      error = "Request failed";
    }
    Finish(false, error);
  }

  void OnRetry(base::OnceClosure start_retry) override {
    // Discard anything received before the retry.
    buffer_.clear();
    error_body_.clear();
    std::move(start_retry).Run();
  }

 private:
  void Finish(bool ok, const std::string& error) {
    if (on_done_) {
      std::move(on_done_).Run(ok, error);
    }
  }

  void ProcessBuffer() {
    size_t nl;
    while ((nl = buffer_.find('\n')) != std::string::npos) {
      std::string line = buffer_.substr(0, nl);
      buffer_.erase(0, nl + 1);
      HandleLine(line);
    }
  }

  void HandleLine(std::string line) {
    // Strip a trailing CR (SSE uses CRLF on some servers).
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      return;
    }
    // For error responses the body is a JSON error object, not SSE, so
    // OnComplete can surface it. Cap the buffer so a long *successful*
    // stream (whose lines also flow through here) can't grow it without
    // bound.
    constexpr size_t kMaxErrorBody = 16 * 1024;
    if (error_body_.size() < kMaxErrorBody) {
      error_body_.append(line);
      error_body_.append("\n");
    }

    // SSE framing: only "data:" lines carry payloads. Ignore event:/
    // id:/retry: and comments.
    constexpr char kDataPrefix[] = "data:";
    if (!base::StartsWith(line, kDataPrefix)) {
      return;
    }
    std::string data = line.substr(sizeof(kDataPrefix) - 1);
    // Optional single leading space after the colon.
    if (!data.empty() && data.front() == ' ') {
      data.erase(0, 1);
    }
    if (data.empty() || data == "[DONE]") {
      return;
    }
    std::optional<base::Value> parsed =
        base::JSONReader::Read(data, /*options=*/0);
    if (!parsed) {
      return;
    }
    std::string delta;
    switch (format_) {
      case CloudFormat::kOpenAI:
        delta = ExtractOpenAIDelta(*parsed);
        break;
      case CloudFormat::kAnthropic:
        delta = ExtractAnthropicDelta(*parsed);
        break;
      case CloudFormat::kGemini:
        delta = ExtractGeminiDelta(*parsed);
        break;
    }
    if (!delta.empty() && on_token_) {
      on_token_.Run(delta);
    }
  }

  const CloudFormat format_;
  base::RepeatingCallback<void(const std::string& delta)> on_token_;
  base::OnceCallback<void(bool ok, const std::string& error)> on_done_;

  scoped_refptr<network::SharedURLLoaderFactory> factory_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  std::string buffer_;      // incomplete-line carry-over
  std::string error_body_;  // raw body, surfaced only on HTTP error
};

// ==================================================================
// Model-list fetch — one-shot DownloadToString + parse.
// ==================================================================

class CloudModelsFetcher {
 public:
  CloudModelsFetcher(
      CloudFormat format,
      base::OnceCallback<
          void(bool ok, std::vector<std::string> models, std::string error)>
          on_done)
      : format_(format), on_done_(std::move(on_done)) {}

  void Start(scoped_refptr<network::SharedURLLoaderFactory> factory,
             std::unique_ptr<network::ResourceRequest> request) {
    factory_ = std::move(factory);
    loader_ = network::SimpleURLLoader::Create(std::move(request),
                                               kCloudModelsAnnotation);
    loader_->SetAllowHttpErrorResults(true);
    loader_->SetTimeoutDuration(base::Seconds(120));
    // DownloadToString caps the body; model lists are small.
    loader_->DownloadToString(
        factory_.get(),
        base::BindOnce(&CloudModelsFetcher::OnBody, base::Unretained(this)),
        /*max_body_size=*/4 * 1024 * 1024);
  }

 private:
  void OnBody(std::optional<std::string> body) {
    int response_code = 0;
    if (loader_->ResponseInfo() && loader_->ResponseInfo()->headers) {
      response_code = loader_->ResponseInfo()->headers->response_code();
    }
    if (!body) {
      Finish(false, {},
             base::StrCat({"Network error (",
                           base::NumberToString(loader_->NetError()), ")"}));
      return;
    }
    if (response_code >= 400) {
      std::string err =
          base::StrCat({"HTTP ", base::NumberToString(response_code)});
      if (!body->empty()) {
        err += ": " + *body;
      }
      Finish(false, {}, err);
      return;
    }
    std::optional<base::Value> parsed =
        base::JSONReader::Read(*body, /*options=*/0);
    if (!parsed || !parsed->is_dict()) {
      Finish(false, {}, "Malformed model-list response");
      return;
    }
    std::vector<std::string> models = ParseModels(parsed->GetDict());
    Finish(true, std::move(models), std::string());
  }

  std::vector<std::string> ParseModels(const base::DictValue& root) {
    std::vector<std::string> out;
    if (format_ == CloudFormat::kGemini) {
      // { "models": [ { "name": "models/gemini-..." }, ... ] }
      const base::ListValue* models = root.FindList("models");
      if (!models) {
        return out;
      }
      for (const base::Value& m : *models) {
        if (!m.is_dict()) {
          continue;
        }
        const std::string* name = m.GetDict().FindString("name");
        if (!name) {
          continue;
        }
        std::string id = *name;
        // Strip the leading "models/" prefix.
        constexpr char kPrefix[] = "models/";
        if (base::StartsWith(id, kPrefix)) {
          id = id.substr(sizeof(kPrefix) - 1);
        }
        if (!id.empty()) {
          out.push_back(id);
        }
      }
      return out;
    }
    // OpenAI + Anthropic both use { "data": [ { "id": "..." }, ... ] }.
    const base::ListValue* data = root.FindList("data");
    if (!data) {
      return out;
    }
    for (const base::Value& m : *data) {
      if (!m.is_dict()) {
        continue;
      }
      const std::string* id = m.GetDict().FindString("id");
      if (id && !id->empty()) {
        out.push_back(*id);
      }
    }
    return out;
  }

  void Finish(bool ok,
              std::vector<std::string> models,
              std::string error) {
    // Hand off, then self-destruct — StartCloudModels leaked us on
    // purpose so we outlive the async call.
    auto cb = std::move(on_done_);
    std::move(cb).Run(ok, std::move(models), std::move(error));
    delete this;
  }

  const CloudFormat format_;
  base::OnceCallback<
      void(bool ok, std::vector<std::string> models, std::string error)>
      on_done_;
  scoped_refptr<network::SharedURLLoaderFactory> factory_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
};

}  // namespace

// ==================================================================
// Registry accessors
// ==================================================================

const std::vector<CloudProviderInfo>& GetCloudProviders() {
  static base::NoDestructor<std::vector<CloudProviderInfo>> providers(
      BuildProviders());
  return *providers;
}

const CloudProviderInfo* FindCloudProvider(const std::string& id) {
  for (const CloudProviderInfo& info : GetCloudProviders()) {
    if (info.id == id) {
      return &info;
    }
  }
  return nullptr;
}

// ==================================================================
// StartCloudChat
// ==================================================================

std::unique_ptr<CloudChatStream> StartCloudChat(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    const CloudChatRequest& request,
    base::RepeatingCallback<void(const std::string& delta)> on_token,
    base::OnceCallback<void(bool ok, const std::string& error)> on_done) {
  const CloudProviderInfo* info = FindCloudProvider(request.provider_id);
  if (!info) {
    std::move(on_done).Run(false, "Unknown provider: " + request.provider_id);
    return nullptr;
  }
  std::string base = ResolveBaseUrl(*info, request.base_url);
  if (base.empty()) {
    std::move(on_done).Run(false, "No base URL configured for provider");
    return nullptr;
  }

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->method = "POST";
  resource_request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  resource_request->load_flags = net::LOAD_DO_NOT_SAVE_COOKIES;

  std::string url;
  std::string body;
  const char kJsonContentType[] = "application/json";

  switch (info->format) {
    case CloudFormat::kOpenAI: {
      url = base + "/chat/completions";
      resource_request->headers.SetHeader(
          "Authorization", base::StrCat({"Bearer ", request.api_key}));
      body = BuildOpenAIBody(request, /*stream=*/true);
      break;
    }
    case CloudFormat::kAnthropic: {
      url = base + "/v1/messages";
      resource_request->headers.SetHeader("x-api-key", request.api_key);
      resource_request->headers.SetHeader("anthropic-version", "2023-06-01");
      body = BuildAnthropicBody(request, /*stream=*/true);
      break;
    }
    case CloudFormat::kGemini: {
      // key goes in the query string; SSE requested via alt=sse.
      url = base + "/models/" + request.model +
            ":streamGenerateContent?alt=sse&key=" + request.api_key;
      body = BuildGeminiBody(request);
      break;
    }
  }

  resource_request->url = GURL(url);

  auto stream = std::make_unique<CloudChatStreamImpl>(
      info->format, std::move(on_token), std::move(on_done));
  stream->Start(std::move(url_loader_factory), std::move(resource_request),
                body, kJsonContentType);
  return stream;
}

// ==================================================================
// FetchCloudModels
// ==================================================================

void FetchCloudModels(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    const std::string& provider_id,
    const std::string& api_key,
    const std::string& base_url,
    base::OnceCallback<
        void(bool ok, std::vector<std::string> models, std::string error)>
        on_done) {
  const CloudProviderInfo* info = FindCloudProvider(provider_id);
  if (!info) {
    std::move(on_done).Run(false, {}, "Unknown provider: " + provider_id);
    return;
  }
  std::string base = ResolveBaseUrl(*info, base_url);
  if (base.empty()) {
    std::move(on_done).Run(false, {}, "No base URL configured for provider");
    return;
  }

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->method = "GET";
  resource_request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  resource_request->load_flags = net::LOAD_DO_NOT_SAVE_COOKIES;

  std::string url;
  switch (info->format) {
    case CloudFormat::kOpenAI:
      url = base + "/models";
      resource_request->headers.SetHeader(
          "Authorization", base::StrCat({"Bearer ", api_key}));
      break;
    case CloudFormat::kAnthropic:
      url = base + "/v1/models";
      resource_request->headers.SetHeader("x-api-key", api_key);
      resource_request->headers.SetHeader("anthropic-version", "2023-06-01");
      break;
    case CloudFormat::kGemini:
      url = base + "/models?key=" + api_key;
      break;
  }
  resource_request->url = GURL(url);

  // Owns itself until its callback fires (deletes in Finish()).
  auto* fetcher = new CloudModelsFetcher(info->format, std::move(on_done));
  fetcher->Start(std::move(url_loader_factory), std::move(resource_request));
}

// ==================================================================
// Cloud model-id helpers
// ==================================================================

bool IsCloudModelId(const std::string& id) {
  return id.find(':') != std::string::npos;
}

bool SplitCloudModelId(const std::string& id,
                       std::string* provider,
                       std::string* model) {
  size_t colon = id.find(':');
  if (colon == std::string::npos) {
    return false;
  }
  if (provider) {
    *provider = id.substr(0, colon);
  }
  if (model) {
    *model = id.substr(colon + 1);
  }
  return true;
}

}  // namespace molt_ai
