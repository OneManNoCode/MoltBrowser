// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef MOLT_AI_API_BROWSER_AI_API_H_
#define MOLT_AI_API_BROWSER_AI_API_H_

#include <string>
#include <vector>

#include "src/molt_ai/runtime/browser_ai_runtime.h"

namespace molt_ai {

// ============================================================
// BrowserAIAPI — Chrome Extension API: browser.ai.*
// ============================================================
// Exposes AI capabilities to Chrome extensions via Manifest V3.
//
// Extension manifest requirement:
//   "permissions": ["ai"]
//
// JavaScript API surface:
//   browser.ai.run(prompt, opts?)       → Run prompt against local LLM
//   browser.ai.pageSummary(tabId?)      → Summarize current page
//   browser.ai.agentTask(goal, opts?)   → Run autonomous agent task
//   browser.ai.extractData(selector)    → Extract structured data
//   browser.ai.getModels()              → List available models
//   browser.ai.embed(text)              → Get text embedding vector
//   browser.ai.onModelLoaded(callback)  → Event: model loaded
//
// All API calls are validated through ActionValidator before
// execution to prevent malicious extension behavior.
// ============================================================

// API request from extension
struct APIRequest {
  std::string method;        // "run", "pageSummary", "agentTask", etc.
  std::string extension_id;  // Calling extension's ID
  std::string prompt;
  int tab_id = -1;
  std::string selector;
  PromptOptions options;
};

// API response to extension
struct APIResponse {
  bool success;
  std::string data;          // JSON response data
  std::string error;
  int tokens_used = 0;
  float time_ms = 0;
};

class BrowserAIAPI {
 public:
  explicit BrowserAIAPI(BrowserAIRuntime* runtime);
  ~BrowserAIAPI();

  // Handle an API call from an extension
  APIResponse HandleRequest(const APIRequest& request);

  // ---- Individual API methods ----

  // browser.ai.run(prompt, opts?)
  APIResponse Run(const std::string& prompt,
                  const PromptOptions& options,
                  const std::string& extension_id);

  // browser.ai.pageSummary(tabId?)
  APIResponse PageSummary(int tab_id, const std::string& extension_id);

  // browser.ai.agentTask(goal, opts?)
  APIResponse AgentTask(const std::string& goal,
                        const PromptOptions& options,
                        const std::string& extension_id);

  // browser.ai.extractData(selector)
  APIResponse ExtractData(int tab_id,
                           const std::string& selector,
                           const std::string& extension_id);

  // browser.ai.getModels()
  APIResponse GetModels(const std::string& extension_id);

  // browser.ai.embed(text)
  APIResponse Embed(const std::string& text,
                    const std::string& extension_id);

  // ---- Permission Management ----

  // Check if an extension has AI permission
  bool HasPermission(const std::string& extension_id) const;

  // Rate limiting per extension
  bool CheckRateLimit(const std::string& extension_id) const;

 private:
  BrowserAIRuntime* runtime_;  // Not owned

  // Extension rate limiting: max requests per minute
  static constexpr int kMaxRequestsPerMinute = 60;

  std::string SerializeModelsToJSON() const;
};

}  // namespace molt_ai

#endif  // MOLT_AI_API_BROWSER_AI_API_H_
