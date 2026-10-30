// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// When voice mode listens, and what it does with what it hears
// (docs/design/TOUCHLESS_BROWSING.md §5.1, §7): the listening states, the
// wake phrase, the follow-up window, barge-in, "stop", ducking and the
// earcons, as a pure state machine. voice_session.h feeds it the speech
// helper's events (voice_listener_protocol.h) and what the voice
// controller is doing, and carries out the actions it returns.
//
// States (§5.1): off; starting (the helper is loading); waiting (only the
// wake phrase counts); listening (a command is being spoken, or the
// browser listens without the wake phrase); thinking; speaking.
//
// What counts as a command (the final text goes to the voice loop as if
// typed):
//   - An utterance after the wake phrase ("Hey Molt, open YouTube").
//   - Without it: every utterance in always-listening mode, and in the
//     follow-up window: kFollowUpWindowMs after the browser has finished
//     answering, or kAnswerWindowMs while it waits for a yes or no or a
//     choice ("the fourth one"). While a page plays sound, such an
//     utterance also needs kMinConfidenceWithoutWake (§7.2).
//   - "Stop", "cancel", "stop talking" and "turn off voice mode" always
//     count: the helper hears everything (wake_required false) while the
//     browser thinks or speaks, so they work without the wake phrase.
//   - While the browser speaks (and kEchoTailMs after), nothing else
//     counts without the wake phrase. The platform's voices play outside
//     Chromium's audio service, so the echo canceller has no reference for
//     them and a recognizer transcribes the browser's own voice
//     (VOICE_BENCH.md §3.2): what it hears then may be the browser itself.
//     A wake phrase while the browser's own words held one is ignored too.
//   - While the speech engine speaks for someone else (a page's
//     speechSynthesis; OnOtherSpeech()), and kOtherSpeechTailMs after,
//     only the commands that always count do: no wake phrase, no command
//     after one, nothing in the follow-up window or in always-listening
//     mode. That voice plays outside Chromium's audio service too, and a
//     page chooses what it says ("Hey Molt, ...").
//   - Without an echo canceller (OnEchoCancellation(false): the microphone
//     has neither Chrome-wide echo cancellation nor the platform's), a
//     tab's sound reaches the recognizer as it is: while a tab plays
//     sound, and kOtherSpeechTailMs after, it is taken as another source's
//     speech, so only the commands that always count do.
//
// Barge-in: the wake phrase (and "stop") while the browser speaks stops its
// speech at once; the command follows.
//
// Ducking (§5.1): media is ducked while a command is being spoken (from
// the wake phrase or the start of an utterance that counts, to its end).
//
// Earcons: ready (waiting, or listening when always listening), wake
// (listening), a command taken (thinking), the browser starting to speak
// (speaking), back to waiting (the follow-up window closed or the wake
// phrase timed out), and off.
//
// Std-only, unit-tested; all times are a monotonic clock in milliseconds.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SESSION_LOGIC_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SESSION_LOGIC_H_

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace voice {

// How long the browser listens without the wake phrase after it answers,
// and while it waits for an answer to its own question.
inline constexpr int64_t kFollowUpWindowMs = 8000;
inline constexpr int64_t kAnswerWindowMs = 30000;
// After the wake phrase, how long the user has to start speaking.
inline constexpr int64_t kWakeTimeoutMs = 6000;
// After the browser stops speaking, how long its voice may still reach the
// microphone (the room, the recognizer's lag).
inline constexpr int64_t kEchoTailMs = 700;
// A final after the end of speech, and an utterance's longest.
inline constexpr int64_t kFinalGraceMs = 2000;
inline constexpr int64_t kMaxUtteranceMs = 30000;
// What the browser said, kept to recognize its own wake phrase.
inline constexpr int64_t kSpokenMemoryMs = 20000;
inline constexpr size_t kMaxSpokenTexts = 32;
// An utterance without the wake phrase while a page plays sound.
inline constexpr double kMinConfidenceWithoutWake = 0.5;
// After another source's speech was last seen, how long what is heard may
// still be it (the room, the spotter's and the recognizer's lag, and the
// time between two looks at the speech engine).
inline constexpr int64_t kOtherSpeechTailMs = 1000;

enum class SessionState : uint8_t {
  kOff,
  kStarting,
  kWaiting,
  kListening,
  kThinking,
  kSpeaking,
};

std::string_view SessionStateName(SessionState state);

// What the voice controller is doing (VoiceActivity, voice_controller.h).
enum class BrowserActivity : uint8_t {
  kIdle,
  kThinking,
  kSpeaking,
};

enum class EarconKind : uint8_t {
  kOff,
  kWaiting,
  kListening,
  kThinking,
  kSpeaking,
};

struct ListeningSettings {
  ListeningSettings();
  ~ListeningSettings();
  ListeningSettings(const ListeningSettings&);
  ListeningSettings& operator=(const ListeningSettings&);
  ListeningSettings(ListeningSettings&&);
  ListeningSettings& operator=(ListeningSettings&&);

  // Every utterance is a command (the helper's wake_required false).
  bool always_listening = false;
  // ReduceSpeechPhrase() forms ("hey molt").
  std::vector<std::string> wake_phrases;
  int vad_silence_ms = 500;
};

// What to do after an event. Only what changed is set.
struct SessionActions {
  SessionActions();
  ~SessionActions();
  SessionActions(const SessionActions&);
  SessionActions& operator=(const SessionActions&);
  SessionActions(SessionActions&&);
  SessionActions& operator=(SessionActions&&);

  // The voice loop gets this, as if typed.
  std::optional<std::string> utterance;
  // What is being said, live, for the caption; empty clears it.
  std::optional<std::string> partial;
  // Stop the browser's speech now (barge-in).
  bool stop_speech = false;
  // Drop the helper's current utterance (RESET).
  bool reset_helper = false;
  // The helper's wake_required changed: send CONFIG.
  std::optional<bool> wake_required;
  std::optional<EarconKind> earcon;
  // Duck (true) or restore (false) media.
  std::optional<bool> duck;
  // The state shown changed.
  std::optional<SessionState> state;
};

class VoiceSessionLogic {
 public:
  explicit VoiceSessionLogic(ListeningSettings settings);
  ~VoiceSessionLogic();
  VoiceSessionLogic(const VoiceSessionLogic&) = delete;
  VoiceSessionLogic& operator=(const VoiceSessionLogic&) = delete;

  // The session starts (the helper is being started) and stops.
  SessionActions Start(int64_t now_ms);
  SessionActions Stop(int64_t now_ms);
  // The helper is ready (its `ready` line), or went away; it is being
  // restarted.
  SessionActions OnHelperReady(int64_t now_ms);
  SessionActions OnHelperLost(int64_t now_ms);

  // The helper's events (text already through SpeechSafeText()).
  SessionActions OnWake(int64_t now_ms);
  SessionActions OnSpeechStart(int64_t now_ms);
  SessionActions OnPartial(std::string_view text, int64_t now_ms);
  SessionActions OnFinal(std::string_view text,
                         double confidence,
                         int64_t now_ms);
  SessionActions OnSpeechEnd(int64_t now_ms);

  // The browser: what the voice controller does, what it says, whether it
  // waits for an answer (a yes or no, or a choice), whether a tab plays
  // sound.
  SessionActions OnBrowserActivity(BrowserActivity activity, int64_t now_ms);
  SessionActions OnBrowserSaid(std::string_view text, int64_t now_ms);
  SessionActions OnAwaitingAnswer(bool awaiting, int64_t now_ms);
  SessionActions OnPageAudible(bool audible, int64_t now_ms);
  // Whether the speech engine is speaking for someone else (a page's
  // speechSynthesis, an extension): looked at often, and at each of the
  // helper's events.
  SessionActions OnOtherSpeech(bool speaking, int64_t now_ms);
  // Whether the microphone has an echo canceller whose reference holds what
  // tabs play (VoiceAudio's EchoCancellation other than kNone). Until told
  // otherwise, it does.
  SessionActions OnEchoCancellation(bool cancelled, int64_t now_ms);
  SessionActions OnSettingsChanged(ListeningSettings settings, int64_t now_ms);

  // Timers: call at NextDeadline() (or any time).
  SessionActions OnTick(int64_t now_ms);
  std::optional<int64_t> NextDeadline() const;

  SessionState state() const { return state_; }
  // What CONFIG says now.
  bool wake_required() const { return wake_required_; }
  const ListeningSettings& settings() const { return settings_; }
  bool running() const { return running_; }

 private:
  struct Utterance {
    // The wake phrase began it.
    bool woke = false;
    // It began in always-listening mode or the follow-up window.
    bool open = false;
    // It began while the browser spoke (or just after).
    bool during_speech = false;
    bool started = false;
    int64_t started_ms = 0;
    // Its first event (the wake phrase, or the start of speech).
    int64_t begun_ms = 0;
    int64_t ended_ms = -1;
    bool partial_shown = false;
  };

  bool Listening() const { return running_ && ready_; }
  bool FollowUpOpen(int64_t now_ms) const;
  bool SpeakingOrTail(int64_t now_ms) const;
  // Whether another source's speech was heard since |since_ms| (or
  // kOtherSpeechTailMs before): the speech engine speaking for someone
  // else, or, without an echo canceller, a tab playing sound.
  bool OtherSpeechSince(int64_t since_ms) const;
  bool Counts(const Utterance& utterance) const;
  // Whether what the browser said lately holds a wake phrase.
  bool SaidWakePhrase(int64_t now_ms) const;
  Utterance NewUtterance(bool woke, int64_t now_ms) const;
  SessionState ComputeState(int64_t now_ms) const;
  // Fills in what changed (state, wake_required, ducking) and the earcon
  // of a return to waiting.
  SessionActions Finish(SessionActions actions, int64_t now_ms);
  void EndUtterance(SessionActions* actions);

  ListeningSettings settings_;
  bool running_ = false;
  bool ready_ = false;
  BrowserActivity activity_ = BrowserActivity::kIdle;
  int64_t speaking_ended_ms_ = -1;
  // The follow-up window's end; 0 when it is closed.
  int64_t follow_up_until_ms_ = 0;
  bool awaiting_answer_ = false;
  bool page_audible_ = false;
  // When a tab was last seen playing sound; -1 never.
  int64_t page_audible_ms_ = -1;
  bool echo_cancelled_ = true;
  bool other_speaking_ = false;
  // When another source's speech was last seen; -1 never.
  int64_t other_speech_ms_ = -1;
  std::optional<Utterance> utterance_;
  // After the wake phrase, until speech starts.
  int64_t wake_until_ms_ = 0;
  // What the browser said: when, and its words folded.
  std::deque<std::pair<int64_t, std::string>> said_;

  SessionState state_ = SessionState::kOff;
  bool wake_required_ = true;
  bool ducked_ = false;
};

// Whether |text| is one of the commands that always count: stop, stop
// talking, cancel, turn off voice mode.
bool IsAlwaysHeardCommand(std::string_view text);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SESSION_LOGIC_H_
