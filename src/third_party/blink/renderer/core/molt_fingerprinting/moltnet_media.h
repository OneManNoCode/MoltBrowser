// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the media hardware MoltNet windows
// (kUniform) describe. Every MoltNet user gets the same answers; the fixed
// values and the rules that derive them are in moltnet_media_values.h.
//
// Upstream hooks (each marked "MoltBrowser:"):
//
//   Web Audio   AudioContext::Create (audio_context.cc): the sample rate when
//                 the page gives none, the "hardware" render quantum
//                                         -> UniformAudioContextSampleRate,
//                                            UniformRenderQuantumFrames
//               AudioContext::outputLatency, getOutputTimestamp,
//                 HandlePreRenderTasks (playbackStats)
//                                         -> UniformAudioOutputLatency,
//                                            UniformAudioOutputPosition,
//                                            UniformAudioPlayoutDelay
//               AudioContext::IsValidSinkDescriptor (the default device's
//                 ID, for setSinkId() and device changes)
//                                         -> HasUniformDefaultAudioOutput
//               AudioDestination's constructor: the device it renders for
//                 (platform/audio/audio_destination.cc)
//                                         -> moltnet_audio_device.h
//               RealtimeAudioDestinationHandler::MaxChannelCount,
//                 GetCallbackBufferSize, GetFramesPerBuffer
//                 (realtime_audio_destination_handler.cc: maxChannelCount
//                 and the channelCount check, the base latency the context
//                 computes from the device's buffer, createScriptProcessor()'s
//                 default size)       -> UniformAudioMaxChannelCount,
//                                        UniformAudioCallbackFrames
//   EME         EncryptedMediaUtils::GetEncryptedMediaClientFromLocalDOMWindow
//                 (encrypted_media_utils.cc; requestMediaKeySystemAccess()
//                 and Media Capabilities' key-system queries both get their
//                 client there)           -> MoltNetEncryptedMediaClient
//   Media       MediaCapabilities::decodingInfo / encodingInfo ("webrtc"),
//   Capabilities  GetPerfInfo (video)     -> UniformWebrtcCodingInfo,
//                                            UniformVideoDecodingInfo
//   WebCodecs   RetrieveGpuFactories (gpu_factories_retriever.cc: hardware
//                 decoders and encoders, OS encoders),
//               VideoDecoderBroker's UpdateDecoderFactory
//                 (video_decoder_broker.cc: platform decoders)
//                                         -> UsesPlatformMediaCodecs
//
// What MoltNet leaves alone, and why (docs/FINGERPRINTING.md, "MoltNet:
// audio, DRM and media capabilities"): AudioContext.sinkId and setSinkId()
// (output device IDs only after the user grants microphone access, per site
// and per session, like enumerateDevices()); canPlayType(),
// MediaSource.isTypeSupported() and MediaRecorder.isTypeSupported() (the
// decoders and encoders compiled into the build); getVideoPlaybackQuality()
// (dropped frames are a measure of speed a page can also time itself);
// OfflineAudioContext (farbled when read, like every AudioBuffer).
//
// Which contexts: the whole MoltNet renderer process, windows and workers
// (WebCodecs and Media Capabilities exist in workers), like the other
// uniform values. Normal and Incognito windows (kFarble) are unchanged:
// none of these values can vary per site without breaking audio or video,
// and fixing them would only make a user look like other MoltBrowser users.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEDIA_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEDIA_H_

#include <cstdint>
#include <optional>

#include "base/time/time.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media_values.h"

namespace blink {

class WebAudioLatencyHint;
class WebEncryptedMediaClient;

namespace molt_fingerprinting {

// --- Web Audio --------------------------------------------------------------

// The sample rate a new AudioContext runs at: `requested` when the page
// gave one (or outside MoltNet, where nullopt means the device's rate),
// else MoltNet's 48 kHz.
CORE_EXPORT std::optional<float> UniformAudioContextSampleRate(
    std::optional<float> requested);

// The render quantum a new AudioContext uses: `requested` (nullopt for
// renderSizeHint "hardware"), except that MoltNet renders "hardware" in
// 128 frames, whatever the device's buffer.
CORE_EXPORT std::optional<uint32_t> UniformRenderQuantumFrames(
    std::optional<uint32_t> requested);

// The callback buffer, in frames, MoltNet reports for a context with
// `latency_hint` running at `context_sample_rate`: the uniform device's
// (moltnet::AudioCallbackFrames). nullopt outside MoltNet (or for a rate an
// AudioContext can't have).
CORE_EXPORT std::optional<uint32_t> UniformAudioCallbackFrames(
    const WebAudioLatencyHint& latency_hint,
    double context_sample_rate);

// The uniform device's callback buffer for `latency_hint` at
// `context_sample_rate` in any mode (0 for a rate an AudioContext can't
// have): what UniformAudioCallbackFrames() reports in MoltNet and the tick of
// MoltNet's audio clock (moltnet_audio_device.h).
CORE_EXPORT uint32_t UniformDeviceCallbackFrames(
    const WebAudioLatencyHint& latency_hint,
    double context_sample_rate);

// Whether the default output device always exists (true in MoltNet, whose
// contexts render on their own clock): AudioContext's sink checks then
// accept it on a machine that lists no output device.
CORE_EXPORT bool HasUniformDefaultAudioOutput();

// destination.maxChannelCount: 2 in MoltNet, else `real`.
CORE_EXPORT uint32_t UniformAudioMaxChannelCount(uint32_t real);

// AudioContext.outputLatency in MoltNet (the same before, while and after
// the context plays). nullopt outside MoltNet.
CORE_EXPORT std::optional<double> UniformAudioOutputLatency();

// The playout delay MoltNet's playbackStats see for every rendered buffer
// (with no underruns). nullopt outside MoltNet. Audio rendering thread.
CORE_EXPORT std::optional<base::TimeDelta> UniformAudioPlayoutDelay();

// getOutputTimestamp() in MoltNet for a context whose currentTime is
// `current_time`: the context time being heard, and whether anything has
// been rendered yet (performanceTime is then "now", else 0). nullopt outside
// MoltNet.
CORE_EXPORT std::optional<moltnet::AudioOutputPosition>
UniformAudioOutputPosition(double current_time);

// --- Encrypted Media Extensions ---------------------------------------------

// In MoltNet: a client that refuses every key system (Clear Key included)
// with NotSupportedError, used instead of the frame's own, which is then
// never created. nullptr outside MoltNet.
CORE_EXPORT WebEncryptedMediaClient* MoltNetEncryptedMediaClient();

// --- Media Capabilities -----------------------------------------------------

// decodingInfo() for a clear video configuration the build supports, in
// MoltNet: from the fixed table (moltnet::VideoDecodingInfo), never from the
// decode history or the GPU. `has_built_in_decoder`: whether the build has
// its own decoder for the codec (media::IsDecoderBuiltInVideoCodec).
CORE_EXPORT std::optional<moltnet::MediaCapabilitiesAnswer>
UniformVideoDecodingInfo(bool has_built_in_decoder);

// decodingInfo() and encodingInfo() of type "webrtc", in MoltNet.
CORE_EXPORT std::optional<moltnet::MediaCapabilitiesAnswer>
UniformWebrtcCodingInfo();

// --- WebCodecs --------------------------------------------------------------

// Whether WebCodecs may use the platform's codecs (hardware decoders and
// encoders, the operating system's encoders): false in MoltNet, which then
// only uses the codecs compiled into the build. Any thread.
CORE_EXPORT bool UsesPlatformMediaCodecs();

// Makes the functions above behave as in kUniform without configuring the
// process for it (which would pin the time zone and locale for every later
// test); also renders new AudioContexts on MoltNet's clock
// (PinUniformAudioClock).
CORE_EXPORT void SetMoltNetMediaForTesting(bool uniform);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEDIA_H_
