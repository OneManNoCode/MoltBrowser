// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What the voice loop knows and keeps about the rest of the browser
// (docs/design/TOUCHLESS_BROWSING.md §2, "Run the browser itself" and
// "Recover"): the browser actions a kBrowserAction step asks for, the
// browser's lists read aloud (bookmarks, history, downloads, the browser's
// own buttons), what the window can do now, and the conversation's part
// in it (undo, find, the sites opened by voice). Planned by
// voice_browser_logic.h, carried out by voice_browser_actions.h.
//
// Std-only. Nothing here is stored: it lives in the window's voice loop,
// in memory, and voice mode is off in MoltNet windows.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_TYPES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/voice/browser_ui_map.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/voice_undo.h"

namespace molt_ai {
namespace voice {

// The browser's lists, and the matches of a find, kept at most.
inline constexpr size_t kMaxBrowserItems = 50;
inline constexpr size_t kMaxFindMatches = 50;
// Sites opened by voice in the window, kept for "YouTube" alone.
inline constexpr size_t kMaxOpenedSites = 20;
// A browser list holds for this long after it was read (ms).
inline constexpr int64_t kBrowserListHoldMs = 120000;

// What a kBrowserAction step asks of the browser (VoiceStep::browser_action;
// its other fields as said).
enum class BrowserAction : uint8_t {
  kNone,
  // The page's media element (voice_media_controls.h), in |target|'s frame
  // (the player the map found), else the top-level document's.
  kSetPlaybackRate,  // |index|: hundredths; 0: one step, |count| +1 or -1
  kSetVolumeLevel,   // |index|: 0..100
  kMediaCaptions,    // |flag|: on or off
  // Bookmarks of the active tab's page.
  kAddBookmark,     // |url| and |text| (its title) when undoing; else the page
  kRemoveBookmark,  // |url| when undoing; else the page
  // The browser's lists: read, then planned with PlanBrowserItems().
  kListBookmarks,  // |text|: a name to find ("": all)
  kListHistory,    // |text|: a site or title to find ("": the newest)
  kListDownloads,
  kListBrowserUi,  // the browser's own buttons, or the open menu
  // An entry of a list read.
  kOpenUrl,         // |url| (a bookmark's, a history entry's), |new_tab|
  kOpenDownload,    // |index|: the download's id
  kPressBrowserUi,  // |index|: the view's accessibility id
  kReopenTab,       // |flag|: the tab voice mode closed (undo), else the last
  kOpenSettings,    // |index|: SettingsPage
  kSetAlwaysListening,  // |flag|
  kNewWindow,           // |index|: WindowKind
  kCloseWindow,
  kSwitchWindow,  // |count|: +1 the next window, -1 the one before
  kAskAI,         // |text|: the question
  kOpenAIChat,
  kOpenMenu,
  kCloseMenu,
};

// The browser's lists voice mode reads.
enum class BrowserListKind : uint8_t {
  kNone,
  kBookmarks,
  kHistory,
  kDownloads,
  kBrowserUi,  // the browser's buttons, or a menu's items
};

// One entry of a browser list. Titles and names are the user's or a page's
// (a bookmark's name, a history entry's title, a file's name): cleaned for
// speech by the browser before they get here.
struct BrowserItem {
  BrowserItem();
  ~BrowserItem();
  BrowserItem(const BrowserItem&);
  BrowserItem& operator=(const BrowserItem&);
  BrowserItem(BrowserItem&&);
  BrowserItem& operator=(BrowserItem&&);

  std::string title;
  // The site ("youtube.com"), for "from" and for what is said with it.
  std::string host;
  // A bookmark's or a history entry's address (http(s) only).
  std::string url;
  // A download's id, or a browser button's accessibility id.
  int64_t id = 0;
  // Downloads: finished, blocked as dangerous, and whether the file could
  // run code on this computer (voice mode doesn't open those).
  bool complete = true;
  bool dangerous = false;
  bool runs_code = false;
  // The browser's buttons.
  BrowserUiRegion region = BrowserUiRegion::kOther;
  BrowserUiRole role = BrowserUiRole::kOther;
  bool enabled = true;
  bool checked = false;
  bool has_submenu = false;
};

// What the window can do now (the glue fills it; VoiceEnvironment::
// browser).
struct BrowserEnvironment {
  BrowserEnvironment();
  ~BrowserEnvironment();
  BrowserEnvironment(const BrowserEnvironment&);
  BrowserEnvironment& operator=(const BrowserEnvironment&);
  BrowserEnvironment(BrowserEnvironment&&);
  BrowserEnvironment& operator=(BrowserEnvironment&&);

  // The glue knows the browser (false in tests that don't set it: the
  // actions are planned anyway).
  bool known = false;
  // The active tab's page: its address (http(s), else empty) and title
  // (speech-safe); whether it can be bookmarked, and is.
  std::string page_url;
  std::string page_title;
  bool can_bookmark = true;
  bool bookmarked = false;
  // An off-the-record window (a private window): no history.
  bool private_window = false;
  // The profile's other windows voice mode can move to (not MoltNet
  // windows).
  int other_windows = 0;
  // Always listening is on (voice_session_settings.h).
  bool always_listening = false;
  // The on-device AI chat is open beside the page.
  bool ai_chat_open = false;
  // A menu of the browser shows, and its items (the browser's buttons when
  // the browser's list read last is theirs).
  bool menu_open = false;
  std::vector<BrowserItem> ui;
};

// A place "find" found: what it is (spoken), the node and its item number.
struct FindMatch {
  FindMatch();
  ~FindMatch();
  FindMatch(const FindMatch&);
  FindMatch& operator=(const FindMatch&);
  FindMatch(FindMatch&&);
  FindMatch& operator=(FindMatch&&);

  std::string where;
  PageNodeHandle handle;
  int item_number = 0;
};

// A site opened by voice in the window: "YouTube" alone opens it again.
struct OpenedSite {
  // Folded, as said ("bbc news") and as its host's name ("bbc").
  std::string words;
  std::string host_name;
  // As the browser says it, and its address.
  std::string name;
  std::string url;
};

// The conversation's part (ConversationState::browser).
struct BrowserConversation {
  BrowserConversation();
  ~BrowserConversation();
  BrowserConversation(const BrowserConversation&);
  BrowserConversation& operator=(const BrowserConversation&);
  BrowserConversation(BrowserConversation&&);
  BrowserConversation& operator=(BrowserConversation&&);

  // The browser's list read last, while it holds (until another list is
  // read, or kBrowserListHoldMs), and the entry last chosen or read
  // (1-based), and the close ones offered after an unclear choice.
  BrowserListKind list = BrowserListKind::kNone;
  std::vector<BrowserItem> items;
  int64_t list_expires_ms = 0;
  int current = 0;
  std::vector<size_t> offered;
  // The last "find" on |find_page_id|: its words, its matches, and the one
  // shown (1-based).
  std::string find_query;
  std::vector<FindMatch> find_matches;
  int find_index = 0;
  int64_t find_page_id = -1;
  // Undo (voice_undo.h).
  std::vector<UndoEntry> undo;
  // Sites opened by voice, newest last.
  std::vector<OpenedSite> opened_sites;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_TYPES_H_
