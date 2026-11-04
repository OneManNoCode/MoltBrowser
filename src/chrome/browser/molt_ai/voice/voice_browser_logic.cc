// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_browser_logic.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "chrome/browser/molt_ai/voice/spoken_answers.h"
#include "chrome/browser/molt_ai/voice/spoken_choice.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_candidates.h"
#include "chrome/browser/molt_ai/voice/voice_media_controls.h"
#include "chrome/browser/molt_ai/voice/voice_prompts.h"
#include "chrome/browser/molt_ai/voice/voice_sites.h"
#include "chrome/browser/molt_ai/voice/voice_speech_text.h"
#include "chrome/browser/molt_ai/voice/voice_undo.h"

namespace molt_ai {
namespace voice {

namespace {

using K = VoiceCommandKind;

// "YouTube" alone: at most this many words.
constexpr size_t kMaxSiteNameWords = 4;

VoiceStep Step(VoiceStepKind kind) {
  VoiceStep step;
  step.kind = kind;
  return step;
}

VoiceStep BrowserStep(BrowserAction action) {
  VoiceStep step = Step(VoiceStepKind::kBrowserAction);
  step.browser_action = action;
  return step;
}

VoiceStep SpeakStep(std::string text) {
  VoiceStep step = Step(VoiceStepKind::kSpeak);
  step.text = std::move(text);
  return step;
}

VoiceStep WaitFor(FollowUpKind follow_up,
                  std::string subject,
                  std::string preface = std::string()) {
  VoiceStep step = Step(VoiceStepKind::kWaitForPage);
  step.follow_up = follow_up;
  step.subject = std::move(subject);
  step.preface = std::move(preface);
  return step;
}

std::string Tr(const TurnPlanner& planner,
               std::string_view english,
               std::initializer_list<std::string_view> args) {
  return Said(planner.Env().language, english, args);
}

// The player the media steps act on: the site skill's, else the top-level
// document's first media, else a frame's (an embedded player). Invalid when
// the page has none.
PageNodeHandle Player(const PageMap& map) {
  if (map.site.media.valid()) {
    return map.site.media;
  }
  for (const bool in_page : {true, false}) {
    for (const PageMedia& media : map.media) {
      if (map.InPage(media.handle) == in_page) {
        return media.handle;
      }
    }
  }
  return PageNodeHandle();
}

bool HasMedia(const TurnPlanner& planner) {
  return !planner.Map().media.empty() || planner.Env().media_session;
}

// The entries' words, one and many, per list.
std::pair<std::string_view, std::string_view> ListWords(BrowserListKind kind,
                                                        bool menu) {
  switch (kind) {
    case BrowserListKind::kBookmarks:
      return {"bookmark", "bookmarks"};
    case BrowserListKind::kHistory:
      return {"page", "pages"};
    case BrowserListKind::kDownloads:
      return {"download", "downloads"};
    case BrowserListKind::kBrowserUi:
      return menu ? std::pair<std::string_view, std::string_view>{"menu item",
                                                                  "menu items"}
                  : std::pair<std::string_view, std::string_view>{"button",
                                                                  "buttons"};
    case BrowserListKind::kNone:
      break;
  }
  return {"item", "items"};
}

BrowserListKind ListKindFor(BrowserAction action) {
  switch (action) {
    case BrowserAction::kListBookmarks:
      return BrowserListKind::kBookmarks;
    case BrowserAction::kListHistory:
      return BrowserListKind::kHistory;
    case BrowserAction::kListDownloads:
      return BrowserListKind::kDownloads;
    case BrowserAction::kListBrowserUi:
      return BrowserListKind::kBrowserUi;
    default:
      return BrowserListKind::kNone;
  }
}

ChoiceRole RoleOf(const BrowserItem& item, BrowserListKind kind) {
  if (kind != BrowserListKind::kBrowserUi) {
    return kind == BrowserListKind::kDownloads ? ChoiceRole::kListItem
                                               : ChoiceRole::kLink;
  }
  switch (item.role) {
    case BrowserUiRole::kTab:
      return ChoiceRole::kTab;
    case BrowserUiRole::kLink:
      return ChoiceRole::kLink;
    case BrowserUiRole::kCheckBox:
    case BrowserUiRole::kMenuItemCheckBox:
      return ChoiceRole::kCheckBox;
    case BrowserUiRole::kMenuItemRadio:
      return ChoiceRole::kRadioButton;
    default:
      return ChoiceRole::kButton;
  }
}

// |items| as choices: each with its title, its site ("from") and, for a
// download, its state.
CandidateSet ItemSet(BrowserListKind kind,
                     const std::vector<BrowserItem>& items,
                     Language language) {
  CandidateSet set;
  for (size_t i = 0; i < items.size(); ++i) {
    const BrowserItem& item = items[i];
    ChoiceCandidate candidate;
    candidate.position = static_cast<int>(i) + 1;
    candidate.role = RoleOf(item, kind);
    candidate.title = item.title;
    if (!item.host.empty() && kind != BrowserListKind::kBrowserUi) {
      candidate.fields.emplace_back(ChoiceFieldKind::kSource, item.host);
    }
    if (kind == BrowserListKind::kDownloads) {
      if (item.dangerous) {
        candidate.fields.emplace_back(
            ChoiceFieldKind::kDetail,
            std::string(Translated(language, "blocked")));
      } else if (!item.complete) {
        candidate.fields.emplace_back(
            ChoiceFieldKind::kDetail,
            std::string(Translated(language, "not finished")));
      }
    }
    if (kind == BrowserListKind::kBrowserUi) {
      if (item.has_submenu) {
        candidate.fields.emplace_back(
            ChoiceFieldKind::kDetail,
            std::string(Translated(language, "more options")));
      } else if (item.checked) {
        candidate.fields.emplace_back(ChoiceFieldKind::kDetail,
                                      std::string(Translated(language, "on")));
      }
      if (!item.enabled) {
        candidate.fields.emplace_back(
            ChoiceFieldKind::kDetail,
            std::string(Translated(language, "turned off")));
      }
    }
    set.candidates.push_back(std::move(candidate));
    CandidateTarget target;
    target.index = static_cast<int>(i);
    set.targets.push_back(std::move(target));
  }
  return set;
}

// Words that make pressing a browser button or menu item worth a "yes":
// it quits the browser or closes windows.
bool BrowserPressNeedsYes(const BrowserItem& item, Language language) {
  const std::string folded = " " + FoldForComparison(item.title) + " ";
  for (std::string_view word :
       {" exit ", " quit ", " close window ", " close all ", " salir ",
        " cerrar ventana ", " cerrar todo "}) {
    if (folded.find(word) != std::string::npos) {
      return true;
    }
  }
  PageMap none;
  return NeedsConfirmation(item.title, std::string_view(), none, language);
}

void ClearBrowserList(BrowserConversation& browser) {
  browser.list = BrowserListKind::kNone;
  browser.items.clear();
  browser.offered.clear();
  browser.current = 0;
  browser.list_expires_ms = 0;
}

// Acts on entry |index| of the browser's list |kind|.
void ActOnItem(BrowserListKind kind,
               const std::vector<BrowserItem>& items,
               size_t index,
               TurnPlanner& p) {
  if (index >= items.size()) {
    return;
  }
  const BrowserItem& item = items[index];
  const std::string name = PageText(item.title);
  p.State().browser.current = static_cast<int>(index) + 1;
  p.State().browser.offered.clear();
  switch (kind) {
    case BrowserListKind::kBookmarks:
    case BrowserListKind::kHistory: {
      if (item.url.empty()) {
        p.Say(Tr(p, "{0}. There's nothing to open there.", {name}));
        return;
      }
      VoiceStep open = BrowserStep(BrowserAction::kOpenUrl);
      open.url = item.url;
      p.Add(std::move(open));
      p.Status(Tr(p, "Opening {0}", {name}));
      p.Add(WaitFor(FollowUpKind::kAnnounceOpened, name));
      return;
    }
    case BrowserListKind::kDownloads: {
      if (item.dangerous) {
        p.Say(Tr(p,
                 "{0} was blocked as dangerous, so I won't open it. The "
                 "downloads page has what you can do with it.",
                 {name}));
        return;
      }
      if (!item.complete) {
        p.Say(Tr(p, "{0} hasn't finished downloading.", {name}));
        return;
      }
      if (item.runs_code) {
        p.Say(Tr(p,
                 "{0} can run programs on your computer, so I don't open it "
                 "by voice. Open it from the downloads page with the mouse or "
                 "keyboard.",
                 {name}));
        return;
      }
      VoiceStep open = BrowserStep(BrowserAction::kOpenDownload);
      open.index = static_cast<int>(item.id);
      open.text = name;
      p.Add(std::move(open));
      p.Status(Tr(p, "Opening {0}", {name}));
      p.Say(Tr(p, "Opening {0}.", {name}));
      return;
    }
    case BrowserListKind::kBrowserUi: {
      if (!item.enabled) {
        p.Say(Tr(p, "{0} is turned off now.", {name}));
        return;
      }
      VoiceStep press = BrowserStep(BrowserAction::kPressBrowserUi);
      press.index = static_cast<int>(item.id);
      press.text = name;
      std::vector<VoiceStep> steps;
      steps.push_back(std::move(press));
      if (item.has_submenu) {
        // The submenu it opens is read next.
        steps.push_back(BrowserStep(BrowserAction::kListBrowserUi));
      } else {
        steps.push_back(SpeakStep(Tr(p, "Pressed {0}.", {name})));
      }
      if (BrowserPressNeedsYes(item, p.Env().language)) {
        p.AskToConfirm(std::move(steps), Tr(p, "press {0}", {name}));
        return;
      }
      p.Status(Tr(p, "Pressing: {0}", {name}));
      for (VoiceStep& step : steps) {
        if (step.kind == VoiceStepKind::kSpeak) {
          p.Say(step.text);
        } else {
          p.Add(std::move(step));
        }
      }
      return;
    }
    case BrowserListKind::kNone:
      return;
  }
}

// Offers the close entries |close| of the browser's list to choose from.
void OfferItems(const CandidateSet& set,
                const std::vector<size_t>& close,
                TurnPlanner& p) {
  p.State().browser.offered = close;
  p.Status("Which one?");
  for (const std::string& chunk :
       OfferSpeech(SubsetOf(set, close), p.Env().language)) {
    p.Say(chunk);
  }
}

// Chooses from the browser's list |kind| what |ref| names; false when
// nothing does (after saying why).
bool ChooseItem(BrowserListKind kind,
                const std::vector<BrowserItem>& items,
                const ChoiceRef& ref,
                TurnPlanner& p) {
  const Language language = p.Env().language;
  const CandidateSet set = ItemSet(kind, items, language);
  BrowserConversation& browser = p.State().browser;
  MatchOptions options;
  options.language = language;
  options.current_position = browser.current;
  // Among the close ones offered first ("the second one", "the Live Aid
  // one"), then the whole list.
  if (!browser.offered.empty()) {
    std::vector<size_t> offered;
    for (size_t i : browser.offered) {
      if (i < set.size()) {
        offered.push_back(i);
      }
    }
    const CandidateSet subset = SubsetOf(set, offered);
    MatchOptions sub = options;
    sub.current_position = 0;
    const ChoiceMatch match = MatchSpokenChoice(ref, subset.candidates, sub);
    if (match.outcome == ChoiceOutcome::kUnique &&
        match.index < offered.size()) {
      ActOnItem(kind, items, offered[match.index], p);
      return true;
    }
  }
  const ChoiceMatch match = MatchSpokenChoice(ref, set.candidates, options);
  switch (match.outcome) {
    case ChoiceOutcome::kUnique:
      ActOnItem(kind, items, match.index, p);
      return true;
    case ChoiceOutcome::kAmbiguous:
      OfferItems(set, match.close, p);
      return true;
    case ChoiceOutcome::kNoMatch:
      break;
  }
  if (match.reason == NoMatchReason::kOutOfRange) {
    const int n =
        ref.kind == ChoiceRef::Kind::kNumber ? ref.number : ref.ordinal;
    p.Say(Tr(p, "There's no number {0} here. {1}",
             {std::to_string(n), ChooseHint(language)}));
    return true;
  }
  return false;
}

// "Next" and "previous" through the browser's list.
void MoveItem(bool next, TurnPlanner& p) {
  BrowserConversation& browser = p.State().browser;
  const int n = static_cast<int>(browser.items.size());
  const int at = browser.current + (next ? 1 : -1);
  if (at < 1 || at > n) {
    p.Say(next ? "That's the last one." : "That's the first one.");
    return;
  }
  browser.current = at;
  const CandidateSet set =
      ItemSet(browser.list, browser.items, p.Env().language);
  p.Say(std::to_string(at) + ": " +
        SpokenCandidate(set.candidates[static_cast<size_t>(at - 1)],
                        p.State().verbosity, p.Env().language) +
        ".");
}

// ---- Media ------------------------------------------------------------------

void StartOver(TurnPlanner& p) {
  const VoiceEnvironment& env = p.Env();
  ConversationState& s = p.State();
  if (env.media_playing) {
    // While something plays, "start over" plays it from the start.
    VoiceStep seek = Step(VoiceStepKind::kMedia);
    seek.media = MediaCommand::kSeekToStart;
    p.Add(std::move(seek));
    p.Say("From the start.");
    return;
  }
  s.list = ListContext::kNone;
  s.list_container = PageNodeHandle();
  s.list_label.clear();
  s.list_page_id = -1;
  s.offered.clear();
  s.offered_handles.clear();
  s.current_position = 0;
  s.expecting_choice = false;
  s.field = PageNodeHandle();
  s.field_page_id = -1;
  s.heading_cursor = 0;
  s.link_cursor = 0;
  ClearBrowserList(s.browser);
  s.browser.find_query.clear();
  s.browser.find_matches.clear();
  s.browser.find_index = 0;
  s.browser.find_page_id = -1;
  if (env.browser.menu_open) {
    p.Add(BrowserStep(BrowserAction::kCloseMenu));
  }
  p.Status("Starting over");
  if (PromptWaits(s.prompt, env.now_ms)) {
    p.Say(
        "Starting over. A question from the browser is still waiting for "
        "your answer.");
    return;
  }
  p.Say("Starting over. What would you like to do?");
}

void MoveMatch(bool next, TurnPlanner& p) {
  BrowserConversation& browser = p.State().browser;
  if (browser.find_page_id != p.Env().page_id || browser.find_matches.empty()) {
    p.Say("Find something first: say find, then the words.");
    return;
  }
  const int n = static_cast<int>(browser.find_matches.size());
  const int at = browser.find_index + (next ? 1 : -1);
  if (at < 1 || at > n) {
    p.Say(next ? "That's the last match." : "That's the first match.");
    return;
  }
  browser.find_index = at;
  const FindMatch& match = browser.find_matches[static_cast<size_t>(at - 1)];
  VoiceStep scroll = Step(VoiceStepKind::kScrollIntoView);
  scroll.target = match.handle;
  scroll.text = match.where;
  p.Add(std::move(scroll));
  if (match.item_number) {
    p.SetCurrentItem(match.item_number);
  }
  p.Say(Tr(p, "Match {0} of {1}: {2}.",
           {std::to_string(at), std::to_string(n), match.where}));
}

// ---- The browser itself --------------------------------------------------

void Bookmark(bool add, TurnPlanner& p) {
  const VoiceEnvironment& env = p.Env();
  const BrowserEnvironment& b = env.browser;
  if (!env.web_page || (b.known && (!b.can_bookmark || b.page_url.empty()))) {
    p.Say(add ? "I can only bookmark web pages."
              : "This page isn't bookmarked.");
    return;
  }
  const std::string name =
      PageText(b.page_title.empty() ? env.host : b.page_title);
  if (add && b.known && b.bookmarked) {
    p.Say("This page is already bookmarked.");
    return;
  }
  if (!add && b.known && !b.bookmarked) {
    p.Say("This page isn't bookmarked.");
    return;
  }
  UndoEntry undo;
  undo.kind = add ? UndoKind::kRemoveBookmark : UndoKind::kRestoreBookmark;
  undo.url = b.page_url;
  undo.text = b.page_title;
  undo.name = name;
  NoteUndo(p.State().browser.undo, std::move(undo));
  VoiceStep step = BrowserStep(add ? BrowserAction::kAddBookmark
                                   : BrowserAction::kRemoveBookmark);
  p.Add(std::move(step));
  if (add) {
    p.Status(Tr(p, "Bookmarked {0}", {name}));
    p.Say(name.empty() ? std::string("Bookmarked this page.")
                       : Tr(p, "Bookmarked {0}.", {name}));
    return;
  }
  p.Status("Removed the bookmark");
  p.Say("Removed this page's bookmark. Say undo to put it back.");
}

void OpenSettings(SettingsPage page, TurnPlanner& p) {
  VoiceStep open = BrowserStep(BrowserAction::kOpenSettings);
  open.index = static_cast<int>(page);
  p.Add(std::move(open));
  switch (page) {
    case SettingsPage::kMain:
      p.Status("Opening settings");
      p.Say("Opening settings.");
      break;
    case SettingsPage::kPrivacy:
      p.Status("Opening privacy settings");
      p.Say("Opening privacy and security settings.");
      break;
    case SettingsPage::kAI:
      p.Status("Opening AI settings");
      p.Say("Opening the AI settings.");
      break;
    case SettingsPage::kVoice:
      p.Status("Opening voice mode settings");
      p.Say(
          "Opening voice mode's settings. I change some of them myself: "
          "say speak faster, be brief, or turn on always listening.");
      return;
    case SettingsPage::kMoltNet:
      p.Status("Opening MoltNet settings");
      p.Say("Opening MoltNet's settings.");
      break;
  }
  p.Say(
      "I can read settings to you, but changing one there needs the mouse "
      "or keyboard.");
}

void AlwaysListening(bool on, TurnPlanner& p) {
  const BrowserEnvironment& b = p.Env().browser;
  if (b.known && b.always_listening == on) {
    p.Say(on ? "Always listening is already on."
             : "Always listening is already off.");
    return;
  }
  VoiceStep set = BrowserStep(BrowserAction::kSetAlwaysListening);
  set.flag = on;
  if (!on) {
    p.Add(std::move(set));
    p.Status("Always listening off");
    p.Say("Always listening is off. Say Hey Molt before what you want.");
    return;
  }
  // Every sentence becomes a command: the user says so first.
  std::vector<VoiceStep> steps;
  steps.push_back(std::move(set));
  steps.push_back(SpeakStep(
      "Always listening is on. Everything you say is a command now, without "
      "Hey Molt."));
  p.Say(
      "With always listening on, everything you say is a command, without "
      "Hey Molt, also what others nearby say.");
  p.AskToConfirm(std::move(steps), "turn on always listening");
}

void ProtectedSettingSaid(ProtectedSetting setting, TurnPlanner& p) {
  switch (setting) {
    case ProtectedSetting::kWordAloud:
      p.Say(
          "Saying the confirmation word aloud lets a page or another "
          "program hear it, so only a click changes that: in settings, "
          "Voice mode, Say the confirmation word aloud.");
      return;
    case ProtectedSetting::kWorkflowTrust:
      p.Say(
          "A workflow's trust changes only with a click, in Agent mode, so "
          "nothing you say or a page plays can change it.");
      return;
  }
}

void NewWindow(WindowKind kind, TurnPlanner& p) {
  VoiceStep open = BrowserStep(BrowserAction::kNewWindow);
  open.index = static_cast<int>(kind);
  p.Add(std::move(open));
  switch (kind) {
    case WindowKind::kNormal:
      p.Status("New window");
      p.Say(
          "New window. I'm still listening in this one: say switch to the "
          "other window to use it by voice.");
      return;
    case WindowKind::kPrivate:
      p.Status("New private window");
      p.Say(
          "New private window. I'm still listening in this one: say "
          "switch to the other window to use it by voice.");
      return;
    case WindowKind::kMoltNet:
      p.Status("Opening a MoltNet window");
      p.Say(
          "Opening a MoltNet window. Voice mode never listens in MoltNet "
          "windows, so I stay in this one.");
      return;
  }
}

void CloseWindow(TurnPlanner& p) {
  const VoiceEnvironment& env = p.Env();
  const size_t tabs = env.tabs.size();
  if (tabs > 1) {
    p.Say(Tr(p, "This window has {0}.",
             {CountOf(tabs, Said(env.language, "tab", {}),
                      Said(env.language, "tabs", {}))}));
  }
  if (env.browser.known && env.browser.other_windows == 0) {
    p.Say(
        "It's the only window: MoltBrowser will close, and I can't start "
        "it again by voice.");
  } else {
    p.Say("I'll go on listening in your other window.");
  }
  std::vector<VoiceStep> steps;
  steps.push_back(BrowserStep(BrowserAction::kCloseWindow));
  p.AskToConfirm(std::move(steps), "close this window");
}

void SwitchWindow(TurnPlanner& p) {
  const BrowserEnvironment& b = p.Env().browser;
  if (b.known && b.other_windows == 0) {
    p.Say("There's no other window. Say new window to open one.");
    return;
  }
  VoiceStep step = BrowserStep(BrowserAction::kSwitchWindow);
  step.count = 1;
  p.Add(std::move(step));
  p.Status("Switching windows");
}

void AskAI(const std::string& question, TurnPlanner& p) {
  const std::string text = PageText(question);
  if (text.empty()) {
    p.Say("Ask what? Say ask the AI, then your question.");
    return;
  }
  VoiceStep ask = BrowserStep(BrowserAction::kAskAI);
  ask.text = text;
  p.Add(std::move(ask));
  p.Status("Asking the AI");
}

void OpenAIChat(TurnPlanner& p) {
  if (p.Env().browser.known && p.Env().browser.ai_chat_open) {
    p.Say(
        "The AI chat is already open. To ask it by voice, say ask the AI, "
        "then your question.");
    return;
  }
  p.Add(BrowserStep(BrowserAction::kOpenAIChat));
  p.Status("Opening the AI chat");
  p.Say(
      "The AI chat is open beside the page. To ask it by voice, say ask "
      "the AI, then your question.");
}

void ListBrowser(BrowserAction action, std::string text, TurnPlanner& p) {
  VoiceStep list = BrowserStep(action);
  list.text = std::move(text);
  p.Add(std::move(list));
  switch (action) {
    case BrowserAction::kListBookmarks:
      p.Status("Looking at your bookmarks");
      break;
    case BrowserAction::kListHistory:
      p.Status("Looking at your history");
      break;
    case BrowserAction::kListDownloads:
      p.Status("Looking at your downloads");
      break;
    default:
      p.Status("Looking at the browser's buttons");
      break;
  }
}

void OpenDownload(const VoiceCommand& command, TurnPlanner& p) {
  const ConversationState& s = p.State();
  if (BrowserListValid(s, p.Env()) &&
      s.browser.list == BrowserListKind::kDownloads) {
    const std::vector<BrowserItem> items = s.browser.items;
    if (!ChooseItem(BrowserListKind::kDownloads, items, command.choice, p)) {
      p.Say("Say the download's number: open download, then its number.");
    }
    return;
  }
  // Not read yet: the browser finds it in the list, newest first.
  const ChoiceRef& ref = command.choice;
  int position = 0;
  if (ref.kind == ChoiceRef::Kind::kNumber) {
    position = ref.number;
  } else if (ref.kind == ChoiceRef::Kind::kPosition && !ref.from_end) {
    position = ref.ordinal;
  }
  if (position < 1) {
    // "The last one" from the end: the oldest shown.
    position = ref.kind == ChoiceRef::Kind::kPosition ? -ref.ordinal : 1;
  }
  VoiceStep list = BrowserStep(BrowserAction::kListDownloads);
  list.index = position;
  p.Add(std::move(list));
  p.Status("Looking at your downloads");
}

}  // namespace

bool BrowserListValid(const ConversationState& state,
                      const VoiceEnvironment& env) {
  return state.browser.list != BrowserListKind::kNone &&
         !state.browser.items.empty() &&
         env.now_ms < state.browser.list_expires_ms;
}

bool PlanBrowserFirst(const VoiceCommand& command, TurnPlanner& p) {
  ConversationState& s = p.State();
  const VoiceEnvironment& env = p.Env();
  // The menu that shows: what is said chooses from it.
  if (env.browser.menu_open && !env.browser.ui.empty()) {
    if (command.kind == K::kCancel || command.kind == K::kStop) {
      p.Add(BrowserStep(BrowserAction::kCloseMenu));
      ClearBrowserList(s.browser);
      return false;  // "Cancelled." as for any cancel.
    }
    if (command.kind == K::kChoose) {
      s.browser.list = BrowserListKind::kBrowserUi;
      s.browser.items = env.browser.ui;
      s.browser.list_expires_ms = env.now_ms + kBrowserListHoldMs;
      if (ChooseItem(BrowserListKind::kBrowserUi, s.browser.items,
                     command.choice, p)) {
        return true;
      }
      p.Say(Tr(p,
               "The menu has nothing called {0}. Say close the menu, or "
               "the name of what to press.",
               {PageText(command.heard)}));
      return true;
    }
  }
  if (!BrowserListValid(s, env)) {
    if (s.browser.list != BrowserListKind::kNone) {
      ClearBrowserList(s.browser);
    }
    return false;
  }
  switch (command.kind) {
    case K::kChoose: {
      const std::vector<BrowserItem> items = s.browser.items;
      if (ChooseItem(s.browser.list, items, command.choice, p)) {
        return true;
      }
      // Nothing of the list: the page's own choice, as before.
      ClearBrowserList(s.browser);
      return false;
    }
    case K::kNextItem:
    case K::kPreviousItem:
      MoveItem(command.kind == K::kNextItem, p);
      return true;
    case K::kRepeat:
    case K::kHelp:
    case K::kWhereAmI:
    case K::kStopSpeaking:
    case K::kSpeakFaster:
    case K::kSpeakSlower:
    case K::kVerbosity:
    case K::kOpenDownload:
      return false;
    default:
      // Anything else: the list is done with.
      ClearBrowserList(s.browser);
      return false;
  }
}

void PlanBrowserCommand(const VoiceCommand& command, TurnPlanner& p) {
  switch (command.kind) {
    case K::kSetSpeed:
      PlanPlaybackRate(K::kSetSpeed, command.rate, p);
      return;
    case K::kStartOver:
      StartOver(p);
      return;
    case K::kNextMatch:
    case K::kPreviousMatch:
      MoveMatch(command.kind == K::kNextMatch, p);
      return;
    case K::kBookmarkPage:
    case K::kRemoveBookmark:
      Bookmark(command.kind == K::kBookmarkPage, p);
      return;
    case K::kListBookmarks:
      ListBrowser(BrowserAction::kListBookmarks, std::string(), p);
      return;
    case K::kOpenBookmark: {
      const std::string name = PageText(command.text);
      if (name.empty()) {
        p.Say("Which bookmark? Say open the bookmark, then its name.");
        return;
      }
      ListBrowser(BrowserAction::kListBookmarks, name, p);
      return;
    }
    case K::kListHistory:
    case K::kOpenFromHistory: {
      if (p.Env().browser.private_window) {
        p.Say("Private windows keep no history.");
        return;
      }
      const std::string what = command.kind == K::kOpenFromHistory
                                   ? PageText(command.text)
                                   : std::string();
      if (command.kind == K::kOpenFromHistory && what.empty()) {
        p.Say("Open what? Say open, then the site, then from my history.");
        return;
      }
      ListBrowser(BrowserAction::kListHistory, what, p);
      return;
    }
    case K::kReopenTab:
      p.Add(BrowserStep(BrowserAction::kReopenTab));
      p.Status("Reopening the closed tab");
      return;
    case K::kListDownloads:
      ListBrowser(BrowserAction::kListDownloads, std::string(), p);
      return;
    case K::kOpenDownload:
      OpenDownload(command, p);
      return;
    case K::kOpenSettings:
      OpenSettings(command.settings_page, p);
      return;
    case K::kAlwaysListening:
      AlwaysListening(command.on, p);
      return;
    case K::kProtectedSetting:
      ProtectedSettingSaid(command.protected_setting, p);
      return;
    case K::kNewWindow:
      NewWindow(command.window, p);
      return;
    case K::kCloseWindow:
      CloseWindow(p);
      return;
    case K::kSwitchWindow:
      SwitchWindow(p);
      return;
    case K::kAskAI:
      AskAI(command.text, p);
      return;
    case K::kOpenAIChat:
      OpenAIChat(p);
      return;
    case K::kShowBrowserButtons:
      ListBrowser(BrowserAction::kListBrowserUi, std::string(), p);
      return;
    case K::kOpenMenu:
      p.Add(BrowserStep(BrowserAction::kOpenMenu));
      p.Add(BrowserStep(BrowserAction::kListBrowserUi));
      p.Status("Opening the menu");
      return;
    case K::kCloseMenu:
      if (p.Env().browser.known && !p.Env().browser.menu_open) {
        p.Say("No menu is open.");
        return;
      }
      ClearBrowserList(p.State().browser);
      p.Add(BrowserStep(BrowserAction::kCloseMenu));
      p.Say("Closed the menu.");
      return;
    default:
      return;
  }
}

void PlanPlaybackRate(VoiceCommandKind kind, int rate, TurnPlanner& p) {
  if (!HasMedia(p)) {
    p.Say("There's no video or audio on this page.");
    return;
  }
  VoiceStep step = BrowserStep(BrowserAction::kSetPlaybackRate);
  step.target = Player(p.Map());
  switch (kind) {
    case K::kSpeedUp:
    case K::kSlowDown:
      step.index = 0;
      step.count = kind == K::kSpeedUp ? 1 : -1;
      break;
    case K::kNormalSpeed:
      step.index = kNormalPlaybackRate;
      break;
    default:
      step.index = ClampPlaybackRate(rate);
      break;
  }
  p.Add(std::move(step));
  p.Status("Changing the speed");
}

void PlanVolumeLevel(int level, TurnPlanner& p) {
  if (!HasMedia(p)) {
    p.Say(
        "There's no video or audio on this page. Say mute or unmute for "
        "the tab's sound.");
    return;
  }
  VoiceStep step = BrowserStep(BrowserAction::kSetVolumeLevel);
  step.target = Player(p.Map());
  step.index = std::clamp(level, 0, 100);
  p.Add(std::move(step));
  p.Status("Setting the volume");
}

bool PlanMediaCaptions(bool on, TurnPlanner& p) {
  if (p.Map().media.empty()) {
    return false;
  }
  VoiceStep step = BrowserStep(BrowserAction::kMediaCaptions);
  step.target = Player(p.Map());
  step.flag = on;
  p.Add(std::move(step));
  return true;
}

void PlanUndo(TurnPlanner& p) {
  const VoiceEnvironment& env = p.Env();
  UndoTake take = TakeUndo(p.State().browser.undo, env.page_id, env.active_tab);
  if (take.in_flight) {
    p.Say("Wait until the page has opened, then say undo.");
    return;
  }
  if (take.stale) {
    p.Say(
        "I can't undo the last thing I did any more: the page changed "
        "since.");
    return;
  }
  if (!take.entry) {
    p.Say(env.can_go_back ? "There's nothing I can undo. Say go back to "
                            "return to the page before."
                          : "There's nothing I can undo.");
    return;
  }
  const UndoEntry& entry = *take.entry;
  p.Status("Undoing");
  switch (entry.kind) {
    case UndoKind::kNone:
      return;
    case UndoKind::kReopenTab: {
      VoiceStep reopen = BrowserStep(BrowserAction::kReopenTab);
      reopen.flag = true;
      reopen.text = entry.name;
      p.Add(std::move(reopen));
      return;
    }
    case UndoKind::kRestoreText: {
      VoiceStep set = Step(VoiceStepKind::kSetText);
      set.target = entry.field;
      set.text = entry.text;
      p.Add(std::move(set));
      const std::string name =
          entry.name.empty() ? Tr(p, "The field", {}) : entry.name;
      p.Say(entry.text.empty() ? Tr(p, "Undone: {0} is empty again.", {name})
                               : Tr(p, "Undone: {0} says {1} again.",
                                    {name, PageText(entry.text)}));
      return;
    }
    case UndoKind::kGoBack:
    case UndoKind::kGoForward: {
      const bool back = entry.kind == UndoKind::kGoBack;
      if (back ? !env.can_go_back : !env.can_go_forward) {
        p.Say(back ? "There's no page to go back to."
                   : "There's no page to go forward to.");
        return;
      }
      VoiceStep go = Step(VoiceStepKind::kBrowserCommand);
      go.command = back ? BrowserCommand::kBack : BrowserCommand::kForward;
      p.Add(std::move(go));
      p.Add(WaitFor(FollowUpKind::kAnnouncePage, std::string(),
                    Tr(p,
                       back ? "Undone: back to the page before."
                            : "Undone: forward again.",
                       {})));
      return;
    }
    case UndoKind::kRemoveBookmark: {
      VoiceStep remove = BrowserStep(BrowserAction::kRemoveBookmark);
      remove.url = entry.url;
      p.Add(std::move(remove));
      p.Say("Undone: the bookmark is removed.");
      return;
    }
    case UndoKind::kRestoreBookmark: {
      VoiceStep add = BrowserStep(BrowserAction::kAddBookmark);
      add.url = entry.url;
      add.text = entry.text;
      p.Add(std::move(add));
      p.Say("Undone: the bookmark is back.");
      return;
    }
  }
}

bool PlanSiteNameAlone(const VoiceCommand& command, TurnPlanner& p) {
  const VoiceEnvironment& env = p.Env();
  ParseContext context;
  context.language = env.language;
  const std::string words = CommandWords(command.heard, context);
  if (words.empty() ||
      static_cast<size_t>(std::count(words.begin(), words.end(), ' ')) + 1 >
          kMaxSiteNameWords) {
    return false;
  }
  std::string joined;
  for (char c : words) {
    if (c != ' ') {
      joined += c;
    }
  }
  std::string name;
  std::string url;
  if (const KnownSite* site = FindKnownSite(joined)) {
    name = std::string(site->name);
    url = std::string(site->home_url);
  } else {
    const std::vector<OpenedSite>& opened = p.State().browser.opened_sites;
    for (auto it = opened.rbegin(); it != opened.rend(); ++it) {
      if (it->words == words || it->host_name == joined) {
        name = it->name;
        url = it->url;
        break;
      }
    }
  }
  if (url.empty()) {
    return false;
  }
  VoiceStep open = Step(VoiceStepKind::kNavigate);
  open.url = url;
  p.Add(std::move(open));
  const std::string spoken = PageText(name);
  p.Status(Tr(p, "Opening {0}", {spoken}));
  p.Add(WaitFor(FollowUpKind::kAnnounceOpened, spoken,
                Tr(p, "Opening {0}.", {spoken})));
  return true;
}

void NoteOpenedSite(ConversationState& state,
                    std::string_view said,
                    std::string_view name,
                    std::string_view url) {
  if (url.empty() || name.empty()) {
    return;
  }
  OpenedSite site;
  site.words = FoldForComparison(said);
  site.name = std::string(name);
  site.url = std::string(url);
  // The host's own name: "bbc" for https://www.bbc.co.uk/news.
  std::string_view host = url;
  if (const size_t scheme = host.find("://");
      scheme != std::string_view::npos) {
    host.remove_prefix(scheme + 3);
  }
  host = host.substr(0, host.find_first_of("/?#:"));
  if (host.substr(0, 4) == "www.") {
    host.remove_prefix(4);
  }
  site.host_name = FoldForComparison(host.substr(0, host.find('.')));
  std::vector<OpenedSite>& opened = state.browser.opened_sites;
  std::erase_if(opened, [&](const OpenedSite& o) { return o.url == site.url; });
  opened.push_back(std::move(site));
  while (opened.size() > kMaxOpenedSites) {
    opened.erase(opened.begin());
  }
}

void PlanBrowserItemsRead(const VoiceStep& asked,
                          std::vector<BrowserItem> items,
                          TurnPlanner& p) {
  const BrowserListKind kind = ListKindFor(asked.browser_action);
  if (kind == BrowserListKind::kNone) {
    return;
  }
  if (items.size() > kMaxBrowserItems) {
    items.resize(kMaxBrowserItems);
  }
  const VoiceEnvironment& env = p.Env();
  const bool menu = kind == BrowserListKind::kBrowserUi && !items.empty() &&
                    items.front().region == BrowserUiRegion::kMenu;
  if (items.empty()) {
    switch (kind) {
      case BrowserListKind::kBookmarks:
        p.Say(asked.text.empty()
                  ? std::string("You have no bookmarks yet. Say bookmark this "
                                "page to add one.")
                  : Tr(p, "I can't find {0} in your bookmarks.",
                       {PageText(asked.text)}));
        return;
      case BrowserListKind::kHistory:
        p.Say(asked.text.empty() ? std::string("Your history is empty.")
                                 : Tr(p, "I can't find {0} in your history.",
                                      {PageText(asked.text)}));
        return;
      case BrowserListKind::kDownloads:
        p.Say("You have no downloads.");
        return;
      case BrowserListKind::kBrowserUi:
        p.Say("I can't find any of the browser's buttons to press here.");
        return;
      case BrowserListKind::kNone:
        return;
    }
  }
  ConversationState& s = p.State();
  // The page's list is no longer the one to choose from.
  s.list = ListContext::kNone;
  s.offered.clear();
  s.expecting_choice = false;
  s.browser.list = kind;
  s.browser.items = items;
  s.browser.list_expires_ms = env.now_ms + kBrowserListHoldMs;
  s.browser.current = 0;
  s.browser.offered.clear();
  // A name to find in it ("open the bookmark news").
  if (!asked.text.empty()) {
    std::optional<ChoiceRef> ref =
        ParseChoiceRef(asked.text, env.language, /*bare_numbers=*/false);
    if (ref && ChooseItem(kind, s.browser.items, *ref, p)) {
      return;
    }
    p.Say(Tr(p,
             kind == BrowserListKind::kHistory
                 ? "I can't find {0} in your history."
                 : "I can't find {0} in your bookmarks.",
             {PageText(asked.text)}));
    return;
  }
  // A download by its place ("open download 3", "open the last download").
  if (kind == BrowserListKind::kDownloads && asked.index != 0) {
    const int n = static_cast<int>(items.size());
    const int at = asked.index > 0 ? asked.index : n + 1 + asked.index;
    if (at < 1 || at > n) {
      p.Say(Tr(p, "There's no download {0}. You have {1}.",
               {std::to_string(asked.index > 0 ? asked.index : -asked.index),
                CountOf(items.size(), Said(env.language, "download", {}),
                        Said(env.language, "downloads", {}))}));
      return;
    }
    ActOnItem(kind, s.browser.items, static_cast<size_t>(at - 1), p);
    return;
  }
  const auto [one, many] = ListWords(kind, menu);
  const CandidateSet set = ItemSet(kind, items, env.language);
  // The browser's buttons and a menu are read whole: there is no other
  // way to hear what they hold.
  const Verbosity verbosity =
      kind == BrowserListKind::kBrowserUi ? Verbosity::kFull : s.verbosity;
  p.Status(menu ? std::string("Menu open") : std::string("Reading the list"));
  for (const std::string& chunk : ListSpeech(
           set, Said(env.language, one, {}), Said(env.language, many, {}),
           verbosity, kNormalResultsRead, env.language)) {
    p.Say(chunk);
  }
}

void NoteFindMatches(ConversationState& state,
                     std::string_view query,
                     std::vector<FindMatch> matches,
                     int64_t page_id) {
  if (matches.size() > kMaxFindMatches) {
    matches.resize(kMaxFindMatches);
  }
  state.browser.find_query = std::string(query);
  state.browser.find_matches = std::move(matches);
  state.browser.find_index = state.browser.find_matches.empty() ? 0 : 1;
  state.browser.find_page_id = page_id;
}

void NoteNavigationForUndo(VoiceCommandKind kind,
                           const std::vector<VoiceStep>& steps,
                           const VoiceEnvironment& env,
                           ConversationState& state) {
  if (kind == K::kReload || kind == K::kStopLoading || kind == K::kUndo ||
      env.moltnet) {
    return;
  }
  for (const VoiceStep& step : steps) {
    if (step.kind == VoiceStepKind::kWaitForPage) {
      NoteUndo(state.browser.undo,
               PendingNavigation(env.page_id, env.active_tab,
                                 /*back=*/kind == K::kBack));
      return;
    }
  }
}

std::vector<std::string> WhereAmIContext(const ConversationState& state,
                                         const PageMap& map,
                                         const VoiceEnvironment& env) {
  std::vector<std::string> said;
  const Language language = env.language;
  if (env.browser.menu_open) {
    said.push_back(std::string(Translated(
        language,
        "The browser's menu is open: say what to press, or close the menu.")));
  }
  if (PromptWaits(state.prompt, env.now_ms)) {
    said.push_back(std::string(Translated(
        language, "A question from the browser is waiting for your answer.")));
  }
  if (BrowserListValid(state, env)) {
    const auto [one, many] = ListWords(state.browser.list, false);
    said.push_back(state.browser.current > 0
                       ? Said(language,
                              "You're in the list of {0}, at {1} of {2}.",
                              {Said(language, many, {}),
                               std::to_string(state.browser.current),
                               std::to_string(state.browser.items.size())})
                       : Said(language, "You're in the list of {0}: {1}.",
                              {Said(language, many, {}),
                               std::to_string(state.browser.items.size())}));
  } else if (state.list != ListContext::kNone &&
             (state.list == ListContext::kTabs ||
              state.list_page_id == env.page_id) &&
             state.current_position > 0) {
    said.push_back(Said(language, "You're at number {0} of the list.",
                        {std::to_string(state.current_position)}));
  }
  if (state.field.valid() && state.field_page_id == env.page_id) {
    if (const PageItem* field = map.ItemForHandle(state.field)) {
      said.push_back(Said(
          language, "You're typing in {0}.",
          {PageText(field->label.empty() ? Said(language, "a text field", {})
                                         : field->label)}));
    }
  }
  if (env.media_playing) {
    said.push_back(std::string(Translated(language, "Something is playing.")));
  } else if (env.media_session) {
    said.push_back(std::string(
        Translated(language, "The video is paused: say play to go on.")));
  }
  if (env.reading == VoiceEnvironment::Reading::kStopped) {
    said.push_back(std::string(Translated(
        language, "I stopped reading this page: say continue reading.")));
  }
  return said;
}

void AddBrowserHelpKinds(const ConversationState& state,
                         const VoiceEnvironment& env,
                         std::vector<VoiceCommandKind>& kinds) {
  std::vector<VoiceCommandKind> first;
  if (env.browser.menu_open) {
    first = {K::kChoose, K::kCloseMenu};
  } else if (BrowserListValid(state, env)) {
    first = {K::kChoose, K::kNextItem};
  }
  if (env.reading == VoiceEnvironment::Reading::kReading ||
      env.reading == VoiceEnvironment::Reading::kStopped) {
    first.insert(first.end(),
                 {K::kReadFromHere, K::kNextParagraph, K::kSpeakFaster});
  }
  if (env.media_session || env.media_playing) {
    first.insert(first.end(), {K::kSetVolume, K::kSetSpeed});
  }
  for (VoiceCommandKind kind : kinds) {
    if (std::find(first.begin(), first.end(), kind) == first.end()) {
      first.push_back(kind);
    }
  }
  first.push_back(K::kUndo);
  kinds = std::move(first);
}

std::string BrowserHelpContext(const ConversationState& state,
                               const PageMap& map,
                               const VoiceEnvironment& env,
                               bool typing) {
  std::string out;
  const Language language = env.language;
  if (PromptWaits(state.prompt, env.now_ms)) {
    out += std::string(Translated(
        language,
        "A question from the browser is waiting: answer it, or say no."));
  }
  if (typing) {
    if (!out.empty()) {
      out += ' ';
    }
    out += std::string(Translated(
        language,
        "When you type, say comma, period, question mark or new line for "
        "punctuation, and the word comma for the word itself."));
  }
  return out;
}

}  // namespace voice
}  // namespace molt_ai
