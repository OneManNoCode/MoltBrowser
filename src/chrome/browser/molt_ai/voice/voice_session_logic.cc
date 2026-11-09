// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_session_logic.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "chrome/browser/molt_ai/voice/phonetic_key.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_said_aloud.h"

namespace molt_ai {
namespace voice {

namespace {

// What the browser said is folded from at most this many bytes.
constexpr size_t kMaxSaidBytes = 4096;

// Between the two forms of what the browser said (SaidWords()): never a
// folded word, so no run of words reaches across it.
constexpr std::string_view kFormSeam = "|";

// What the browser said, as folded words: as written, and as its voice
// says them (voice_said_aloud.h: "example dot com", "two", "molt
// browser"), which is how an echo the canceller missed is heard.
std::string SaidWords(std::string_view text) {
  text = text.substr(0, kMaxSaidBytes);
  std::string words = FoldForComparison(text);
  const std::string aloud =
      FoldForComparison(SaidAloudWords(text, kMaxSaidBytes));
  if (!aloud.empty()) {
    if (!words.empty()) {
      words.push_back(' ');
      words.append(kFormSeam);
      words.push_back(' ');
    }
    words.append(aloud);
  }
  return words;
}

std::vector<std::string_view> SplitWords(std::string_view text) {
  std::vector<std::string_view> words;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find(' ', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    if (end > start) {
      words.push_back(text.substr(start, end - start));
    }
    start = end + 1;
  }
  return words;
}

// Whether |wanted| is in |said| in order, from any word of it, with at
// most one word skipped between two of them (one the recognizer lost):
// how the browser's own words sound heard back. Not across kFormSeam.
bool SaysInOrder(const std::vector<std::string_view>& said,
                 const std::vector<std::string_view>& wanted) {
  if (wanted.empty()) {
    return false;
  }
  for (size_t start = 0; start < said.size(); ++start) {
    if (said[start] != wanted[0]) {
      continue;
    }
    size_t at = start;
    size_t matched = 1;
    while (matched < wanted.size()) {
      bool found = false;
      for (size_t next = at + 1; next < said.size() && next <= at + 2;
           ++next) {
        if (said[next] == kFormSeam) {
          break;
        }
        if (said[next] == wanted[matched]) {
          at = next;
          found = true;
          break;
        }
      }
      if (!found) {
        break;
      }
      ++matched;
    }
    if (matched == wanted.size()) {
      return true;
    }
  }
  return false;
}

// Whether |word| sounds like the folded name |name| (the wake phrase's
// last word): it is the name, about as long with its phonetic key ("mold"
// and "moult" for "molt"), or begins with it ("moltbrowser", "molts").
bool SoundsLikeName(std::string_view word,
                    std::string_view name,
                    std::string_view key) {
  if (word.size() >= name.size() && word.substr(0, name.size()) == name) {
    return true;
  }
  // A sound-alike is about as long as the name ("mold", "mould").
  const bool near_length =
      word.size() + 1 >= name.size() && word.size() <= name.size() + 2;
  return near_length && PhoneticKey(word) == key;
}

// Whether |word| sounds like the folded |first|, the wake phrase's word
// before its name: it is that word, or its key is within a vowel or an "h"
// of it ("hay", "hi", "a" or "eh" for "hey").
bool SoundsLikeFirst(std::string_view word,
                     std::string_view first,
                     std::string_view first_key) {
  if (word == first) {
    return true;
  }
  const std::string key = PhoneticKey(word);
  return !key.empty() && !first_key.empty() &&
         PhoneticDistance(key, first_key, 1) <= 1;
}

// Whether the folded words |haystack| hold a sound-alike of the folded
// wake phrase |phrase| as the speech helper's spotter could hear it: the
// phrase's word before its name (or one that sounds like it) right before
// the name or a word that sounds like it ("hey mold", "hay moult", "hey
// moltbrowser"). The name, or a word like it, on its own ("black mold",
// "birds molt") is not the phrase. A phrase of one word: that word or one
// like it. Only for names of four letters or more, whose keys are specific
// enough.
bool SaysSoundAlike(std::string_view haystack, std::string_view phrase) {
  const size_t space = phrase.rfind(' ');
  const std::string_view name =
      space == std::string_view::npos ? phrase : phrase.substr(space + 1);
  const std::string_view before =
      space == std::string_view::npos ? std::string_view()
                                      : phrase.substr(0, space);
  const std::string_view first = before.substr(before.rfind(' ') + 1);
  const std::string key = PhoneticKey(name);
  if (name.size() < 4 || key.size() < 3) {
    return false;
  }
  const std::string first_key = PhoneticKey(first);
  std::string_view previous;
  size_t start = 0;
  while (start < haystack.size()) {
    size_t end = haystack.find(' ', start);
    if (end == std::string_view::npos) {
      end = haystack.size();
    }
    const std::string_view word = haystack.substr(start, end - start);
    if (SoundsLikeName(word, name, key) &&
        (first.empty() ||
         (!previous.empty() && SoundsLikeFirst(previous, first, first_key)))) {
      return true;
    }
    previous = word;
    start = end + 1;
  }
  return false;
}

// Whether the folded words |haystack| hold the folded words |needle| as
// whole words.
bool HasWords(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return false;
  }
  size_t at = haystack.find(needle);
  while (at != std::string_view::npos) {
    const bool starts = at == 0 || haystack[at - 1] == ' ';
    const size_t end = at + needle.size();
    const bool ends = end == haystack.size() || haystack[end] == ' ';
    if (starts && ends) {
      return true;
    }
    at = haystack.find(needle, at + 1);
  }
  return false;
}

}  // namespace

std::string_view EarconKindName(EarconKind kind) {
  switch (kind) {
    case EarconKind::kOff:
      return "off";
    case EarconKind::kWaiting:
      return "waiting";
    case EarconKind::kListening:
      return "listening";
    case EarconKind::kThinking:
      return "thinking";
    case EarconKind::kSpeaking:
      return "speaking";
  }
  return "";
}

std::string_view SessionStateName(SessionState state) {
  switch (state) {
    case SessionState::kOff:
      return "off";
    case SessionState::kStarting:
      return "starting";
    case SessionState::kWaiting:
      return "waiting";
    case SessionState::kListening:
      return "listening";
    case SessionState::kThinking:
      return "thinking";
    case SessionState::kSpeaking:
      return "speaking";
  }
  return "unknown";
}

bool IsAlwaysHeardCommand(std::string_view text) {
  return IsAlwaysHeardCommand(text, Language::kEnglish);
}

bool IsAlwaysHeardCommand(std::string_view text, Language language) {
  ParseContext context;
  context.language = language;
  switch (ParseVoiceCommand(text, context).kind) {
    case VoiceCommandKind::kStop:
    case VoiceCommandKind::kStopSpeaking:
    case VoiceCommandKind::kCancel:
    case VoiceCommandKind::kVoiceModeOff:
      return true;
    default:
      return false;
  }
}

namespace {

// Whether |command| stays with what the browser reads or does
// (TakenOverOwnVoice()).
bool StaysWithWhatIsRead(const VoiceCommand& command) {
  // The browser's own actions (windows, settings, downloads, bookmarks,
  // the AI, its menus) wait for the browser to be quiet, as opening a site
  // does. Starting over drops what it reads or asks, as "cancel" does: at
  // once.
  if (ActsOnTheBrowser(command.kind)) {
    return false;
  }
  switch (command.kind) {
    case VoiceCommandKind::kChoose:
      // A place in the list ("the third one", "number four", "the next
      // one"); a description may match nothing and go to a site or the
      // planner ("open evil dot com", "buy the cheapest one").
      return command.choice.kind == ChoiceRef::Kind::kPosition ||
             command.choice.kind == ChoiceRef::Kind::kNumber ||
             command.choice.kind == ChoiceRef::Kind::kRelative;
    case VoiceCommandKind::kUnknown:
    case VoiceCommandKind::kRefused:
    case VoiceCommandKind::kOpenSite:
    case VoiceCommandKind::kSearch:
    case VoiceCommandKind::kNewTab:
    case VoiceCommandKind::kCloseTab:
    case VoiceCommandKind::kType:
    case VoiceCommandKind::kNewLine:
    case VoiceCommandKind::kDeleteLastWord:
    case VoiceCommandKind::kClearField:
    case VoiceCommandKind::kSubmit:
    case VoiceCommandKind::kYes:
    case VoiceCommandKind::kNo:
    case VoiceCommandKind::kUndo:
    case VoiceCommandKind::kCorrection:
    case VoiceCommandKind::kRememberRoutine:
    case VoiceCommandKind::kForgetRoutine:
    case VoiceCommandKind::kRunRoutine:
    case VoiceCommandKind::kForgetWord:
    case VoiceCommandKind::kForgetLearning:
    case VoiceCommandKind::kVoiceModeOn:
      return false;
    default:
      return true;
  }
}

}  // namespace

bool TakenOverOwnVoice(std::string_view text) {
  return TakenOverOwnVoice(text, Language::kEnglish);
}

bool OnlyTheWakePhrase(std::string_view text, Language language) {
  ParseContext context;
  context.language = language;
  return !FoldForComparison(text).empty() &&
         ParseVoiceCommand(text, context).heard.empty();
}

bool TakenOverOwnVoice(std::string_view text, Language language) {
  // As the browser reads a list: a bare number or description chooses.
  ParseContext list;
  list.expecting_choice = true;
  list.language = language;
  const VoiceCommand in_list = ParseVoiceCommand(text, list);
  if (!StaysWithWhatIsRead(in_list)) {
    return false;
  }
  // And as it reads anything else: the loop parses it in its own context,
  // where "open three" opens a site called "three". A bare number with no
  // list ("three") goes to the planner as nothing but that number.
  ParseContext own;
  own.language = language;
  const VoiceCommand plain = ParseVoiceCommand(text, own);
  if (plain.kind == VoiceCommandKind::kUnknown &&
      in_list.kind == VoiceCommandKind::kChoose &&
      in_list.choice.kind == ChoiceRef::Kind::kNumber &&
      in_list.verb == ChoiceVerb::kDefault) {
    return true;
  }
  return StaysWithWhatIsRead(plain);
}

ListeningSettings::ListeningSettings() = default;
ListeningSettings::~ListeningSettings() = default;
ListeningSettings::ListeningSettings(const ListeningSettings&) = default;
ListeningSettings& ListeningSettings::operator=(const ListeningSettings&) =
    default;
ListeningSettings::ListeningSettings(ListeningSettings&&) = default;
ListeningSettings& ListeningSettings::operator=(ListeningSettings&&) = default;

SessionActions::SessionActions() = default;
SessionActions::~SessionActions() = default;
SessionActions::SessionActions(const SessionActions&) = default;
SessionActions& SessionActions::operator=(const SessionActions&) = default;
SessionActions::SessionActions(SessionActions&&) = default;
SessionActions& SessionActions::operator=(SessionActions&&) = default;

VoiceSessionLogic::VoiceSessionLogic(ListeningSettings settings)
    : settings_(std::move(settings)) {}

VoiceSessionLogic::~VoiceSessionLogic() = default;

// ---- Lifecycle
// ---------------------------------------------------------------

SessionActions VoiceSessionLogic::Start(int64_t now_ms) {
  SessionActions actions;
  EndUtterance(&actions);
  running_ = true;
  ready_ = false;
  follow_up_until_ms_ = 0;
  follow_up_closed_ms_ = -1;
  wake_until_ms_ = 0;
  wake_earcon_at_ms_ = 0;
  waiting_earcon_at_ms_ = 0;
  return Finish(std::move(actions), now_ms);
}

SessionActions VoiceSessionLogic::Stop(int64_t now_ms) {
  SessionActions actions;
  if (!running_) {
    return actions;
  }
  EndUtterance(&actions);
  running_ = false;
  ready_ = false;
  follow_up_until_ms_ = 0;
  follow_up_closed_ms_ = -1;
  wake_until_ms_ = 0;
  wake_earcon_at_ms_ = 0;
  waiting_earcon_at_ms_ = 0;
  actions.earcon = EarconKind::kOff;
  return Finish(std::move(actions), now_ms);
}

SessionActions VoiceSessionLogic::OnHelperReady(int64_t now_ms) {
  SessionActions actions;
  if (!running_) {
    return actions;
  }
  if (!ready_) {
    ready_ = true;
    actions.earcon = settings_.always_listening ? EarconKind::kListening
                                                : EarconKind::kWaiting;
  }
  actions = Finish(std::move(actions), now_ms);
  // A fresh helper needs its CONFIG whatever it was before.
  actions.wake_required = wake_required_;
  return actions;
}

SessionActions VoiceSessionLogic::OnHelperLost(int64_t now_ms) {
  SessionActions actions;
  EndUtterance(&actions);
  ready_ = false;
  wake_until_ms_ = 0;
  wake_earcon_at_ms_ = 0;
  waiting_earcon_at_ms_ = 0;
  return Finish(std::move(actions), now_ms);
}

// ---- The helper's events ---------------------------------------------------

// static
int64_t VoiceSessionLogic::HeardAt(int64_t now_ms,
                                   std::optional<int64_t> lag_ms) {
  // Never later than it arrived; and audio the helper wasn't sent is taken
  // to be at least as late as the most an utterance heard cleanly may lag.
  const int64_t lag = lag_ms ? std::clamp<int64_t>(*lag_ms, 0, kMaxUtteranceMs)
                             : kMaxHeardLagMs;
  return now_ms - lag;
}

// static
bool VoiceSessionLogic::Lagged(std::optional<int64_t> lag_ms) {
  return !lag_ms || *lag_ms > kMaxHeardLagMs;
}

VoiceSessionLogic::Utterance VoiceSessionLogic::NewUtterance(
    bool woke,
    int64_t now_ms,
    std::optional<int64_t> lag_ms,
    bool at_start) const {
  const int64_t heard_ms = HeardAt(now_ms, lag_ms);
  Utterance utterance;
  utterance.woke = woke;
  // Begun just before the window ran out, as the helper places it.
  const bool begun_in_window =
      at_start && lag_ms && follow_up_closed_ms_ >= 0 &&
      activity_ == BrowserActivity::kIdle &&
      heard_ms <= follow_up_closed_ms_ + kWindowCloseGraceMs;
  utterance.open =
      !woke && (settings_.always_listening || FollowUpOpen(now_ms) ||
                BargeInOpen() || begun_in_window);
  // When its audio was captured: the browser's echo tail reaches back
  // that far for a helper that runs behind.
  utterance.during_speech = SpeakingOrTail(heard_ms);
  utterance.echo_cancelled = own_speech_in_reference_;
  utterance.started_ms = now_ms;
  utterance.begun_ms = heard_ms;
  utterance.lagged = Lagged(lag_ms);
  return utterance;
}

SessionActions VoiceSessionLogic::OnWake(int64_t now_ms,
                                         std::optional<int64_t> lag_ms) {
  SessionActions actions;
  if (!Listening()) {
    actions.wake_ignored = "not listening";
    return actions;
  }
  // The browser's own voice saying the wake phrase (a page's title read
  // out), or another source's speech (a page saying it, or voice mode's
  // own voice with no echo canceller): not the user.
  const int64_t heard_ms = HeardAt(now_ms, lag_ms);
  const bool own_words = SpeakingOrTail(heard_ms) && SaidWakePhrase(heard_ms);
  if (own_words || OtherSpeechSince(heard_ms)) {
    actions.reset_helper = true;
    actions.said_over_voice = OwnVoiceUnfilteredSince(heard_ms);
    actions.wake_ignored = own_words ? "voice mode's own words had it"
                           : actions.said_over_voice
                               ? "over voice mode's own voice"
                               : "another source was speaking";
    return Finish(std::move(actions), now_ms);
  }
  EndUtterance(&actions);
  utterance_ = NewUtterance(/*woke=*/true, now_ms, lag_ms);
  wake_until_ms_ = now_ms + kWakeTimeoutMs;
  if (activity_ == BrowserActivity::kSpeaking) {
    actions.stop_speech = true;
  }
  // The tone, if the user pauses (kWakeEarconDelayMs).
  wake_earcon_at_ms_ = now_ms + kWakeEarconDelayMs;
  wake_earcon_latest_ms_ = now_ms + kWakeEarconLatestMs;
  return Finish(std::move(actions), now_ms);
}

SessionActions VoiceSessionLogic::OnSpeechStart(int64_t now_ms,
                                                std::optional<int64_t> lag_ms) {
  SessionActions actions;
  if (!Listening()) {
    return actions;
  }
  if (utterance_ && utterance_->woke && !utterance_->started) {
    utterance_->started = true;
    utterance_->started_ms = now_ms;
    utterance_->lagged = utterance_->lagged || Lagged(lag_ms);
    wake_until_ms_ = 0;
    wake_earcon_at_ms_ = 0;
    return Finish(std::move(actions), now_ms);
  }
  EndUtterance(&actions);
  utterance_ = NewUtterance(/*woke=*/false, now_ms, lag_ms);
  utterance_->started = true;
  return Finish(std::move(actions), now_ms);
}

SessionActions VoiceSessionLogic::OnPartial(std::string_view text,
                                            int64_t now_ms,
                                            std::optional<int64_t> lag_ms) {
  SessionActions actions;
  if (!Listening()) {
    return actions;
  }
  if (!utterance_) {
    utterance_ = NewUtterance(/*woke=*/false, now_ms, lag_ms);
  }
  utterance_->lagged = utterance_->lagged || Lagged(lag_ms);
  if (!utterance_->started) {
    utterance_->started = true;
    utterance_->started_ms = now_ms;
    wake_until_ms_ = 0;
    wake_earcon_at_ms_ = 0;
  }
  if (Counts(*utterance_) && !text.empty()) {
    actions.partial = std::string(text);
    utterance_->partial_shown = true;
  }
  return Finish(std::move(actions), now_ms);
}

std::optional<int64_t> VoiceSessionLogic::UtteranceBegunMs() const {
  if (!utterance_) {
    return std::nullopt;
  }
  return utterance_->begun_ms;
}

void VoiceSessionLogic::NoteUtteranceLoudness(Loudness loudness) {
  if (utterance_) {
    utterance_->stood_out = loudness.loud && loudness.quiet_before;
  }
}

SessionActions VoiceSessionLogic::OnFinal(std::string_view text,
                                          double confidence,
                                          int64_t now_ms,
                                          std::optional<int64_t> lag_ms) {
  SessionActions actions;
  if (!Listening()) {
    return actions;
  }
  Utterance utterance =
      utterance_ ? *utterance_
                 : NewUtterance(/*woke=*/false, now_ms, lag_ms,
                                /*at_start=*/false);
  utterance.lagged = utterance.lagged || Lagged(lag_ms);
  EndUtterance(&actions);
  wake_until_ms_ = 0;
  wake_earcon_at_ms_ = 0;
  bool take = false;
  // Begun with the wake phrase over the browser's own voice.
  bool addressed = false;
  std::string_view reason;
  if (!text.empty()) {
    if (IsAlwaysHeardCommand(text, settings_.language)) {
      take = true;
    } else if (OtherSpeechSince(utterance.begun_ms)) {
      // Another source spoke while this was said: it may be what was heard.
      take = false;
      actions.said_over_voice = OwnVoiceUnfilteredSince(utterance.begun_ms);
      reason = actions.said_over_voice ? "over voice mode's own voice"
                                       : "another source was speaking";
    } else if (utterance.woke) {
      take = true;
    } else if (utterance.open && !utterance.during_speech) {
      take = !page_audible_ || confidence >= kMinConfidenceWithoutWake ||
             AddressedToVoiceMode(text, utterance.begun_ms);
      if (!take) {
        reason = "a tab's sound, and not heard surely";
      }
    } else if (!utterance.open) {
      reason = "no wake phrase";
    } else if (!utterance.echo_cancelled) {
      reason = "during voice mode's speech, with no echo canceller";
    } else if (utterance.open && utterance.echo_cancelled) {
      // Said over the browser's own voice: a command unless it is that
      // voice heard back. Begun with the wake phrase, it is the user
      // speaking to voice mode, as after a wake the helper heard; standing
      // out of what the echo canceller leaves of that voice (always in its
      // reference), it is someone speaking, not the voice.
      addressed = AddressedToVoiceMode(text, utterance.begun_ms);
      const bool sure = addressed || utterance.stood_out ||
                        confidence >= kMinConfidenceWithoutWake;
      const bool echo = sure && EchoOfBrowser(text, utterance.begun_ms);
      take = sure && !echo;
      if (!sure) {
        reason = "over voice mode's voice, and not heard surely";
      } else if (echo) {
        reason = "voice mode's voice heard back";
      }
      // What needs the browser quiet stops it and is asked for again.
      if (take && !addressed &&
          !TakenOverOwnVoice(text, settings_.language)) {
        take = false;
        actions.say_again = true;
        actions.stop_speech = activity_ == BrowserActivity::kSpeaking;
      }
      if (take && activity_ == BrowserActivity::kSpeaking) {
        actions.stop_speech = true;
      }
    }
  }
  if (take && (activity_ == BrowserActivity::kIdle || addressed) &&
      OnlyTheWakePhrase(text, settings_.language)) {
    // The wake phrase alone, and a pause ("Himalt." ... "Scroll down"),
    // heard in the follow-up window or over the browser's own voice (which
    // it stops): not a command, but a wake. The command after it counts,
    // as after any wake, and the helper keeps hearing everything until
    // then.
    utterance_ = NewUtterance(/*woke=*/true, now_ms, lag_ms);
    wake_until_ms_ = now_ms + kWakeTimeoutMs;
    wake_earcon_at_ms_ = now_ms;
    wake_earcon_latest_ms_ = now_ms + kWakeEarconLatestMs;
    follow_up_until_ms_ = std::max(follow_up_until_ms_, wake_until_ms_);
    return Finish(std::move(actions), now_ms);
  }
  if (!take && !actions.say_again && !reason.empty()) {
    actions.dropped = std::string(text);
    actions.dropped_reason = reason;
  }
  if (take) {
    actions.utterance = std::string(text);
    actions.heard_cleanly = HeardCleanly(utterance, confidence);
    actions.earcon = EarconKind::kThinking;
    follow_up_until_ms_ = 0;
    follow_up_closed_ms_ = -1;
    // Until the controller says what it does with it.
    if (activity_ == BrowserActivity::kSpeaking) {
      speaking_ended_ms_ = now_ms;
    }
    activity_ = BrowserActivity::kThinking;
  }
  return Finish(std::move(actions), now_ms);
}

SessionActions VoiceSessionLogic::OnSpeechEnd(int64_t now_ms,
                                              std::optional<int64_t> lag_ms) {
  SessionActions actions;
  if (!Listening()) {
    return actions;
  }
  if (utterance_ && utterance_->ended_ms < 0) {
    utterance_->ended_ms = now_ms;
    utterance_->lagged = utterance_->lagged || Lagged(lag_ms);
  }
  return Finish(std::move(actions), now_ms);
}

// ---- The browser -----------------------------------------------------------

SessionActions VoiceSessionLogic::OnBrowserActivity(BrowserActivity activity,
                                                    int64_t now_ms) {
  SessionActions actions;
  if (activity == activity_) {
    return Finish(std::move(actions), now_ms);
  }
  const BrowserActivity before = activity_;
  activity_ = activity;
  follow_up_closed_ms_ = -1;
  if (before == BrowserActivity::kSpeaking) {
    speaking_ended_ms_ = now_ms;
  }
  if (activity == BrowserActivity::kSpeaking && Listening()) {
    actions.earcon = EarconKind::kSpeaking;
  }
  if (activity == BrowserActivity::kIdle) {
    follow_up_until_ms_ =
        now_ms + (awaiting_answer_ ? kAnswerWindowMs : kFollowUpWindowMs);
  } else if (activity == BrowserActivity::kThinking &&
             before == BrowserActivity::kSpeaking && awaiting_answer_) {
    // It asked, and what it was doing waits for the answer (a page's
    // "Leave site?" holds up the "go back" that raised it).
    follow_up_until_ms_ = now_ms + kAnswerWindowMs;
  }
  return Finish(std::move(actions), now_ms);
}

SessionActions VoiceSessionLogic::OnBrowserSaid(std::string_view text,
                                                int64_t now_ms) {
  said_.emplace_back(now_ms, SaidWords(text));
  while (said_.size() > kMaxSpokenTexts ||
         (!said_.empty() && !SaidLately(said_.front().first, now_ms))) {
    said_.pop_front();
  }
  return Finish(SessionActions(), now_ms);
}

SessionActions VoiceSessionLogic::OnAwaitingAnswer(bool awaiting,
                                                   int64_t now_ms) {
  awaiting_answer_ = awaiting;
  if (awaiting && activity_ == BrowserActivity::kIdle &&
      follow_up_until_ms_ > now_ms) {
    follow_up_until_ms_ =
        std::max(follow_up_until_ms_, now_ms + kAnswerWindowMs);
  }
  return Finish(SessionActions(), now_ms);
}

SessionActions VoiceSessionLogic::OnPageAudible(bool audible, int64_t now_ms) {
  // When it goes quiet, the last time it was heard is now.
  if (audible || page_audible_) {
    page_audible_ms_ = now_ms;
  }
  page_audible_ = audible;
  return Finish(SessionActions(), now_ms);
}

SessionActions VoiceSessionLogic::OnOtherSpeech(bool speaking, int64_t now_ms) {
  // When it stops, the last time it was seen speaking is now.
  if (speaking || other_speaking_) {
    other_speech_ms_ = now_ms;
  }
  other_speaking_ = speaking;
  return Finish(SessionActions(), now_ms);
}

SessionActions VoiceSessionLogic::OnEchoCancellation(bool cancelled,
                                                     int64_t now_ms) {
  echo_cancelled_ = cancelled;
  return Finish(SessionActions(), now_ms);
}

SessionActions VoiceSessionLogic::OnOwnSpeechInReference(bool in_reference,
                                                         int64_t now_ms) {
  own_speech_in_reference_ = in_reference;
  return Finish(SessionActions(), now_ms);
}

SessionActions VoiceSessionLogic::OnOwnVoiceUnfiltered(bool playing,
                                                       int64_t now_ms) {
  // When it stops, the last time it was seen playing is now.
  if (playing || own_voice_unfiltered_) {
    own_voice_unfiltered_ms_ = now_ms;
  }
  own_voice_unfiltered_ = playing;
  return Finish(SessionActions(), now_ms);
}

SessionActions VoiceSessionLogic::OnSettingsChanged(ListeningSettings settings,
                                                    int64_t now_ms) {
  settings_ = std::move(settings);
  return Finish(SessionActions(), now_ms);
}

// ---- Timers ----------------------------------------------------------------

SessionActions VoiceSessionLogic::OnTick(int64_t now_ms) {
  SessionActions actions;
  if (!Listening()) {
    return Finish(std::move(actions), now_ms);
  }
  if (wake_earcon_at_ms_ > 0 && now_ms >= wake_earcon_at_ms_) {
    if (MicrophoneLoud(now_ms) && now_ms < wake_earcon_latest_ms_) {
      // Still speaking, ahead of the recognizer: the tone waits for the
      // pause.
      wake_earcon_at_ms_ = std::min(microphone_loud_ms_ + kQuietBeforeToneMs,
                                    wake_earcon_latest_ms_);
    } else {
      // The wake phrase, and the user paused: the tone says go ahead.
      wake_earcon_at_ms_ = 0;
      if (utterance_ && utterance_->woke && !utterance_->started &&
          !actions.earcon) {
        actions.earcon = EarconKind::kListening;
      }
    }
  }
  if (waiting_earcon_at_ms_ > 0 && now_ms >= waiting_earcon_at_ms_) {
    if (MicrophoneLoud(now_ms) && now_ms < waiting_earcon_latest_ms_) {
      // Still speaking: the tone waits for the pause.
      waiting_earcon_at_ms_ = std::min(microphone_loud_ms_ + kQuietBeforeToneMs,
                                       waiting_earcon_latest_ms_);
    } else {
      waiting_earcon_at_ms_ = 0;
      if (state_ == SessionState::kWaiting && !actions.earcon) {
        actions.earcon = EarconKind::kWaiting;
      }
    }
  }
  if (wake_until_ms_ > 0 && now_ms >= wake_until_ms_) {
    // The wake phrase, then nothing.
    wake_until_ms_ = 0;
    if (utterance_ && utterance_->woke && !utterance_->started) {
      EndUtterance(&actions);
      actions.reset_helper = true;
    }
  }
  if (utterance_ && utterance_->ended_ms >= 0 &&
      now_ms - utterance_->ended_ms >= kFinalGraceMs) {
    // Speech ended and no final came.
    EndUtterance(&actions);
  }
  if (utterance_ && utterance_->started &&
      now_ms - utterance_->started_ms >= kMaxUtteranceMs) {
    EndUtterance(&actions);
    actions.reset_helper = true;
  }
  if (follow_up_until_ms_ > 0 && now_ms >= follow_up_until_ms_ &&
      !(utterance_ && utterance_->open && utterance_->ended_ms < 0)) {
    follow_up_closed_ms_ = follow_up_until_ms_;
    follow_up_until_ms_ = 0;
  }
  return Finish(std::move(actions), now_ms);
}

std::optional<int64_t> VoiceSessionLogic::NextDeadline() const {
  std::optional<int64_t> next;
  auto consider = [&next](int64_t at) {
    if (!next || at < *next) {
      next = at;
    }
  };
  if (!Listening()) {
    return next;
  }
  if (wake_until_ms_ > 0) {
    consider(wake_until_ms_);
  }
  if (wake_earcon_at_ms_ > 0) {
    consider(wake_earcon_at_ms_);
  }
  if (waiting_earcon_at_ms_ > 0) {
    consider(waiting_earcon_at_ms_);
  }
  if (utterance_) {
    if (utterance_->ended_ms >= 0) {
      consider(utterance_->ended_ms + kFinalGraceMs);
    }
    if (utterance_->started) {
      consider(utterance_->started_ms + kMaxUtteranceMs);
    }
  }
  if (follow_up_until_ms_ > 0) {
    consider(follow_up_until_ms_);
  }
  return next;
}

// ---- Helpers ---------------------------------------------------------------

bool VoiceSessionLogic::MicrophoneLoud(int64_t now_ms) const {
  return microphone_loud_ms_ >= 0 &&
         microphone_loud_ms_ + kQuietBeforeToneMs > now_ms;
}

bool VoiceSessionLogic::FollowUpOpen(int64_t now_ms) const {
  if (follow_up_until_ms_ == 0) {
    return false;
  }
  // While it is busy, only an answer it waits for: its question holds up
  // what it was doing (OnBrowserActivity()).
  const bool answer_awaited =
      awaiting_answer_ && activity_ == BrowserActivity::kThinking;
  if (activity_ != BrowserActivity::kIdle && !answer_awaited) {
    return false;
  }
  // An utterance begun in the window keeps it open until it ends.
  return follow_up_until_ms_ > now_ms ||
         (utterance_ && utterance_->open && utterance_->ended_ms < 0);
}

bool VoiceSessionLogic::SpeakingOrTail(int64_t at_ms) const {
  return activity_ == BrowserActivity::kSpeaking ||
         (speaking_ended_ms_ >= 0 && at_ms - speaking_ended_ms_ < kEchoTailMs);
}

bool VoiceSessionLogic::OwnVoiceUnfilteredSince(int64_t since_ms) const {
  return own_voice_unfiltered_ ||
         (own_voice_unfiltered_ms_ >= 0 &&
          own_voice_unfiltered_ms_ + kOtherSpeechTailMs >= since_ms);
}

bool VoiceSessionLogic::OtherSpeechSince(int64_t since_ms) const {
  if (other_speaking_ || (other_speech_ms_ >= 0 &&
                          other_speech_ms_ + kOtherSpeechTailMs >= since_ms)) {
    return true;
  }
  // Voice mode's own voice with no echo canceller: its helper chooses what
  // the microphone hears ("Hey Molt, open ...").
  if (OwnVoiceUnfilteredSince(since_ms)) {
    return true;
  }
  // Without an echo canceller, a tab's sound is heard as it is: a video
  // that says "Hey Molt, ..." would be taken for the user.
  return !echo_cancelled_ &&
         (page_audible_ || (page_audible_ms_ >= 0 &&
                            page_audible_ms_ + kOtherSpeechTailMs >= since_ms));
}

bool VoiceSessionLogic::Counts(const Utterance& utterance) const {
  return utterance.woke || (utterance.open && (!utterance.during_speech ||
                                               utterance.echo_cancelled));
}

bool VoiceSessionLogic::HeardCleanly(const Utterance& utterance,
                                     double confidence) const {
  if (utterance.during_speech || activity_ == BrowserActivity::kSpeaking) {
    return false;
  }
  // A helper far behind the microphone may have heard the browser's last
  // words as if after them; one whose audio_ms doesn't fit what it was
  // sent can't be placed at all.
  if (utterance.lagged) {
    return false;
  }
  // The browser's last words, their echo and the helper's lag.
  if (speaking_ended_ms_ >= 0 &&
      utterance.begun_ms - speaking_ended_ms_ < kGestureQuietMs) {
    return false;
  }
  // Another source, or a tab's sound with no echo canceller.
  if (OtherSpeechSince(utterance.begun_ms)) {
    return false;
  }
  const bool audible =
      page_audible_ ||
      (page_audible_ms_ >= 0 &&
       page_audible_ms_ + kOtherSpeechTailMs >= utterance.begun_ms);
  return !audible || confidence >= kMinConfidenceWithoutWake;
}

bool VoiceSessionLogic::BargeInOpen() const {
  return own_speech_in_reference_ && activity_ == BrowserActivity::kSpeaking;
}

bool VoiceSessionLogic::EchoOfBrowser(std::string_view text,
                                      int64_t now_ms) const {
  const std::string folded = FoldForComparison(text.substr(0, kMaxSaidBytes));
  const std::vector<std::string_view> wanted = SplitWords(folded);
  if (wanted.empty()) {
    return false;
  }
  // What it said lately, in the order it said it.
  std::vector<std::string_view> said;
  for (const auto& [when, words] : said_) {
    if (!SaidLately(when, now_ms)) {
      continue;
    }
    const std::vector<std::string_view> these = SplitWords(words);
    said.insert(said.end(), these.begin(), these.end());
  }
  // One word: anywhere in it. More: in its order, as it would be heard
  // back. "Number two" is not "1: ..., 2: ... Say a number or a name."
  if (wanted.size() == 1) {
    return std::find(said.begin(), said.end(), wanted[0]) != said.end();
  }
  return SaysInOrder(said, wanted);
}

bool VoiceSessionLogic::SaidLately(int64_t noted_ms, int64_t at_ms) const {
  if (activity_ == BrowserActivity::kSpeaking) {
    return true;
  }
  return at_ms - std::max(noted_ms, speaking_ended_ms_) <= kSpokenMemoryMs;
}

bool VoiceSessionLogic::AddressedToVoiceMode(std::string_view text,
                                             int64_t heard_ms) const {
  if (SpeakingOrTail(heard_ms) && SaidWakePhrase(heard_ms)) {
    return false;
  }
  ParseContext context;
  context.language = settings_.language;
  for (const std::string& phrase : settings_.wake_phrases) {
    context.wake_phrase = phrase;
    if (BeginsWithWakePhrase(text, context)) {
      return true;
    }
  }
  return false;
}

bool VoiceSessionLogic::SaidWakePhrase(int64_t now_ms) const {
  // What the browser was saying when the phrase was heard: the last text it
  // began before then, the one before it (a phrase may run from one into
  // the next) and any begun since. A sound-alike said earlier in a long
  // reading doesn't keep the user's own wake phrase out for all of it.
  size_t from = 0;
  for (size_t i = 0; i < said_.size(); ++i) {
    if (said_[i].first <= now_ms) {
      from = i == 0 ? 0 : i - 1;
    }
  }
  std::string previous;
  for (size_t i = from; i < said_.size(); ++i) {
    const auto& [when, words] = said_[i];
    if (!SaidLately(when, now_ms)) {
      previous.clear();
      continue;
    }
    // Across the seam with the text before it.
    const std::string joined =
        previous.empty() ? words : previous + " " + words;
    for (const std::string& phrase : settings_.wake_phrases) {
      const std::string folded = FoldForComparison(phrase);
      if (HasWords(joined, folded) || SaysSoundAlike(joined, folded)) {
        return true;
      }
    }
    // The text's last word, for the seam with the next one.
    const size_t last_space = words.rfind(' ');
    previous =
        last_space == std::string::npos ? words : words.substr(last_space + 1);
  }
  return false;
}

SessionState VoiceSessionLogic::ComputeState(int64_t now_ms) const {
  if (!running_) {
    return SessionState::kOff;
  }
  if (!ready_) {
    return SessionState::kStarting;
  }
  if (activity_ == BrowserActivity::kSpeaking) {
    return SessionState::kSpeaking;
  }
  if (activity_ == BrowserActivity::kThinking) {
    return SessionState::kThinking;
  }
  // A command being spoken, or just spoken (its final may still come).
  if (wake_until_ms_ > 0 || (utterance_ && Counts(*utterance_))) {
    return SessionState::kListening;
  }
  if (settings_.always_listening || FollowUpOpen(now_ms)) {
    return SessionState::kListening;
  }
  return SessionState::kWaiting;
}

void VoiceSessionLogic::EndUtterance(SessionActions* actions) {
  if (utterance_ && utterance_->partial_shown) {
    actions->partial = std::string();
  }
  utterance_.reset();
}

SessionActions VoiceSessionLogic::Finish(SessionActions actions,
                                         int64_t now_ms) {
  const SessionState state = ComputeState(now_ms);
  if (state != state_) {
    waiting_earcon_at_ms_ = 0;
    if (state == SessionState::kWaiting && !actions.earcon &&
        (state_ == SessionState::kListening ||
         state_ == SessionState::kThinking ||
         state_ == SessionState::kSpeaking)) {
      if (MicrophoneLoud(now_ms)) {
        // Said as the window closed: the tone in the echo canceller's
        // reference takes the words under it (kQuietBeforeToneMs).
        waiting_earcon_at_ms_ = microphone_loud_ms_ + kQuietBeforeToneMs;
        waiting_earcon_latest_ms_ = now_ms + kWakeEarconLatestMs;
      } else {
        actions.earcon = EarconKind::kWaiting;
      }
    }
    state_ = state;
    actions.state = state;
  }
  const bool wake_required =
      !(settings_.always_listening || FollowUpOpen(now_ms) ||
        activity_ != BrowserActivity::kIdle);
  if (wake_required != wake_required_) {
    wake_required_ = wake_required;
    actions.wake_required = wake_required;
  }
  const bool duck = Listening() &&
                    (wake_until_ms_ > 0 || (utterance_ && Counts(*utterance_) &&
                                            utterance_->ended_ms < 0));
  if (duck != ducked_) {
    ducked_ = duck;
    actions.duck = duck;
  }
  return actions;
}

}  // namespace voice
}  // namespace molt_ai
