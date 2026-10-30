// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's microphone and sounds (docs/design/TOUCHLESS_BROWSING.md
// §5.1, §6.1; VOICE_BENCH.md §3.2): the default microphone, captured in the
// browser process through Chromium's audio service at 16 kHz mono, with
// echo cancellation and noise suppression, cut into the speech helper's
// frames; and the earcons, played through the same audio service.
//
// Echo cancellation, per OS (Chromium 156):
//   - Windows, macOS, Linux, by default: Chrome-wide echo cancellation
//     (media::kChromeWideEchoCancellation, built where
//     BUILDFLAG(CHROME_WIDE_ECHO_CANCELLATION): is_win || is_mac ||
//     is_linux). The stream is created with an AudioProcessingConfig
//     (echo cancellation and noise suppression; no gain control, which
//     would move the system's microphone level), so the audio service runs
//     WebRTC's audio processing on it (AudioProcessorHandler) with
//     OutputDeviceMixerManager's reference: everything the browser plays to
//     the default output device through the audio service, pages' media and
//     Web Audio and the earcons alike. That is the reference the design asks
//     for (§6.1 step 2). audio::CreateInputDevice() passes no processing
//     config, so this file has its own AudioInputIPC (as
//     audio::InputIPC, with the config) under a media::AudioInputDevice;
//     content::SpeechRecognizerImpl has no echo cancellation at all.
//   - Where Chrome-wide echo cancellation is off (the feature disabled, or
//     kEnforceSystemEchoCancellation on Windows and macOS): the platform's
//     echo canceller when the device offers one
//     (AudioParameters::ECHO_CANCELLER: macOS Voice Processing I/O, the
//     Windows voice capture DSP), whose reference is the system's output.
//   - Otherwise none; the log says so. Voice mode still runs, but a tab's
//     sound then reaches the recognizer as it is: while a tab plays sound,
//     only the commands that always count do (voice_session_logic.h,
//     OnEchoCancellation()), and recognition suffers.
//   Not in any reference: the platform's text-to-speech voices, which play
//   outside the audio service (VoiceSessionLogic handles that).
//
// The microphone permission: the OS asks the first time
// (system_permission_settings: macOS's prompt); if the OS refuses, or
// capture fails for want of it, the delegate hears kPermissionDenied, and
// voice mode says so aloud and on screen.
//
// UI thread, except Capture() (the audio capture thread), which only cuts
// frames and hands them to the sink.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_AUDIO_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_AUDIO_H_

#include <memory>
#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/threading/sequence_bound.h"
#include "chrome/browser/molt_ai/voice/voice_audio_samples.h"
#include "chrome/browser/molt_ai/voice/voice_listener.h"
#include "chrome/browser/molt_ai/voice/voice_session_logic.h"
#include "media/base/audio_capturer_source.h"
#include "media/base/audio_parameters.h"

namespace media {
class AudioSystem;
}  // namespace media

namespace molt_ai {
namespace voice {

// Which echo canceller the microphone gets.
enum class EchoCancellation {
  kChromeWide,  // WebRTC's, in the audio service; the browser's output
  kPlatform,    // the OS's; the system's output
  kNone,
};

// The choice above, from what the build and the device offer.
EchoCancellation ChooseEchoCancellation(bool chrome_wide_enabled,
                                        int device_effects);

// The parameters voice mode asks for: 16 kHz mono in 10 ms buffers, with
// the platform's echo canceller and noise suppression when |echo| is
// kPlatform.
media::AudioParameters VoiceCaptureParameters(EchoCancellation echo);

class VoiceAudio : public media::AudioCapturerSource::CaptureCallback {
 public:
  enum class Problem {
    kPermissionDenied,
    kNoMicrophone,
    kFailed,
  };

  class Delegate {
   public:
    virtual void OnMicrophoneStarted(EchoCancellation echo) = 0;
    virtual void OnMicrophoneProblem(Problem problem) = 0;

   protected:
    virtual ~Delegate() = default;
  };

  // Frames go to |sink| (VoiceListener::audio_sink()) from the capture
  // thread.
  VoiceAudio(Delegate* delegate, scoped_refptr<VoiceListener::AudioSink> sink);
  VoiceAudio(const VoiceAudio&) = delete;
  VoiceAudio& operator=(const VoiceAudio&) = delete;
  ~VoiceAudio() override;

  // Asks the OS for the microphone if it hasn't yet, then captures.
  void Start();
  void Stop();
  bool capturing() const { return source_ != nullptr; }

  // media::AudioCapturerSource::CaptureCallback:
  void OnCaptureStarted() override;
  void Capture(const media::AudioBus* audio_source,
               base::TimeTicks audio_capture_time,
               const media::AudioGlitchInfo& glitch_info,
               double volume) override;
  void OnCaptureError(media::AudioCapturerSource::ErrorCode code,
                      const std::string& message) override;
  void OnCaptureMuted(bool is_muted) override;

 private:
  void OnPermissionAsked();
  void OnDeviceParameters(const std::optional<media::AudioParameters>& params);
  void Open(EchoCancellation echo);

  SEQUENCE_CHECKER(sequence_checker_);
  raw_ptr<Delegate> delegate_;
  const scoped_refptr<VoiceListener::AudioSink> sink_;
  std::unique_ptr<media::AudioSystem> audio_system_;
  scoped_refptr<media::AudioCapturerSource> source_;
  EchoCancellation echo_ = EchoCancellation::kNone;
  bool starting_ = false;
  // The capture thread's.
  AudioFrameChunker chunker_;
  base::WeakPtrFactory<VoiceAudio> weak_factory_{this};
};

// Plays the earcons (voice_audio_samples.h) through the audio service, so
// the echo canceller has them in its reference. One stream, opened on
// first use and kept until this goes; its own sequence.
class VoiceEarcons {
 public:
  VoiceEarcons();
  VoiceEarcons(const VoiceEarcons&) = delete;
  VoiceEarcons& operator=(const VoiceEarcons&) = delete;
  ~VoiceEarcons();

  void Play(EarconKind kind);

 private:
  class Player;
  base::SequenceBound<Player> player_;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_AUDIO_H_
