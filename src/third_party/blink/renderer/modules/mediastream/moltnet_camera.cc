// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/modules/mediastream/moltnet_camera.h"

#include <algorithm>
#include <utility>

#include "media/base/video_frame.h"
#include "media/base/video_frame_converter.h"
#include "media/base/video_frame_pool.h"
#include "media/base/video_types.h"
#include "media/base/video_util.h"
#include "third_party/blink/public/mojom/mediastream/media_devices.mojom-blink.h"
#include "third_party/blink/public/platform/modules/mediastream/web_media_stream_track.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_camera_menu.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media.h"
#include "third_party/blink/renderer/modules/mediastream/media_stream_constraints_util.h"
#include "third_party/blink/renderer/modules/mediastream/media_stream_constraints_util_video_device.h"
#include "third_party/blink/renderer/modules/mediastream/video_track_adapter_settings.h"
#include "ui/gfx/geometry/rect.h"

namespace blink::molt_fingerprinting {

namespace {

using moltnet::CameraMode;

template <typename Formats>
Vector<CameraMode> ToModes(const Formats& formats) {
  Vector<CameraMode> modes;
  modes.reserve(static_cast<wtf_size_t>(formats.size()));
  for (const media::VideoCaptureFormat& format : formats) {
    modes.push_back(CameraMode{format.frame_size.width(),
                               format.frame_size.height(), format.frame_rate});
  }
  return modes;
}

template <typename Formats>
Formats MenuFormats(const Formats& formats) {
  Formats menu;
  for (const CameraMode& entry : UniformCameraMenu(ToModes(formats))) {
    menu.push_back(media::VideoCaptureFormat(
        gfx::Size(entry.width, entry.height), entry.frame_rate,
        media::PIXEL_FORMAT_I420));
  }
  return menu;
}

// A camera has no captured surface: the size and scale a screen capture's
// frames carry for getSettings() and getCapabilities() (the Captured Surface
// Resolution API), which Chromium's fake camera sets to its own mode, aren't
// kept.
void DropCapturedSurface(media::VideoFrame& frame) {
  frame.metadata().source_size.reset();
  frame.metadata().device_scale_factor.reset();
}

// The largest centred part of `bounds` with the aspect ratio of `size`, with
// an even origin and even sides so that the chroma of subsampled formats
// stays aligned (as VideoTrackAdapter crops).
gfx::Rect EvenLetterboxRegion(const gfx::Rect& bounds, const gfx::Size& size) {
  gfx::Rect region = media::ComputeLetterboxRegion(bounds, size);
  region.set_x(region.x() & ~1);
  region.set_y(region.y() & ~1);
  const bool width_has_slack = region.right() < bounds.right();
  region.set_width((region.width() + width_has_slack) & ~1);
  const bool height_has_slack = region.bottom() < bounds.bottom();
  region.set_height((region.height() + height_has_slack) & ~1);
  return region;
}

}  // namespace

bool UsesUniformCamera(mojom::blink::MediaStreamType type) {
  return type == mojom::blink::MediaStreamType::DEVICE_VIDEO_CAPTURE &&
         UsesUniformMediaDevices();
}

Vector<media::VideoCaptureFormat> UniformCameraFormats(
    const Vector<media::VideoCaptureFormat>& formats,
    mojom::blink::MediaStreamType type) {
  return UsesUniformCamera(type) ? MenuFormats(formats) : formats;
}

media::VideoCaptureFormats UniformCameraFormats(
    const media::VideoCaptureFormats& formats,
    mojom::blink::MediaStreamType type) {
  return UsesUniformCamera(type) ? MenuFormats(formats) : formats;
}

void UseUniformCameraFormats(VideoDeviceCaptureCapabilities& capabilities) {
  for (VideoInputDeviceCapabilities& device :
       capabilities.device_capabilities) {
    device.formats = UniformCameraFormats(
        device.formats, mojom::blink::MediaStreamType::DEVICE_VIDEO_CAPTURE);
  }
}

void UseUniformCameraFormats(
    mojom::blink::VideoInputDeviceCapabilities& capabilities) {
  capabilities.formats =
      UniformCameraFormats(capabilities.formats,
                           mojom::blink::MediaStreamType::DEVICE_VIDEO_CAPTURE);
}

VideoCaptureSettings UniformCameraSettings(const VideoCaptureSettings& settings,
                                           mojom::blink::MediaStreamType type) {
  if (!UsesUniformCamera(type) || !settings.HasValue()) {
    return settings;
  }
  const VideoTrackAdapterSettings& selected =
      settings.track_adapter_settings();
  const double menu_rate = moltnet::kUniformCameraFrameRate;
  VideoTrackAdapterSettings adapter(
      selected.target_size().value_or(settings.Format().frame_size),
      selected.min_aspect_ratio(), selected.max_aspect_ratio(),
      std::min(selected.max_frame_rate().value_or(menu_rate), menu_rate));
  adapter.set_molt_uniform_camera(
      /*reports_no_resize=*/!selected.target_size().has_value());
  return VideoCaptureSettings(
      settings.device_id(), settings.capture_params(),
      settings.noise_reduction(), adapter, settings.min_frame_rate(),
      settings.max_frame_rate(), settings.image_capture_device_settings());
}

void UseUniformCameraSettings(Vector<VideoCaptureSettings>& settings) {
  for (VideoCaptureSettings& camera : settings) {
    camera = UniformCameraSettings(
        camera, mojom::blink::MediaStreamType::DEVICE_VIDEO_CAPTURE);
  }
}

media::VideoCaptureParams UniformCameraCaptureParams(
    const media::VideoCaptureParams& params,
    const Vector<media::VideoCaptureFormat>& native_formats,
    mojom::blink::MediaStreamType type) {
  if (!UsesUniformCamera(type)) {
    return params;
  }
  const std::optional<UniformCameraOpening> open =
      UniformCameraOpeningFor(ToModes(native_formats));
  if (!open) {
    return params;
  }
  media::VideoCaptureParams uniform = params;
  uniform.requested_format = native_formats[open->mode];
  uniform.requested_format.frame_rate = open->frame_rate;
  return uniform;
}

std::optional<gfx::Size> UniformCameraFrameSize(
    bool is_rotated,
    const VideoTrackAdapterSettings& adapter) {
  if (!adapter.molt_uniform_camera() || !adapter.target_size()) {
    return std::nullopt;
  }
  const gfx::Size& size = *adapter.target_size();
  return is_rotated ? gfx::Size(size.height(), size.width()) : size;
}

base::expected<scoped_refptr<media::VideoFrame>,
               media::VideoCaptureFrameDropReason>
AdaptUniformCameraFrame(scoped_refptr<media::VideoFrame> frame,
                        const gfx::Size& size,
                        media::VideoFramePool& pool,
                        media::VideoFrameConverter& converter) {
  using DropReason = media::VideoCaptureFrameDropReason;
  if (size.IsEmpty()) {
    return base::unexpected(
        DropReason::kResolutionAdapterCannotCreateConvertFrame);
  }
  // Already a frame of exactly `size`, I420, in memory: as it is.
  if (!frame->HasSharedImage() && frame->HasDirectCpuAccess() &&
      frame->format() == media::PIXEL_FORMAT_I420 &&
      frame->coded_size() == size && frame->visible_rect() == gfx::Rect(size) &&
      frame->natural_size() == size) {
    if (!frame->metadata().source_size &&
        !frame->metadata().device_scale_factor) {
      return frame;
    }
    scoped_refptr<media::VideoFrame> wrapped =
        media::VideoFrame::WrapVideoFrame(frame, frame->format(),
                                          frame->visible_rect(),
                                          frame->natural_size());
    if (!wrapped) {
      return base::unexpected(
          DropReason::kResolutionAdapterWrappingFrameForCroppingFailed);
    }
    DropCapturedSurface(*wrapped);
    return wrapped;
  }
  if (frame->HasSharedImage()) {
    if (!frame->HasMappableSharedImage()) {
      return base::unexpected(DropReason::kResolutionAdapterFrameIsNotMappable);
    }
    frame = media::ConvertToMemoryMappedFrame(std::move(frame));
  }
  if (!frame || !frame->HasDirectCpuAccess()) {
    return base::unexpected(DropReason::kResolutionAdapterFrameIsNotMappable);
  }

  const gfx::Rect crop = EvenLetterboxRegion(frame->visible_rect(), size);
  if (crop.IsEmpty()) {
    return base::unexpected(
        DropReason::kResolutionAdapterWrappingFrameForCroppingFailed);
  }
  scoped_refptr<media::VideoFrame> cropped =
      crop == frame->visible_rect()
          ? frame
          : media::VideoFrame::WrapVideoFrame(frame, frame->format(), crop,
                                              crop.size());
  if (!cropped) {
    return base::unexpected(
        DropReason::kResolutionAdapterWrappingFrameForCroppingFailed);
  }

  scoped_refptr<media::VideoFrame> uniform =
      pool.CreateFrame(media::PIXEL_FORMAT_I420, size, gfx::Rect(size), size,
                       frame->timestamp());
  if (!uniform) {
    return base::unexpected(
        DropReason::kResolutionAdapterCannotCreateConvertFrame);
  }
  uniform->metadata().MergeMetadataFrom(frame->metadata());
  uniform->metadata().ClearTextureFrameMetadata();
  DropCapturedSurface(*uniform);
  if (!converter.ConvertAndScale(*cropped, *uniform).is_ok()) {
    return base::unexpected(
        DropReason::kResolutionAdapterConvertAndScaleFailed);
  }
  return uniform;
}

void UseUniformCameraTrackSettings(const VideoTrackAdapterSettings& adapter,
                                   MediaStreamTrackPlatform::Settings& settings) {
  if (!adapter.molt_uniform_camera()) {
    return;
  }
  const double menu_rate = moltnet::kUniformCameraFrameRate;
  settings.frame_rate =
      std::min(adapter.max_frame_rate().value_or(menu_rate), menu_rate);
  settings.resize_mode = String(adapter.molt_reports_no_resize()
                                    ? WebMediaStreamTrack::kResizeModeNone
                                    : WebMediaStreamTrack::kResizeModeRescale);
}

}  // namespace blink::molt_fingerprinting
