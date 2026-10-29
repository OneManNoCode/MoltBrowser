// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: a MoltNet camera track's
// ImageCapture answers as a camera without photo or image controls, whatever
// the device. That covers the ImageCapture every camera track makes for
// itself (getCapabilities(), getSettings(), getConstraints() and
// applyConstraints() of pan, tilt, zoom, exposure, focus, white balance,
// torch and the rest) and a page's own (getPhotoCapabilities(),
// getPhotoSettings(), takePhoto(), grabFrame()).
//
// ImageCapture asks a media::mojom::ImageCapture service for the camera's
// photo state, sets its options and takes its photos. Chromium's service is
// the browser's, which asks the device. A MoltNet camera track's is
// UniformImageCapture (BindUniformImageCapture()), in the renderer, which
// answers as a camera without controls whose photos are its frames:
//
//   * GetPhotoState(): no control (every range empty, no modes, no torch, no
//     points of interest), no red-eye reduction, no fill light, and photos
//     of the track's own size (UniformPhotoState()). ImageCapture's own code
//     then gives the track no capability or setting of a control, refuses a
//     required constraint on one with OverconstrainedError and ignores the
//     others, as for any camera without them; getPhotoCapabilities() gives
//     redEyeReduction "never" and imageWidth and imageHeight of the track's
//     size, getPhotoSettings() that size.
//   * SetPhotoOptions(): accepted, as nothing is to be set, but refused like
//     the browser refuses it while the page is hidden, and for pan, tilt or
//     zoom (no permission for them).
//   * TakePhoto(): the track's next frame (a MoltNet camera frame: the
//     track's size, BT.601, on the grid) as a JPEG of quality 90
//     (EncodeUniformCameraPhoto()), encoded off the main thread; nothing
//     (the "platform error" Chromium gives for a failing device) when no
//     frame comes within 2 s.
//
// grabFrame() needs nothing: it already returns the track's own frame. The
// pan-tilt-zoom permission is never asked for (UserMediaProcessor::
// SetupVideoInput(), moltnet_camera.h) nor used: such an ImageCapture
// starts without it, whatever the browser granted.
//
// Upstream hook (marked "MoltBrowser:"): ImageCapture::ImageCapture binds its
// service here for a MoltNet camera track (UsesUniformImageCapture()).

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_IMAGECAPTURE_MOLTNET_IMAGE_CAPTURE_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_IMAGECAPTURE_MOLTNET_IMAGE_CAPTURE_H_

#include <cstdint>

#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "media/capture/mojom/image_capture.mojom-blink.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"
#include "ui/gfx/geometry/size.h"

namespace media {
class VideoFrame;
}  // namespace media

namespace blink {

class ExecutionContext;
class MediaStreamTrack;

namespace molt_fingerprinting {

// How long takePhoto() waits for the track's next frame (as grabFrame()).
inline constexpr base::TimeDelta kUniformPhotoTimeout = base::Seconds(2);

// takePhoto()'s JPEG quality (as Chromium's own JPEG photos, media/capture/
// video/blob_utils.cc).
inline constexpr int kUniformPhotoJpegQuality = 90;

// Whether `track`'s ImageCapture answers as MoltNet's camera: a MoltNet
// camera's track (moltnet_camera.h, UsesUniformCamera()).
MODULES_EXPORT bool UsesUniformImageCapture(MediaStreamTrack* track);

// Binds `receiver` to UniformImageCapture for `track` in `context`, on
// `task_runner`; it lives as long as the connection.
MODULES_EXPORT void BindUniformImageCapture(
    MediaStreamTrack* track,
    ExecutionContext* context,
    mojo::PendingReceiver<media::mojom::blink::ImageCapture> receiver,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner);

// The photo state of a MoltNet camera track whose frames are `size`: a camera
// without controls whose photos are its frames. An empty `size` gives no
// photo size either.
MODULES_EXPORT media::mojom::blink::PhotoStatePtr UniformPhotoState(
    const gfx::Size& size);

// takePhoto()'s JPEG of `frame`'s visible part (quality
// kUniformPhotoJpegQuality, 4:2:0, no colour profile): an I420 frame in CPU
// memory in BT.601 at limited range, as a MoltNet camera track's are. Empty
// for another frame or a failure. Any thread.
MODULES_EXPORT Vector<uint8_t> EncodeUniformCameraPhoto(
    const media::VideoFrame& frame);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_IMAGECAPTURE_MOLTNET_IMAGE_CAPTURE_H_
