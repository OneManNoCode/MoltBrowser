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
//     choice ("the fourth one"), also while what it was doing waits on
//     that answer (a page's "Leave site?" holds up the "go back" that
//     raised it). One begun just before the window ran out, as the
//     helper places it, counts too (kWindowCloseGraceMs). The wake
//     phrase alone there ("Himalt." and a pause) is a wake, not a command
//     (OnlyTheWakePhrase()): the command after it counts. While a page
//     plays sound, such an utterance also needs kMinConfidenceWithoutWake
//     (§7.2), unless it begins with the wake phrase
//     (AddressedToVoiceMode()).
//   - "Stop", "cancel", "stop talking" and "turn off voice mode" always
//     count: the helper hears everything (wake_required false) while the
//     browser thinks or speaks, so they work without the wake phrase.
//   - While the browser speaks with the platform's voices (and
//     kEchoTailMs after), nothing else counts without the wake phrase.
//     Those voices play outside Chromium's audio service, so the echo
//     canceller has no reference for them and a recognizer transcribes
//     the browser's own voice (VOICE_BENCH.md §3.2): what it hears then may
//     be the browser itself. A wake phrase while the browser's own words
//     held one (or a word that sounds like its name, "mold", or one that
//     begins with the name after "hey": "Hey Moltbrowser") is ignored too.
//   - While it speaks with its own voice (voice_speech.h: browser output,
//     in the echo canceller's reference; OnOwnSpeechInReference()), the
//     user can interrupt it with a command, no wake phrase needed: an
//     utterance begun then counts if it has kMinConfidenceWithoutWake or
//     stood out of what the microphone gave before it (as loud as speech
//     after a quiet moment, NoteUtteranceLoudness()), and isn't the
//     browser's own words heard back (its words in the order
//     the browser said them in the last kSpokenMemoryMs, at most one
//     skipped between two, as written or as its voice says them,
//     voice_said_aloud.h: "example dot com" for "example.com"; that is
//     what an echo the canceller missed would be. One word: anywhere in
//     it. "Number two" over "1: ..., 2: ... Say a number" is the user's;
//     so is one that stood out of a quiet stretch that was the browser
//     speaking, kEchoCancellerSettledMs into its speech: "three" over
//     "3: ...").
//     It stops the browser's speech. Only commands that stay with what the
//     browser is reading or doing are taken that way (TakenOverOwnVoice()):
//     one that reaches another site, types or sends text, closes a tab,
//     changes what voice mode learned or goes to the planner stops the
//     speech and is not run (SessionActions::say_again): the user says it
//     again once the browser is quiet. Audio the voice's helper chose
//     would have to survive the echo canceller to be heard at all; this
//     keeps what it could do then to the page being read.
//     An utterance begun with the wake phrase ("Himalt. Play the fourth
//     one": the helper's spotter missed it, and the recognizer is unsure
//     of the phrase's odd spelling) is the user speaking to voice mode, as
//     after a wake the helper heard (AddressedToVoiceMode()): it needs no
//     confidence and any command runs, and the phrase alone is a wake;
//     not when the browser's own words had a wake phrase then
//     (SaidWakePhrase(), as for the helper's wake), nor when it is those
//     words heard back.
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
//   - So is voice mode's own voice then (OnOwnVoiceUnfiltered()): its
//     helper chooses the audio the browser plays, which needn't be the
//     text it was given, so what the browser said can't tell its words
//     from the user's. While it plays with no echo canceller, and
//     kOtherSpeechTailMs after, nothing heard gives a command, the wake
//     phrase included: only the commands that always count ("stop"), and
//     the user says the command once the speech stopped
//     (SessionActions::said_over_voice).
//
// When it was said: each of the helper's events reports its place in the
// audio it was sent (audio_ms), which the listener maps back to when that
// audio was captured (HelperAudioClock). The event's lag behind the
// microphone moves the utterance's start earlier for the browser's echo
// tail, other speech and the voice gesture's quiet period; the helper says
// it, so it never moves it later, and a lag past kMaxHeardLagMs (or an
// audio_ms that doesn't fit what was sent) keeps it from being heard
// cleanly.
//
// The voice gesture (voice_gesture.h, TOUCHLESS_BROWSING.md §7.3): an
// utterance that counts is also marked heard_cleanly when it can stand in
// for a click: its audio began kGestureQuietMs or more after the browser
// last spoke (with either voice: the platform's isn't in the echo
// canceller's reference, and the helper reports the start of speech a
// little late), the browser didn't speak while it was said, no other source
// spoke since it began (nor, without an echo canceller, did a tab play
// sound or voice mode's own voice), with a tab playing sound it had
// kMinConfidenceWithoutWake, and every event of it came within
// kMaxHeardLagMs of its audio.
//
// Barge-in: the wake phrase (and "stop") while the browser speaks stops its
// speech at once; the command follows. With the browser's own voice, a
// command said over it does too (above).
//
// Ducking (§5.1): media is ducked while a command is being spoken (from
// the wake phrase or the start of an utterance that counts, to its end).
// A trial (ListeningSettings::duck_at_speech_onset) also ducks it from the
// microphone's speech onset while waiting for the wake phrase
// (OnMicrophoneOnset()).
//
// Earcons: ready (waiting, or listening when always listening), wake
// (listening), a command taken (thinking), the browser starting to speak
// (speaking), back to waiting (the follow-up window closed or the wake
// phrase timed out; at a pause in what the microphone hears, as the
// listening tone), and off.
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

#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

// How long the browser listens without the wake phrase after it answers,
// and while it waits for an answer to its own question.
inline constexpr int64_t kFollowUpWindowMs = 8000;
inline constexpr int64_t kAnswerWindowMs = 30000;
// An utterance whose speech the helper places up to this long after the
// window closed began in it: the helper's detector hears speech a moment
// in, and the helper was told of the close a moment late
// (SPEECH_HELPER_PROTOCOL.md section 2.2).
inline constexpr int64_t kWindowCloseGraceMs = 500;
// After the wake phrase, how long the user has to start speaking.
inline constexpr int64_t kWakeTimeoutMs = 6000;
// After the wake phrase, the listening tone waits this long, and doesn't
// play once the command has started: said in the same breath ("Hey Molt,
// click ..."), a tone from the speakers over its first words reaches the
// microphone, and the echo canceller takes those words with it.
inline constexpr int64_t kWakeEarconDelayMs = 700;
// And it waits for a pause: the microphone quiet this long
// (NoteMicrophoneLoud()). The helper says the command started only once
// the recognizer wrote one of its words, often more than
// kWakeEarconDelayMs after the phrase; a tone over the command's last
// words takes them with it as well (the spoken suite's run with the
// speakers' sound in the microphone heard "open Wikipedia" as "open
// wikip"). It never waits past kWakeEarconLatestMs after the phrase. The
// tone back to waiting does the same: "Hey Molt" said as the follow-up
// window closed lost its "Molt" under it in the file run.
inline constexpr int64_t kQuietBeforeToneMs = 300;
inline constexpr int64_t kWakeEarconLatestMs = 2500;
// After the browser stops speaking, how long its voice may still reach the
// microphone (the room, the recognizer's lag).
inline constexpr int64_t kEchoTailMs = 700;
// A final after the end of speech, and an utterance's longest.
inline constexpr int64_t kFinalGraceMs = 2000;
inline constexpr int64_t kMaxUtteranceMs = 30000;
// What the browser said, kept to recognize its own wake phrase: for as
// long as it speaks, and this long after its speech ended (or after it was
// noted, if later).
inline constexpr int64_t kSpokenMemoryMs = 20000;
inline constexpr size_t kMaxSpokenTexts = 32;
// An utterance without the wake phrase while a page plays sound, or over
// the browser's own voice.
inline constexpr double kMinConfidenceWithoutWake = 0.5;
// Over the browser's own voice, also one that stood out
// (NoteUtteranceLoudness()): as loud as speech while it was said, from
// kLoudnessOnsetMarginMs before the helper placed its start, where the
// microphone was quiet in the kQuietBeforeUtteranceMs before that. The
// recognizer's confidence hardly tells a short word heard rightly from one
// heard wrongly (0.23 to 0.70 for "okay", "three", "start over" said
// cleanly), and what the echo canceller leaves of that voice is 20 dB and
// more below someone speaking (-50 dBFS or less against -8 to -14 in the
// spoken suite with the speakers' sound in the microphone).
inline constexpr int64_t kLoudnessOnsetMarginMs = 400;
inline constexpr int64_t kQuietBeforeUtteranceMs = 1500;
// One that stood out of a quiet stretch which was itself the browser
// speaking, begun at least this long before it (the echo canceller has
// settled on the voice by then), is the user even in words the browser
// also said: the canceller was taking that voice out right before, so what
// is loud now isn't it ("three" over "3: ..." as the results are read).
inline constexpr int64_t kEchoCancellerSettledMs = 2000;
// After another source's speech was last seen, how long what is heard may
// still be it (the room, the spotter's and the recognizer's lag, and the
// time between two looks at the speech engine).
inline constexpr int64_t kOtherSpeechTailMs = 1000;
// An utterance heard cleanly (the voice gesture) began at least this long
// after the browser last spoke.
inline constexpr int64_t kGestureQuietMs = 1200;
// The most an event of an utterance heard cleanly may lag behind its audio
// (it arrived this long after that audio was captured). The helper is fed
// as the microphone captures and normally answers within a few hundred
// milliseconds; one further behind may have heard the browser's last words
// as if they came after them.
inline constexpr int64_t kMaxHeardLagMs = 1000;
// The trial's duck from the microphone's speech onset, while voice mode
// waits for the wake phrase with a tab playing sound, lasts this long
// unless the wake phrase comes (then it is the command's duck, as
// before). With the speakers heard in the microphone, the echo canceller
// working over a playing video takes the start of "Hey Molt" ("He
// looked", "you mold" in the spoken suite), and the duck from the wake
// phrase comes too late for it.
inline constexpr int64_t kOnsetDuckMs = 2500;

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

std::string_view EarconKindName(EarconKind kind);

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
  // The language the user speaks to voice mode in: what is said over its
  // own voice is parsed in it (TakenOverOwnVoice()).
  Language language = Language::kEnglish;
  // A trial, off by default: duck media at the microphone's speech onset
  // while waiting for the wake phrase (kOnsetDuckMs). A page playing sound
  // could notice that duck, so it would tell the page when someone near
  // the microphone starts speaking, not only when the wake phrase is said.
  bool duck_at_speech_onset = false;
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
  // What the helper heard that the session set aside, and why (for the
  // spoken test kit's log: voice_test_log.h); nothing for what is taken, or
  // asked for again.
  std::optional<std::string> dropped;
  std::string_view dropped_reason;
  // A wake from the helper set aside (OnWake()), and why; empty when it
  // was taken. For the test log.
  std::string_view wake_ignored;
  // |utterance| was heard cleanly enough for the voice gesture (above).
  bool heard_cleanly = false;
  // |utterance| began with the wake phrase as the helper's detector heard
  // it, or followed the wake phrase said alone ("Himalt." and a pause): not
  // one heard in the follow-up window or while always listening. The
  // recognizer may have written the wake phrase as anything
  // (ParseContext::wake_heard).
  bool woke = false;
  // Something said while voice mode's own voice played with no echo
  // canceller was dropped (OnOwnVoiceUnfiltered()): the caption tells the
  // user to say stop first, or wait.
  bool said_over_voice = false;
  // A command said over voice mode's own voice that needs the browser to be
  // quiet (TakenOverOwnVoice()) was set aside: the speech stops, and the
  // browser asks for it again.
  bool say_again = false;
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

  // The helper's events (text already through SpeechSafeText()). |lag_ms|
  // is how far the event is behind the microphone: |now_ms| less when the
  // audio at its audio_ms was captured (HelperAudioClock::LagMs()), or
  // nullopt when its audio_ms doesn't fit the audio the helper was sent.
  // The helper says it, so it only makes decisions stricter (above).
  SessionActions OnWake(int64_t now_ms, std::optional<int64_t> lag_ms = 0);
  SessionActions OnSpeechStart(int64_t now_ms,
                               std::optional<int64_t> lag_ms = 0);
  SessionActions OnPartial(std::string_view text,
                           int64_t now_ms,
                           std::optional<int64_t> lag_ms = 0);
  SessionActions OnFinal(std::string_view text,
                         double confidence,
                         int64_t now_ms,
                         std::optional<int64_t> lag_ms = 0);
  SessionActions OnSpeechEnd(int64_t now_ms, std::optional<int64_t> lag_ms = 0);

  // The browser: what the voice controller does, what it says, whether it
  // waits for an answer (a yes or no, or a choice), whether a tab plays
  // sound.
  SessionActions OnBrowserActivity(BrowserActivity activity, int64_t now_ms);
  SessionActions OnBrowserSaid(std::string_view text, int64_t now_ms);
  SessionActions OnAwaitingAnswer(bool awaiting, int64_t now_ms);
  SessionActions OnPageAudible(bool audible, int64_t now_ms);
  // The microphone gave something as loud as speech after a quiet stretch,
  // after the browser's processing (VoiceAudio): with the trial
  // (duck_at_speech_onset), media is ducked for kOnsetDuckMs when voice
  // mode waits for the wake phrase and a tab plays sound.
  SessionActions OnMicrophoneOnset(int64_t now_ms);
  // When the microphone last gave something as loud as speech, after the
  // browser's processing (VoiceAudio::LastLoud()); -1 never. Noted before
  // each OnTick(), for the listening tone.
  void NoteMicrophoneLoud(int64_t loud_ms) { microphone_loud_ms_ = loud_ms; }
  // When the audio of the utterance being heard begins (its begun_ms), or
  // nullopt with none: what the session looks up the microphone's levels
  // for, before OnFinal().
  std::optional<int64_t> UtteranceBegunMs() const;
  // How the microphone sounded for that utterance, after the browser's
  // processing (VoiceAudio's level history): as loud as speech while it
  // was said, and quiet just before (kQuietBeforeUtteranceMs). Unknown is
  // neither.
  struct Loudness {
    bool loud = false;
    bool quiet_before = false;
  };
  void NoteUtteranceLoudness(Loudness loudness);
  // Whether the speech engine is speaking for someone else (a page's
  // speechSynthesis, an extension): looked at often, and at each of the
  // helper's events.
  SessionActions OnOtherSpeech(bool speaking, int64_t now_ms);
  // Whether the microphone has an echo canceller whose reference holds what
  // tabs play (VoiceAudio's EchoCancellation other than kNone). Until told
  // otherwise, it does.
  SessionActions OnEchoCancellation(bool cancelled, int64_t now_ms);
  // Whether the browser's speech is in the echo canceller's reference: its
  // own voice speaks and the microphone has echo cancellation.
  SessionActions OnOwnSpeechInReference(bool in_reference, int64_t now_ms);
  // Whether voice mode's own voice (the bundled one, voice_speech_bundled.h)
  // plays while the microphone has no echo canceller: its audio, which its
  // helper chooses, reaches the recognizer as it is. Looked at often, and
  // at each of the helper's events.
  SessionActions OnOwnVoiceUnfiltered(bool playing, int64_t now_ms);
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
    // And that speech was in the echo canceller's reference.
    bool echo_cancelled = false;
    bool started = false;
    int64_t started_ms = 0;
    // When the audio of its first event (the wake phrase, or the start of
    // speech) was captured, as far as the helper's audio_ms tells: never
    // later than the event arrived.
    int64_t begun_ms = 0;
    // One of its events lagged past kMaxHeardLagMs, or reported audio that
    // doesn't fit what the helper was sent.
    bool lagged = false;
    // It stood out of what the microphone gave before it
    // (NoteUtteranceLoudness()).
    bool stood_out = false;
    int64_t ended_ms = -1;
    bool partial_shown = false;
  };

  bool Listening() const { return running_ && ready_; }
  bool FollowUpOpen(int64_t now_ms) const;
  // The microphone heard something within kQuietBeforeToneMs: a tone now
  // would go over it.
  bool MicrophoneLoud(int64_t now_ms) const;
  // Whether the browser spoke at |at_ms| or within kEchoTailMs before
  // (looked at now: it counts if the browser speaks now).
  bool SpeakingOrTail(int64_t at_ms) const;
  // Whether another source's speech was heard since |since_ms| (or
  // kOtherSpeechTailMs before): the speech engine speaking for someone
  // else, or, without an echo canceller, a tab playing sound or voice
  // mode's own voice.
  bool OtherSpeechSince(int64_t since_ms) const;
  // The last of those: voice mode's own voice with no echo canceller.
  bool OwnVoiceUnfilteredSince(int64_t since_ms) const;
  bool Counts(const Utterance& utterance) const;
  // Whether what the browser said lately holds a wake phrase.
  bool SaidWakePhrase(int64_t now_ms) const;
  // Whether |text|, heard from |heard_ms|, begins with one of the wake
  // phrases (BeginsWithWakePhrase(): "Himalt. Play the fourth one", the
  // helper's spotter having missed it), and the browser's own words didn't
  // have one then (SaidWakePhrase(), as for a wake the helper heard).
  bool AddressedToVoiceMode(std::string_view text, int64_t heard_ms) const;
  // Whether every word of |text| is among what the browser said lately:
  // its own voice heard back.
  bool EchoOfBrowser(std::string_view text, int64_t now_ms) const;
  // Whether |utterance| stood out of a quiet stretch that was the browser
  // speaking, kEchoCancellerSettledMs and more into its speech: someone
  // speaking, not its voice heard back.
  bool StoodOutOfCancelledVoice(const Utterance& utterance) const;
  // Whether what the browser noted at |noted_ms| may still be heard at
  // |at_ms|: while it speaks (a long answer, or reading aloud, can last far
  // longer than kSpokenMemoryMs), and kSpokenMemoryMs after the later of
  // its noting and the end of its speech.
  bool SaidLately(int64_t noted_ms, int64_t at_ms) const;
  // A command may interrupt the browser's own voice now.
  bool BargeInOpen() const;
  // Whether |utterance|, taken with |confidence|, was heard cleanly (the
  // voice gesture).
  bool HeardCleanly(const Utterance& utterance, double confidence) const;
  // When the audio of an event that came at |now_ms| with |lag_ms| was
  // captured: |now_ms| less the lag (clamped to 0..kMaxUtteranceMs, and
  // kMaxHeardLagMs when it doesn't fit).
  static int64_t HeardAt(int64_t now_ms, std::optional<int64_t> lag_ms);
  static bool Lagged(std::optional<int64_t> lag_ms);
  // |at_start|: the event marks where its speech began (speech_start, a
  // first partial), not where it ended (a final with nothing before it).
  Utterance NewUtterance(bool woke,
                         int64_t now_ms,
                         std::optional<int64_t> lag_ms,
                         bool at_start = true) const;
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
  // When it last began speaking; -1 never.
  int64_t speaking_began_ms_ = -1;
  // The follow-up window's end; 0 when it is closed.
  int64_t follow_up_until_ms_ = 0;
  // When it last ran out with nothing said in it (kWindowCloseGraceMs);
  // -1 if it didn't, or something happened since.
  int64_t follow_up_closed_ms_ = -1;
  bool awaiting_answer_ = false;
  bool page_audible_ = false;
  // Until when the trial's duck from a speech onset lasts; 0 none.
  int64_t onset_duck_until_ms_ = 0;
  // When a tab was last seen playing sound; -1 never.
  int64_t page_audible_ms_ = -1;
  bool echo_cancelled_ = true;
  bool other_speaking_ = false;
  bool own_speech_in_reference_ = false;
  // When another source's speech was last seen; -1 never.
  int64_t other_speech_ms_ = -1;
  // Voice mode's own voice plays with no echo canceller; when it last
  // did (-1 never).
  bool own_voice_unfiltered_ = false;
  int64_t own_voice_unfiltered_ms_ = -1;
  std::optional<Utterance> utterance_;
  // After the wake phrase, until speech starts.
  int64_t wake_until_ms_ = 0;
  // When the listening tone plays after a wake, unless the command started
  // first (0: none due), and the latest it may (kWakeEarconLatestMs).
  int64_t wake_earcon_at_ms_ = 0;
  int64_t wake_earcon_latest_ms_ = 0;
  // When the tone back to waiting plays, if the microphone wasn't quiet
  // when it was due (0: none due), and the latest it may
  // (kWakeEarconLatestMs).
  int64_t waiting_earcon_at_ms_ = 0;
  int64_t waiting_earcon_latest_ms_ = 0;
  int64_t microphone_loud_ms_ = -1;
  // What the browser said: when, and its words folded.
  std::deque<std::pair<int64_t, std::string>> said_;

  SessionState state_ = SessionState::kOff;
  bool wake_required_ = true;
  bool ducked_ = false;
};

// Whether |text| is one of the commands that always count: stop, stop
// talking, cancel, turn off voice mode. Parsed in |language| (English
// without it), as the loop parses it ("para", "cancela").
bool IsAlwaysHeardCommand(std::string_view text);
bool IsAlwaysHeardCommand(std::string_view text, Language language);

// Whether |text|, said over voice mode's own voice without the wake phrase
// (barge-in), may run before the voice has stopped: everything but what
// reaches another site (open, search, a new tab), types or sends text,
// closes a tab, changes what voice mode learned (routines, words), turns
// voice mode on, or goes to the planner (what the grammar doesn't know).
// The loop parses it later in its own context, so it must stay so whether
// a list is being read or not ("open three" chooses from a list, and opens
// a site called "three" without one); a bare number is let through.
// Parsed in |language|, as the loop parses it ("el tercero", "abre tres").
bool TakenOverOwnVoice(std::string_view text);
// Whether |text| is a wake phrase and nothing else, as the grammar strips
// it ("Hey Molt.", "Himalt", "him old"): heard on its own in the follow-up
// window, it is a wake, and the command follows.
bool OnlyTheWakePhrase(std::string_view text, Language language);
bool TakenOverOwnVoice(std::string_view text, Language language);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SESSION_LOGIC_H_
