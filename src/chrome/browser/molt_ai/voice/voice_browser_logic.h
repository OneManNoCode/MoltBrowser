// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The voice loop's decisions for the rest of the browser
// (docs/design/TOUCHLESS_BROWSING.md §2): what the grammar's newer commands
// plan, beside voice_controller_logic.h's, through the same turn
// (TurnPlanner: what it says, the steps, the state, its confirmations).
//
//   - Media: the playback speed ("speed 1.5", "faster" while nothing is
//     read, "normal speed"), the volume to a level, and captions on a
//     player with no captions button: a kBrowserAction step on the page's
//     media element (voice_media_controls.h).
//   - Recover: "undo" (voice_undo.h), "start over", and the context "where
//     am I?" and "what can I say?" add (a list being read, a field, a
//     player, a menu, a question from the browser).
//   - Find: "next match" and "previous match" through the last find's
//     matches on the page.
//   - The browser itself: bookmarks, history, downloads, settings, windows,
//     the AI chat and the browser's own buttons and menus. Lists are
//     gathered by the browser (a kBrowserAction step) and read with
//     PlanBrowserItemsRead(); a number or a name then chooses from them,
//     as from a page's list.
//   - A site's name on its own ("YouTube"): opened, with a readback, when
//     it is a site the browser knows by name or one opened by voice in
//     this window; anything else stays the planner's.
//
// Trust (§7): nothing a page says or shows becomes one of these actions;
// closing a window, turning always listening on and pressing the
// browser's "Exit" ask first; a downloaded file that could run code isn't
// opened by voice; the settings that weaken a protection are only said to
// be click-only. Voice mode is off in MoltNet windows ("new MoltNet window"
// only opens one).
//
// Pure and std-only; unit-tested in the voice harness and
// molt_ai_unittests (voice_browser_logic_unittest.cc).

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_LOGIC_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_LOGIC_H_

#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_browser_types.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"

namespace molt_ai {
namespace voice {

// The turn being planned (voice_controller_logic.cc's planner).
class TurnPlanner {
 public:
  virtual const PageMap& Map() const = 0;
  virtual const VoiceEnvironment& Env() const = 0;
  virtual ConversationState& State() = 0;
  // Says |text| in the window's language (a constant sentence is looked up
  // whole; build others with Said()), cleaned for speech.
  virtual void Say(std::string text) = 0;
  virtual void Status(std::string status) = 0;
  virtual void Add(VoiceStep step) = 0;
  // "Say yes to |what|, or no to cancel.": |steps| run on the "yes".
  virtual void AskToConfirm(std::vector<VoiceStep> steps,
                            const std::string& what) = 0;
  // The overlay rings item |number| (0: none).
  virtual void SetCurrentItem(int number) = 0;

 protected:
  virtual ~TurnPlanner() = default;
};

// Whether |command| is for the browser's list read last (a number or a
// name chooses from it, "next" moves through it) or for the menu that
// shows ("click Settings"; "cancel" closes it first): planned, and true,
// when it is. Before the grammar's own planning.
bool PlanBrowserFirst(const VoiceCommand& command, TurnPlanner& planner);

// The grammar's commands for the rest of the browser (kSetSpeed to
// kCloseMenu, voice_command.h).
void PlanBrowserCommand(const VoiceCommand& command, TurnPlanner& planner);

// Media. |kind| kSpeedUp or kSlowDown (one step), kNormalSpeed, or kSetSpeed
// with |rate| (hundredths).
void PlanPlaybackRate(VoiceCommandKind kind, int rate, TurnPlanner& planner);
// The volume to |level| (0..100).
void PlanVolumeLevel(int level, TurnPlanner& planner);
// Captions on the media element itself, for a player with no captions
// button: false (nothing planned) when the page has no media.
bool PlanMediaCaptions(bool on, TurnPlanner& planner);

// "Undo".
void PlanUndo(TurnPlanner& planner);

// A site's name on its own: true when |command| (kUnknown) was one and it
// is opened.
bool PlanSiteNameAlone(const VoiceCommand& command, TurnPlanner& planner);
// Voice mode opened |name| (as the browser says it) at |url|, said as
// |said|: "YouTube" alone opens it again in this window.
void NoteOpenedSite(ConversationState& state,
                    std::string_view said,
                    std::string_view name,
                    std::string_view url);

// The browser's list the glue gathered for |asked| (a kBrowserAction step:
// kListBookmarks, kListHistory, kListDownloads, kListBrowserUi): read to
// choose from, or the entry |asked| names opened.
void PlanBrowserItemsRead(const VoiceStep& asked,
                          std::vector<BrowserItem> items,
                          TurnPlanner& planner);

// Whether a downloaded file named |file_name| could run code on this
// computer when opened (a program, an installer, a script, a shortcut, a
// disk image, an archive of an app; its extension, any case, the last of
// several): voice mode doesn't open those. A name without an extension
// counts as one that could.
bool FileCouldRunCode(std::string_view file_name);

// Whether the browser's list read last still holds.
bool BrowserListValid(const ConversationState& state,
                      const VoiceEnvironment& env);

// "Find X on this page" found |matches|: kept for "next match" (the first
// is shown now).
void NoteFindMatches(ConversationState& state,
                     std::string_view query,
                     std::vector<FindMatch> matches,
                     int64_t page_id);

// The pending navigation a turn of |kind| with a wait for a page notes for
// "undo" (voice_undo.h); nothing for a reload, a stop or an undo itself.
void NoteNavigationForUndo(VoiceCommandKind kind,
                           const std::vector<VoiceStep>& steps,
                           const VoiceEnvironment& env,
                           ConversationState& state);

// What "where am I?" adds for the context: the list being read and the
// place in it, the field being typed into, a player, the browser's menu, a
// question from the browser, a reading stopped.
std::vector<std::string> WhereAmIContext(const ConversationState& state,
                                         const PageMap& map,
                                         const VoiceEnvironment& env);

// The commands "what can I say?" puts first for the context (a menu, the
// browser's list read last, a reading), and a sentence it adds (typing's
// punctuation, a question from the browser); empty when none.
void AddBrowserHelpKinds(const ConversationState& state,
                         const VoiceEnvironment& env,
                         std::vector<VoiceCommandKind>& kinds);
std::string BrowserHelpContext(const ConversationState& state,
                               const PageMap& map,
                               const VoiceEnvironment& env,
                               bool typing);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_LOGIC_H_
