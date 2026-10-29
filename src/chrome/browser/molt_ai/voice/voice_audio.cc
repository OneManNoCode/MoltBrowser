// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_audio.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/synchronization/lock.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/unguessable_token.h"
#include "chrome/browser/permissions/system/system_permission_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "content/public/browser/audio_service.h"
#include "media/audio/audio_device_description.h"
#include "media/audio/audio_input_device.h"
#include "media/audio/audio_input_ipc.h"
#include "media/audio/audio_system.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_capturer_source.h"
#include "media/base/audio_glitch_info.h"
#include "media/base/audio_parameters.h"
#include "media/base/audio_processing.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/channel_layout.h"
#include "media/base/media_switches.h"
#include "media/mojo/common/input_error_code_converter.h"
#include "media/mojo/mojom/audio_data_pipe.mojom.h"
#include "media/mojo/mojom/audio_input_stream.mojom.h"
#include "media/mojo/mojom/audio_processing.mojom.h"
#include "media/mojo/mojom/audio_stream_factory.mojom.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/audio/public/cpp/output_device.h"

namespace molt_ai {
namespace voice {

namespace {

// 10 ms of 16 kHz: the buffer WebRTC's audio processing works in.
constexpr int kCaptureFramesPerBuffer = kHelperSampleRate / 100;
// The earcons' stream: 20 ms buffers.
constexpr int kEarconFramesPerBuffer = kEarconSampleRate / 50;

// An AudioInputIPC like audio::InputIPC (services/audio/public/cpp/
// input_ipc.cc) that asks the audio service for audio processing: echo
// cancellation and noise suppression, with the browser's own output as the
// echo canceller's reference (Chrome-wide echo cancellation).
// audio::CreateInputDevice() asks for none. Used on one sequence.
class ProcessedInputIPC : public media::AudioInputIPC,
                          public media::mojom::AudioInputStreamClient {
 public:
  ProcessedInputIPC(
      mojo::PendingRemote<media::mojom::AudioStreamFactory> stream_factory,
      bool processing)
      : pending_stream_factory_(std::move(stream_factory)),
        processing_(processing) {
    DETACH_FROM_SEQUENCE(sequence_checker_);
  }

  ProcessedInputIPC(const ProcessedInputIPC&) = delete;
  ProcessedInputIPC& operator=(const ProcessedInputIPC&) = delete;
  ~ProcessedInputIPC() override = default;

  // media::AudioInputIPC:
  void CreateStream(media::AudioInputIPCDelegate* delegate,
                    const media::AudioParameters& params,
                    bool automatic_gain_control,
                    uint32_t total_segments) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    delegate_ = delegate;
    if (!stream_factory_.is_bound()) {
      stream_factory_.Bind(std::move(pending_stream_factory_));
    }
    auto client = stream_client_receiver_.BindNewPipeAndPassRemote();
    stream_client_receiver_.set_disconnect_handler(
        base::BindOnce(&ProcessedInputIPC::OnError, base::Unretained(this),
                       media::mojom::InputStreamErrorCode::kUnknown));
    media::mojom::AudioProcessingConfigPtr config;
    if (processing_) {
      media::AudioProcessingSettings settings;
      settings.echo_cancellation = true;
      settings.noise_suppression = true;
      // Gain control would move the system's microphone level.
      settings.automatic_gain_control = false;
      settings.multi_channel_capture_processing = false;
      settings.use_loopback_aec_reference = false;
      config = media::mojom::AudioProcessingConfig::New(
          controls_.BindNewPipeAndPassReceiver(), settings);
    }
    stream_factory_->CreateInputStream(
        stream_.BindNewPipeAndPassReceiver(), std::move(client),
        /*observer=*/{}, /*log=*/{},
        media::AudioDeviceDescription::kDefaultDeviceId, params,
        base::UnguessableToken::Create(), total_segments,
        automatic_gain_control, std::move(config),
        base::BindOnce(&ProcessedInputIPC::StreamCreated,
                       weak_factory_.GetWeakPtr()));
  }

  void RecordStream() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (stream_.is_bound()) {
      stream_->Record();
    }
  }

  void SetVolume(double volume) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (stream_.is_bound()) {
      stream_->SetVolume(volume);
    }
  }

  void SetOutputDeviceForAec(const std::string& output_device_id) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (stream_factory_.is_bound() && stream_id_) {
      stream_factory_->AssociateInputAndOutputForAec(*stream_id_,
                                                     output_device_id);
    }
  }

  void CloseStream() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    delegate_ = nullptr;
    if (stream_client_receiver_.is_bound()) {
      stream_client_receiver_.reset();
    }
    stream_.reset();
    controls_.reset();
    weak_factory_.InvalidateWeakPtrs();
  }

 private:
  // media::mojom::AudioInputStreamClient:
  void OnError(media::mojom::InputStreamErrorCode code) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (delegate_) {
      delegate_->OnError(media::ConvertToCaptureCallbackCode(code));
    }
  }

  void OnMutedStateChanged(bool is_muted) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (delegate_) {
      delegate_->OnMuted(is_muted);
    }
  }

  void StreamCreated(media::mojom::ReadWriteAudioDataPipePtr data_pipe,
                     bool initially_muted,
                     const std::optional<base::UnguessableToken>& stream_id) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!delegate_) {
      return;
    }
    if (data_pipe.is_null() || !data_pipe->socket.is_valid_platform_file()) {
      OnError(media::mojom::InputStreamErrorCode::kUnknown);
      return;
    }
    stream_id_ = stream_id;
    base::ScopedPlatformFile socket_handle =
        data_pipe->socket.TakePlatformFile();
    delegate_->OnStreamCreated(std::move(data_pipe->shared_memory),
                               std::move(socket_handle), initially_muted);
  }

  SEQUENCE_CHECKER(sequence_checker_);
  mojo::PendingRemote<media::mojom::AudioStreamFactory> pending_stream_factory_;
  mojo::Remote<media::mojom::AudioStreamFactory> stream_factory_;
  mojo::Remote<media::mojom::AudioInputStream> stream_;
  mojo::Remote<media::mojom::AudioProcessorControls> controls_;
  mojo::Receiver<media::mojom::AudioInputStreamClient> stream_client_receiver_{
      this};
  raw_ptr<media::AudioInputIPCDelegate> delegate_ = nullptr;
  std::optional<base::UnguessableToken> stream_id_;
  const bool processing_;
  base::WeakPtrFactory<ProcessedInputIPC> weak_factory_{this};
};

mojo::PendingRemote<media::mojom::AudioStreamFactory> StreamFactory() {
  mojo::PendingRemote<media::mojom::AudioStreamFactory> factory;
  content::GetAudioServiceStreamFactoryBinder().Run(
      factory.InitWithNewPipeAndPassReceiver());
  return factory;
}

}  // namespace

EchoCancellation ChooseEchoCancellation(bool chrome_wide_enabled,
                                        int device_effects) {
  if (chrome_wide_enabled) {
    return EchoCancellation::kChromeWide;
  }
  if (device_effects & media::AudioParameters::ECHO_CANCELLER) {
    return EchoCancellation::kPlatform;
  }
  return EchoCancellation::kNone;
}

media::AudioParameters VoiceCaptureParameters(EchoCancellation echo) {
  media::AudioParameters params(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
                                media::ChannelLayoutConfig::Mono(),
                                kHelperSampleRate, kCaptureFramesPerBuffer);
  if (echo == EchoCancellation::kPlatform) {
    params.set_effects(media::AudioParameters::ECHO_CANCELLER |
                       media::AudioParameters::NOISE_SUPPRESSION);
  }
  return params;
}

VoiceAudio::VoiceAudio(Delegate* delegate,
                       scoped_refptr<VoiceListener::AudioSink> sink)
    : delegate_(delegate), sink_(std::move(sink)) {}

VoiceAudio::~VoiceAudio() {
  Stop();
}

void VoiceAudio::Start() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (source_ || starting_) {
    return;
  }
  starting_ = true;
  using ContentSettingsType = content_settings::mojom::ContentSettingsType;
  // The OS's microphone permission: refused already, or to be asked (macOS
  // shows its prompt the first time).
  if (system_permission_settings::IsDenied(
          ContentSettingsType::MEDIASTREAM_MIC)) {
    starting_ = false;
    delegate_->OnMicrophoneProblem(Problem::kPermissionDenied);
    return;
  }
  if (system_permission_settings::CanPrompt(
          ContentSettingsType::MEDIASTREAM_MIC)) {
    system_permission_settings::Request(
        ContentSettingsType::MEDIASTREAM_MIC,
        base::BindOnce(&VoiceAudio::OnPermissionAsked,
                       weak_factory_.GetWeakPtr()));
    return;
  }
  OnPermissionAsked();
}

void VoiceAudio::OnPermissionAsked() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!starting_) {
    return;  // stopped meanwhile
  }
  if (system_permission_settings::IsDenied(
          content_settings::mojom::ContentSettingsType::MEDIASTREAM_MIC)) {
    starting_ = false;
    delegate_->OnMicrophoneProblem(Problem::kPermissionDenied);
    return;
  }
  // Chrome-wide echo cancellation needs nothing from the device; the
  // platform's needs a device that has it.
  if (media::IsChromeWideEchoCancellationEnabled()) {
    Open(EchoCancellation::kChromeWide);
    return;
  }
  audio_system_ = content::CreateAudioSystemForAudioService();
  audio_system_->GetInputStreamParameters(
      media::AudioDeviceDescription::kDefaultDeviceId,
      base::BindOnce(&VoiceAudio::OnDeviceParameters,
                     weak_factory_.GetWeakPtr()));
}

void VoiceAudio::OnDeviceParameters(
    const std::optional<media::AudioParameters>& params) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!starting_) {
    return;
  }
  if (!params) {
    starting_ = false;
    delegate_->OnMicrophoneProblem(Problem::kNoMicrophone);
    return;
  }
  Open(
      ChooseEchoCancellation(/*chrome_wide_enabled=*/false, params->effects()));
}

void VoiceAudio::Open(EchoCancellation echo) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  echo_ = echo;
  if (echo == EchoCancellation::kNone) {
    LOG(WARNING) << "[MoltVoice] no echo cancellation for the microphone: "
                    "page audio and the browser's voice reach the recognizer";
  }
  source_ = base::MakeRefCounted<media::AudioInputDevice>(
      std::make_unique<ProcessedInputIPC>(
          StreamFactory(), echo == EchoCancellation::kChromeWide),
      media::AudioInputDevice::Purpose::kUserInput,
      media::AudioInputDevice::DeadStreamDetection::kEnabled);
  source_->Initialize(VoiceCaptureParameters(echo), this);
  source_->Start();
}

void VoiceAudio::Stop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  starting_ = false;
  weak_factory_.InvalidateWeakPtrs();
  if (source_) {
    // Synchronous: no Capture() after this.
    source_->Stop();
    source_ = nullptr;
  }
  audio_system_.reset();
  chunker_.Reset();
}

void VoiceAudio::OnCaptureStarted() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  starting_ = false;
  delegate_->OnMicrophoneStarted(echo_);
}

void VoiceAudio::Capture(const media::AudioBus* audio_source,
                         base::TimeTicks audio_capture_time,
                         const media::AudioGlitchInfo& glitch_info,
                         double volume) {
  // The capture thread. Mono was asked for; a device that gives more
  // channels still has the voice in the first.
  if (!audio_source || audio_source->channels() < 1) {
    return;
  }
  std::vector<std::vector<int16_t>> frames;
  chunker_.Add(audio_source->channel(0), &frames);
  for (const std::vector<int16_t>& frame : frames) {
    sink_->Push(frame);
  }
}

void VoiceAudio::OnCaptureError(media::AudioCapturerSource::ErrorCode code,
                                const std::string& message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LOG(WARNING) << "[MoltVoice] microphone error " << static_cast<int>(code)
               << ": " << message;
  Stop();
  switch (code) {
    case media::AudioCapturerSource::ErrorCode::kSystemPermissions:
      delegate_->OnMicrophoneProblem(Problem::kPermissionDenied);
      return;
    case media::AudioCapturerSource::ErrorCode::kDeviceRemoved:
      delegate_->OnMicrophoneProblem(Problem::kNoMicrophone);
      return;
    case media::AudioCapturerSource::ErrorCode::kUnknown:
    case media::AudioCapturerSource::ErrorCode::kDeviceInUse:
    case media::AudioCapturerSource::ErrorCode::kSocketError:
      delegate_->OnMicrophoneProblem(Problem::kFailed);
      return;
  }
}

void VoiceAudio::OnCaptureMuted(bool is_muted) {
  // A muted microphone (the OS's or the hardware's mute) just gives
  // silence; the helper hears nothing.
}

// ---- Earcons
// -----------------------------------------------------------------

// Plays one earcon at a time through an audio::OutputDevice. Its own
// sequence; Render() runs on the audio thread.
class VoiceEarcons::Player : public media::AudioRendererSink::RenderCallback {
 public:
  Player() = default;
  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;
  ~Player() override = default;

  void Play(EarconKind kind) {
    std::vector<int16_t> samples = MakeEarcon(kind);
    const base::TimeDelta length = base::Milliseconds(
        static_cast<int64_t>(samples.size()) * 1000 / kEarconSampleRate);
    {
      base::AutoLock lock(lock_);
      samples_ = std::move(samples);
      position_ = 0;
    }
    if (!device_) {
      device_ = std::make_unique<audio::OutputDevice>(
          StreamFactory(),
          media::AudioParameters(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
                                 media::ChannelLayoutConfig::Mono(),
                                 kEarconSampleRate, kEarconFramesPerBuffer),
          this, media::AudioDeviceDescription::kDefaultDeviceId);
    }
    device_->Play();
    // Paused once it has played (the stream stays open for the next one).
    pause_timer_.Start(FROM_HERE, length + kEarconTail,
                       base::BindOnce(&audio::OutputDevice::Pause,
                                      base::Unretained(device_.get())));
  }

  // media::AudioRendererSink::RenderCallback (the audio thread):
  int Render(base::TimeDelta delay,
             base::TimeTicks delay_timestamp,
             const media::AudioGlitchInfo& glitch_info,
             media::AudioBus* dest) override {
    base::span<float> out = dest->channel(0);
    base::AutoLock lock(lock_);
    const size_t frames = out.size();
    for (size_t i = 0; i < frames; ++i) {
      out[i] = position_ < samples_.size()
                   ? static_cast<float>(samples_[position_++]) / 32768.0f
                   : 0.0f;
    }
    return static_cast<int>(frames);
  }

  void OnRenderError() override {}

 private:
  // Silence after an earcon before the stream is paused.
  static constexpr base::TimeDelta kEarconTail = base::Milliseconds(300);

  // What Render() reads; destroyed after the device (whose audio thread
  // calls Render() until it goes).
  base::Lock lock_;
  std::vector<int16_t> samples_ GUARDED_BY(lock_);
  size_t position_ GUARDED_BY(lock_) = 0;
  std::unique_ptr<audio::OutputDevice> device_;
  // Destroyed first: it holds the device.
  base::OneShotTimer pause_timer_;
};

VoiceEarcons::VoiceEarcons()
    : player_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {}

VoiceEarcons::~VoiceEarcons() = default;

void VoiceEarcons::Play(EarconKind kind) {
  player_.AsyncCall(&Player::Play).WithArgs(kind);
}

}  // namespace voice
}  // namespace molt_ai
