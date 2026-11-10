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

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
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

class VoiceTestAudio;

// A buffer whose loudest sample reaches this (-30 dBFS), after the echo
// canceller and noise suppression, has speech in it (VoiceAudio::LastLoud()):
// the spoken suite's commands peak at -8 to -13 dBFS there, and the room
// and what the canceller leaves of the browser's own sound at -50 or less.
inline constexpr float kVoiceLoudPeak = 0.0316f;
// How long the microphone was quieter than that before an onset
// (VoiceAudio::Delegate::OnMicrophoneOnset()).
inline constexpr int64_t kOnsetQuietMs = 500;

// Which echo canceller the microphone gets.
enum class EchoCancellation {
  kChromeWide,  // WebRTC's, in the audio service; the browser's output
  kPlatform,    // the OS's; the system's output
  kNone,
};

// The choice above, from what the build and the device offer.
EchoCancellation ChooseEchoCancellation(bool chrome_wide_enabled,
                                        int device_effects);

// The parameters voice mode asks for: 16 kHz in 10 ms buffers. Mono, with
// the platform's echo canceller and noise suppression when |echo| is
// kPlatform; with kChromeWide stereo: the processed microphone, then what
// the echo canceller's linear filter alone left of it, before its
// suppressor (media/webrtc/molt_speech_recognizer_processing.h).
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
    // The microphone gave something as loud as speech (kVoiceLoudPeak),
    // after the browser's processing, after a quiet stretch
    // (kOnsetQuietMs): someone near it may have started speaking.
    virtual void OnMicrophoneOnset() = 0;

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
  // Stops, and forgets the delegate: for a VoiceAudio that is deleted
  // later (DeleteSoon()) than its delegate may be.
  void Release();
  bool capturing() const { return source_ != nullptr; }

  // What the microphone gave since the last call, from the capture
  // thread's counts: its loudest sample (0 to 1, of the first channel) and
  // how many samples. For the voice test log (voice_test_log.h).
  struct Levels {
    float peak = 0;
    int64_t samples = 0;
  };
  Levels TakeLevels();

  // When the microphone last gave something as loud as speech
  // (kVoiceLoudPeak), after the browser's processing: the capture time of
  // that buffer; null before. The listening tone waits for a pause
  // (voice_session_logic.h).
  base::TimeTicks LastLoud() const;
  // The loudest sample the microphone gave from |from| to |to| (capture
  // times), after the browser's processing; nullopt where its level history
  // doesn't reach (MicrophoneLevelHistory, kLevelSlots of kLevelSlotUs).
  std::optional<float> LoudestBetween(base::TimeTicks from,
                                      base::TimeTicks to) const;

  // What the echo canceller's linear filter alone left (the stream's second
  // channel, with Chrome-wide echo cancellation; HasLinearChannel()): the
  // speech helper hears it instead of the processed microphone while
  // |linear| is set (voice_session_logic.h), from the next buffer captured.
  // Over a playing video the suppressor takes the start of "Hey Molt" with
  // the video's echo; the linear filter leaves it, with more of the video.
  void SetHelperHearsLinear(bool linear);
  bool HasLinearChannel() const;
  // Its loudest sample from |from| to |to|, as LoudestBetween(); nullopt
  // also without it.
  std::optional<float> LinearLoudestBetween(base::TimeTicks from,
                                            base::TimeTicks to) const;
  // Whether the helper heard it for any of the audio captured from |from| to
  // |to| (as far as the history reaches).
  bool HelperHeardLinearBetween(base::TimeTicks from, base::TimeTicks to) const;

  // media::AudioCapturerSource::CaptureCallback (OnCaptureStarted() and
  // Capture() on the audio thread, OnCaptureError() on either):
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
  // OnCaptureStarted() and OnCaptureError(), back on this sequence.
  void CaptureStarted();
  // A speech onset Capture() saw, back on this sequence.
  void MicrophoneOnset();
  void CaptureError(media::AudioCapturerSource::ErrorCode code,
                    const std::string& message);

  SEQUENCE_CHECKER(sequence_checker_);
  raw_ptr<Delegate> delegate_;
  const scoped_refptr<VoiceListener::AudioSink> sink_;
  // With --molt-voice-test-audio (a test switch), what the helper gets is
  // also written there (voice_test_audio.h); else null.
  const scoped_refptr<VoiceTestAudio> test_audio_;
  std::unique_ptr<media::AudioSystem> audio_system_;
  scoped_refptr<media::AudioCapturerSource> source_;
  EchoCancellation echo_ = EchoCancellation::kNone;
  bool starting_ = false;
  // Set on this sequence before the stream starts, for the audio thread's
  // OnCaptureStarted() (no callback comes after Stop()).
  scoped_refptr<base::SequencedTaskRunner> owner_;
  base::WeakPtr<VoiceAudio> started_self_;
  // The capture thread's.
  AudioFrameChunker chunker_;
  // Written by the capture thread, taken here (TakeLevels()): the peak in
  // millionths, and the samples.
  std::atomic<uint32_t> peak_millionths_{0};
  std::atomic<int64_t> samples_{0};
  // LastLoud(), in microseconds of base::TimeTicks; 0 before.
  std::atomic<int64_t> loud_at_us_{0};
  // Noted by the capture thread, read here (LoudestBetween()).
  MicrophoneLevelHistory level_history_;
  // The stream's second channel is the linear filter's output: set in
  // Open() before the stream starts.
  std::atomic<bool> linear_channel_{false};
  // SetHelperHearsLinear(), read by the capture thread.
  std::atomic<bool> helper_hears_linear_{false};
  // Noted by the capture thread: the second channel's levels
  // (LinearLoudestBetween()), and 1 for each buffer the helper heard it, 0
  // for the others (HelperHeardLinearBetween()).
  MicrophoneLevelHistory linear_level_history_;
  MicrophoneLevelHistory linear_heard_history_;
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
