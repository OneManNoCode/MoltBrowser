// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// CloudProvider — streaming chat + model-listing engine for frontier
// API-key models, sitting alongside the on-device llama.cpp runtime.
//
// Three wire formats cover every supported provider:
//   * kOpenAI    — OpenAI Chat Completions (also OpenRouter, xAI/Grok,
//                  DeepSeek, Groq, Mistral, Perplexity, and any custom
//                  OpenAI-compatible endpoint).
//   * kAnthropic — Anthropic Messages API.
//   * kGemini    — Google Gemini generateContent (SSE).
//
// Streaming is built on network::SimpleURLLoader::DownloadAsStream with
// an internal SimpleURLLoaderStreamConsumer. All callbacks fire on the
// UI thread (the thread that called StartCloudChat). Deleting the
// returned CloudChatStream cancels the in-flight request.
//
// Security: api_key is passed per-request and NEVER logged. The store
// for keys is molt_ai::MoltKeysStore (keys.enc); this file only
// consumes a key handed to it.

#ifndef CHROME_BROWSER_MOLT_AI_CLOUD_CLOUD_PROVIDER_H_
#define CHROME_BROWSER_MOLT_AI_CLOUD_CLOUD_PROVIDER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace molt_ai {

// ==================================================================
// Provider registry
// ==================================================================

enum class CloudFormat {
  kOpenAI,
  kAnthropic,
  kGemini,
};

struct CloudProviderInfo {
  std::string id;
  std::string display_name;
  std::string default_base_url;
  CloudFormat format;
  // When true the provider has no fixed endpoint and the user must
  // supply a base_url (the "Custom (OpenAI-compatible)" entry).
  bool custom_base_url = false;
};

// The static list of all known cloud providers, in display order.
const std::vector<CloudProviderInfo>& GetCloudProviders();

// Look up a provider by id. nullptr if unknown.
const CloudProviderInfo* FindCloudProvider(const std::string& id);

// ==================================================================
// Chat streaming
// ==================================================================

struct CloudMessage {
  std::string role;     // "system" | "user" | "assistant"
  std::string content;
};

struct CloudChatRequest {
  CloudChatRequest();
  CloudChatRequest(const CloudChatRequest&);
  CloudChatRequest(CloudChatRequest&&);
  CloudChatRequest& operator=(const CloudChatRequest&);
  CloudChatRequest& operator=(CloudChatRequest&&);
  ~CloudChatRequest();

  std::string provider_id;
  std::string model;
  std::string api_key;
  // Empty => use the provider's default_base_url. Required for the
  // "custom" provider.
  std::string base_url;
  std::vector<CloudMessage> messages;
  int max_tokens = 1024;
  double temperature = 0.7;
};

// Handle to an in-flight streaming chat. Destroying it cancels the
// request; no further callbacks fire after Cancel() / destruction.
class CloudChatStream {
 public:
  virtual ~CloudChatStream() = default;
  virtual void Cancel() = 0;
};

// Start a streaming chat completion.
//   on_token — invoked (repeatedly, UI thread) with each incremental
//              text delta as it arrives.
//   on_done  — invoked exactly once (UI thread) when the stream ends:
//              ok=true on success; ok=false with a human-readable
//              `error` (network error string, or the provider's error
//              body text on an HTTP error).
// Returns a handle; keep it alive for the duration of the stream.
std::unique_ptr<CloudChatStream> StartCloudChat(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    const CloudChatRequest& request,
    base::RepeatingCallback<void(const std::string& delta)> on_token,
    base::OnceCallback<void(bool ok, const std::string& error)> on_done);

// ==================================================================
// Model listing
// ==================================================================

// Fetch the list of model ids the given provider/key can use.
//   on_done — invoked once (UI thread): ok=true with the model id list
//             (provider-native ids, e.g. "gpt-5.1"); ok=false with an
//             `error` string.
void FetchCloudModels(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    const std::string& provider_id,
    const std::string& api_key,
    const std::string& base_url,
    base::OnceCallback<
        void(bool ok, std::vector<std::string> models, std::string error)>
        on_done);

// ==================================================================
// Cloud model-id helpers
// ==================================================================
//
// A cloud model id is namespaced "provider:model" (e.g.
// "openai:gpt-5.1") to distinguish it from a local GGUF model id.

// True if `id` names a cloud model (contains ':').
bool IsCloudModelId(const std::string& id);

// Split "provider:model" into its parts. Returns false (and leaves
// outputs untouched) if `id` has no ':'. Only the first ':' splits —
// the model portion may itself contain ':'.
bool SplitCloudModelId(const std::string& id,
                       std::string* provider,
                       std::string* model);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_CLOUD_CLOUD_PROVIDER_H_
