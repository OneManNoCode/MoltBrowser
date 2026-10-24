// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the encoders MoltNet windows
// (kUniform) record and encode with. Everything a MoltNet page can observe
// of MediaRecorder (isTypeSupported(), the default codec and mimeType, the
// encoder, the bitstream) depends on the build and the page's input only.
//
// At 156 MediaRecorder's video track recorder asks the GPU process which
// video encoders the machine has (GpuVideoAcceleratorFactories), and that
// answer picks the default codec (VideoTrackRecorderImpl::GetPreferredCodec:
// the first codec with a hardware encoder, else VP8 for WebM and VP9 for
// MP4) and whether a recording is encoded by the GPU's encoder (or, for
// H.264 and HEVC on macOS, the operating system's software encoder) or by
// the build's own (libvpx, libaom; OpenH264 where the build has it). The
// encoder's identity shows in the bitstream, its timing and the profiles
// it takes. isTypeSupported() asks media::IsEncoderSupportedVideoType(),
// which also counts the GPU's encoders for profiles the build can't encode
// itself, and AAC is the platform's encoder. And the build's own encoders
// size their threads by the machine's core count, which libaom also uses
// to choose AV1's tile layout, part of the bitstream.
//
// Upstream hooks (each marked "MoltBrowser:"):
//
//   video_track_recorder.cc   NotifyEncoderSupportKnown, GetVEASupportedProfiles
//                               (the GPU's encoders: GetPreferredCodec,
//                               CanUseAcceleratedEncoder, InitializeEncoder,
//                               MediaRecorderHandler::EncodingInfo)
//                                          -> MediaRecorderUsesPlatformEncoders
//   media_recorder_handler.cc MediaRecorderHandler::CanSupportMimeTypeForCodec
//                               (isTypeSupported(), the constructor; the
//                               H.264, AV1 and HEVC profile checks and AAC)
//                                          -> IsMediaRecorderVideoTypeSupported,
//                                             MediaRecorderUsesPlatformEncoders
//   media/base/video_encoder.cc  GetNumberOfThreadsForSoftwareEncoding
//                               (libvpx's and libaom's threads, libaom's
//                               tiles: MediaRecorder and WebCodecs)
//                                          <- PinUniformSoftwareEncoding()
//                                             (from Configure(kUniform))
//
// So in MoltNet MediaRecorder never touches the GPU's encoders: the default
// codec is VP8 (WebM, Matroska) or VP9 (MP4) on every machine, every
// recording is encoded by libvpx or libaom, isTypeSupported() is true
// exactly for what the build encodes itself, and the software encoders run
// as on a machine with kUniformHardwareConcurrency (4) cores, the count
// navigator.hardwareConcurrency reports. Audio is Opus (libopus) or PCM
// everywhere; AAC, which only the platform encodes, is unsupported.
// WebCodecs already uses only the build's codecs in MoltNet
// (moltnet_media.h); the thread count applies to it too.
//
// Which contexts: the whole MoltNet renderer process (MediaRecorder exists
// in windows only; WebCodecs in workers too). Normal and Incognito windows
// are unchanged.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_ENCODERS_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_ENCODERS_H_

#include "third_party/blink/renderer/core/core_export.h"

namespace media {
struct VideoType;
}  // namespace media

namespace blink::molt_fingerprinting {

// Whether MediaRecorder may use the platform's encoders: the GPU's video
// encoders (and through them the operating system's), and the platform's
// AAC encoder. False in MoltNet. Any thread.
CORE_EXPORT bool MediaRecorderUsesPlatformEncoders();

// Whether MediaRecorder can encode the video `type` (codec and profile):
// media::IsEncoderSupportedVideoType() outside MoltNet; in MoltNet, whether
// the build has its own encoder for it (media::IsEncoderBuiltInVideoType()).
CORE_EXPORT bool IsMediaRecorderVideoTypeSupported(const media::VideoType& type);

// Configure(kUniform): software video encoders in this process size their
// threads (and libaom its tiles) for kUniformHardwareConcurrency cores.
void PinUniformSoftwareEncoding();

// Makes the functions above behave as in kUniform without configuring the
// process for it.
CORE_EXPORT void SetMoltNetEncodersForTesting(bool uniform);

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_ENCODERS_H_
