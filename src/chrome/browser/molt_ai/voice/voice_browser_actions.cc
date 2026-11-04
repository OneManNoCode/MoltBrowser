// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_browser_actions.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/tor/molt_net_window.h"
#include "chrome/browser/molt_ai/voice/browser_ui_map.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/spoken_answers.h"
#include "chrome/browser/molt_ai/voice/voice_ai_answers.h"
#include "chrome/browser/molt_ai/voice/voice_browser_logic.h"
#include "chrome/browser/molt_ai/voice/voice_browser_ui.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"
#include "chrome/browser/molt_ai/voice/voice_page_controller.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/tab_restore_service_factory.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_live_tab_context.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_id.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_key.h"
#include "chrome/browser/ui/side_panel/side_panel_enums.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/browser/ui/singleton_tabs.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/bookmarks/browser/bookmark_utils.h"
#include "components/download/public/common/download_item.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_types.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/sessions/core/live_tab_context.h"
#include "components/sessions/core/tab_restore_service.h"
#include "components/sessions/core/tab_restore_types.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/isolated_world_ids.h"
#include "ui/base/base_window.h"
#include "ui/base/models/tree_node_iterator.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace molt_ai {
namespace voice {

namespace {

// Titles and names said, in code points.
constexpr size_t kMaxTitleChars = 160;
// Bookmarks looked at, at most, and history entries asked for.
constexpr size_t kMaxBookmarksScanned = 5000;
constexpr int kMaxHistoryEntries = 50;
// A menu's items are read once it has opened (a submenu opens from the
// press that asked for it).
constexpr base::TimeDelta kMenuSettle = base::Milliseconds(150);

// MoltBrowser's AI settings page, and its sections.
constexpr char kAISettingsURL[] = "chrome://molt-ai-settings/";
constexpr char kVoiceSettingsURL[] = "chrome://molt-ai-settings/?section=voice";
constexpr char kMoltNetSettingsURL[] =
    "chrome://molt-ai-settings/?section=moltnet";

std::string Title(std::u16string_view title) {
  return SpeechSafeText(base::UTF16ToUTF8(title), kMaxTitleChars);
}

std::string HostOf(const GURL& url) {
  std::string host(url.host());
  if (host.starts_with("www.")) {
    host.erase(0, 4);
  }
  return SpeechSafeText(host, kMaxTitleChars);
}

// The script's result, as the page's (an isolated world's value, but of
// the page's element): every number clamped.
MediaState MediaStateFrom(const base::Value& result) {
  MediaState state;
  const base::DictValue* dict = result.GetIfDict();
  if (!dict) {
    return state;
  }
  state.found = dict->FindBool("found").value_or(false);
  state.rate = ClampPlaybackRate(dict->FindInt("rate").value_or(
      static_cast<int>(dict->FindDouble("rate").value_or(100))));
  state.volume = std::clamp(dict->FindInt("volume").value_or(static_cast<int>(
                                dict->FindDouble("volume").value_or(100))),
                            0, 100);
  state.muted = dict->FindBool("muted").value_or(false);
  state.caption_tracks =
      std::clamp(dict->FindInt("tracks").value_or(0), 0, 1000);
  state.captions_showing = dict->FindBool("showing").value_or(false);
  return state;
}

bool IsVoiceWindow(BrowserWindowInterface* window) {
  return window && window->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
         window->GetProfile() && !tor::IsMoltNetProfile(window->GetProfile());
}

VoiceBrowserActions::WindowListening& Listening() {
  static base::NoDestructor<VoiceBrowserActions::WindowListening> listening;
  return *listening;
}

}  // namespace

VoiceBrowserActions::WindowListening::WindowListening() = default;
VoiceBrowserActions::WindowListening::~WindowListening() = default;
VoiceBrowserActions::WindowListening::WindowListening(const WindowListening&) =
    default;
VoiceBrowserActions::WindowListening&
VoiceBrowserActions::WindowListening::operator=(const WindowListening&) =
    default;

// static
void VoiceBrowserActions::SetWindowListening(WindowListening listening) {
  Listening() = std::move(listening);
}

VoiceBrowserActions::VoiceBrowserActions(BrowserWindowInterface* browser,
                                         Delegate* delegate)
    : browser_(browser),
      delegate_(delegate),
      ai_answers_(std::make_unique<VoiceAiAnswers>()) {
  // Always listening, as settings say.
  if (Listening().read_always_listening) {
    Listening().read_always_listening.Run(base::BindOnce(
        [](base::WeakPtr<VoiceBrowserActions> self, bool on) {
          if (self) {
            self->always_listening_ = on;
          }
        },
        weak_factory_.GetWeakPtr()));
  }
}

VoiceBrowserActions::~VoiceBrowserActions() = default;

Profile* VoiceBrowserActions::GetProfile() {
  return browser_ ? browser_->GetProfile() : nullptr;
}

bool VoiceBrowserActions::busy() const {
  return pending_run_ != 0;
}

void VoiceBrowserActions::Cancel() {
  pending_run_ = 0;
  run_weak_factory_.InvalidateWeakPtrs();
  history_tracker_.TryCancelAll();
  ai_answers_->Cancel();
}

void VoiceBrowserActions::Finish(int run) {
  if (run != pending_run_) {
    return;
  }
  pending_run_ = 0;
  delegate_->BrowserActionDone(run);
}

void VoiceBrowserActions::Deliver(int run,
                                  VoiceStep step,
                                  std::vector<BrowserItem> items) {
  if (run != pending_run_) {
    return;
  }
  pending_run_ = 0;
  delegate_->BrowserItemsReady(run, step, std::move(items));
}

VoiceBrowserActions::Result VoiceBrowserActions::Run(const VoiceStep& step,
                                                     int run) {
  Result result;
  if (!browser_ || !GetProfile() || tor::IsMoltNetProfile(GetProfile())) {
    result.outcome = Outcome::kFailed;
    result.failure = StepFailure::kNotActive;
    return result;
  }
  switch (step.browser_action) {
    case BrowserAction::kNone:
      return result;
    case BrowserAction::kSetPlaybackRate:
    case BrowserAction::kSetVolumeLevel:
    case BrowserAction::kMediaCaptions:
      return RunMedia(step, run);
    case BrowserAction::kAddBookmark:
      return Bookmark(step, /*add=*/true);
    case BrowserAction::kRemoveBookmark:
      return Bookmark(step, /*add=*/false);
    case BrowserAction::kListBookmarks:
      pending_run_ = run;
      ListBookmarks(step, run);
      result.outcome = Outcome::kPending;
      return result;
    case BrowserAction::kListHistory:
      pending_run_ = run;
      ListHistory(step, run);
      result.outcome = Outcome::kPending;
      return result;
    case BrowserAction::kListDownloads:
      pending_run_ = run;
      ListDownloads(step, run);
      result.outcome = Outcome::kPending;
      return result;
    case BrowserAction::kListBrowserUi:
      pending_run_ = run;
      ListBrowserUi(step, run);
      result.outcome = Outcome::kPending;
      return result;
    case BrowserAction::kOpenUrl:
      return OpenUrl(step);
    case BrowserAction::kOpenDownload:
      return OpenDownload(step);
    case BrowserAction::kPressBrowserUi:
      return PressBrowserUi(step);
    case BrowserAction::kReopenTab:
      return ReopenTab(step);
    case BrowserAction::kOpenSettings:
      return OpenSettings(step);
    case BrowserAction::kSetAlwaysListening:
      SetAlwaysListening(step.flag);
      return result;
    case BrowserAction::kNewWindow:
      return NewWindow(step);
    case BrowserAction::kCloseWindow:
      return CloseWindow();
    case BrowserAction::kSwitchWindow:
      return SwitchWindow();
    case BrowserAction::kAskAI:
      return AskAI(step, run);
    case BrowserAction::kOpenAIChat:
      if (SidePanelUI* side_panel = SidePanelUI::From(browser_)) {
        side_panel->Show(SidePanelEntryId::kMoltAiChat,
                         SidePanelOpenTrigger::kAppMenu);
        return result;
      }
      result.outcome = Outcome::kFailed;
      return result;
    case BrowserAction::kOpenMenu:
      if (!chrome::ExecuteCommand(browser_, IDC_SHOW_APP_MENU)) {
        result.outcome = Outcome::kFailed;
      }
      return result;
    case BrowserAction::kCloseMenu:
      if (VoiceBrowserUi* ui = VoiceBrowserUi::Get()) {
        ui->CloseMenu(browser_);
      }
      return result;
  }
  return result;
}

// ---- Media ------------------------------------------------------------------

bool VoiceBrowserActions::RunMediaScript(
    const VoiceStep& step,
    MediaOp op,
    int value,
    base::OnceCallback<void(base::Value)> done) {
  content::WebContents* contents = delegate_->BrowserActionContents();
  VoicePageController* page = delegate_->BrowserActionPage();
  if (!contents || !page) {
    return false;
  }
  // The player's frame; the top-level document's when the map named none.
  content::RenderFrameHost* frame = nullptr;
  if (step.target.valid()) {
    VoicePageController::ActionResult error;
    frame = page->FrameOf(step.target, &error);
  }
  if (!frame) {
    frame = contents->GetPrimaryMainFrame();
  }
  if (!frame ||
      !agent_safety::IsAllowedNavigationURL(frame->GetLastCommittedURL())) {
    return false;
  }
  frame->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(MediaScript(op, value)), std::move(done),
      content::ISOLATED_WORLD_ID_CONTENT_END);
  return true;
}

VoiceBrowserActions::Result VoiceBrowserActions::RunMedia(const VoiceStep& step,
                                                          int run) {
  Result result;
  pending_run_ = run;
  bool started = false;
  switch (step.browser_action) {
    case BrowserAction::kSetPlaybackRate:
      if (step.index == 0) {
        // One step from the element's own speed: asked first.
        started = RunMediaScript(
            step, MediaOp::kQuery, 0,
            base::BindOnce(&VoiceBrowserActions::OnMediaQueried,
                           run_weak_factory_.GetWeakPtr(), run, step));
      } else {
        started =
            RunMediaScript(step, MediaOp::kSetRate, step.index,
                           base::BindOnce(&VoiceBrowserActions::OnMediaDone,
                                          run_weak_factory_.GetWeakPtr(), run,
                                          MediaOp::kSetRate, step.index));
      }
      break;
    case BrowserAction::kSetVolumeLevel:
      started =
          RunMediaScript(step, MediaOp::kSetVolume, step.index,
                         base::BindOnce(&VoiceBrowserActions::OnMediaDone,
                                        run_weak_factory_.GetWeakPtr(), run,
                                        MediaOp::kSetVolume, step.index));
      break;
    case BrowserAction::kMediaCaptions:
      started =
          RunMediaScript(step, MediaOp::kCaptions, step.flag ? 1 : 0,
                         base::BindOnce(&VoiceBrowserActions::OnMediaDone,
                                        run_weak_factory_.GetWeakPtr(), run,
                                        MediaOp::kCaptions, step.flag ? 1 : 0));
      break;
    default:
      break;
  }
  if (!started) {
    pending_run_ = 0;
    result.outcome = Outcome::kFailed;
    result.failure = StepFailure::kNotAllowed;
    return result;
  }
  result.outcome = Outcome::kPending;
  return result;
}

void VoiceBrowserActions::OnMediaQueried(int run,
                                         VoiceStep step,
                                         base::Value result) {
  if (run != pending_run_) {
    return;
  }
  const MediaState state = MediaStateFrom(result);
  if (!state.found) {
    delegate_->BrowserActionSays(
        MediaOutcomeSpeech(MediaOp::kSetRate, 0, state, false,
                           delegate_->BrowserActionLanguage()));
    Finish(run);
    return;
  }
  const int next = NextPlaybackRate(state.rate, step.count > 0);
  if (!RunMediaScript(step, MediaOp::kSetRate, next,
                      base::BindOnce(&VoiceBrowserActions::OnMediaDone,
                                     run_weak_factory_.GetWeakPtr(), run,
                                     MediaOp::kSetRate, next))) {
    Finish(run);
  }
}

void VoiceBrowserActions::OnMediaDone(int run,
                                      MediaOp op,
                                      int asked,
                                      base::Value result) {
  if (run != pending_run_) {
    return;
  }
  content::WebContents* contents = delegate_->BrowserActionContents();
  const bool tab_muted = contents && contents->IsAudioMuted();
  const std::string said =
      MediaOutcomeSpeech(op, asked, MediaStateFrom(result), tab_muted,
                         delegate_->BrowserActionLanguage());
  if (!said.empty()) {
    delegate_->BrowserActionSays(said);
  }
  Finish(run);
}

// ---- Bookmarks --------------------------------------------------------------

VoiceBrowserActions::Result VoiceBrowserActions::Bookmark(const VoiceStep& step,
                                                          bool add) {
  Result result;
  bookmarks::BookmarkModel* model =
      BookmarkModelFactory::GetForBrowserContext(GetProfile());
  content::WebContents* contents = delegate_->BrowserActionContents();
  const GURL url = !step.url.empty()
                       ? GURL(step.url)
                       : (contents ? contents->GetLastCommittedURL() : GURL());
  if (!model || !model->loaded() || !url.SchemeIsHTTPOrHTTPS()) {
    result.outcome = Outcome::kFailed;
    return result;
  }
  if (add) {
    const std::u16string title = !step.text.empty()
                                     ? base::UTF8ToUTF16(step.text)
                                 : contents ? contents->GetTitle()
                                            : std::u16string();
    bookmarks::AddIfNotBookmarked(model, url, title);
  } else {
    bookmarks::RemoveAllBookmarks(model, url, FROM_HERE);
  }
  return result;
}

void VoiceBrowserActions::ListBookmarks(const VoiceStep& step, int run) {
  std::vector<BrowserItem> items;
  bookmarks::BookmarkModel* model =
      BookmarkModelFactory::GetForBrowserContext(GetProfile());
  if (model && model->loaded()) {
    std::vector<const bookmarks::BookmarkNode*> nodes;
    if (!step.text.empty()) {
      bookmarks::QueryFields query;
      query.word_phrase_query =
          std::make_unique<std::u16string>(base::UTF8ToUTF16(step.text));
      nodes = bookmarks::GetBookmarksMatchingProperties(model, query,
                                                        kMaxBrowserItems);
    }
    if (nodes.empty()) {
      // All of them (the matcher compares the name by sound too), the
      // bookmarks bar's first.
      ui::TreeNodeIterator<const bookmarks::BookmarkNode> it(
          model->root_node());
      size_t scanned = 0;
      while (it.has_next() && scanned++ < kMaxBookmarksScanned &&
             nodes.size() < kMaxBrowserItems) {
        const bookmarks::BookmarkNode* node = it.Next();
        if (node->is_url()) {
          nodes.push_back(node);
        }
      }
    }
    for (const bookmarks::BookmarkNode* node : nodes) {
      if (!node->url().SchemeIsHTTPOrHTTPS()) {
        continue;
      }
      BrowserItem item;
      item.title = Title(node->GetTitle());
      item.host = HostOf(node->url());
      item.url = node->url().spec();
      if (item.title.empty()) {
        item.title = item.host;
      }
      items.push_back(std::move(item));
    }
  }
  // "Open my bookmarks": the bookmark manager shows them too.
  if (step.text.empty()) {
    chrome::ShowBookmarkManager(browser_);
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&VoiceBrowserActions::Deliver,
                                run_weak_factory_.GetWeakPtr(), run, step,
                                std::move(items)));
}

// ---- History ----------------------------------------------------------------

void VoiceBrowserActions::ListHistory(const VoiceStep& step, int run) {
  Profile* profile = GetProfile();
  history::HistoryService* history =
      profile && !profile->IsOffTheRecord()
          ? HistoryServiceFactory::GetForProfile(
                profile, ServiceAccessType::EXPLICIT_ACCESS)
          : nullptr;
  if (!history) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&VoiceBrowserActions::Deliver,
                                  run_weak_factory_.GetWeakPtr(), run, step,
                                  std::vector<BrowserItem>()));
    return;
  }
  if (step.text.empty()) {
    chrome::ShowHistory(browser_);
  }
  QueryHistory(run, step, /*recent=*/step.text.empty());
}

void VoiceBrowserActions::QueryHistory(int run, VoiceStep step, bool recent) {
  Profile* profile = GetProfile();
  history::HistoryService* history =
      profile && !profile->IsOffTheRecord()
          ? HistoryServiceFactory::GetForProfile(
                profile, ServiceAccessType::EXPLICIT_ACCESS)
          : nullptr;
  if (!history) {
    Deliver(run, std::move(step), std::vector<BrowserItem>());
    return;
  }
  history::QueryOptions options;
  options.max_count = kMaxHistoryEntries;
  options.duplicate_policy = history::QueryOptions::REMOVE_ALL_DUPLICATES;
  // The history's own words search first; the newest pages when it finds
  // nothing (the matcher then compares the words by sound too).
  const std::u16string text =
      recent ? std::u16string() : base::UTF8ToUTF16(step.text);
  history->QueryHistory(
      text, options,
      base::BindOnce(
          [](base::WeakPtr<VoiceBrowserActions> self, int run, VoiceStep step,
             bool recent, history::QueryResults results) {
            if (!self) {
              return;
            }
            std::vector<BrowserItem> items;
            for (const history::URLResult& result : results) {
              if (!result.url().SchemeIsHTTPOrHTTPS()) {
                continue;
              }
              BrowserItem item;
              item.title = Title(result.title());
              item.host = HostOf(result.url());
              item.url = result.url().spec();
              if (item.title.empty()) {
                item.title = item.host;
              }
              items.push_back(std::move(item));
            }
            self->OnHistory(run, std::move(step), recent, std::move(items));
          },
          run_weak_factory_.GetWeakPtr(), run, step, recent),
      &history_tracker_);
}

void VoiceBrowserActions::OnHistory(int run,
                                    VoiceStep step,
                                    bool recent,
                                    std::vector<BrowserItem> items) {
  if (items.empty() && !recent) {
    QueryHistory(run, std::move(step), /*recent=*/true);
    return;
  }
  Deliver(run, std::move(step), std::move(items));
}

// ---- Downloads --------------------------------------------------------------

void VoiceBrowserActions::ListDownloads(const VoiceStep& step, int run) {
  std::vector<BrowserItem> items;
  content::DownloadManager* manager =
      GetProfile() ? GetProfile()->GetDownloadManager() : nullptr;
  if (manager) {
    content::DownloadManager::DownloadVector downloads;
    manager->GetAllDownloads(&downloads);
    std::vector<download::DownloadItem*> shown;
    for (download::DownloadItem* item : downloads) {
      if (item && !item->IsTemporary() && !item->IsTransient() &&
          item->GetState() != download::DownloadItem::CANCELLED) {
        shown.push_back(item);
      }
    }
    // Newest first, as the downloads page lists them.
    std::sort(shown.begin(), shown.end(),
              [](download::DownloadItem* a, download::DownloadItem* b) {
                return a->GetStartTime() > b->GetStartTime();
              });
    for (download::DownloadItem* download : shown) {
      if (items.size() >= kMaxBrowserItems) {
        break;
      }
      BrowserItem item;
      const std::string name =
          download->GetFileNameToReportUser().AsUTF8Unsafe();
      item.title = SpeechSafeText(name, kMaxTitleChars);
      item.host = HostOf(download->GetURL());
      item.id = download->GetId();
      item.complete =
          download->GetState() == download::DownloadItem::COMPLETE &&
          !download->GetFileExternallyRemoved();
      item.dangerous = download->IsDangerous() || download->IsInsecure();
      item.runs_code = FileCouldRunCode(name);
      items.push_back(std::move(item));
    }
  }
  // "Show my downloads": the downloads page shows them too.
  if (step.index == 0) {
    chrome::ShowDownloads(browser_);
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&VoiceBrowserActions::Deliver,
                                run_weak_factory_.GetWeakPtr(), run, step,
                                std::move(items)));
}

VoiceBrowserActions::Result VoiceBrowserActions::OpenDownload(
    const VoiceStep& step) {
  Result result;
  content::DownloadManager* manager =
      GetProfile() ? GetProfile()->GetDownloadManager() : nullptr;
  download::DownloadItem* item =
      manager && step.index > 0
          ? manager->GetDownload(static_cast<uint32_t>(step.index))
          : nullptr;
  const Language language = delegate_->BrowserActionLanguage();
  // Checked again as the file is now: finished, not dangerous, and not
  // something that runs code.
  if (!item || item->GetState() != download::DownloadItem::COMPLETE ||
      item->GetFileExternallyRemoved()) {
    delegate_->BrowserActionSays(
        Said(language, "I can't open that download now.", {}));
    return result;
  }
  const std::string name = item->GetFileNameToReportUser().AsUTF8Unsafe();
  if (item->IsDangerous() || item->IsInsecure() || FileCouldRunCode(name) ||
      !item->CanOpenDownload()) {
    delegate_->BrowserActionSays(Said(
        language,
        "{0} can run programs on your computer, so I don't open it by "
        "voice. Open it from the downloads page with the mouse or keyboard.",
        {SpeechSafeText(name, kMaxTitleChars)}));
    return result;
  }
  item->OpenDownload();
  return result;
}

// ---- The browser's buttons
// ---------------------------------------------------

void VoiceBrowserActions::ListBrowserUi(const VoiceStep& step, int run) {
  // A menu that is opening (the app menu, a submenu) has its items in a
  // moment.
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<VoiceBrowserActions> self, int run, VoiceStep step) {
            if (!self) {
              return;
            }
            std::vector<BrowserItem> items;
            if (VoiceBrowserUi* ui = VoiceBrowserUi::Get()) {
              const BrowserUiMap map =
                  BuildBrowserUiMap(ui->Collect(self->browser_));
              for (const BrowserUiItem& found : map.items) {
                BrowserItem item;
                item.title = found.name;
                item.id = found.id;
                item.region = found.region;
                item.role = found.role;
                item.enabled = found.enabled;
                item.checked = found.checked;
                item.has_submenu = found.has_submenu;
                items.push_back(std::move(item));
              }
            }
            self->Deliver(run, std::move(step), std::move(items));
          },
          run_weak_factory_.GetWeakPtr(), run, step),
      kMenuSettle);
}

VoiceBrowserActions::Result VoiceBrowserActions::PressBrowserUi(
    const VoiceStep& step) {
  Result result;
  VoiceBrowserUi* ui = VoiceBrowserUi::Get();
  if (!ui || !ui->Press(browser_, static_cast<int32_t>(step.index))) {
    delegate_->BrowserActionSays(Said(
        delegate_->BrowserActionLanguage(),
        "I can't find that button any more. Say show the browser's buttons.",
        {}));
    result.outcome = Outcome::kFailed;
    result.failure = StepFailure::kCancelled;
  }
  return result;
}

// ---- Tabs, pages and settings ---------------------------------------------

VoiceBrowserActions::Result VoiceBrowserActions::OpenUrl(
    const VoiceStep& step) {
  Result result;
  const GURL url(step.url);
  // Only http(s), as for every agent (agent_safety.h).
  if (!agent_safety::IsAllowedNavigationURL(url)) {
    result.outcome = Outcome::kFailed;
    result.failure = StepFailure::kBlockedURL;
    return result;
  }
  content::WebContents* contents = delegate_->BrowserActionContents();
  if (!contents || step.new_tab) {
    browser_->OpenGURL(url, WindowOpenDisposition::NEW_FOREGROUND_TAB);
    return result;
  }
  content::NavigationController::LoadURLParams params(url);
  params.transition_type = ui::PAGE_TRANSITION_AUTO_BOOKMARK;
  contents->GetController().LoadURLWithParams(params);
  return result;
}

void VoiceBrowserActions::NoteTabClosed() {
  sessions::TabRestoreService* service =
      GetProfile() ? TabRestoreServiceFactory::GetForProfile(GetProfile())
                   : nullptr;
  closed_by_voice_.reset();
  if (!service || service->entries().empty()) {
    return;
  }
  const sessions::tab_restore::Entry& newest = *service->entries().front();
  if (newest.type == sessions::tab_restore::TAB) {
    closed_by_voice_ = newest.id;
  }
}

VoiceBrowserActions::Result VoiceBrowserActions::ReopenTab(
    const VoiceStep& step) {
  Result result;
  const Language language = delegate_->BrowserActionLanguage();
  sessions::TabRestoreService* service =
      TabRestoreServiceFactory::GetForProfile(GetProfile());
  if (!service) {
    result.outcome = Outcome::kFailed;
    return result;
  }
  if (step.flag) {
    // "Undo": the very tab voice mode closed, if it is still there.
    std::optional<std::vector<sessions::LiveTab*>> restored;
    if (closed_by_voice_) {
      restored = service->RestoreEntryById(
          BrowserLiveTabContext::From(browser_), *closed_by_voice_,
          WindowOpenDisposition::UNKNOWN);
    }
    closed_by_voice_.reset();
    if (!restored) {
      delegate_->BrowserActionSays(
          Said(language, "I can't reopen that tab any more.", {}));
      return result;
    }
    delegate_->BrowserActionSays(
        step.text.empty() ? Said(language, "Undone: the tab is open again.", {})
                          : Said(language, "Undone: {0} is open again.",
                                 {SpeechSafeText(step.text, kMaxTitleChars)}));
    return result;
  }
  // What was closed last (IDC_RESTORE_TAB reopens it, a tab or a window),
  // and a tab's title.
  std::string title;
  const bool found = !service->entries().empty();
  if (found && service->entries().front()->type == sessions::tab_restore::TAB) {
    const auto& tab = static_cast<const sessions::tab_restore::Tab&>(
        *service->entries().front());
    if (!tab.navigations.empty()) {
      title = Title(tab.navigations[static_cast<size_t>(
                                        tab.normalized_navigation_index())]
                        .title());
    }
  }
  if (!found) {
    delegate_->BrowserActionSays(
        Said(language, "There's no closed tab to reopen.", {}));
    return result;
  }
  if (!chrome::ExecuteCommand(browser_, IDC_RESTORE_TAB)) {
    result.outcome = Outcome::kFailed;
    return result;
  }
  delegate_->BrowserActionSays(
      title.empty() ? Said(language, "Reopened the closed tab.", {})
                    : Said(language, "Reopened {0}.", {title}));
  return result;
}

VoiceBrowserActions::Result VoiceBrowserActions::OpenSettings(
    const VoiceStep& step) {
  Result result;
  switch (static_cast<SettingsPage>(step.index)) {
    case SettingsPage::kMain:
      chrome::ShowSettings(browser_);
      break;
    case SettingsPage::kPrivacy:
      chrome::ShowSettingsSubPage(browser_, chrome::kPrivacySubPage);
      break;
    case SettingsPage::kAI:
      ShowSingletonTab(browser_, GURL(kAISettingsURL));
      break;
    case SettingsPage::kVoice:
      ShowSingletonTab(browser_, GURL(kVoiceSettingsURL));
      break;
    case SettingsPage::kMoltNet:
      ShowSingletonTab(browser_, GURL(kMoltNetSettingsURL));
      break;
  }
  return result;
}

void VoiceBrowserActions::SetAlwaysListening(bool on) {
  // Settings change (on settings.json's one writer), then every window's
  // listening follows, as the settings page's switch does.
  const bool was = always_listening_;
  always_listening_ = on;
  auto saved = base::BindOnce(
      [](base::WeakPtr<VoiceBrowserActions> self, bool was, bool ok) {
        if (!self || ok) {
          return;
        }
        self->always_listening_ = was;
        self->delegate_->BrowserActionSays(
            Said(self->delegate_->BrowserActionLanguage(),
                 "I couldn't save that setting. Change it in settings.", {}));
      },
      weak_factory_.GetWeakPtr(), was);
  if (!Listening().set_always_listening) {
    std::move(saved).Run(false);
    return;
  }
  Listening().set_always_listening.Run(on, std::move(saved));
}

// ---- Windows
// ------------------------------------------------------------------

std::vector<BrowserWindowInterface*> VoiceBrowserActions::OtherWindows() {
  std::vector<BrowserWindowInterface*> others;
  Profile* mine = GetProfile();
  if (!mine) {
    return others;
  }
  for (BrowserWindowInterface* window : GetAllBrowserWindowInterfaces()) {
    if (window == browser_ || !IsVoiceWindow(window) ||
        window->GetProfile()->GetOriginalProfile() !=
            mine->GetOriginalProfile()) {
      continue;
    }
    others.push_back(window);
  }
  return others;
}

void VoiceBrowserActions::MoveVoiceModeTo(BrowserWindowInterface* other) {
  if (!IsVoiceWindow(other)) {
    return;
  }
  // Listening, if it listens here; voice mode on there either way.
  const WindowListening& listening = Listening();
  if (listening.listening && listening.start &&
      listening.listening.Run(browser_)) {
    listening.start.Run(other);
  }
  if (VoiceController* controller =
          VoiceController::GetOrCreateForBrowser(other)) {
    controller->Start();
    std::string title;
    if (TabStripModel* tabs = other->GetTabStripModel()) {
      if (content::WebContents* active = tabs->GetActiveWebContents()) {
        title = Title(active->GetTitle());
      }
    }
    const Language language = controller->language();
    controller->SayNotice(
        title.empty() ? Said(language, "I'm listening in this window now.", {})
                      : Said(language, "I'm listening in this window now: {0}.",
                             {title}));
  }
}

VoiceBrowserActions::Result VoiceBrowserActions::NewWindow(
    const VoiceStep& step) {
  Result result;
  switch (static_cast<WindowKind>(step.index)) {
    case WindowKind::kNormal:
      if (!chrome::ExecuteCommand(browser_, IDC_NEW_WINDOW)) {
        result.outcome = Outcome::kFailed;
      }
      return result;
    case WindowKind::kPrivate:
      if (!chrome::ExecuteCommand(browser_, IDC_NEW_INCOGNITO_WINDOW)) {
        result.outcome = Outcome::kFailed;
      }
      return result;
    case WindowKind::kMoltNet:
      // Tor first, then the isolated window (molt_net_window.h); voice mode
      // never listens there.
      tor::MoltNetWindow::Open(
          GetProfile(), GURL(),
          base::BindOnce(
              [](base::WeakPtr<VoiceBrowserActions> self, bool ok,
                 std::string error) {
                if (self && !ok) {
                  self->delegate_->BrowserActionSays(
                      Said(self->delegate_->BrowserActionLanguage(),
                           "MoltNet couldn't start. Open it from the "
                           "toolbar to see why.",
                           {}));
                }
              },
              weak_factory_.GetWeakPtr()));
      return result;
  }
  return result;
}

VoiceBrowserActions::Result VoiceBrowserActions::CloseWindow() {
  Result result;
  // Voice mode goes on in another of the person's windows, first.
  const std::vector<BrowserWindowInterface*> others = OtherWindows();
  if (!others.empty()) {
    MoveVoiceModeTo(others.front());
    if (ui::BaseWindow* window = others.front()->GetWindow()) {
      window->Activate();
    }
  }
  if (!chrome::ExecuteCommand(browser_, IDC_CLOSE_WINDOW)) {
    result.outcome = Outcome::kFailed;
  }
  return result;
}

VoiceBrowserActions::Result VoiceBrowserActions::SwitchWindow() {
  Result result;
  const std::vector<BrowserWindowInterface*> others = OtherWindows();
  if (others.empty()) {
    delegate_->BrowserActionSays(
        Said(delegate_->BrowserActionLanguage(),
             "There's no other window. Say new window to open one.", {}));
    return result;
  }
  BrowserWindowInterface* other = others.front();
  if (ui::BaseWindow* window = other->GetWindow()) {
    window->Activate();
  }
  MoveVoiceModeTo(other);
  return result;
}

// ---- The AI
// -------------------------------------------------------------------

VoiceBrowserActions::Result VoiceBrowserActions::AskAI(const VoiceStep& step,
                                                       int run) {
  Result result;
  pending_run_ = run;
  ai_answers_->Ask(
      step.text, delegate_->BrowserActionLanguage(),
      base::BindRepeating(
          [](base::WeakPtr<VoiceBrowserActions> self, const std::string& said) {
            if (self) {
              self->delegate_->BrowserActionSays(said);
            }
          },
          run_weak_factory_.GetWeakPtr()),
      base::BindOnce(
          [](base::WeakPtr<VoiceBrowserActions> self, int run, std::string text,
             bool model) {
            if (!self || run != self->pending_run_) {
              return;
            }
            if (!text.empty()) {
              if (model) {
                // Model output: read as page text, never a command.
                self->delegate_->BrowserActionReads(text);
              } else {
                self->delegate_->BrowserActionSays(text);
              }
            }
            self->Finish(run);
          },
          run_weak_factory_.GetWeakPtr(), run));
  result.outcome = Outcome::kPending;
  return result;
}

// ---- What the window can do
// --------------------------------------------------

BrowserEnvironment VoiceBrowserActions::Environment() {
  BrowserEnvironment env;
  env.known = true;
  Profile* profile = GetProfile();
  if (!browser_ || !profile) {
    return env;
  }
  content::WebContents* contents = delegate_->BrowserActionContents();
  const GURL url = contents ? contents->GetLastCommittedURL() : GURL();
  bookmarks::BookmarkModel* model =
      BookmarkModelFactory::GetForBrowserContext(profile);
  env.can_bookmark = url.SchemeIsHTTPOrHTTPS() && model && model->loaded();
  if (url.SchemeIsHTTPOrHTTPS()) {
    env.page_url = url.spec();
    env.page_title = contents ? Title(contents->GetTitle()) : std::string();
  }
  env.bookmarked =
      env.can_bookmark && bookmarks::IsBookmarkedByUser(model, url);
  env.private_window =
      profile->IsOffTheRecord() && !tor::IsMoltNetProfile(profile);
  env.other_windows = static_cast<int>(OtherWindows().size());
  env.always_listening = always_listening_;
  if (const SidePanelUI* side_panel = SidePanelUI::From(browser_)) {
    env.ai_chat_open = side_panel->IsSidePanelEntryShowing(
        SidePanelEntryKey(SidePanelEntryId::kMoltAiChat));
  }
  if (VoiceBrowserUi* ui = VoiceBrowserUi::Get();
      ui && ui->MenuOpen(browser_)) {
    env.menu_open = true;
    for (const BrowserUiItem& found :
         BuildBrowserUiMap(ui->Collect(browser_)).items) {
      BrowserItem item;
      item.title = found.name;
      item.id = found.id;
      item.region = found.region;
      item.role = found.role;
      item.enabled = found.enabled;
      item.checked = found.checked;
      item.has_submenu = found.has_submenu;
      env.ui.push_back(std::move(item));
    }
  }
  return env;
}

}  // namespace voice
}  // namespace molt_ai
