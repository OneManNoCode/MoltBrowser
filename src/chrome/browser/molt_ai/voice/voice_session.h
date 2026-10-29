// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode listening in a browser window (docs/design/
// TOUCHLESS_BROWSING.md §5.1, §6.1, §7; Phase 1): the microphone
// (voice_audio.h), the speech helper (voice_listener.h), the rules for what
// counts (voice_session_logic.h) and the window's voice loop
// (voice_controller.h), joined.
//
//   "Hey Molt, open YouTube"
//     microphone, echo-cancelled -> helper: wake, speech_start,
//     partial "open you" (the caption), final "open YouTube"
//     -> VoiceSessionLogic: it counts -> VoiceController::HandleUtterance()
//
//   - States, earcons, the follow-up window, always-listening mode,
//     barge-in, "stop", ducking: VoiceSessionLogic decides; this carries it
//     out (earcons through VoiceEarcons, ducking through every tab's media
//     session, barge-in through VoiceController::StopSpeaking()).
//   - The caption bar shows the state, and what is being said as it is said
//     (VoiceController::SetListening()).
//   - Hotwords: the page map each utterance was planned on gives the
//     recognizer the titles and channels on screen
//     (HotwordsFromPageMap()); a changed list goes out in CONFIG.
//   - One window listens at a time: starting another stops the first (one
//     microphone, one helper).
//   - MoltNet windows: never started, and the microphone is never opened
//     for them (§7.4). The voice loop says why.
//   - Problems are said aloud and shown: no microphone permission, no
//     microphone, the helper missing, or failing again and again.
//   - Started by: the toolbar's Voice button, "turn on voice mode" typed in
//     the Agent mode panel, the settings switch, and with the browser when
//     the setting says so (OnWindowReady()). Everything behind
//     chrome://flags/#molt-voice-mode.
//
// UI thread. Owned by its window (created on first use, deleted when the
// window closes).

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SESSION_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SESSION_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/voice/voice_audio.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"
#include "chrome/browser/molt_ai/voice/voice_listener.h"
#include "chrome/browser/molt_ai/voice/voice_session_logic.h"
#include "chrome/browser/molt_ai/voice/voice_session_settings.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

class BrowserWindowInterface;

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {
namespace voice {

class VoiceSession : public VoiceController::Observer,
                     public VoiceListener::Delegate,
                     public VoiceAudio::Delegate {
 public:
  DECLARE_USER_DATA(VoiceSession);

  class Observer : public base::CheckedObserver {
   public:
    // Listening started or stopped, or its state changed.
    virtual void OnVoiceSessionChanged(VoiceSession* session) {}
    // The window is closing: stop observing |session|.
    virtual void OnVoiceSessionDestroying(VoiceSession* session) {}
  };

  // The window's session, created on first use; null for a window that
  // isn't a normal tabbed one, or while #molt-voice-mode is off.
  static VoiceSession* GetOrCreateForBrowser(BrowserWindowInterface* browser);
  static VoiceSession* FromBrowser(BrowserWindowInterface* browser);

  // A normal window is ready (its toolbar was made): the first one of this
  // run of the browser starts listening when the settings say so.
  static void OnWindowReady(BrowserWindowInterface* browser);

  // The settings changed (molt://ai-settings): every session applies them.
  static void OnSettingsChanged(const VoiceModeSettings& settings);

  // The window that listens, if any.
  static VoiceSession* Listening();

  VoiceSession(const VoiceSession&) = delete;
  VoiceSession& operator=(const VoiceSession&) = delete;
  ~VoiceSession() override;

  // Starts listening in this window. False, after saying why, in a MoltNet
  // window or while #molt-voice-mode is off.
  bool Start();
  // Stops listening (the microphone and the helper) and voice mode in the
  // window (the caption bar goes); typed commands start it again.
  void Stop();
  void Toggle();
  // Listening, or getting ready to.
  bool listening() const { return logic_.running(); }
  SessionState state() const { return logic_.state(); }

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // VoiceController::Observer:
  void OnVoiceCaption(VoiceActivity activity,
                      const std::string& heard,
                      const std::string& status) override;
  void OnVoiceSpeech(const std::string& text) override;
  void OnVoiceModeRequested(bool on) override;
  void OnVoicePageMapped(const PageMap& map) override;
  void OnVoiceControllerDestroying(VoiceController* controller) override;

  // VoiceListener::Delegate:
  void OnListenerReady(const HelperEvent& ready) override;
  void OnListenerEvent(const HelperEvent& event) override;
  void OnListenerLost(VoiceListener::StopReason reason,
                      const std::string& why,
                      bool restarting) override;
  void OnListenerGaveUp(const std::string& why) override;

  // VoiceAudio::Delegate:
  void OnMicrophoneStarted(EchoCancellation echo) override;
  void OnMicrophoneProblem(VoiceAudio::Problem problem) override;

  // Tests: the listener (null until the helper was found).
  VoiceListener* listener_for_testing() { return listener_.get(); }

 private:
  explicit VoiceSession(BrowserWindowInterface* browser);

  void OnBrowserDidClose(BrowserWindowInterface* browser);
  bool IsMoltNet() const;
  void OnSettingsRead(const VoiceModeSettings& settings);
  void OnHelperFound(std::optional<VoiceListener::Paths> paths,
                     const std::string& why);
  // Stops listening and says |message| (a problem); voice mode stays on
  // to show it.
  void Fail(const std::string& message);
  // Stops listening; and voice mode too with |stop_voice_mode| (not after
  // "turn off voice mode", whose controller stops once it has said so).
  void StopSession(bool stop_voice_mode);
  void StopListening();

  // Carries out what the logic decided.
  void Apply(SessionActions actions);
  void SendConfig();
  void SetDucking(bool duck);
  void UpdateCaption();
  void ScheduleTick();
  void OnTick();
  // Every second while listening: whether a tab plays sound.
  void OnPoll();
  int64_t NowMs() const;
  BrowserActivity ControllerActivity() const;

  raw_ptr<BrowserWindowInterface> browser_;
  ui::ScopedUnownedUserData<VoiceSession> scoped_data_holder_;
  base::CallbackListSubscription did_close_subscription_;
  raw_ptr<VoiceController> controller_;

  VoiceModeSettings settings_;
  VoiceSessionLogic logic_;
  std::unique_ptr<VoiceListener> listener_;
  std::unique_ptr<VoiceAudio> audio_;
  std::unique_ptr<VoiceEarcons> earcons_;
  std::vector<std::string> hotwords_;
  std::string partial_;
  std::string problem_;
  // The tabs whose media is ducked.
  std::vector<base::WeakPtr<content::WebContents>> ducked_;
  // "Voice mode is on" was said in this session.
  bool announced_ = false;
  base::OneShotTimer tick_timer_;
  base::RepeatingTimer poll_timer_;
  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<VoiceSession> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SESSION_H_
