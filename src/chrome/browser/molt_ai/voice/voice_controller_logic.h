// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The voice loop's decisions (docs/design/TOUCHLESS_BROWSING.md §2, §5,
// §6.2, §6.5, §7): from what the user said, the page map, what the
// browser knows about the window and the conversation so far, the steps
// to run and the words to say. Pure and std-only, so every decision is
// unit-tested; voice_controller.h runs the steps in a browser window.
//
//   PlanUtterance("search for Bohemian Rhapsody", map, env, state)
//     -> [SetText(search box, "Bohemian Rhapsody"), Submit(search box),
//         WaitForPage(read the results of "Bohemian Rhapsody")]
//   PlanFollowUp(that wait step, the results page's map, ...)
//     -> [Speak("20 results. 1: ... Say a number or a name.")]
//   PlanUtterance("play the fourth one", ...)
//     -> [Press(the fourth result's link), WaitForPage(play what opens)]
//
// What it holds to:
//   - Trust stays with the browser. A site's name or address becomes a URL
//     only through voice_sites.h (the browser checks it with agent_safety
//     before loading it); a name it doesn't know is searched for with the
//     default search engine, and the browser says so. Choices are resolved
//     against the page map by the matcher; an action names a node of the
//     map, which the page controller finds again before acting.
//   - Page text is only ever read out. It never becomes a command, a URL
//     or a step: the steps come from the grammar's command and the map's
//     structure.
//   - Before pressing anything that can't be undone (buy, pay, order,
//     delete, send, post, subscribe, sign out, ...; NeedsConfirmation()),
//     and before submitting a form that isn't a search, the browser says
//     "Say yes to press <name>" and acts only on a "yes" heard in that
//     context, on the same page, within kConfirmationTimeoutMs.
//   - Nothing from a credential field is spoken or typed by voice.
//   - In a MoltNet window voice mode is off (§7.4): nothing is done, and
//     the browser says so.
//
// The planner (voice_planner.h): anything the grammar leaves kUnknown, and
// routines, get a kToPlanner step, where the web agent's on-device planner
// takes the utterance and the map (without a planner the browser says it
// can't do that yet). Its questions go through the same confirmation:
// PlanPlannerQuestion() holds a kPlannerAnswer step for "yes", and "no"
// answers it with one too.
//
// Site skills (site_skills.h, TOUCHLESS_BROWSING.md §6.7): the parts a
// site's skill named on the page (PageMap::site) come before the map's own
// guesses: its search box and submit, its results as the main list, its
// player's skip-ad, captions and full-screen buttons, its next page.
//
// Reading aloud (page_reading.h, voice_reader.h): "read the page", "read
// from here", "read the selection", "read the section on history", "next
// heading" and "next paragraph" become a kRead step the reader carries out
// with the voice; "pause", "stop", "continue reading", "faster" and
// "slower" while it reads keep or resume its place (VoiceEnvironment::
// reading).
//
// The activation guard (molt_ai/security/activation_guard.h,
// TOUCHLESS_BROWSING.md §7.5) asks the same way when a page tries to use
// the activation voice mode's actions gave it (a new window, a download,
// another app): PlanPageRequestQuestion() holds a kPageRequestAnswer step.
// The presses voice mode makes for full screen and to play carry that
// intent (VoiceStep::intent), so the page may then go full screen.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CONTROLLER_LOGIC_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CONTROLLER_LOGIC_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/security/activation_policy.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/spoken_choice.h"
#include "chrome/browser/molt_ai/voice/voice_candidates.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"

namespace molt_ai {
namespace voice {

// The speech rate (1 is the voice's normal rate) and its bounds; "speak
// faster" multiplies it by kSpeechRateStep.
inline constexpr double kMinSpeechRate = 0.5;
inline constexpr double kMaxSpeechRate = 2.5;
inline constexpr double kSpeechRateStep = 1.25;
// How long the browser waits for the "yes" to a confirmation.
inline constexpr int64_t kConfirmationTimeoutMs = 30000;
// The player's default skip.
inline constexpr int kDefaultSkipSeconds = 10;
// Increments per "louder" or "quieter" on a page's volume slider.
inline constexpr int kVolumeSteps = 2;

// ---------------------------------------------------------------------------
// Steps
// ---------------------------------------------------------------------------

enum class VoiceStepKind : uint8_t {
  kSpeak,           // |text|
  kNavigate,        // |url|, |new_tab|
  kSearchWeb,       // |text| with the default search engine; |new_tab|
  kNewTab,          // an empty new tab
  kSetText,         // |target| := |text|
  kSubmit,          // |target|'s form
  kPress,           // |target|'s default action
  kPlayMedia,       // |target|, a <video> or <audio>
  kFocus,           // |target|
  kScrollIntoView,  // |target|
  kIncrement,       // |target| (a slider), |count| times
  kDecrement,       // |target| (a slider), |count| times
  kScrollPage,      // |scroll|, |count| pages (|half|: half a page)
  kBrowserCommand,  // |command|
  kMedia,           // |media| (|seconds| for kSeek)
  kMute,            // |flag|: mute or unmute the tab
  kSelectTab,       // |index|: 0-based
  kCloseTab,        // |index|: 0-based, -1 for the active tab
  kShowNumbers,     // |flag|: the numbers overlay on or off
  kSetCurrentItem,  // |index|: the item number the overlay rings (0: none)
  // Wait until the navigation the steps before it started has loaded and
  // the page's tree has settled (or nothing navigated), then plan
  // |follow_up| with PlanFollowUp().
  kWaitForPage,
  // Hand |text| (what was heard) to the on-device planner; without one,
  // say |preface|.
  kToPlanner,
  // |flag|: start (true) or stop (false) listening (voice_session.h).
  kSetVoiceMode,
  // The answer to the planner's question |index|: |flag| yes (true) or no.
  kPlannerAnswer,
  // The answer to the activation guard's question |index| (a page's new
  // window, download or app): |flag| yes (true) or no.
  kPageRequestAnswer,
  // Reading aloud (voice_reader.h): |read|, with |text| the section's name
  // for ReadOp::kSection.
  kRead,
};

// What a kRead step asks of the reader.
enum class ReadOp : uint8_t {
  kPage,      // the page's text from its start
  kFromHere,  // from the focused item, else where reading stopped
  kResume,    // where reading stopped
  kSelection,
  kSection,  // the section |text| names, to its end
  kNextHeading,
  kPreviousHeading,
  kNextParagraph,
  kPreviousParagraph,
};

enum class BrowserCommand : uint8_t {
  kBack,
  kForward,
  kReload,
  kStopLoading,
  kHome,
  kZoomIn,
  kZoomOut,
  kZoomReset,
  kToggleFullScreen,
};

enum class MediaCommand : uint8_t {
  kPause,
  kResume,
  kSeek,         // by |seconds| (negative: back)
  kSeekToStart,
  kNextTrack,
  kPreviousTrack,
};

enum class PageScroll : uint8_t {
  kDown,
  kUp,
  kLeft,
  kRight,
  kTop,
  kBottom,
};

// What to do once a page has loaded.
enum class FollowUpKind : uint8_t {
  kNone,
  kAnnounceOpened,  // "<subject> is open."
  kAnnouncePage,    // the title, and its list ("20 items. Say ...")
  kReadResults,     // a search for <subject>: read its results
  kPlayOpened,      // a video page opened for <subject>: play it
};

struct VoiceStep {
  VoiceStep();
  ~VoiceStep();
  VoiceStep(const VoiceStep&);
  VoiceStep& operator=(const VoiceStep&);
  VoiceStep(VoiceStep&&);
  VoiceStep& operator=(VoiceStep&&);

  VoiceStepKind kind = VoiceStepKind::kSpeak;
  // kSpeak: the words. kSearchWeb: the query. kSetText: the text.
  // kToPlanner: what was heard. Actions on a node: its name, for the
  // caption.
  std::string text;
  // kNavigate.
  std::string url;
  bool new_tab = false;
  // Actions on a node of the map.
  PageNodeHandle target;
  // kPress: what the user asked for, when the browser knows (full screen,
  // playing a video): the page may go full screen then
  // (activation_policy.h).
  activation::Intent intent = activation::Intent::kNone;
  // kRead.
  ReadOp read = ReadOp::kPage;
  int count = 1;
  int index = 0;
  int seconds = 0;
  bool flag = false;
  bool half = false;
  BrowserCommand command = BrowserCommand::kBack;
  MediaCommand media = MediaCommand::kPause;
  PageScroll scroll = PageScroll::kDown;
  // kWaitForPage.
  FollowUpKind follow_up = FollowUpKind::kNone;
  std::string subject;
  // kWaitForPage: said before the follow-up's own words. kToPlanner: said
  // when there is no planner.
  std::string preface;
};

// Why a step couldn't be carried out (the page controller's results and
// the browser's own checks). The steps after it are dropped.
enum class StepFailure : uint8_t {
  kNotActive,
  kStale,
  kNotAllowed,
  kDisabled,
  kNotEditable,
  kCredentialField,
  kTooLong,
  kNotMedia,
  kNoMediaSession,
  kFocusNotConfirmed,
  kCancelled,
  kBlockedURL,
  kNoTab,
  kUnavailable,
};

// What the browser says when a step failed; empty for kCancelled.
std::string_view FailureText(StepFailure failure);

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

// What the browser knows about the window and its active tab when the user
// speaks. The glue fills it; page text in it (tab titles) is cleaned
// first.
struct VoiceEnvironment {
  VoiceEnvironment();
  ~VoiceEnvironment();
  VoiceEnvironment(const VoiceEnvironment&);
  VoiceEnvironment& operator=(const VoiceEnvironment&);
  VoiceEnvironment(VoiceEnvironment&&);
  VoiceEnvironment& operator=(VoiceEnvironment&&);

  // A MoltNet window: voice mode is off there (§7.4).
  bool moltnet = false;
  // Changes with every new document in the active tab, and when another
  // tab becomes active. Lists, fields and confirmations hold only on the
  // page they were made on.
  int64_t page_id = 0;
  // The active tab's host, and whether it shows an http(s) page (actions
  // on other pages are refused).
  std::string host;
  bool web_page = true;
  bool can_go_back = false;
  bool can_go_forward = false;
  // Media in the tab: whether something has played (the media session can
  // pause, resume and seek it), and whether it plays now.
  bool media_session = false;
  bool media_playing = false;
  bool muted = false;
  // The window is in full screen.
  bool fullscreen = false;
  // The browser listens to the microphone (voice_session.h); false while
  // commands are only typed.
  bool listening = false;
  // Reading aloud (voice_reader.h): it was reading when this was said (the
  // new command stopped it, keeping its place), or it stopped earlier on
  // this page and can go on.
  enum class Reading : uint8_t { kNone, kReading, kStopped };
  Reading reading = Reading::kNone;
  std::vector<TabInfo> tabs;
  int active_tab = 0;
  // A monotonic clock, in milliseconds.
  int64_t now_ms = 0;
};

// What the user and the browser said so far that matters for what comes
// next.
enum class ListContext : uint8_t {
  kNone,
  kPageList,  // one of PageMap::lists (|list_container|, |list_label|)
  kItems,     // every item (close choices offered from them)
  kLinks,
  kHeadings,
  kTabs,
};

struct ConversationState {
  ConversationState();
  ~ConversationState();
  ConversationState(const ConversationState&);
  ConversationState& operator=(const ConversationState&);
  ConversationState(ConversationState&&);
  ConversationState& operator=(ConversationState&&);

  // The list the browser read last, on |list_page_id|.
  ListContext list = ListContext::kNone;
  PageNodeHandle list_container;
  std::string list_label;
  int64_t list_page_id = -1;
  // After an unclear choice: the close candidates read back (indices into
  // the list's candidates, in the order read), the nodes they named then
  // (an answer counts only while they still name them), and the verb to
  // apply.
  std::vector<size_t> offered;
  std::vector<PageNodeHandle> offered_handles;
  ChoiceVerb offered_verb = ChoiceVerb::kDefault;
  // The list's entry last chosen or read (1-based), for "the next one".
  int current_position = 0;
  // A bare number or description now picks from the list.
  bool expecting_choice = false;

  // A confirmation the browser asked for: |confirm_steps| run on "yes".
  bool confirming = false;
  std::vector<VoiceStep> confirm_steps;
  std::string confirm_what;
  int64_t confirm_page_id = -1;
  int64_t confirm_expires_ms = 0;

  // The text field being typed into.
  PageNodeHandle field;
  int64_t field_page_id = -1;

  // "Next heading", "next link": 1-based positions on |cursor_page_id|.
  int heading_cursor = 0;
  int link_cursor = 0;
  int64_t cursor_page_id = -1;

  bool numbers_shown = false;
  // kBrief, kNormal or kFull.
  Verbosity verbosity = Verbosity::kNormal;
  double speech_rate = 1.0;
  // What the browser said last, for "repeat", as it was spoken.
  std::vector<std::string> last_spoken;
};

// One turn of the conversation.
struct VoiceTurn {
  VoiceTurn();
  ~VoiceTurn();
  VoiceTurn(const VoiceTurn&);
  VoiceTurn& operator=(const VoiceTurn&);
  VoiceTurn(VoiceTurn&&);
  VoiceTurn& operator=(VoiceTurn&&);

  // What was understood (the parsed command; kUnknown for a follow-up).
  VoiceCommandKind kind = VoiceCommandKind::kUnknown;
  // What was heard, cleaned and capped (the caption's "Heard: ...").
  std::string heard;
  // What the browser is doing, for the caption ("Opening YouTube").
  std::string status;
  std::vector<VoiceStep> steps;
  // The state after this turn.
  ConversationState state;
};

// The grammar's context for the next utterance: a yes/no question pending
// on this page, a list to choose from (or numbers shown), a text field
// being typed into.
ParseContext ContextFor(const ConversationState& state,
                        const PageMap& map,
                        const VoiceEnvironment& env);

// One utterance (text, as typed or recognized): parsed with ContextFor()
// and planned.
VoiceTurn PlanUtterance(std::string_view utterance,
                        const PageMap& map,
                        const VoiceEnvironment& env,
                        const ConversationState& state);

// A command the grammar already parsed.
VoiceTurn PlanCommand(const VoiceCommand& command,
                      const PageMap& map,
                      const VoiceEnvironment& env,
                      const ConversationState& state);

// After a kWaitForPage step: |map| is the page that loaded.
VoiceTurn PlanFollowUp(const VoiceStep& wait,
                       const PageMap& map,
                       const VoiceEnvironment& env,
                       const ConversationState& state);

// The planner (voice_planner.h) asks the user to confirm |what| (its words,
// built from model output and page text: cleaned here), as the loop asks
// before pressing Buy: "Say yes to <what>, or no to cancel.", on this page,
// within kConfirmationTimeoutMs. A "yes" then runs a kPlannerAnswer step
// (|index| |question|, |flag| true), a "no" one with |flag| false; any other
// command drops the question. Nothing in a MoltNet window.
VoiceTurn PlanPlannerQuestion(int question,
                              std::string_view what,
                              const VoiceEnvironment& env,
                              const ConversationState& state);

// The planner question |state| holds for its "yes", or 0 if none.
int PlannerQuestionHeld(const ConversationState& state);

// The activation guard asks about what the page tried with the activation
// voice mode's action gave it: |said| ("The page tried to open a new window
// to ads.example.", the browser's words with the site cleaned; cleaned again
// here), then "Say yes to |verb|, or no to cancel." ("open it"), on this
// page, within kConfirmationTimeoutMs. A "yes" runs a kPageRequestAnswer
// step (|index| |question|, |flag| true), a "no" one with |flag| false; any
// other command drops the question. Nothing in a MoltNet window.
VoiceTurn PlanPageRequestQuestion(int question,
                                  std::string_view said,
                                  std::string_view verb,
                                  const VoiceEnvironment& env,
                                  const ConversationState& state);

// The activation guard's question |state| holds for its "yes", or 0.
int PageRequestQuestionHeld(const ConversationState& state);

// Whether pressing an item named |label| (linking to |url|, raw page data)
// on |map| needs a spoken "yes" first: its words buy, purchase, pay,
// order, checkout, delete, remove, erase, discard, trash, send, post,
// publish, reply, comment, tweet, submit, confirm, subscribe, unsubscribe,
// donate, transfer, sign out or log out (as whole words, any case); a link
// to a sign-out or log-out address; or anything pressed on a page with a
// payment-card field (agent_safety treats those as sensitive).
bool NeedsConfirmation(std::string_view label,
                       std::string_view url,
                       const PageMap& map);

// The page's search field: the site skill's (PageMap::site), else a
// writable text field of the top-level document (never of a frame it
// embeds) the map marks as a search field, on screen first; null if there
// is none.
const PageItem* FindSearchField(const PageMap& map);

// The item a site part names (PageMap::site), if it is an item of the
// top-level document that can be pressed now (not disabled): null
// otherwise.
const PageItem* SitePart(const PageMap& map, const PageNodeHandle& part);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CONTROLLER_LOGIC_H_
