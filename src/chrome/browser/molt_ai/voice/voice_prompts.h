// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser's prompts and a page's dialogs in voice mode
// (docs/design/TOUCHLESS_BROWSING.md §7.3, Phase 3): what the browser says
// for each, and which answers it takes. Pure and std-only; the browser's
// side (voice_browser_prompts.h) finds the prompts and carries out the
// answers.
//
//   - A permission prompt: "example.com wants to use your camera. To allow
//     it, say harbor. To block it, say no." Its word allows it (the voice
//     gesture, voice_gesture.h); "no", "block" or "deny" blocks it;
//     "cancel" or "not now" dismisses it.
//   - alert(): "example.com says: <text>. Say OK to close it." "OK",
//     "yes" or "close" closes it.
//   - confirm(): "example.com asks: <text>. Say yes or no." "yes" or "OK";
//     "no" or "cancel".
//   - prompt(): "example.com asks: <text>. Say answer, then your answer,
//     or say no to cancel." "answer ..." dictates the answer, read back,
//     then "yes" sends it; "no" or "cancel" cancels.
//   - beforeunload: "Leave example.com? Changes you made may not be saved.
//     Say leave or stay." "leave" (or "reload"), "yes"; "stay", "no",
//     "cancel".
//   - A dangerous download: "The file setup.exe from example.com may be
//     harmful. To keep it, say harbor. To delete it, say discard." Its word
//     keeps it; "discard", "delete" or "no" deletes it. A file known to be
//     malicious can only be discarded by voice.
//   - Saving a password: "Save your password for example.com? Say save,
//     never, or not now." "save" or "yes"; "never"; "not now" or "no".
//   - A workflow draft's sites: "Confirm the sites this routine uses:
//     a.example and b.example? To confirm them, say harbor. To leave them
//     unconfirmed, say no." Its word confirms them, as a click in the
//     Agent studio would (automation_security.h); "no" leaves them.
//
//   - Page text (a dialog's message, a file's name) is cleaned and capped
//     (SpeechSafeText()), and always said after the site it comes from;
//     the site is cleaned to letters, digits, dots and dashes
//     (activation::SiteForSpeech()).
//   - A page that shows dialog after dialog: within kDialogFloodWindowMs,
//     only the first kMaxDialogsRead have their text read
//     (DialogReadLimiter); the rest only say that the site shows another.
//   - The user's password and user name are never said.
//   - Allowing a permission or keeping a dangerous file needs the prompt's
//     word; without one (the gesture is off for now, or the browser only
//     reads typed text) the prompt says to use the mouse or keyboard, and
//     "no" still refuses. While the screen is shared the prompt has no word
//     and says so ("I can't take the word while your screen is shared").
//   - The word reaches the user as voice_gesture_channel.h decides: what is
//     said with it ("To allow it, say harbor.") always has a twin without
//     it ("To allow it, say the word on your screen."), kept with the
//     waiting prompt (PendingPrompt::word_texts): the browser says the
//     twin wherever the word can't be said, and shows only the twin as
//     text.
//
// While a prompt waits (PendingPrompt, in the conversation's state), an
// utterance that is one of its answers answers it (ParsePromptReply());
// anything else is a command as usual and the prompt keeps waiting until
// its window ends.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PROMPTS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PROMPTS_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace voice {

// How long a prompt waits for its answer (the voice gesture's window).
inline constexpr int64_t kPromptWindowMs = 30000;
// A dialog's text, read at most this many characters.
inline constexpr size_t kMaxDialogSpokenChars = 300;
// A file's name, said at most this many characters.
inline constexpr size_t kMaxFileNameSpokenChars = 80;
// prompt()'s answer, at most (bytes).
inline constexpr size_t kMaxPromptAnswerBytes = 500;
// Dialogs whose text is read within the window, per tab.
inline constexpr size_t kMaxDialogsRead = 3;
inline constexpr int64_t kDialogFloodWindowMs = 20000;

enum class PromptKind : uint8_t {
  kPermission,
  kAlert,
  kConfirm,
  kTextPrompt,  // prompt()
  kBeforeUnload,
  kDangerousDownload,
  kSavePassword,
  // A workflow draft's sites (automation_security.h's ConfirmDraftSites()):
  // |text| holds them, "a.example and b.example", each cleaned as a site.
  kConfirmSites,
};

std::string_view PromptKindName(PromptKind kind);

// Whether answering "allow" to |kind| needs the voice gesture.
bool NeedsGesture(PromptKind kind);

// What the browser knows about a prompt it reads (its own words and the
// page's).
struct BrowserPrompt {
  BrowserPrompt();
  ~BrowserPrompt();
  BrowserPrompt(const BrowserPrompt&);
  BrowserPrompt& operator=(const BrowserPrompt&);
  BrowserPrompt(BrowserPrompt&&);
  BrowserPrompt& operator=(BrowserPrompt&&);

  PromptKind kind = PromptKind::kAlert;
  // The host the prompt is for (the requesting origin's, the dialog
  // frame's, the download's); cleaned here.
  std::string site;
  // kPermission: what is asked, in the browser's words ("use your camera",
  // "show notifications"); letters and spaces kept.
  std::string what;
  // A dialog's message, or a download's file name: page text.
  std::string text;
  // kTextPrompt: the field's default (page text; never said, sent on a
  // "yes" without an answer).
  std::string default_text;
  // kBeforeUnload: the page is being reloaded, not left.
  bool reload = false;
  // kDangerousDownload: known to be malicious: it can only be discarded by
  // voice.
  bool malicious = false;
  // The dialog's text isn't read (DialogReadLimiter said no).
  bool flood = false;
  // Allowing needs the word, and there is none because the screen or a
  // window is captured (voice_gesture_channel.h).
  bool screen_shared = false;
  // The word is shown, not said, for the first time in this window: the
  // prompt says why.
  bool explain_shown_only = false;
  // Said before the prompt (why it is read again with a new word).
  std::string preface;
};

// What the browser says for |prompt|. |word| is the voice gesture's word
// when allowing needs one and the gesture is on; empty otherwise. The same
// with kWordOnScreen for |word| is its twin without the word.
std::string PromptSpeech(const BrowserPrompt& prompt, std::string_view word);

// How long a prompt of |kind| waits: kPromptWindowMs for the browser's
// own prompts; a page's dialog waits until it closes (kDialogWindowMs),
// since the page stays blocked behind it.
inline constexpr int64_t kDialogWindowMs = 10 * 60 * 1000;
int64_t PromptWindowMs(PromptKind kind);

// What is said when the prompt's window ended without an answer (empty:
// nothing).
std::string PromptTimeoutSpeech(PromptKind kind);

// What is said when the word was heard but not cleanly (over the
// browser's own voice, or while another source spoke).
std::string NotCleanSpeech(std::string_view word);

// Said before a prompt read again with a new word, by why the word it had
// was retired.
enum class WordRetired : uint8_t {
  kNotClean,       // heard, not cleanly, while something captured
  kAudioCaptured,  // a microphone capture began after it was said
  kScreenShared,   // the screen or a window is captured now
  kScreenFreed,    // no longer: the prompt gets a word again
};
std::string WordRetiredPreface(WordRetired why);

// What is said when a word the prompt had before was heard: |word| is its
// word now (or kWordOnScreen).
std::string RetiredWordSpeech(PromptKind kind, std::string_view word);

// What the browser says with the voice gesture's word, and its twin
// without it (kWordOnScreen in its place).
struct WordText {
  std::string said;
  std::string without;

  friend bool operator==(const WordText&, const WordText&) = default;
};

// The prompt that waits, in the conversation's state.
struct PendingPrompt {
  PendingPrompt();
  ~PendingPrompt();
  PendingPrompt(const PendingPrompt&);
  PendingPrompt& operator=(const PendingPrompt&);
  PendingPrompt(PendingPrompt&&);
  PendingPrompt& operator=(PendingPrompt&&);

  // The browser's id for it; 0: none waits.
  int id = 0;
  PromptKind kind = PromptKind::kAlert;
  // The voice gesture's word, when allowing needs one and the gesture is
  // on.
  std::string word;
  // A file known to be malicious (kDangerousDownload): no word, discard
  // only.
  bool malicious = false;
  // No word because the screen or a window is captured.
  bool screen_shared = false;
  // What was said with |word| and each one's twin without it
  // (kMaxWordTexts at most, the latest kept): VoiceController::Say() says
  // the twin where the word can't be said, and observers only ever get
  // the twin.
  std::vector<WordText> word_texts;
  // kTextPrompt: what the user dictated, waiting for "yes".
  std::string answer_text;
  int64_t expires_ms = 0;
};

// Whether |prompt| waits at |now_ms|.
bool PromptWaits(const PendingPrompt& prompt, int64_t now_ms);

// What is said with the word, at most, per prompt.
inline constexpr size_t kMaxWordTexts = 4;

// Keeps |text| (said with |prompt|'s word) as one of its word texts.
void NoteWordText(PendingPrompt& prompt, WordText text);

// The twin without the word of |said|, when |said| is one of |prompt|'s
// word texts and its word is still the prompt's; null otherwise.
const std::string* WordTextTwin(const PendingPrompt& prompt,
                                std::string_view said);

enum class PromptAnswer : uint8_t {
  kNone,       // not an answer: a command as usual
  kAllow,      // the gesture's word: allow, keep
  kWrongWord,  // another confirmation word: refused (one try)
  kRefuse,     // permission: block; download: discard; password: never
  kAccept,     // alert's OK, confirm's yes, prompt()'s send, leave, save
  kDecline,    // confirm's no, prompt()'s cancel, stay, password: not now
  kDictate,    // prompt(): |text| becomes the answer
  kNeedWord,   // "yes" or "allow" where only the word allows: say so
  kSayAgain,   // like a confirmation word, not for sure which: asked again
  kCantAllow,  // "yes", "keep" where voice can't allow it: say so
};

std::string_view PromptAnswerName(PromptAnswer answer);

struct PromptReply {
  PromptAnswer answer = PromptAnswer::kNone;
  // kDictate: the answer, as said (cleaned, at most kMaxPromptAnswerBytes).
  std::string text;
};

// |heard| (the wake phrase removed) while |prompt| waits.
PromptReply ParsePromptReply(const PendingPrompt& prompt,
                             std::string_view heard);

// What the browser says after |reply| answered |prompt| (|reply|'s text for
// a dictated answer: "Your answer: ... Say yes to send it, or no to
// cancel.").
std::string ReplySpeech(const PendingPrompt& prompt, const PromptReply& reply);

// The same, with kWordOnScreen in place of the word: the twin of what
// ReplySpeech() says with it (kNeedWord).
std::string ReplySpeechWithoutWord(const PendingPrompt& prompt,
                                   const PromptReply& reply);

// How many dialogs of a tab have had their text read lately.
class DialogReadLimiter {
 public:
  DialogReadLimiter();
  ~DialogReadLimiter();
  DialogReadLimiter(const DialogReadLimiter&) = delete;
  DialogReadLimiter& operator=(const DialogReadLimiter&) = delete;

  // A dialog shows at |now_ms|: whether its text is read. Kept per tab
  // across its pages (a page that reloads itself doesn't start again).
  bool ShouldRead(int64_t now_ms);

 private:
  std::deque<int64_t> read_ms_;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PROMPTS_H_
