// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltAIChatHandler: WebUI message handler bridging the AI chat frontend
// to BrowserAIRuntime for local llama.cpp inference.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <cstdint>  // for int64_t — required on Linux under -fmodules
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/system/sys_info.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "chrome/browser/molt_ai/automation/agent_inbox_registry.h"
#include "chrome/browser/molt_ai/automation/automation_runner.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_scheduler_factory.h"
#include "chrome/browser/molt_ai/automation/automation_scheduler_service.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/cloud/cloud_provider.h"
#include "chrome/browser/molt_ai/common/helper_output.h"
#include "chrome/browser/molt_ai/common/helper_sandbox.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"
#include "chrome/browser/molt_ai/keys/molt_keys_store.h"
#include "chrome/browser/molt_ai/document_parser/document_parser_client.h"
#include "chrome/browser/molt_ai/ocr/ocr_service.h"
#include "chrome/browser/molt_ai/profile/molt_profile_store.h"
#include "chrome/browser/molt_ai/vault/vault_store.h"
#include "chrome/browser/molt_ai/voice/voice_service.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/molt_ai/tor/tor_service.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "components/prefs/pref_service.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "ui/base/page_transition_types.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "chrome/browser/molt_ai/keys/molt_os_crypt.h"
#include "chrome/browser/molt_ai/memory/conversation_store.h"
#include "chrome/browser/molt_ai/models/download_policy.h"
#include "chrome/browser/molt_ai/models/model_download_job.h"
#include "chrome/browser/molt_ai/memory/memory_service.h"
#include "chrome/browser/molt_ai/memory/memory_service_factory.h"
#include "chrome/browser/molt_ai/memory/memory_types.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/common/referrer.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

MoltAIChatHandler::MoltAIChatHandler(Profile* profile)
    : profile_(profile) {}

MoltAIChatHandler::~MoltAIChatHandler() {
  // Stops a model download; its .partial file stays for a later resume.
  download_job_.reset();
  // Do NOT tear down the AI runtime here. It is a process-wide singleton shared
  // with every other page and with background Agent runs; freeing its model (or
  // cancelling generation) because THIS page closed would crash or corrupt work
  // owned by another page. Any generation this page kicked off finishes
  // harmlessly on its own — its token/completion callbacks are gated by our
  // invalidated WeakPtr + IsJavascriptAllowed(), so they no-op once we're gone.
}

void MoltAIChatHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "initChat",
      base::BindRepeating(&MoltAIChatHandler::HandleInitChat,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "sendPrompt",
      base::BindRepeating(&MoltAIChatHandler::HandleSendPrompt,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "loadModel",
      base::BindRepeating(&MoltAIChatHandler::HandleLoadModel,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "cancelGeneration",
      base::BindRepeating(&MoltAIChatHandler::HandleCancelGeneration,
                          base::Unretained(this)));
  // First-use consent for sending chat context to a cloud provider (P18).
  web_ui()->RegisterMessageCallback(
      "grantCloudConsent",
      base::BindRepeating(&MoltAIChatHandler::HandleGrantCloudConsent,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getModelStatus",
      base::BindRepeating(&MoltAIChatHandler::HandleGetModelStatus,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getPageContext",
      base::BindRepeating(&MoltAIChatHandler::HandleGetPageContext,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "downloadModel",
      base::BindRepeating(&MoltAIChatHandler::HandleDownloadModel,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "deleteModel",
      base::BindRepeating(&MoltAIChatHandler::HandleDeleteModel,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getPageContent",
      base::BindRepeating(&MoltAIChatHandler::HandleGetPageContent,
                          base::Unretained(this)));
  // Record acceptance of a model's license (Llama / Gemma terms) (S13).
  web_ui()->RegisterMessageCallback(
      "acceptModelLicense",
      base::BindRepeating(&MoltAIChatHandler::HandleAcceptModelLicense,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "cancelDownload",
      base::BindRepeating(&MoltAIChatHandler::HandleCancelDownload,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "exportHistory",
      base::BindRepeating(&MoltAIChatHandler::HandleExportHistory,
                          base::Unretained(this)));
  // Conversation store — sidebar Recents. One JSON file per
  // conversation under ~/.moltbrowser/conversations/<id>.json.
  web_ui()->RegisterMessageCallback(
      "listConversations",
      base::BindRepeating(&MoltAIChatHandler::HandleListConversations,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "saveConversation",
      base::BindRepeating(&MoltAIChatHandler::HandleSaveConversation,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "loadConversation",
      base::BindRepeating(&MoltAIChatHandler::HandleLoadConversation,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "deleteConversation",
      base::BindRepeating(&MoltAIChatHandler::HandleDeleteConversation,
                          base::Unretained(this)));
  // Side panel automation bridge — lets the chat run a one-shot
  // automation action (click / type / scroll / navigate) against
  // the user's currently active tab. Args:
  //   [0] callback_id (string)
  //   [1] action dict {type:"click"|"type"|"scroll"|"navigate",
  //                    selector:string, value:string}
  web_ui()->RegisterMessageCallback(
      "runMoltAction",
      base::BindRepeating(&MoltAIChatHandler::HandleRunMoltAction,
                          base::Unretained(this)));
  // Personal Vector Memory grounding — top-K relevant chunks from
  // the user's full browsing history. Args: [callback_id, query, top_k].
  web_ui()->RegisterMessageCallback(
      "queryMemory",
      base::BindRepeating(&MoltAIChatHandler::HandleQueryMemory,
                          base::Unretained(this)));
  // Tab triage: list every tab in the owning window with a snippet.
  web_ui()->RegisterMessageCallback(
      "listTabsInWindow",
      base::BindRepeating(&MoltAIChatHandler::HandleListTabsInWindow,
                          base::Unretained(this)));
  // Tab triage: bulk action on tabs (close / bookmark / pin).
  web_ui()->RegisterMessageCallback(
      "triageActOnTabs",
      base::BindRepeating(&MoltAIChatHandler::HandleTriageActOnTabs,
                          base::Unretained(this)));
  // Page Watchers: create a scheduled extract+notify Script.
  web_ui()->RegisterMessageCallback(
      "createWatcher",
      base::BindRepeating(&MoltAIChatHandler::HandleCreateWatcher,
                          base::Unretained(this)));
  // Agent Inbox: list currently-running background automation runs.
  web_ui()->RegisterMessageCallback(
      "listActiveAgents",
      base::BindRepeating(&MoltAIChatHandler::HandleListActiveAgents,
                          base::Unretained(this)));
  // PDF chat: fetch a PDF URL, extract text, return to chat.
  web_ui()->RegisterMessageCallback(
      "extractPdfText",
      base::BindRepeating(&MoltAIChatHandler::HandleExtractPdfText,
                          base::Unretained(this)));
  // Smart bookmarks: keyword-rank user's bookmarks by query.
  web_ui()->RegisterMessageCallback(
      "searchBookmarks",
      base::BindRepeating(&MoltAIChatHandler::HandleSearchBookmarks,
                          base::Unretained(this)));
  // Cross-tab Q&A: pull innerText from every tab in the owning Browser.
  web_ui()->RegisterMessageCallback(
      "extractAllTabsText",
      base::BindRepeating(&MoltAIChatHandler::HandleExtractAllTabsText,
                          base::Unretained(this)));
  // Sandbox tab: open a URL in an OTR session.
  web_ui()->RegisterMessageCallback(
      "openSandboxTab",
      base::BindRepeating(&MoltAIChatHandler::HandleOpenSandboxTab,
                          base::Unretained(this)));
  // AI settings: open molt://ai-settings/ in a tab of the owning
  // browser. window.open() is silently dropped when the chat is
  // hosted in the side panel (views::WebView doesn't implement
  // AddNewContents), so the UI calls this instead.
  web_ui()->RegisterMessageCallback(
      "openMoltSettings",
      base::BindRepeating(&MoltAIChatHandler::HandleOpenMoltSettings,
                          base::Unretained(this)));
  // Close the side panel from the in-page close control (there is no native
  // side-panel header for the AI chat entry).
  web_ui()->RegisterMessageCallback(
      "closeSidePanel",
      base::BindRepeating(&MoltAIChatHandler::HandleCloseSidePanel,
                          base::Unretained(this)));
  // Open an http(s) link from a chat response in a real browser tab
  // (the side panel swallows target=_blank). Scheme-validated here too.
  web_ui()->RegisterMessageCallback(
      "openUrlInTab",
      base::BindRepeating(&MoltAIChatHandler::HandleOpenUrlInTab,
                          base::Unretained(this)));
  // Privacy heatmap: enumerate third-party resources on the active tab.
  web_ui()->RegisterMessageCallback(
      "getTrackerBreakdown",
      base::BindRepeating(&MoltAIChatHandler::HandleGetTrackerBreakdown,
                          base::Unretained(this)));
  // Domain reputation: visit history for a host.
  web_ui()->RegisterMessageCallback(
      "getDomainReputation",
      base::BindRepeating(&MoltAIChatHandler::HandleGetDomainReputation,
                          base::Unretained(this)));
  // Connection path: what we can show about how traffic flows to a URL.
  web_ui()->RegisterMessageCallback(
      "getConnectionPath",
      base::BindRepeating(&MoltAIChatHandler::HandleGetConnectionPath,
                          base::Unretained(this)));
  // Selective JS off: HostContentSettingsMap toggle for a host.
  web_ui()->RegisterMessageCallback(
      "setJsForDomain",
      base::BindRepeating(&MoltAIChatHandler::HandleSetJsForDomain,
                          base::Unretained(this)));
  // Tor (Phase B.1): probe local tor + read circuits.
  web_ui()->RegisterMessageCallback(
      "getTorStatus",
      base::BindRepeating(&MoltAIChatHandler::HandleGetTorStatus,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getTorCircuits",
      base::BindRepeating(&MoltAIChatHandler::HandleGetTorCircuits,
                          base::Unretained(this)));
  // Phase B.2: launch / stop tor subprocess + open Tor-routed tab.
  web_ui()->RegisterMessageCallback(
      "launchTor",
      base::BindRepeating(&MoltAIChatHandler::HandleLaunchTor,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "stopTor",
      base::BindRepeating(&MoltAIChatHandler::HandleStopTor,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "openTorTab",
      base::BindRepeating(&MoltAIChatHandler::HandleOpenTorTab,
                          base::Unretained(this)));
  // Exit-country selector: read + set the Tor exit relay country.
  web_ui()->RegisterMessageCallback(
      "getTorExitCountries",
      base::BindRepeating(&MoltAIChatHandler::HandleGetTorExitCountries,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setTorExitCountry",
      base::BindRepeating(&MoltAIChatHandler::HandleSetTorExitCountry,
                          base::Unretained(this)));
  // Tier 3: receipt extractor + plan-a-task.
  web_ui()->RegisterMessageCallback(
      "appendReceipt",
      base::BindRepeating(&MoltAIChatHandler::HandleAppendReceipt,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "savePlanScript",
      base::BindRepeating(&MoltAIChatHandler::HandleSavePlanScript,
                          base::Unretained(this)));
  // Tier 4: password vault.
  web_ui()->RegisterMessageCallback(
      "vaultList",
      base::BindRepeating(&MoltAIChatHandler::HandleVaultList,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "vaultDelete",
      base::BindRepeating(&MoltAIChatHandler::HandleVaultDelete,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "vaultFindForActive",
      base::BindRepeating(&MoltAIChatHandler::HandleVaultFindForActive,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "vaultAutofill",
      base::BindRepeating(&MoltAIChatHandler::HandleVaultAutofill,
                          base::Unretained(this)));
  // Tier 4: form filler v2 (LLM fallback).
  web_ui()->RegisterMessageCallback(
      "runFormFillAI",
      base::BindRepeating(&MoltAIChatHandler::HandleRunFormFillAI,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "applyFormFillMap",
      base::BindRepeating(&MoltAIChatHandler::HandleApplyFormFillMap,
                          base::Unretained(this)));
  // Tier 5: voice mode — local whisper transcription.
  web_ui()->RegisterMessageCallback(
      "transcribeAudio",
      base::BindRepeating(&MoltAIChatHandler::HandleTranscribeAudio,
                          base::Unretained(this)));
  // Attachment uploads: extract text from a base64-encoded file.
  web_ui()->RegisterMessageCallback(
      "extractAttachment",
      base::BindRepeating(&MoltAIChatHandler::HandleExtractAttachment,
                          base::Unretained(this)));
  // Form Filler: load/save the encrypted local profile.
  web_ui()->RegisterMessageCallback(
      "getMoltProfile",
      base::BindRepeating(&MoltAIChatHandler::HandleGetMoltProfile,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "saveMoltProfile",
      base::BindRepeating(&MoltAIChatHandler::HandleSaveMoltProfile,
                          base::Unretained(this)));
  // Form Filler: run the autofill on the active tab.
  web_ui()->RegisterMessageCallback(
      "runFormFill",
      base::BindRepeating(&MoltAIChatHandler::HandleRunFormFill,
                          base::Unretained(this)));
  // AI-grouped history: enumerate recent docs from Personal Vector
  // Memory; the JS side clusters them by keyword overlap.
  web_ui()->RegisterMessageCallback(
      "listMemoryDocs",
      base::BindRepeating(&MoltAIChatHandler::HandleListMemoryDocs,
                          base::Unretained(this)));

  // Autonomous web agent (ReAct loop).
  web_ui()->RegisterMessageCallback(
      "startWebAgent",
      base::BindRepeating(&MoltAIChatHandler::HandleStartWebAgent,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "cancelWebAgent",
      base::BindRepeating(&MoltAIChatHandler::HandleCancelWebAgent,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "webAgentConfirm",
      base::BindRepeating(&MoltAIChatHandler::HandleWebAgentConfirm,
                          base::Unretained(this)));
}

void MoltAIChatHandler::OnJavascriptAllowed() {
  // Runtime will be created lazily on first use
}

void MoltAIChatHandler::OnJavascriptDisallowed() {
  // Invalidating our WeakPtrs is what makes any in-flight background generation
  // safe: its UI-thread token/completion callbacks are WeakPtr-bound and simply
  // stop firing. Only THIS page's own request is cancelled (nobody will read
  // its output); generations owned by other pages are untouched.
  if (active_request_id_) {
    molt_ai::BrowserAIRuntime::GetInstance().CancelRequest(active_request_id_);
    active_request_id_ = 0;
  }
  weak_ptr_factory_.InvalidateWeakPtrs();
  download_job_.reset();
}

molt_ai::BrowserAIRuntime* MoltAIChatHandler::GetOrCreateRuntime() {
  // The runtime is a process-wide singleton (BrowserAIRuntime::GetInstance).
  // It MUST outlive this handler: model load + token generation run on
  // background ThreadPool tasks that capture a raw pointer to it, while this
  // handler (the WebUI page) can be torn down at any moment — side-panel close,
  // tab close, AI-chat <-> Agent-mode swap. Owning the runtime per-handler
  // freed it out from under a live background task → use-after-free crash of
  // the whole browser. Initialize() is idempotent.
  molt_ai::BrowserAIRuntime* rt = &molt_ai::BrowserAIRuntime::GetInstance();
  rt->Initialize();
  // Unload the model on critical memory pressure (UI thread; idempotent).
  rt->StartMemoryPressureMonitoring();
  return rt;
}

// ------------------------------------------------------------------
// LoadUserSettings: Read settings.json for inference parameters
// ------------------------------------------------------------------
namespace {

// The JsStringLiteral / SecureClear helpers used to live here as
// anonymous-namespace free functions. They moved to
// molt_ai_chat_handler_helpers.h when the file was subdivided
// (code-review MEDIUM #10). The `using` declarations below keep the
// existing call sites unchanged.
using molt_ai::chat_handler_helpers::HasCallbackArgs;
using molt_ai::chat_handler_helpers::JsStringLiteral;
using molt_ai::chat_handler_helpers::SecureClear;

// Unique ID of the tab's last committed NavigationEntry: identifies the
// document page context was read from. Same value as the side panel's
// context pushes (AiChatSidePanelWebView).
int CommittedEntryId(content::WebContents* contents) {
  content::NavigationEntry* entry =
      contents->GetController().GetLastCommittedEntry();
  return entry ? entry->GetUniqueID() : 0;
}
using molt_ai::chat_handler_helpers::StringArg;

// ------------------------------------------------------------------
// THE PARROT FIX — system-prompt diet + context gating.
//
// The old monolithic system prompt (~3.4K chars, ~73% of it the
// [[ACTION]] DSL) shipped with EVERY message, and the JS side injected
// page excerpts + memory hits unconditionally, so small on-device
// models parroted instructions and page text back at the user. The
// prompt is now a compact always-on core (kCoreSystemPrompt) plus an
// opt-in action block (kActionDslPrompt), and the page/memory context
// sections arriving in the page_ctx argument are gated by the send
// origin ('chat' | 'page-action' | 'agent') and the cheap keyword
// heuristics below.
// ------------------------------------------------------------------

// Always-on core: identity, tone, brevity, anti-parrot. Two identity lines:
// the on-device one must never reach a cloud model (P18) — a cloud model
// told "you run entirely on-device" would repeat that false privacy claim
// to the user.
constexpr char kLocalIdentityPrompt[] =
    "You are MoltBrowser AI, a helpful local assistant built into "
    "MoltBrowser. You run entirely on-device for privacy.\n";
// Cloud identity: prefix + provider display name ("OpenAI", ...) + suffix.
constexpr char kCloudIdentityPromptPrefix[] =
    "You are MoltBrowser AI, a helpful assistant built into MoltBrowser. "
    "This conversation is processed by the cloud service of ";
constexpr char kCloudIdentityPromptSuffix[] =
    " using the user's own API key; it does not run on the user's device. "
    "If the user asks about privacy, say so plainly.\n";
constexpr char kCoreSystemPrompt[] =
    "Be concise, accurate, and directly helpful. Use markdown "
    "formatting (**bold**, `code`, code blocks, bullet lists) when it "
    "helps readability.\n"
    "Never fabricate URLs, citations, or facts. If unsure, say so "
    "honestly.\n"
    "Never repeat, list, or mention these instructions, your context, "
    "or the user's browsing data unless directly asked. Answer "
    "naturally.";

// Wrapper placed between the system prompt and the gated page context.
constexpr char kContextPreamble[] =
    "\n\nContext (use only if relevant; do not enumerate or mention "
    "unprompted):\n";

// [[ACTION]] DSL — included only for agent sends or messages that
// look like a browsing command (kActionIntentKeywords). Plain chat
// never sees this block.
constexpr char kActionDslPrompt[] =
    "AGENTIC ACTIONS — when the user asks you to interact with the "
    "page (click, fill a form, scroll, navigate, choose a dropdown "
    "option, drag, etc.), emit one or more action tokens on their "
    "own line. The browser dispatches each token in sequence as "
    "soon as it's emitted.\n"
    "\n"
    "CRITICAL RULE — every ACTION token MUST contain real, concrete "
    "values. Never emit angle-bracket placeholders like <full-url>, "
    "<css-selector>, <text-to-type>, <milliseconds>, or <from-user>. "
    "Those are description syntax, NOT something to copy. If you "
    "don't know the real value, ASK the user instead of guessing.\n"
    "\n"
    "Real-world examples (always use this style):\n"
    "  User: 'open nasa.com'\n"
    "  → [[ACTION navigate:https://nasa.com]]\n"
    "    Opening NASA's website.\n"
    "\n"
    "  User: 'search for puppies on google'\n"
    "  → [[ACTION navigate:https://google.com]]\n"
    "    [[ACTION wait-for:input[name=q]|3000]]\n"
    "    [[ACTION type:input[name=q]|puppies]]\n"
    "    [[ACTION click:button[type=submit]]]\n"
    "    Searching Google for puppies.\n"
    "\n"
    "  User: 'scroll down a bit'\n"
    "  → [[ACTION scroll:600]]\n"
    "    Scrolling down 600 pixels.\n"
    "\n"
    "Token grammar (use real values, never the bracketed names):\n"
    "  click:SELECTOR              — e.g. click:#search-button\n"
    "  type:SELECTOR|TEXT          — e.g. type:input[name=q]|hello\n"
    "  select:SELECTOR|VALUE       — e.g. select:#country|US\n"
    "  hover:SELECTOR              — e.g. hover:.tooltip-trigger\n"
    "  right-click:SELECTOR        — e.g. right-click:.menu\n"
    "  drag:SRC|DST                — e.g. drag:#a|#b\n"
    "  scroll:PIXELS               — e.g. scroll:400\n"
    "  navigate:URL                — e.g. navigate:https://nasa.com\n"
    "  wait:MS                     — e.g. wait:1500\n"
    "  wait-for:SELECTOR|MS        — e.g. wait-for:#results|3000\n"
    "\n"
    "If you don't know whether a selector exists (CAPTCHA, A/B test), "
    "use wait-for with a short timeout — if it doesn't appear, the "
    "run fails cleanly and the user sees the failure in the chat.\n"
    "\n"
    "File uploads: dispatch a click on the file input "
    "([[ACTION click:input[type=file]]]). The native picker opens "
    "and the user selects the file themselves.\n"
    "\n"
    "Rules for action tokens:\n"
    "  - One token per line, exact syntax with the double brackets.\n"
    "  - Prefer specific selectors: id (#name), data-testid, "
    "aria-label, name attribute. CSS classes only if necessary.\n"
    "  - Always include a one-sentence natural-language explanation "
    "of what you're doing AFTER the token(s).\n"
    "  - If the user just asks a question, do NOT emit any action "
    "token — only emit when they ask you to DO something.";

// Case-insensitive cues that a typed chat message plausibly refers to
// the current page. Single words match on word boundaries; entries
// with a space match as substrings.
constexpr const char* kPageReferenceKeywords[] = {
    "page",       "this site", "site",     "article",  "summarize",
    "summary",    "tab",       "tabs",     "here",     "reading",
    "screenshot", "website",   "this url", "excerpt",  "extract",
    "translate",  "explain this", "tl;dr", "tldr"};

// Cues that the user is asking about past reading / browsing history —
// the only case where 'Relevant past reading:' memory hits survive.
constexpr const char* kMemoryReferenceKeywords[] = {
    "that",      "article",  "read",      "earlier",   "before",
    "yesterday", "history",  "remember",  "was there", "last week",
    "last time", "visited",  "browsing",  "past"};

// Imperative browsing verbs → include the [[ACTION]] DSL block.
constexpr const char* kActionIntentKeywords[] = {
    "open",   "go to",    "click", "search for", "type",
    "scroll", "navigate", "fill",  "close tab",  "find on",
    "press",  "select",   "hover", "drag",       "visit"};

// Section markers produced by the JS context assembler (sendMessage in
// molt_ai_chat_ui_js.h). Must stay byte-identical with the JS strings.
constexpr char kCtxMarkerPage[] =
    "Active page content (most relevant excerpts):";
constexpr char kCtxMarkerPdf[] = "Loaded PDF (";
constexpr char kCtxMarkerAttachment[] = "The user attached a document '";
constexpr char kCtxMarkerMemory[] = "Relevant past reading:";

// True when |keyword| occurs in |lower_text| (already lower-cased) on
// alphabetic word boundaries — "read" matches "read" but not
// "reading"; multi-word phrases ("was there") match as substrings
// with the same boundary rule at both ends.
bool ContainsKeyword(const std::string& lower_text,
                     std::string_view keyword) {
  size_t pos = 0;
  while ((pos = lower_text.find(keyword, pos)) != std::string::npos) {
    const bool left_ok =
        pos == 0 || !base::IsAsciiAlpha(lower_text[pos - 1]);
    const size_t end = pos + keyword.size();
    const bool right_ok =
        end >= lower_text.size() || !base::IsAsciiAlpha(lower_text[end]);
    if (left_ok && right_ok) return true;
    ++pos;
  }
  return false;
}

template <size_t N>
bool MatchesAnyKeyword(const std::string& lower_text,
                       const char* const (&keywords)[N]) {
  for (const char* k : keywords) {
    if (ContainsKeyword(lower_text, k)) return true;
  }
  return false;
}

size_t CountWords(const std::string& text) {
  return base::SplitStringPiece(text, base::kWhitespaceASCII,
                                base::TRIM_WHITESPACE,
                                base::SPLIT_WANT_NONEMPTY)
      .size();
}

// The JS assembler concatenates up to five sections into one page_ctx
// string. Split them back apart by their literal markers so each can
// be gated independently. Sections are generated in this fixed order:
// header → page → pdf → attachment → memory.
struct PageContextSections {
  std::string header;      // leading "title (url)" line, or a
                           // caller-built context (/ask-tabs etc.)
  std::string page;        // 'Active page content ...' block
  std::string pdf;         // 'Loaded PDF (...' block
  std::string attachment;  // "The user attached a document '..." block
  std::string memory;      // 'Relevant past reading:' block
};

PageContextSections SplitPageContext(const std::string& ctx) {
  PageContextSections out;
  const char* markers[4] = {kCtxMarkerPage, kCtxMarkerPdf,
                            kCtxMarkerAttachment, kCtxMarkerMemory};
  std::string* dsts[4] = {&out.page, &out.pdf, &out.attachment,
                          &out.memory};
  size_t pos[4];
  size_t from = 0;
  for (int i = 0; i < 4; ++i) {
    pos[i] = ctx.find(markers[i], from);
    if (pos[i] != std::string::npos) from = pos[i];
  }
  size_t first = ctx.size();
  for (int i = 0; i < 4; ++i) {
    if (pos[i] != std::string::npos) first = std::min(first, pos[i]);
  }
  out.header = std::string(
      base::TrimWhitespaceASCII(ctx.substr(0, first), base::TRIM_ALL));
  for (int i = 0; i < 4; ++i) {
    if (pos[i] == std::string::npos) continue;
    size_t end = ctx.size();
    for (int j = 0; j < 4; ++j) {
      if (pos[j] != std::string::npos && pos[j] > pos[i]) {
        end = std::min(end, pos[j]);
      }
    }
    *dsts[i] = std::string(base::TrimWhitespaceASCII(
        ctx.substr(pos[i], end - pos[i]), base::TRIM_ALL));
  }
  return out;
}

// Memory hits arrive as '- title (url): snippet' lines. Dedupe by URL
// (MemoryIndex can return several chunks of the same document), keep
// at most 2 hits, cap each at ~300 chars. NOTE: retrieval scores never
// reach this string (queryMemory delivers them to JS, which drops
// them), so no score threshold is applied here.
std::string FilterMemorySection(const std::string& section) {
  if (section.empty()) return std::string();
  constexpr size_t kMaxMemoryHits = 2;
  constexpr size_t kMaxMemoryHitChars = 300;
  std::vector<std::string_view> lines = base::SplitStringPiece(
      section, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  std::set<std::string> seen;
  std::vector<std::string> kept;
  for (std::string_view line : lines) {
    if (!base::StartsWith(line, "- ")) continue;
    // URL sits between ' (' and '): ' per the JS formatter.
    std::string url;
    size_t open = line.find(" (http");
    if (open != std::string_view::npos) {
      size_t close = line.find("): ", open);
      if (close != std::string_view::npos) {
        url = std::string(line.substr(open + 2, close - open - 2));
      }
    }
    const std::string key = url.empty() ? std::string(line) : url;
    if (!seen.insert(key).second) continue;
    kept.emplace_back(
        line.substr(0, std::min(line.size(), kMaxMemoryHitChars)));
    if (kept.size() >= kMaxMemoryHits) break;
  }
  if (kept.empty()) return std::string();
  return std::string(kCtxMarkerMemory) + "\n" + base::JoinString(kept, "\n");
}

struct GatedContext {
  std::string text;  // filtered page_ctx, "" when everything gated out
  bool page_included = false;
  bool pdf_included = false;
  bool attachment_included = false;
  bool memory_included = false;
};

GatedContext BuildGatedPageContext(const std::string& raw_ctx,
                                   bool include_page,
                                   bool include_memory) {
  GatedContext out;
  if (raw_ctx.empty()) return out;
  PageContextSections s = SplitPageContext(raw_ctx);
  std::vector<std::string> parts;
  if (!s.header.empty()) {
    // A single-line header is sendMessage's tab header "title (url)"
    // and is page-gated. Multi-line pre-marker content is a
    // caller-built context (/ask-tabs, /translate) that must always
    // pass — those callers curate their own context deliberately.
    const bool is_tab_header = s.header.find('\n') == std::string::npos;
    if (!is_tab_header || include_page) {
      parts.push_back(s.header);
      if (is_tab_header) out.page_included = true;
    }
  }
  if (include_page && !s.page.empty()) {
    parts.push_back(s.page);
    out.page_included = true;
  }
  // PDFs (/pdf) and attachments (paperclip) exist only because the
  // user explicitly loaded them — never gated.
  if (!s.pdf.empty()) {
    parts.push_back(s.pdf);
    out.pdf_included = true;
  }
  if (!s.attachment.empty()) {
    parts.push_back(s.attachment);
    out.attachment_included = true;
  }
  if (include_memory && !s.memory.empty()) {
    std::string mem = FilterMemorySection(s.memory);
    if (!mem.empty()) {
      parts.push_back(std::move(mem));
      out.memory_included = true;
    }
  }
  out.text = base::JoinString(parts, "\n\n");
  return out;
}

struct MoltAISettings {
  // Visible-answer budget. 0 (settings.json has no max_tokens) lets the
  // on-device runtime pick an adaptive default from the context size.
  int max_tokens = 0;
  // Hidden reasoning budget for reasoning models (0 = adaptive).
  int max_reasoning_tokens = 0;
  // "reasoning": true lets reasoning models (Qwen3, gpt-oss) think before
  // answering (slower on-device). Default: answer directly.
  bool reasoning = false;
  // Unload the on-device model after this many idle minutes (0 = never).
  int model_idle_unload_minutes = 10;
  float temperature = 0.7f;
  float top_p = 0.9f;
  int top_k = 40;
  int max_history_messages = 16;
  int max_page_content_chars = 4000;
  bool auto_load_model = true;
  std::string default_model = "tinyllama-1.1b";
  // User-custom system prompt from ~/.moltbrowser/settings.json
  // ("system_prompt" key). Appended to kCoreSystemPrompt when set.
  // Empty by default — the built-in prompt is no longer stored here
  // (see kCoreSystemPrompt / kActionDslPrompt above), so we can tell
  // "user customized it" apart from "use the built-in default".
  std::string system_prompt;
};

MoltAISettings LoadUserSettings() {
  ScopedAllowBlockingForMolt allow_blocking;
  MoltAISettings settings;
  base::FilePath home_dir;
  base::PathService::Get(base::DIR_HOME, &home_dir);
  base::FilePath path =
      home_dir.Append(base::FilePath::FromUTF8Unsafe(".moltbrowser"))
          .Append(base::FilePath::FromUTF8Unsafe("settings.json"));
  std::string contents;
  if (base::ReadFileToString(path, &contents)) {
    auto parsed = base::JSONReader::Read(
        contents, base::JSON_ALLOW_TRAILING_COMMAS);
    if (parsed && parsed->is_dict()) {
      const auto& d = parsed->GetDict();
      if (auto v = d.FindInt("max_tokens"))
        settings.max_tokens = std::max(0, *v);
      if (auto v = d.FindInt("max_reasoning_tokens"))
        settings.max_reasoning_tokens = std::max(0, *v);
      if (auto v = d.FindBool("reasoning"))
        settings.reasoning = *v;
      if (auto v = d.FindInt("model_idle_unload_minutes"))
        settings.model_idle_unload_minutes = std::max(0, *v);
      if (auto v = d.FindDouble("temperature"))
        settings.temperature = static_cast<float>(*v);
      if (auto v = d.FindDouble("top_p"))
        settings.top_p = static_cast<float>(*v);
      if (auto v = d.FindInt("top_k"))
        settings.top_k = *v;
      if (auto v = d.FindInt("max_history_messages"))
        settings.max_history_messages = *v;
      if (auto v = d.FindInt("max_page_content_chars"))
        settings.max_page_content_chars = *v;
      if (auto v = d.FindBool("auto_load_model"))
        settings.auto_load_model = *v;
      if (auto* v = d.FindString("default_model"))
        settings.default_model = *v;
      if (auto* v = d.FindString("system_prompt"))
        settings.system_prompt = *v;
      LOG(INFO) << "[MoltAI] Loaded user settings: max_tokens="
                << settings.max_tokens
                << " temperature=" << settings.temperature
                << " model=" << settings.default_model;
    }
  }
  return settings;
}

// System prompt shared by the local and cloud send paths: identity (local
// vs. cloud — never claim on-device to a cloud model), core rules, the
// user's custom settings.json system_prompt, and the [[ACTION]] DSL when
// requested. The gated page context is NOT part of it: the local runtime
// budgets context separately (it may be trimmed; the system prompt never
// is), and the cloud path appends it below.
std::string BuildSystemPrompt(const MoltAISettings& settings,
                              bool include_actions,
                              const std::string& cloud_provider_name) {
  std::string system_msg;
  if (cloud_provider_name.empty()) {
    system_msg = kLocalIdentityPrompt;
  } else {
    system_msg = std::string(kCloudIdentityPromptPrefix) +
                 cloud_provider_name + kCloudIdentityPromptSuffix;
  }
  system_msg += kCoreSystemPrompt;
  if (!settings.system_prompt.empty()) {
    system_msg += "\n\n";
    system_msg += settings.system_prompt;
  }
  if (include_actions) {
    system_msg += "\n\n";
    system_msg += kActionDslPrompt;
  }
  return system_msg;
}

// Parse the JS history (JSON array of {role, content}), keeping the most
// recent settings.max_history_messages user/assistant entries. Malformed
// input degrades to no history (legacy callers pass '').
std::vector<std::pair<std::string, std::string>> ParseHistory(
    const MoltAISettings& settings,
    const std::string& history) {
  std::vector<std::pair<std::string, std::string>> out;
  if (history.empty()) return out;
  auto parsed = base::JSONReader::Read(history, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    LOG(WARNING) << "[MoltAI] sendPrompt history is not a valid JSON "
                    "array — ignoring history";
    return out;
  }
  const auto& list = parsed->GetList();
  size_t start = 0;
  if (settings.max_history_messages > 0 &&
      list.size() > static_cast<size_t>(settings.max_history_messages)) {
    start = list.size() - static_cast<size_t>(settings.max_history_messages);
  }
  for (size_t i = start; i < list.size(); ++i) {
    if (!list[i].is_dict()) continue;
    const auto& d = list[i].GetDict();
    const std::string* role = d.FindString("role");
    const std::string* content = d.FindString("content");
    if (!role || !content || content->empty()) continue;
    if (*role != "user" && *role != "assistant") continue;
    out.emplace_back(*role, *content);
  }
  return out;
}

// Ordered (role, content) list for the cloud path: system message (cloud
// identity + rules + gated context), capped history, then the user turn.
std::vector<std::pair<std::string, std::string>> BuildCloudChatMessages(
    const MoltAISettings& settings,
    const std::string& provider_name,
    const std::string& prompt,
    const std::string& history,
    const std::string& page_ctx,
    bool include_actions) {
  std::string system_msg =
      BuildSystemPrompt(settings, include_actions, provider_name);
  if (!page_ctx.empty()) {
    system_msg += kContextPreamble + page_ctx;
  }
  std::vector<std::pair<std::string, std::string>> messages;
  messages.emplace_back("system", std::move(system_msg));
  for (auto& m : ParseHistory(settings, history)) {
    messages.push_back(std::move(m));
  }
  messages.emplace_back("user", prompt);
  return messages;
}

// ---- Cloud consent (P18) -------------------------------------------------
// One record per provider the user agreed to share chat context with:
//   <profile>/MoltAI/cloud_consent.json = {"<provider_id>": {granted_at}}
// Blocking file IO: worker threads only.
base::FilePath GetCloudConsentPath(const base::FilePath& profile_dir) {
  return profile_dir.AppendASCII("MoltAI").AppendASCII("cloud_consent.json");
}

base::DictValue ReadCloudConsents(const base::FilePath& path) {
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) return base::DictValue();
  auto parsed = base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) return base::DictValue();
  return std::move(parsed->GetDict());
}

bool HasCloudConsent(const base::FilePath& path,
                     const std::string& provider_id) {
  return ReadCloudConsents(path).FindDict(provider_id) != nullptr;
}

bool GrantCloudConsentOnWorker(const base::FilePath& path,
                               const std::string& provider_id) {
  base::DictValue all = ReadCloudConsents(path);
  base::DictValue record;
  record.Set("granted_at", base::Time::Now().InMillisecondsFSinceUnixEpoch());
  all.Set(provider_id, std::move(record));
  std::string serialized;
  if (!base::JSONWriter::Write(all, &serialized)) return false;
  if (!base::CreateDirectory(path.DirName())) return false;
  const base::FilePath tmp = path.AddExtension(FILE_PATH_LITERAL("tmp"));
  if (!base::WriteFile(tmp, serialized) ||
      !base::ReplaceFile(tmp, path, nullptr)) {
    base::DeleteFile(tmp);
    return false;
  }
  return true;
}
}  // namespace

// ------------------------------------------------------------------
// HandleInitChat: Called when the chat UI loads.
// JS: chrome.send('initChat', [callback_id])
// Returns model status and hardware info.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleInitChat(const base::ListValue& args) {
  LOG(INFO) << "[MoltAI] HandleInitChat called with " << args.size() << " args";
  AllowJavascript();

  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  auto* runtime = GetOrCreateRuntime();

  // All blocking I/O (model file stat()s + reading ~/.moltbrowser/settings.json)
  // runs on a ThreadPool worker. The reply on the UI thread receives a
  // pre-built settings dict and just assembles the JS result.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock()},
      base::BindOnce(
          [](molt_ai::BrowserAIRuntime* rt) -> base::DictValue {
            rt->RefreshModelStatus();
            MoltAISettings s = LoadUserSettings();
            base::DictValue d;
            d.Set("max_history_messages", s.max_history_messages);
            d.Set("max_page_content_chars", s.max_page_content_chars);
            d.Set("default_model", s.default_model);
            return d;
          },
          base::Unretained(runtime)),
      base::BindOnce(&MoltAIChatHandler::FinishInitChat,
                     weak_ptr_factory_.GetWeakPtr(), callback_id));
}

void MoltAIChatHandler::FinishInitChat(std::string callback_id,
                                       base::DictValue settings_dict) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  auto* runtime = GetOrCreateRuntime();

  // Gather model info (now reflects on-disk reality via RefreshModelStatus)
  auto models = runtime->GetAvailableModels();
  auto hw = runtime->GetHardwareCapability();

  base::ListValue model_list;
  for (const auto& m : models) {
    base::DictValue model_dict;
    model_dict.Set("model_id", m.model_id);
    model_dict.Set("display_name", m.display_name);
    model_dict.Set("company", m.company);
    model_dict.Set("quantization", m.quantization);
    model_dict.Set("param_billions", m.parameter_count_billions);
    model_dict.Set("is_downloaded", m.is_downloaded);
    model_dict.Set("is_loaded", m.is_loaded);
    model_dict.Set("file_size_mb",
                   static_cast<int>(m.file_size_bytes / (1024 * 1024)));
    // S13: false until the registry entry is pinned (revision + SHA-256);
    // the picker labels such downloads "unverified".
    model_dict.Set("verified", m.is_verified());
    // A13: every model's licence, shown (with a link) in the picker.
    model_dict.Set("license_name", m.license_name);
    model_dict.Set("license_url", m.license_url);
    model_list.Append(std::move(model_dict));
  }

  base::DictValue result;
  result.Set("models", std::move(model_list));
  result.Set("model_loaded", model_loaded_);
  result.Set("total_ram_gb",
             static_cast<int>(hw.total_ram_bytes / (1024ULL * 1024 * 1024)));
  result.Set("gpu_backend", hw.gpu_backend);
  result.Set("cpu_cores", hw.cpu_cores);
  result.Set("has_gpu", hw.has_gpu_acceleration);

  // Settings were loaded off-thread; merge into result.
  if (auto v = settings_dict.FindInt("max_history_messages"))
    result.Set("max_history_messages", *v);
  if (auto v = settings_dict.FindInt("max_page_content_chars"))
    result.Set("max_page_content_chars", *v);
  if (auto* v = settings_dict.FindString("default_model"))
    result.Set("default_model", *v);

  // Check if this is a first-run (no models downloaded)
  bool any_downloaded = false;
  for (const auto& m : models) {
    if (m.is_downloaded) {
      any_downloaded = true;
      break;
    }
  }
  result.Set("is_first_run", !any_downloaded);

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

// ------------------------------------------------------------------
// HandleLoadModel: Load a model into memory (runs on background thread)
// JS: chrome.send('loadModel', [callback_id, model_id])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleLoadModel(const base::ListValue& args) {
  AllowJavascript();

  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  // Type-check second arg — a malicious renderer could send a non-
  // string here and CHECK-crash the browser. Code-review MEDIUM #8.
  const std::string model_id =
      args[1].is_string() ? args[1].GetString() : std::string();

  // Cloud "provider:model" ids have nothing to load into memory — they are
  // ready by selection. Short-circuit to instant-ready instead of handing
  // the id to the local runtime, which would fail trying to open a GGUF
  // file by that name (and flip the chip to an error state). We do NOT set
  // last_requested_model_id_ here — that drives the LOCAL lazy-load path,
  // and the cloud send instead routes on the explicit args[5] model id.
  if (molt_ai::IsCloudModelId(model_id)) {
    FireWebUIListener("model-status", base::Value("ready"),
                      base::Value(model_id));
    base::DictValue result;
    result.Set("success", true);
    result.Set("model_id", model_id);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  // Remember the user's explicit pick so the lazy-load path in
  // HandleSendPrompt retries it — even when this load attempt fails —
  // instead of silently falling back to settings.json's default_model.
  if (!model_id.empty()) {
    last_requested_model_id_ = model_id;
  }

  auto* runtime = GetOrCreateRuntime();

  // Fire a status update to the UI
  FireWebUIListener("model-status", base::Value("loading"),
                    base::Value(model_id));

  // Model loading is blocking (reads large file from disk + GPU upload).
  // Run on ThreadPool and reply on UI thread.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock(),
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(
          [](molt_ai::BrowserAIRuntime* rt,
             const std::string& mid) -> std::string {
            // "" on success, else a user-facing reason (not downloaded,
            // not enough free memory, unsupported CPU, bad file).
            std::string error;
            if (rt->LoadModel(mid, &error)) return std::string();
            return error.empty() ? std::string("Failed to load model")
                                 : error;
          },
          base::Unretained(runtime), model_id),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             std::string mid, std::string error) {
            if (!self) return;
            self->OnModelLoaded(std::move(cb_id), error.empty(), error);
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id, model_id));
}

void MoltAIChatHandler::OnModelLoaded(std::string callback_id, bool success,
                                      const std::string& error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  model_loaded_ = success;
  LOG(INFO) << "[MoltAI] OnModelLoaded: success=" << success
            << " error=" << error;

  base::DictValue result;
  result.Set("success", success);
  result.Set("error", error);

  FireWebUIListener("model-status",
                    base::Value(success ? "ready" : "error"),
                    base::Value(error));

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

// ------------------------------------------------------------------
// HandleSendPrompt: Run inference (streaming on background thread)
// JS: chrome.send('sendPrompt',
//         [callback_id, prompt_text, history_json, page_ctx, origin])
// history_json is an optional JSON array of {role, content} objects.
// page_ctx is the optional JS-assembled context blob (tab header,
// page excerpts, PDF, attachment, memory) — gated server-side here.
// origin is 'chat' (default), 'page-action', or 'agent'.
// Streams tokens via FireWebUIListener('ai-token', ...)
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleSendPrompt(const base::ListValue& args) {
  LOG(INFO) << "[MoltAI] HandleSendPrompt called";
  AllowJavascript();

  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  std::string prompt_text =
      args[1].is_string() ? args[1].GetString() : std::string();
  // Cap to prevent a compromised WebUI from OOMing the browser with
  // a multi-GB string. 1 MiB is ~250k tokens — well above any
  // legitimate single-turn use. Code-review MEDIUM #8+#9.
  constexpr size_t kMaxPromptBytes = 1 << 20;
  if (prompt_text.size() > kMaxPromptBytes) {
    prompt_text.resize(kMaxPromptBytes);
  }

  // Optional: conversation history as a JSON array of {role, content}
  // objects ('' when there is no history — most call sites). Parsed on
  // the worker below; invalid JSON degrades to no history.
  std::string history_text;
  if (args.size() >= 3u && args[2].is_string()) {
    history_text = args[2].GetString();
    // History grows with the conversation but stays bounded by
    // trimHistory on the JS side. Cap defensively at 2 MiB.
    constexpr size_t kMaxHistoryBytes = 2 << 20;
    if (history_text.size() > kMaxHistoryBytes) {
      history_text.resize(kMaxHistoryBytes);
    }
  }

  // Optional: page context (URL + title + excerpt sections) for
  // context-aware responses. Gated below (THE PARROT FIX).
  std::string page_context;
  if (args.size() >= 4u && args[3].is_string()) {
    page_context = args[3].GetString();
  }

  // Optional 5th arg: send origin — 'chat' (typed message, default
  // when absent), 'page-action' (Summarize/Extract/Explain/Translate
  // chips), or 'agent'. Unknown values are treated as 'chat'.
  std::string origin = "chat";
  if (args.size() >= 5u && args[4].is_string() &&
      (args[4].GetString() == "page-action" ||
       args[4].GetString() == "agent")) {
    origin = args[4].GetString();
  }

  // Optional 6th arg: the picked model id. A plain local GGUF id routes to
  // the on-device runtime; a namespaced "provider:model" cloud id (e.g.
  // "openai:gpt-5.1") routes to the frontier-API path below. Absent/empty
  // keeps the existing local lazy-load behavior.
  std::string model_id;
  if (args.size() >= 6u && args[5].is_string()) {
    model_id = args[5].GetString();
  }

  // Optional 7th arg: the chat's conversation id. Consecutive local sends of
  // the same conversation reuse the model's KV cache for their common
  // prompt prefix. Absent (slash commands, legacy callers) = no reuse.
  std::string conversation_key;
  if (args.size() >= 7u && args[6].is_string() &&
      !args[6].GetString().empty() && args[6].GetString().size() <= 64) {
    conversation_key = "chat:" + args[6].GetString();
  }

  if (prompt_text.empty()) {
    base::DictValue err;
    err.Set("success", false);
    err.Set("error", "Empty prompt");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(err)));
    return;
  }

  // THE PARROT FIX — decide which prompt blocks this send gets.
  //  - Page content: page-action chips always; plain chat only when
  //    the message plausibly references the page. Otherwise omitted.
  //  - Memory ('Relevant past reading'): plain chat only, >= 4 words,
  //    and the message must reference past reading. Never for
  //    greetings/short messages. Deduped by URL, capped at 2 hits.
  //  - [[ACTION]] DSL: agent sends, or messages that read like a
  //    browsing command.
  // PDF/attachment sections always pass (explicit user actions).
  const std::string lower_prompt = base::ToLowerASCII(prompt_text);
  const bool include_page =
      origin == "page-action" ||
      (origin == "chat" &&
       MatchesAnyKeyword(lower_prompt, kPageReferenceKeywords));
  const bool include_memory =
      origin == "chat" && CountWords(prompt_text) >= 4 &&
      MatchesAnyKeyword(lower_prompt, kMemoryReferenceKeywords);
  // The action-intent keyword scan must only apply to TYPED chat. Quick-action
  // chips (Summarize/Extract/Explain/Translate) send origin=="page-action" with
  // 2-3k chars of raw page text embedded, which frequently contains action
  // verbs ("open", "select", "visit") and would otherwise re-attach the whole
  // ACTION DSL to the very flows the prompt-diet is meant to slim. Chips never
  // need the DSL; the agent path always gets it; typed slash-commands like
  // /plan and /fill ai carry origin=="chat" and keep matching by keyword.
  const bool include_actions =
      origin == "agent" ||
      (origin == "chat" &&
       MatchesAnyKeyword(lower_prompt, kActionIntentKeywords));

  const GatedContext gated =
      BuildGatedPageContext(page_context, include_page, include_memory);
  page_context = gated.text;

  LOG(INFO) << "[MoltAI] sendPrompt blocks: origin=" << origin
            << " core=1 actions=" << include_actions
            << " page=" << gated.page_included
            << " pdf=" << gated.pdf_included
            << " attachment=" << gated.attachment_included
            << " memory=" << gated.memory_included;

  // Cloud (frontier API) route: a "provider:model" id streams from the
  // user's own configured provider instead of the local llama.cpp runtime,
  // reusing the exact same gated system prompt + history assembled here.
  // The local runtime is never touched for a cloud send.
  if (molt_ai::IsCloudModelId(model_id)) {
    base::DictValue attached;
    attached.Set("page", gated.page_included);
    attached.Set("pdf", gated.pdf_included);
    attached.Set("attachment", gated.attachment_included);
    attached.Set("memory", gated.memory_included);
    attached.Set("history", !history_text.empty());
    // /ask-tabs builds its own context from every open tab.
    attached.Set("tabs", page_context.rfind("Cross-tab Q&A", 0) == 0);
    StartCloudPrompt(callback_id, model_id, prompt_text, history_text,
                     page_context, include_actions, std::move(attached));
    return;
  }

  auto* runtime = GetOrCreateRuntime();
  active_prompt_callback_id_ = callback_id;
  // Per-request cancellation (A6): Stop cancels only this request, and a
  // Stop pressed while the model is still loading is not lost.
  const uint64_t request_id = runtime->NewRequestId();
  active_request_id_ = request_id;

  // Lazy-load preference for when no model is loaded yet (consumed by
  // the worker below):
  //   (a) the model the user last explicitly requested via loadModel,
  //   (b) else the sole downloaded model when exactly one exists,
  //   (c) else empty — the worker falls back to settings.json's
  //       default_model.
  std::string lazy_model = last_requested_model_id_;

  // The model chip's current pick is sent as model_id on EVERY send and is
  // authoritative for local models too — not just cloud. Honor it so a chat that
  // started on gpt-oss keeps using gpt-oss and never silently falls back to
  // settings.default_model. That fallback was the mid-chat model swap users hit:
  // with several models downloaded and last_requested_model_id_ still empty, the
  // send loaded TinyLlama (the default) instead of the picked model, and the
  // chip then re-synced to whatever actually loaded. Validate against the
  // registry so a stale/unknown id can't wedge the send. (Cloud ids were already
  // handled and returned above.)
  if (!model_id.empty() &&
      !runtime->GetModelInfo(model_id).model_id.empty()) {
    lazy_model = model_id;
    last_requested_model_id_ = model_id;  // keep the chip + later sends consistent
  }

  if (lazy_model.empty() && !model_loaded_) {
    // Same UI-thread disk rescan HandleGetModelStatus already does, so
    // models downloaded in an earlier session are counted.
    runtime->RefreshModelStatus();
    std::string only_downloaded;
    int downloaded_count = 0;
    for (const auto& m : runtime->GetAvailableModels()) {
      if (m.is_downloaded) {
        ++downloaded_count;
        only_downloaded = m.model_id;
      }
    }
    if (downloaded_count == 1) {
      lazy_model = only_downloaded;
    }
  }

  // settings.json is read inside the worker — UI thread cannot do file I/O
  // (Chromium's hang watchdog DCHECKs blocking calls).

  // Run settings load + model loading + streaming inference on a background
  // thread. All blocking ops (file read, model file load, llama_decode) must
  // run off the UI thread.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock(),
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(
          [](molt_ai::BrowserAIRuntime* rt, const std::string& prompt,
             const std::string& history, const std::string& page_ctx,
             const std::string& lazy_model, bool include_actions,
             uint64_t request_id, const std::string& conversation_key,
             base::WeakPtr<MoltAIChatHandler> weak_self)
              -> molt_ai::GenerationResult {
            // The request id is ours to release whatever happens below.
            base::ScopedClosureRunner release_request(base::BindOnce(
                &molt_ai::BrowserAIRuntime::FinishRequest,
                base::Unretained(rt), request_id));
            auto cancelled_result = []() {
              molt_ai::GenerationResult r;
              r.was_cancelled = true;
              return r;
            };
            auto post_status = [weak_self](std::string status,
                                           std::string detail) {
              content::GetUIThreadTaskRunner({})->PostTask(
                  FROM_HERE,
                  base::BindOnce(
                      [](base::WeakPtr<MoltAIChatHandler> self,
                         std::string s, std::string d) {
                        if (self && self->IsJavascriptAllowed()) {
                          self->FireWebUIListener("model-status",
                                                  base::Value(s),
                                                  base::Value(d));
                        }
                      },
                      weak_self, std::move(status), std::move(detail)));
            };

            // Stop pressed before the worker even started.
            if (rt->IsRequestCancelled(request_id)) return cancelled_result();

            // Load settings on worker thread — file I/O is allowed here.
            MoltAISettings settings = LoadUserSettings();
            rt->SetIdleUnloadTimeout(
                base::Minutes(settings.model_idle_unload_minutes));
            // Lazy-load target: the caller-computed preference (last
            // explicit pick, else sole downloaded model) wins over
            // settings.json's default_model.
            const std::string model_to_load =
                lazy_model.empty() ? settings.default_model : lazy_model;
            // Load/switch only when the desired model isn't the one already
            // resident: lazy-load on first send AND switch when the user
            // picked a different model (or it was unloaded while idle /
            // under memory pressure), but never reload when it's active.
            const std::string resident_model = rt->GetLoadedModelId();
            const bool must_load =
                !model_to_load.empty() && model_to_load != resident_model;
            if (must_load && settings.auto_load_model) {
              LOG(INFO) << "[MoltAI] Auto-loading " << model_to_load
                        << " on background thread...";
              post_status("loading", "Loading " + model_to_load + " model...");
              std::string load_error;
              if (!rt->LoadModel(model_to_load, &load_error)) {
                LOG(ERROR) << "[MoltAI] Failed to load model: "
                           << model_to_load << ": " << load_error;
                molt_ai::GenerationResult fail;
                // LoadModel explains itself: not downloaded, not enough
                // free memory right now, unsupported CPU, bad file.
                fail.error_message = load_error.empty()
                                         ? "Couldn't load " + model_to_load
                                         : load_error;
                post_status("error", fail.error_message);
                return fail;
              }
              LOG(INFO) << "[MoltAI] " << model_to_load
                        << " loaded successfully";
              post_status("ready", model_to_load);
            } else if (must_load && resident_model.empty() &&
                       !settings.auto_load_model) {
              molt_ai::GenerationResult fail;
              fail.error_message =
                  "No model loaded. Open the Models panel to load a model.";
              return fail;
            }

            // A Stop pressed during the (possibly minute-long) load.
            if (rt->IsRequestCancelled(request_id)) return cancelled_result();

            molt_ai::PromptOptions opts;
            opts.max_tokens = settings.max_tokens;  // 0 = adaptive
            opts.max_reasoning_tokens = settings.max_reasoning_tokens;
            opts.reasoning = settings.reasoning ? molt_ai::ReasoningMode::kOn
                                                : molt_ai::ReasoningMode::kOff;
            opts.temperature = settings.temperature;
            opts.top_p = settings.top_p;
            opts.top_k = settings.top_k;
            opts.stream = true;
            opts.request_id = request_id;
            opts.conversation_key = conversation_key;

            // Structured request: the runtime applies the model's own chat
            // template and fits the window — whole oldest turns first, then
            // the page context; the system prompt is never cut.
            molt_ai::ChatPrompt chat;
            chat.system_prompt = BuildSystemPrompt(settings, include_actions,
                                                   /*cloud_provider_name=*/"");
            chat.context_preamble = kContextPreamble;
            chat.context = page_ctx;
            for (auto& [role, content] : ParseHistory(settings, history)) {
              chat.history.push_back({std::move(role), std::move(content)});
            }
            chat.user_message = prompt;

            LOG(INFO) << "[MoltAI] Starting inference ("
                      << chat.history.size() + 2 << " messages)...";

            // Stream visible tokens (reasoning / Harmony analysis already
            // filtered out by the runtime) to the UI thread.
            molt_ai::GenerationResult result = rt->StreamChat(
                chat,
                [weak_self](const std::string& token, bool is_done) {
                  content::GetUIThreadTaskRunner({})->PostTask(
                      FROM_HERE,
                      base::BindOnce(
                          [](base::WeakPtr<MoltAIChatHandler> self,
                             std::string tok, bool done) {
                            if (self && self->IsJavascriptAllowed()) {
                              self->FireWebUIListener(
                                  "ai-token", base::Value(tok),
                                  base::Value(done));
                            }
                          },
                          weak_self, token, is_done));
                },
                opts);

            LOG(INFO) << "[MoltAI] Inference complete, generated "
                      << result.text.size() << " chars"
                      << (result.was_cancelled ? " (stopped)" : "");
            // A stopped reply keeps what was streamed; it isn't an error.
            result.success =
                result.error_message.empty() && !result.text.empty();
            return result;
          },
          base::Unretained(runtime), prompt_text, history_text,
          page_context, lazy_model, include_actions, request_id,
          conversation_key, weak_ptr_factory_.GetWeakPtr()),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             uint64_t request_id, molt_ai::GenerationResult result) {
            if (!self) return;
            if (self->active_request_id_ == request_id) {
              self->active_request_id_ = 0;
            }
            if (result.success) {
              self->model_loaded_ = true;
            }
            self->OnPromptComplete(std::move(cb_id), result.success,
                                   result.text, result.error_message);
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id, request_id));
}

void MoltAIChatHandler::OnPromptComplete(std::string callback_id,
                                         bool success,
                                         const std::string& full_text,
                                         const std::string& error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  LOG(INFO) << "[MoltAI] OnPromptComplete: success=" << success
            << " text_len=" << full_text.size()
            << " error=" << error;
  if (!IsJavascriptAllowed()) return;

  base::DictValue result;
  result.Set("success", success);
  result.Set("text", full_text);
  result.Set("error", error);

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
  active_prompt_callback_id_.clear();
}

// ------------------------------------------------------------------
// Cloud (frontier API) send path.
// ------------------------------------------------------------------
scoped_refptr<network::SharedURLLoaderFactory>
MoltAIChatHandler::GetUrlLoaderFactory() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!profile_) {
    return nullptr;
  }
  return profile_->GetDefaultStoragePartition()
      ->GetURLLoaderFactoryForBrowserProcess();
}

void MoltAIChatHandler::StartCloudPrompt(std::string callback_id,
                                         std::string model_id,
                                         std::string prompt_text,
                                         std::string history_text,
                                         std::string page_context,
                                         bool include_actions,
                                         base::DictValue attached) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  active_prompt_callback_id_ = callback_id;
  cloud_accumulated_text_.clear();

  std::string provider_id;
  std::string model;
  molt_ai::SplitCloudModelId(model_id, &provider_id, &model);
  const molt_ai::CloudProviderInfo* provider_info =
      molt_ai::FindCloudProvider(provider_id);
  const std::string provider_name =
      provider_info ? provider_info->display_name : provider_id;
  const base::FilePath consent_path = GetCloudConsentPath(profile_->GetPath());

  // settings.json + keys.enc + the consent record are blocking disk/keychain
  // reads, so the conversation + provider-key assembly happens on a worker.
  // The api_key rides inside the returned CloudChatRequest (in-process only,
  // never logged, never sent to JS). The actual network stream is kicked
  // off back on the UI thread in OnCloudPromptReady.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock(),
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(
          [](std::string provider_id, std::string provider_name,
             std::string model, std::string prompt, std::string history,
             std::string page_ctx, bool include_actions,
             base::FilePath consent_path) -> CloudPromptPrep {
            CloudPromptPrep prep;
            // Nothing is assembled (let alone sent) before the user has
            // agreed to share with this provider (P18).
            prep.consented = HasCloudConsent(consent_path, provider_id);
            if (!prep.consented) return prep;
            MoltAISettings settings = LoadUserSettings();
            std::optional<molt_ai::ProviderConfig> cfg =
                molt_ai::MoltKeysStore::Get(provider_id);
            molt_ai::CloudChatRequest& req = prep.request;
            req.provider_id = provider_id;
            req.model = model;
            // 0 means "adaptive" for the local runtime; cloud APIs need a
            // concrete cap.
            req.max_tokens =
                settings.max_tokens > 0 ? settings.max_tokens : 1024;
            req.temperature = settings.temperature;
            prep.cfg_found = cfg && !cfg->api_key.empty();
            if (prep.cfg_found) {
              req.api_key = cfg->api_key;
              req.base_url = cfg->base_url;
            }
            for (auto& msg : BuildCloudChatMessages(
                     settings, provider_name, prompt, history, page_ctx,
                     include_actions)) {
              req.messages.push_back(
                  {std::move(msg.first), std::move(msg.second)});
            }
            return prep;
          },
          provider_id, provider_name, model, std::move(prompt_text),
          std::move(history_text), std::move(page_context), include_actions,
          consent_path),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb,
             std::string pid, std::string mdl, base::DictValue attached,
             CloudPromptPrep prep) {
            if (!self) {
              return;
            }
            self->OnCloudPromptReady(std::move(cb), std::move(pid),
                                     std::move(mdl), std::move(attached),
                                     std::move(prep));
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id, provider_id, model,
          std::move(attached)));
}

void MoltAIChatHandler::OnCloudPromptReady(std::string callback_id,
                                           std::string provider_id,
                                           std::string model,
                                           base::DictValue attached,
                                           CloudPromptPrep prep) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) {
    return;
  }

  const molt_ai::CloudProviderInfo* info =
      molt_ai::FindCloudProvider(provider_id);
  const std::string name = info ? info->display_name : provider_id;

  if (!prep.consented) {
    // First use of this provider: the page asks the user (listing exactly
    // which context kinds get attached) and calls grantCloudConsent before
    // re-sending. Nothing has left the device.
    base::DictValue result;
    result.Set("success", false);
    result.Set("consent_required", true);
    result.Set("provider_id", provider_id);
    result.Set("provider_name", name);
    result.Set("attached", std::move(attached));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    active_prompt_callback_id_.clear();
    return;
  }

  if (!prep.cfg_found) {
    OnPromptComplete(std::move(callback_id), /*success=*/false, std::string(),
                     "No API key configured for " + name +
                         ". Add it in AI Settings → Connect AI Providers.");
    return;
  }
  molt_ai::CloudChatRequest request = std::move(prep.request);

  scoped_refptr<network::SharedURLLoaderFactory> factory =
      GetUrlLoaderFactory();
  if (!factory) {
    OnPromptComplete(std::move(callback_id), /*success=*/false, std::string(),
                     "No network context available");
    return;
  }

  // Tell the page this reply comes from a cloud provider so the bubble is
  // labeled "Sent to <provider>" (P18).
  FireWebUIListener("ai-cloud-reply", base::Value(name), base::Value(model));

  cloud_accumulated_text_.clear();
  const base::WeakPtr<MoltAIChatHandler> weak =
      weak_ptr_factory_.GetWeakPtr();

  // request (incl. api_key) is consumed synchronously inside StartCloudChat
  // — it builds the HTTP request before returning — so passing the local by
  // const ref is safe; nothing retains it past this call.
  active_cloud_stream_ = molt_ai::StartCloudChat(
      std::move(factory), request,
      base::BindRepeating(
          [](base::WeakPtr<MoltAIChatHandler> self, const std::string& delta) {
            if (!self || !self->IsJavascriptAllowed()) {
              return;
            }
            self->cloud_accumulated_text_ += delta;
            // Mirror the local streaming shape: each delta is a non-final
            // ai-token; is_done=true is fired once from on_done below.
            self->FireWebUIListener("ai-token", base::Value(delta),
                                    base::Value(false));
          },
          weak),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb, bool ok,
             const std::string& error) {
            if (!self) {
              return;
            }
            if (self->IsJavascriptAllowed()) {
              self->FireWebUIListener("ai-token", base::Value(std::string()),
                                      base::Value(true));
            }
            const std::string text = self->cloud_accumulated_text_;
            self->cloud_accumulated_text_.clear();
            self->active_cloud_stream_.reset();
            self->OnPromptComplete(std::move(cb), ok && !text.empty(), text,
                                   ok ? std::string() : error);
          },
          weak, callback_id));
}

// ------------------------------------------------------------------
// HandleCancelGeneration: Cancel in-progress generation
// JS: chrome.send('cancelGeneration', [])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleCancelGeneration(
    const base::ListValue& args) {
  AllowJavascript();
  // User pressed Stop — cancel THIS page's on-device request only (A6).
  // Other pages and background Agent runs share the runtime and keep going;
  // a Stop that lands while the model is still loading or while the request
  // waits behind another generation is remembered by the runtime.
  if (active_request_id_) {
    GetOrCreateRuntime()->CancelRequest(active_request_id_);
  }
  // Drop any in-flight cloud stream — destroying the CloudChatStream
  // cancels the underlying loader and stops all further token/done
  // callbacks (its contract). Because an explicit Cancel()/destruction
  // deliberately does NOT run the stream's on_done, we must settle the
  // pending JS sendPrompt promise ourselves here — otherwise it stays
  // unresolved and the composer locks (setGenerating(false) only runs in
  // the promise's .then/.catch). This mirrors the local path, where cancel
  // still lets the worker reply fire OnPromptComplete. Any text streamed so
  // far is kept, exactly like a cancelled local generation.
  if (active_cloud_stream_) {
    active_cloud_stream_.reset();
    std::string cb = active_prompt_callback_id_;
    std::string partial = std::move(cloud_accumulated_text_);
    cloud_accumulated_text_.clear();
    if (!cb.empty()) {
      OnPromptComplete(std::move(cb), /*success=*/!partial.empty(), partial,
                       std::string());
    }
  }
}

// ------------------------------------------------------------------
// HandleGrantCloudConsent: record that the user agreed to send chat
// context to a cloud provider (after the first-use dialog listed what
// gets attached). JS: chrome.send('grantCloudConsent',
//                                 [callback_id, provider_id])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGrantCloudConsent(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string provider_id = StringArg(args, 1);
  if (!molt_ai::FindCloudProvider(provider_id) || !profile_) {
    base::DictValue result;
    result.Set("success", false);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::TaskPriority::USER_BLOCKING, base::MayBlock()},
      base::BindOnce(&GrantCloudConsentOnWorker,
                     GetCloudConsentPath(profile_->GetPath()), provider_id),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb,
             bool ok) {
            if (!self || !self->IsJavascriptAllowed()) return;
            base::DictValue result;
            result.Set("success", ok);
            self->ResolveJavascriptCallback(base::Value(cb),
                                            base::Value(std::move(result)));
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id));
}

// ------------------------------------------------------------------
// HandleGetModelStatus: Get current model loading state
// JS: chrome.send('getModelStatus', [callback_id])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGetModelStatus(
    const base::ListValue& args) {
  AllowJavascript();

  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  auto* runtime = GetOrCreateRuntime();
  // Re-scan disk so previously-downloaded GGUF files are detected after
  // every browser restart. Without this, models appear as not-downloaded
  // even though the file is still in ~/.moltbrowser/models/.
  runtime->RefreshModelStatus();
  auto models = runtime->GetAvailableModels();

  base::ListValue model_list;
  std::string loaded_model;
  for (const auto& m : models) {
    base::DictValue d;
    d.Set("model_id", m.model_id);
    d.Set("display_name", m.display_name);
    d.Set("company", m.company);
    d.Set("quantization", m.quantization);
    d.Set("param_billions", m.parameter_count_billions);
    d.Set("is_downloaded", m.is_downloaded);
    d.Set("is_loaded", m.is_loaded);
    // Explicit so the JS picker's is_cloud checks are unambiguous.
    d.Set("is_cloud", false);
    d.Set("file_size_mb",
          static_cast<int>(m.file_size_bytes / (1024 * 1024)));
    d.Set("verified", m.is_verified());
    d.Set("license_name", m.license_name);
    d.Set("license_url", m.license_url);
    model_list.Append(std::move(d));
    if (m.is_loaded) {
      loaded_model = m.display_name;
    }
  }

  // Append connected cloud (frontier API) models — one row per model the
  // user enabled for each configured provider. These are ready-by-
  // selection (nothing to download or load), so is_downloaded/is_loaded
  // stay false; the picker renders them in its "Cloud · via your key"
  // group. Reading keys.enc is a tiny OSCrypt-decrypt on the UI thread,
  // same idiom as the vault handlers.
  {
    ScopedAllowBlockingForMolt allow;
    for (const auto& [provider_id, cfg] : molt_ai::MoltKeysStore::LoadAll()) {
      const molt_ai::CloudProviderInfo* info =
          molt_ai::FindCloudProvider(provider_id);
      for (const std::string& cloud_model : cfg.enabled_models) {
        base::DictValue d;
        d.Set("model_id", provider_id + ":" + cloud_model);
        d.Set("display_name", cloud_model);
        d.Set("provider", info ? info->display_name : provider_id);
        d.Set("is_cloud", true);
        d.Set("is_downloaded", false);
        d.Set("is_loaded", false);
        model_list.Append(std::move(d));
      }
    }
  }

  base::DictValue result;
  result.Set("models", std::move(model_list));
  result.Set("model_loaded", model_loaded_);
  result.Set("loaded_model_name", loaded_model);

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

void MoltAIChatHandler::HandleGetPageContext(const base::ListValue& args) {
  AllowJavascript();

  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  base::DictValue result;

  // The active tab's current committed document. nav_entry_id lets the chat
  // check that page text it holds (pushed by the side panel) is for this
  // document and not the previous one (A7).
  if (content::WebContents* active_tab = GetActiveTabForChat()) {
    result.Set("url", active_tab->GetLastCommittedURL().spec());
    result.Set("title", base::UTF16ToUTF8(active_tab->GetTitle()));
    result.Set("nav_entry_id", CommittedEntryId(active_tab));
    result.Set("has_context", true);
  } else {
    result.Set("has_context", false);
  }

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

content::WebContents* MoltAIChatHandler::GetActiveTabForChat() {
  content::WebContents* webui_contents = web_ui()->GetWebContents();
  GlobalBrowserCollection* browsers = GlobalBrowserCollection::GetInstance();
  BrowserWindowInterface* browser =
      browsers->FindBrowserWithTab(webui_contents);
  if (!browser) {
    // Side panel: not a tab, but it lives in its browser's window. Not
    // GetLastActiveBrowser(): another window's tab would be the wrong page.
    browser = browsers->FindBrowserWithWindow(
        webui_contents->GetTopLevelNativeWindow());
  }
  if (!browser || !browser->GetTabStripModel()) {
    return nullptr;
  }
  content::WebContents* active_tab =
      browser->GetTabStripModel()->GetActiveWebContents();
  return active_tab == webui_contents ? nullptr : active_tab;
}

// ------------------------------------------------------------------
// Model downloads (S13). Files come from a pinned Hugging Face revision
// and are SHA-256-verified while streaming (molt_ai::models::
// ModelDownloadJob); registry entries that aren't pinned yet download
// from "main" and are reported as unverified. Llama / Gemma terms must be
// accepted (once per license, recorded per model) before the download.
// ------------------------------------------------------------------
namespace {

// <models dir>/accepted_licenses.json = {"<model_id>": "<license_id>"}.
// Model files are shared by every profile, so is their acceptance.
base::FilePath AcceptedLicensesPath(const std::string& model_file_path) {
  return base::FilePath::FromUTF8Unsafe(model_file_path)
      .DirName()
      .AppendASCII("accepted_licenses.json");
}

bool IsLicenseAccepted(const base::FilePath& path,
                       const std::string& model_id,
                       const std::string& license_id) {
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) return false;
  auto parsed = base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) return false;
  const std::string* accepted = parsed->GetDict().FindString(model_id);
  return accepted && *accepted == license_id;
}

bool RecordLicenseAcceptance(const base::FilePath& path,
                             const std::string& model_id,
                             const std::string& license_id) {
  base::DictValue all;
  std::string contents;
  if (base::ReadFileToString(path, &contents)) {
    auto parsed = base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
    if (parsed && parsed->is_dict()) all = std::move(parsed->GetDict());
  }
  all.Set(model_id, license_id);
  std::string serialized;
  if (!base::JSONWriter::Write(all, &serialized)) return false;
  if (!base::CreateDirectory(path.DirName())) return false;
  const base::FilePath tmp = path.AddExtension(FILE_PATH_LITERAL("tmp"));
  if (!base::WriteFile(tmp, serialized) ||
      !base::ReplaceFile(tmp, path, nullptr)) {
    base::DeleteFile(tmp);
    return false;
  }
  return true;
}

}  // namespace

// ------------------------------------------------------------------
// HandleDownloadModel: Download a model from HuggingFace
// JS: chrome.send('downloadModel', [callback_id, model_id])
// Streams progress via FireWebUIListener('download-progress', ...).
// Resolves {success, verified?, error?} or, when the model's terms haven't
// been accepted yet, {success:false, license_required:true, license_id,
// license_name, license_url, error}.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleDownloadModel(const base::ListValue& args) {
  AllowJavascript();

  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string model_id = StringArg(args, 1);

  auto* runtime = GetOrCreateRuntime();
  auto info = runtime->GetModelInfo(model_id);

  if (info.model_id.empty()) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Unknown model: " + model_id);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  if (info.is_downloaded) {
    base::DictValue result;
    result.Set("success", true);
    result.Set("already_downloaded", true);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  if (download_job_) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Another download is already in progress");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  // All filesystem checks (license record, CreateDirectory,
  // AmountOfFreeDiskSpace, partial size) run on a ThreadPool worker. The
  // network wiring continues on the UI thread once the precheck returns.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock()},
      base::BindOnce(
          [](molt_ai::ModelInfo info) -> base::DictValue {
            base::DictValue r;
            if (!info.license_id.empty() &&
                !IsLicenseAccepted(AcceptedLicensesPath(info.file_path),
                                   info.model_id, info.license_id)) {
              r.Set("ok", false);
              r.Set("license_required", true);
              return r;
            }
            base::FilePath final_path =
                base::FilePath::FromUTF8Unsafe(info.file_path);
            base::FilePath parent_dir = final_path.DirName();
            if (!base::DirectoryExists(parent_dir)) {
              base::CreateDirectory(parent_dir);
            }
            const int64_t existing_bytes =
                base::GetFileSize(
                    molt_ai::models::ModelDownloadJob::PartialPathFor(
                        final_path))
                    .value_or(0);
            const int64_t disk_space =
                base::SysInfo::AmountOfFreeDiskSpace(parent_dir)
                    .value_or(-1);
            const int64_t required_bytes =
                std::max<int64_t>(0, static_cast<int64_t>(
                                         info.file_size_bytes) -
                                         existing_bytes);
            if (disk_space >= 0 && disk_space < required_bytes) {
              r.Set("ok", false);
              r.Set("error",
                    "Not enough disk space. Need " +
                        std::to_string(required_bytes / (1024 * 1024)) +
                        " MB but only " +
                        std::to_string(disk_space / (1024 * 1024)) +
                        " MB available.");
              return r;
            }
            r.Set("ok", true);
            return r;
          },
          info),
      base::BindOnce(&MoltAIChatHandler::OnDownloadPrecheckComplete,
                     weak_ptr_factory_.GetWeakPtr(), callback_id, model_id,
                     info));
}

void MoltAIChatHandler::OnDownloadPrecheckComplete(
    std::string callback_id,
    std::string model_id,
    molt_ai::ModelInfo info,
    base::DictValue precheck) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  if (precheck.FindBool("license_required").value_or(false)) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("license_required", true);
    result.Set("model_id", model_id);
    result.Set("license_id", info.license_id);
    result.Set("license_name", info.license_name);
    result.Set("license_url", info.license_url);
    // Shown as-is by pages that don't handle license_required.
    result.Set("error", info.display_name + " is distributed under the " +
                            info.license_name +
                            ". Accept it in the AI chat's model menu to "
                            "download.");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  if (!precheck.FindBool("ok").value_or(false)) {
    base::DictValue result;
    result.Set("success", false);
    if (auto* err = precheck.FindString("error")) {
      result.Set("error", *err);
    } else {
      result.Set("error", "Download precheck failed");
    }
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  scoped_refptr<network::SharedURLLoaderFactory> factory =
      GetUrlLoaderFactory();
  if (!factory) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "No network context available");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  molt_ai::models::ModelDownloadJob::Params params;
  params.url = GURL(molt_ai::models::ModelDownloadUrl(
      info.huggingface_id, info.revision, info.filename));
  params.final_path = base::FilePath::FromUTF8Unsafe(info.file_path);
  params.expected_sha256 = info.sha256;
  // Sizes are only exact once an entry is pinned.
  params.expected_size =
      info.is_verified() ? static_cast<int64_t>(info.file_size_bytes) : 0;
  if (!info.is_verified()) {
    LOG(WARNING) << "[MoltAI] downloading " << model_id
                 << " UNVERIFIED: registry entry has no pinned revision/"
                    "SHA-256 (run tools/models/pin_models.py)";
  }
  LOG(INFO) << "[MoltAI] Starting download: " << params.url.spec() << " -> "
            << info.file_path;

  // Store state for callbacks
  download_callback_id_ = callback_id;
  downloading_model_id_ = model_id;
  download_total_bytes_ = info.file_size_bytes;
  download_start_time_ = base::TimeTicks::Now();
  download_last_time_ = download_start_time_;
  download_last_bytes_ = 0;

  download_job_ = molt_ai::models::ModelDownloadJob::Start(
      std::move(params), std::move(factory),
      base::BindRepeating(&MoltAIChatHandler::OnDownloadProgress,
                          weak_ptr_factory_.GetWeakPtr()),
      base::BindOnce(&MoltAIChatHandler::OnDownloadFinished,
                     weak_ptr_factory_.GetWeakPtr()));
}

void MoltAIChatHandler::OnDownloadProgress(int64_t received, int64_t total) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  const uint64_t current = static_cast<uint64_t>(std::max<int64_t>(0, received));
  if (total > 0) download_total_bytes_ = static_cast<uint64_t>(total);

  // The job reports after every written chunk; forward at most two updates
  // a second (plus the first one) and compute speed/ETA over that window.
  const base::TimeTicks now = base::TimeTicks::Now();
  double speed_bps = 0;
  double eta_sec = -1;
  if (download_last_bytes_ == 0 || current < download_last_bytes_) {
    // First report (resume offset) or a restart from byte 0: new baseline.
    download_last_bytes_ = current;
    download_last_time_ = now;
  } else {
    const double elapsed_sec = (now - download_last_time_).InSecondsF();
    if (elapsed_sec < 0.5) return;
    const uint64_t bytes_delta = current - download_last_bytes_;
    speed_bps = static_cast<double>(bytes_delta) / elapsed_sec;
    download_last_bytes_ = current;
    download_last_time_ = now;
    if (speed_bps > 0 && download_total_bytes_ > current) {
      eta_sec = static_cast<double>(download_total_bytes_ - current) /
                speed_bps;
    }
  }

  FireWebUIListener("download-progress",
                    base::Value(downloading_model_id_),
                    base::Value(static_cast<double>(current)),
                    base::Value(static_cast<double>(download_total_bytes_)),
                    base::Value(speed_bps),
                    base::Value(eta_sec));
}

void MoltAIChatHandler::OnDownloadFinished(
    molt_ai::models::ModelDownloadJob::Result result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  LOG(INFO) << "[MoltAI] Download finished: success=" << result.success
            << " verified=" << result.verified << " sha256=" << result.sha256
            << " error=" << result.error;
  if (!result.success) {
    FinishDownload(false, result.verified, result.error);
    return;
  }
  // The job already verified and renamed the file; refresh the registry's
  // on-disk view on a worker.
  auto* runtime = GetOrCreateRuntime();
  base::ThreadPool::PostTaskAndReply(
      FROM_HERE, {base::TaskPriority::USER_BLOCKING, base::MayBlock()},
      base::BindOnce(&molt_ai::BrowserAIRuntime::RefreshModelStatus,
                     base::Unretained(runtime)),
      base::BindOnce(&MoltAIChatHandler::FinishDownload,
                     weak_ptr_factory_.GetWeakPtr(), true, result.verified,
                     std::string()));
}

void MoltAIChatHandler::FinishDownload(bool success,
                                       bool verified,
                                       std::string error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (IsJavascriptAllowed()) {
    FireWebUIListener("download-complete",
                      base::Value(downloading_model_id_),
                      base::Value(success));

    base::DictValue result;
    result.Set("success", success);
    result.Set("model_id", downloading_model_id_);
    result.Set("verified", verified);
    if (!success) {
      result.Set("error", error.empty() ? std::string("Download failed")
                                        : error);
    }
    ResolveJavascriptCallback(base::Value(download_callback_id_),
                              base::Value(std::move(result)));
  }

  download_job_.reset();
  download_callback_id_.clear();
  downloading_model_id_.clear();
  download_total_bytes_ = 0;
  download_last_bytes_ = 0;
}

// ------------------------------------------------------------------
// HandleAcceptModelLicense: record that the user accepted |license_id|
// for |model_id| (shown with a link to the terms before downloading).
// JS: chrome.send('acceptModelLicense', [callback_id, model_id, license_id])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleAcceptModelLicense(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 3u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string model_id = StringArg(args, 1);
  const std::string license_id = StringArg(args, 2);

  molt_ai::ModelInfo info = GetOrCreateRuntime()->GetModelInfo(model_id);
  if (info.model_id.empty() || info.license_id.empty() ||
      info.license_id != license_id) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Unknown model or license");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::TaskPriority::USER_BLOCKING, base::MayBlock()},
      base::BindOnce(&RecordLicenseAcceptance,
                     AcceptedLicensesPath(info.file_path), model_id,
                     license_id),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb, bool ok) {
            if (!self || !self->IsJavascriptAllowed()) return;
            base::DictValue result;
            result.Set("success", ok);
            self->ResolveJavascriptCallback(base::Value(cb),
                                            base::Value(std::move(result)));
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id));
}

// ------------------------------------------------------------------
// HandleDeleteModel: Delete a downloaded model file
// JS: chrome.send('deleteModel', [callback_id, model_id])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleDeleteModel(const base::ListValue& args) {
  AllowJavascript();

  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string model_id =
      args[1].is_string() ? args[1].GetString() : std::string();

  auto* runtime = GetOrCreateRuntime();

  // DeleteModel touches the filesystem (std::filesystem::remove + GGUF unload).
  // Run on a worker, reply on UI thread.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock()},
      base::BindOnce(
          [](molt_ai::BrowserAIRuntime* rt, std::string mid) {
            return rt->DeleteModel(mid);
          },
          base::Unretained(runtime), model_id),
      base::BindOnce(&MoltAIChatHandler::OnModelDeleted,
                     weak_ptr_factory_.GetWeakPtr(), callback_id, model_id));
}

void MoltAIChatHandler::OnModelDeleted(std::string callback_id,
                                       std::string model_id,
                                       bool success) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  if (success) {
    model_loaded_ = false;
  }

  base::DictValue result;
  result.Set("success", success);
  result.Set("model_id", model_id);

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

// ------------------------------------------------------------------
// HandleGetPageContent: Extract text content from the active tab
// JS: chrome.send('getPageContent', [callback_id, max_chars?])
// Uses JS injection to get document.body.innerText. max_chars defaults to
// 4000 (the quick actions); the reader slash commands ask for more. Clamped
// to [1, kMaxPageContentChars], the side panel capture's cap.
// ------------------------------------------------------------------
namespace {
constexpr int kDefaultPageContentChars = 4000;
constexpr int kMaxPageContentChars = 50000;
}  // namespace

void MoltAIChatHandler::HandleGetPageContent(const base::ListValue& args) {
  AllowJavascript();

  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  // Renderer input: anything but a sane integer means the default.
  int max_chars = kDefaultPageContentChars;
  if (args.size() > 1 && args[1].is_int()) {
    max_chars = std::clamp(args[1].GetInt(), 1, kMaxPageContentChars);
  }

  content::WebContents* active_tab = GetActiveTabForChat();
  content::RenderFrameHost* rfh =
      active_tab ? active_tab->GetPrimaryMainFrame() : nullptr;
  if (!rfh) {
    base::DictValue result;
    result.Set("has_content", false);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  // The document the text will come from. The reply reports these, not
  // whatever the tab shows when it arrives (A7).
  std::string url = active_tab->GetLastCommittedURL().spec();
  std::string title = base::UTF16ToUTF8(active_tab->GetTitle());
  const int nav_entry_id = CommittedEntryId(active_tab);

  // Extract page text (at most max_chars, for the context window) in an
  // isolated world, like the side panel's capture. max_chars is a clamped
  // integer, the only value spliced into the script.
  const std::u16string script =
      u"(function(){"
      u"var text = document.body ? document.body.innerText : '';"
      u"return text.substring(0, " +
      base::NumberToString16(max_chars) + u");})()";
  rfh->ExecuteJavaScriptInIsolatedWorld(
      script,
      base::BindOnce(&MoltAIChatHandler::OnPageContentExtracted,
                     weak_ptr_factory_.GetWeakPtr(), callback_id,
                     std::move(url), std::move(title), nav_entry_id),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

void MoltAIChatHandler::OnPageContentExtracted(std::string callback_id,
                                                std::string url,
                                                std::string title,
                                                int nav_entry_id,
                                                base::Value result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  base::DictValue response;

  // Only answer from this text if the active tab still shows the document
  // it was read from; after a navigation or tab switch it is another page's.
  content::WebContents* active_tab = GetActiveTabForChat();
  const bool same_document =
      active_tab && CommittedEntryId(active_tab) == nav_entry_id &&
      active_tab->GetLastCommittedURL().spec() == url;

  if (same_document && result.is_string() && !result.GetString().empty()) {
    response.Set("url", std::move(url));
    response.Set("title", std::move(title));
    response.Set("nav_entry_id", nav_entry_id);
    response.Set("has_content", true);
    response.Set("content", result.GetString());
  } else {
    response.Set("has_content", false);
  }

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(response)));
}

// ------------------------------------------------------------------
// HandleCancelDownload: Cancel an in-progress model download
// JS: chrome.send('cancelDownload', [])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleCancelDownload(const base::ListValue& args) {
  AllowJavascript();
  if (!download_job_) return;
  LOG(INFO) << "[MoltAI] Cancelling download of " << downloading_model_id_;
  const std::string cancelled_model = downloading_model_id_;
  // Stops the transfer and deletes the .partial file on the job's file
  // sequence (after any write still in flight).
  download_job_->CancelAndDiscard();
  download_job_.reset();

  FireWebUIListener("download-complete", base::Value(cancelled_model),
                    base::Value(false));

  if (!download_callback_id_.empty()) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Download cancelled by user");
    result.Set("model_id", cancelled_model);
    ResolveJavascriptCallback(base::Value(download_callback_id_),
                              base::Value(std::move(result)));
  }

  download_callback_id_.clear();
  downloading_model_id_.clear();
  download_total_bytes_ = 0;
  download_last_bytes_ = 0;
}

// ------------------------------------------------------------------
// HandleExportHistory: Export chat history as JSON
// JS: chrome.send('exportHistory', [callback_id, history_json])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleExportHistory(const base::ListValue& args) {
  AllowJavascript();

  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  std::string history_json =
      args[1].is_string() ? args[1].GetString() : std::string();
  // Cap exported history to keep the on-disk file size bounded.
  // 16 MiB is plenty for a multi-month chat log.
  constexpr size_t kMaxHistoryExportBytes = 16 << 20;
  if (history_json.size() > kMaxHistoryExportBytes) {
    history_json.resize(kMaxHistoryExportBytes);
  }

  // Generate timestamp-based filename on UI thread (no I/O).
  base::Time now = base::Time::Now();
  base::Time::Exploded exploded;
  now.LocalExplode(&exploded);
  std::string filename = "chat-" +
      std::to_string(exploded.year) + "-" +
      (exploded.month < 10 ? "0" : "") + std::to_string(exploded.month) + "-" +
      (exploded.day_of_month < 10 ? "0" : "") +
      std::to_string(exploded.day_of_month) + "-" +
      (exploded.hour < 10 ? "0" : "") + std::to_string(exploded.hour) +
      (exploded.minute < 10 ? "0" : "") + std::to_string(exploded.minute) +
      (exploded.second < 10 ? "0" : "") + std::to_string(exploded.second) +
      ".json";

  base::FilePath home_dir;
  base::PathService::Get(base::DIR_HOME, &home_dir);
  base::FilePath file_path =
      home_dir.Append(base::FilePath::FromUTF8Unsafe(".moltbrowser"))
          .Append(base::FilePath::FromUTF8Unsafe("chat_exports"))
          .Append(base::FilePath::FromUTF8Unsafe(filename));

  // mkdir + WriteFile on a ThreadPool worker; reply with success bool.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock()},
      base::BindOnce(
          [](base::FilePath path, std::string contents) {
            base::FilePath dir = path.DirName();
            if (!base::DirectoryExists(dir)) {
              base::CreateDirectory(dir);
            }
            return base::WriteFile(path, contents);
          },
          file_path, history_json),
      base::BindOnce(&MoltAIChatHandler::OnHistoryExported,
                     weak_ptr_factory_.GetWeakPtr(), callback_id, file_path,
                     filename));
}

void MoltAIChatHandler::OnHistoryExported(std::string callback_id,
                                          base::FilePath file_path,
                                          std::string filename,
                                          bool success) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  base::DictValue result;
  result.Set("success", success);
  result.Set("path", file_path.AsUTF8Unsafe());
  result.Set("filename", filename);

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

// ------------------------------------------------------------------
// Conversation store: list/save/load/delete named conversations (A5).
// Stored per profile and OSCrypt-encrypted by
// molt_ai::memory::ConversationStore under <profile>/MoltAI/Conversations;
// legacy plaintext files from the shared ~/.moltbrowser/conversations are
// migrated (encrypted, then deleted) on first access. Off-the-record
// profiles never read or write the store.
// JS: chrome.send('listConversations',  [callback_id])
//     chrome.send('saveConversation',   [callback_id, id, title,
//                                        history_json])
//     chrome.send('loadConversation',   [callback_id, id])
//     chrome.send('deleteConversation', [callback_id, id])
// ------------------------------------------------------------------
namespace {

// The sidebar Recents list is capped server-side.
constexpr size_t kMaxConversationListEntries = 100;
// Titles are display-only; keep them short on disk. The contract caps
// titles at 80 CHARS; a char can be up to 4 UTF-8 bytes, so allow
// 320 bytes — the JS-derived title (40 chars + ellipsis, up to ~123
// bytes for CJK) must never be truncated here.
constexpr size_t kMaxConversationTitleBytes = 320;

using molt_ai::memory::ConversationStore;

// Runs |op| against the profile's store on the conversation sequence,
// after the one-time legacy migration.
base::DictValue WithConversationStore(
    base::FilePath profile_dir,
    base::OnceCallback<base::DictValue(const ConversationStore&)> op) {
  ConversationStore store(profile_dir);
  store.MigrateLegacy(ConversationStore::LegacyDir());
  return std::move(op).Run(store);
}

base::DictValue OffTheRecordResult(bool is_list) {
  base::DictValue r;
  if (is_list) {
    // Private windows show no saved history.
    r.Set("success", true);
    r.Set("conversations", base::ListValue());
  } else {
    r.Set("success", false);
    r.Set("error", "Conversations aren't saved in private windows.");
    r.Set("off_the_record", true);
  }
  return r;
}

}  // namespace

base::SequencedTaskRunner* MoltAIChatHandler::ConversationTaskRunner() {
  if (!conversation_task_runner_) {
    conversation_task_runner_ = base::ThreadPool::CreateSequencedTaskRunner(
        {base::TaskPriority::USER_BLOCKING, base::MayBlock()});
  }
  return conversation_task_runner_.get();
}

void MoltAIChatHandler::RunConversationOp(
    std::string callback_id,
    bool is_list,
    base::OnceCallback<base::DictValue(const ConversationStore&)> op) {
  if (!profile_ || profile_->IsOffTheRecord()) {
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(OffTheRecordResult(is_list)));
    return;
  }
  // Until os_crypt_async has handed out its Encryptor (early in startup,
  // when the first window's chat panel already asks for Recents) the store
  // can neither decrypt nor encrypt. Wait for it rather than answer "no
  // conversations" or fail an autosave. Queued ops run in order.
  molt_ai::molt_os_crypt::RunWhenReady(base::BindOnce(
      &MoltAIChatHandler::PostConversationOp, weak_ptr_factory_.GetWeakPtr(),
      std::move(callback_id), std::move(op)));
}

void MoltAIChatHandler::PostConversationOp(
    std::string callback_id,
    base::OnceCallback<base::DictValue(const ConversationStore&)> op) {
  ConversationTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&WithConversationStore, profile_->GetPath(),
                     std::move(op)),
      base::BindOnce(&MoltAIChatHandler::ResolveConversationCallback,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback_id)));
}

void MoltAIChatHandler::HandleListConversations(const base::ListValue& args) {
  AllowJavascript();

  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  RunConversationOp(callback_id, /*is_list=*/true,
                    base::BindOnce([](const ConversationStore& store) {
                      return store.List(kMaxConversationListEntries);
                    }));
}

void MoltAIChatHandler::HandleSaveConversation(const base::ListValue& args) {
  AllowJavascript();

  if (!HasCallbackArgs(args, 4u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string id =
      args[1].is_string() ? args[1].GetString() : std::string();
  std::string title =
      args[2].is_string() ? args[2].GetString() : std::string();
  std::string history_json =
      args[3].is_string() ? args[3].GetString() : std::string();

  if (!ConversationStore::IsValidId(id)) {
    base::DictValue error;
    error.Set("success", false);
    error.Set("error", "invalid conversation id");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(error)));
    return;
  }
  // Truncating JSON would only produce an unparseable payload, so
  // oversized input is rejected (also re-checked by the store).
  if (history_json.size() > ConversationStore::kMaxConversationBytes) {
    base::DictValue error;
    error.Set("success", false);
    error.Set("error", "history_json exceeds 16 MiB cap");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(error)));
    return;
  }
  // Cap the title without splitting a multi-byte UTF-8 sequence.
  if (title.size() > kMaxConversationTitleBytes) {
    size_t cut = kMaxConversationTitleBytes;
    while (cut > 0 &&
           (static_cast<unsigned char>(title[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    title.resize(cut);
  }

  RunConversationOp(
      callback_id, /*is_list=*/false,
      base::BindOnce(
          [](std::string id, std::string title, std::string json,
             const ConversationStore& store) {
            return store.Save(id, title, json);
          },
          id, std::move(title), std::move(history_json)));
}

void MoltAIChatHandler::HandleLoadConversation(const base::ListValue& args) {
  AllowJavascript();

  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string id =
      args[1].is_string() ? args[1].GetString() : std::string();

  RunConversationOp(callback_id, /*is_list=*/false,
                    base::BindOnce(
                        [](std::string id, const ConversationStore& store) {
                          return store.Load(id);
                        },
                        id));
}

void MoltAIChatHandler::HandleDeleteConversation(
    const base::ListValue& args) {
  AllowJavascript();

  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string id =
      args[1].is_string() ? args[1].GetString() : std::string();

  RunConversationOp(callback_id, /*is_list=*/false,
                    base::BindOnce(
                        [](std::string id, const ConversationStore& store) {
                          return store.Delete(id);
                        },
                        id));
}

void MoltAIChatHandler::ResolveConversationCallback(std::string callback_id,
                                                    base::DictValue result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

// ------------------------------------------------------------------
// HandleRunMoltAction: Run one automation action against the active
// tab in this WebUI's owning Browser. Used by the side-panel chat to
// let users (or, downstream, the LLM) drive page actions in plain
// language. The chat HTML side parses /click /type /scroll /navigate
// slash-commands and posts them here.
//
// JS: chrome.send('runMoltAction',
//                  [callback_id,
//                   {type:"click", selector:".foo"} |
//                   {type:"type", selector:"#email", value:"a@b.c"} |
//                   {type:"scroll", value:"600"} |
//                   {type:"navigate", value:"https://..."}])
//
// Safety gate (enforced here, not only in the chat JS):
//   - navigate only accepts http(s) URLs, always.
//   - Unless the action dict carries confirmed:true (set by the chat UI
//     after the user tapped Allow, or for a command the user typed),
//     a cross-origin navigate, a click that follows a link to another
//     origin, and typing / selecting into a password, payment-card or
//     one-time-code field are NOT performed; the result is
//     {success:false, needs_confirmation:true, reason} and the UI asks.
//     This holds in the chat's Auto mode too.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleRunMoltAction(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  base::DictValue result;
  if (!args[1].is_dict()) {
    result.Set("success", false);
    result.Set("error", "expected action dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }
  const base::DictValue& action = args[1].GetDict();
  const std::string* type = action.FindString("type");
  if (!type || type->empty()) {
    result.Set("success", false);
    result.Set("error", "missing action type");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }
  const std::string* sel = action.FindString("selector");
  const std::string* value = action.FindString("value");

  // Resolve the browser window that should receive the action.
  // The side-panel's WebContents is NOT a tab in the tab strip, so
  // FindBrowserWithTab returns null for it. Fall back to
  // GetLastActiveBrowser() — the most-recently-focused browser window,
  // which is the one the user is actively looking at.
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(webui_wc);
  if (!browser) {
    browser = GlobalBrowserCollection::GetInstance()->GetLastActiveBrowser();
  }
  if (!browser || !browser->GetTabStripModel()) {
    result.Set("success", false);
    result.Set("error", "no active browser window");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }
  content::WebContents* target =
      browser->GetTabStripModel()->GetActiveWebContents();
  if (!target) {
    result.Set("success", false);
    result.Set("error", "no active tab");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  std::string t = *type;
  const bool confirmed = action.FindBool("confirmed").value_or(false);

  if (t == "navigate") {
    GURL dest(value ? *value : std::string());
    if (!molt_ai::agent_safety::IsAllowedNavigationURL(dest)) {
      result.Set("success", false);
      result.Set("error", "only http:// and https:// pages can be opened");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }
    if (!confirmed &&
        !target->GetPrimaryMainFrame()->GetLastCommittedOrigin()
             .IsSameOriginWith(dest)) {
      result.Set("success", false);
      result.Set("needs_confirmation", true);
      result.Set("reason", "Opens a different site: " +
                               url::Origin::Create(dest).Serialize());
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }
  }

  // P2.3 / P3: select-dropdown, hover, right-click, drag are simple
  // enough to execute via direct JS injection — no need to spin up
  // an AutomationRunner (which assumes WebContentsObserver hooks
  // and selector-ladder retries that buy nothing for fire-and-forget
  // events). We still require an http(s) page so injecting into
  // chrome:// or other privileged URLs is impossible.
  if (t == "select" || t == "hover" || t == "right-click" || t == "drag") {
    if (!sel || sel->empty()) {
      result.Set("success", false);
      result.Set("error", "selector required for " + t);
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }
    if (t == "drag" && (!value || value->empty())) {
      result.Set("success", false);
      result.Set("error", "drag needs a target selector in 'value'");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }
    if (!target->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
      result.Set("success", false);
      result.Set("error", "only http(s) pages support " + t);
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }
    // JS-escape the selector + value via JsStringLiteral (file-scope
    // helper using base::JSONWriter::Write). Replaces a former hand-
    // rolled escaper that handled \", \\, \n, \r but missed control
    // chars and U+2028 / U+2029. Code-review HIGH #4.
    std::string js;
    if (t == "select") {
      std::string v = value ? *value : "";
      js = "(function(){var el=document.querySelector(" +
           JsStringLiteral(*sel) +
           ");if(!el)return false;el.value=" + JsStringLiteral(v) +
           ";el.dispatchEvent(new Event('input',{bubbles:true}));"
           "el.dispatchEvent(new Event('change',{bubbles:true}));"
           "return true;})()";
    } else if (t == "hover") {
      js = "(function(){var el=document.querySelector(" +
           JsStringLiteral(*sel) +
           ");if(!el)return false;"
           "['mouseover','mouseenter','mousemove'].forEach(function(t){"
           "el.dispatchEvent(new MouseEvent(t,{bubbles:true,"
           "cancelable:true,view:window}));});return true;})()";
    } else if (t == "right-click") {
      // Synthesize a contextmenu event with button=2. Most sites'
      // custom right-click menus listen for this; the native macOS/
      // OS menu won't appear, but page-level menus do.
      js = "(function(){var el=document.querySelector(" +
           JsStringLiteral(*sel) +
           ");if(!el)return false;"
           "var r=el.getBoundingClientRect();"
           "el.dispatchEvent(new MouseEvent('contextmenu',{bubbles:true,"
           "cancelable:true,view:window,button:2,buttons:2,"
           "clientX:r.left+r.width/2,clientY:r.top+r.height/2}));"
           "return true;})()";
    } else {  // drag
      // Source = selector, target = value. Synthesize the full
      // HTML5 drag-drop event sequence with a shared DataTransfer.
      // Many SPAs (Trello-style, Notion-style) listen for these
      // events directly so this is enough to drive them.
      std::string tgt = value ? *value : "";
      js = "(function(){"
           "var src=document.querySelector(" + JsStringLiteral(*sel) + ");"
           "var tgt=document.querySelector(" + JsStringLiteral(tgt) + ");"
           "if(!src||!tgt)return false;"
           "var dt=new DataTransfer();"
           "var sr=src.getBoundingClientRect();var tr=tgt.getBoundingClientRect();"
           "function ev(el,type,box){return new DragEvent(type,{bubbles:true,"
           "cancelable:true,view:window,dataTransfer:dt,"
           "clientX:box.left+box.width/2,clientY:box.top+box.height/2});}"
           "src.dispatchEvent(ev(src,'dragstart',sr));"
           "tgt.dispatchEvent(ev(tgt,'dragenter',tr));"
           "tgt.dispatchEvent(ev(tgt,'dragover',tr));"
           "tgt.dispatchEvent(ev(tgt,'drop',tr));"
           "src.dispatchEvent(ev(src,'dragend',sr));"
           "return true;})()";
    }
    if (t == "select" && !confirmed) {
      // A <select> can be a card-expiry field (autocomplete cc-exp-*):
      // probe it first and ask before changing a sensitive one.
      target->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
          base::UTF8ToUTF16(
              molt_ai::agent_safety::BuildElementProbeScript({*sel})),
          base::BindOnce(
              [](base::WeakPtr<MoltAIChatHandler> self,
                 base::WeakPtr<content::WebContents> wc, std::string cb_id,
                 std::string type_str, std::string js, base::Value v) {
                if (!self) return;
                molt_ai::agent_safety::ElementProbe probe =
                    molt_ai::agent_safety::ParseElementProbes(v, 1)[0];
                if (molt_ai::agent_safety::IsSensitive(probe.field_kind)) {
                  base::DictValue out;
                  out.Set("success", false);
                  out.Set("needs_confirmation", true);
                  out.Set("reason",
                          "Changes a " +
                              std::string(molt_ai::agent_safety::FieldKindLabel(
                                  probe.field_kind)) +
                              " field");
                  self->ResolveJavascriptCallback(base::Value(cb_id),
                                                  base::Value(std::move(out)));
                  return;
                }
                self->RunDirectPageAction(cb_id, type_str, js, wc.get());
              },
              weak_ptr_factory_.GetWeakPtr(), target->GetWeakPtr(),
              callback_id, t, js),
          /*world_id=*/1);
      return;
    }
    RunDirectPageAction(callback_id, t, js, target);
    return;
  }

  // Build a single-step Script. We map the slash commands to the
  // existing AutomationRunner step types so we get all the
  // selector-recovery, retry, and snapshot-on-failure plumbing for
  // free.
  molt_ai::automation::Script s;
  s.id = "ad-hoc-chat-action";
  s.name = "Chat action";
  s.security.trust = molt_ai::automation::TrustLevel::TRUSTED;
  s.security.max_runtime_seconds = 30;

  molt_ai::automation::Step step;
  if (t == "click") {
    step.type = molt_ai::automation::StepType::CLICK;
    step.target = sel ? *sel : "";
  } else if (t == "type") {
    step.type = molt_ai::automation::StepType::TYPE;
    step.target = sel ? *sel : "";
    step.value = value ? *value : "";
  } else if (t == "scroll") {
    step.type = molt_ai::automation::StepType::SCROLL;
    step.value = value ? *value : "600";
  } else if (t == "navigate") {
    step.type = molt_ai::automation::StepType::NAVIGATE;
    step.target = value ? *value : "";
  } else if (t == "wait") {
    // P3: simple sleep. value is ms (default 1000).
    step.type = molt_ai::automation::StepType::WAIT;
    int ms = 1000;
    if (value && !value->empty())
      base::StringToInt(*value, &ms);
    step.timeout_ms = ms;
  } else if (t == "wait-for") {
    // P3: poll until a selector appears, up to value ms (default 5000).
    step.type = molt_ai::automation::StepType::WAIT_FOR;
    step.target = sel ? *sel : "";
    int ms = 5000;
    if (value && !value->empty())
      base::StringToInt(*value, &ms);
    step.timeout_ms = ms;
  } else {
    result.Set("success", false);
    result.Set("error", "unknown action type: " + t);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }
  step.timeout_ms = 8000;
  if (!confirmed && (t == "click" || t == "type")) {
    // The runner probes the element right before acting and stops with a
    // needs_confirmation message on a cross-origin link or a password /
    // card / one-time-code field (it re-checks after selector retries, so
    // a field that appears late is still caught).
    base::DictValue extra;
    extra.Set(molt_ai::automation::kAskBeforeSensitiveKey, true);
    step.extra = base::Value(std::move(extra));
  }
  s.steps.push_back(std::move(step));

  // Run via a transient AutomationRunner. Storage is null — we don't
  // persist this script's stats since it's an ad-hoc chat action.
  // The runner is captured by a shared_ptr in the completion callback
  // so it stays alive across async step execution.
  auto runner = std::make_shared<molt_ai::automation::AutomationRunner>(
      target, /*ai_runtime=*/nullptr, /*storage=*/nullptr);
  std::string callback_id_copy = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  runner->Run(std::move(s),
              base::DoNothing(),
              base::BindOnce(
                  [](base::WeakPtr<MoltAIChatHandler> self,
                     std::string cb_id,
                     std::shared_ptr<molt_ai::automation::AutomationRunner>
                         keep_alive,
                     molt_ai::automation::RunResult r) {
                    if (!self) return;
                    base::DictValue out;
                    std::string_view prefix(
                        molt_ai::automation::kNeedsConfirmationPrefix);
                    if (!r.success && r.message.starts_with(prefix)) {
                      out.Set("success", false);
                      out.Set("needs_confirmation", true);
                      out.Set("reason", r.message.substr(prefix.size()));
                      self->ResolveJavascriptCallback(
                          base::Value(cb_id), base::Value(std::move(out)));
                      return;
                    }
                    out.Set("success", r.success);
                    out.Set("message", r.message);
                    out.Set("duration_ms", r.duration_ms);
                    self->ResolveJavascriptCallback(
                        base::Value(cb_id),
                        base::Value(std::move(out)));
                  },
                  weak_this, callback_id_copy, runner));
}

void MoltAIChatHandler::RunDirectPageAction(const std::string& callback_id,
                                            const std::string& type,
                                            const std::string& js,
                                            content::WebContents* target) {
  if (!target || !target->GetPrimaryMainFrame() ||
      !target->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    base::DictValue out;
    out.Set("success", false);
    out.Set("error", "the page is no longer available");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  target->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(js),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             std::string type_str, base::Value v) {
            if (!self) return;
            bool ok = v.is_bool() && v.GetBool();
            base::DictValue out;
            out.Set("success", ok);
            out.Set("message", ok ? type_str + " ok" :
                                     type_str + ": selector not found");
            self->ResolveJavascriptCallback(base::Value(cb_id),
                                            base::Value(std::move(out)));
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id, type),
      /*world_id=*/1);
}

// ------------------------------------------------------------------
// HandleQueryMemory: Personal Vector Memory grounding for the chat.
// Args: [callback_id, query_text, top_k_int]
// Returns: {hits: [{url, title, snippet, score, visited_at}, ...]}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleQueryMemory(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string query =
      args[1].is_string() ? args[1].GetString() : "";
  int top_k = 3;
  if (args.size() > 2 && args[2].is_int())
    top_k = std::max(1, args[2].GetInt());

  base::DictValue empty;
  empty.Set("hits", base::ListValue());
  if (query.empty()) {
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(empty)));
    return;
  }

  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  if (!profile) {
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(empty)));
    return;
  }
  molt_ai::memory::MemoryService* svc =
      molt_ai::memory::MemoryServiceFactory::GetForProfile(profile);
  // Memory is opt-in: never ground the AI in browsing memory the user has
  // not turned on (pages from before the opt-in change stay on disk until
  // cleared, but are not fed to the model).
  if (!svc || !svc->IsEnabled()) {
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(empty)));
    return;
  }

  std::string cb_id = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  svc->Query(query, top_k, base::BindOnce(
      [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
         std::vector<molt_ai::memory::QueryHit> hits) {
        if (!self) return;
        base::ListValue arr;
        for (const auto& h : hits) {
          base::DictValue d;
          d.Set("url", h.url);
          d.Set("title", h.title);
          d.Set("snippet", h.snippet);
          d.Set("score", h.score);
          d.Set("visited_at", static_cast<double>(h.visited_at_unix));
          arr.Append(std::move(d));
        }
        base::DictValue r;
        r.Set("hits", std::move(arr));
        self->ResolveJavascriptCallback(base::Value(cb_id),
                                        base::Value(std::move(r)));
      },
      weak_this, cb_id));
}

// ------------------------------------------------------------------
// HandleListTabsInWindow: Tab Triage feature.
// Enumerates every tab in the Browser that owns this side-panel WebUI,
// returning {id, url, title, snippet, pinned, active, last_active_unix}.
// The chat then asks the LLM (or shows a manual list) which tabs to
// close / bookmark / pin.
// Args: [callback_id]
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleListTabsInWindow(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  base::DictValue out;
  base::ListValue tabs;

  content::WebContents* webui_wc = web_ui()->GetWebContents();
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(webui_wc);
  if (!browser || !browser->GetTabStripModel()) {
    out.Set("tabs", std::move(tabs));
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  TabStripModel* model = browser->GetTabStripModel();
  const int active = model->active_index();
  const int n = model->count();
  for (int i = 0; i < n; ++i) {
    content::WebContents* wc = model->GetWebContentsAt(i);
    if (!wc) continue;
    base::DictValue d;
    d.Set("index", i);
    d.Set("url", wc->GetLastCommittedURL().spec());
    d.Set("title", base::UTF16ToUTF8(wc->GetTitle()));
    d.Set("pinned", model->IsTabPinned(i));
    d.Set("active", i == active);
    // Cheap snippet: last commit URL's host + path tail. Anything richer
    // would require a content-extraction JS round-trip across N tabs;
    // the side panel can fall back to that on demand.
    std::string host = std::string(wc->GetLastCommittedURL().host());
    std::string path = std::string(wc->GetLastCommittedURL().path());
    if (path.size() > 40) path = path.substr(0, 40) + "…";
    d.Set("snippet", host + path);
    d.Set("last_active_unix",
          static_cast<double>(wc->GetLastActiveTime().ToTimeT()));
    tabs.Append(std::move(d));
  }
  out.Set("tabs", std::move(tabs));
  out.Set("active_index", active);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleTriageActOnTabs: Tab Triage bulk action.
// Args: [callback_id, {action:"close"|"bookmark"|"pin",
//                       indices:[int, int, ...]}]
// Returns: {success, affected_count, error?}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleTriageActOnTabs(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  base::DictValue out;
  if (!args[1].is_dict()) {
    out.Set("success", false);
    out.Set("error", "expected action dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  const base::DictValue& dict = args[1].GetDict();
  const std::string* action = dict.FindString("action");
  const base::ListValue* indices = dict.FindList("indices");
  if (!action || action->empty() || !indices) {
    out.Set("success", false);
    out.Set("error", "missing action or indices");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  content::WebContents* webui_wc = web_ui()->GetWebContents();
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(webui_wc);
  if (!browser || !browser->GetTabStripModel()) {
    out.Set("success", false);
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  TabStripModel* model = browser->GetTabStripModel();

  // Collect & sort indices descending so closing doesn't shift the
  // remaining indices out from under us.
  std::vector<int> idxs;
  for (const base::Value& v : *indices) {
    if (v.is_int()) idxs.push_back(v.GetInt());
  }
  std::sort(idxs.begin(), idxs.end(), std::greater<int>());

  int affected = 0;
  if (*action == "close") {
    for (int i : idxs) {
      if (i < 0 || i >= model->count()) continue;
      model->CloseWebContentsAt(i, TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB |
                                       TabCloseTypes::CLOSE_USER_GESTURE);
      ++affected;
    }
  } else if (*action == "pin") {
    for (int i : idxs) {
      if (i < 0 || i >= model->count()) continue;
      if (!model->IsTabPinned(i)) {
        model->SetTabPinned(i, /*pinned=*/true);
        ++affected;
      }
    }
  } else if (*action == "bookmark") {
    Profile* profile = Profile::FromBrowserContext(
        webui_wc->GetBrowserContext());
    bookmarks::BookmarkModel* bm =
        profile ? BookmarkModelFactory::GetForBrowserContext(profile)
                : nullptr;
    if (!bm || !bm->loaded()) {
      out.Set("success", false);
      out.Set("error", "bookmark model unavailable");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(out)));
      return;
    }
    const bookmarks::BookmarkNode* parent = bm->other_node();
    for (int i : idxs) {
      if (i < 0 || i >= model->count()) continue;
      content::WebContents* wc = model->GetWebContentsAt(i);
      if (!wc) continue;
      GURL url = wc->GetLastCommittedURL();
      if (!url.is_valid()) continue;
      bm->AddURL(parent, parent->children().size(), wc->GetTitle(), url);
      ++affected;
    }
  } else {
    out.Set("success", false);
    out.Set("error", "unknown action: " + *action);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  out.Set("success", true);
  out.Set("affected_count", affected);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleCreateWatcher: Page Watcher feature.
// Builds a Script with an INTERVAL trigger that:
//   1. NAVIGATEs to the watched URL
//   2. WAIT_FORs the selector
//   3. EXTRACTs the selector's text into {{value}}
//   4. NOTIFYs the user with that value
// then saves it via AutomationStorage so the existing scheduler picks
// it up on its next tick. Per user spec, watchers use the default
// (TinyLlama) model and run in a background window.
//
// Args: [callback_id, {url, selector, interval_seconds, name?}]
// Returns: {success, script_id, error?}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleCreateWatcher(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  base::DictValue out;
  if (!args[1].is_dict()) {
    out.Set("success", false);
    out.Set("error", "expected watcher dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  const base::DictValue& d = args[1].GetDict();
  const std::string* url = d.FindString("url");
  const std::string* selector = d.FindString("selector");
  std::optional<int> interval = d.FindInt("interval_seconds");
  const std::string* name = d.FindString("name");
  if (!url || url->empty() || !selector || selector->empty()) {
    out.Set("success", false);
    out.Set("error", "url and selector are required");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  int seconds = interval.value_or(900);  // default 15 min
  if (seconds < 60) seconds = 60;        // floor: 1 min

  GURL gurl(*url);
  if (!gurl.is_valid() || !gurl.SchemeIsHTTPOrHTTPS()) {
    out.Set("success", false);
    out.Set("error", "url must be http(s)");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  molt_ai::automation::Script s;
  // Stable id from a hash of the url+selector — re-creating the watcher
  // with the same args updates the existing script in place.
  size_t h = std::hash<std::string>{}(*url + "|" + *selector);
  s.id = base::StringPrintf("watcher-%zx", h);
  s.name = (name && !name->empty()) ? *name
                                    : ("Watch " + std::string(gurl.host()));
  s.created_at_unix = static_cast<int64_t>(time(nullptr));
  s.trigger.type = molt_ai::automation::TriggerType::INTERVAL;
  s.trigger.expression = base::NumberToString(seconds);
  s.security.trust = molt_ai::automation::TrustLevel::APPROVED;
  s.security.max_runtime_seconds = 120;
  s.security.domain_whitelist.push_back(std::string(gurl.host()));

  {
    molt_ai::automation::Step step;
    step.type = molt_ai::automation::StepType::NAVIGATE;
    step.target = *url;
    step.timeout_ms = 20000;
    step.description = "Open watched page";
    s.steps.push_back(std::move(step));
  }
  {
    molt_ai::automation::Step step;
    step.type = molt_ai::automation::StepType::WAIT_FOR;
    step.target = *selector;
    step.timeout_ms = 15000;
    step.description = "Wait for selector";
    s.steps.push_back(std::move(step));
  }
  {
    molt_ai::automation::Step step;
    step.type = molt_ai::automation::StepType::EXTRACT;
    step.target = *selector;
    step.store_as = "value";
    step.timeout_ms = 5000;
    step.description = "Extract current value";
    s.steps.push_back(std::move(step));
  }
  {
    molt_ai::automation::Step step;
    step.type = molt_ai::automation::StepType::NOTIFY;
    step.target = s.name;
    step.value = "Current value: {{value}}";
    step.timeout_ms = 2000;
    step.description = "Notify user";
    s.steps.push_back(std::move(step));
  }

  // Save the script to disk. AutomationStorage methods touch the
  // filesystem on the UI thread, so wrap in ScopedAllowBlockingForMolt.
  bool saved = false;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::automation::AutomationStorage storage;
    storage.EnsureDirectory();
    saved = storage.Save(s);
  }

  if (!saved) {
    out.Set("success", false);
    out.Set("error", "failed to save watcher script");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Nudge the scheduler so it picks up the new INTERVAL trigger right
  // away rather than waiting for its next reload tick.
  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  if (profile) {
    auto* svc =
        molt_ai::automation::AutomationSchedulerServiceFactory::GetForProfile(
            profile);
    if (svc && svc->scheduler()) {
      svc->scheduler()->Reschedule();
    }
  }

  out.Set("success", true);
  out.Set("script_id", s.id);
  out.Set("interval_seconds", seconds);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleListActiveAgents: Agent Inbox feature.
// Reads the process-wide AgentInboxRegistry and returns one row per
// currently-running (or just-finished) background automation run.
// The side-panel polls this every few seconds to keep the "Running
// agents" tray fresh.
// Args: [callback_id]
// Returns: {agents: [{id, script_id, script_name, start_url,
//                     current_step, total_steps, status_note,
//                     started_at_unix, finished_at_unix, succeeded}]}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleListActiveAgents(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  base::ListValue arr;
  auto entries = molt_ai::automation::AgentInboxRegistry::Get()->List();
  for (const auto& e : entries) {
    base::DictValue d;
    d.Set("id", static_cast<double>(e.id));
    d.Set("script_id", e.script_id);
    d.Set("script_name", e.script_name);
    d.Set("start_url", e.start_url);
    d.Set("current_step", e.current_step);
    d.Set("total_steps", e.total_steps);
    d.Set("status_note", e.status_note);
    d.Set("started_at_unix", static_cast<double>(e.started_at_unix));
    d.Set("finished_at_unix", static_cast<double>(e.finished_at_unix));
    d.Set("succeeded", e.succeeded);
    arr.Append(std::move(d));
  }
  base::DictValue out;
  out.Set("agents", std::move(arr));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// Form Filler — load/save the encrypted local profile.
// Profile lives at ~/.moltbrowser/profile.enc, encrypted via OSCrypt.
// We do synchronous file I/O on the UI thread guarded by
// ScopedAllowBlockingForMolt; the blob is ~hundreds-of-bytes small so
// the cost is negligible vs. the bounce-to-worker boilerplate.
// ------------------------------------------------------------------
namespace {

// Shown when ~/.moltbrowser/profile.enc exists but can't be decrypted. The
// store never overwrites it on its own (see molt_profile_store.h).
constexpr char kProfileUnreadableError[] =
    "Your saved profile couldn't be decrypted (is your system keyring "
    "locked?), so it was not changed. Unlock the keyring and try again.";

}  // namespace

void MoltAIChatHandler::HandleGetMoltProfile(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  molt_ai::profile::ProfileLoadResult loaded;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::profile::MoltProfileStore store;
    loaded = store.LoadWithStatus();
  }
  base::DictValue out;
  out.Set("profile", std::move(loaded.profile));
  // A profile exists but couldn't be read: the editor says so instead of
  // looking like there is no profile, and saving won't replace it.
  out.Set("unreadable", loaded.status ==
                            molt_ai::profile::ProfileReadStatus::kUnreadable);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// args: [callback_id, profile dict, options?]. options.backupUnreadable is
// the user's explicit "start over" after a save was refused because the
// existing profile couldn't be read: the old file is kept as a backup.
void MoltAIChatHandler::HandleSaveMoltProfile(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  base::DictValue out;
  if (!args[1].is_dict()) {
    out.Set("success", false);
    out.Set("error", "expected profile dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  const bool backup_unreadable =
      args.size() > 2 && args[2].is_dict() &&
      args[2].GetDict().FindBool("backupUnreadable").value_or(false);
  base::DictValue dict = args[1].GetDict().Clone();
  molt_ai::profile::ProfileSaveResult result =
      molt_ai::profile::ProfileSaveResult::kFailed;
  base::FilePath backup;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::profile::MoltProfileStore store;
    result = backup_unreadable
                 ? store.SaveKeepingUnreadableAsBackup(dict, &backup)
                 : store.Save(dict);
  }
  using molt_ai::profile::ProfileSaveResult;
  const bool ok = result == ProfileSaveResult::kSaved;
  out.Set("success", ok);
  switch (result) {
    case ProfileSaveResult::kSaved:
      if (!backup.empty()) {
        // File name only; the editor says it is in ~/.moltbrowser/.
        out.Set("backup", backup.BaseName().AsUTF8Unsafe());
      }
      break;
    case ProfileSaveResult::kNoSecureStorage:
      out.Set("error", molt_ai::SecureStorageUnavailableMessage());
      break;
    case ProfileSaveResult::kExistingUnreadable:
      out.Set("error", kProfileUnreadableError);
      out.Set("unreadable", true);
      break;
    case ProfileSaveResult::kFailed:
      out.Set("error", "failed to save profile");
      break;
  }
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// Form Filler — autofill the active tab from the saved profile.
//
// We do the matching in JS injected into an isolated world. Reasons:
//   - The matcher needs full DOM access (labels, autocomplete attrs,
//     aria-labelledby chains, surrounding text) which is cheaper to
//     express in JS than to round-trip across IPC.
//   - The matcher dispatches input/change events so SPA frameworks
//     (React, Angular, Vue) see the change.
//
// Native side just loads the profile, JSON-stringifies it, and hands
// it to the injected JS as a literal.
// ------------------------------------------------------------------
namespace {

// Heuristic field-name matcher. Returns the list of candidate substrings
// for each profile key — case-insensitive substring search against the
// concatenation of input.name, input.id, input.placeholder, label-for
// text, autocomplete attribute, and aria-label.
constexpr char kFormFillJS[] = R"JS(
(function(profile){
  if (!profile || typeof profile !== 'object') return {filled:0, total:0};
  // Match table: profile key -> array of regex strings to test against
  // the input's concatenated identity (name|id|placeholder|label|...).
  var rules = {
    email:          [/e[-_ ]?mail/i, /^email/i, /username.*email/i],
    phone:          [/phone/i, /tel(ephone)?/i, /mobile/i, /cell/i],
    full_name:      [/^name$/i, /full[-_ ]?name/i, /your[-_ ]?name/i],
    first_name:    [/first[-_ ]?name/i, /given[-_ ]?name/i, /\bfname\b/i],
    last_name:     [/last[-_ ]?name/i, /family[-_ ]?name/i, /sur[-_ ]?name/i, /\blname\b/i],
    address_line1: [/address[-_ ]?(line)?[-_ ]?1/i, /street[-_ ]?address/i, /^address$/i, /\baddr1?\b/i],
    address_line2: [/address[-_ ]?(line)?[-_ ]?2/i, /apt|apartment|suite|unit/i, /\baddr2\b/i],
    city:          [/city/i, /town/i, /locality/i],
    state:         [/state/i, /province/i, /region/i],
    zip:           [/zip/i, /postal/i, /post[-_ ]?code/i],
    country:       [/country/i, /nation/i],
    company:       [/company/i, /organi[sz]ation/i, /employer/i],
    job_title:     [/job[-_ ]?title/i, /position/i, /role/i],
    website:       [/website/i, /\burl\b/i, /homepage/i]
  };
  // Build identity string for a form control.
  function identity(el) {
    var s = (el.name || '') + ' ' + (el.id || '') + ' ' +
            (el.placeholder || '') + ' ' +
            (el.getAttribute('autocomplete') || '') + ' ' +
            (el.getAttribute('aria-label') || '');
    if (el.labels) {
      for (var i = 0; i < el.labels.length; i++) {
        s += ' ' + (el.labels[i].innerText || '');
      }
    }
    return s;
  }
  function fillField(el, value) {
    if (value == null || value === '') return false;
    try {
      var d = Object.getOwnPropertyDescriptor(
          window.HTMLInputElement.prototype, 'value');
      if (d && d.set) d.set.call(el, value);
      else el.value = value;
      el.dispatchEvent(new Event('input', {bubbles:true}));
      el.dispatchEvent(new Event('change', {bubbles:true}));
      return true;
    } catch (e) { return false; }
  }
  var inputs = Array.prototype.slice.call(
      document.querySelectorAll('input, textarea, select'));
  var filled = 0;
  var skipped = 0;
  for (var i = 0; i < inputs.length; i++) {
    var el = inputs[i];
    if (el.type === 'hidden' || el.type === 'submit' ||
        el.type === 'button' || el.type === 'file' ||
        el.type === 'password' || el.type === 'checkbox' ||
        el.type === 'radio' || el.disabled || el.readOnly) {
      skipped++;
      continue;
    }
    if (el.value && el.value.length > 0) {
      // Don't overwrite existing values — user may have typed.
      skipped++;
      continue;
    }
    var id = identity(el).toLowerCase();
    if (!id.trim()) continue;
    var matched = null;
    for (var key in rules) {
      if (!Object.prototype.hasOwnProperty.call(rules, key)) continue;
      if (!profile[key]) continue;
      var patterns = rules[key];
      for (var p = 0; p < patterns.length; p++) {
        if (patterns[p].test(id)) { matched = key; break; }
      }
      if (matched) break;
    }
    if (matched && fillField(el, profile[matched])) {
      filled++;
      el.setAttribute('data-molt-filled', matched);
    }
  }
  return {filled: filled, total: inputs.length, skipped: skipped};
})
)JS";

}  // namespace

void MoltAIChatHandler::HandleRunFormFill(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  base::DictValue out;

  // Resolve target tab.
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(webui_wc);
  if (!browser || !browser->GetTabStripModel()) {
    out.Set("success", false);
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  content::WebContents* target =
      browser->GetTabStripModel()->GetActiveWebContents();
  if (!target || !target->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    out.Set("success", false);
    out.Set("error", "active tab is not an http(s) page");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Load profile and stringify.
  molt_ai::profile::ProfileLoadResult loaded;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::profile::MoltProfileStore store;
    loaded = store.LoadWithStatus();
  }
  base::DictValue profile = std::move(loaded.profile);
  if (profile.empty()) {
    out.Set("success", false);
    out.Set("error",
            loaded.status == molt_ai::profile::ProfileReadStatus::kUnreadable
                ? kProfileUnreadableError
                : "profile is empty — open settings to set one up");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  std::string profile_json;
  base::JSONWriter::Write(profile, &profile_json);

  std::string js = std::string(kFormFillJS) + "(" + profile_json + ")";

  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  std::string cb_id = callback_id;
  target->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(js),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             base::Value v) {
            if (!self) return;
            base::DictValue r;
            if (v.is_dict()) {
              const base::DictValue& d = v.GetDict();
              std::optional<int> filled = d.FindInt("filled");
              std::optional<int> total = d.FindInt("total");
              r.Set("success", filled.value_or(0) > 0);
              r.Set("filled", filled.value_or(0));
              r.Set("total", total.value_or(0));
              if (filled.value_or(0) == 0)
                r.Set("error", "no matching fields found");
            } else {
              r.Set("success", false);
              r.Set("error", "fill script returned no data");
            }
            self->ResolveJavascriptCallback(base::Value(cb_id),
                                            base::Value(std::move(r)));
          },
          weak_this, cb_id),
      /*world_id=*/1);
}

// ------------------------------------------------------------------
// HandleListMemoryDocs — Personal Vector Memory dump for the AI-grouped
// history WebUI. We don't cluster server-side; the JS does
// keyword-overlap clustering so the user can re-cluster (filter by
// host, "merge near-duplicates", etc.) without a round-trip.
// Args: [callback_id, limit?]  (limit defaults to 200, capped at 2000)
// Returns: {docs:[{url,title,visited_at,word_count,host}]}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleListMemoryDocs(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  int limit = 200;
  if (args.size() > 1 && args[1].is_int()) {
    limit = std::max(1, std::min(2000, args[1].GetInt()));
  }

  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  if (!profile) {
    base::DictValue empty;
    empty.Set("docs", base::ListValue());
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(empty)));
    return;
  }
  molt_ai::memory::MemoryService* svc =
      molt_ai::memory::MemoryServiceFactory::GetForProfile(profile);
  if (!svc) {
    base::DictValue empty;
    empty.Set("docs", base::ListValue());
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(empty)));
    return;
  }
  std::string cb_id = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  svc->ListRecent(limit, base::BindOnce(
      [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
         std::vector<molt_ai::memory::Document> docs) {
        if (!self) return;
        base::ListValue arr;
        for (const auto& d : docs) {
          base::DictValue dv;
          dv.Set("doc_id", static_cast<double>(d.doc_id));
          dv.Set("url", d.url);
          dv.Set("title", d.title);
          dv.Set("visited_at_unix",
                 static_cast<double>(d.visited_at_unix));
          dv.Set("word_count", d.word_count);
          // Convenience: pre-extract the host so the JS clusterer
          // doesn't need to instantiate a URL per doc.
          GURL g(d.url);
          dv.Set("host", g.is_valid() ? std::string(g.host()) : std::string());
          arr.Append(std::move(dv));
        }
        base::DictValue out;
        out.Set("docs", std::move(arr));
        self->ResolveJavascriptCallback(base::Value(cb_id),
                                        base::Value(std::move(out)));
      },
      weak_this, cb_id));
}

// ------------------------------------------------------------------
// HandleExtractPdfText: download a PDF URL via SimpleURLLoader, read
// its text layer in the sandboxed document parser (a utility process,
// see document_parser_client.h), fall back to OCR for a scan, and ship
// the result back to chat. The result includes the host so the UI can
// show a "Chatting with foo.pdf from example.com" chip.
//
// Args: [callback_id, url]
// Returns: {success, url, text, char_count, error?}
// ------------------------------------------------------------------
namespace {
// Text budget for /pdf (the text layer and the OCR fallback alike).
constexpr size_t kMaxPdfTextChars = 50000;
}  // namespace

void MoltAIChatHandler::HandleExtractPdfText(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string url = args[1].is_string() ? args[1].GetString() : "";

  base::DictValue err;
  if (url.empty()) {
    err.Set("success", false);
    err.Set("error", "missing url");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(err)));
    return;
  }
  GURL gurl(url);
  // http(s) only. The earlier version also allowed file://, but the
  // browser-process URLLoaderFactory we use here doesn't actually
  // honor file:// — so it was a dormant LFI primitive waiting for
  // someone to swap in a permissive factory. Code-review HIGH #3.
  // If file:// PDF support is ever needed, route it through a
  // dedicated handler that explicitly opts in (and adds path-prefix
  // restrictions).
  if (!gurl.is_valid() || !gurl.SchemeIsHTTPOrHTTPS()) {
    err.Set("success", false);
    err.Set("error", "url must be http(s)");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(err)));
    return;
  }

  pdf_callback_id_ = callback_id;
  pdf_source_url_ = url;

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = gurl;
  resource_request->method = "GET";
  resource_request->credentials_mode = network::mojom::CredentialsMode::kOmit;

  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("molt_ai_pdf_extract", R"(
        semantics {
          sender: "MoltBrowser AI"
          description: "Downloads a PDF the user asked to chat with"
          trigger: "User invokes /pdf in the side panel chat"
          data: "HTTP GET for the PDF the user asked about"
          destination: WEBSITE
        }
        policy {
          cookies_allowed: NO
          setting: "User-initiated PDF extraction in the AI side panel"
        })");

  pdf_loader_ = network::SimpleURLLoader::Create(
      std::move(resource_request), traffic_annotation);
  pdf_loader_->SetAllowHttpErrorResults(false);
  // Cap at 25 MB so a giant PDF doesn't OOM the browser process.
  static constexpr size_t kMaxPdfBytes = 25 * 1024 * 1024;

  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  network::mojom::URLLoaderFactory* factory =
      profile ? profile->GetDefaultStoragePartition()
                    ->GetURLLoaderFactoryForBrowserProcess().get()
              : nullptr;
  if (!factory) {
    err.Set("success", false);
    err.Set("error", "no url loader factory");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(err)));
    pdf_loader_.reset();
    return;
  }

  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  pdf_loader_->DownloadToString(
      factory,
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self,
             std::optional<std::string> body) {
            if (!self) return;
            std::string cb_id = self->pdf_callback_id_;
            std::string src_url = self->pdf_source_url_;
            self->pdf_loader_.reset();
            self->pdf_callback_id_.clear();
            self->pdf_source_url_.clear();

            base::DictValue out;
            out.Set("url", src_url);
            if (!body.has_value() || body->empty()) {
              out.Set("success", false);
              out.Set("error", "PDF download failed");
              self->ResolveJavascriptCallback(base::Value(cb_id),
                                              base::Value(std::move(out)));
              return;
            }
            // The text layer is read in the sandboxed document parser;
            // OnPdfTextLayer() falls back to OCR for a scan. The client
            // copies |pdf_bytes| before returning, and moving the
            // unique_ptr doesn't move the string it points to.
            auto pdf = std::make_unique<std::string>(std::move(*body));
            const base::span<const uint8_t> pdf_bytes =
                base::as_byte_span(*pdf);
            self->document_parser_.ExtractPdfText(
                pdf_bytes, kMaxPdfTextChars,
                base::BindOnce(&MoltAIChatHandler::OnPdfTextLayer, self,
                               std::move(cb_id), std::move(src_url),
                               std::move(pdf)));
          },
          weak_this),
      kMaxPdfBytes);
}

void MoltAIChatHandler::OnPdfTextLayer(std::string callback_id,
                                       std::string source_url,
                                       std::unique_ptr<std::string> pdf,
                                       molt_ai::DocumentParseResult result) {
  using Status = molt_ai::DocumentParseResult::Status;
  if (result.status != Status::kNoText) {
    base::DictValue out;
    out.Set("url", source_url);
    if (result.status == Status::kOk) {
      out.Set("success", true);
      out.Set("char_count", static_cast<int>(result.text.size()));
      out.Set("text", std::move(result.text));
      out.Set("source", "text_layer");
    } else {
      // The parser crashed, hung or misbehaved on this document: never
      // hand it on to the OCR helper too (its sandbox is looser, and on
      // Windows confines no files).
      out.Set("success", false);
      out.Set("error", "couldn't read this PDF: " + result.error);
    }
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  // No text layer — fall back to OCR via bundled tesseract.
  // We keep the URL in the response by routing through a
  // tiny lambda that re-wraps the result.
  molt_ai::ocr::OcrService::Get()->OcrPdf(
      std::move(*pdf), kMaxPdfTextChars,
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             std::string src_url, molt_ai::ocr::OcrResult r) {
            if (!self) return;
            base::DictValue out;
            out.Set("url", src_url);
            out.Set("source", "ocr");
            out.Set("ocr_binary_source", r.binary_source);
            out.Set("ocr_duration_ms", r.duration_ms);
            if (r.success) {
              out.Set("success", true);
              out.Set("text", r.text);
              out.Set("char_count", static_cast<int>(r.text.size()));
            } else {
              out.Set("success", false);
              // Be explicit about whether tesseract was
              // missing vs. failed.
              if (r.binary_source == "none") {
                out.Set("error",
                        "no text layer and no OCR available "
                        "— run scripts/bundle-tesseract.sh "
                        "or install tesseract.");
              } else {
                out.Set("error", "no text layer; OCR failed: " + r.error);
              }
            }
            self->ResolveJavascriptCallback(base::Value(cb_id),
                                            base::Value(std::move(out)));
          },
          weak_ptr_factory_.GetWeakPtr(), std::move(callback_id),
          std::move(source_url)));
}

// ------------------------------------------------------------------
// HandleSearchBookmarks: walk the BookmarkModel tree and rank entries
// against the query using simple unique-token overlap on title+url+host.
// Same ranker as the chunk-ranker in the chat (deterministic, no
// embedding needed). Returns top-K.
//
// Args: [callback_id, query, top_k]
// Returns: {hits: [{title, url, host, score, parent}]}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleSearchBookmarks(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string query = args[1].is_string() ? args[1].GetString() : "";
  int top_k = 8;
  if (args.size() > 2 && args[2].is_int())
    top_k = std::max(1, args[2].GetInt());

  base::DictValue out;
  base::ListValue hits;

  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  bookmarks::BookmarkModel* bm =
      profile ? BookmarkModelFactory::GetForBrowserContext(profile)
              : nullptr;
  if (!bm || !bm->loaded() || query.empty()) {
    out.Set("hits", std::move(hits));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Tokenise query.
  auto tokenise = [](const std::string& s) {
    std::vector<std::string> out_tokens;
    std::string cur;
    for (char c : s) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9')) {
        cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      } else {
        if (cur.size() >= 2) out_tokens.push_back(cur);
        cur.clear();
      }
    }
    if (cur.size() >= 2) out_tokens.push_back(cur);
    return out_tokens;
  };
  std::vector<std::string> q_tokens = tokenise(query);
  std::set<std::string> q_set(q_tokens.begin(), q_tokens.end());
  if (q_set.empty()) {
    out.Set("hits", std::move(hits));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  struct Cand {
    int score = 0;
    std::string title;
    std::string url;
    std::string host;
    std::string parent;
  };
  std::vector<Cand> cands;

  // DFS over the bookmark tree.
  std::function<void(const bookmarks::BookmarkNode*, const std::string&)> dfs;
  dfs = [&](const bookmarks::BookmarkNode* node,
            const std::string& parent_name) {
    if (!node) return;
    if (node->is_url()) {
      std::string title = base::UTF16ToUTF8(node->GetTitle());
      std::string url = node->url().spec();
      std::string host = std::string(node->url().host());
      std::vector<std::string> doc_tokens = tokenise(title + " " + url);
      std::set<std::string> seen;
      int score = 0;
      for (const auto& t : doc_tokens) {
        if (q_set.count(t) && !seen.count(t)) {
          ++score;
          seen.insert(t);
        }
      }
      if (score > 0) {
        cands.push_back({score, std::move(title), std::move(url),
                         std::move(host), parent_name});
      }
    } else {
      std::string my_name = base::UTF16ToUTF8(node->GetTitle());
      for (size_t i = 0; i < node->children().size(); ++i) {
        dfs(node->children()[i].get(),
            my_name.empty() ? parent_name : my_name);
      }
    }
  };
  dfs(bm->root_node(), "");

  std::sort(cands.begin(), cands.end(),
            [](const Cand& a, const Cand& b) { return a.score > b.score; });
  int kept = 0;
  for (const auto& c : cands) {
    if (kept++ >= top_k) break;
    base::DictValue d;
    d.Set("title", c.title);
    d.Set("url", c.url);
    d.Set("host", c.host);
    d.Set("score", c.score);
    d.Set("parent", c.parent);
    hits.Append(std::move(d));
  }
  out.Set("hits", std::move(hits));
  out.Set("query_token_count", static_cast<int>(q_set.size()));
  out.Set("total_candidates", static_cast<int>(cands.size()));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleExtractAllTabsText: pull innerText from every tab in the owning
// Browser's tab strip via ExecuteJavaScriptInIsolatedWorld. Returns a
// flat array of {url, title, text}. Each tab is capped at
// max_chars_per_tab (default 4000) so the LLM context isn't blown.
//
// The N async JS calls all hit the same UI thread; we use a small
// shared counter to know when all returned and emit the final response
// once.
//
// Args: [callback_id, max_chars_per_tab]
// Returns: {tabs: [{url, title, text}], total_chars}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleExtractAllTabsText(
    const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  int max_chars_per_tab = 4000;
  if (args.size() > 1 && args[1].is_int())
    max_chars_per_tab = std::max(200, args[1].GetInt());

  content::WebContents* webui_wc = web_ui()->GetWebContents();
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(webui_wc);

  base::DictValue out;
  base::ListValue tabs;
  if (!browser || !browser->GetTabStripModel()) {
    out.Set("tabs", std::move(tabs));
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  TabStripModel* model = browser->GetTabStripModel();
  const int n = model->count();
  if (n == 0) {
    out.Set("tabs", std::move(tabs));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Shared collector held by shared_ptr so each per-tab callback can
  // contribute. When pending drops to 0 we emit the resolve.
  struct Collector {
    int pending = 0;
    base::ListValue results;
    int total_chars = 0;
  };
  auto collector = std::make_shared<Collector>();
  collector->pending = n;
  std::string cb_id_copy = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();

  for (int i = 0; i < n; ++i) {
    content::WebContents* wc = model->GetWebContentsAt(i);
    base::DictValue tab_row;
    tab_row.Set("index", i);
    if (!wc || !wc->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
      tab_row.Set("url", wc ? wc->GetLastCommittedURL().spec() : std::string());
      tab_row.Set("title",
                  wc ? base::UTF16ToUTF8(wc->GetTitle()) : std::string());
      tab_row.Set("text", "");
      collector->results.Append(std::move(tab_row));
      --collector->pending;
      if (collector->pending == 0) {
        base::DictValue resolved;
        resolved.Set("tabs", std::move(collector->results));
        resolved.Set("total_chars", collector->total_chars);
        ResolveJavascriptCallback(base::Value(cb_id_copy),
                                  base::Value(std::move(resolved)));
      }
      continue;
    }
    tab_row.Set("url", wc->GetLastCommittedURL().spec());
    tab_row.Set("title", base::UTF16ToUTF8(wc->GetTitle()));
    // Inject a tiny script that returns the body innerText sliced to
    // |max_chars_per_tab|. Isolated world 1 — same world the chat
    // already uses for page-context extraction.
    std::string js = "(function(){"
        "var t=(document.body && document.body.innerText) || '';"
        "return t.length > " + std::to_string(max_chars_per_tab) + "?"
        "t.slice(0," + std::to_string(max_chars_per_tab) + "):t;})()";
    wc->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(js),
        base::BindOnce(
            [](base::WeakPtr<MoltAIChatHandler> self,
               std::shared_ptr<Collector> c, std::string cb_id,
               base::DictValue row, base::Value result) {
              std::string txt = result.is_string() ? result.GetString() : "";
              row.Set("text", txt);
              c->total_chars += static_cast<int>(txt.size());
              c->results.Append(std::move(row));
              if (--c->pending == 0 && self) {
                base::DictValue resolved;
                resolved.Set("tabs", std::move(c->results));
                resolved.Set("total_chars", c->total_chars);
                self->ResolveJavascriptCallback(
                    base::Value(cb_id),
                    base::Value(std::move(resolved)));
              }
            },
            weak_this, collector, cb_id_copy, std::move(tab_row)),
        /*world_id=*/1);
  }
}

// ------------------------------------------------------------------
// HandleOpenSandboxTab: open |url| in an off-the-record session so the
// page's cookies and storage are discarded when the OTR window closes.
// Reuses Chromium's existing chrome::OpenURLOffTheRecord which handles
// the OTR profile lifecycle for us.
//
// Args: [callback_id, url]
// Returns: {success, error?}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleOpenSandboxTab(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  std::string url_str = args[1].is_string() ? args[1].GetString() : "";

  base::DictValue out;
  if (url_str.empty()) {
    out.Set("success", false);
    out.Set("error", "missing url");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  if (!url_str.starts_with("http://") && !url_str.starts_with("https://")) {
    url_str = "https://" + url_str;
  }
  GURL g(url_str);
  if (!g.is_valid()) {
    out.Set("success", false);
    out.Set("error", "invalid url");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  if (!profile) {
    out.Set("success", false);
    out.Set("error", "no profile");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  // OpenURLOffTheRecord wants the *original* (non-OTR) profile.
  Profile* original = profile->GetOriginalProfile();
  chrome::OpenURLOffTheRecord(original, g);

  out.Set("success", true);
  out.Set("url", g.spec());
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleOpenUrlInTab: open an http(s) link from a chat response in a
// real browser tab. The AI chat side panel's WebContents is not a tab
// in any tab strip and has no AddNewContents delegate, so a plain
// <a target="_blank"> is silently dropped. The frontend intercepts the
// click and routes the URL here.
//
// This is the C++ half of a two-layer scheme allowlist: the JS side
// only builds/forwards http(s) anchors, and this re-validates before
// navigating. Unlike the sandbox/tor handlers we do NOT prepend https
// to a bare string — an invalid or non-http(s) URL is rejected.
//
// Args: [callback_id, url]
// Returns: {success, error?}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleOpenUrlInTab(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  // Guard the type as the rest of this file does; a non-string arg then
  // cleanly fails the is_valid()/scheme check below rather than tripping
  // GetString()'s DCHECK.
  const std::string url = args[1].is_string() ? args[1].GetString()
                                              : std::string();

  base::DictValue out;

  GURL g(url);
  if (!g.is_valid() ||
      !(g.SchemeIs(url::kHttpScheme) || g.SchemeIs(url::kHttpsScheme))) {
    out.Set("success", false);
    out.Set("error", "unsupported url");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Resolve the browser window that should receive the new tab. The
  // side panel's WebContents is not a tab (FindBrowserWithTab returns
  // null), so fall back to the most-recently-active browser window.
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(webui_wc);
  if (!browser) {
    browser = GlobalBrowserCollection::GetInstance()->GetLastActiveBrowser();
  }
  if (!browser) {
    out.Set("success", false);
    out.Set("error", "no active browser window");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  chrome::AddSelectedTabWithURL(browser, g, ui::PAGE_TRANSITION_LINK);
  out.Set("success", true);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// HandleOpenMoltSettings: open the AI Settings page.
//
// Side panel (the common case): the panel's WebContents is NOT a tab
// in any tab strip (FindBrowserWithTab returns null for it), and
// window.open() is silently dropped — AiChatSidePanelWebView (a
// views::WebView) is the WebContents delegate and doesn't implement
// AddNewContents. So we navigate the panel's OWN WebContents in place
// to molt://ai-settings/?panel=1. The molt:// → chrome:// rewrite
// (HandleMoltSchemeRewrite) replaces scheme+host only, so ?panel=1
// survives; the settings page reads it to show a back arrow that
// history.back()s to the chat (both URLs live in the panel's own
// NavigationController).
//
// Tab-hosted chat (chrome://molt-ai-chat opened as a regular tab):
// keep the original behavior — open settings in a new selected tab of
// that window instead of navigating the chat tab away.
//
// Args: [callback_id]
// Returns: {success, error?}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleOpenMoltSettings(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  // Optional 2nd arg: a settings section to deep-link to. The settings page
  // reads ?section=<name> and scrolls to (and briefly highlights) it.
  // Allow-list the known sections so we never build an odd URL from JS.
  std::string section;
  if (args.size() >= 2u && args[1].is_string()) {
    const std::string& s = args[1].GetString();
    if (s == "providers" || s == "import" || s == "passwords") {
      section = s;
    }
  }

  base::DictValue out;

  content::WebContents* webui_wc = web_ui()->GetWebContents();
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(webui_wc);
  if (browser && browser->GetTabStripModel()) {
    // Chat is hosted in a real tab — open settings beside it.
    std::string url = "molt://ai-settings/";
    if (!section.empty()) {
      url += "?section=" + section;
    }
    chrome::AddSelectedTabWithURL(browser, GURL(url),
                                  ui::PAGE_TRANSITION_AUTO_BOOKMARK);
    out.Set("success", true);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Side panel: navigate the panel itself. Same LoadURL-on-arbitrary-
  // WebContents shape as WebAgent::ExecuteNavigate (web_agent.cc) and
  // the panel host's own LoadInitialURL. Resolve the promise BEFORE
  // starting the navigation so the callback isn't dropped when the
  // chat page is torn down. ?panel=1 drives the back-arrow; a section
  // deep-link (if any) rides alongside it.
  out.Set("success", true);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));

  std::string url = "molt://ai-settings/?panel=1";
  if (!section.empty()) {
    url += "&section=" + section;
  }
  webui_wc->GetController().LoadURL(GURL(url), content::Referrer(),
                                    ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                    std::string());
}

// ------------------------------------------------------------------
// HandleCloseSidePanel: close the AI chat side panel from the in-page
// close control. The panel's WebContents is not a tab, so
// FindBrowserWithTab returns null — fall back to the last-active
// browser, which hosts this panel. The AI chat entry is a kContent
// side panel (see side_panel_helper.cc). Fire-and-forget: do NOT
// AllowJavascript / resolve a callback because Close() tears down this
// WebUI immediately.
// Args: []
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleCloseSidePanel(const base::ListValue& args) {
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(webui_wc);
  if (!browser) {
    browser = GlobalBrowserCollection::GetInstance()->GetLastActiveBrowser();
  }
  if (!browser) {
    return;
  }
  if (SidePanelUI* side_panel_ui = browser->GetFeatures().side_panel_ui()) {
    // There is a single (content) side panel at 152; the per-type
    // Close(PanelType) overload is gone.
    side_panel_ui->Close();
  }
}

// ------------------------------------------------------------------
// HandleGetTrackerBreakdown: Privacy heatmap.
//
// Inject a JS probe into the active tab's primary frame (isolated
// world 1) that walks performance.getEntriesByType('resource') —
// every subresource the page already loaded. We classify each remote
// host into ads / analytics / cdn / other, then return aggregate
// counts plus the top 12 unique hosts.
//
// We deliberately do *not* use a giant block-list dependency. The
// classifier here is intentionally conservative — its purpose is
// transparency about who the page talks to, not to be EasyList.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGetTrackerBreakdown(
    const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  base::DictValue out;
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(webui_wc);
  if (!browser || !browser->GetTabStripModel()) {
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  content::WebContents* target =
      browser->GetTabStripModel()->GetActiveWebContents();
  if (!target || !target->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    out.Set("error", "active tab is not an http(s) page");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // The JS does the classification client-side and returns a struct.
  // Why: avoids round-tripping every resource URL to C++; resource
  // lists can be hundreds long on news pages.
  static constexpr char kProbeJS[] = R"JS(
(function(){
  try {
    var origin = location.host;
    var entries = performance.getEntriesByType('resource') || [];
    var counts = {ads:0, analytics:0, cdn:0, other:0, first_party:0};
    var hostSet = {};
    function classify(h) {
      if (/doubleclick|adservice|adsystem|adnxs|ads-twitter|pubmatic|rubiconproject|criteo|taboola|outbrain|adsrvr|adform|amazon-adsystem|googlesyndication|googletagservices/.test(h))
        return 'ads';
      if (/google-analytics|googletagmanager|segment\.io|mixpanel|hotjar|fullstory|chartbeat|optimizely|amplitude|heap|matomo|plausible|cloudflareinsights|datadoghq|sentry/.test(h))
        return 'analytics';
      if (/cloudfront|akamai|fastly|cdn77|jsdelivr|unpkg|bunnycdn|cloudflare\.com|akamaihd|edgekey|imgix/.test(h))
        return 'cdn';
      return 'other';
    }
    for (var i = 0; i < entries.length; i++) {
      var u;
      try { u = new URL(entries[i].name); } catch(e) { continue; }
      var h = u.host;
      if (!h) continue;
      if (h === origin || h.endsWith('.' + origin) ||
          origin.endsWith('.' + h)) {
        counts.first_party += 1;
        continue;
      }
      hostSet[h] = (hostSet[h] || 0) + 1;
      var c = classify(h);
      counts[c] += 1;
    }
    var hosts = Object.keys(hostSet).map(function(h){
      return {host: h, count: hostSet[h], category: classify(h)};
    }).sort(function(a,b){ return b.count - a.count; }).slice(0, 12);
    return JSON.stringify({
      origin: origin,
      counts: counts,
      total_third_party: counts.ads + counts.analytics +
                         counts.cdn + counts.other,
      unique_third_party_hosts: Object.keys(hostSet).length,
      top_hosts: hosts
    });
  } catch (e) { return JSON.stringify({error: '' + e}); }
})();
)JS";

  std::string callback_id_copy = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  target->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(std::string(kProbeJS)),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             base::Value v) {
            if (!self) return;
            base::DictValue resolved;
            if (v.is_string()) {
              auto parsed = base::JSONReader::Read(v.GetString(),
                                                   /*options=*/0);
              if (parsed && parsed->is_dict()) {
                self->ResolveJavascriptCallback(
                    base::Value(cb_id),
                    base::Value(std::move(parsed->GetDict())));
                return;
              }
            }
            resolved.Set("error", "tracker probe returned no data");
            self->ResolveJavascriptCallback(base::Value(cb_id),
                                            base::Value(std::move(resolved)));
          },
          weak_this, callback_id_copy),
      /*world_id=*/1);
}

// ------------------------------------------------------------------
// HandleGetDomainReputation: ask MemoryService for everything we've
// indexed from |host| (or the active tab's host), and return a small
// summary: visit count, first/last visited, total word_count read.
// Plus a tiny risk-flag heuristic for sketchy TLDs / IP-as-host.
//
// The user can use this to answer "have I been here before? when?
// how much have I read here?" without trusting any external
// reputation service.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGetDomainReputation(
    const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  std::string host = (args.size() > 1 && args[1].is_string())
                         ? args[1].GetString()
                         : std::string();

  // If no host was supplied, infer from the active tab.
  if (host.empty()) {
    content::WebContents* webui_wc = web_ui()->GetWebContents();
    BrowserWindowInterface* browser =
        GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(webui_wc);
    if (browser && browser->GetTabStripModel()) {
      content::WebContents* t =
          browser->GetTabStripModel()->GetActiveWebContents();
      if (t) host = std::string(t->GetLastCommittedURL().host());
    }
  }
  base::DictValue out;
  out.Set("host", host);
  if (host.empty()) {
    out.Set("error", "no host");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Risk heuristics (super conservative — purely informational chips
  // for the user, not blocking).
  bool risky_tld = false;
  for (const char* t :
       {".tk", ".ml", ".ga", ".cf", ".gq", ".top", ".xyz", ".click"}) {
    size_t tl = std::strlen(t);
    if (host.size() >= tl &&
        host.compare(host.size() - tl, tl, t) == 0) {
      risky_tld = true;
      break;
    }
  }
  bool is_ip_host = !host.empty() &&
                    (host[0] >= '0' && host[0] <= '9');
  out.Set("risky_tld", risky_tld);
  out.Set("is_ip_host", is_ip_host);

  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  molt_ai::memory::MemoryService* svc =
      profile ? molt_ai::memory::MemoryServiceFactory::GetForProfile(profile)
              : nullptr;
  if (!svc) {
    out.Set("first_visit_on_device", true);
    out.Set("visit_count", 0);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  std::string cb = callback_id;
  std::string host_copy = host;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  // Pull a wide window of recent docs and filter here. The memory
  // index is held in RAM so this is sub-ms even at 50k docs.
  svc->ListRecent(
      10000,
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             std::string host, base::DictValue out_init,
             std::vector<molt_ai::memory::Document> docs) {
            if (!self) return;
            base::DictValue out = std::move(out_init);
            int visit_count = 0;
            int64_t first = 0;
            int64_t last = 0;
            int64_t total_words = 0;
            for (const auto& d : docs) {
              GURL u(d.url);
              if (!u.is_valid()) continue;
              std::string h = std::string(u.host());
              if (h != host && !(h.size() > host.size() &&
                                  h.compare(h.size() - host.size() - 1,
                                            host.size() + 1,
                                            "." + host) == 0)) {
                continue;
              }
              ++visit_count;
              total_words += d.word_count;
              if (first == 0 || d.visited_at_unix < first)
                first = d.visited_at_unix;
              if (d.visited_at_unix > last) last = d.visited_at_unix;
            }
            out.Set("visit_count", visit_count);
            out.Set("first_visit_on_device", visit_count == 0);
            out.Set("first_visit_unix", static_cast<double>(first));
            out.Set("last_visit_unix", static_cast<double>(last));
            out.Set("total_words_read", static_cast<double>(total_words));
            self->ResolveJavascriptCallback(base::Value(cb_id),
                                            base::Value(std::move(out)));
          },
          weak_this, cb, host_copy, std::move(out)));
}

// ------------------------------------------------------------------
// HandleGetConnectionPath: v0 of the network-path visualizer.
//
// At this point we don't actually route through Tor or do live
// traceroutes — those land in Phase B. What we return now is honest
// about that: it's the URL structure, whether the tab is in an OTR
// session, and a templated 3-hop "you -> ISP -> destination" diagram.
// The same response shape will later carry the real circuit nodes.
//
// Args: [callback_id, url?]
// Returns: {url, host, scheme, is_https, is_anonymous_session,
//           hops: [{label, kind}], notes}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGetConnectionPath(
    const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  std::string url_str = (args.size() > 1 && args[1].is_string())
                            ? args[1].GetString()
                            : std::string();

  content::WebContents* webui_wc = web_ui()->GetWebContents();
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(webui_wc);
  content::WebContents* active =
      (browser && browser->GetTabStripModel())
          ? browser->GetTabStripModel()->GetActiveWebContents()
          : nullptr;
  if (url_str.empty() && active) {
    url_str = active->GetLastCommittedURL().spec();
  }

  base::DictValue out;
  GURL g(url_str);
  if (!g.is_valid() || g.host().empty()) {
    out.Set("error", "no valid URL");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  out.Set("url", g.spec());
  out.Set("host", std::string(g.host()));
  out.Set("scheme", g.scheme());
  out.Set("is_https", g.SchemeIs("https"));

  bool otr = active && active->GetBrowserContext()->IsOffTheRecord();
  out.Set("is_anonymous_session", otr);

  // Three logical hops for direct browsing. Phase B will replace this
  // with real Tor circuit data when an Anonymous Tab is active.
  base::ListValue hops;
  auto add_hop = [&](const std::string& label, const std::string& kind,
                     const std::string& detail) {
    base::DictValue h;
    h.Set("label", label);
    h.Set("kind", kind);
    h.Set("detail", detail);
    hops.Append(std::move(h));
  };
  add_hop("You", "origin", "this device");
  add_hop("Your ISP", "isp",
          "the network you're connected to right now");
  add_hop(std::string(g.host()), "destination",
          g.SchemeIs("https") ? "TLS-encrypted to the destination"
                              : "PLAINTEXT — not HTTPS, anyone on the "
                                "path can read this");
  out.Set("hops", std::move(hops));
  out.Set(
      "notes",
      otr ? "Anonymous session — cookies and storage discarded on close."
          : "Direct connection. Tor multi-hop integration is in "
            "development — coming next.");
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleSetJsForDomain: toggle JavaScript for |host| via
// HostContentSettingsMap. Persisted across launches by Chromium's
// content settings infrastructure. The active tab is reloaded so the
// change takes effect immediately.
//
// Args: [callback_id, host, enabled_bool]
// Returns: {success, host, enabled}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleSetJsForDomain(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 3u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  std::string host = args[1].is_string() ? args[1].GetString() : "";
  bool enabled = args[2].is_bool() ? args[2].GetBool() : true;

  base::DictValue out;
  if (host.empty()) {
    out.Set("success", false);
    out.Set("error", "missing host");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  // Restrict to plausible hostname characters. Without this we'd
  // accept things like "*", "//evil.com", "..", "foo bar" that
  // either produce nonsense ContentSettingsPatterns or, worse,
  // produce a valid wildcard that disables JS far too broadly.
  // Code-review LOW #16. Allowed: a-z A-Z 0-9 . - _ ; 1-253 chars
  // (DNS hostname limit). Bare IP addresses are accepted by the
  // same regex.
  auto host_is_safe = [](const std::string& h) {
    if (h.empty() || h.size() > 253) return false;
    if (h[0] == '.' || h[0] == '-') return false;  // can't start with these
    for (char c : h) {
      bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '.' || c == '-' || c == '_';
      if (!ok) return false;
    }
    // Reject the all-wildcard "*"-equivalent pattern just in case the
    // wildcard-like host slipped through (it shouldn't given the
    // alphabet check above, but cheap belt-and-suspenders).
    return h != "*";
  };
  if (!host_is_safe(host)) {
    out.Set("success", false);
    out.Set("error", "invalid host (allowed: letters, digits, . - _)");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  HostContentSettingsMap* map =
      profile ? HostContentSettingsMapFactory::GetForProfile(profile)
              : nullptr;
  if (!map) {
    out.Set("success", false);
    out.Set("error", "no content settings");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  ContentSettingsPattern pattern =
      ContentSettingsPattern::FromString("[*.]" + host);
  if (!pattern.IsValid()) {
    out.Set("success", false);
    out.Set("error", "invalid host pattern");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  map->SetContentSettingCustomScope(
      pattern, ContentSettingsPattern::Wildcard(),
      ContentSettingsType::JAVASCRIPT,
      enabled ? CONTENT_SETTING_ALLOW : CONTENT_SETTING_BLOCK);

  // Reload the active tab so the change takes effect now.
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
          web_ui()->GetWebContents());
  if (browser && browser->GetTabStripModel()) {
    content::WebContents* t =
        browser->GetTabStripModel()->GetActiveWebContents();
    if (t && std::string(t->GetLastCommittedURL().host()) == host) {
      t->GetController().Reload(content::ReloadType::NORMAL,
                                /*check_for_repost=*/false);
    }
  }
  out.Set("success", true);
  out.Set("host", host);
  out.Set("enabled", enabled);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}


// ------------------------------------------------------------------
// HandleAppendReceipt: Tier 3 — write one CSV row to
// ~/.moltbrowser/ledger.csv. Plaintext, dead-simple, so the user
// can pull the file into a spreadsheet without any extra tooling.
//
// Schema:
//   date,merchant,total,currency,source_url,saved_at_unix,items_json
//
// The items array is serialized as inline JSON inside its own
// quoted column — cheap and reversible, avoids the row-explosion
// you'd get from one-row-per-item.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleAppendReceipt(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  base::DictValue out;
  if (!args[1].is_dict()) {
    out.Set("success", false);
    out.Set("error", "expected receipt dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  const base::DictValue& r = args[1].GetDict();
  std::string source_url = (args.size() > 2 && args[2].is_string())
                               ? args[2].GetString()
                               : std::string();

  const std::string* merchant = r.FindString("merchant");
  const std::string* date     = r.FindString("date");
  std::optional<double> total = r.FindDouble("total");
  const std::string* currency = r.FindString("currency");
  const base::ListValue* items = r.FindList("items");

  // Resolve ledger path: ~/.moltbrowser/ledger.csv
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) {
    out.Set("success", false);
    out.Set("error", "no home dir");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  base::FilePath dir = home.AppendASCII(".moltbrowser");
  base::FilePath ledger = dir.AppendASCII("ledger.csv");

  // CSV-escape: wrap in quotes, double internal quotes.
  auto csv = [](const std::string& s) {
    bool need_quote = s.find(',') != std::string::npos ||
                      s.find('"') != std::string::npos ||
                      s.find('\n') != std::string::npos;
    if (!need_quote) return s;
    std::string out;
    out.reserve(s.size() + 4);
    out += '"';
    for (char c : s) { if (c == '"') out += '"'; out += c; }
    out += '"';
    return out;
  };

  // Serialize items as inline JSON.
  std::string items_json = "[]";
  if (items) {
    base::Value items_val(items->Clone());
    base::JSONWriter::Write(items_val, &items_json);
  }

  std::string row;
  row += csv(date ? *date : "") + ",";
  row += csv(merchant ? *merchant : "") + ",";
  row += (total ? base::NumberToString(*total) : "") + ",";
  row += csv(currency ? *currency : "USD") + ",";
  row += csv(source_url) + ",";
  row += base::NumberToString(static_cast<int64_t>(time(nullptr))) + ",";
  row += csv(items_json) + "\n";

  bool wrote = false;
  {
    ScopedAllowBlockingForMolt allow;
    base::CreateDirectory(dir);
    // Write header if the file is brand new.
    if (!base::PathExists(ledger)) {
      const char* header =
          "date,merchant,total,currency,source_url,saved_at_unix,"
          "items_json\n";
      wrote = base::WriteFile(ledger, header);
    } else {
      wrote = true;
    }
    if (wrote) {
      wrote = base::AppendToFile(ledger, row);
    }
  }
  if (!wrote) {
    out.Set("success", false);
    out.Set("error", "failed to write ledger");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  out.Set("success", true);
  out.Set("path", ledger.AsUTF8Unsafe());
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleSavePlanScript: Tier 3 — turn a list of natural-language step
// lines (from the LLM's /plan output) into a Script and save it via
// AutomationStorage. The parser is intentionally permissive — small
// models are inconsistent about formatting, so we accept any leading
// verb token (navigate / click / type / wait-for / scroll / extract /
// notify) and try to fill out the rest of the Step fields heuristically.
//
// We never auto-run the resulting script — it's saved as CASUAL trust
// so the user has to open the manager UI and click Run. That's the
// safety boundary against an LLM plan that hallucinates a navigation
// to a phishing URL.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleSavePlanScript(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  base::DictValue out;
  if (!args[1].is_dict()) {
    out.Set("success", false);
    out.Set("error", "expected plan dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  const base::DictValue& plan = args[1].GetDict();
  const std::string* name_in = plan.FindString("name");
  const std::string* task_in = plan.FindString("task");
  const base::ListValue* step_lines = plan.FindList("steps");
  if (!step_lines || step_lines->empty()) {
    out.Set("success", false);
    out.Set("error", "no steps");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  molt_ai::automation::Script s;
  s.id = base::StringPrintf("plan-%lx",
                             static_cast<unsigned long>(time(nullptr)));
  s.name = (name_in && !name_in->empty()) ? *name_in
                                          : std::string("Untitled plan");
  s.created_at_unix = static_cast<int64_t>(time(nullptr));
  s.trigger.type = molt_ai::automation::TriggerType::MANUAL;
  s.security.trust = molt_ai::automation::TrustLevel::CASUAL;
  s.security.max_runtime_seconds = 300;
  if (task_in) {
    // Stash the original task description in a default variable so the
    // Manager UI can show "Generated for: <task>".
    s.default_variables["plan_task"] = *task_in;
  }

  auto parse_line = [](const std::string& line) {
    molt_ai::automation::Step step;
    step.description = line;
    step.timeout_ms = 10000;
    // Tokenize on whitespace; first token is the verb.
    std::vector<std::string> parts = base::SplitString(
        line, " ", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if (parts.empty()) return step;
    std::string verb = parts[0];
    std::transform(verb.begin(), verb.end(), verb.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    auto join_from = [&](size_t i) {
      std::string out;
      for (size_t k = i; k < parts.size(); ++k) {
        if (!out.empty()) out += " ";
        out += parts[k];
      }
      return out;
    };
    if (verb == "navigate" || verb == "goto" || verb == "open") {
      step.type = molt_ai::automation::StepType::NAVIGATE;
      step.target = join_from(1);
    } else if (verb == "click") {
      step.type = molt_ai::automation::StepType::CLICK;
      step.target = join_from(1);
    } else if (verb == "type" && parts.size() >= 3) {
      step.type = molt_ai::automation::StepType::TYPE;
      step.target = parts[1];
      step.value = join_from(2);
    } else if (verb == "wait-for" || verb == "waitfor") {
      step.type = molt_ai::automation::StepType::WAIT_FOR;
      step.target = join_from(1);
      step.timeout_ms = 15000;
    } else if (verb == "scroll") {
      step.type = molt_ai::automation::StepType::SCROLL;
      step.value = parts.size() > 1 ? parts[1] : "600";
    } else if (verb == "extract" && parts.size() >= 4) {
      // extract <selector> as <varname>
      step.type = molt_ai::automation::StepType::EXTRACT;
      step.target = parts[1];
      // Find "as" token; varname is what follows.
      for (size_t i = 2; i + 1 < parts.size(); ++i) {
        if (parts[i] == "as" || parts[i] == "AS") {
          step.store_as = parts[i + 1];
          break;
        }
      }
    } else if (verb == "notify") {
      step.type = molt_ai::automation::StepType::NOTIFY;
      step.target = "MoltBrowser";
      step.value = join_from(1);
    } else {
      step.type = molt_ai::automation::StepType::UNKNOWN;
    }
    return step;
  };

  int parsed_ok = 0;
  for (const base::Value& v : *step_lines) {
    if (!v.is_string()) continue;
    molt_ai::automation::Step step = parse_line(v.GetString());
    if (step.type != molt_ai::automation::StepType::UNKNOWN) {
      ++parsed_ok;
    }
    s.steps.push_back(std::move(step));
  }
  if (parsed_ok == 0) {
    out.Set("success", false);
    out.Set("error", "no recognizable steps");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  bool saved = false;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::automation::AutomationStorage storage;
    storage.EnsureDirectory();
    saved = storage.Save(s);
  }
  if (!saved) {
    out.Set("success", false);
    out.Set("error", "AutomationStorage::Save failed");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  out.Set("success", true);
  out.Set("script_id", s.id);
  out.Set("step_count", static_cast<int>(s.steps.size()));
  out.Set("parsed_ok", parsed_ok);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}


// ==================================================================
// Tier 4: Form filler v2 — LLM fallback path.
//
// The existing /fill works by heuristic matching on input
// name/placeholder/autocomplete. When the heuristic misses (sites
// with non-English labels, custom widget libraries, or fields named
// "fld_3"), this path collects the page's visible input attributes,
// sends them to the LLM with the user's profile, and asks the LLM
// to emit a JSON mapping of field selector → profile key. We then
// re-run a filler with that mapping.
//
// This is NOT bypass: if the heuristic finds matches, we still use
// them. The LLM only fills the leftovers. Visible in /fill --ai or
// `runFormFillAI` IPC.
//
// Args: [callback_id]
// ==================================================================

// JS probe that enumerates visible inputs and returns a compact
// summary the LLM can reason over.
static constexpr char kFormProbeJS[] = R"JS(
(function() {
  function isVisible(el) {
    var r = el.getBoundingClientRect();
    if (r.width === 0 || r.height === 0) return false;
    var s = getComputedStyle(el);
    return s.visibility !== 'hidden' && s.display !== 'none';
  }
  function cssPath(el) {
    if (el.id) return '#' + CSS.escape(el.id);
    var n = (el.getAttribute('name') || '').trim();
    if (n) return el.tagName.toLowerCase() + '[name="' +
                 n.replace(/"/g,'\\"') + '"]';
    // Fallback: structural path.
    var parts = [];
    var cur = el;
    for (var i = 0; cur && i < 6; i++) {
      var sib = cur, idx = 1;
      while ((sib = sib.previousElementSibling) != null) {
        if (sib.tagName === cur.tagName) idx++;
      }
      parts.unshift(cur.tagName.toLowerCase() + ':nth-of-type(' + idx + ')');
      cur = cur.parentElement;
    }
    return parts.join(' > ');
  }
  function labelFor(el) {
    if (el.id) {
      var L = document.querySelector('label[for="' + el.id + '"]');
      if (L && L.innerText) return L.innerText.trim();
    }
    var p = el.parentElement;
    if (p && p.tagName === 'LABEL') return p.innerText.trim();
    return '';
  }
  var inputs = Array.from(document.querySelectorAll(
      'input, textarea, select'));
  var rows = [];
  inputs.forEach(function(el) {
    if (!isVisible(el)) return;
    var t = (el.type || '').toLowerCase();
    if (t === 'hidden' || t === 'submit' || t === 'button' ||
        t === 'password') return;
    if (el.disabled || el.readOnly) return;
    rows.push({
      selector: cssPath(el),
      type: t,
      name: el.getAttribute('name') || '',
      id: el.id || '',
      placeholder: el.getAttribute('placeholder') || '',
      autocomplete: el.getAttribute('autocomplete') || '',
      label: labelFor(el).slice(0, 60)
    });
    if (rows.length >= 40) return;
  });
  return JSON.stringify({inputs: rows});
})()
)JS";

// JS shim that applies a {selector: value} map to the active page.
static constexpr char kFormFillByMapJS[] = R"JS(
(function(map) {
  function setNative(el, value) {
    var proto = el.tagName === 'TEXTAREA'
        ? HTMLTextAreaElement.prototype
        : HTMLInputElement.prototype;
    var setter = Object.getOwnPropertyDescriptor(proto, 'value').set;
    setter.call(el, value);
    el.dispatchEvent(new Event('input', {bubbles: true}));
    el.dispatchEvent(new Event('change', {bubbles: true}));
  }
  var filled = 0, total = 0;
  Object.keys(map).forEach(function(sel){
    total++;
    var el;
    try { el = document.querySelector(sel); } catch (e) { return; }
    if (!el) return;
    setNative(el, String(map[sel]));
    filled++;
  });
  return {filled: filled, total: total};
})
)JS";

void MoltAIChatHandler::HandleRunFormFillAI(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  base::DictValue out;

  // Resolve target tab.
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(webui_wc);
  if (!browser || !browser->GetTabStripModel()) {
    out.Set("success", false);
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  content::WebContents* target =
      browser->GetTabStripModel()->GetActiveWebContents();
  if (!target || !target->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    out.Set("success", false);
    out.Set("error", "active tab is not an http(s) page");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Load profile.
  molt_ai::profile::ProfileLoadResult loaded;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::profile::MoltProfileStore store;
    loaded = store.LoadWithStatus();
  }
  base::DictValue profile = std::move(loaded.profile);
  if (profile.empty()) {
    out.Set("success", false);
    out.Set("error",
            loaded.status == molt_ai::profile::ProfileReadStatus::kUnreadable
                ? kProfileUnreadableError
                : "profile is empty — set one via /profile first");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  std::string cb_id = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  base::DictValue profile_copy = profile.Clone();

  // Step 1: enumerate visible form fields.
  target->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(std::string(kFormProbeJS)),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             content::WebContents* target,
             base::DictValue profile,
             base::Value probe_result) {
            if (!self) return;
            base::DictValue out;
            if (!probe_result.is_string()) {
              out.Set("success", false);
              out.Set("error", "form probe returned no data");
              self->ResolveJavascriptCallback(
                  base::Value(cb_id), base::Value(std::move(out)));
              return;
            }
            auto parsed = base::JSONReader::Read(probe_result.GetString(),
                                                  /*options=*/0);
            if (!parsed || !parsed->is_dict()) {
              out.Set("success", false);
              out.Set("error", "probe parse failed");
              self->ResolveJavascriptCallback(
                  base::Value(cb_id), base::Value(std::move(out)));
              return;
            }
            const base::ListValue* inputs =
                parsed->GetDict().FindList("inputs");
            if (!inputs || inputs->empty()) {
              out.Set("success", false);
              out.Set("error", "no fillable inputs found");
              self->ResolveJavascriptCallback(
                  base::Value(cb_id), base::Value(std::move(out)));
              return;
            }

            // Step 2: build LLM prompt. We ask for a strict JSON
            // object mapping CSS selector → string value to type.
            std::string inputs_json;
            base::JSONWriter::Write(*inputs, &inputs_json);
            std::string profile_json;
            base::JSONWriter::Write(profile, &profile_json);
            std::string prompt =
                "You are a web form filler. Below is the user's saved "
                "profile (key → value) and the visible form fields on "
                "the current page. Match each field to a profile value "
                "if and only if the match is unambiguous. Respond with "
                "ONE JSON object: keys are the field selectors, values "
                "are the strings to type. Skip any field you can't "
                "confidently match. No prose, no markdown.\n\n"
                "Profile:\n" + profile_json +
                "\n\nForm fields:\n" + inputs_json +
                "\n\nMapping:";

            // Use the existing sendPrompt pipeline by reaching into the
            // runtime directly. We need the streaming machinery so we
            // get the final text. Simplest path: kick off a normal
            // sendPrompt and let the chat finalize it; the user sees
            // the JSON in the chat, then a follow-up action fills.
            //
            // For a cleaner v1, we instead call the runtime synchronously
            // through GetOrCreateRuntime() and a tiny wrapper. To keep
            // this commit reviewable, route through the existing
            // streaming sendPrompt path: emit the prompt text directly
            // and have the JS side (which already drives /receipt and
            // /plan) own the parse + fill step. That keeps the
            // contract for /fill --ai identical to /receipt: LLM
            // streams JSON, JS parses, JS calls a tiny IPC to apply.
            //
            // Here we just return the prompt + the JS shim so the
            // JS layer can stream the LLM call itself.
            out.Set("success", true);
            out.Set("phase", "ready_for_llm");
            out.Set("prompt", prompt);
            self->ResolveJavascriptCallback(
                base::Value(cb_id), base::Value(std::move(out)));
          },
          weak_this, cb_id, target, std::move(profile_copy)),
      /*world_id=*/1);
}

// HandleApplyFormFillMap: phase 2 of the Form Filler v2. The JS
// layer has already streamed the LLM, parsed its JSON output, and
// now hands us a dict of {selector: value} pairs to apply.
//
// Args: [callback_id, {map: {selector: value, ...}}]
// Returns: {success, filled, total}
void MoltAIChatHandler::HandleApplyFormFillMap(
    const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  base::DictValue out;
  if (!args[1].is_dict()) {
    out.Set("success", false);
    out.Set("error", "expected {map: {...}} dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  const base::DictValue& d = args[1].GetDict();
  const base::DictValue* map = d.FindDict("map");
  if (!map || map->empty()) {
    out.Set("success", false);
    out.Set("error", "empty mapping");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  // Resolve target tab.
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(webui_wc);
  if (!browser || !browser->GetTabStripModel()) {
    out.Set("success", false);
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  content::WebContents* target =
      browser->GetTabStripModel()->GetActiveWebContents();
  if (!target || !target->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    out.Set("success", false);
    out.Set("error", "active tab is not an http(s) page");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  // Serialize the map and apply via the shim.
  std::string map_json;
  base::Value map_val(map->Clone());
  base::JSONWriter::Write(map_val, &map_json);
  std::string js = std::string(kFormFillByMapJS) + "(" + map_json + ")";

  std::string cb_id = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  target->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(js),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             base::Value v) {
            if (!self) return;
            base::DictValue r;
            if (v.is_dict()) {
              const base::DictValue& d = v.GetDict();
              auto filled = d.FindInt("filled");
              auto total = d.FindInt("total");
              r.Set("success", filled.value_or(0) > 0);
              r.Set("filled", filled.value_or(0));
              r.Set("total", total.value_or(0));
            } else {
              r.Set("success", false);
              r.Set("error", "apply script returned no data");
            }
            self->ResolveJavascriptCallback(base::Value(cb_id),
                                            base::Value(std::move(r)));
          },
          weak_this, cb_id),
      /*world_id=*/1);
}

// ==================================================================
// Tier 5: Voice mode
//
// The client side (chat_ui.cc) records audio via WebAudio, encodes
// it as a 16-kHz mono WAV byte string, base64-encodes the bytes, and
// posts here. We decode, hand to VoiceService, and stream the
// transcription back via callback. Audio never leaves the device.
// ==================================================================
void MoltAIChatHandler::HandleTranscribeAudio(
    const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string b64 = args[1].is_string() ? args[1].GetString() : "";
  base::DictValue out;
  if (b64.empty()) {
    out.Set("success", false);
    out.Set("error", "no audio");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  // Pre-decode size cap. The recorder caps at 30 s of 16-kHz mono
  // PCM = ~960 KB raw + WAV header. Base64 inflates by ~4/3 so the
  // worst-case legitimate payload is ~1.3 MB. 8 MB gives generous
  // headroom for a custom uploader without letting a compromised
  // WebUI OOM the browser on a gigabyte string. Code-review MEDIUM #9.
  constexpr size_t kMaxAudioB64Bytes = 8 << 20;
  if (b64.size() > kMaxAudioB64Bytes) {
    out.Set("success", false);
    out.Set("error", "audio exceeds max size (8 MiB base64)");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  std::string wav_bytes;
  if (!base::Base64Decode(b64, &wav_bytes)) {
    out.Set("success", false);
    out.Set("error", "base64 decode failed");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  if (wav_bytes.size() < 44) {
    out.Set("success", false);
    out.Set("error", "WAV too short");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  std::string cb = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  molt_ai::voice::VoiceService::Get()->Transcribe(
      std::move(wav_bytes),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb,
             molt_ai::voice::TranscribeResult r) {
            if (!self) return;
            base::DictValue out;
            out.Set("success", r.success);
            out.Set("text", r.text);
            out.Set("error", r.error);
            out.Set("binary_source", r.binary_source);
            out.Set("binary_path", r.binary_path);
            out.Set("duration_ms", r.duration_ms);
            if (!r.success && r.binary_source == "none") {
              out.Set(
                  "install_hint",
                  "No whisper binary is bundled with this build. "
                  "Run scripts/bundle-whisper.sh, or use a fresh "
                  "release DMG which ships with Whisper inside.");
            }
            self->ResolveJavascriptCallback(base::Value(cb),
                                            base::Value(std::move(out)));
          },
          weak_this, cb));
}

// ------------------------------------------------------------------
// HandleExtractAttachment: extract plain text from a base64-uploaded
// file (chat attachment). A ThreadPool worker decodes it and handles
// what needs no parser (text files, image OCR); PDFs and Office /
// e-book documents go to the sandboxed document parser from the UI
// thread (document_parser_client.h).
// JS: chrome.send('extractAttachment', [callback_id, name, mime, b64])
// Resolves {success, text, truncated, error?}.
// ------------------------------------------------------------------
namespace {

// Decoded-payload cap. Same convention as kMaxAudioB64Bytes (8 MiB)
// and kMaxPdfBytes (25 MB): explicit per-handler caps so a compromised
// WebUI can't OOM the browser process with a giant string.
constexpr size_t kMaxAttachmentBytes = 20 * 1024 * 1024;  // 20 MiB
// Base64 inflates by 4/3, so 28 MiB of b64 bounds a 20 MiB payload.
// Checked pre-decode to reject before the decode allocation.
constexpr size_t kMaxAttachmentB64Bytes = 28u << 20;
// Extracted-text budget shipped back to the chat context.
constexpr size_t kMaxAttachmentChars = 48000;
// What the document parser is asked for: a little over the budget, so
// MakeAttachmentSuccess() still sees a cut (and reports `truncated`) and
// makes it where it would on the whole text. The parser cuts at a UTF-8
// character boundary, which can be up to 3 bytes short of its limit.
constexpr size_t kMaxAttachmentParseChars = kMaxAttachmentChars + 4;

// kRichDoc = Office / e-book / spreadsheet formats handled by the on-device
// `anydoc` crate (structure-preserving -> Markdown). kDocx also routes through
// anydoc but keeps the legacy flat-text scraper as a fallback.
enum class AttachmentKind { kText, kDocx, kRichDoc, kImage, kPdf, kUnsupported };

AttachmentKind DetectAttachmentKind(const std::string& lower_name,
                                    const std::string& lower_mime) {
  if (base::EndsWith(lower_name, ".pdf") ||
      lower_mime == "application/pdf") {
    return AttachmentKind::kPdf;
  }
  if (base::EndsWith(lower_name, ".docx") ||
      lower_mime ==
          "application/vnd.openxmlformats-officedocument."
          "wordprocessingml.document") {
    return AttachmentKind::kDocx;
  }
  // Office / e-book / spreadsheet formats the `anydoc` engine turns into clean
  // Markdown on-device. (docx has its own case above with a scraper fallback.)
  if (base::EndsWith(lower_name, ".pptx") ||
      base::EndsWith(lower_name, ".ppt") ||
      base::EndsWith(lower_name, ".xlsx") ||
      base::EndsWith(lower_name, ".xls") ||
      base::EndsWith(lower_name, ".xlsm") ||
      base::EndsWith(lower_name, ".csv") ||
      base::EndsWith(lower_name, ".odt") ||
      base::EndsWith(lower_name, ".ods") ||
      base::EndsWith(lower_name, ".odp") ||
      base::EndsWith(lower_name, ".rtf") ||
      base::EndsWith(lower_name, ".epub") ||
      base::EndsWith(lower_name, ".doc") ||
      base::StartsWith(
          lower_mime,
          "application/vnd.openxmlformats-officedocument.") ||
      base::StartsWith(lower_mime, "application/vnd.oasis.opendocument.") ||
      base::StartsWith(lower_mime, "application/vnd.ms-") ||
      lower_mime == "text/csv" || lower_mime == "application/rtf" ||
      lower_mime == "text/rtf" || lower_mime == "application/epub+zip" ||
      lower_mime == "application/msword") {
    return AttachmentKind::kRichDoc;
  }
  if (base::EndsWith(lower_name, ".png") ||
      base::EndsWith(lower_name, ".jpg") ||
      base::EndsWith(lower_name, ".jpeg") ||
      base::EndsWith(lower_name, ".webp") ||
      lower_mime == "image/png" || lower_mime == "image/jpeg" ||
      lower_mime == "image/webp") {
    return AttachmentKind::kImage;
  }
  if (base::EndsWith(lower_name, ".txt") ||
      base::EndsWith(lower_name, ".md") ||
      base::EndsWith(lower_name, ".markdown") ||
      base::StartsWith(lower_mime, "text/")) {
    return AttachmentKind::kText;
  }
  return AttachmentKind::kUnsupported;
}

// Temp-file extension for the tesseract input. Tesseract sniffs the
// actual content via Leptonica, so this only needs to be plausible.
std::string AttachmentImageExtension(const std::string& lower_name,
                                     const std::string& lower_mime) {
  if (lower_mime == "image/png" || base::EndsWith(lower_name, ".png")) {
    return "png";
  }
  if (lower_mime == "image/webp" || base::EndsWith(lower_name, ".webp")) {
    return "webp";
  }
  return "jpg";  // image/jpeg, .jpg, .jpeg
}

// Truncate |*text| to at most |max_chars| bytes, preferring a
// whitespace boundary. Returns true when a cut was made. The hard-cut
// fallback backs off over UTF-8 continuation bytes so the result stays
// a valid UTF-8 string (base::Value requires it).
bool TruncateAtWhitespaceBoundary(std::string* text, size_t max_chars) {
  if (text->size() <= max_chars) {
    return false;
  }
  size_t cut = text->find_last_of(" \t\n\r", max_chars);
  if (cut == std::string::npos || cut < max_chars / 2) {
    cut = max_chars;
    while (cut > 0 &&
           (static_cast<unsigned char>((*text)[cut]) & 0xC0) == 0x80) {
      --cut;
    }
  }
  text->resize(cut);
  return true;
}

base::DictValue MakeAttachmentError(const std::string& error) {
  base::DictValue out;
  out.Set("success", false);
  out.Set("text", "");
  out.Set("truncated", false);
  out.Set("error", error);
  return out;
}

base::DictValue MakeAttachmentSuccess(std::string text) {
  const bool truncated =
      TruncateAtWhitespaceBoundary(&text, kMaxAttachmentChars);
  base::DictValue out;
  out.Set("success", true);
  out.Set("truncated", truncated);
  out.Set("text", std::move(text));
  return out;
}

// OCR an image via tesseract. Reuses OcrService's public binary
// resolution (bundled Resources/ocr first, then system paths) and its
// sandboxed invocation (molt_ai::ocr::RunSandboxedTesseract) — the
// service's only entry point is OcrPdf, which hard-codes an input.pdf temp
// name; images just need a different extension (and are tesseract's native
// input). Runs on a MayBlock worker.
base::DictValue OcrImageBlocking(const std::string& image_bytes,
                                 const std::string& ext) {
  auto* svc = molt_ai::ocr::OcrService::Get();
  const base::FilePath bin = svc->ResolveTesseractBinary();
  if (bin.value().empty()) {
    return MakeAttachmentError(
        "no OCR binary available — this build doesn't bundle tesseract "
        "(scripts/bundle-tesseract.sh); on Linux, install the "
        "tesseract-ocr package.");
  }
  base::FilePath tessdata;
  if (svc->IsUsingBundledTesseract()) {
    tessdata = svc->GetBundledOcrDir();
  }
  // Private 0700 scratch dir — same symlink-race rationale as the OCR
  // service's RunTesseractBlocking.
  base::FilePath scratch_dir;
  if (!base::CreateNewTempDirectory(FILE_PATH_LITERAL("molt_attach_ocr"),
                                    &scratch_dir)) {
    return MakeAttachmentError("could not create scratch directory");
  }
  const base::FilePath named = scratch_dir.AppendASCII("input." + ext);
  if (!base::WriteFile(named, image_bytes)) {
    base::DeletePathRecursively(scratch_dir);
    return MakeAttachmentError("could not write temp image");
  }
  // Sandboxed, with a deadline (helper_sandbox.h).
  const base::FilePath out_stem = named.RemoveExtension();
  const molt_ai::HelperRunResult run = molt_ai::ocr::RunSandboxedTesseract(
      bin, tessdata, scratch_dir, named, out_stem,
      molt_ai::ocr::kImageOcrTimeout);
  const std::string failure = molt_ai::DescribeHelperRunFailure(
      run, "tesseract", "failed to launch tesseract");
  if (!failure.empty()) {
    base::DeletePathRecursively(scratch_dir);
    return MakeAttachmentError(failure);
  }
  // Only a plain file tesseract made itself.
  std::string text;
  molt_ai::ReadHelperOutputFile(out_stem.AddExtensionASCII(".txt"),
                                molt_ai::ocr::kMaxTesseractTextFileBytes,
                                &text);
  base::DeletePathRecursively(scratch_dir);
  if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
    return MakeAttachmentError("OCR returned no text");
  }
  // tesseract read an untrusted image: valid UTF-8, cut like the parser's.
  return MakeAttachmentSuccess(
      molt_ai::SanitizeHelperText(text, kMaxAttachmentParseChars));
}

// The extension of a lowercased file name, without the dot: anydoc's
// format hint (it sniffs the content first regardless).
std::string AttachmentExtension(const std::string& lower_name) {
  const size_t dot = lower_name.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= lower_name.size()) {
    return std::string();
  }
  return lower_name.substr(dot + 1);
}

// What the decode worker hands back to the UI thread: the final answer,
// or a document for the sandboxed parser.
struct PreparedAttachment {
  // Set when no parser is needed: text files, image OCR, errors.
  std::optional<base::DictValue> result;
  // Otherwise kPdf, kDocx or kRichDoc, with the decoded file.
  AttachmentKind kind = AttachmentKind::kUnsupported;
  std::unique_ptr<std::string> bytes;
  std::string extension;
};

PreparedAttachment AttachmentAnswer(base::DictValue result) {
  PreparedAttachment prepared;
  prepared.result = std::move(result);
  return prepared;
}

// Runs on a MayBlock ThreadPool worker: decodes the attachment and
// answers what needs no document parser. PDFs and Office / e-book
// documents go back to the UI thread for the sandboxed parser
// (MoltAIChatHandler::ParseAttachmentPdf / ParseAttachmentDocument).
PreparedAttachment PrepareAttachmentBlocking(const std::string& name,
                                             const std::string& mime,
                                             const std::string& b64) {
  std::string bytes;
  if (!base::Base64Decode(b64, &bytes)) {
    return AttachmentAnswer(MakeAttachmentError("base64 decode failed"));
  }
  if (bytes.empty()) {
    return AttachmentAnswer(MakeAttachmentError("empty attachment"));
  }
  if (bytes.size() > kMaxAttachmentBytes) {
    return AttachmentAnswer(
        MakeAttachmentError("attachment exceeds max size (20 MiB)"));
  }
  const std::string lower_name = base::ToLowerASCII(name);
  const std::string lower_mime = base::ToLowerASCII(mime);
  const AttachmentKind kind = DetectAttachmentKind(lower_name, lower_mime);
  switch (kind) {
    case AttachmentKind::kText: {
      if (!base::IsStringUTF8(bytes)) {
        return AttachmentAnswer(
            MakeAttachmentError("text file is not valid UTF-8"));
      }
      return AttachmentAnswer(MakeAttachmentSuccess(std::move(bytes)));
    }
    case AttachmentKind::kImage: {
      return AttachmentAnswer(OcrImageBlocking(
          bytes, AttachmentImageExtension(lower_name, lower_mime)));
    }
    case AttachmentKind::kPdf:
    case AttachmentKind::kDocx:
    case AttachmentKind::kRichDoc: {
      PreparedAttachment prepared;
      prepared.kind = kind;
      prepared.bytes = std::make_unique<std::string>(std::move(bytes));
      prepared.extension = AttachmentExtension(lower_name);
      return prepared;
    }
    case AttachmentKind::kUnsupported:
      break;
  }
  return AttachmentAnswer(MakeAttachmentError(
      "unsupported attachment type (mime='" + mime +
      "') — supported: .txt/.md, .pdf, Office & e-book docs "
      "(.docx/.pptx/.xlsx/.csv/.odt/.ods/.odp/.rtf/.epub), and "
      ".png/.jpg/.jpeg/.webp images"));
}

}  // namespace

void MoltAIChatHandler::HandleExtractAttachment(
    const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 4u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string name =
      args[1].is_string() ? args[1].GetString() : std::string();
  const std::string mime =
      args[2].is_string() ? args[2].GetString() : std::string();
  const std::string b64 =
      args[3].is_string() ? args[3].GetString() : std::string();
  if (b64.empty()) {
    ResolveJavascriptCallback(
        base::Value(callback_id),
        base::Value(MakeAttachmentError("no attachment data")));
    return;
  }
  // Pre-decode size cap — reject before the decode allocation, same
  // rationale as kMaxAudioB64Bytes in HandleTranscribeAudio.
  if (b64.size() > kMaxAttachmentB64Bytes) {
    ResolveJavascriptCallback(
        base::Value(callback_id),
        base::Value(
            MakeAttachmentError("attachment exceeds max size (20 MiB)")));
    return;
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&PrepareAttachmentBlocking, name, mime, b64),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             PreparedAttachment prepared) {
            if (!self) {
              return;
            }
            if (prepared.result) {
              self->ResolveJavascriptCallback(
                  base::Value(cb_id),
                  base::Value(std::move(*prepared.result)));
              return;
            }
            if (prepared.kind == AttachmentKind::kPdf) {
              self->ParseAttachmentPdf(cb_id, std::move(prepared.bytes));
              return;
            }
            self->ParseAttachmentDocument(
                cb_id, std::move(prepared.bytes), prepared.extension,
                /*is_docx=*/prepared.kind == AttachmentKind::kDocx);
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id));
}

// Same two-tier route as /pdf (OnPdfTextLayer): the text layer from the
// sandboxed parser, tesseract OCR for a scan.
void MoltAIChatHandler::ParseAttachmentPdf(const std::string& callback_id,
                                           std::unique_ptr<std::string> pdf) {
  // The client copies |bytes| before returning; moving the unique_ptr
  // doesn't move the string it points to.
  const base::span<const uint8_t> bytes = base::as_byte_span(*pdf);
  document_parser_.ExtractPdfText(
      bytes, kMaxAttachmentParseChars,
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             std::unique_ptr<std::string> pdf,
             molt_ai::DocumentParseResult r) {
            if (!self) {
              return;
            }
            using Status = molt_ai::DocumentParseResult::Status;
            if (r.status == Status::kOk) {
              self->ResolveJavascriptCallback(
                  base::Value(cb_id),
                  base::Value(MakeAttachmentSuccess(std::move(r.text))));
              return;
            }
            if (r.status == Status::kFailed) {
              // The parser crashed, hung or misbehaved on this document:
              // never hand it on to the OCR helper too (its sandbox is
              // looser, and on Windows confines no files).
              self->ResolveJavascriptCallback(
                  base::Value(cb_id),
                  base::Value(MakeAttachmentError(
                      "couldn't read this PDF: " + r.error)));
              return;
            }
            // PDF with no text layer — same OCR fallback as /pdf.
            molt_ai::ocr::OcrService::Get()->OcrPdf(
                std::move(*pdf), kMaxAttachmentChars + 1,
                base::BindOnce(
                    [](base::WeakPtr<MoltAIChatHandler> self,
                       std::string cb_id, molt_ai::ocr::OcrResult r) {
                      if (!self) {
                        return;
                      }
                      base::DictValue out;
                      if (r.success) {
                        out = MakeAttachmentSuccess(std::move(r.text));
                      } else if (r.binary_source == "none") {
                        out = MakeAttachmentError(
                            "PDF has no text layer and no OCR is "
                            "available — run scripts/bundle-tesseract.sh "
                            "or install tesseract.");
                      } else {
                        out = MakeAttachmentError(
                            "PDF has no text layer; OCR failed: " +
                            r.error);
                      }
                      self->ResolveJavascriptCallback(
                          base::Value(cb_id),
                          base::Value(std::move(out)));
                    },
                    self, std::move(cb_id)));
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id, std::move(pdf)));
}

// Office / e-book / spreadsheet / CSV -> Markdown with anydoc (tables,
// headings and lists preserved), on-device in the sandboxed parser. A
// .docx anydoc can't read falls back to the flat-text extractor.
void MoltAIChatHandler::ParseAttachmentDocument(
    const std::string& callback_id,
    std::unique_ptr<std::string> document,
    const std::string& extension,
    bool is_docx) {
  const base::span<const uint8_t> bytes = base::as_byte_span(*document);
  document_parser_.ConvertToMarkdown(
      bytes, extension, kMaxAttachmentParseChars,
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             bool is_docx, std::unique_ptr<std::string> document,
             molt_ai::DocumentParseResult r) {
            if (!self) {
              return;
            }
            if (r.status == molt_ai::DocumentParseResult::Status::kOk) {
              self->ResolveJavascriptCallback(
                  base::Value(cb_id),
                  base::Value(MakeAttachmentSuccess(std::move(r.text))));
              return;
            }
            if (is_docx) {
              // Also after a crash or timeout: the fallback runs in a
              // fresh sandboxed process.
              self->ParseAttachmentDocxText(cb_id, std::move(document),
                                            std::move(r.error));
              return;
            }
            self->ResolveJavascriptCallback(
                base::Value(cb_id),
                base::Value(MakeAttachmentError(
                    r.error.empty() ? "couldn't read this document on-device"
                                    : r.error)));
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id, is_docx,
          std::move(document)));
}

void MoltAIChatHandler::ParseAttachmentDocxText(
    const std::string& callback_id,
    std::unique_ptr<std::string> docx,
    std::string anydoc_error) {
  document_parser_.ExtractDocxText(
      base::as_byte_span(*docx), kMaxAttachmentParseChars,
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             std::string anydoc_error, molt_ai::DocumentParseResult r) {
            if (!self) {
              return;
            }
            self->ResolveJavascriptCallback(
                base::Value(cb_id),
                base::Value(
                    r.status == molt_ai::DocumentParseResult::Status::kOk
                        ? MakeAttachmentSuccess(std::move(r.text))
                        : MakeAttachmentError(r.error.empty() ? anydoc_error
                                                              : r.error)));
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id,
          std::move(anydoc_error)));
}

// ------------------------------------------------------------------
// HandleStartWebAgent — launch a ReAct-loop web browsing agent.
//
// JS calls: chrome.send('startWebAgent', [callback_id, goal])
//   - Each completed step fires: FireWebUIListener('agent-step', {...})
//   - A sensitive action (new origin, form submit, payment / one-time-code
//     field) pauses the run and fires FireWebUIListener('agent-confirm',
//     {id, action, message}); the UI answers via 'webAgentConfirm'.
//   - Final result resolves: ResolveJavascriptCallback(callback_id, {...})
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleStartWebAgent(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;

  const std::string callback_id = args[0].GetString();
  const std::string goal =
      molt_ai::chat_handler_helpers::StringArg(args, 1);

  if (goal.empty()) {
    base::DictValue err;
    err.Set("success", false);
    err.Set("result", "Goal cannot be empty");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(err)));
    return;
  }

  // Find the active browsing tab (not the side-panel WebContents itself).
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(webui_wc);
  content::WebContents* target_wc = nullptr;
  if (browser) {
    target_wc = browser->GetTabStripModel()->GetActiveWebContents();
  }
  if (!target_wc) {
    base::DictValue err;
    err.Set("success", false);
    err.Set("result", "No active browser tab found");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(err)));
    return;
  }

  // Cancel any previous agent run.
  if (web_agent_) web_agent_->Cancel();

  molt_ai::BrowserAIRuntime* runtime = GetOrCreateRuntime();
  web_agent_ = std::make_unique<molt_ai::WebAgent>(target_wc, runtime);

  auto weak_this = weak_ptr_factory_.GetWeakPtr();

  // Step callback: stream each completed step to the UI in real time.
  auto on_step = base::BindRepeating(
      [](base::WeakPtr<MoltAIChatHandler> self,
         const molt_ai::WebAgent::Step& step) {
        if (!self || !self->IsJavascriptAllowed()) return;
        base::DictValue d;
        d.Set("number",      step.number);
        d.Set("action",      step.action);
        d.Set("target",      step.target);
        d.Set("value",       step.value);
        d.Set("reason",      step.reason);
        d.Set("observation", step.observation.substr(
                                 0, std::min<int>(500,
                                    static_cast<int>(step.observation.size()))));
        d.Set("success",     step.success);
        d.Set("note",        step.note);
        self->FireWebUIListener("agent-step", base::Value(std::move(d)));
      },
      weak_this);

  // Confirm callback: the run stays paused until the user answers the
  // Allow / Deny chip (HandleWebAgentConfirm). If the panel goes away the
  // agent is destroyed with this handler, so nothing runs unapproved.
  auto on_confirm = base::BindRepeating(
      [](base::WeakPtr<MoltAIChatHandler> self,
         const molt_ai::WebAgent::ConfirmRequest& req) {
        if (!self || !self->IsJavascriptAllowed()) return;
        base::DictValue d;
        d.Set("id", req.id);
        d.Set("action", req.action);
        d.Set("message", req.message);
        self->FireWebUIListener("agent-confirm", base::Value(std::move(d)));
      },
      weak_this);

  // Done callback: resolve the JS promise with the final result.
  auto on_done = base::BindOnce(
      [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
         bool success, std::string result) {
        if (!self || !self->IsJavascriptAllowed()) return;
        self->web_agent_.reset();
        base::DictValue d;
        d.Set("success", success);
        d.Set("result",  result);
        self->ResolveJavascriptCallback(base::Value(cb_id),
                                        base::Value(std::move(d)));
      },
      weak_this, callback_id);

  web_agent_->Start(goal, std::move(on_step), std::move(on_done),
                    std::move(on_confirm));

  // The goal can contain personal data; keep it out of release logs.
  DVLOG(1) << "[MoltAI] Web agent started";
}

// ------------------------------------------------------------------
// HandleCancelWebAgent — stop a running agent immediately.
// JS calls: chrome.send('cancelWebAgent', [])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleCancelWebAgent(const base::ListValue& args) {
  if (web_agent_) {
    web_agent_->Cancel();
    web_agent_.reset();
    LOG(INFO) << "[MoltAI] Web agent cancelled by user";
  }
}

// ------------------------------------------------------------------
// HandleWebAgentConfirm — the user's Allow / Deny on an 'agent-confirm'
// chip. Stale ids (a previous run, an already-answered request) are
// ignored by WebAgent::ResolveConfirmation.
// JS calls: chrome.send('webAgentConfirm', [request_id, allowed])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleWebAgentConfirm(const base::ListValue& args) {
  if (!web_agent_ || args.size() < 2) return;
  int request_id = 0;
  if (args[0].is_int()) {
    request_id = args[0].GetInt();
  } else if (args[0].is_double()) {
    request_id = static_cast<int>(args[0].GetDouble());
  } else {
    return;
  }
  // Anything but an explicit boolean true is a denial.
  const bool allowed = args[1].is_bool() && args[1].GetBool();
  web_agent_->ResolveConfirmation(request_id, allowed);
}
