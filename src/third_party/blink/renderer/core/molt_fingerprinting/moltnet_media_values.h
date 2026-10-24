// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: what MoltNet windows report about
// the machine's media hardware, the same for every MoltNet user. Pure and
// std-only (tested standalone, like farbling.h); the Blink side is
// moltnet_media.h.
//
//   Web Audio   one output device: 48 kHz, stereo, a 10 ms buffer, a fixed
//               output latency, no underruns. AudioContext.sampleRate,
//               baseLatency for every latencyHint, outputLatency,
//               getOutputTimestamp(), destination.maxChannelCount,
//               createScriptProcessor()'s default buffer size and
//               playbackStats all describe it.
//   EME         no key system at all (as in a build without CDMs).
//   Media       a fixed table: supported from the decoders compiled into
//   Capabilities  the build, smooth and never power efficient for video,
//               nothing for WebRTC (which MoltNet doesn't offer).
//   WebCodecs   software codecs only, so "prefer-hardware" is never
//               supported.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEDIA_VALUES_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEDIA_VALUES_H_

#include <cstdint>

namespace blink::molt_fingerprinting::moltnet {

// --- Web Audio: the output device -------------------------------------------
//
// The sample rate an AudioContext runs at when the page doesn't ask for one.
// 48 kHz is what nearly every output device runs at today (Windows' and
// PipeWire's default, Apple's built-in speakers, every HDMI and USB
// interface), Chromium's own rate when it can't read the device, and the
// rate of Opus and WebRTC audio. A device at another rate gets the context's
// audio resampled by the audio service, which Chromium already does for any
// page that asks for a sample rate. (Tor Browser reports 44.1 kHz; a page
// tells Chromium from Firefox in many other ways, so matching it would only
// cost most MoltNet users a resampler.)
inline constexpr float kAudioSampleRate = 48000.0f;

// destination.maxChannelCount: stereo, as Tor Browser reports. A page can't
// set destination.channelCount above it, so no more channels can be probed.
inline constexpr uint32_t kAudioMaxChannelCount = 2;

// The device's buffer: 10 ms at 48 kHz, Windows' shared-mode period and the
// buffer Chromium falls back to when it can't read the device. The callback
// buffer a context gets is derived from it the way Chromium derives it from
// a real device (RendererWebAudioDeviceImpl::GetOutputBufferSize), with no
// hardware minimum or maximum of its own.
inline constexpr int kAudioDeviceSampleRate = 48000;
inline constexpr int kAudioDeviceFramesPerBuffer = 480;
// media::limits::kMinWebAudioBufferSize / kMaxWebAudioBufferSize.
inline constexpr int kAudioMinCallbackFrames = 128;
inline constexpr int kAudioMaxCallbackFrames = 8192;

// AudioContext.outputLatency: 24 ms, a 10 ms device with double buffering
// and a few milliseconds of converter latency, on the 8 ms grid Chromium
// rounds outputLatency to. playbackStats reports it as the minimum,
// average and maximum latency. (Tor Browser: 25 ms on Linux, 40 ms on
// Windows, 512 frames on macOS.)
inline constexpr double kAudioOutputLatencySeconds = 0.024;

// renderSizeHint "hardware": the device's buffer would be the hardware's;
// MoltNet renders in Chromium's default quantum.
inline constexpr uint32_t kAudioHardwareRenderQuantum = 128;

// WebAudioLatencyHint::AudioContextLatencyCategory, without kLastValue
// (moltnet_media.cc checks the values).
enum class AudioLatency { kInteractive, kBalanced, kPlayback, kExact };

// The callback buffer, in frames, of a context running at
// `context_sample_rate` (a valid AudioContext rate, 3000 to 768000 Hz) on the
// uniform device, for latencyHint `latency` (`exact_seconds` is the hint's
// value for kExact and ignored otherwise; any double is safe):
//   interactive, balanced  the device's 10 ms, at the context's rate
//   playback               20 ms rounded up to a power of two (1024 frames
//                          at 48 kHz), at least the 10 ms
//   exact                  the hint in frames, at least 128 and the 10 ms
//                          unless smaller than 128 frames, rounded up to
//                          128 frames, at most 8192
// Returns 0 for a rate outside the valid range.
int AudioCallbackFrames(AudioLatency latency,
                        double exact_seconds,
                        double context_sample_rate);

// AudioContext.baseLatency for that context: its callback buffer or its
// render quantum, whichever is longer, in seconds. 0 for an invalid rate.
double AudioBaseLatency(AudioLatency latency,
                        double exact_seconds,
                        uint32_t render_quantum_frames,
                        double context_sample_rate);

// createScriptProcessor()'s buffer size for bufferSize 0: four callback
// buffers rounded to a power of two, in [256, 16384], as Chromium chooses
// it (script_processor_node.cc, ChooseBufferSize).
uint32_t ScriptProcessorBufferSize(uint32_t callback_frames);

// getOutputTimestamp(): the context time that is being heard and whether it
// has started. Before the context has rendered anything both are 0, as in
// Chromium; afterwards the frame being heard is the one rendered
// kAudioOutputLatencySeconds ago, heard now.
struct AudioOutputPosition {
  double context_time;
  bool playing;
};
AudioOutputPosition AudioOutputPositionAt(double current_time);

// --- Encrypted Media Extensions ---------------------------------------------
//
// Every requestMediaKeySystemAccess() (and every Media Capabilities query
// with a keySystemConfiguration) is refused with this message, Chromium's
// own for an unsupported key system, Clear Key included. Which CDMs a
// machine has, their versions, robustness levels and hardware-secure
// decoding are device signals. MoltBrowser's desktop builds ship neither
// Widevine nor PlayReady (both need Chrome branding), so today this only
// turns off Clear Key, a test and interoperability key system that real
// DRM content doesn't use; it keeps the answer the same on every machine
// whatever CDMs a build or the operating system offers, and the renderer
// never asks the browser which key systems exist. Tor Browser ships with
// DRM off.
inline constexpr char kNoKeySystemMessage[] =
    "Unsupported keySystem or supportedConfigurations.";

// --- Media Capabilities -----------------------------------------------------
//
// decodingInfo() for a clear (unencrypted) video file or MediaSource stream
// the build can parse: supported only when a decoder for the codec is
// compiled in (never a platform or hardware decoder), smooth, and never
// power efficient (software decoding), as Tor Browser reports; never from
// the device or the decode history. Audio-only queries keep Chromium's
// answer, which only depends on the decoders compiled in.
struct MediaCapabilitiesAnswer {
  bool supported;
  bool smooth;
  bool power_efficient;
};
MediaCapabilitiesAnswer VideoDecodingInfo(bool has_built_in_decoder);

// decodingInfo() and encodingInfo() of type "webrtc": WebRTC isn't available
// in MoltNet windows, so no configuration is supported.
inline constexpr MediaCapabilitiesAnswer kWebrtcCodingInfo = {false, false,
                                                              false};

}  // namespace blink::molt_fingerprinting::moltnet

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEDIA_VALUES_H_
