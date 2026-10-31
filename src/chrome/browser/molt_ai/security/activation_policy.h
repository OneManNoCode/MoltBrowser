// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What a page may do with the user activation an agent's or voice mode's
// accessibility action gives it (docs/design/TOUCHLESS_BROWSING.md §7.5,
// docs/design/AGENT_ACTIONS.md §2.10.8).
//
// Blink at 156 takes the accessibility actions voice mode and the web agent
// send (kDoDefault, kFocus, kIncrement, kDecrement) as the user's own
// interaction, and so does the Enter key voice mode sends to submit a
// field: the frame acted on, its ancestors and the frames of its origin get
// transient user activation, for 5 seconds. That is what lets a video
// started by voice play with sound. But the page's own script can use the
// same activation for things the user never asked for.
//
// A tab is "acting" from each such action until the user's own next input
// on the page that gives it activation of its own (a key other than Esc, a
// click, a touch; not a scroll), or until kActingWindowMs have
// passed since the later of the action and the activation it gave a frame,
// whichever comes first. While it is, what the activation would unlock is
// decided here:
//
//   request             while acting
//   new tab or window   asked about (refused now, opened on "yes"), unless
//                       the action was the user's request to open a link in
//                       a new tab
//   download            asked about (held until the answer)
//   another app         asked about (refused now, launched on "yes")
//   permission prompt   voice mode: asked by voice (the voice gesture,
//                       TOUCHLESS_BROWSING.md §7.3) when it takes prompts
//                       by voice, else refused, and said; otherwise shown
//                       as usual, for the user to answer: never answered
//                       for them
//   full screen         refused, unless the user asked for full screen or
//                       for media to play
//   clipboard write     refused, unless the user asked for a copy
//
// With no one to ask (no voice mode on the tab, no agent run), what would be
// asked about is refused. Outside that state everything is decided as for a
// person's click. Plain clicks by a person never start it.
//
// The page keeps sticky activation from an action for as long as it shows
// the same document, which is what lets it add session history entries
// that "go back" doesn't skip (Chromium's history manipulation
// intervention skips only entries a document added without it). While the
// page's activation comes only from actions (no input of the user's
// since), the entries it adds count as skippable for voice mode's "go
// back", except the first one an action leads to within its window (the
// page the user asked for): NoteHistoryEntry(). An action whose intent
// never navigates the tab (full screen, a copy, a link opened in a new
// tab) leads to no such page: every entry the page adds after it is
// skippable. Otherwise, at 156, the browser can't tell the page the action
// led to from an entry the page adds for itself in the same click (both
// are the page's script, with the action's activation; see
// TOUCHLESS_BROWSING.md §7.5), so the first one is taken as the user's.
//
// Std-only, so molt_ai_unittests and a standalone harness build the same
// code. The browser's side (the tab's record, the hooks, the questions) is
// activation_guard.h.

#ifndef CHROME_BROWSER_MOLT_AI_SECURITY_ACTIVATION_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_SECURITY_ACTIVATION_POLICY_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace molt_ai {
namespace activation {

// Blink's transient user activation lasts 5 s (blink::kActivationLifespan,
// third_party/blink/public/common/frame/user_activation_state.h; 60 s in
// MemorySanitizer builds), and the browser keeps its own copy with the same
// lifespan. A request the page made while its activation was alive can reach
// the browser a little later: kInFlightMs more.
inline constexpr int64_t kTransientActivationMs = 5000;
inline constexpr int64_t kInFlightMs = 2000;
inline constexpr int64_t kActingWindowMs = kTransientActivationMs + kInFlightMs;

// What the user asked an action for, when the browser knows it.
enum class Intent : uint8_t {
  kNone,
  kFullScreen,    // "full screen": the page's own full screen button
  kPlayMedia,     // "play", or a video chosen to play
  kCopy,          // a copy (no voice or agent command copies yet)
  kOpenInNewTab,  // a link opened in a new tab (no command does yet)
};

// Who answers questions for the tab: voice mode on it, else a web agent
// run on it, else no one.
enum class Asker : uint8_t {
  kNone,
  kVoiceMode,
  kWebAgent,
};

// The tab's record of its last action. Times are milliseconds on a
// monotonic clock.
struct TabRecord {
  // The last action sent to the page; -1 for none.
  int64_t action_ms = -1;
  Intent intent = Intent::kNone;
  // The latest user activation a frame of the tab reported while acting
  // (Blink's 5 s start when the renderer carries the action out); -1 for
  // none.
  int64_t activation_ms = -1;
  // The user's own input on the page since the action.
  bool user_input = false;
  // How long the state lasts after the later of the action and its
  // activation (the browser sets it from blink::kActivationLifespan).
  int64_t window_ms = kActingWindowMs;
  // The page's requests were asked about once since the action.
  bool asked = false;
  // Session history entries the page added since the action.
  int history_entries = 0;
};

// An action was sent to the page (it starts the state afresh).
void NoteAction(TabRecord& tab, int64_t now_ms, Intent intent);
// A frame of the tab reported user activation. It counts only while
// acting: then it can only be the action's (the user's own input would have
// ended the state first).
void NoteActivation(TabRecord& tab, int64_t now_ms);
// The user's own input on the page: the state ends.
void NoteUserInput(TabRecord& tab);
bool IsActing(const TabRecord& tab, int64_t now_ms);
// The page added a session history entry (not a replacement, not a move
// back or forward) by a navigation that began at |now_ms| (it may commit
// later: a server that answers late doesn't make the page an action led to
// one the page added on its own). Returns whether the entry it was added
// from counts as skippable for voice mode's "go back" (above).
bool NoteHistoryEntry(TabRecord& tab, int64_t now_ms);

// When each of a tab's navigations in flight began (by navigation id), for
// NoteHistoryEntry(); one not remembered is judged by when it commits. At
// most kMaxNavigationsInFlight are remembered: a page that piles up its
// frames' navigations (iframes it keeps navigating to a server that never
// answers) can't push out its main frame's, the navigation an action most
// often leads to: when full, the other frames' are forgotten first.
inline constexpr size_t kMaxNavigationsInFlight = 64;
class NavigationStarts {
 public:
  NavigationStarts();
  ~NavigationStarts();
  NavigationStarts(const NavigationStarts&) = delete;
  NavigationStarts& operator=(const NavigationStarts&) = delete;

  // Navigation |id| began at |now_ms|, in the tab's main frame or not.
  void Started(int64_t id, int64_t now_ms, bool main_frame);
  // When navigation |id| began, if remembered; it is forgotten.
  std::optional<int64_t> Finished(int64_t id);
  size_t size() const { return starts_.size(); }

 private:
  struct Start {
    int64_t ms = 0;
    bool main_frame = false;
  };
  std::map<int64_t, Start> starts_;
};

// One question per action: once the user was asked about one of the page's
// requests, the page's next requests until the next action are refused
// instead of asked. Otherwise a page could withdraw its question (by
// navigating) and put another in its place just as the user answers the
// first ("yes" answers whichever is asked).
bool MayAsk(const TabRecord& tab);
void NoteAsked(TabRecord& tab);

// What the page asks for.
enum class Request : uint8_t {
  kNewWindow,         // a popup, a new tab or window (window.open, target,
                      // a modified click), a document picture-in-picture
  kDownload,          // a download the page started
  kExternalApp,       // a link to another app (mailto:, an app's scheme)
  kPermissionPrompt,  // a permission prompt (camera, location, ...)
  kFullScreen,        // the page's own full screen
  kClipboardWrite,    // a write to the clipboard
  kPictureInPicture,  // a video's picture-in-picture window (always on top)
};

enum class Decision : uint8_t {
  kProceed,  // decided as for a person's click
  kAsk,      // refused or held now; the asker asks, and "yes" lets it go on
  kRefuse,   // refused
};

struct Ruling {
  Decision decision = Decision::kProceed;
  // Voice mode says what it refused.
  bool tell = false;

  friend bool operator==(const Ruling&, const Ruling&) = default;
};

// |prompts_by_voice|: voice mode, the asker, reads permission prompts and
// takes a spoken answer (the voice gesture): a prompt raised while acting
// is asked about (kAsk) instead of refused.
Ruling Decide(const TabRecord& tab,
              int64_t now_ms,
              Request request,
              Asker asker,
              bool prompts_by_voice = false);

std::string_view RequestName(Request request);
std::string_view DecisionName(Decision decision);

// ---------------------------------------------------------------------------
// The words
// ---------------------------------------------------------------------------

// A host as the browser says and shows it: lower-case ASCII letters,
// digits, dots and dashes only (anything else, and a leading "www.", is
// dropped; IDNs stay in their xn-- form), at most kMaxSiteChars. A longer
// host is said from its end, whole labels where it can, so the registrable
// domain that says whose site it is is always said.
inline constexpr size_t kMaxSiteChars = 80;
std::string SiteForSpeech(std::string_view host);

// A URL scheme the same way (letters, digits, '+', '-', '.'), at most
// kMaxSchemeChars.
inline constexpr size_t kMaxSchemeChars = 24;
std::string SchemeForSpeech(std::string_view scheme);

// A question about kNewWindow, kDownload or kExternalApp: what the page
// tried ("The page tried to open a new window to example.com.") and what
// "yes" does ("open it"). |site| and |scheme| are cleaned here.
struct Question {
  Request request = Request::kNewWindow;
  std::string said;
  std::string verb;
};
Question QuestionFor(Request request,
                     std::string_view site,
                     std::string_view scheme);

// What voice mode says when it refused |request| (a permission prompt
// names |what|: "camera", "location", as in "camera permission"; cleaned
// here).
std::string RefusalNotice(Request request, std::string_view what);

}  // namespace activation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_SECURITY_ACTIVATION_POLICY_H_
