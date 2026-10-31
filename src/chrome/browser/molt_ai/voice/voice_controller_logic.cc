// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <utility>

#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/spoken_answers.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_command_phrases.h"
#include "chrome/browser/molt_ai/voice/voice_sites.h"
#include "chrome/browser/molt_ai/voice/voice_speech_text.h"
#include "chrome/services/molt_ai/inference/engine/raw_ptr_exclusion.h"

namespace molt_ai {
namespace voice {

namespace {

// "Repeat" says at most this much of what was said last, in bytes.
constexpr size_t kMaxRememberedSpeechBytes = 16384;
// How many examples "what can I say?" reads.
constexpr size_t kMaxHelpExamples = 9;
// A link's address is scanned this far for a sign-out path.
constexpr size_t kMaxUrlScan = 2048;
// One spoken text and the caption's status, in code points.
constexpr size_t kMaxSpokenTextChars = 2 * kMaxSpeechChunkChars;
constexpr size_t kMaxStatusChars = 300;

using K = VoiceCommandKind;

// Words that make pressing something irreversible enough to ask first.
constexpr std::string_view kConfirmWords[] = {
    "buy",     "purchase", "pay",       "order",       "checkout",
    "delete",  "remove",   "erase",     "discard",     "trash",
    "send",    "post",     "publish",   "reply",       "comment",
    "tweet",   "submit",   "confirm",   "subscribe",   "unsubscribe",
    "donate",  "transfer", "logout",    "signout",
};
// ... and pairs of words.
constexpr std::string_view kConfirmPairs[][2] = {
    {"sign", "out"},
    {"log", "out"},
    {"check", "out"},
    {"sign", "off"},
};
// Addresses that sign the user out.
constexpr std::string_view kSignOutPaths[] = {
    "logout", "signout", "log-out", "sign-out", "log_out", "sign_out",
};

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

std::vector<std::string> SplitWords(const std::string& folded) {
  std::vector<std::string> words;
  size_t start = 0;
  while (start < folded.size()) {
    size_t space = folded.find(' ', start);
    if (space == std::string::npos) {
      space = folded.size();
    }
    if (space > start) {
      words.push_back(folded.substr(start, space - start));
    }
    start = space + 1;
  }
  return words;
}

bool HasWord(const std::vector<std::string>& words, std::string_view word) {
  return std::find(words.begin(), words.end(), word) != words.end();
}

bool ListValid(const ConversationState& state, const VoiceEnvironment& env) {
  return state.list != ListContext::kNone &&
         (state.list == ListContext::kTabs ||
          state.list_page_id == env.page_id);
}

bool ConfirmationValid(const ConversationState& state,
                       const VoiceEnvironment& env) {
  return state.confirming && state.confirm_page_id == env.page_id &&
         env.now_ms < state.confirm_expires_ms;
}

// |command|'s answer to the browser's prompt |state| holds (kNone when it
// isn't one). A question the loop asked since the prompt was read (the
// prompt dropped any before it) is newer: a plain yes or no is its.
PromptReply PromptReplyFor(const VoiceCommand& command,
                           const VoiceEnvironment& env,
                           const ConversationState& state) {
  if (ConfirmationValid(state, env)) {
    ParseContext answering;
    answering.language = env.language;
    answering.expecting_yes_no = true;
    const VoiceCommandKind kind =
        ParseVoiceCommand(command.heard, answering).kind;
    if (kind == VoiceCommandKind::kYes || kind == VoiceCommandKind::kNo) {
      return PromptReply();
    }
  }
  return ParsePromptReply(state.prompt, command.heard);
}

const PageItem* FieldInFocus(const ConversationState& state,
                             const PageMap& map,
                             const VoiceEnvironment& env) {
  if (state.field.valid() && state.field_page_id == env.page_id) {
    const PageItem* item = map.ItemForHandle(state.field);
    if (item && item->kind == ItemKind::kTextField) {
      return item;
    }
  }
  if (map.focused_item > 0) {
    const PageItem* item = map.ItemByNumber(map.focused_item);
    if (item && item->kind == ItemKind::kTextField) {
      return item;
    }
  }
  return nullptr;
}

// The page's own list of results: the most prominent list that isn't
// navigation, if it has entries.
const PageList* MainList(const PageMap& map) {
  for (const PageList& list : map.lists) {
    if (!list.in_navigation && !list.entries.empty()) {
      return &list;
    }
  }
  return nullptr;
}

const PageList* FindList(const PageMap& map,
                         const PageNodeHandle& container,
                         const std::string& label) {
  for (const PageList& list : map.lists) {
    if (container.valid() && list.container == container) {
      return &list;
    }
  }
  for (const PageList& list : map.lists) {
    if (!label.empty() && list.label == label) {
      return &list;
    }
  }
  return MainList(map);
}

// The item whose label (folded) has one of |phrases| as words: the
// top-level document's first, then a frame's (an embedded player's), so a
// frame the page embeds (an ad) can't answer for the page.
const PageItem* ItemSaying(const PageMap& map,
                           ItemKind kind,
                           std::initializer_list<std::string_view> phrases) {
  for (const bool in_page : {true, false}) {
    for (const PageItem& item : map.items) {
      if (item.kind != kind || map.InPage(item.handle) != in_page) {
        continue;
      }
      const std::string folded = " " + FoldForComparison(item.label) + " ";
      for (std::string_view phrase : phrases) {
        if (folded.find(" " + std::string(phrase) + " ") != std::string::npos) {
          return &item;
        }
      }
    }
  }
  return nullptr;
}

// What a Spanish page's controls say, for the buttons and links the loop
// looks for by their words (English's are at the calls): read only in
// Spanish, so English finds what it always did.
enum class SpanishControl {
  kNextPage,
  kVolume,
  kFullScreen,
  kSkipAd,
  kCaptions
};

const PageItem* SpanishItemSaying(const PageMap& map,
                                  ItemKind kind,
                                  SpanishControl control) {
  switch (control) {
    case SpanishControl::kNextPage:
      return ItemSaying(map, kind,
                        {"siguiente", "pagina siguiente", "mas resultados",
                         "mostrar mas", "ver mas", "cargar mas"});
    case SpanishControl::kVolume:
      return ItemSaying(map, kind, {"volumen"});
    case SpanishControl::kFullScreen:
      return ItemSaying(map, kind, {"pantalla completa"});
    case SpanishControl::kSkipAd:
      return ItemSaying(
          map, kind,
          {"saltar anuncio", "omitir anuncio", "saltar anuncios",
           "omitir anuncios", "saltar publicidad", "omitir publicidad"});
    case SpanishControl::kCaptions:
      return ItemSaying(map, kind, {"subtitulos", "subtitulado"});
  }
  return nullptr;
}

std::string Quoted(std::string_view text) {
  return "\"" + std::string(text) + "\"";
}

VoiceStep Step(VoiceStepKind kind) {
  VoiceStep step;
  step.kind = kind;
  return step;
}

VoiceStep NodeStep(VoiceStepKind kind,
                   const PageNodeHandle& target,
                   std::string name) {
  VoiceStep step = Step(kind);
  step.target = target;
  step.text = std::move(name);
  return step;
}

VoiceStep WaitStep(FollowUpKind follow_up,
                   std::string subject,
                   std::string preface = std::string()) {
  VoiceStep step = Step(VoiceStepKind::kWaitForPage);
  step.follow_up = follow_up;
  step.subject = std::move(subject);
  step.preface = std::move(preface);
  return step;
}

VoiceStep CommandStep(BrowserCommand command) {
  VoiceStep step = Step(VoiceStepKind::kBrowserCommand);
  step.command = command;
  return step;
}

VoiceStep MediaStep(MediaCommand media, int seconds = 0) {
  VoiceStep step = Step(VoiceStepKind::kMedia);
  step.media = media;
  step.seconds = seconds;
  return step;
}

// One turn being planned.
class Planner {
 public:
  Planner(const PageMap& map,
          const VoiceEnvironment& env,
          const ConversationState& state)
      : map_(map), env_(env) {
    turn_.state = state;
    ForgetOtherPages();
  }

  VoiceTurn Finish() {
    if (!spoken_.empty()) {
      std::vector<std::string>& last = turn_.state.last_spoken;
      last.clear();
      size_t bytes = 0;
      for (std::string& text : spoken_) {
        bytes += text.size();
        if (bytes > kMaxRememberedSpeechBytes) {
          break;
        }
        last.push_back(std::move(text));
      }
    }
    return std::move(turn_);
  }

  ConversationState& state() { return turn_.state; }
  VoiceTurn& turn() { return turn_; }

  void Say(std::string text) {
    // In the window's language (a constant sentence; the others come
    // through Tr()), then built from cleaned pieces, cleaned once more as
    // it leaves.
    text = Localized(env_.language, text);
    text = SpeechSafeText(text, kMaxSpokenTextChars);
    if (text.empty()) {
      return;
    }
    VoiceStep step = Step(VoiceStepKind::kSpeak);
    step.text = text;
    turn_.steps.push_back(std::move(step));
    spoken_.push_back(std::move(text));
  }

  void Say(std::string_view text) { Say(std::string(text)); }
  void Say(const char* text) { Say(std::string(text)); }

  void Say(const std::vector<std::string>& chunks) {
    for (const std::string& chunk : chunks) {
      Say(chunk);
    }
  }

  void Add(VoiceStep step) { turn_.steps.push_back(std::move(step)); }

  void Status(std::string status) {
    turn_.status =
        SpeechSafeText(Localized(env_.language, status), kMaxStatusChars);
  }

  // |english| in the window's language with |args| in its {0}, {1}...
  // (spoken_answers.h); exactly the English sentence in English.
  std::string Tr(std::string_view english,
                 std::initializer_list<std::string_view> args = {}) const {
    return Said(env_.language, english, args);
  }

  // ---- Commands ----------------------------------------------------------

  void Plan(const VoiceCommand& command, int depth) {
    turn_.kind = command.kind;
    turn_.heard = SpeechSafeText(command.heard, kMaxStatusChars);
    if (env_.moltnet) {
      Status("Voice mode is off in MoltNet windows");
      Say("Voice mode is off in MoltNet windows.");
      return;
    }
    // The browser's prompt hears its answers first.
    if (depth == 0 && AnswerBrowserPrompt(command)) {
      return;
    }
    const bool confirming = ConfirmationValid(state(), env_);
    if (command.kind != K::kYes && command.kind != K::kNo) {
      ClearConfirmation();
    }
    if (command.kind != K::kChoose && command.kind != K::kRepeat &&
        command.kind != K::kStopSpeaking && command.kind != K::kHelp &&
        command.kind != K::kSpeakFaster && command.kind != K::kSpeakSlower &&
        command.kind != K::kVerbosity) {
      state().offered.clear();
      DropOfferContext();
    }

    switch (command.kind) {
      case K::kUnknown:
        Unknown(command);
        break;
      case K::kRefused:
        Say(command.refusal == RefusalReason::kTooLong
                ? "That was too long for me. Say it in fewer words."
                : "I couldn't read that. Say it again.");
        break;

      // Navigation.
      case K::kOpenSite:
        OpenSite(command);
        break;
      case K::kSearch:
        Search(command);
        break;
      case K::kFindInPage:
        FindInPage(command.text);
        break;
      case K::kBack:
        if (!env_.can_go_back) {
          Say("There's no page to go back to.");
          break;
        }
        Status("Going back");
        Add(CommandStep(BrowserCommand::kBack));
        Add(WaitStep(FollowUpKind::kAnnouncePage, std::string()));
        break;
      case K::kForward:
        if (!env_.can_go_forward) {
          Say("There's no page to go forward to.");
          break;
        }
        Status("Going forward");
        Add(CommandStep(BrowserCommand::kForward));
        Add(WaitStep(FollowUpKind::kAnnouncePage, std::string()));
        break;
      case K::kReload:
        Status("Reloading");
        Add(CommandStep(BrowserCommand::kReload));
        Add(WaitStep(FollowUpKind::kAnnouncePage, std::string()));
        break;
      case K::kStopLoading:
        Add(CommandStep(BrowserCommand::kStopLoading));
        Say("Stopped loading.");
        break;
      case K::kHome:
        Status("Going home");
        Add(CommandStep(BrowserCommand::kHome));
        Add(WaitStep(FollowUpKind::kAnnouncePage, std::string()));
        break;
      case K::kNextPage:
        NextPage();
        break;
      case K::kNewTab:
        Add(Step(VoiceStepKind::kNewTab));
        Say("New tab.");
        break;
      case K::kCloseTab:
        CloseTab(command);
        break;
      case K::kSwitchTab:
        SwitchTab(command);
        break;

      // Viewing.
      case K::kScrollDown:
        Scroll(PageScroll::kDown, command, "Down.");
        break;
      case K::kScrollUp:
        Scroll(PageScroll::kUp, command, "Up.");
        break;
      case K::kScrollLeft:
        Scroll(PageScroll::kLeft, command, "Left.");
        break;
      case K::kScrollRight:
        Scroll(PageScroll::kRight, command, "Right.");
        break;
      case K::kScrollToTop:
        Scroll(PageScroll::kTop, command, "Top of the page.");
        break;
      case K::kScrollToBottom:
        Scroll(PageScroll::kBottom, command, "Bottom of the page.");
        break;
      case K::kZoomIn:
        Add(CommandStep(BrowserCommand::kZoomIn));
        Say("Zoomed in.");
        break;
      case K::kZoomOut:
        Add(CommandStep(BrowserCommand::kZoomOut));
        Say("Zoomed out.");
        break;
      case K::kZoomReset:
        Add(CommandStep(BrowserCommand::kZoomReset));
        Say("Normal size.");
        break;

      // Media.
      case K::kPlay:
      case K::kResume:
        if (command.kind == K::kResume && ResumesReading()) {
          Read(ReadOp::kResume);
          break;
        }
        Play();
        break;
      case K::kPause:
        if (env_.reading == VoiceEnvironment::Reading::kReading) {
          // The new command stopped the reading; its place is kept.
          Status("Paused reading");
          Say("Paused. Say continue reading to go on.");
          break;
        }
        if (!env_.media_playing) {
          Say("Nothing is playing.");
          break;
        }
        Add(MediaStep(MediaCommand::kPause));
        Say("Paused.");
        break;
      case K::kVolumeUp:
      case K::kVolumeDown:
        Volume(command.kind == K::kVolumeUp);
        break;
      case K::kSetVolume:
        Say("Setting the volume to a level comes later. Say louder or "
            "quieter.");
        break;
      case K::kMute:
      case K::kUnmute: {
        const bool mute = command.kind == K::kMute;
        if (env_.muted == mute) {
          Say(mute ? "It's already muted." : "It isn't muted.");
          break;
        }
        VoiceStep step = Step(VoiceStepKind::kMute);
        step.flag = mute;
        Add(std::move(step));
        Say(mute ? "Muted." : "Sound on.");
        break;
      }
      case K::kSkipForward:
      case K::kSkipBack: {
        if (!env_.media_session) {
          Say("Nothing is playing.");
          break;
        }
        const int seconds =
            command.seconds > 0 ? command.seconds : kDefaultSkipSeconds;
        const bool forward = command.kind == K::kSkipForward;
        Add(MediaStep(MediaCommand::kSeek, forward ? seconds : -seconds));
        Say(Tr(forward ? "Skipped ahead {0}." : "Went back {0}.",
               {SpokenLength(seconds, env_.language)}));
        break;
      }
      case K::kRestart:
        if (!env_.media_session) {
          Say("Nothing is playing.");
          break;
        }
        Add(MediaStep(MediaCommand::kSeekToStart));
        Say("From the start.");
        break;
      case K::kNextTrack:
      case K::kPreviousTrack:
        if (!env_.media_session) {
          Say("Nothing is playing.");
          break;
        }
        Add(MediaStep(command.kind == K::kNextTrack
                          ? MediaCommand::kNextTrack
                          : MediaCommand::kPreviousTrack));
        Say(command.kind == K::kNextTrack ? "Next." : "Previous.");
        break;
      case K::kFullScreen:
        FullScreen();
        break;
      case K::kExitFullScreen:
        if (!env_.fullscreen) {
          Say("It isn't full screen.");
          break;
        }
        Add(CommandStep(BrowserCommand::kToggleFullScreen));
        Say("Left full screen.");
        break;
      case K::kCaptionsOn:
      case K::kCaptionsOff:
        Captions(command.kind == K::kCaptionsOn);
        break;
      case K::kSpeedUp:
      case K::kSlowDown:
        // While the browser reads, "faster" is its reading speed.
        if (env_.reading != VoiceEnvironment::Reading::kNone) {
          SpeechRate(command.kind == K::kSpeedUp);
          break;
        }
        Say("Changing the playback speed comes later.");
        break;
      case K::kNormalSpeed:
        Say("Changing the playback speed comes later.");
        break;
      case K::kSkipAd:
        SkipAd();
        break;

      // Reading.
      case K::kReadPage:
        Read(ReadOp::kPage);
        break;
      case K::kReadFromHere:
        Read(ReadOp::kFromHere);
        break;
      case K::kReadHeadings:
        ReadHeadings();
        break;
      case K::kReadLinks:
        ReadLinks();
        break;
      case K::kReadResults:
        if (!ReadResults(std::string())) {
          Say("There's no list of results on this page.");
        }
        break;
      case K::kReadTitle:
        Say(map_.title.empty()
                ? std::string("This page has no title.")
                : Tr("The title is: {0}.", {PageText(map_.title)}));
        break;
      case K::kReadSelection:
        Read(ReadOp::kSelection);
        break;
      case K::kReadSection:
        Read(ReadOp::kSection, PageText(command.text));
        break;
      case K::kAskPage:
        AskPage(command);
        break;
      case K::kDescribeScreen:
        Describe();
        break;
      case K::kNextHeading:
        Read(ReadOp::kNextHeading);
        break;
      case K::kPreviousHeading:
        Read(ReadOp::kPreviousHeading);
        break;
      case K::kNextParagraph:
        Read(ReadOp::kNextParagraph);
        break;
      case K::kPreviousParagraph:
        Read(ReadOp::kPreviousParagraph);
        break;
      case K::kNextLink:
      case K::kPreviousLink:
        MoveLink(command.kind == K::kNextLink);
        break;
      case K::kNextItem:
      case K::kPreviousItem:
        MoveItem(command.kind == K::kNextItem);
        break;

      // Choosing.
      case K::kChoose:
        Choose(command.choice, command.verb);
        break;
      case K::kShowNumbers:
      case K::kHideNumbers: {
        const bool show = command.kind == K::kShowNumbers;
        state().numbers_shown = show;
        VoiceStep step = Step(VoiceStepKind::kShowNumbers);
        step.flag = show;
        Add(std::move(step));
        Say(show ? "Numbers on." : "Numbers off.");
        break;
      }

      // Dictation.
      case K::kType:
      case K::kNewLine:
      case K::kDeleteLastWord:
      case K::kClearField:
      case K::kReadBack:
      case K::kSubmit:
        Dictation(command);
        break;

      // Conversation.
      case K::kYes:
        if (!confirming) {
          ClearConfirmation();
          Say("There's nothing to confirm.");
          break;
        }
        Status(Tr("Going ahead: {0}", {state().confirm_what}));
        for (VoiceStep& step : state().confirm_steps) {
          if (step.kind == VoiceStepKind::kSpeak) {
            Say(step.text);
          } else {
            Add(std::move(step));
          }
        }
        ClearConfirmation();
        break;
      case K::kNo:
        // The planner's question (PlanPlannerQuestion()) and the
        // activation guard's (PlanPageRequestQuestion()) hear their "no".
        if (confirming) {
          for (const VoiceStep& held : state().confirm_steps) {
            if (held.kind == VoiceStepKind::kPlannerAnswer ||
                held.kind == VoiceStepKind::kPageRequestAnswer) {
              VoiceStep no = held;
              no.flag = false;
              Add(std::move(no));
              break;
            }
          }
        }
        ClearConfirmation();
        Say(confirming ? "Okay, I won't." : "Okay.");
        break;
      case K::kCancel:
        Status("Cancelled");
        Say("Cancelled.");
        break;
      case K::kStop:
      case K::kStopSpeaking:
        // The glue already stopped speech and whatever was running.
        Status("Stopped");
        break;
      case K::kUndo:
        Say("Undo comes later. Say go back to return to the page before.");
        break;
      case K::kRepeat:
        if (state().last_spoken.empty()) {
          Say("I haven't said anything yet.");
        } else {
          const std::vector<std::string> last = state().last_spoken;
          Say(last);
        }
        break;
      case K::kHelp:
        Help();
        break;
      case K::kWhereAmI:
        WhereAmI();
        break;
      case K::kSpeakFaster:
      case K::kSpeakSlower:
        SpeechRate(command.kind == K::kSpeakFaster);
        break;
      case K::kVerbosity:
        SetVerbosity(command.verbosity);
        break;
      case K::kCorrection: {
        if (depth > 0) {
          Say("I couldn't read that. Say it again.");
          break;
        }
        // What the user meant, parsed again in the same context.
        const VoiceCommand corrected = ParseVoiceCommand(
            command.text, ContextFor(state(), map_, env_));
        Plan(corrected, depth + 1);
        break;
      }

      // Learning (§6.8).
      case K::kRememberRoutine:
        RememberRoutine(command.text);
        break;
      case K::kRunRoutine:
        RunRoutine(command.text);
        break;
      case K::kForgetRoutine:
        ForgetRoutine(command.text);
        break;
      case K::kListRoutines:
        ListRoutines();
        break;
      case K::kForgetWord:
        ForgetWord(command.text);
        break;
      case K::kForgetLearning:
        ForgetLearning();
        break;

      // Voice mode itself.
      case K::kVoiceModeOn: {
        if (env_.listening) {
          Say("I'm already listening.");
          break;
        }
        Status("Turning on voice mode");
        VoiceStep on = Step(VoiceStepKind::kSetVoiceMode);
        on.flag = true;
        Add(std::move(on));
        break;
      }
      case K::kVoiceModeOff: {
        if (!env_.listening) {
          Say("I'm not listening now.");
          break;
        }
        // A user who has only their voice can't turn it back on by voice:
        // ask first.
        VoiceStep said = Step(VoiceStepKind::kSpeak);
        said.text = "Voice mode is off.";
        VoiceStep off = Step(VoiceStepKind::kSetVoiceMode);
        off.flag = false;
        std::vector<VoiceStep> steps;
        steps.push_back(std::move(said));
        steps.push_back(std::move(off));
        Say("After this I won't hear you until voice mode is turned on "
            "again with the Voice button in the toolbar, or in settings.");
        AskToConfirm(std::move(steps), "turn off voice mode");
        break;
      }
    }
  }

  // ---- The planner's questions ------------------------------------------

  void PlannerQuestion(int question, std::string_view what) {
    turn_.kind = K::kUnknown;
    if (env_.moltnet || question <= 0) {
      return;
    }
    // The planner's words: model output and page text among them.
    const std::string text = PageText(what);
    VoiceStep yes = Step(VoiceStepKind::kPlannerAnswer);
    yes.index = question;
    yes.flag = true;
    std::vector<VoiceStep> steps;
    steps.push_back(std::move(yes));
    AskToConfirm(std::move(steps), text.empty() ? std::string("go on") : text);
  }

  void PageRequestQuestion(int question,
                           std::string_view said,
                           std::string_view verb) {
    turn_.kind = K::kUnknown;
    if (env_.moltnet || question <= 0) {
      return;
    }
    // The browser's words, with the page's site in them: cleaned again.
    Say(PageText(said));
    const std::string what = PageText(verb);
    VoiceStep yes = Step(VoiceStepKind::kPageRequestAnswer);
    yes.index = question;
    yes.flag = true;
    std::vector<VoiceStep> steps;
    steps.push_back(std::move(yes));
    AskToConfirm(std::move(steps),
                 what.empty() ? std::string("allow it") : what);
  }

  // ---- The browser's prompts --------------------------------------------

  void ReadPrompt(int id,
                  const voice::BrowserPrompt& prompt,
                  std::string_view word) {
    turn_.kind = K::kUnknown;
    if (env_.moltnet || id <= 0) {
      return;
    }
    // It takes the floor: what the loop asked is dropped.
    ClearConfirmation();
    const bool gesture = NeedsGesture(prompt.kind) && !prompt.malicious;
    PendingPrompt& pending = state().prompt;
    pending = PendingPrompt();
    pending.id = id;
    pending.kind = prompt.kind;
    pending.word = gesture ? std::string(word) : std::string();
    pending.malicious = prompt.malicious;
    pending.expires_ms = env_.now_ms + PromptWindowMs(prompt.kind);
    Status(prompt.kind == PromptKind::kPermission ? "Permission request"
                                                  : "Waiting for your answer");
    Say(PromptSpeech(prompt, pending.word));
  }

  void PromptGone(int id, std::string_view said) {
    turn_.kind = K::kUnknown;
    if (state().prompt.id != 0 && state().prompt.id == id) {
      state().prompt = PendingPrompt();
    }
    if (!said.empty()) {
      Say(said);
    }
  }

  // Whether |command| answered the browser's waiting prompt.
  bool AnswerBrowserPrompt(const VoiceCommand& command) {
    PendingPrompt& prompt = state().prompt;
    if (!PromptWaits(prompt, env_.now_ms)) {
      if (prompt.id != 0) {
        prompt = PendingPrompt();
      }
      return false;
    }
    const PromptReply reply = PromptReplyFor(command, env_, state());
    if (reply.answer == PromptAnswer::kNone) {
      return false;
    }
    VoiceStep step = Step(VoiceStepKind::kPromptAnswer);
    step.index = prompt.id;
    step.prompt_answer = reply.answer;
    switch (reply.answer) {
      case PromptAnswer::kNone:
        return false;
      case PromptAnswer::kDictate:
        prompt.answer_text = reply.text;
        Say(ReplySpeech(prompt, reply));
        return true;
      case PromptAnswer::kNeedWord:
      case PromptAnswer::kSayAgain:
      case PromptAnswer::kCantAllow:
        Say(ReplySpeech(prompt, reply));
        return true;
      case PromptAnswer::kAllow:
      case PromptAnswer::kWrongWord:
        // The browser checks the word, with how it was heard, and says
        // what came of it.
        step.text = command.heard;
        Status("Checking the word");
        Add(std::move(step));
        return true;
      case PromptAnswer::kRefuse:
      case PromptAnswer::kAccept:
      case PromptAnswer::kDecline:
        step.text = prompt.kind == PromptKind::kTextPrompt ? prompt.answer_text
                                                           : std::string();
        Status(ReplySpeech(prompt, reply));
        Add(std::move(step));
        Say(ReplySpeech(prompt, reply));
        prompt = PendingPrompt();
        return true;
    }
    return false;
  }

  // ---- Follow-ups --------------------------------------------------------

  void FollowUp(const VoiceStep& wait) {
    turn_.kind = K::kUnknown;
    if (env_.moltnet) {
      return;
    }
    if (!wait.preface.empty()) {
      Say(wait.preface);
    }
    const std::string title = PageText(map_.title);
    switch (wait.follow_up) {
      case FollowUpKind::kNone:
        break;
      case FollowUpKind::kAnnounceOpened:
        Status(Tr("Opened {0}", {wait.subject}));
        Say(Tr("{0} is open.", {wait.subject}));
        break;
      case FollowUpKind::kAnnouncePage: {
        Say(title.empty() ? std::string("The page is open.") : title + ".");
        if (const PageList* list = MainList(map_)) {
          SetListContext(ListContext::kPageList, list);
          state().expecting_choice = true;
          Say(CountOf(list->entries.size(), Tr("item"), Tr("items")) + ". " +
              std::string(ChooseHint(env_.language)));
        }
        break;
      }
      case FollowUpKind::kReadResults:
        if (!ReadResults(wait.subject)) {
          Say(title.empty()
                  ? Tr("The page is open, but I can't find a list of results "
                       "there.")
                  : Tr("{0} is open, but I can't find a list of results "
                       "there.",
                       {title}));
        }
        break;
      case FollowUpKind::kPlayOpened:
        if (env_.media_playing) {
          Status(Tr("Playing: {0}", {wait.subject}));
          Say(Tr("Playing {0}.", {wait.subject}));
        } else if (const PageMedia* player = Locate(SitePart::kPlayer).media) {
          const PageMedia& media = *player;
          Add(NodeStep(VoiceStepKind::kPlayMedia, media.handle, wait.subject));
          Status(Tr("Playing: {0}", {wait.subject}));
          Say(Tr("Playing {0}.", {wait.subject}));
        } else {
          Say(title.empty() ? std::string("The page is open.")
                            : Tr("{0} is open.", {title}));
        }
        break;
    }
  }

 private:
  // Lists, fields, cursors and confirmations of another page are gone.
  void ForgetOtherPages() {
    ConversationState& s = state();
    if (!ListValid(s, env_)) {
      s.list = ListContext::kNone;
      s.list_container = PageNodeHandle();
      s.list_label.clear();
      s.offered.clear();
      s.current_position = 0;
      s.expecting_choice = false;
    }
    if (s.field_page_id != env_.page_id) {
      s.field = PageNodeHandle();
      s.field_page_id = -1;
    }
    if (s.cursor_page_id != env_.page_id) {
      s.heading_cursor = 0;
      s.link_cursor = 0;
      s.cursor_page_id = env_.page_id;
    }
  }

  // Every item, or the tabs, are a list to choose from only while close
  // ones are offered from them; after that, choosing goes back to the
  // page's lists.
  void DropOfferContext() {
    ConversationState& s = state();
    if ((s.list == ListContext::kItems || s.list == ListContext::kTabs) &&
        s.offered.empty()) {
      s.list = ListContext::kNone;
      s.current_position = 0;
      s.expecting_choice = false;
    }
  }

  void ClearConfirmation() {
    ConversationState& s = state();
    s.confirming = false;
    s.confirm_steps.clear();
    s.confirm_what.clear();
    s.confirm_page_id = -1;
    s.confirm_expires_ms = 0;
  }

  void SetListContext(ListContext context, const PageList* list) {
    ConversationState& s = state();
    if (s.list != context || s.list_page_id != env_.page_id ||
        (list && !(s.list_container == list->container))) {
      s.current_position = 0;
    }
    s.list = context;
    s.list_page_id = env_.page_id;
    s.list_container = list ? list->container : PageNodeHandle();
    s.list_label = list ? list->label : std::string();
    s.offered.clear();
  }

  // The candidates of the state's list, on this page.
  CandidateSet ContextSet() {
    const ConversationState& s = state();
    switch (s.list) {
      case ListContext::kNone:
        return CandidateSet();
      case ListContext::kPageList:
        if (const PageList* list =
                FindList(map_, s.list_container, s.list_label)) {
          return CandidatesFromList(map_, *list, s.numbers_shown);
        }
        return CandidateSet();
      case ListContext::kItems:
        return CandidatesFromItems(map_);
      case ListContext::kLinks:
        return CandidatesFromLinks(map_, s.numbers_shown);
      case ListContext::kHeadings:
        return CandidatesFromHeadings(map_);
      case ListContext::kTabs:
        return CandidatesFromTabs(env_.tabs);
    }
    return CandidateSet();
  }

  // ---- Choosing ----------------------------------------------------------

  struct Attempt {
    CandidateSet set;
    ListContext context = ListContext::kNone;
    // Into |map_|, which outlives the planner.
    MOLT_RAW_PTR_EXCLUSION const PageList* list = nullptr;
    // For a subset: the index in the context's set of each candidate.
    std::vector<size_t> base;
    bool uses_current = false;
  };

  void Choose(const ChoiceRef& ref, ChoiceVerb verb) {
    ConversationState& s = state();
    if (verb == ChoiceVerb::kDefault && !s.offered.empty()) {
      verb = s.offered_verb;
    }
    std::vector<Attempt> attempts;
    if (s.list != ListContext::kNone) {
      CandidateSet base = ContextSet();
      if (!s.offered.empty() && !OfferStillHolds(base)) {
        // What was read out names other things now (the page, or a frame
        // in it, changed since): an answer to it would choose one the
        // user didn't hear.
        s.offered.clear();
        s.offered_handles.clear();
        s.expecting_choice = false;
        Say("The page changed since I asked. Say it again.");
        return;
      }
      if (!s.offered.empty()) {
        Attempt subset;
        subset.set = SubsetOf(base, s.offered);
        subset.context = s.list;
        subset.base = s.offered;
        attempts.push_back(std::move(subset));
      }
      Attempt full;
      full.set = std::move(base);
      full.context = s.list;
      full.uses_current = true;
      if (s.list == ListContext::kPageList) {
        full.list = FindList(map_, s.list_container, s.list_label);
      }
      attempts.push_back(std::move(full));
    } else if (ref.kind != ChoiceRef::Kind::kNumber) {
      if (const PageList* list =
              Locate(SitePart::kResultList, /*record=*/false).list) {
        Attempt main;
        main.set = CandidatesFromList(map_, *list, s.numbers_shown);
        main.context = ListContext::kPageList;
        main.list = list;
        attempts.push_back(std::move(main));
      }
    }
    // Every item, after the list: for a description ("the search box"), for
    // a number shown on screen, or when no list was read. A position or a
    // number the browser read out stays within its list.
    const bool items_too =
        s.list == ListContext::kNone ||
        ref.kind == ChoiceRef::Kind::kDescription ||
        (ref.kind == ChoiceRef::Kind::kNumber && s.numbers_shown);
    if (items_too && s.list != ListContext::kItems &&
        s.list != ListContext::kTabs) {
      Attempt items;
      items.set = CandidatesFromItems(map_);
      items.context = ListContext::kItems;
      attempts.push_back(std::move(items));
    }

    NoMatchReason reason = NoMatchReason::kNoCandidates;
    bool first = true;
    for (Attempt& attempt : attempts) {
      if (attempt.set.empty()) {
        continue;
      }
      MatchOptions options;
      options.current_position = attempt.uses_current ? s.current_position : 0;
      const ChoiceMatch match =
          MatchSpokenChoice(ref, attempt.set.candidates, options);
      if (match.outcome == ChoiceOutcome::kUnique) {
        Chosen(attempt, match.index, verb);
        return;
      }
      if (match.outcome == ChoiceOutcome::kAmbiguous) {
        Offer(attempt, match.close, verb);
        return;
      }
      if (first) {
        reason = match.reason;
        first = false;
      }
      if (match.reason == NoMatchReason::kNoCurrentItem ||
          match.reason == NoMatchReason::kEmptyReference) {
        break;
      }
    }
    NoMatch(ref, reason);
  }

  void Chosen(const Attempt& attempt, size_t index, ChoiceVerb verb) {
    ConversationState& s = state();
    const size_t base_index =
        attempt.base.empty() ? index : attempt.base[index];
    s.offered.clear();
    s.expecting_choice = false;
    if (attempt.context == ListContext::kItems ||
        attempt.context == ListContext::kTabs) {
      // An item or tab chosen by its name or number: the list read last
      // stays the one "the next one" moves through.
      DropOfferContext();
    } else {
      if (s.list != attempt.context ||
          (attempt.list && !(attempt.list->container == s.list_container))) {
        SetListContext(attempt.context, attempt.list);
      }
      // The chosen entry's position in the whole list, for "the next one".
      s.current_position = attempt.base.empty()
                               ? attempt.set.candidates[index].position
                               : static_cast<int>(base_index) + 1;
      if (attempt.context == ListContext::kPageList && attempt.list) {
        UsedList(*attempt.list);
      }
    }
    Act(attempt.set.targets[index], attempt.set.candidates[index], verb);
  }

  // Whether each choice offered is still at its place in |set|.
  bool OfferStillHolds(const CandidateSet& set) {
    const ConversationState& s = state();
    if (s.offered_handles.size() != s.offered.size()) {
      return false;
    }
    for (size_t i = 0; i < s.offered.size(); ++i) {
      if (s.offered[i] >= set.size() ||
          !(set.targets[s.offered[i]].handle == s.offered_handles[i])) {
        return false;
      }
    }
    return true;
  }

  // The nodes |indices| of |set| name, to tell later whether they moved.
  static std::vector<PageNodeHandle> HandlesOf(
      const CandidateSet& set,
      const std::vector<size_t>& indices) {
    std::vector<PageNodeHandle> handles;
    for (size_t i : indices) {
      handles.push_back(i < set.size() ? set.targets[i].handle
                                       : PageNodeHandle());
    }
    return handles;
  }

  void Offer(const Attempt& attempt,
             const std::vector<size_t>& close,
             ChoiceVerb verb) {
    ConversationState& s = state();
    std::vector<size_t> base;
    for (size_t i : close) {
      base.push_back(attempt.base.empty() ? i : attempt.base[i]);
    }
    if (s.list != attempt.context ||
        (attempt.list && !(attempt.list->container == s.list_container))) {
      SetListContext(attempt.context, attempt.list);
    }
    const CandidateSet context_set =
        attempt.base.empty() ? attempt.set : ContextSet();
    CandidateSet subset = SubsetOf(context_set, base);
    s.offered_handles = HandlesOf(context_set, base);
    s.offered = std::move(base);
    s.offered_verb = verb;
    s.expecting_choice = true;
    Status("Which one?");
    Say(OfferSpeech(subset, env_.language));
  }

  void NoMatch(const ChoiceRef& ref, NoMatchReason reason) {
    switch (reason) {
      case NoMatchReason::kNoCandidates:
        Say("There's nothing here to choose from.");
        return;
      case NoMatchReason::kOutOfRange: {
        const int n = ref.kind == ChoiceRef::Kind::kNumber ? ref.number
                                                           : ref.ordinal;
        Say(Tr("There's no number {0} here. {1}",
               {std::to_string(n), ChooseHint(env_.language)}));
        return;
      }
      case NoMatchReason::kNoCurrentItem:
        Say("Choose one first: say a number or a name.");
        return;
      case NoMatchReason::kEmptyReference:
      case NoMatchReason::kNothingMatched:
      case NoMatchReason::kNone:
        Say(Tr("Nothing here matches {0}. {1}",
               {Quoted(turn_.heard), ChooseHint(env_.language)}));
        return;
    }
  }

  // Asks before |steps| run when pressing |name| can't be undone; returns
  // whether it asked.
  bool Confirm(std::vector<VoiceStep> steps,
               const std::string& name,
               std::string_view label,
               std::string_view url) {
    if (!NeedsConfirmation(label, url, map_, env_.language) &&
        !NeedsConfirmation(name, std::string_view(), map_, env_.language)) {
      return false;
    }
    AskToConfirm(std::move(steps), Tr("press {0}", {name}));
    return true;
  }

  void AskToConfirm(std::vector<VoiceStep> steps, const std::string& what) {
    ConversationState& s = state();
    s.confirming = true;
    s.confirm_steps = std::move(steps);
    // "turn off voice mode", "go on": in the window's language too.
    s.confirm_what = Localized(env_.language, what);
    s.confirm_page_id = env_.page_id;
    s.confirm_expires_ms = env_.now_ms + kConfirmationTimeoutMs;
    Status(Tr("Say yes to {0}", {s.confirm_what}));
    Say(Tr("Say yes to {0}, or no to cancel.", {s.confirm_what}));
  }

  void SetCurrentItem(int number) {
    VoiceStep step = Step(VoiceStepKind::kSetCurrentItem);
    step.index = number;
    Add(std::move(step));
  }

  void Act(const CandidateTarget& target,
           const ChoiceCandidate& candidate,
           ChoiceVerb verb) {
    const std::string name = PageText(candidate.title);
    if (target.source == CandidateSource::kTab) {
      SelectTab(target.index);
      return;
    }
    if (verb == ChoiceVerb::kRead || verb == ChoiceVerb::kDescribe) {
      if (target.handle.valid()) {
        Add(NodeStep(VoiceStepKind::kScrollIntoView, target.handle, name));
        SetCurrentItem(target.item_number);
      }
      Say(SpokenCandidate(candidate, Verbosity::kFull) + ".");
      return;
    }
    if (target.source == CandidateSource::kHeading) {
      Add(NodeStep(VoiceStepKind::kScrollIntoView, target.handle, name));
      Say(Tr("Heading: {0}.", {name}));
      return;
    }
    if (!target.handle.valid()) {
      Say(Tr("{0}. There's nothing to open there.", {name}));
      return;
    }
    const PageItem* item = map_.ItemByNumber(target.item_number);
    const std::string label = item ? item->label : name;
    SetCurrentItem(target.item_number);
    switch (target.kind) {
      case ItemKind::kTextField: {
        Add(NodeStep(VoiceStepKind::kFocus, target.handle, name));
        if (item && item->credential != CredentialKind::kNone) {
          Say(
              Tr("{0}, {1} field. Voice mode never types or reads passwords, "
                 "card numbers or codes.",
                 {name, Tr(CredentialKindName(item->credential))}));
          return;
        }
        state().field = target.handle;
        state().field_page_id = env_.page_id;
        std::string said =
            name.empty() ? Tr("Text field.") : Tr("{0} field.", {name});
        if (item && !item->value.empty()) {
          said += Tr(" It says: {0}.", {PageText(item->value)});
        }
        Say(said + Tr(" Say type, then your text."));
        return;
      }
      case ItemKind::kMedia:
        Add(NodeStep(VoiceStepKind::kPlayMedia, target.handle, name));
        Status(Tr("Playing: {0}", {name}));
        Say(Tr("Playing {0}.", {name}));
        return;
      case ItemKind::kSlider: {
        Add(NodeStep(VoiceStepKind::kFocus, target.handle, name));
        std::string said = Tr("{0} slider", {name});
        if (item && !item->value.empty()) {
          said += ", " + PageText(item->value);
        }
        Say(said + ".");
        return;
      }
      case ItemKind::kLink: {
        const bool play =
            verb == ChoiceVerb::kPlay || candidate.role == ChoiceRole::kVideo;
        std::vector<VoiceStep> steps;
        steps.push_back(NodeStep(VoiceStepKind::kPress, target.handle, name));
        if (play) {
          // To play: its player may go full screen (activation_policy.h).
          steps.back().intent = activation::Intent::kPlayMedia;
        }
        steps.push_back(WaitStep(play ? FollowUpKind::kPlayOpened
                                      : FollowUpKind::kAnnouncePage,
                                 name));
        if (Confirm(steps, name, label, target.url)) {
          return;
        }
        Status(Tr(play ? "Opening to play: {0}" : "Opening: {0}", {name}));
        for (VoiceStep& step : steps) {
          Add(std::move(step));
        }
        return;
      }
      case ItemKind::kButton:
      case ItemKind::kCheckBox:
      case ItemKind::kRadio:
      case ItemKind::kSwitch:
      case ItemKind::kSelect:
      case ItemKind::kOption:
      case ItemKind::kMenuItem:
      case ItemKind::kTab:
      case ItemKind::kTreeItem:
      case ItemKind::kClickable: {
        std::vector<VoiceStep> steps;
        steps.push_back(NodeStep(VoiceStepKind::kPress, target.handle, name));
        VoiceStep said = Step(VoiceStepKind::kSpeak);
        said.text = Tr("Pressed {0}.", {name});
        steps.push_back(said);
        if (Confirm(steps, name, label, target.url)) {
          return;
        }
        Status(Tr("Pressing: {0}", {name}));
        for (VoiceStep& step : steps) {
          if (step.kind == VoiceStepKind::kSpeak) {
            Say(step.text);
          } else {
            Add(std::move(step));
          }
        }
        return;
      }
    }
  }

  // ---- Navigation --------------------------------------------------------

  void OpenSite(const VoiceCommand& command) {
    const SiteTarget target = ResolveSpokenSite(
        command.site, command.site_is_address, command.site_words);
    if (target.kind == SiteTarget::Kind::kUnknown) {
      const std::string name = PageText(target.name);
      if (name.empty()) {
        Say("Which site? Say open, then its name or address.");
        return;
      }
      VoiceStep search = Step(VoiceStepKind::kSearchWeb);
      search.text = name;
      search.new_tab = command.new_tab;
      Add(std::move(search));
      Status(Tr("Searching for {0}", {name}));
      Add(WaitStep(
          FollowUpKind::kReadResults, name,
          Tr("I don't know a site called {0}, so I searched for it.", {name})));
      return;
    }
    VoiceStep open = Step(VoiceStepKind::kNavigate);
    open.url = target.url;
    open.new_tab = command.new_tab;
    Add(std::move(open));
    Status(Tr("Opening {0}", {target.name}));
    Add(WaitStep(FollowUpKind::kAnnounceOpened, target.name));
  }

  void SearchTheWeb(const std::string& query, std::string preface) {
    VoiceStep search = Step(VoiceStepKind::kSearchWeb);
    search.text = query;
    Add(std::move(search));
    Status(Tr("Searching for {0}", {query}));
    Add(WaitStep(FollowUpKind::kReadResults, query, std::move(preface)));
  }

  void Search(const VoiceCommand& command) {
    std::string query = command.text;
    SearchScope scope = command.scope;
    if (scope == SearchScope::kNamedSite) {
      const KnownSite* site =
          command.site_is_address ? nullptr : FindKnownSite(command.site);
      // On that site already, with its search box: the box, as the user
      // would (below).
      if (site && env_.web_page && KnownSiteForHost(env_.host) == site &&
          FindSearchField(map_)) {
        scope = SearchScope::kCurrentSite;
      }
    }
    if (scope == SearchScope::kNamedSite) {
      const KnownSite* site =
          command.site_is_address ? nullptr : FindKnownSite(command.site);
      if (site) {
        VoiceStep open = Step(VoiceStepKind::kNavigate);
        open.url = SiteSearchURL(*site, query);
        Add(std::move(open));
        Status(Tr("Searching {0} for {1}", {site->name, query}));
        Add(WaitStep(FollowUpKind::kReadResults, query));
        return;
      }
      if (!command.whole_query.empty()) {
        // "search for things to do on Sunday": not a site.
        query = command.whole_query;
        scope = SearchScope::kAuto;
      } else {
        const std::string name = PageText(command.site_words.empty()
                                              ? command.site
                                              : command.site_words);
        const std::string whole = name + " " + query;
        SearchTheWeb(whole, Tr("I don't know a site called {0}, so I "
                               "searched the web for {1}.",
                               {name, whole}));
        return;
      }
    }
    if (scope == SearchScope::kDefaultEngine) {
      SearchTheWeb(query, std::string());
      return;
    }
    // On this site: its own search box, else its search page, else the
    // web.
    if (env_.web_page) {
      const LocatedPart box = Locate(SitePart::kSearchField);
      if (const PageItem* field = box.field) {
        Add(NodeStep(VoiceStepKind::kSetText, field->handle, query));
        // The site's own search button with its own box (its skill names
        // both), else the form's submit or Enter.
        const PageItem* submit = box.source == PartSource::kSkill
                                     ? SiteItem(map_, map_.site.search_submit)
                                     : nullptr;
        if (submit) {
          Add(NodeStep(VoiceStepKind::kPress, submit->handle,
                       PageText(submit->label)));
        } else {
          Add(NodeStep(VoiceStepKind::kSubmit, field->handle, field->label));
        }
        Status(Tr("Searching for {0}", {query}));
        Add(WaitStep(FollowUpKind::kReadResults, query));
        return;
      }
      if (const KnownSite* site = KnownSiteForHost(env_.host)) {
        VoiceStep open = Step(VoiceStepKind::kNavigate);
        open.url = SiteSearchURL(*site, query);
        Add(std::move(open));
        Status(Tr("Searching {0} for {1}", {site->name, query}));
        Add(WaitStep(FollowUpKind::kReadResults, query));
        return;
      }
    }
    SearchTheWeb(query, scope == SearchScope::kCurrentSite
                            ? Tr("This page has no search box I can use, so I "
                                 "searched the web.")
                            : std::string());
  }

  void NextPage() {
    // The site's own (its skill names it): a link, a button, or more
    // results as the page scrolls.
    const PageItem* next = SiteItem(map_, map_.site.next_page);
    if (!next && map_.site.next_page_scrolls) {
      VoiceStep scroll = Step(VoiceStepKind::kScrollPage);
      scroll.scroll = PageScroll::kDown;
      scroll.count = 2;
      Add(std::move(scroll));
      Say("This site shows more results as you scroll, so I scrolled down. "
          "Say read the results to hear them.");
      return;
    }
    if (!next) {
      next = ItemSaying(map_, ItemKind::kLink,
                        {"next", "next page", "more results"});
    }
    if (!next) {
      next = ItemSaying(map_, ItemKind::kButton,
                        {"next", "next page", "more results", "show more",
                         "load more"});
    }
    if (!next && env_.language == Language::kSpanish) {
      next =
          SpanishItemSaying(map_, ItemKind::kLink, SpanishControl::kNextPage);
    }
    if (!next && env_.language == Language::kSpanish) {
      next =
          SpanishItemSaying(map_, ItemKind::kButton, SpanishControl::kNextPage);
    }
    if (next) {
      const std::string name = PageText(next->label);
      Add(NodeStep(VoiceStepKind::kPress, next->handle, name));
      if (next->kind == ItemKind::kLink) {
        Add(WaitStep(FollowUpKind::kReadResults, std::string()));
      } else {
        Say(Tr("Pressed {0}.", {name}));
      }
      return;
    }
    VoiceStep scroll = Step(VoiceStepKind::kScrollPage);
    scroll.scroll = PageScroll::kDown;
    Add(std::move(scroll));
    Say("There's no next page button, so I scrolled down.");
  }

  void SelectTab(int index) {
    if (index < 0 || static_cast<size_t>(index) >= env_.tabs.size()) {
      Say("There's no such tab.");
      return;
    }
    VoiceStep select = Step(VoiceStepKind::kSelectTab);
    select.index = index;
    Add(std::move(select));
    std::string said = Tr("Tab {0} of {1}", {std::to_string(index + 1),
                                             std::to_string(env_.tabs.size())});
    const std::string title = PageText(env_.tabs[index].title);
    if (!title.empty()) {
      said += ": " + title;
    }
    Say(said + ".");
  }

  // A tab |ref| names; nullopt (after saying why) when none does. When
  // several match, a switch offers them to choose from; a close asks for
  // the number instead, so that a later choice never closes a tab.
  std::optional<int> ChooseTab(const ChoiceRef& ref, bool closing) {
    const CandidateSet tabs = CandidatesFromTabs(env_.tabs);
    MatchOptions options;
    options.current_position = env_.active_tab + 1;
    const ChoiceMatch match = MatchSpokenChoice(ref, tabs.candidates, options);
    if (match.outcome == ChoiceOutcome::kUnique) {
      return tabs.targets[match.index].index;
    }
    if (match.outcome == ChoiceOutcome::kAmbiguous && closing) {
      Say("More than one tab matches. Say close tab, then its number.");
      return std::nullopt;
    }
    if (match.outcome == ChoiceOutcome::kAmbiguous) {
      state().list = ListContext::kTabs;
      state().list_page_id = env_.page_id;
      state().offered = match.close;
      state().offered_handles = HandlesOf(tabs, match.close);
      state().offered_verb = ChoiceVerb::kSelect;
      state().expecting_choice = true;
      Say(OfferSpeech(SubsetOf(tabs, match.close)));
      return std::nullopt;
    }
    NoMatch(ref, match.reason);
    return std::nullopt;
  }

  void SwitchTab(const VoiceCommand& command) {
    const int n = static_cast<int>(env_.tabs.size());
    if (n == 0) {
      Say("There's no tab to switch to.");
      return;
    }
    const ChoiceRef& ref = command.choice;
    if (ref.kind == ChoiceRef::Kind::kRelative &&
        ref.relative != ChoiceRef::Relative::kCurrent) {
      const int step = ref.relative == ChoiceRef::Relative::kNext ? 1 : -1;
      SelectTab(((env_.active_tab + step) % n + n) % n);
      return;
    }
    if (std::optional<int> index = ChooseTab(ref, /*closing=*/false)) {
      SelectTab(*index);
    }
  }

  void CloseTab(const VoiceCommand& command) {
    if (command.choice.kind == ChoiceRef::Kind::kNone) {
      VoiceStep close = Step(VoiceStepKind::kCloseTab);
      close.index = -1;
      Add(std::move(close));
      Say("Closed the tab.");
      return;
    }
    if (std::optional<int> index =
            ChooseTab(command.choice, /*closing=*/true)) {
      VoiceStep close = Step(VoiceStepKind::kCloseTab);
      close.index = *index;
      Add(std::move(close));
      std::string said = Tr("Closed tab {0}", {std::to_string(*index + 1)});
      const std::string title = PageText(env_.tabs[*index].title);
      if (!title.empty()) {
        said += ": " + title;
      }
      Say(said + ".");
    }
  }

  void Scroll(PageScroll direction,
              const VoiceCommand& command,
              std::string_view said) {
    VoiceStep scroll = Step(VoiceStepKind::kScrollPage);
    scroll.scroll = direction;
    scroll.count = std::clamp(command.count, 1, 20);
    scroll.half = command.scroll == ScrollAmount::kHalfPage;
    Add(std::move(scroll));
    Say(said);
  }

  // ---- Media -------------------------------------------------------------

  void Play() {
    if (env_.media_playing) {
      Say("It's already playing.");
      return;
    }
    if (env_.media_session) {
      Add(MediaStep(MediaCommand::kResume));
      Status("Playing");
      Say("Playing.");
      return;
    }
    // The site's player (its skill names it), else the hinted one, else
    // the page's first.
    if (const PageMedia* player = Locate(SitePart::kPlayer).media) {
      const PageMedia& media = *player;
      const std::string name = PageText(media.label);
      Add(NodeStep(VoiceStepKind::kPlayMedia, media.handle, name));
      SetCurrentItem(media.number);
      Status("Playing");
      Say("Playing.");
      return;
    }
    Say("There's nothing to play on this page.");
  }

  void Volume(bool up) {
    const PageItem* slider = ItemSaying(map_, ItemKind::kSlider, {"volume"});
    if (!slider && env_.language == Language::kSpanish) {
      slider =
          SpanishItemSaying(map_, ItemKind::kSlider, SpanishControl::kVolume);
    }
    if (!slider || slider->disabled) {
      Say("I can't find a volume control on this page. Say mute or unmute.");
      return;
    }
    VoiceStep step = NodeStep(
        up ? VoiceStepKind::kIncrement : VoiceStepKind::kDecrement,
        slider->handle, PageText(slider->label));
    step.count = kVolumeSteps;
    Add(std::move(step));
    Say(up ? "Louder." : "Quieter.");
  }

  void FullScreen() {
    if (env_.fullscreen) {
      Say("It's already full screen.");
      return;
    }
    // The player's own button first: it makes the video fill the screen.
    // The page may go full screen from this press (activation_policy.h).
    const PageItem* button = SiteItem(map_, map_.site.fullscreen);
    if (!button) {
      button =
          ItemSaying(map_, ItemKind::kButton, {"full screen", "fullscreen"});
    }
    if (!button && env_.language == Language::kSpanish) {
      button = SpanishItemSaying(map_, ItemKind::kButton,
                                 SpanishControl::kFullScreen);
    }
    if (button) {
      VoiceStep press = NodeStep(VoiceStepKind::kPress, button->handle,
                                 PageText(button->label));
      press.intent = activation::Intent::kFullScreen;
      Add(std::move(press));
      Say("Full screen.");
      return;
    }
    Add(CommandStep(BrowserCommand::kToggleFullScreen));
    Say("Full screen.");
  }

  // "Skip the ad": the site skill's skip button, else a button of the page
  // or of a frame its player is in that says "skip ad".
  void SkipAd() {
    const PageItem* button = SiteItem(map_, map_.site.skip_ad);
    if (!button) {
      button = ItemSaying(map_, ItemKind::kButton,
                          {"skip ad", "skip ads", "skip advertisement"});
    }
    if (!button && env_.language == Language::kSpanish) {
      button =
          SpanishItemSaying(map_, ItemKind::kButton, SpanishControl::kSkipAd);
    }
    if (!button) {
      Say("There's no skip button now. An ad can often be skipped after a "
          "few seconds: say skip the ad again then.");
      return;
    }
    if (button->disabled) {
      Say("The ad can't be skipped yet.");
      return;
    }
    Add(NodeStep(VoiceStepKind::kPress, button->handle,
                 PageText(button->label)));
    Status("Skipping the ad");
    Say("Skipped the ad.");
  }

  void Captions(bool on) {
    const PageItem* button = SiteItem(map_, map_.site.captions);
    if (!button) {
      button = ItemSaying(map_, ItemKind::kButton,
                          {"subtitles", "captions", "closed captions", "cc"});
    }
    if (!button && env_.language == Language::kSpanish) {
      button =
          SpanishItemSaying(map_, ItemKind::kButton, SpanishControl::kCaptions);
    }
    if (!button) {
      Say("I can't find a captions button on this page.");
      return;
    }
    if (button->checked == on) {
      Say(on ? "Captions are already on." : "Captions are already off.");
      return;
    }
    Add(NodeStep(VoiceStepKind::kPress, button->handle,
                 PageText(button->label)));
    Say(on ? "Captions on." : "Captions off.");
  }

  // ---- Reading aloud -----------------------------------------------------

  // A reading the user stopped, which "continue" or "resume" goes on with
  // (rather than media): it was reading when they spoke, or it stopped
  // earlier and nothing has played since.
  bool ResumesReading() const {
    using R = VoiceEnvironment::Reading;
    return !env_.media_playing &&
           (env_.reading == R::kReading ||
            (env_.reading == R::kStopped && !env_.media_session));
  }

  // The reader (voice_reader.h) reads or moves; it says what it does.
  void Read(ReadOp op, std::string section = std::string()) {
    if (op == ReadOp::kSection && section.empty()) {
      Say("Which section? Say read the section on, then its name.");
      return;
    }
    VoiceStep read = Step(VoiceStepKind::kRead);
    read.read = op;
    read.text = std::move(section);
    switch (op) {
      case ReadOp::kPage:
        Status("Reading the page");
        break;
      case ReadOp::kSection:
        Status(Tr("Reading the section on {0}", {read.text}));
        break;
      case ReadOp::kSelection:
        Status("Reading the selection");
        break;
      default:
        break;
    }
    Add(std::move(read));
  }

  // A question about the page ("what is this page about?"): a kAskPage
  // step, which the browser answers with the on-device model from the
  // page's text (voice_page_questions.h). The answer is only said: no step
  // here acts on the page.
  void AskPage(const VoiceCommand& command) {
    if (!env_.web_page) {
      Say("I can only answer questions about web pages.");
      return;
    }
    VoiceStep ask = Step(VoiceStepKind::kAskPage);
    ask.text = SpeechSafeText(command.text, kMaxCommandTextBytes);
    ask.index = static_cast<int>(command.page_question);
    Status(command.page_question == PageQuestionKind::kMore
               ? "Looking for more on the page"
               : "Looking at the page");
    Add(std::move(ask));
  }

  // "Speak faster" and "slower", and "faster" and "slower" while reading:
  // the voice's rate, kept for the window; a reading the command stopped
  // goes on at the new rate.
  void SpeechRate(bool faster) {
    double rate =
        state().speech_rate * (faster ? kSpeechRateStep : 1 / kSpeechRateStep);
    rate = std::clamp(rate, kMinSpeechRate, kMaxSpeechRate);
    const bool reading = env_.reading == VoiceEnvironment::Reading::kReading;
    if (rate == state().speech_rate) {
      Say(faster ? "That's as fast as I go." : "That's as slow as I go.");
    } else {
      state().speech_rate = rate;
      Say(reading ? (faster ? "Faster." : "Slower.")
                  : (faster ? "Speaking faster." : "Speaking slower."));
    }
    if (reading) {
      Read(ReadOp::kResume);
    }
  }

  // ---- Reading -----------------------------------------------------------

  // Reads the page's main list and makes it the one to choose from;
  // false (and nothing said) when there is none.
  bool ReadResults(const std::string& query) {
    const PageList* list =
        Locate(SitePart::kResultList, /*record=*/false).list;
    if (!list) {
      return false;
    }
    SetListContext(ListContext::kPageList, list);
    state().expecting_choice = true;
    const CandidateSet set =
        CandidatesFromList(map_, *list, state().numbers_shown);
    Status(query.empty() ? std::string("Reading the results")
                         : Tr("Results for {0}", {query}));
    Say(ListSpeech(set, Tr("result"), Tr("results"), state().verbosity,
                   kNormalResultsRead, env_.language));
    return true;
  }

  void ReadHeadings() {
    if (map_.headings.empty()) {
      Say("This page has no headings.");
      return;
    }
    SetListContext(ListContext::kHeadings, nullptr);
    state().expecting_choice = true;
    Say(ListSpeech(CandidatesFromHeadings(map_), Tr("heading"), Tr("headings"),
                   state().verbosity, kNormalOutlineRead, env_.language));
  }

  void ReadLinks() {
    const CandidateSet links = CandidatesFromLinks(map_, state().numbers_shown);
    if (links.empty()) {
      Say("This page has no links.");
      return;
    }
    SetListContext(ListContext::kLinks, nullptr);
    state().expecting_choice = true;
    Say(ListSpeech(links, Tr("link"), Tr("links"), state().verbosity,
                   kNormalOutlineRead, env_.language));
  }

  void Describe() {
    std::vector<std::string> parts;
    if (!map_.media.empty()) {
      parts.push_back(Tr(map_.media[0].video ? "a video" : "an audio player"));
    }
    if (const PageList* list = MainList(map_)) {
      parts.push_back(Tr("a list of {0}", {CountOf(list->entries.size(),
                                                   Tr("item"), Tr("items"))}));
    }
    if (FindSearchField(map_)) {
      parts.push_back(Tr("a search box"));
    }
    size_t links = 0;
    size_t fields = 0;
    for (const PageItem& item : map_.items) {
      links += item.kind == ItemKind::kLink;
      fields += item.kind == ItemKind::kTextField;
    }
    if (links) {
      parts.push_back(CountOf(links, Tr("link"), Tr("links")));
    }
    if (fields) {
      parts.push_back(CountOf(fields, Tr("text field"), Tr("text fields")));
    }
    if (!map_.headings.empty()) {
      parts.push_back(
          CountOf(map_.headings.size(), Tr("heading"), Tr("headings")));
    }
    const std::string title = PageText(map_.title);
    std::string said = title.empty() ? Tr("This page") : title;
    if (parts.empty()) {
      Say(Tr("{0}. I can't find anything to read or choose on it.", {said}));
      return;
    }
    said += Tr(". On it: ");
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) {
        said += i + 1 == parts.size() ? Tr(" and ") : ", ";
      }
      said += parts[i];
    }
    Say(said + ".");
  }

  void WhereAmI() {
    std::string said;
    const std::string title = PageText(map_.title);
    const std::string host = PageText(env_.host);
    if (!host.empty() && env_.web_page) {
      said = Tr("You're on {0}", {host});
      said += title.empty() ? std::string(".") : ": " + title + ".";
    } else {
      said = title.empty() ? Tr("You're on a browser page.")
                           : Tr("You're on {0}.", {title});
    }
    if (env_.tabs.size() > 1) {
      said += Tr(" Tab {0} of {1}.", {std::to_string(env_.active_tab + 1),
                                      std::to_string(env_.tabs.size())});
    }
    Say(said);
  }

  void FindInPage(const std::string& query) {
    const std::string needle = FoldForComparison(query);
    if (needle.empty()) {
      Say("Find what? Say find, then the words.");
      return;
    }
    auto has = [&](const std::string& text) {
      return (" " + FoldForComparison(text) + " ").find(" " + needle + " ") !=
             std::string::npos;
    };
    size_t found = 0;
    std::string where;
    PageNodeHandle target;
    int number = 0;
    for (const PageHeading& heading : map_.headings) {
      if (has(heading.text) && found++ == 0) {
        where = Tr("the heading {0}", {PageText(heading.text)});
        target = heading.handle;
      }
    }
    for (const PageItem& item : map_.items) {
      if (has(item.label) && found++ == 0) {
        // "the link", "the button": in the window's language.
        where = Localized(env_.language,
                          "the " + std::string(ItemKindName(item.kind))) +
                " " + PageText(item.label);
        target = item.handle;
        number = item.number;
      }
    }
    if (!found) {
      Say(Tr("I can't find {0} in this page's headings, links or buttons.",
             {Quoted(PageText(query))}));
      return;
    }
    Add(NodeStep(VoiceStepKind::kScrollIntoView, target, where));
    if (number) {
      SetCurrentItem(number);
    }
    std::string said = Tr("Found it in {0}.", {where});
    if (found > 1) {
      said += Tr(" It's in {0} places.", {std::to_string(found)});
    }
    Say(said);
  }

  void MoveLink(bool next) {
    ConversationState& s = state();
    std::vector<const PageItem*> links;
    for (const PageItem& item : map_.items) {
      if (item.kind == ItemKind::kLink) {
        links.push_back(&item);
      }
    }
    const int n = static_cast<int>(links.size());
    if (n == 0) {
      Say("This page has no links.");
      return;
    }
    const int at = s.link_cursor + (next ? 1 : -1);
    if (at < 1 || at > n) {
      Say(next ? "That's the last link." : "That's the first link.");
      return;
    }
    s.link_cursor = at;
    const PageItem& link = *links[at - 1];
    Add(NodeStep(VoiceStepKind::kFocus, link.handle, PageText(link.label)));
    SetCurrentItem(link.number);
    Say(Tr("Link {0}: {1}.",
           {std::to_string(link.number), PageText(link.label)}));
  }

  void MoveItem(bool next) {
    ConversationState& s = state();
    const CandidateSet set = ContextSet();
    if (s.list == ListContext::kNone || set.empty()) {
      if (next && env_.media_playing) {
        Add(MediaStep(MediaCommand::kNextTrack));
        Say("Next.");
        return;
      }
      Say("There's no list to move through. Say read the results.");
      return;
    }
    const int n = static_cast<int>(set.size());
    const int at = s.current_position + (next ? 1 : -1);
    if (at < 1 || at > n) {
      Say(next ? "That's the last one." : "That's the first one.");
      return;
    }
    s.current_position = at;
    const ChoiceCandidate& candidate = set.candidates[at - 1];
    const CandidateTarget& target = set.targets[at - 1];
    if (target.source == CandidateSource::kTab) {
      Say(std::to_string(at) + ": " +
          SpokenCandidate(candidate, state().verbosity) + ".");
      return;
    }
    if (target.handle.valid()) {
      Add(NodeStep(VoiceStepKind::kScrollIntoView, target.handle,
                   PageText(candidate.title)));
      if (target.item_number) {
        SetCurrentItem(target.item_number);
      }
    }
    Say(std::to_string(SpokenNumber(candidate)) + ": " +
        SpokenCandidate(candidate, state().verbosity) + ".");
  }

  // ---- Dictation ---------------------------------------------------------

  void Dictation(const VoiceCommand& command) {
    const PageItem* field = FieldInFocus(state(), map_, env_);
    if (!field) {
      // A page with one field to type in: that one.
      const PageItem* only = nullptr;
      size_t writable = 0;
      for (const PageItem& item : map_.items) {
        if (item.kind == ItemKind::kTextField && item.writable) {
          only = &item;
          ++writable;
        }
      }
      if (writable == 1) {
        field = only;
      }
    }
    if (!field) {
      Say("Choose a field first: say its name or its number.");
      return;
    }
    const std::string name =
        field->label.empty() ? Tr("The field") : PageText(field->label);
    if (field->credential != CredentialKind::kNone) {
      Say(
          Tr("{0} is a {1} field. Voice mode never types or reads passwords, "
             "card numbers or codes.",
             {name, Tr(CredentialKindName(field->credential))}));
      return;
    }
    state().field = field->handle;
    state().field_page_id = env_.page_id;
    const std::string& value = field->value;
    // The map's copy of a value is capped: one that long may be cut, so it
    // isn't written back.
    const bool complete = CountCodePoints(value) < kMaxItemValueChars;
    switch (command.kind) {
      case K::kReadBack:
        Say(value.empty() ? Tr("{0} is empty.", {name})
                          : Tr("{0} says: {1}.", {name, PageText(value)}));
        return;
      case K::kSubmit:
        Submit(*field, name);
        return;
      default:
        break;
    }
    if (!field->writable) {
      Say(Tr("I can't type in {0}.", {name}));
      return;
    }
    std::string text;
    std::string said;
    switch (command.kind) {
      case K::kType:
        text = value.empty() ? command.text : value + " " + command.text;
        said = "Typed.";
        break;
      case K::kNewLine:
        text = value + "\n";
        said = "New line.";
        break;
      case K::kDeleteLastWord: {
        std::string rest = value;
        while (!rest.empty() && rest.back() == ' ') {
          rest.pop_back();
        }
        const size_t space = rest.rfind(' ');
        if (rest.empty()) {
          Say(Tr("{0} is empty.", {name}));
          return;
        }
        text = space == std::string::npos ? std::string()
                                          : rest.substr(0, space);
        said = Tr("Deleted {0}.",
                  {std::string_view(rest).substr(
                      space == std::string::npos ? 0 : space + 1)});
        break;
      }
      case K::kClearField:
        text.clear();
        said = "Cleared.";
        break;
      default:
        return;
    }
    if (!complete && command.kind != K::kClearField) {
      Say(
          Tr("{0} already holds long text. Say clear the field, then type "
             "it again.",
             {name}));
      return;
    }
    Add(NodeStep(VoiceStepKind::kSetText, field->handle, text));
    Say(said);
  }

  void Submit(const PageItem& field, const std::string& name) {
    std::vector<VoiceStep> steps;
    steps.push_back(NodeStep(VoiceStepKind::kSubmit, field.handle, name));
    if (field.search_field) {
      for (VoiceStep& step : steps) {
        Add(std::move(step));
      }
      Status(Tr("Searching for {0}", {PageText(field.value)}));
      Add(WaitStep(FollowUpKind::kReadResults, PageText(field.value)));
      return;
    }
    // A form that isn't a search may send something: ask first, as the
    // web agent does before submitting a form.
    steps.push_back(WaitStep(FollowUpKind::kAnnouncePage, std::string()));
    AskToConfirm(std::move(steps), Tr("send what is in {0}", {name}));
  }

  // ---- Learning (§6.8) ----------------------------------------------------

  // The learned hint for the page's site, while learning is on.
  const SiteHint* Hint() const {
    return env_.learning.enabled && !env_.learning.hint.host.empty()
               ? &env_.learning.hint
               : nullptr;
  }

  // |part| of the site, found as LocateSitePart() orders it, and noted for
  // its hint unless |record| is false (a list is noted once chosen from).
  LocatedPart Locate(SitePart part, bool record = true) {
    // The site's built-in skill (§6.7) is the first source: what it named
    // when the map was built (PageMap::site).
    const LocatedPart skill = SkillPart(part);
    const LocatedPart found =
        LocateSitePart(part, map_, skill.found() ? &skill : nullptr, Hint());
    if (record && found.found()) {
      NoteSitePart(part, found);
    }
    return found;
  }

  // |part| as the site's skill named it on this map, or nothing found.
  LocatedPart SkillPart(SitePart part) const {
    LocatedPart found;
    switch (part) {
      case SitePart::kSearchField:
        if (map_.site.search_field.valid() &&
            map_.InPage(map_.site.search_field)) {
          const PageItem* field = map_.ItemForHandle(map_.site.search_field);
          if (field && field->kind == ItemKind::kTextField && field->writable &&
              field->credential == CredentialKind::kNone) {
            found.field = field;
          }
        }
        break;
      case SitePart::kResultList:
        if (map_.site.results && !map_.lists.empty() &&
            map_.lists[0].from_site) {
          found.list = &map_.lists[0];
        }
        break;
      case SitePart::kPlayer:
        if (map_.site.media.valid()) {
          for (const PageMedia& media : map_.media) {
            if (media.handle == map_.site.media) {
              found.media = &media;
              break;
            }
          }
        }
        break;
    }
    return found;
  }

  void NoteSitePart(SitePart part, const LocatedPart& found) {
    if (found.source == PartSource::kSkill || !env_.web_page) {
      return;
    }
    const SiteHint* hint = Hint();
    SitePartUse use;
    use.part = part;
    use.source = found.source;
    use.host = env_.host;
    use.name = HintName(part, found);
    use.hint_missed = hint && !hint->name(part).empty() &&
                      found.source == PartSource::kGeneric;
    turn_.site_parts.push_back(std::move(use));
  }

  void UsedList(const PageList& list) {
    const SiteHint* hint = Hint();
    LocatedPart found;
    found.list = &list;
    // The site skill's results aren't learned (the skill knows them).
    found.source = list.from_site ? PartSource::kSkill
                   : hint && !hint->result_list.empty() &&
                           FoldForComparison(hint->result_list) ==
                               FoldForComparison(list.label)
                       ? PartSource::kHint
                       : PartSource::kGeneric;
    NoteSitePart(SitePart::kResultList, found);
  }

  static VoiceStep LearningStep(LearningOp op, std::string text) {
    VoiceStep step = Step(VoiceStepKind::kLearning);
    step.index = static_cast<int>(op);
    step.text = std::move(text);
    return step;
  }

  void RememberRoutine(const std::string& said) {
    const LearningEnvironment& learning = env_.learning;
    if (!learning.routines_available) {
      Say("I can't keep routines here.");
      return;
    }
    const std::string name = RoutineDisplayName(said);
    if (name.empty()) {
      Say("Say remember this as, then a short name for it.");
      return;
    }
    const std::string spoken = PageText(name);
    if (FindRoutineName(learning.routines, name)) {
      Say("You already have a routine called " + spoken + ". Say forget " +
          spoken + " first, or choose another name.");
      return;
    }
    if (learning.this_steps.empty()) {
      Say("There's nothing to remember yet. Do it once, then say remember "
          "this as " + spoken + ".");
      return;
    }
    // A step of the AI's that a routine can't repeat (planner-routines).
    if (!learning.this_refusal.empty()) {
      Say("I can't remember this: " + learning.this_refusal);
      return;
    }
    Status("Remembering " + spoken);
    Say("I'll remember: " + JoinSpokenList(learning.this_steps) + ".");
    if (learning.this_planned) {
      Say("Some of these steps are the AI's, so you'll confirm the sites they "
          "use before it runs.");
    }
    VoiceStep save = LearningStep(LearningOp::kSaveRoutine, name);
    save.count = learning.this_version;
    std::vector<VoiceStep> steps;
    steps.push_back(std::move(save));
    AskToConfirm(std::move(steps), "save it as " + spoken);
  }

  void RunRoutine(const std::string& said) {
    const LearningEnvironment& learning = env_.learning;
    if (!learning.routines_available) {
      Say("I can't run routines here.");
      return;
    }
    const std::string* name = FindRoutineName(learning.routines, said);
    if (!name) {
      Say(learning.routines.empty()
              ? std::string("You don't have any routines yet. Do something, "
                            "then say remember this as, and a name.")
              : "I don't have a routine called " + PageText(said) +
                    ". Say what are my routines.");
      return;
    }
    Status("Running " + PageText(*name));
    Add(LearningStep(LearningOp::kRunRoutine, *name));
  }

  void ForgetRoutine(const std::string& said) {
    const LearningEnvironment& learning = env_.learning;
    const std::string* name = learning.routines_available
                                  ? FindRoutineName(learning.routines, said)
                                  : nullptr;
    if (!name) {
      Say("I don't have a routine called " + PageText(said) + ".");
      return;
    }
    std::vector<VoiceStep> steps;
    steps.push_back(LearningStep(LearningOp::kForgetRoutine, *name));
    AskToConfirm(std::move(steps), "forget the routine " + PageText(*name));
  }

  void ListRoutines() {
    const std::vector<std::string>& names = env_.learning.routines;
    if (!env_.learning.routines_available || names.empty()) {
      Say("You don't have any routines yet. Do something, then say remember "
          "this as, and a name.");
      return;
    }
    std::vector<std::string> read;
    for (size_t i = 0; i < names.size() && i < kMaxRoutinesRead; ++i) {
      read.push_back(PageText(names[i]));
    }
    std::string said = names.size() == 1
                           ? std::string("You have one routine: ")
                           : "You have " + std::to_string(names.size()) +
                                 " routines: ";
    said += JoinSpokenList(read);
    if (names.size() > read.size()) {
      said += ", and more in Agent mode";
    }
    Say(said + ". Say run, then its name.");
  }

  void ForgetWord(const std::string& said) {
    const std::string key = FoldForComparison(said);
    const std::vector<std::string>& words = env_.learning.words;
    if (key.empty() ||
        std::find(words.begin(), words.end(), key) == words.end()) {
      Say("I haven't learned the word " + PageText(said) + ".");
      return;
    }
    std::vector<VoiceStep> steps;
    steps.push_back(LearningStep(LearningOp::kForgetWord, said));
    AskToConfirm(std::move(steps), "forget the word " + PageText(said));
  }

  void ForgetLearning() {
    std::vector<VoiceStep> steps;
    steps.push_back(LearningStep(LearningOp::kForgetEverything, std::string()));
    Say("That's your routines, the words and corrections I learned, and the "
        "site hints.");
    AskToConfirm(std::move(steps), "forget everything I learned from you");
  }

  // ---- Conversation ------------------------------------------------------

  void Unknown(const VoiceCommand& command) {
    // What was heard is said back cleaned and capped, as page text is: an
    // utterance can hold characters no voice should be given.
    const std::string echo = PageText(command.heard);
    if (echo.empty()) {
      Say("I didn't catch that.");
      return;
    }
    // A "yes" or "no" with no question pending (it expired, or another
    // command dropped it) is no task for the planner.
    ParseContext answering = ContextFor(state(), map_, env_);
    answering.expecting_yes_no = true;
    const VoiceCommandKind answer =
        ParseVoiceCommand(command.heard, answering).kind;
    if (answer == K::kYes || answer == K::kNo) {
      Say("There's nothing to confirm.");
      return;
    }
    // A routine's name on its own runs it (before the planner is asked).
    if (const std::string* name =
            FindRoutineName(env_.learning.routines, command.heard)) {
      RunRoutine(*name);
      return;
    }
    VoiceStep planner = Step(VoiceStepKind::kToPlanner);
    planner.text = command.heard;
    planner.preface = Tr(
        "I can't do that yet: {0}. Say what can I say to hear what I can do.",
        {Quoted(echo)});
    Add(std::move(planner));
  }

  void SetVerbosity(Verbosity requested) {
    Verbosity& v = state().verbosity;
    switch (requested) {
      case Verbosity::kBrief:
      case Verbosity::kNormal:
      case Verbosity::kFull:
        v = requested;
        break;
      case Verbosity::kMore:
        v = v == Verbosity::kBrief ? Verbosity::kNormal : Verbosity::kFull;
        break;
      case Verbosity::kLess:
        v = v == Verbosity::kFull ? Verbosity::kNormal : Verbosity::kBrief;
        break;
    }
    Say(v == Verbosity::kBrief    ? "Brief answers."
        : v == Verbosity::kNormal ? "Normal detail."
                                  : "Full detail.");
  }

  void Help() {
    std::vector<VoiceCommandKind> kinds;
    if (ConfirmationValid(state(), env_)) {
      kinds = {K::kYes, K::kNo};
    }
    if (FieldInFocus(state(), map_, env_)) {
      kinds.insert(kinds.end(), {K::kType, K::kReadBack, K::kSubmit});
    }
    if (MainList(map_) || ListValid(state(), env_)) {
      kinds.insert(kinds.end(), {K::kChoose, K::kReadResults});
    }
    if (env_.media_session || !map_.media.empty()) {
      kinds.insert(kinds.end(),
                   {K::kPause, K::kVolumeUp, K::kSkipForward, K::kFullScreen});
    }
    kinds.insert(kinds.end(), {K::kOpenSite, K::kSearch, K::kBack,
                               K::kReadHeadings, K::kWhereAmI, K::kStop});
    std::vector<std::string> examples;
    for (VoiceCommandKind kind : kinds) {
      if (examples.size() >= kMaxHelpExamples) {
        break;
      }
      for (const CommandInfo& info : CommandInfos(env_.language)) {
        if (info.kind == kind && !info.example.empty()) {
          examples.push_back(std::string(info.example));
          break;
        }
      }
    }
    std::string said = Tr("You can say: ");
    for (size_t i = 0; i < examples.size(); ++i) {
      if (i > 0) {
        said += i + 1 == examples.size() ? Tr("; or ") : "; ";
      }
      said += examples[i];
    }
    Say(ChunkSentences(
        {said + ".", Tr("The full list is in Voice commands, in the help.")}));
  }

  // A planner lives on its Plan*() function's stack, inside its inputs' lives.
  MOLT_RAW_PTR_EXCLUSION const PageMap& map_;
  MOLT_RAW_PTR_EXCLUSION const VoiceEnvironment& env_;
  VoiceTurn turn_;
  std::vector<std::string> spoken_;
};

}  // namespace

VoiceStep::VoiceStep() = default;
VoiceStep::~VoiceStep() = default;
VoiceStep::VoiceStep(const VoiceStep&) = default;
VoiceStep& VoiceStep::operator=(const VoiceStep&) = default;
VoiceStep::VoiceStep(VoiceStep&&) = default;
VoiceStep& VoiceStep::operator=(VoiceStep&&) = default;

VoiceEnvironment::VoiceEnvironment() = default;
VoiceEnvironment::~VoiceEnvironment() = default;
VoiceEnvironment::VoiceEnvironment(const VoiceEnvironment&) = default;
VoiceEnvironment& VoiceEnvironment::operator=(const VoiceEnvironment&) =
    default;
VoiceEnvironment::VoiceEnvironment(VoiceEnvironment&&) = default;
VoiceEnvironment& VoiceEnvironment::operator=(VoiceEnvironment&&) = default;

ConversationState::ConversationState() = default;
ConversationState::~ConversationState() = default;
ConversationState::ConversationState(const ConversationState&) = default;
ConversationState& ConversationState::operator=(const ConversationState&) =
    default;
ConversationState::ConversationState(ConversationState&&) = default;
ConversationState& ConversationState::operator=(ConversationState&&) =
    default;

VoiceTurn::VoiceTurn() = default;
VoiceTurn::~VoiceTurn() = default;
VoiceTurn::VoiceTurn(const VoiceTurn&) = default;
VoiceTurn& VoiceTurn::operator=(const VoiceTurn&) = default;
VoiceTurn::VoiceTurn(VoiceTurn&&) = default;
VoiceTurn& VoiceTurn::operator=(VoiceTurn&&) = default;

std::string_view FailureText(StepFailure failure) {
  switch (failure) {
    case StepFailure::kNotActive:
      return "Voice mode is off in this tab.";
    case StepFailure::kStale:
      return "The page changed before I could do that. Say it again.";
    case StepFailure::kNotAllowed:
      return "I can't do that on this page.";
    case StepFailure::kDisabled:
      return "That's turned off on the page.";
    case StepFailure::kNotEditable:
      return "I can't type there.";
    case StepFailure::kCredentialField:
      return "That's a password, card or code field. Voice mode never types "
             "or reads those.";
    case StepFailure::kTooLong:
      return "That's too much text to type.";
    case StepFailure::kNotMedia:
      return "That isn't something I can play.";
    case StepFailure::kNoMediaSession:
      return "Nothing has played in this tab yet.";
    case StepFailure::kFocusNotConfirmed:
      return "The page didn't let me send that.";
    case StepFailure::kCancelled:
      return "";
    case StepFailure::kBlockedURL:
      return "I can only open web pages.";
    case StepFailure::kNoTab:
      return "There's no tab to do that in.";
    case StepFailure::kUnavailable:
      return "I can't do that here.";
  }
  return "";
}

std::string_view FailureText(StepFailure failure, Language language) {
  return Translated(language, FailureText(failure));
}

ParseContext ContextFor(const ConversationState& state,
                        const PageMap& map,
                        const VoiceEnvironment& env) {
  ParseContext context;
  context.language = env.language;
  context.expecting_yes_no = ConfirmationValid(state, env);
  context.expecting_choice =
      (state.expecting_choice && ListValid(state, env)) || state.numbers_shown;
  context.text_field_focused = FieldInFocus(state, map, env) != nullptr;
  return context;
}

VoiceTurn PlanUtterance(std::string_view utterance,
                        const PageMap& map,
                        const VoiceEnvironment& env,
                        const ConversationState& state) {
  return PlanCommand(
      ParseVoiceCommand(utterance, ContextFor(state, map, env)), map, env,
      state);
}

VoiceTurn PlanCommand(const VoiceCommand& command,
                      const PageMap& map,
                      const VoiceEnvironment& env,
                      const ConversationState& state) {
  Planner planner(map, env, state);
  planner.Plan(command, /*depth=*/0);
  return planner.Finish();
}

VoiceTurn PlanFollowUp(const VoiceStep& wait,
                       const PageMap& map,
                       const VoiceEnvironment& env,
                       const ConversationState& state) {
  Planner planner(map, env, state);
  planner.FollowUp(wait);
  return planner.Finish();
}

VoiceTurn PlanPlannerQuestion(int question,
                              std::string_view what,
                              const VoiceEnvironment& env,
                              const ConversationState& state) {
  const PageMap no_map;
  Planner planner(no_map, env, state);
  planner.PlannerQuestion(question, what);
  return planner.Finish();
}

int PlannerQuestionHeld(const ConversationState& state) {
  if (!state.confirming) {
    return 0;
  }
  for (const VoiceStep& held : state.confirm_steps) {
    if (held.kind == VoiceStepKind::kPlannerAnswer && held.flag) {
      return held.index;
    }
  }
  return 0;
}

VoiceTurn PlanPageRequestQuestion(int question,
                                  std::string_view said,
                                  std::string_view verb,
                                  const VoiceEnvironment& env,
                                  const ConversationState& state) {
  const PageMap no_map;
  Planner planner(no_map, env, state);
  planner.PageRequestQuestion(question, said, verb);
  return planner.Finish();
}

VoiceTurn PlanBrowserPrompt(int id,
                            const BrowserPrompt& prompt,
                            std::string_view word,
                            const VoiceEnvironment& env,
                            const ConversationState& state) {
  const PageMap no_map;
  Planner planner(no_map, env, state);
  planner.ReadPrompt(id, prompt, word);
  return planner.Finish();
}

VoiceTurn PlanBrowserPromptGone(int id,
                                std::string_view said,
                                const VoiceEnvironment& env,
                                const ConversationState& state) {
  const PageMap no_map;
  Planner planner(no_map, env, state);
  planner.PromptGone(id, said);
  return planner.Finish();
}

int BrowserPromptHeld(const ConversationState& state, int64_t now_ms) {
  return PromptWaits(state.prompt, now_ms) ? state.prompt.id : 0;
}

bool AnswersBrowserPrompt(const VoiceCommand& command,
                          const VoiceEnvironment& env,
                          const ConversationState& state) {
  return !env.moltnet && PromptWaits(state.prompt, env.now_ms) &&
         PromptReplyFor(command, env, state).answer != PromptAnswer::kNone;
}

int PageRequestQuestionHeld(const ConversationState& state) {
  if (!state.confirming) {
    return 0;
  }
  for (const VoiceStep& held : state.confirm_steps) {
    if (held.kind == VoiceStepKind::kPageRequestAnswer && held.flag) {
      return held.index;
    }
  }
  return 0;
}

bool NeedsConfirmation(std::string_view label,
                       std::string_view url,
                       const PageMap& map) {
  const std::vector<std::string> words =
      SplitWords(FoldForComparison(label));
  for (std::string_view word : kConfirmWords) {
    if (HasWord(words, word)) {
      return true;
    }
  }
  for (size_t i = 0; i + 1 < words.size(); ++i) {
    for (const auto& pair : kConfirmPairs) {
      if (words[i] == pair[0] && words[i + 1] == pair[1]) {
        return true;
      }
    }
  }
  const std::string lower_url = Lower(url.substr(0, kMaxUrlScan));
  for (std::string_view path : kSignOutPaths) {
    if (lower_url.find(path) != std::string::npos) {
      return true;
    }
  }
  for (const PageItem& item : map.items) {
    if (item.credential == CredentialKind::kPaymentCard) {
      return true;
    }
  }
  return false;
}

// What a Spanish page's buttons say for the same actions (folded).
constexpr std::string_view kSpanishConfirmWords[] = {
    "comprar",     "compra",    "pagar",       "pago",          "pedido",
    "pedir",       "tramitar",  "eliminar",    "borrar",        "suprimir",
    "descartar",   "papelera",  "enviar",      "publicar",      "responder",
    "comentar",    "confirmar", "suscribirse", "suscribir",     "suscribete",
    "desuscribir", "donar",     "transferir",  "transferencia",
};
constexpr std::string_view kSpanishConfirmPairs[][2] = {
    {"cerrar", "sesion"},
    {"finalizar", "compra"},
    {"realizar", "pedido"},
};

bool NeedsConfirmation(std::string_view label,
                       std::string_view url,
                       const PageMap& map,
                       Language language) {
  if (NeedsConfirmation(label, url, map)) {
    return true;
  }
  if (language != Language::kSpanish) {
    return false;
  }
  const std::vector<std::string> words = SplitWords(FoldForComparison(label));
  for (std::string_view word : kSpanishConfirmWords) {
    if (HasWord(words, word)) {
      return true;
    }
  }
  for (size_t i = 0; i + 1 < words.size(); ++i) {
    for (const auto& pair : kSpanishConfirmPairs) {
      if (words[i] == pair[0] && words[i + 1] == pair[1]) {
        return true;
      }
    }
  }
  return false;
}

const PageItem* SiteItem(const PageMap& map, const PageNodeHandle& part) {
  if (!part.valid() || !map.InPage(part)) {
    return nullptr;
  }
  const PageItem* item = map.ItemForHandle(part);
  return item && !item->disabled ? item : nullptr;
}

const PageItem* FindSearchField(const PageMap& map) {
  // The site's (its skill names it).
  if (map.site.search_field.valid() && map.InPage(map.site.search_field)) {
    const PageItem* field = map.ItemForHandle(map.site.search_field);
    if (field && field->kind == ItemKind::kTextField && field->writable &&
        field->credential == CredentialKind::kNone) {
      return field;
    }
  }
  const PageItem* found = nullptr;
  for (const PageItem& item : map.items) {
    // Never a frame's: what the user searches for isn't typed into a box
    // another site (an ad, a widget) put on the page.
    if (item.kind != ItemKind::kTextField || !item.writable ||
        !item.search_field || !map.InPage(item.handle)) {
      continue;
    }
    if (item.on_screen) {
      return &item;
    }
    if (!found) {
      found = &item;
    }
  }
  return found;
}

}  // namespace voice
}  // namespace molt_ai
