// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltAIChatHandler: WebUIMessageHandler that bridges the AI chat WebUI
// JavaScript frontend to BrowserAIRuntime for local LLM inference.
//
// JS → C++:  chrome.send('sendPrompt',
//                [callback_id, prompt_text, history_json?, page_ctx?])
//            (history_json: JSON array of {role, content} objects)
//            chrome.send('loadModel', [callback_id, model_id])
//            chrome.send('cancelGeneration', [])
//            chrome.send('getModelStatus', [callback_id])
//            chrome.send('initChat', [callback_id])
//            chrome.send('proposeChatAction', [callback_id, action_request])
//            chrome.send('runChatProposal', [callback_id, id, confirmed])
//            chrome.send('proposeFormFill',
//                [callback_id, source, hint, pasted_text])
//
// C++ → JS:  FireWebUIListener('ai-token', {token, is_done, ...})
//            ResolveJavascriptCallback(callback_id, result)

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_HANDLER_H_

#include <cstdint>  // for uint64_t — required on Linux under -fmodules
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/agents/chat_actions.h"
#include "chrome/browser/molt_ai/agents/web_agent.h"
#include "chrome/browser/molt_ai/automation/agent_run_draft.h"
#include "chrome/browser/molt_ai/cloud/cloud_provider.h"
#include "chrome/browser/molt_ai/document_parser/document_parser_client.h"
#include "chrome/browser/molt_ai/models/model_download_job.h"
#include "chrome/browser/molt_ai/models/model_download_plan.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_form_fill.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "url/origin.h"

class BrowserWindowInterface;
class Profile;

namespace content {
class RenderFrameHost;
}  // namespace content

namespace molt_ai::memory {
class ConversationStore;
}  // namespace molt_ai::memory

namespace url {
class Origin;
}  // namespace url

namespace network {
class SharedURLLoaderFactory;
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
  // Record first-use consent for a cloud provider (P18).
  // Args: [callback_id, provider_id] -> {success}
  void HandleGrantCloudConsent(const base::ListValue& args);
  void HandleGetModelStatus(const base::ListValue& args);
  void HandleGetPageContext(const base::ListValue& args);
  void HandleDownloadModel(const base::ListValue& args);
  // Args: [callback_id, model_id, license_id] -> {success}
  void HandleAcceptModelLicense(const base::ListValue& args);
  void HandleDeleteModel(const base::ListValue& args);
  void HandleGetPageContent(const base::ListValue& args);
  void HandleCancelDownload(const base::ListValue& args);
  void HandleExportHistory(const base::ListValue& args);
  // Conversation store (sidebar Recents): per-profile, OSCrypt-encrypted
  // (molt_ai::memory::ConversationStore); nothing is read or written for
  // off-the-record profiles.
  //   listConversations(callback_id)         → {success, conversations:[…]}
  //   saveConversation(callback_id, id, title, history_json)
  //   loadConversation(callback_id, id)      → {success, id, title,
  //                                             history_json}
  //   deleteConversation(callback_id, id)    → {success, error?}
  void HandleListConversations(const base::ListValue& args);
  void HandleSaveConversation(const base::ListValue& args);
  void HandleLoadConversation(const base::ListValue& args);
  void HandleDeleteConversation(const base::ListValue& args);
  // Side panel automation bridge for the commands the USER types
  // (/click, /type, /scroll, /navigate, ... and "open nasa.com"). Runs a
  // single click/type/scroll/navigate against the currently-active tab in
  // the Browser that owns this WebUI's WebContents. Model output never
  // reaches it: the model's page actions are proposals (below).
  void HandleRunMoltAction(const base::ListValue& args);
  // ---- Page actions the model proposes (molt_ai_chat_handler_actions.cc,
  // molt_ai/agents/chat_actions.h, docs/design/AGENT_ACTIONS.md §4) -----
  // After the text reply to a typed message that asks to act on the page,
  // whose sendPrompt result carried `action_request`, the page asks for
  // one proposed action. The browser observes the active tab of the
  // chat's window (never across MoltNet's boundary), asks the on-device
  // model for one action under a grammar, validates it, and probes the
  // elements it touches.
  // Args: [callback_id, action_request]
  //   -> {status: 'proposal', id, verb, summary, reason}
  //    | {status: 'none' | 'cancelled'}
  //    | {status: 'unavailable' | 'failed', message}
  // |summary| is the browser's description; |reason| (may be "") what the
  // user must approve first, even in Auto mode.
  void HandleProposeChatAction(const base::ListValue& args);
  // Runs proposal |id| on the document it was proposed for, after probing
  // the page again. A proposal runs once; a newer message or proposal
  // replaces it.
  // Args: [callback_id, id, confirmed]
  //   -> {success, message} | {success: false, error}
  //    | {success: false, needs_confirmation: true, reason}
  // |confirmed| is true only after the user's Allow; it approves the
  // reason last shown for this proposal, nothing else.
  void HandleRunChatProposal(const base::ListValue& args);
  // /fill ai and /paste (molt_ai_chat_form_fill.h, AGENT_ACTIONS.md §4.7):
  // the same machinery for a form fill. The browser observes the page,
  // asks the on-device model for one FILL_FORM of its text fields (never a
  // password field) with the saved profile (read here, never sent by the
  // page) or the pasted text as data, validates it, and proposes it with
  // its own description of every value. It always asks first; Allow runs
  // it through runChatProposal like any proposal.
  // Args: [callback_id, source ('profile' | 'paste'), hint, pasted_text]
  //   -> as proposeChatAction, and {status: 'none', message} when nothing
  //      in the data fits a field.
  void HandleProposeFormFill(const base::ListValue& args);
  // Runs one of HandleRunMoltAction's direct-JS actions (select / hover /
  // right-click / drag) in |target| and resolves |callback_id|.
  void RunDirectPageAction(const std::string& callback_id,
                           const std::string& type,
                           const std::string& js,
                           content::WebContents* target);
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
  // /pdf continuation: the sandboxed parser's answer for the text layer;
  // falls back to OCR when there is none.
  void OnPdfTextLayer(std::string callback_id,
                      std::string source_url,
                      std::unique_ptr<std::string> pdf,
                      molt_ai::DocumentParseResult result);
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
  // Open molt://ai-settings/ in a new foreground tab of the browser
  // that owns this WebUI. Falls back to the last-active browser when
  // hosted in the side panel (whose WebContents is not a tab, so
  // window.open there is silently dropped by views::WebView).
  // Args: [callback_id] → {success, error?}
  void HandleOpenMoltSettings(const base::ListValue& args);
  // Close the AI chat side panel from inside the WebUI. The panel has no
  // native header/close button (the page provides its own in-page close
  // control), so this asks the SidePanelUI to close the kContent panel.
  // Fire-and-forget — closing tears down this WebUI. Args: []
  void HandleCloseSidePanel(const base::ListValue& args);
  // Open an http(s) link from a chat response in a real browser tab.
  void HandleOpenUrlInTab(const base::ListValue& args);
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
  // MoltNet (Tor): status/circuits of MoltNet's own Tor, read over its
  // SAFECOOKIE-authenticated control port (see molt_ai_chat_handler_tor.cc).
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
  // Open |url| in a MoltNet window (the isolated, Tor-routed profile;
  // see molt_ai/tor/molt_net_window.h).
  // Args: [callback_id, url]
  void HandleOpenTorTab(const base::ListValue& args);
  // Exit-country selector (VPN-style exit relay control). Returns the
  // currently-selected exit country and the curated list of available
  // codes mapped to display names:
  //   { selected: "<cc or ''>",
  //     available: [ {code, name}, ... ] }
  // Args: [callback_id]
  void HandleGetTorExitCountries(const base::ListValue& args);
  // Constrain (or clear, when "") the Tor exit relay's country. Calls
  // TorManager::SetExitCountry, which rewrites the torrc and reloads a
  // running Tor in place. Resolves { success: true, selected: "<cc>" }.
  // Args: [callback_id, country_code]
  void HandleSetTorExitCountry(const base::ListValue& args);
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
  // Tier 4: Password vault (molt_ai_chat_handler_vault.cc). The chat
  // page never receives a password; adding entries and revealing one
  // happen on molt://ai-settings (MoltVaultHandler).
  //   vaultList(callback_id)           → entry metadata, no secrets
  //   vaultDelete(callback_id, id)
  //   vaultFindForActive(callback_id)  → entries matching active tab
  //   vaultAutofill(callback_id, id?)  → inject creds into active tab
  void HandleVaultList(const base::ListValue& args);
  void HandleVaultDelete(const base::ListValue& args);
  void HandleVaultFindForActive(const base::ListValue& args);
  void HandleVaultAutofill(const base::ListValue& args);
  // Second half of vaultAutofill: re-verifies that |frame_id| is still
  // the active primary main frame showing |origin|, then injects the
  // entry's credentials into it.
  void FillVaultEntry(const std::string& callback_id,
                      const std::string& entry_id,
                      const content::GlobalRenderFrameHostId& frame_id,
                      const url::Origin& origin);
  // Tier 5: Voice mode. Client records audio via WebAudio, encodes
  // as base64-WAV, ships it here. We hand to VoiceService which
  // shells out to bundled whisper.cpp and returns transcribed text.
  // Args: [callback_id, wav_b64]
  void HandleTranscribeAudio(const base::ListValue& args);
  // Attachment uploads — decode a base64-encoded file the user picked
  // in the chat (on a ThreadPool worker) and extract plain text from it:
  //   .txt/.md   → UTF-8 validate + pass through (worker)
  //   images     → tesseract OCR (worker; same binary the OCR service
  //                resolves)
  //   .pdf       → text layer in the sandboxed document parser, then
  //                OCR fallback (mirrors /pdf)
  //   .docx      → anydoc Markdown in the sandboxed document parser,
  //                else its flat-text extractor (word/document.xml)
  //   Office / e-book / CSV → anydoc Markdown (sandboxed parser)
  // Resolves {success, text, truncated, error?}.
  // Args: [callback_id, name, mime, base64_data]
  void HandleExtractAttachment(const base::ListValue& args);
  // The attachment kinds the sandboxed document parser reads (see
  // HandleExtractAttachment); |pdf| / |document| / |docx| hold the
  // decoded file.
  void ParseAttachmentPdf(const std::string& callback_id,
                          std::unique_ptr<std::string> pdf);
  void ParseAttachmentDocument(const std::string& callback_id,
                               std::unique_ptr<std::string> document,
                               const std::string& extension,
                               bool is_docx);
  void ParseAttachmentDocxText(const std::string& callback_id,
                               std::unique_ptr<std::string> docx,
                               std::string anydoc_error);
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

  // ---- Autonomous Web Agent (WebAgent / ReAct loop) ---------------
  // Start an agent task. Steps are streamed back via 'agent-step'
  // WebUI listener events; the final result resolves the callback.
  // Args: [callback_id, goal_string]
  void HandleStartWebAgent(const base::ListValue& args);
  // Cancel the currently running agent (no-op if none running).
  // Args: []
  void HandleCancelWebAgent(const base::ListValue& args);
  // The user's Allow / Deny answer to an 'agent-confirm' event (the agent
  // pauses on cross-origin navigation, form submits and sensitive fields).
  // Args: [request_id, allowed]
  void HandleWebAgentConfirm(const base::ListValue& args);

  // ---- Saving a successful agent run as a draft workflow ------------
  // (automation/agent_run_draft.h; molt_ai_chat_handler_agent_run.cc.)
  // The run is recorded as the agent reports its steps; when it ends with
  // DONE, its result carries `save_run` (the run's id) if it can be saved,
  // or `save_note` (why not). "Save as automation" in the agent bar sends
  // saveAgentRun: honoured only on the user's click, for the last run,
  // never in MoltNet; the draft starts CASUAL with unconfirmed sites.
  // Args: [callback_id, run_id]. Returns {success, id, name, message} or
  // {success: false, error}.
  void HandleSaveAgentRun(const base::ListValue& args);
  // Opens Agent mode (the studio's side panel) in the chat's window, to
  // review a saved draft. Args: []
  void HandleOpenAgentStudio(const base::ListValue& args);
  // A new run starts recording (its id), a step is recorded, the run ends
  // (adds `save_run` / `save_note` to its |result|), or it is forgotten.
  int BeginAgentRunRecord(const std::string& goal,
                          content::WebContents* target);
  void RecordAgentRunStep(int run_id, const molt_ai::WebAgent::Step& step);
  void FinishAgentRunRecord(int run_id, bool success, base::DictValue* result);
  void ForgetAgentRun();

  // ---- Page-action proposals: state and steps -----------------------
  // One proposal being decided, carried through the asynchronous steps
  // below (defined in molt_ai_chat_handler_actions.cc).
  struct ChatActionDecision;
  // The action proposed for the last typed request, until it runs, is
  // refused, or another message or proposal replaces it.
  struct ChatProposal {
    ChatProposal();
    ~ChatProposal();
    ChatProposal(const ChatProposal&) = delete;
    ChatProposal& operator=(const ChatProposal&) = delete;

    int id = 0;
    molt_ai::agent_actions::AgentAction action;
    // The observation the model chose from: its element ids map to the
    // browser's own selectors.
    molt_ai::agent_actions::Observation observation;
    molt_ai::chat_actions::Scope scope;
    // The document it was proposed for; a navigation away invalidates it.
    content::WeakDocumentPtr document;
    url::Origin origin;
    // The confirmation reason last sent to the page for it ("" if none):
    // a confirmed run acts on a kAsk check only for this same reason.
    std::string shown_reason;
    // A form fill (/fill ai, /paste): where its values came from, and the
    // reason it always asks with (chat_form_fill::RequireConfirmation).
    std::optional<molt_ai::chat_form_fill::Source> form_fill;
    std::string form_fill_reason;
  };
  // CheckProposal() for |proposal| on |probes|; a form fill always asks.
  molt_ai::chat_actions::Check CheckChatProposal(
      const ChatProposal& proposal,
      const std::vector<molt_ai::agent_safety::ElementProbe>& probes);
  // The main frame a proposal is made for: the active tab of the chat's
  // own window (ActiveTabForChatAction), showing an http(s) page, with an
  // on-device model loaded. Else null, and |*unavailable| says why.
  content::RenderFrameHost* PageForChatAction(std::string* unavailable);
  // Pins |frame|'s document to |decision| and lists its elements.
  void StartChatActionDecision(std::unique_ptr<ChatActionDecision> decision,
                               content::RenderFrameHost* frame);
  // Snapshot of the page's interactive elements arrived.
  void OnChatActionSnapshot(std::unique_ptr<ChatActionDecision> decision,
                            base::Value snapshot);
  // Asks the on-device model (again, after an unusable reply).
  void RunChatActionDecision(std::unique_ptr<ChatActionDecision> decision);
  void OnChatActionDecision(std::unique_ptr<ChatActionDecision> decision,
                            uint64_t request_id,
                            molt_ai::GenerationResult result);
  // Probes |selectors| in |frame| (none: |done| runs with no probes).
  void ProbeChatActionTargets(
      content::RenderFrameHost* frame,
      const std::vector<std::string>& selectors,
      base::OnceCallback<void(std::vector<molt_ai::agent_safety::ElementProbe>)>
          done);
  void OnChatProposalProbed(
      std::string callback_id,
      std::unique_ptr<ChatProposal> proposal,
      int epoch,
      std::vector<molt_ai::agent_safety::ElementProbe> probes);
  void OnChatProposalRechecked(
      std::string callback_id,
      int id,
      bool confirmed,
      std::vector<molt_ai::agent_safety::ElementProbe> probes);
  void RunChatProposalNow(const std::string& callback_id,
                          const ChatProposal& proposal,
                          content::RenderFrameHost* frame);
  // The frame of |proposal|'s document if it is still the active page of
  // the chat's window (same document, same origin, primary main frame,
  // active tab, same side of MoltNet's boundary); else null and |*error|
  // says why.
  content::RenderFrameHost* ChatProposalFrame(const ChatProposal& proposal,
                                              std::string* error);
  void ResolveChatAction(const std::string& callback_id,
                         base::DictValue result);

  // Async callbacks (run on UI thread after background work)
  void FinishInitChat(std::string callback_id,
                      base::DictValue settings_dict);
  void OnModelLoaded(std::string callback_id, bool success,
                     const std::string& error);
  void OnPromptComplete(std::string callback_id, bool success,
                        const std::string& full_text,
                        const std::string& error);
  // What the download's worker found before anything is fetched.
  struct DownloadPrecheck {
    DownloadPrecheck();
    ~DownloadPrecheck();
    DownloadPrecheck(DownloadPrecheck&&);
    DownloadPrecheck& operator=(DownloadPrecheck&&);

    // The model's terms must be accepted first.
    bool license_required = false;
    // User-facing reason the download can't start ("" if it can).
    std::string error;
    // The files still to fetch, and the bytes.
    molt_ai::models::DownloadPlan plan;
  };
  // A model download is one unit (molt_ai/models/model_download_plan.h):
  // the weights and, for a model that sees images, its vision projector,
  // each fetched and verified by its own ModelDownloadJob, in order. The
  // model counts as downloaded only once every file is in place.
  void OnDownloadPrecheckComplete(
      std::string callback_id,
      std::string model_id,
      molt_ai::ModelInfo info,
      std::vector<molt_ai::models::ModelFileSpec> files,
      DownloadPrecheck precheck);
  // Starts the job for download_queue_'s first file.
  void StartNextModelFile();
  void OnDownloadProgress(int64_t received, int64_t total);
  void OnDownloadFinished(molt_ai::models::ModelDownloadJob::Result result);
  void FinishDownload(bool success, bool verified, std::string error);
  // Clears the download state (after FinishDownload, a cancel, or a
  // precheck that didn't start one).
  void ResetDownloadState();
  // The size the model pickers show: the whole unit, in MB.
  static int DownloadSizeMb(const molt_ai::ModelInfo& info);
  void OnModelDeleted(std::string callback_id, std::string model_id,
                      bool success);
  void OnHistoryExported(std::string callback_id,
                         base::FilePath file_path,
                         std::string filename,
                         bool success);
  // Shared reply for the conversation-store handlers: resolves the JS
  // promise with the result dict built on the ThreadPool worker.
  void ResolveConversationCallback(std::string callback_id,
                                   base::DictValue result);
  // Lazily-created single sequence for conversation-store file IO (keeps
  // same-id autosaves ordered).
  base::SequencedTaskRunner* ConversationTaskRunner();
  // Runs |op| on the conversation sequence against this profile's store
  // (after the one-time legacy migration) and resolves |callback_id| with
  // its result. Off-the-record: resolves immediately without touching
  // disk (an empty list when |is_list|, else an error). Waits for
  // molt_ai::molt_os_crypt to be ready first.
  void RunConversationOp(
      std::string callback_id,
      bool is_list,
      base::OnceCallback<base::DictValue(
          const molt_ai::memory::ConversationStore&)> op);
  void PostConversationOp(
      std::string callback_id,
      base::OnceCallback<base::DictValue(
          const molt_ai::memory::ConversationStore&)> op);
  // |url| / |title| / |nav_entry_id| describe the document the capture was
  // started on; the reply is dropped if the tab moved on meanwhile.
  void OnPageContentExtracted(std::string callback_id,
                              std::string url,
                              std::string title,
                              int nav_entry_id,
                              base::Value result);

  // The page this chat is about: ActiveTabForChatAction(), for reading.
  content::WebContents* GetActiveTabForChat();

  // The window everything the chat does goes to (page actions and slash
  // commands, the web agent, /fill, vault autofill, links from replies, the
  // tab and privacy commands): the chat's own only, the window holding it
  // as a tab or hosting its side panel (chat_handler_helpers::
  // ChooseChatWindow; the side panel's WebContents is not a tab, so
  // FindBrowserWithTab alone misses it). Never another window. Null when
  // there is none, or when it is across MoltNet's boundary from the chat
  // (molt_ai::tor::MayChatActInWindow); then `*crossed_moltnet` is true.
  BrowserWindowInterface* BrowserForChatAction(bool* crossed_moltnet);
  // The active tab of that window, never the chat itself (a chat opened as
  // a tab is its window's active tab). Null as above, or when the chat is
  // the active tab.
  content::WebContents* ActiveTabForChatAction(bool* crossed_moltnet);

  // What this chat may use outside on-device processing. False in MoltNet
  // windows (molt_ai/tor/molt_net_requests_policy.h): cloud AI providers,
  // and page watchers / saved plans, which run later outside Tor.
  bool CloudAiAllowed() const;
  bool SavedWorkflowsAllowed();

  // Ensure BrowserAIRuntime is created and initialized
  molt_ai::BrowserAIRuntime* GetOrCreateRuntime();

  // The profile's browser-process URL loader factory, used for the
  // cloud (frontier API) chat + model-listing paths.
  scoped_refptr<network::SharedURLLoaderFactory> GetUrlLoaderFactory();

  // Cloud (frontier API) send path — invoked from HandleSendPrompt when
  // the picked model id is a cloud "provider:model" id. Assembles the
  // same gated system prompt + history + user message as the local
  // path (settings + key read off-thread), then streams via
  // molt_ai::StartCloudChat with all callbacks on the UI thread.
  // |attached| lists which context kinds this send carries ({page, pdf,
  // attachment, memory, history} -> bool); it is echoed to the page when
  // first-use consent is required.
  void StartCloudPrompt(std::string callback_id,
                        std::string model_id,
                        std::string prompt_text,
                        std::string history_text,
                        std::string page_context,
                        bool page_action_note,
                        base::DictValue attached);
  // Worker result for StartCloudPrompt.
  struct CloudPromptPrep {
    // False until the user accepted the first-use dialog for this provider;
    // nothing is assembled or sent before that.
    bool consented = false;
    // False when the provider has no stored key.
    bool cfg_found = false;
    molt_ai::CloudChatRequest request;
  };
  // UI-thread continuation of StartCloudPrompt after the off-thread
  // consent + settings + provider-config read. Kicks off the actual
  // network stream, or asks the page for consent.
  void OnCloudPromptReady(std::string callback_id,
                          std::string provider_id,
                          std::string model,
                          base::DictValue attached,
                          CloudPromptPrep prep);

  raw_ptr<Profile> profile_;
  // The on-device runtime is a process-wide singleton reached via
  // GetOrCreateRuntime() -> BrowserAIRuntime::GetInstance(); this handler does
  // NOT own it (owning it per-page caused a use-after-free on page close).
  bool model_loaded_ = false;
  // The model id most recently requested via loadModel (the chat
  // model-chip picker). The lazy-load path in HandleSendPrompt prefers
  // this over settings.json's default_model, so a pick whose explicit
  // load failed is retried on the next send instead of silently
  // falling back to a different model.
  std::string last_requested_model_id_;
  std::string active_prompt_callback_id_;
  // Runtime request id of this page's in-flight local generation (0 if
  // none). Stop cancels only this one (A6).
  uint64_t active_request_id_ = 0;

  // Model download state (one download at a time).
  std::unique_ptr<molt_ai::models::ModelDownloadJob> download_job_;
  // PDF extraction transient state (separate loader so it doesn't race
  // with a concurrent model download).
  std::unique_ptr<network::SimpleURLLoader> pdf_loader_;
  std::string pdf_callback_id_;
  std::string pdf_source_url_;

  // Untrusted documents (/pdf, chat attachments) are parsed only through
  // this: in a sandboxed utility process per call, never in the browser
  // process. Destroying it ends pending parses (and their processes).
  molt_ai::DocumentParserClient document_parser_;
  // The download in progress, from its precheck to FinishDownload (one at a
  // time; downloading_model_id_ is "" when there is none).
  std::string download_callback_id_;
  std::string downloading_model_id_;
  // Its files (the weights, then a vision projector), the indices of
  // those still to fetch (the first one is downloading), the complete
  // files' bytes, and whether every file fetched matched its pin.
  std::vector<molt_ai::models::ModelFileSpec> download_files_;
  std::vector<size_t> download_queue_;
  uint64_t download_done_bytes_ = 0;
  bool download_verified_ = true;
  // The whole unit's size, for progress.
  uint64_t download_total_bytes_ = 0;
  base::TimeTicks download_start_time_;
  uint64_t download_last_bytes_ = 0;
  base::TimeTicks download_last_time_;

  // Active WebAgent (null when no agent task is running).
  std::unique_ptr<molt_ai::WebAgent> web_agent_;
  // The last agent run as the browser saw it, for "Save as automation",
  // until it is saved, cancelled, refused or replaced by the next run; and
  // its id (bumped when it is forgotten, so stale events miss).
  int agent_run_id_ = 0;
  molt_ai::automation::agent_draft::Run agent_run_;

  // Page-action proposals (molt_ai_chat_handler_actions.cc). The typed
  // message that asked to act on the page waits here for its text reply:
  // the token the reply carries as `action_request` (0: none), the
  // sendPrompt callback it belongs to, and the user's words.
  int chat_action_token_ = 0;
  int next_chat_action_token_ = 0;
  std::string chat_action_prompt_callback_id_;
  std::string chat_action_request_;
  // Bumped by Stop and by every send; a decision started in an older epoch
  // ends as cancelled.
  int chat_action_epoch_ = 0;
  std::unique_ptr<ChatProposal> chat_proposal_;
  int next_chat_proposal_id_ = 0;
  // The fields form fills wrote on one document (their selectors): the next
  // /fill ai or /paste there leaves them out and continues with the others.
  content::WeakDocumentPtr form_fill_document_;
  std::vector<std::string> form_fill_done_selectors_;

  // Active cloud (frontier API) chat stream (null when no cloud send is
  // in flight). Kept alive for the stream's duration; destroying it or
  // calling Cancel() stops all callbacks. HandleCancelGeneration resets
  // it in addition to cancelling the local runtime.
  std::unique_ptr<molt_ai::CloudChatStream> active_cloud_stream_;
  // Accumulated cloud reply text, for the final ResolveJavascriptCallback.
  std::string cloud_accumulated_text_;

  // Conversation-store file IO runs on a single sequence so a slow older
  // autosave can never land after (and clobber) a newer one for the same
  // conversation id. Created lazily on first use.
  scoped_refptr<base::SequencedTaskRunner> conversation_task_runner_;

  base::WeakPtrFactory<MoltAIChatHandler> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_HANDLER_H_
