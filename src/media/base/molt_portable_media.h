// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: media pixels and bytes that don't depend on the machine.
//
// MoltNet's renderer processes call SetPortableMediaProcess() once, when
// they start (third_party/blink/renderer/core/molt_fingerprinting/
// moltnet_pixels.h), before any media object exists. From then on what a
// page of the process can read back of decoded, converted, scaled or encoded
// media is computed the same way on every machine of an OS:
//
//   * libopus encodes and decodes with its C kernels, never the SSE, AVX2 or
//     NEON ones, whose sums round differently and whose x86 pulse search is
//     built on approximate-reciprocal instructions that differ between CPU
//     vendors (third_party/opus: BUILD.gn, src/celt/celt.c,
//     src/celt/x86/x86cpu.c, src/celt/arm/*_neon_intr.c);
//   * video is decoded by the build's software decoders, never the GPU's
//     (DefaultDecoderFactory), and decoded frames stay in CPU memory
//     (MappableSharedImageVideoFramePool);
//   * YUV frames a page draws into a canvas or uploads to WebGL are
//     converted to RGB by libyuv on the CPU, never by the GPU
//     (PaintCanvasVideoRenderer);
//   * frames an encoder scales use PortableScalePlane() instead of libyuv's
//     filters, which round differently on x86 and Arm (VideoFrameConverter);
//   * audio an encoder resamples or downmixes uses the plain C convolution
//     and multiply-add instead of the AVX2/FMA, SSE or NEON ones
//     (SincResampler, vector_math::FMAC);
//   * Symphonia (FLAC, MP3, PCM, Vorbis) computes its (I)FFTs with rustfft's
//     scalar code instead of the AVX/FMA, SSE4.1 or NEON code picked for the
//     CPU (SymphoniaAudioDecoder, symphonia-core's dsp/fft/simd.rs), so
//     decoded Vorbis samples are the same on x86-64 and arm64;
//   * FFmpeg's decoders, which take the codecs Symphonia and libopus don't,
//     run their C code (av_force_cpu_flags(0)) instead of the SSE, AVX2/FMA
//     or NEON code for the CPU;
//   * a media element's audio played at another rate with its pitch kept
//     (WSOLA time stretching) picks its blocks with the SSE sums on every
//     x86 CPU, which NEON's equal, never the AVX2 + FMA ones
//     (media/filters/wsola_internals.cc);
//   * a microphone stream's processing (echo cancellation, noise
//     suppression, gain control), wherever it runs, is asked to compute the
//     same samples on every CPU (AudioProcessingSettings::cpu_independent,
//     set in Blink's MediaStreamAudioProcessingLayout; WebRTC's
//     cpu_info::ScopedCpuIndependent);
//   * a microphone has no platform effects (PortableCaptureEffects() below):
//     its processing is always WebRTC's, never the OS's, and the page reads
//     the same processing settings and capabilities on every OS;
//   * every microphone has one format as the page sees it (48 kHz, one or
//     two channels, 10 ms; PortableCaptureParameters() below), and its
//     stream is delivered in it, converted from the device's by the
//     CPU-independent processing (PortableCaptureOutputFormat()), which also
//     down-mixes a device of another layout, such as QUAD or 5.1
//     (PortableCaptureDownmix);
//   * the device opens without the OS's processing also where the audio
//     service opens it, and the platform code keeps the OS's processing out
//     of the stream: on Linux it records from the microphone under a
//     PulseAudio or PipeWire filter source, on Windows it never takes the
//     communications category without raw mode (kPortableCaptureEffect);
//   * the playout its echo canceller hears, which the audio service mixes for
//     the output device, is mixed with the plain C code while a stream of such
//     a process listens (ScopedPortableAudioThread below);
//   * the portable audio code computes with the CPU's flush-to-zero modes off
//     and keeps subnormal floats out of its loops in software, the same way on
//     every CPU (ScopedPortableSubnormals, FlushTinyPortableSamples() below);
//   * a camera's frames carry one colour space, their samples converted to it
//     with integer arithmetic (UsePortableCameraColor() below,
//     molt_portable_color.h).
//
// libyuv's colour conversions and libvpx's encoders and decoders needed no
// change: they give the same bytes on every SIMD level of x86 and Arm
// (docs/FINGERPRINTING.md, "MoltNet: pixels and encoded bytes").
//
// Other processes, and the renderers of normal and Incognito windows, never
// set it. Any thread may ask.

#ifndef MEDIA_BASE_MOLT_PORTABLE_MEDIA_H_
#define MEDIA_BASE_MOLT_PORTABLE_MEDIA_H_

#include <cmath>
#include <cstdint>
#include <memory>

#include "base/containers/span.h"
#include "media/base/channel_layout.h"
#include "media/base/media_export.h"

namespace media {

class AudioBus;
class AudioParameters;
class ChannelMixer;
class VideoFrame;

MEDIA_EXPORT void SetPortableMediaProcess();
MEDIA_EXPORT bool IsPortableMediaProcess();

// Turns the mode on or off without the process being a MoltNet renderer.
MEDIA_EXPORT void SetPortableMediaProcessForTesting(bool portable);

// The platform effects (AudioParameters::PlatformEffectsMask) of an audio
// capture device as a portable media process sees it and opens it: none.
// Blink reads a microphone's capabilities from them (the echo cancellation
// modes and voice isolation getCapabilities() lists) and asks the OS for its
// own processing with them (system echo cancellation, noise suppression and
// gain control, voice isolation), which is the OS's code and differs between
// OSes and machines. Without them the processing is always WebRTC's, in the
// mode above. Other processes: `platform_effects`, unchanged.
MEDIA_EXPORT int PortableCaptureEffects(int platform_effects);

// The format of every microphone as a portable media process's pages see it:
// 48 kHz, stereo, a 10 ms buffer.
inline constexpr int kPortableCaptureSampleRate = 48000;
inline constexpr int kPortableCaptureChannels = 2;
inline constexpr int kPortableCaptureFramesPerBuffer = 480;

// An audio capture device's parameters as a portable media process's pages
// read them and constraints are checked against (getCapabilities()'
// sampleRate, channelCount and latency ranges, those of enumerateDevices()'
// InputDeviceInfo, and what a request may ask for): the format above with no
// platform effects (PortableCaptureEffects()) and no hardware buffer range,
// whatever the device. `device` itself is still what the stream opens with:
// its own rate and channels, which the CPU-independent processing converts
// to the stream's (PortableCaptureOutputFormat()), so that no OS or
// CPU-dependent converter sits in between. Other processes, and parameters
// that aren't valid (no device): `device` with only the effects changed as
// PortableCaptureEffects() changes them.
MEDIA_EXPORT AudioParameters
PortableCaptureParameters(const AudioParameters& device);

// The format a microphone stream of a portable media process is delivered
// in, whatever the device's (`input`, of which only the sample format is
// kept): kPortableCaptureSampleRate, stereo when the page asked for more than
// one channel (`stereo`) and mono otherwise, kPortableCaptureFramesPerBuffer
// frames (the processing's 10 ms). getSettings()' sampleRate, channelCount
// and latency are read from it.
MEDIA_EXPORT AudioParameters
PortableCaptureOutputFormat(const AudioParameters& input, bool stereo);

// How the audio service's platform code knows a microphone stream comes from
// a portable media process: the effects of the format it opens the device in
// are this bit alone (no platform effects; AudioParameters::
// PlatformEffectsMask uses none this high, and no platform code reads it
// otherwise). AudioProcessor::ComputeInputFormat() sets them for a
// CPU-independent stream (PortableCaptureStreamEffects()), in the renderer
// and in the audio service alike. Then:
//   * Linux (PulseAudio and pipewire-pulse): the stream records from the
//     microphone under a filter source (an echo canceller's, a noise filter's)
//     instead of the filter (media/audio/pulse/molt_capture_source.h);
//   * Windows: the stream never takes the communications category without
//     raw mode (PortableCaptureMayUseCommunicationsCategory() below).
inline constexpr int kPortableCaptureEffect = 1 << 30;

// The effects a CPU-independent stream opens its device with: no platform
// effects, and kPortableCaptureEffect on Linux and Windows, whose platform
// code reads it. Elsewhere none at all: Android's audio code takes any effect
// as a request for the OS's communication mode and its processing.
MEDIA_EXPORT int PortableCaptureStreamEffects();

// Windows: whether a capture stream opened with `params` may ask for the
// communications category on a device with raw mode (WASAPIAudioInputStream).
// Chromium asks for it with raw mode on devices of up to 8 channels and
// without raw mode, so with the OS's communications processing (Voice Clarity
// on Windows 11 24H2 when the device has none of its own), on devices with
// more. A portable media process's stream (kPortableCaptureEffect) takes it
// only with raw mode: on a device with more channels it stays in the default
// category, as on a device without raw mode. `channels`: the audio engine's
// channel count for the device.
inline constexpr int kPortableCaptureMaxRawChannels = 8;
MEDIA_EXPORT bool PortableCaptureMayUseCommunicationsCategory(
    const AudioParameters& params,
    int channels);

// The audio service's mixing for a portable process's echo canceller.
//
// While a ScopedPortableAudioThread(true) is alive on a thread, the audio code
// there computes as in a portable media process: vector_math::FMAC and the
// SincResampler's convolution take their plain C versions, and a
// SincResampler built on the thread computes its kernel with the portable math
// functions (gfx::molt_math::PortableSin(), PortableCos()). The audio service,
// which every profile shares, opens one around the mixing of an output device
// while the echo canceller of a microphone stream from a portable process
// listens to it (services/audio/output_device_mixer_impl.cc), and around the
// creation of that mixing's converters (services/audio/mixing_graph_impl.cc),
// so that the reference such a stream's echo canceller hears is the same on
// every CPU and OS. Scopes nest; the innermost one decides.
class MEDIA_EXPORT ScopedPortableAudioThread {
 public:
  explicit ScopedPortableAudioThread(bool portable);
  ~ScopedPortableAudioThread();
  ScopedPortableAudioThread(const ScopedPortableAudioThread&) = delete;
  ScopedPortableAudioThread& operator=(const ScopedPortableAudioThread&) =
      delete;

 private:
  const bool previous_;
};

// Whether audio computed on the calling thread takes the portable code: in a
// portable media process, or inside a ScopedPortableAudioThread(true).
MEDIA_EXPORT bool IsPortableAudioThread();

// Subnormal floats in the portable audio code.
//
// The CPU's flush-to-zero modes don't agree between architectures. x86's
// MXCSR.FTZ decides that a result is tiny after rounding it, and DAZ reads
// subnormal operands as zero; Arm's FPCR.FZ decides before rounding and
// flushes operands too. So a product whose exact value lies just below
// FLT_MIN but rounds up to it is FLT_MIN on x86 and zero on Arm, and
// cc::ScopedSubnormalFloatDisabler sets the modes on x86 only. With the
// modes off, IEEE 754 subnormal arithmetic gives the same results on both.
// Chromium turns them on around its audio loops because x86 can take tens to
// hundreds of cycles per operation on a subnormal operand or result.
//
// The portable audio code computes with the modes off instead
// (ScopedPortableSubnormals) and keeps subnormals out of its loops in
// software, the same way on every CPU: where samples enter it, every sample
// whose magnitude is below kPortableAudioFloor becomes zero
// (FlushTinyPortableSamples()). The product of two values at or above the
// floor is a normal float, since the floor's square is FLT_MIN.
inline constexpr float kPortableAudioFloor = 0x1p-63f;  // 1.08e-19, -379 dBFS

// `sample`, or a zero of its sign when its magnitude is below
// kPortableAudioFloor. Infinities and NaNs are unchanged.
inline float FlushTinyPortableSample(float sample) {
  return std::fabs(sample) < kPortableAudioFloor ? std::copysign(0.0f, sample)
                                                 : sample;
}

// The same floor for state kept in doubles (the history of Web Audio's
// IIRFilterNode): a zero of its sign below kPortableAudioFloor, so that a
// decaying state settles at zero instead of decaying on into the subnormal
// doubles. Infinities and NaNs are unchanged.
inline double FlushTinyPortableSample(double sample) {
  return std::fabs(sample) < static_cast<double>(kPortableAudioFloor)
             ? std::copysign(0.0, sample)
             : sample;
}

// FlushTinyPortableSample() on every sample of `samples`, or of every channel
// of `bus`.
MEDIA_EXPORT void FlushTinyPortableSamples(base::span<float> samples);
MEDIA_EXPORT void FlushTinyPortableSamples(AudioBus& bus);

// Turns the CPU's flush-to-zero modes off on the calling thread while it
// lives (x86: MXCSR's FTZ and DAZ bits; Arm: FPCR's or FPSCR's FZ bit), and
// puts back the mode it found when it ends, so scopes nest. The portable
// audio code opens one where Chromium's opens a
// cc::ScopedSubnormalFloatDisabler or a DenormalDisabler. Only the control
// register is touched, and only when a flush mode is on.
class MEDIA_EXPORT ScopedPortableSubnormals {
 public:
  ScopedPortableSubnormals();
  ~ScopedPortableSubnormals();
  ScopedPortableSubnormals(const ScopedPortableSubnormals&) = delete;
  ScopedPortableSubnormals& operator=(const ScopedPortableSubnormals&) =
      delete;

  // Whether a flush-to-zero mode is on on the calling thread.
  static bool FlushModeIsOn();

 private:
  uint64_t saved_ = 0;
  bool restore_ = false;
};

// Microphones of other layouts.
//
// The audio processing takes mono, stereo and discrete (unnamed) channels,
// and Chromium fails a processed stream from a device of another layout:
// some Windows microphone arrays report QUAD or 5.1, and a PulseAudio source
// of 4 or 6 channels gets those layouts too. A CPU-independent stream (a
// portable media process's, AudioProcessingSettings::cpu_independent) opens
// such a device in its own layout instead (AudioProcessor::
// ComputeInputFormat()), and its processing down-mixes the device's channels
// to the stream's, one or two, before anything else, with media's
// ChannelMixer computing the same samples on every CPU: its multiply-adds in
// their plain C versions (ScopedPortableAudioThread) and the flush-to-zero
// modes off (ScopedPortableSubnormals).

// Whether a device of `layout` is down-mixed so: a named layout other than
// mono and stereo.
MEDIA_EXPORT bool PortableCaptureNeedsDownmix(ChannelLayout layout);

class MEDIA_EXPORT PortableCaptureDownmix {
 public:
  // `device`: the format the device delivers (a layout
  // PortableCaptureNeedsDownmix() accepts); `channels`: the stream's, 1 or 2.
  PortableCaptureDownmix(const AudioParameters& device, int channels);
  ~PortableCaptureDownmix();
  PortableCaptureDownmix(const PortableCaptureDownmix&) = delete;
  PortableCaptureDownmix& operator=(const PortableCaptureDownmix&) = delete;

  int channels() const;

  // `source`, the device's channels for at most the device's buffer of
  // frames, down-mixed. The bus stays valid until the next call.
  const AudioBus& Downmix(const AudioBus& source);

 private:
  std::unique_ptr<ChannelMixer> mixer_;
  std::unique_ptr<AudioBus> output_;
};

// A camera's colour.
//
// Every frame of a MoltNet camera track carries one colour space, BT.601 at
// limited range (SMPTE 170M primaries, transfer and matrix), whatever the
// camera's (third_party/blink/renderer/modules/mediastream/moltnet_camera.h).

// Whether `frame`'s samples already are in that encoding as Chromium reads
// them (gfx::ColorSpace::ToSkYUVColorSpace()): a BT.601 matrix (BT.470BG or
// SMPTE 170M) at any range but full, or no YUV matrix at all (none, or one a
// camera's YUV doesn't come in), which Chromium reads as BT.601 at limited
// range.
MEDIA_EXPORT bool HasPortableCameraEncoding(const VideoFrame& frame);

// Gives `frame` that colour space (and no HDR metadata), its samples as they
// are.
MEDIA_EXPORT void SetPortableCameraColorSpace(VideoFrame& frame);

// Converts the visible samples of an I420 `frame` in writable memory from the
// encoding its colour space gives them (BT.709, full-range BT.601, ...) to
// BT.601 at limited range in place, with molt_portable_color.h's integer
// arithmetic, and gives it that colour space (SetPortableCameraColorSpace()).
// Returns false, changing nothing, for another frame.
MEDIA_EXPORT bool UsePortableCameraColor(VideoFrame& frame);

// Scales every plane of `dest_frame` from the same plane of `src_frame` with
// molt::PortableScalePlane() (molt_portable_scale.h): the visible regions,
// one or more interleaved samples per element (an NV12 UV plane has two, an
// ARGB plane four), 16-bit samples when the frames are deeper than 8 bits.
// Returns false, possibly after writing some planes, when the frames' planes
// don't correspond (different element sizes or bit depths).
MEDIA_EXPORT bool PortableScaleFrame(const VideoFrame& src_frame,
                                     VideoFrame& dest_frame);

}  // namespace media

#endif  // MEDIA_BASE_MOLT_PORTABLE_MEDIA_H_
