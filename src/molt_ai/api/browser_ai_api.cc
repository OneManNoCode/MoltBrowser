// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "src/molt_ai/api/browser_ai_api.h"

#include <sstream>

namespace molt_ai {

BrowserAIAPI::BrowserAIAPI(BrowserAIRuntime* runtime)
    : runtime_(runtime) {}

BrowserAIAPI::~BrowserAIAPI() = default;

APIResponse BrowserAIAPI::HandleRequest(const APIRequest& request) {
  // Check extension permission
  if (!HasPermission(request.extension_id)) {
    return {false, "", "Extension does not have 'ai' permission", 0, 0};
  }

  // Check rate limit
  if (!CheckRateLimit(request.extension_id)) {
    return {false, "", "Rate limit exceeded", 0, 0};
  }

  // Route to appropriate method
  if (request.method == "run") {
    return Run(request.prompt, request.options, request.extension_id);
  } else if (request.method == "pageSummary") {
    return PageSummary(request.tab_id, request.extension_id);
  } else if (request.method == "agentTask") {
    return AgentTask(request.prompt, request.options, request.extension_id);
  } else if (request.method == "extractData") {
    return ExtractData(request.tab_id, request.selector, request.extension_id);
  } else if (request.method == "getModels") {
    return GetModels(request.extension_id);
  } else if (request.method == "embed") {
    return Embed(request.prompt, request.extension_id);
  }

  return {false, "", "Unknown API method: " + request.method, 0, 0};
}

APIResponse BrowserAIAPI::Run(const std::string& prompt,
                               const PromptOptions& options,
                               const std::string& extension_id) {
  auto result = runtime_->RunPrompt(prompt, options);

  APIResponse response;
  response.success = result.success;
  response.tokens_used = result.tokens_generated;
  response.time_ms = result.generation_time_ms;

  if (result.success) {
    std::ostringstream json;
    json << "{\"text\": \"" << result.text << "\", "
         << "\"model\": \"" << result.model_used << "\", "
         << "\"tokens\": " << result.tokens_generated << ", "
         << "\"time_ms\": " << result.generation_time_ms << "}";
    response.data = json.str();
  } else {
    response.error = result.error_message;
  }

  return response;
}

APIResponse BrowserAIAPI::PageSummary(int tab_id,
                                       const std::string& extension_id) {
  auto result = runtime_->SummarizePage(tab_id);

  APIResponse response;
  response.success = result.success;
  response.tokens_used = result.tokens_generated;
  response.time_ms = result.generation_time_ms;

  if (result.success) {
    std::ostringstream json;
    json << "{\"summary\": \"" << result.text << "\", "
         << "\"model\": \"" << result.model_used << "\"}";
    response.data = json.str();
  } else {
    response.error = result.error_message;
  }

  return response;
}

APIResponse BrowserAIAPI::AgentTask(const std::string& goal,
                                     const PromptOptions& options,
                                     const std::string& extension_id) {
  // TODO: Integrate with AgentEngine
  APIResponse response;
  response.success = false;
  response.error = "Agent task API not yet implemented";
  return response;
}

APIResponse BrowserAIAPI::ExtractData(int tab_id,
                                       const std::string& selector,
                                       const std::string& extension_id) {
  auto data = runtime_->ExtractData(tab_id, selector);

  APIResponse response;
  response.success = !data.empty();
  if (response.success) {
    response.data = data;
  } else {
    response.error = "No data found for selector: " + selector;
  }

  return response;
}

APIResponse BrowserAIAPI::GetModels(const std::string& extension_id) {
  APIResponse response;
  response.success = true;
  response.data = SerializeModelsToJSON();
  return response;
}

APIResponse BrowserAIAPI::Embed(const std::string& text,
                                 const std::string& extension_id) {
  // TODO: Use MemoryEngine's ComputeEmbedding
  APIResponse response;
  response.success = false;
  response.error = "Embedding API not yet implemented";
  return response;
}

bool BrowserAIAPI::HasPermission(const std::string& extension_id) const {
  // TODO: Check extension manifest for "ai" permission
  // Integration with Chromium's ExtensionRegistry
  return true;  // Default allow for development
}

bool BrowserAIAPI::CheckRateLimit(const std::string& extension_id) const {
  // TODO: Implement per-extension rate limiting
  // Track request timestamps, enforce kMaxRequestsPerMinute
  return true;
}

std::string BrowserAIAPI::SerializeModelsToJSON() const {
  auto models = runtime_->GetAvailableModels();

  std::ostringstream json;
  json << "[";
  for (size_t i = 0; i < models.size(); ++i) {
    const auto& m = models[i];
    json << "{\"id\": \"" << m.model_id << "\", "
         << "\"name\": \"" << m.display_name << "\", "
         << "\"size_bytes\": " << m.file_size_bytes << ", "
         << "\"ram_required\": " << m.ram_required_bytes << ", "
         << "\"params_b\": " << m.parameter_count_billions << ", "
         << "\"quantization\": \"" << m.quantization << "\", "
         << "\"downloaded\": " << (m.is_downloaded ? "true" : "false") << ", "
         << "\"loaded\": " << (m.is_loaded ? "true" : "false") << "}";
    if (i < models.size() - 1) json << ", ";
  }
  json << "]";

  return json.str();
}

}  // namespace molt_ai
