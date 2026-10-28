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
//   * the playout its echo canceller hears, which the audio service mixes for
//     the output device, is mixed with the plain C code while a stream of such
//     a process listens (ScopedPortableAudioThread below).
//
// libyuv's colour conversions and libvpx's encoders and decoders needed no
// change: they give the same bytes on every SIMD level of x86 and Arm
// (docs/FINGERPRINTING.md, "MoltNet: pixels and encoded bytes").
//
// Other processes, and the renderers of normal and Incognito windows, never
// set it. Any thread may ask.

#ifndef MEDIA_BASE_MOLT_PORTABLE_MEDIA_H_
#define MEDIA_BASE_MOLT_PORTABLE_MEDIA_H_

#include "media/base/media_export.h"

namespace media {

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
