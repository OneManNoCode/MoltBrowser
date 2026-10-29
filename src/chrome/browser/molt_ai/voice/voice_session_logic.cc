// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_session_logic.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"

namespace molt_ai {
namespace voice {

namespace {

// What the browser said is folded from at most this many bytes.
constexpr size_t kMaxSaidBytes = 4096;

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

VoiceSessionLogic::Utterance VoiceSessionLogic::NewUtterance(
    bool woke,
    int64_t now_ms) const {
  Utterance utterance;
  utterance.woke = woke;
  utterance.open =
      !woke && (settings_.always_listening || FollowUpOpen(now_ms));
  utterance.during_speech = SpeakingOrTail(now_ms);
  utterance.started_ms = now_ms;
  utterance.begun_ms = now_ms;
  return utterance;
}

SessionActions VoiceSessionLogic::OnWake(int64_t now_ms) {
  SessionActions actions;
  if (!Listening()) {
    return actions;
  }
  // The browser's own voice saying the wake phrase (a page's title read
  // out), or another source's speech (a page saying it): not the user.
  if ((SpeakingOrTail(now_ms) && SaidWakePhrase(now_ms)) ||
      OtherSpeechSince(now_ms)) {
    actions.reset_helper = true;
    return Finish(std::move(actions), now_ms);
  }
  EndUtterance(&actions);
  utterance_ = NewUtterance(/*woke=*/true, now_ms);
  wake_until_ms_ = now_ms + kWakeTimeoutMs;
  if (activity_ == BrowserActivity::kSpeaking) {
    actions.stop_speech = true;
  }
  actions.earcon = EarconKind::kListening;
  return Finish(std::move(actions), now_ms);
}

SessionActions VoiceSessionLogic::OnSpeechStart(int64_t now_ms) {
  SessionActions actions;
  if (!Listening()) {
    return actions;
  }
  if (utterance_ && utterance_->woke && !utterance_->started) {
    utterance_->started = true;
    utterance_->started_ms = now_ms;
    wake_until_ms_ = 0;
    return Finish(std::move(actions), now_ms);
  }
  EndUtterance(&actions);
  utterance_ = NewUtterance(/*woke=*/false, now_ms);
  utterance_->started = true;
  return Finish(std::move(actions), now_ms);
}

SessionActions VoiceSessionLogic::OnPartial(std::string_view text,
                                            int64_t now_ms) {
  SessionActions actions;
  if (!Listening()) {
    return actions;
  }
  if (!utterance_) {
    utterance_ = NewUtterance(/*woke=*/false, now_ms);
  }
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
                                          int64_t now_ms) {
  SessionActions actions;
  if (!Listening()) {
    return actions;
  }
  const Utterance utterance =
      utterance_ ? *utterance_ : NewUtterance(/*woke=*/false, now_ms);
  EndUtterance(&actions);
  wake_until_ms_ = 0;
  bool take = false;
  if (!text.empty()) {
    if (IsAlwaysHeardCommand(text)) {
      take = true;
    } else if (OtherSpeechSince(utterance.begun_ms)) {
      // Another source spoke while this was said: it may be what was heard.
      take = false;
    } else if (utterance.woke) {
      take = true;
    } else if (utterance.open && !utterance.during_speech) {
      take = !page_audible_ || confidence >= kMinConfidenceWithoutWake;
    }
  }
  if (take) {
    actions.utterance = std::string(text);
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

SessionActions VoiceSessionLogic::OnSpeechEnd(int64_t now_ms) {
  SessionActions actions;
  if (!Listening()) {
    return actions;
  }
  if (utterance_ && utterance_->ended_ms < 0) {
    utterance_->ended_ms = now_ms;
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
  said_.emplace_back(now_ms, FoldForComparison(text.substr(0, kMaxSaidBytes)));
  while (said_.size() > kMaxSpokenTexts ||
         (!said_.empty() && now_ms - said_.front().first > kSpokenMemoryMs)) {
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

bool VoiceSessionLogic::SpeakingOrTail(int64_t now_ms) const {
  return activity_ == BrowserActivity::kSpeaking ||
         (speaking_ended_ms_ >= 0 && now_ms - speaking_ended_ms_ < kEchoTailMs);
}

bool VoiceSessionLogic::OtherSpeechSince(int64_t since_ms) const {
  return other_speaking_ || (other_speech_ms_ >= 0 &&
                             other_speech_ms_ + kOtherSpeechTailMs >= since_ms);
}

bool VoiceSessionLogic::Counts(const Utterance& utterance) const {
  return utterance.woke || (utterance.open && !utterance.during_speech);
}

bool VoiceSessionLogic::SaidWakePhrase(int64_t now_ms) const {
  for (const auto& [when, words] : said_) {
    if (now_ms - when > kSpokenMemoryMs) {
      continue;
    }
    for (const std::string& phrase : settings_.wake_phrases) {
      const std::string folded = FoldForComparison(phrase);
      const size_t space = folded.rfind(' ');
      const std::string_view name =
          space == std::string::npos
              ? std::string_view(folded)
              : std::string_view(folded).substr(space + 1);
      if (HasWords(words, folded) || HasWords(words, name)) {
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
