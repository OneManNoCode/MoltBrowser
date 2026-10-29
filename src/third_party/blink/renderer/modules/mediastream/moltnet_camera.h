// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: a MoltNet page's cameras show the
// formats of one menu, whatever modes the device has (the rules:
// core/molt_fingerprinting/moltnet_camera_values.h). Normal and Incognito
// windows, and capture of screens, windows and tabs, are unchanged.
//
// Upstream hooks (each marked "MoltBrowser:"):
//
//   getUserMedia()    UserMediaProcessor::SelectVideoDeviceSettings: the
//                       cameras' formats      -> UseUniformCameraFormats
//                       the selected settings  -> UniformCameraSettings,
//                                                 UseUniformCameraSettings
//                     UserMediaProcessor::InitializeVideoSourceObject: the
//                       track's capabilities   -> UniformCameraFormats
//                     UserMediaProcessor::CreateVideoSource: the mode the
//                       device opens in        -> UniformCameraCaptureParams
//   applyConstraints() ApplyConstraintsProcessor::ProcessVideoDeviceRequest:
//                       the source isn't restarted -> UsesUniformCamera
//                     ApplyConstraintsProcessor::SelectVideoDeviceSettings:
//                       as getUserMedia()      -> UniformCameraFormats,
//                                                 UniformCameraSettings
//   frames            VideoTrackAdapter::CalculateDesiredSize
//                                              -> UniformCameraFrameSize
//                     VideoFrameResolutionAdapter::AdaptFrameResolution
//                                              -> AdaptUniformCameraFrame
//   getSettings()     MediaStreamVideoTrack::GetSettings
//                                              -> UseUniformCameraTrackSettings
//   enumerateDevices() MediaDevices::DevicesEnumerated (InputDeviceInfo.
//                       getCapabilities())     -> UseUniformCameraFormats
//
// Chromium's constraints algorithm runs on the menu entries, every camera's
// whatever its modes, as if they were its modes, so the capabilities, the
// settings and the constraints a page can or can't satisfy are the menu's. The selected
// settings then mark the track's VideoTrackAdapterSettings
// (molt_uniform_camera()): its frames are cropped to the aspect ratio of the
// track's size and scaled to exactly that size, into new I420 frames, where
// Chromium would hand the camera's own frame on with a smaller visible part
// (which a VideoFrame from MediaStreamTrackProcessor shows as codedWidth and
// visibleRect) and would never scale up. A track whose page asked for
// resizeMode "none" gets the entry's own size and getSettings() reports
// "none". The device opens in the mode UniformCameraOpenMode() picks, the
// smallest that reaches the larger entry (or the largest it has, from which
// the entries it doesn't reach are scaled up), and applyConstraints()
// reconfigures the track on the running source instead of reopening the
// device.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIASTREAM_MOLTNET_CAMERA_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIASTREAM_MOLTNET_CAMERA_H_

#include <optional>

#include "base/memory/scoped_refptr.h"
#include "base/types/expected.h"
#include "media/capture/video_capture_types.h"
#include "third_party/blink/public/mojom/mediastream/media_devices.mojom-blink-forward.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom-blink-forward.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_track_platform.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"
#include "ui/gfx/geometry/size.h"

namespace media {
class VideoFrame;
class VideoFrameConverter;
class VideoFramePool;
}  // namespace media

namespace blink {

class VideoCaptureSettings;
class VideoTrackAdapterSettings;
struct VideoDeviceCaptureCapabilities;

namespace molt_fingerprinting {

// Whether a capture device of `type` shows MoltNet's menu: a camera
// (DEVICE_VIDEO_CAPTURE) in MoltNet.
MODULES_EXPORT bool UsesUniformCamera(mojom::blink::MediaStreamType type);

// The formats a page sees of a capture device of `type` whose modes are
// `formats`: a MoltNet camera's are the menu entries
// (moltnet::ShownUniformCameraModes(): all of them), I420, none for a device
// without a mode; other devices' are `formats`. They don't depend on which
// of the camera's modes `formats` lists (all of them, or the one it runs in
// when another window has it open).
MODULES_EXPORT Vector<media::VideoCaptureFormat> UniformCameraFormats(
    const Vector<media::VideoCaptureFormat>& formats,
    mojom::blink::MediaStreamType type);
MODULES_EXPORT media::VideoCaptureFormats UniformCameraFormats(
    const media::VideoCaptureFormats& formats,
    mojom::blink::MediaStreamType type);

// getUserMedia()'s cameras, and an enumerateDevices() camera's
// InputDeviceInfo.getCapabilities(), with the formats above.
MODULES_EXPORT void UseUniformCameraFormats(
    VideoDeviceCaptureCapabilities& capabilities);
MODULES_EXPORT void UseUniformCameraFormats(
    mojom::blink::VideoInputDeviceCapabilities& capabilities);

// The settings for a track of a device of `type`, from Chromium's selection
// among the formats above (`settings`): a MoltNet camera's track adapter
// produces frames of exactly the size selected, or of the selected entry's
// own for resizeMode "none", at no more than the menu's rate, and its
// getSettings() reports the resize mode selected. Other devices', and a
// selection that failed, are `settings`.
MODULES_EXPORT VideoCaptureSettings
UniformCameraSettings(const VideoCaptureSettings& settings,
                      mojom::blink::MediaStreamType type);
// The same for each of a camera request's eligible settings (one per
// camera, getUserMedia()'s deferred selection).
MODULES_EXPORT void UseUniformCameraSettings(
    Vector<VideoCaptureSettings>& settings);

// The capture parameters a source of a device of `type` opens it with:
// a MoltNet camera opens in its mode in `native_formats` that reaches the
// larger entry, or its largest (moltnet::UniformCameraOpenMode()), at no
// more than the menu's rate, whatever entry `params` (the selection's) asks
// for. Other devices', or a camera without a mode, `params`.
MODULES_EXPORT media::VideoCaptureParams UniformCameraCaptureParams(
    const media::VideoCaptureParams& params,
    const Vector<media::VideoCaptureFormat>& native_formats,
    mojom::blink::MediaStreamType type);

// The size of the frames of a track with `adapter` settings: a MoltNet
// camera track's are its target size exactly (rotated with the device);
// nullopt for other tracks.
MODULES_EXPORT std::optional<gfx::Size> UniformCameraFrameSize(
    bool is_rotated,
    const VideoTrackAdapterSettings& adapter);

// A MoltNet camera track's frame of `size` (UniformCameraFrameSize()) from
// the camera's `frame`: cropped to the aspect ratio of `size` (the largest
// centred part) and scaled to it, up or down, by `converter` into an I420
// frame of `pool` that holds exactly that, its samples converted to BT.601
// at limited range (media/base/molt_portable_color.h) where the camera's
// encoding differs, with that colour space (SMPTE 170M), the camera frame's
// timestamp and metadata but for a captured surface's size and scale (which
// Chromium's fake camera sets to its own mode); a wrapper of `frame` when it
// already is one. An error for a frame the CPU can't read or the converter
// can't take.
MODULES_EXPORT base::expected<scoped_refptr<media::VideoFrame>,
                              media::VideoCaptureFrameDropReason>
AdaptUniformCameraFrame(scoped_refptr<media::VideoFrame> frame,
                        const gfx::Size& size,
                        media::VideoFramePool& pool,
                        media::VideoFrameConverter& converter);

// getSettings() of a track with `adapter` settings: a MoltNet camera
// track's frame rate is the menu's, or the lower one it asked for, and its
// resize mode the one selected. Other tracks' settings are left as they are.
MODULES_EXPORT void UseUniformCameraTrackSettings(
    const VideoTrackAdapterSettings& adapter,
    MediaStreamTrackPlatform::Settings& settings);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIASTREAM_MOLTNET_CAMERA_H_
