// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the media pixels and bytes MoltNet
// windows (kUniform) read back. What a MoltNet page reads of a video frame
// (drawImage(), getImageData(), VideoFrame.copyTo(), texImage2D(),
// importExternalTexture()), of a WebCodecs or MediaRecorder encoder's output
// and of a decoded audio clip it re-encodes is computed the same way on every
// machine of an OS, whatever its CPU's vector instructions, its GPU or its
// video decoder.
//
// The work is done in //media and libopus, in a process-wide portable mode
// (media/base/molt_portable_media.h lists every hook):
//
//   libopus                    C kernels only (no SSE/AVX2/NEON kernels, no
//                                approximate-reciprocal pulse search)
//   DefaultDecoderFactory      no hardware video decoders: <video> and MSE
//                                are decoded by the build's software
//                                decoders, like WebCodecs already is
//                                (moltnet_media.h)
//   MappableSharedImageVideoFramePool
//                              decoded frames stay in CPU memory
//   PaintCanvasVideoRenderer   YUV frames are converted to RGB by libyuv on
//                                the CPU, also for accelerated canvases,
//                                WebGL and WebGPU
//   VideoFrameConverter        encoders' filtered scaling uses an integer
//                                scaler (molt_portable_scale.h)
//   SincResampler, FMAC        the C convolution and multiply-add
//
// libyuv's colour conversions and libvpx needed no change: they give the
// same bytes on every SIMD level of x86 and Arm (docs/FINGERPRINTING.md,
// "MoltNet: pixels and encoded bytes").
//
// Which contexts: the whole MoltNet renderer process, windows and workers.
// Normal and Incognito windows are unchanged.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_PIXELS_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_PIXELS_H_

#include "third_party/blink/renderer/core/core_export.h"

namespace blink::molt_fingerprinting {

// Configure(kUniform): media in this process decodes, converts, scales and
// encodes the same way on every machine of an OS. Before any media object
// exists; for the life of the process.
CORE_EXPORT void PinUniformMediaPixels();

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_PIXELS_H_
