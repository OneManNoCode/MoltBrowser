// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltAIChatHandler: WebUIMessageHandler that bridges the AI chat WebUI
// JavaScript frontend to BrowserAIRuntime for local LLM inference.
//
// JS → C++:  chrome.send('sendPrompt', [callback_id, prompt_text])
//            chrome.send('loadModel', [callback_id, model_id])
//            chrome.send('cancelGeneration', [])
//            chrome.send('getModelStatus', [callback_id])
//            chrome.send('initChat', [callback_id])
//
// C++ → JS:  FireWebUIListener('ai-token', {token, is_done, ...})
//            ResolveJavascriptCallback(callback_id, result)

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_HANDLER_H_

#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "content/public/browser/web_ui_message_handler.h"

class Profile;

namespace network {
class SimpleURLLoader;
}  // namespace network

class MoltAIChatHandler : public content::WebUIMessageHandler {
 public:
  explicit MoltAIChatHandler(Profile* profile);
  ~MoltAIChatHandler() override;

  MoltAIChatHandler(const MoltAIChatHandler&) = delete;
  MoltAIChatHandler& operator=(const MoltAIChatHandler&) = delete;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

 private:
  // Message handlers (called from JavaScript via chrome.send)
  void HandleInitChat(const base::ListValue& args);
  void HandleSendPrompt(const base::ListValue& args);
  void HandleLoadModel(const base::ListValue& args);
  void HandleCancelGeneration(const base::ListValue& args);
  void HandleGetModelStatus(const base::ListValue& args);
  void HandleGetPageContext(const base::ListValue& args);
  void HandleDownloadModel(const base::ListValue& args);
  void HandleDeleteModel(const base::ListValue& args);
  void HandleGetPageContent(const base::ListValue& args);
  void HandleCancelDownload(const base::ListValue& args);
  void HandleExportHistory(const base::ListValue& args);
  // Side panel automation bridge. Runs a single click/type/scroll/
  // navigate against the currently-active tab in the Browser that
  // owns this WebUI's WebContents.
  void HandleRunMoltAction(const base::ListValue& args);
  // Personal Vector Memory lookup — returns top-K relevant chunks
  // from the user's full browsing history so the chat can ground
  // answers in things the user has actually read before.
  void HandleQueryMemory(const base::ListValue& args);
  // Tab Triage — enumerate the owning Browser's tabs with snippets.
  void HandleListTabsInWindow(const base::ListValue& args);
  // Tab Triage — bulk close / bookmark / pin on a list of tab ids.
  void HandleTriageActOnTabs(const base::ListValue& args);
  // Page Watchers — create a scheduled Script that periodically
  // extracts a selector and fires a notification on change/threshold.
  void HandleCreateWatcher(const base::ListValue& args);
  // Agent Inbox — list currently-running background automation runs.
  void HandleListActiveAgents(const base::ListValue& args);
  // PDF chat — download the PDF at |url|, extract text, return.
  // Args: [callback_id, url]
  void HandleExtractPdfText(const base::ListValue& args);
  // Smart bookmarks — keyword-rank the user's bookmarks against |query|.
  // Args: [callback_id, query, top_k]
  void HandleSearchBookmarks(const base::ListValue& args);
  // Cross-tab Q&A — pull innerText from every tab in the owning Browser.
  // Args: [callback_id, max_chars_per_tab]
  void HandleExtractAllTabsText(const base::ListValue& args);
  // Sandbox tab — open |url| in an off-the-record window so cookies and
  // storage are discarded when the window closes.
  // Args: [callback_id, url]
  void HandleOpenSandboxTab(const base::ListValue& args);
  // Privacy heatmap — enumerate third-party resources loaded into the
  // active tab and bucket them into ads / analytics / cdn / other.
  // Args: [callback_id]
  void HandleGetTrackerBreakdown(const base::ListValue& args);
  // Domain reputation — return visit count / first / last for the host
  // of the active tab (or |args[1]| if supplied) from MemoryService.
  // Args: [callback_id, host?]
  void HandleGetDomainReputation(const base::ListValue& args);
  // Connection path — for a URL, return what we can show about how the
  // user's traffic gets there. v0: just the URL structure and whether
  // the tab is in an OTR session. Foundation for Tor circuit display.
  // Args: [callback_id, url?]
  void HandleGetConnectionPath(const base::ListValue& args);
  // Selective JS off — toggle JavaScript content-setting for |host|.
  // Args: [callback_id, host, enabled_bool]
  void HandleSetJsForDomain(const base::ListValue& args);
  // Tor (Phase B.1): probe local Tor on 127.0.0.1:9051 and read
  // current circuit info from its control port. Routing through Tor
  // ships in Phase B.2 — this batch is the visualization foundation
  // (talk to Tor, show real circuits) plus the response shape that
  // routing will plug into without UI changes.
  // Args: [callback_id]
  void HandleGetTorStatus(const base::ListValue& args);
  void HandleGetTorCircuits(const base::ListValue& args);
  // Phase B.2: launch a locally-installed tor binary as a child
  // process of MoltBrowser, with a managed torrc.
  // Args: [callback_id]
  void HandleLaunchTor(const base::ListValue& args);
  // Phase B.2: stop the managed tor child.
  // Args: [callback_id]
  void HandleStopTor(const base::ListValue& args);
  // Phase B.2: open |url| in an OTR window whose SOCKS5 proxy is set
  // to route through the local Tor instance on 127.0.0.1:9050. All
  // tabs in that OTR profile share the proxy.
  // Args: [callback_id, url]
  void HandleOpenTorTab(const base::ListValue& args);
  // Tier 3: Receipt extractor — append a parsed receipt dict to
  // ~/.moltbrowser/ledger.csv. The LLM JSON parsing lives in JS; we
  // just write the CSV row here so the file lifecycle is in one
  // place.
  // Args: [callback_id, receipt_dict, source_url]
  void HandleAppendReceipt(const base::ListValue& args);
  // Tier 3: Plan-a-task — turn a list of natural-language step lines
  // into a saved Script, written via AutomationStorage.
  // Args: [callback_id, {name, steps: [string], task}]
  void HandleSavePlanScript(const base::ListValue& args);
  // Tier 4: Password vault.
  //   vaultList(callback_id, [include_passwords_bool=false])
  //   vaultAdd(callback_id, {site_host, username, password, notes?})
  //   vaultDelete(callback_id, id)
  //   vaultFindForActive(callback_id)  → entries matching active tab
  //   vaultAutofill(callback_id, id)   → inject creds into active tab
  void HandleVaultList(const base::ListValue& args);
  void HandleVaultAdd(const base::ListValue& args);
  void HandleVaultDelete(const base::ListValue& args);
  void HandleVaultFindForActive(const base::ListValue& args);
  void HandleVaultAutofill(const base::ListValue& args);
  // Tier 4: Form filler v2 — LLM fallback. Two-phase:
  //   1. HandleRunFormFillAI: probe visible form fields, return a
  //      ready-to-send prompt for the LLM.
  //   2. HandleApplyFormFillMap: take the LLM's parsed mapping
  //      (selector → value) and apply it to the active tab.
  void HandleRunFormFillAI(const base::ListValue& args);
  void HandleApplyFormFillMap(const base::ListValue& args);
  // Form Filler — load/save the encrypted local profile blob.
  // Args (load): [callback_id]
  // Args (save): [callback_id, dict]
  void HandleGetMoltProfile(const base::ListValue& args);
  void HandleSaveMoltProfile(const base::ListValue& args);
  // Form Filler — execute autofill on the active tab. The matching
  // logic lives in injected JS; this handler only forwards the
  // profile dict and reports back affected_field_count.
  // Args: [callback_id]
  void HandleRunFormFill(const base::ListValue& args);
  // AI-grouped history — read up to N recent docs from MemoryService
  // and ship back {docs:[{url,title,visited_at,word_count}], ...}.
  // The JS side does the clustering so it can re-cluster on filter
  // without a round-trip.
  // Args: [callback_id, limit?]
  void HandleListMemoryDocs(const base::ListValue& args);

  // Async callbacks (run on UI thread after background work)
  void FinishInitChat(std::string callback_id,
                      base::DictValue settings_dict);
  void OnModelLoaded(std::string callback_id, bool success,
                     const std::string& error);
  void OnPromptComplete(std::string callback_id, bool success,
                        const std::string& full_text,
                        const std::string& error);
  void OnDownloadProgress(uint64_t current);
  void OnDownloadComplete(base::FilePath path);
  void OnDownloadPrecheckComplete(std::string callback_id,
                                  std::string model_id,
                                  molt_ai::ModelInfo info,
                                  base::DictValue precheck);
  void FinishDownload(bool success, int net_error);
  void OnModelDeleted(std::string callback_id, std::string model_id,
                      bool success);
  void OnHistoryExported(std::string callback_id,
                         base::FilePath file_path,
                         std::string filename,
                         bool success);
  void OnPageContentExtracted(std::string callback_id, base::Value result);

  // Ensure BrowserAIRuntime is created and initialized
  molt_ai::BrowserAIRuntime* GetOrCreateRuntime();

  raw_ptr<Profile> profile_;
  std::unique_ptr<molt_ai::BrowserAIRuntime> runtime_;
  bool model_loaded_ = false;
  std::string active_prompt_callback_id_;

  // Model download state
  std::unique_ptr<network::SimpleURLLoader> url_loader_;
  // PDF extraction transient state (separate loader so it doesn't race
  // with a concurrent model download).
  std::unique_ptr<network::SimpleURLLoader> pdf_loader_;
  std::string pdf_callback_id_;
  std::string pdf_source_url_;
  std::string download_callback_id_;
  std::string downloading_model_id_;
  uint64_t download_total_bytes_ = 0;
  uint64_t download_resume_bytes_ = 0;
  base::FilePath download_final_path_;
  base::TimeTicks download_start_time_;
  uint64_t download_last_bytes_ = 0;
  base::TimeTicks download_last_time_;

  base::WeakPtrFactory<MoltAIChatHandler> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_HANDLER_H_
