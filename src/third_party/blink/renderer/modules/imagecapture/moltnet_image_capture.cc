// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/modules/imagecapture/moltnet_image_capture.h"

#include <memory>
#include <utility>

#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "media/base/video_frame.h"
#include "media/base/video_types.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/platform/modules/mediastream/web_media_stream_track.h"
#include "third_party/blink/public/web/modules/mediastream/media_stream_video_sink.h"
#include "third_party/blink/public/web/modules/mediastream/media_stream_video_source.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/modules/mediastream/media_stream_track.h"
#include "third_party/blink/renderer/modules/mediastream/media_stream_video_track.h"
#include "third_party/blink/renderer/modules/mediastream/moltnet_camera.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/image-encoders/image_encoder.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_component.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cancellable_task.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/scheduler/public/worker_pool.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "third_party/libyuv/include/libyuv/convert_argb.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkPixmap.h"

namespace blink::molt_fingerprinting {

namespace {

// A range a camera without the control reports: none.
media::mojom::blink::RangePtr NoRange() {
  return media::mojom::blink::Range::New();
}

// A range of one value (max, min and current; no step), as a camera reports
// the size of photos it takes from its stream (Linux's V4L2 cameras).
media::mojom::blink::RangePtr OneValue(double value) {
  return media::mojom::blink::Range::New(value, value, value, 0.0);
}

// The size of `track`'s frames: a MoltNet camera track's own size, which its
// adapter settings fix (moltnet_camera.h); empty for another track.
gfx::Size TrackSize(MediaStreamTrack* track) {
  if (!track || !track->Component()) {
    return gfx::Size();
  }
  MediaStreamVideoTrack* video_track =
      MediaStreamVideoTrack::From(track->Component());
  if (!video_track) {
    return gfx::Size();
  }
  return UniformCameraFrameSize(/*is_rotated=*/false,
                                video_track->adapter_settings())
      .value_or(gfx::Size());
}

// Takes a MoltNet camera track's next frame and encodes it as takePhoto()'s
// JPEG, off the main thread. Lives on the main thread; reports once, through
// `done`, with the JPEG or nothing (no frame within kUniformPhotoTimeout, or
// a frame it can't encode).
class UniformPhotoTaker final : public MediaStreamVideoSink {
 public:
  using DoneCallback =
      base::OnceCallback<void(UniformPhotoTaker*, Vector<uint8_t> jpeg)>;

  UniformPhotoTaker(MediaStreamComponent* component,
                    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
                    DoneCallback done)
      : task_runner_(std::move(task_runner)), done_(std::move(done)) {
    timeout_ = PostDelayedCancellableTask(
        *task_runner_, FROM_HERE,
        blink::BindOnce(&UniformPhotoTaker::Finish, weak_factory_.GetWeakPtr(),
                        Vector<uint8_t>()),
        kUniformPhotoTimeout);
    ConnectToTrack(WebMediaStreamTrack(component),
                   ConvertToBaseRepeatingCallback(CrossThreadBindRepeating(
                       &UniformPhotoTaker::OnFrameOnVideoTaskRunner,
                       task_runner_, weak_factory_.GetWeakPtr())),
                   MediaStreamVideoSink::IsSecure::kNo,
                   MediaStreamVideoSink::UsesAlpha::kNo);
  }

  UniformPhotoTaker(const UniformPhotoTaker&) = delete;
  UniformPhotoTaker& operator=(const UniformPhotoTaker&) = delete;
  ~UniformPhotoTaker() override = default;

 private:
  static void OnFrameOnVideoTaskRunner(
      scoped_refptr<base::SingleThreadTaskRunner> task_runner,
      base::WeakPtr<UniformPhotoTaker> taker,
      scoped_refptr<media::VideoFrame> frame,
      base::TimeTicks /*estimated_capture_time*/) {
    PostCrossThreadTask(*task_runner, FROM_HERE,
                        CrossThreadBindOnce(&UniformPhotoTaker::OnFrame,
                                            std::move(taker), std::move(frame)));
  }

  static void EncodeOnWorker(
      scoped_refptr<media::VideoFrame> frame,
      scoped_refptr<base::SingleThreadTaskRunner> task_runner,
      base::WeakPtr<UniformPhotoTaker> taker) {
    Vector<uint8_t> jpeg = EncodeUniformCameraPhoto(*frame);
    PostCrossThreadTask(*task_runner, FROM_HERE,
                        CrossThreadBindOnce(&UniformPhotoTaker::Finish,
                                            std::move(taker), std::move(jpeg)));
  }

  void OnFrame(scoped_refptr<media::VideoFrame> frame) {
    if (encoding_ || !done_) {
      return;
    }
    encoding_ = true;
    DisconnectFromTrack();
    worker_pool::PostTask(
        FROM_HERE, {base::TaskPriority::USER_BLOCKING},
        CrossThreadBindOnce(&UniformPhotoTaker::EncodeOnWorker,
                            std::move(frame), task_runner_,
                            weak_factory_.GetWeakPtr()));
  }

  // Last: `done_` may delete this.
  void Finish(Vector<uint8_t> jpeg) {
    if (!done_) {
      return;
    }
    timeout_.Cancel();
    DisconnectFromTrack();
    std::move(done_).Run(this, std::move(jpeg));
  }

  const scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
  DoneCallback done_;
  bool encoding_ = false;
  TaskHandle timeout_;
  base::WeakPtrFactory<UniformPhotoTaker> weak_factory_{this};
};

// The ImageCapture service of a MoltNet camera track: a camera without
// controls whose photos are its frames (moltnet_image_capture.h).
class UniformImageCapture final : public media::mojom::blink::ImageCapture {
 public:
  UniformImageCapture(MediaStreamTrack* track,
                      ExecutionContext* context,
                      scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : track_(track), context_(context), task_runner_(std::move(task_runner)) {}

  UniformImageCapture(const UniformImageCapture&) = delete;
  UniformImageCapture& operator=(const UniformImageCapture&) = delete;
  ~UniformImageCapture() override = default;

  // media::mojom::blink::ImageCapture:
  void GetPhotoState(const String& /*source_id*/,
                     GetPhotoStateCallback callback) override {
    std::move(callback).Run(UniformPhotoState(TrackSize(track_.Get())));
  }

  void SetPhotoOptions(const String& /*source_id*/,
                       media::mojom::blink::PhotoSettingsPtr settings,
                       SetPhotoOptionsCallback callback) override {
    // Nothing to set; refused as the browser refuses it while the page is
    // hidden, and for pan, tilt and zoom without their permission.
    auto* window = DynamicTo<LocalDOMWindow>(context_.Get());
    const bool visible = window && window->document()->IsPageVisible();
    std::move(callback).Run(visible && settings && !settings->has_pan &&
                            !settings->has_tilt && !settings->has_zoom);
  }

  void TakePhoto(const String& /*source_id*/,
                 TakePhotoCallback callback) override {
    MediaStreamTrack* track = track_.Get();
    if (!track || !track->Component()) {
      std::move(callback).Run(media::mojom::blink::Blob::New());
      return;
    }
    takers_.push_back(std::make_unique<UniformPhotoTaker>(
        track->Component(), task_runner_,
        blink::BindOnce(&UniformImageCapture::OnPhotoTaken,
                        weak_factory_.GetWeakPtr(), std::move(callback))));
  }

 private:
  void OnPhotoTaken(TakePhotoCallback callback,
                    UniformPhotoTaker* taker,
                    Vector<uint8_t> jpeg) {
    auto blob = media::mojom::blink::Blob::New();
    if (!jpeg.empty()) {
      blob->mime_type = "image/jpeg";
      blob->data = std::move(jpeg);
    }
    // The taker reported last thing: it may go.
    for (wtf_size_t i = 0; i < takers_.size(); ++i) {
      if (takers_[i].get() == taker) {
        takers_.EraseAt(i);
        break;
      }
    }
    std::move(callback).Run(std::move(blob));
  }

  WeakPersistent<MediaStreamTrack> track_;
  WeakPersistent<ExecutionContext> context_;
  const scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
  Vector<std::unique_ptr<UniformPhotoTaker>> takers_;
  base::WeakPtrFactory<UniformImageCapture> weak_factory_{this};
};

}  // namespace

bool UsesUniformImageCapture(MediaStreamTrack* track) {
  if (!track || !track->Component()) {
    return false;
  }
  MediaStreamVideoTrack* video_track =
      MediaStreamVideoTrack::From(track->Component());
  return video_track && video_track->source() &&
         UsesUniformCamera(video_track->source()->device().type);
}

void BindUniformImageCapture(
    MediaStreamTrack* track,
    ExecutionContext* context,
    mojo::PendingReceiver<media::mojom::blink::ImageCapture> receiver,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<UniformImageCapture>(track, context, task_runner),
      std::move(receiver), task_runner);
}

media::mojom::blink::PhotoStatePtr UniformPhotoState(const gfx::Size& size) {
  auto state = media::mojom::blink::PhotoState::New();
  // No control: no modes (empty lists, NONE), no ranges, no torch, no points
  // of interest, no effects.
  state->exposure_compensation = NoRange();
  state->exposure_time = NoRange();
  state->color_temperature = NoRange();
  state->iso = NoRange();
  state->brightness = NoRange();
  state->contrast = NoRange();
  state->saturation = NoRange();
  state->sharpness = NoRange();
  state->focus_distance = NoRange();
  state->pan = NoRange();
  state->tilt = NoRange();
  state->zoom = NoRange();
  state->supports_torch = false;
  state->torch = false;
  // Photos: the track's own frames, no red-eye reduction, no fill light.
  state->red_eye_reduction = media::mojom::blink::RedEyeReduction::NEVER;
  state->height = size.IsEmpty() ? NoRange() : OneValue(size.height());
  state->width = size.IsEmpty() ? NoRange() : OneValue(size.width());
  return state;
}

Vector<uint8_t> EncodeUniformCameraPhoto(const media::VideoFrame& frame) {
  using Plane = media::VideoFrame::Plane;
  if (frame.format() != media::PIXEL_FORMAT_I420 ||
      !frame.HasDirectCpuAccess()) {
    return {};
  }
  const int width = frame.visible_rect().width();
  const int height = frame.visible_rect().height();
  // A camera frame's size at most (media::limits::kMaxDimension per axis is
  // larger than any menu entry).
  constexpr int kMaxPhotoDimension = 8192;
  if (width <= 0 || height <= 0 || width > kMaxPhotoDimension ||
      height > kMaxPhotoDimension ||
      width > ImageEncoder::MaxDimension(kMimeTypeJpeg) ||
      height > ImageEncoder::MaxDimension(kMimeTypeJpeg)) {
    return {};
  }
  // libyuv's ARGB (B, G, R, A in memory) from BT.601 at limited range, the
  // samples a MoltNet camera frame carries; libyuv's YUV to RGB gives the same
  // bytes on every CPU (docs/FINGERPRINTING.md, "MoltNet: pixels and encoded
  // bytes").
  const size_t row_bytes = static_cast<size_t>(width) * 4;
  Vector<uint8_t> argb(static_cast<wtf_size_t>(row_bytes * height));
  if (libyuv::I420ToARGB(
          frame.visible_data(Plane::kY), static_cast<int>(frame.stride(Plane::kY)),
          frame.visible_data(Plane::kU), static_cast<int>(frame.stride(Plane::kU)),
          frame.visible_data(Plane::kV), static_cast<int>(frame.stride(Plane::kV)),
          argb.data(), static_cast<int>(row_bytes), width, height) != 0) {
    return {};
  }
  const SkImageInfo info = SkImageInfo::Make(
      width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
  const SkPixmap pixmap(info, argb.data(), row_bytes);
  SkJpegEncoder::Options options;
  options.fQuality = kUniformPhotoJpegQuality;
  options.fDownsample = SkJpegEncoder::Downsample::k420;
  Vector<unsigned char> jpeg;
  if (!ImageEncoder::Encode(&jpeg, pixmap, options)) {
    return {};
  }
  return jpeg;
}

}  // namespace blink::molt_fingerprinting
