// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "media/base/molt_portable_media.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "base/containers/span.h"
#include "build/build_config.h"
#include "media/base/audio_parameters.h"
#include "media/base/molt_portable_scale.h"
#include "media/base/video_frame.h"
#include "media/media_buildflags.h"

#if BUILDFLAG(ENABLE_FFMPEG)
extern "C" {
#include <libavutil/cpu.h>
}
#endif

// MoltBrowser's hook in third_party/opus/src/celt/celt.c.
extern "C" void opus_moltbrowser_set_portable(int portable);

namespace media {

namespace {

constinit std::atomic<bool> g_portable_media{false};

// Whether this thread is inside a ScopedPortableAudioThread(true).
constinit thread_local bool t_portable_audio = false;

void SetPortable(bool portable) {
  g_portable_media.store(portable, std::memory_order_relaxed);
  opus_moltbrowser_set_portable(portable ? 1 : 0);
#if BUILDFLAG(ENABLE_FFMPEG)
  // No CPU flags: FFmpeg's decoders use their C code. -1 detects the CPU's
  // flags again, as at start-up.
  av_force_cpu_flags(portable ? 0 : -1);
#endif
}

}  // namespace

void SetPortableMediaProcess() {
  SetPortable(true);
}

bool IsPortableMediaProcess() {
  return g_portable_media.load(std::memory_order_relaxed);
}

void SetPortableMediaProcessForTesting(bool portable) {
  SetPortable(portable);
}

int PortableCaptureEffects(int platform_effects) {
  return IsPortableMediaProcess() ? AudioParameters::NO_EFFECTS
                                  : platform_effects;
}

ScopedPortableAudioThread::ScopedPortableAudioThread(bool portable)
    : previous_(t_portable_audio) {
  t_portable_audio = portable;
}

ScopedPortableAudioThread::~ScopedPortableAudioThread() {
  t_portable_audio = previous_;
}

bool IsPortableAudioThread() {
  return t_portable_audio || IsPortableMediaProcess();
}

bool PortableScaleFrame(const VideoFrame& src_frame, VideoFrame& dest_frame) {
  const VideoPixelFormat src_format = src_frame.format();
  const VideoPixelFormat dest_format = dest_frame.format();
  const size_t sample_bytes = src_frame.BitDepth() > 8 ? 2 : 1;
  if ((dest_frame.BitDepth() > 8 ? 2u : 1u) != sample_bytes) {
    return false;
  }
  for (size_t plane = 0; plane < VideoFrame::NumPlanes(dest_format); ++plane) {
    if (!VideoFrame::IsValidPlane(src_format, plane)) {
      return false;
    }
    const size_t element_bytes =
        static_cast<size_t>(VideoFrame::BytesPerElement(dest_format, plane));
    if (static_cast<size_t>(VideoFrame::BytesPerElement(src_format, plane)) !=
            element_bytes ||
        element_bytes % sample_bytes != 0) {
      return false;
    }
    const int channels = static_cast<int>(element_bytes / sample_bytes);
    const size_t src_stride = src_frame.stride(plane) / sample_bytes;
    const size_t dest_stride = dest_frame.stride(plane) / sample_bytes;
    bool ok;
    if (sample_bytes == 1) {
      ok = molt::PortableScalePlane(
          src_frame.GetVisiblePlaneData(plane), src_stride,
          src_frame.GetVisibleColumns(plane), src_frame.GetVisibleRows(plane),
          dest_frame.GetWritableVisiblePlaneData(plane), dest_stride,
          dest_frame.GetVisibleColumns(plane),
          dest_frame.GetVisibleRows(plane), channels);
    } else {
      ok = molt::PortableScalePlane16(
          base::subtle::reinterpret_span<const uint16_t>(
              src_frame.GetVisiblePlaneData(plane)),
          src_stride, src_frame.GetVisibleColumns(plane),
          src_frame.GetVisibleRows(plane),
          base::subtle::reinterpret_span<uint16_t>(
              dest_frame.GetWritableVisiblePlaneData(plane)),
          dest_stride, dest_frame.GetVisibleColumns(plane),
          dest_frame.GetVisibleRows(plane), channels);
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

}  // namespace media
