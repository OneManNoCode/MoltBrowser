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
//   getUserMedia()    UserMediaProcessor::SetupVideoInput: no
//                       pan-tilt-zoom permission -> RequestsPanTiltZoomPermission
//                     UserMediaProcessor::SelectVideoDeviceSettings: the
//                       cameras' formats, no pan, tilt or zoom
//                                              -> UseUniformCameraFormats
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
//   the grid          VideoTrackAdapter::DeliverFrameOnVideoTaskRunner,
//                     MoltCameraTickOnVideoTaskRunner
//                                              -> UniformCameraClock
//                     VideoFrameResolutionAdapter::DeliverFrame (holds the
//                       newest frame), DeliverUniformCameraTick
//                                              -> UniformCameraTickFrame
//                     VideoFrameResolutionAdapter::OnFrameDropped (the
//                       camera's own drops aren't the tracks')
//   frames the CPU    VideoTrackAdapter::DeliverFrameOnVideoTaskRunner,
//   can't read          ReadBackMoltCameraFrame (on a worker)
//                                              -> NeedsUniformCameraReadback,
//                                                 UniformCameraFrameReader
//                     VideoFrameResolutionAdapter::DeliverUniformCameraTick
//                       (a second without a frame it could read)
//                                              -> UniformCameraUnreadableFrame
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
// "none". The frames reach the tracks on a fixed grid of 30 a second
// (moltnet::UniformCameraGrid): each resolution adapter keeps the newest
// frame it converted, and each tick hands it on, again if the camera sent
// none since, with the tick's timestamp and capture time, so frame
// timestamps, requestVideoFrameCallback()'s captureTime, the track's
// statistics and what WebRTC measures follow the grid, not the camera's rate
// and jitter. A frame the CPU can't read as it is (a GPU image without a CPU
// mapping, which Chromium hands on as it is) is read back through the GPU
// first, on a worker; while a camera's frames can't be read at all, its
// tracks get black frames after a second rather than the last picture. The
// device opens in the mode UniformCameraOpenMode() picks, the
// smallest that reaches the larger entry (or the largest it has, from which
// the entries it doesn't reach are scaled up), and applyConstraints()
// reconfigures the track on the running source instead of reopening the
// device.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIASTREAM_MOLTNET_CAMERA_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIASTREAM_MOLTNET_CAMERA_H_

#include <optional>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "media/base/capture_version.h"
#include "media/capture/video_capture_types.h"
#include "third_party/blink/public/mojom/mediastream/media_devices.mojom-blink-forward.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom-blink-forward.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_camera_menu.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_track_platform.h"
#include "third_party/blink/renderer/platform/wtf/thread_safe_ref_counted.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"
#include "ui/gfx/geometry/size.h"

namespace media {
class VideoFrame;
class VideoFrameConverter;
struct VideoFrameMetadata;
class VideoFramePool;
}  // namespace media

namespace blink {

class MediaConstraints;
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
// InputDeviceInfo.getCapabilities(), with the formats above; a MoltNet camera
// also has no pan, tilt or zoom (control_support), so constraints on them
// are met or refused as for a camera without them.
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

// Whether a request with `video` constraints for a device of `type` asks the
// browser for the pan-tilt-zoom permission (StreamControls::
// request_pan_tilt_zoom_permission), whose prompt names a camera that moves:
// as Chromium (pan, tilt or zoom present and not false), but never for a
// MoltNet camera, which has none of them.
MODULES_EXPORT bool RequestsPanTiltZoomPermission(
    const MediaConstraints& video,
    mojom::blink::MediaStreamType type);

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
// timestamp, and of its metadata only the capture version and the
// orientation (UniformCameraFrameMetadata()); a wrapper of `frame` when it
// already is one. An error for a frame the CPU can't read (read it back
// first: NeedsUniformCameraReadback()) or the converter can't take.
MODULES_EXPORT base::expected<scoped_refptr<media::VideoFrame>,
                              media::VideoCaptureFrameDropReason>
AdaptUniformCameraFrame(scoped_refptr<media::VideoFrame> frame,
                        const gfx::Size& size,
                        media::VideoFramePool& pool,
                        media::VideoFrameConverter& converter);

// Of a camera frame's `metadata`, what a MoltNet camera track's frames carry:
// the capture version (which orders frames for the track) and the
// orientation (the rotation of the device's picture), none of the camera's
// times, rate, effects (background blur), captured surface or GPU details.
MODULES_EXPORT media::VideoFrameMetadata UniformCameraFrameMetadata(
    const media::VideoFrameMetadata& metadata);

// Whether the CPU can't read `frame` as it is: a GPU image without a CPU
// mapping (VideoFrame::HasSharedImage() without HasMappableSharedImage()),
// as some cameras' frames are (Android's zero-copy capture, a texture
// virtual capture device), which Chromium hands on as they are. A MoltNet
// camera's VideoTrackAdapter reads such a frame back into memory for its
// tracks (UniformCameraFrameReader) before AdaptUniformCameraFrame().
MODULES_EXPORT bool NeedsUniformCameraReadback(const media::VideoFrame& frame);

// Reads a frame NeedsUniformCameraReadback() into memory. VideoTrackAdapter
// keeps one per video source and runs it on a worker, one frame at a time,
// never on its video task runner.
class MODULES_EXPORT UniformCameraFrameReader
    : public ThreadSafeRefCounted<UniformCameraFrameReader> {
 public:
  // A reader through the GPU context a renderer's media work uses, as
  // Chromium's WebRTC encoders read such frames (WebRtcVideoFrameAdapter::
  // SharedResources::ConstructVideoFrameFromTexture()): it blocks on the GPU
  // process.
  static scoped_refptr<UniformCameraFrameReader> Create();

  UniformCameraFrameReader(const UniformCameraFrameReader&) = delete;
  UniformCameraFrameReader& operator=(const UniformCameraFrameReader&) =
      delete;

  // `frame` in memory, in the format the GPU reads it to (I420, NV12 or
  // RGB), or nullptr: no GPU context yet (it is requested from the main
  // thread on the first read, and again after a lost one) or a read that
  // failed.
  virtual scoped_refptr<media::VideoFrame> Read(
      scoped_refptr<media::VideoFrame> frame) = 0;

 protected:
  UniformCameraFrameReader() = default;
  virtual ~UniformCameraFrameReader() = default;

 private:
  friend class ThreadSafeRefCounted<UniformCameraFrameReader>;
};

// The frame a MoltNet camera track's ticks hand on once its camera's frames
// couldn't be read for a second of the grid (no frame read back, or none the
// converter took): black, of `size`, I420 in the cameras' colour space, with
// `capture_version`, rather than the last frame read (a picture that would
// look live) or none, so that the failure shows. nullptr if it can't be
// made.
MODULES_EXPORT scoped_refptr<media::VideoFrame> UniformCameraUnreadableFrame(
    const gfx::Size& size,
    const media::CaptureVersion& capture_version);

// The grid a MoltNet camera's frames reach its tracks on
// (moltnet::UniformCameraGrid) on base::TimeTicks. VideoTrackAdapter keeps
// one per video source, on its video task runner, and runs each tick when it
// is due; its resolution adapters of MoltNet camera tracks hold their newest
// frame until then.
class MODULES_EXPORT UniformCameraClock {
 public:
  struct Tick {
    // The frames' timestamp: the tick's time after tick 0.
    base::TimeDelta timestamp;
    // When the tick was due: the frames' capture and reference time.
    base::TimeTicks time;
  };

  UniformCameraClock();
  ~UniformCameraClock();
  UniformCameraClock(const UniformCameraClock&) = delete;
  UniformCameraClock& operator=(const UniformCameraClock&) = delete;

  // A frame of the source arrived at `now`: when the pending tick is due if
  // the grid starts or resumes, nullopt while one is pending.
  std::optional<base::TimeTicks> OnSourceFrame(base::TimeTicks now);
  // Runs the pending tick at `now`: the tick to hand on, after which the
  // next is pending; nullopt when none is, or the grid stopped (no frame for
  // a second).
  std::optional<Tick> OnTick(base::TimeTicks now);
  // When the pending tick is due.
  base::TimeTicks NextTickDue() const;

  // Tests run the ticks themselves (VideoTrackAdapter::
  // MoltCameraTickForTesting()), on the grid's own time: each tick runs when
  // due, and a frame arrives just before the pending tick.
  void set_manual_ticks_for_testing() { manual_ticks_for_testing_ = true; }
  bool manual_ticks_for_testing() const { return manual_ticks_for_testing_; }

 private:
  moltnet::UniformCameraGrid grid_;
  bool manual_ticks_for_testing_ = false;
};

// The frame a MoltNet camera track gets at a tick of the grid from `held`,
// the newest frame AdaptUniformCameraFrame() gave its resolution adapter: a
// wrapper of it with the tick's timestamp, the tick's time as its capture
// begin time (requestVideoFrameCallback()'s captureTime) and reference time
// (WebRTC's capture time), the menu's frame rate, and of the camera's own
// metadata only the capture version and the orientation: none of its times,
// rate, effects or capture details. nullptr if `held` can't be wrapped.
MODULES_EXPORT scoped_refptr<media::VideoFrame> UniformCameraTickFrame(
    const scoped_refptr<media::VideoFrame>& held,
    const UniformCameraClock::Tick& tick);

// getSettings() of a track with `adapter` settings: a MoltNet camera
// track's frame rate is the menu's, or the lower one it asked for, and its
// resize mode the one selected. Other tracks' settings are left as they are.
MODULES_EXPORT void UseUniformCameraTrackSettings(
    const VideoTrackAdapterSettings& adapter,
    MediaStreamTrackPlatform::Settings& settings);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIASTREAM_MOLTNET_CAMERA_H_
