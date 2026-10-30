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
    }
    words.append(aloud);
  }
  return words;
}

// Whether the folded words |haystack| hold a sound-alike of the folded
// wake phrase |phrase|: a word that sounds like its last word, the name
// ("mold" and "moult" for "molt": one phonetic key), or one that begins
// with the name right after the phrase's word before it ("hey
// moltbrowser", "hay molts"). Only for names of four letters or more,
// whose keys are specific enough.
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
    // A sound-alike is about as long as the name ("mold", "mould").
    const bool near_length =
        word.size() + 1 >= name.size() && word.size() <= name.size() + 2;
    if (near_length && PhoneticKey(word) == key) {
      return true;
    }
    const bool after_first =
        !first.empty() && !previous.empty() &&
        (previous == first ||
         (!first_key.empty() && PhoneticKey(previous) == first_key));
    if (after_first && word.size() > name.size() &&
        word.substr(0, name.size()) == name) {
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
  switch (ParseVoiceCommand(text, ParseContext()).kind) {
    case VoiceCommandKind::kStop:
    case VoiceCommandKind::kStopSpeaking:
    case VoiceCommandKind::kCancel:
    case VoiceCommandKind::kVoiceModeOff:
      return true;
    default:
      return false;
  }
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
  wake_until_ms_ = 0;
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
  wake_until_ms_ = 0;
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
    std::optional<int64_t> lag_ms) const {
  const int64_t heard_ms = HeardAt(now_ms, lag_ms);
  Utterance utterance;
  utterance.woke = woke;
  utterance.open = !woke && (settings_.always_listening ||
                             FollowUpOpen(now_ms) || BargeInOpen());
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
    return actions;
  }
  // The browser's own voice saying the wake phrase (a page's title read
  // out), or another source's speech (a page saying it, or voice mode's
  // own voice with no echo canceller): not the user.
  const int64_t heard_ms = HeardAt(now_ms, lag_ms);
  if ((SpeakingOrTail(heard_ms) && SaidWakePhrase(heard_ms)) ||
      OtherSpeechSince(heard_ms)) {
    actions.reset_helper = true;
    actions.said_over_voice = OwnVoiceUnfilteredSince(heard_ms);
    return Finish(std::move(actions), now_ms);
  }
  EndUtterance(&actions);
  utterance_ = NewUtterance(/*woke=*/true, now_ms, lag_ms);
  wake_until_ms_ = now_ms + kWakeTimeoutMs;
  if (activity_ == BrowserActivity::kSpeaking) {
    actions.stop_speech = true;
  }
  actions.earcon = EarconKind::kListening;
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
  }
  if (Counts(*utterance_) && !text.empty()) {
    actions.partial = std::string(text);
    utterance_->partial_shown = true;
  }
  return Finish(std::move(actions), now_ms);
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
      utterance_ ? *utterance_ : NewUtterance(/*woke=*/false, now_ms, lag_ms);
  utterance.lagged = utterance.lagged || Lagged(lag_ms);
  EndUtterance(&actions);
  wake_until_ms_ = 0;
  bool take = false;
  if (!text.empty()) {
    if (IsAlwaysHeardCommand(text)) {
      take = true;
    } else if (OtherSpeechSince(utterance.begun_ms)) {
      // Another source spoke while this was said: it may be what was heard.
      take = false;
      actions.said_over_voice = OwnVoiceUnfilteredSince(utterance.begun_ms);
    } else if (utterance.woke) {
      take = true;
    } else if (utterance.open && !utterance.during_speech) {
      take = !page_audible_ || confidence >= kMinConfidenceWithoutWake;
    } else if (utterance.open && utterance.echo_cancelled) {
      // Said over the browser's own voice: a command unless it is that
      // voice heard back.
      take = confidence >= kMinConfidenceWithoutWake &&
             !EchoOfBrowser(text, utterance.begun_ms);
      if (take && activity_ == BrowserActivity::kSpeaking) {
        actions.stop_speech = true;
      }
    }
  }
  if (take) {
    actions.utterance = std::string(text);
    actions.heard_cleanly = HeardCleanly(utterance, confidence);
    actions.earcon = EarconKind::kThinking;
    follow_up_until_ms_ = 0;
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
  if (before == BrowserActivity::kSpeaking) {
    speaking_ended_ms_ = now_ms;
  }
  if (activity == BrowserActivity::kSpeaking && Listening()) {
    actions.earcon = EarconKind::kSpeaking;
  }
  if (activity == BrowserActivity::kIdle) {
    follow_up_until_ms_ =
        now_ms + (awaiting_answer_ ? kAnswerWindowMs : kFollowUpWindowMs);
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

bool VoiceSessionLogic::FollowUpOpen(int64_t now_ms) const {
  if (activity_ != BrowserActivity::kIdle || follow_up_until_ms_ == 0) {
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
  if (folded.empty()) {
    return false;
  }
  size_t start = 0;
  while (start < folded.size()) {
    size_t end = folded.find(' ', start);
    if (end == std::string::npos) {
      end = folded.size();
    }
    const std::string_view word =
        std::string_view(folded).substr(start, end - start);
    bool said = false;
    for (const auto& [when, words] : said_) {
      if (SaidLately(when, now_ms) && HasWords(words, word)) {
        said = true;
        break;
      }
    }
    if (!said) {
      return false;
    }
    start = end + 1;
  }
  return true;
}

bool VoiceSessionLogic::SaidLately(int64_t noted_ms, int64_t at_ms) const {
  if (activity_ == BrowserActivity::kSpeaking) {
    return true;
  }
  return at_ms - std::max(noted_ms, speaking_ended_ms_) <= kSpokenMemoryMs;
}

bool VoiceSessionLogic::SaidWakePhrase(int64_t now_ms) const {
  for (const auto& [when, words] : said_) {
    if (!SaidLately(when, now_ms)) {
      continue;
    }
    for (const std::string& phrase : settings_.wake_phrases) {
      const std::string folded = FoldForComparison(phrase);
      const size_t space = folded.rfind(' ');
      const std::string_view name =
          space == std::string::npos
              ? std::string_view(folded)
              : std::string_view(folded).substr(space + 1);
      if (HasWords(words, folded) || HasWords(words, name) ||
          SaysSoundAlike(words, folded)) {
        return true;
      }
    }
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
    if (state == SessionState::kWaiting && !actions.earcon &&
        (state_ == SessionState::kListening ||
         state_ == SessionState::kThinking ||
         state_ == SessionState::kSpeaking)) {
      actions.earcon = EarconKind::kWaiting;
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
