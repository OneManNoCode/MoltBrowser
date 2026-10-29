// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/mediastream/video_track_adapter.h"

#include <array>  // MoltBrowser
#include <limits>

#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/numerics/ranges.h"
#include "base/run_loop.h"
#include "base/synchronization/lock.h"  // MoltBrowser
#include "base/synchronization/waitable_event.h"
#include "base/test/bind.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "gpu/command_buffer/client/test_shared_image_interface.h"
#include "media/base/limits.h"
#include "media/base/video_frame_converter.h"  // MoltBrowser
#include "media/base/video_frame_pool.h"  // MoltBrowser
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/web/web_heap.h"
#include "third_party/blink/renderer/modules/mediastream/mock_encoded_video_frame.h"
#include "third_party/blink/renderer/modules/mediastream/mock_media_stream_video_source.h"
#include "third_party/blink/renderer/modules/mediastream/moltnet_camera.h"  // MoltBrowser
#include "third_party/blink/renderer/modules/mediastream/video_track_adapter_settings.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/testing/io_task_runner_testing_platform_support.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/testing/video_frame_utils.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_copier_media.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "third_party/webrtc_overrides/low_precision_timer.h"
#include "third_party/webrtc_overrides/metronome_source.h"

namespace blink {

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::NiceMock;
using ::testing::Pointee;
using ::testing::Property;

// Most VideoTrackAdapter functionality is tested in MediaStreamVideoSourceTest.
// These tests focus on the computation of cropped frame sizes in edge cases
// that cannot be easily reproduced by a mocked video source, such as tests
// involving frames of zero size.
// Such frames can be produced by sources in the wild (e.g., element capture).

// Test that cropped sizes with zero-area input frames are correctly computed.
// Aspect ratio limits should be ignored.
TEST(VideoTrackAdapterTest, ZeroInputArea) {
  test::TaskEnvironment task_environment;
  const int kMaxWidth = 640;
  const int kMaxHeight = 480;
  const int kSmallDimension = 300;
  const int kLargeDimension = 1000;
  static_assert(kSmallDimension < kMaxWidth && kSmallDimension < kMaxHeight,
                "kSmallDimension must be less than kMaxWidth and kMaxHeight");
  static_assert(
      kLargeDimension > kMaxWidth && kLargeDimension > kMaxHeight,
      "kLargeDimension must be greater than kMaxWidth and kMaxHeight");
  const VideoTrackAdapterSettings kVideoTrackAdapterSettings(
      gfx::Size(kMaxWidth, kMaxHeight), 0.1 /* min_aspect_ratio */,
      2.0 /* max_aspect_ratio */, 0.0 /* max_frame_rate */);
  const bool kIsRotatedValues[] = {true, false};

  for (bool is_rotated : kIsRotatedValues) {
    gfx::Size desired_size;

    VideoTrackAdapter::CalculateDesiredSize(
        is_rotated, gfx::Size(0, 0), kVideoTrackAdapterSettings, &desired_size);
    EXPECT_EQ(desired_size.width(), 0);
    EXPECT_EQ(desired_size.height(), 0);

    // Zero width.
    VideoTrackAdapter::CalculateDesiredSize(
        is_rotated, gfx::Size(0, kSmallDimension), kVideoTrackAdapterSettings,
        &desired_size);
    EXPECT_EQ(desired_size.width(), 0);
    EXPECT_EQ(desired_size.height(), kSmallDimension);

    // Zero height.
    VideoTrackAdapter::CalculateDesiredSize(
        is_rotated, gfx::Size(kSmallDimension, 0), kVideoTrackAdapterSettings,
        &desired_size);
    EXPECT_EQ(desired_size.width(), kSmallDimension);
    EXPECT_EQ(desired_size.height(), 0);

    // Requires "cropping" of height.
    VideoTrackAdapter::CalculateDesiredSize(
        is_rotated, gfx::Size(0, kLargeDimension), kVideoTrackAdapterSettings,
        &desired_size);
    EXPECT_EQ(desired_size.width(), 0);
    EXPECT_EQ(desired_size.height(), is_rotated ? kMaxWidth : kMaxHeight);

    // Requires "cropping" of width.
    VideoTrackAdapter::CalculateDesiredSize(
        is_rotated, gfx::Size(kLargeDimension, 0), kVideoTrackAdapterSettings,
        &desired_size);
    EXPECT_EQ(desired_size.width(), is_rotated ? kMaxHeight : kMaxWidth);
    EXPECT_EQ(desired_size.height(), 0);
  }
}

// Test that zero-size cropped areas are correctly computed. Aspect ratio
// limits should be ignored.
TEST(VideoTrackAdapterTest, ZeroOutputArea) {
  test::TaskEnvironment task_environment;
  const double kMinAspectRatio = 0.1;
  const double kMaxAspectRatio = 2.0;
  const int kInputWidth = 640;
  const int kInputHeight = 480;
  const int kSmallMaxDimension = 300;
  const int kLargeMaxDimension = 1000;
  static_assert(
      kSmallMaxDimension < kInputWidth && kSmallMaxDimension < kInputHeight,
      "kSmallMaxDimension must be less than kInputWidth and kInputHeight");
  static_assert(
      kLargeMaxDimension > kInputWidth && kLargeMaxDimension > kInputHeight,
      "kLargeMaxDimension must be greater than kInputWidth and kInputHeight");

  gfx::Size desired_size;

  VideoTrackAdapter::CalculateDesiredSize(
      false /* is_rotated */, gfx::Size(kInputWidth, kInputHeight),
      VideoTrackAdapterSettings(gfx::Size(0, 0), kMinAspectRatio,
                                kMaxAspectRatio, 0.0 /* max_frame_rate */),
      &desired_size);
  EXPECT_EQ(desired_size.width(), 0);
  EXPECT_EQ(desired_size.height(), 0);

  // Width is cropped to zero.
  VideoTrackAdapter::CalculateDesiredSize(
      false /* is_rotated */, gfx::Size(kInputWidth, kInputHeight),
      VideoTrackAdapterSettings(gfx::Size(0, kLargeMaxDimension), 0.0,
                                HUGE_VAL,  // kMinAspectRatio, kMaxAspectRatio,
                                0.0 /* max_frame_rate */),
      &desired_size);
  EXPECT_EQ(desired_size.width(), 0);
  EXPECT_EQ(desired_size.height(), kInputHeight);

  // Requires "cropping" of width and height.
  VideoTrackAdapter::CalculateDesiredSize(
      false /* is_rotated */, gfx::Size(kInputWidth, kInputHeight),
      VideoTrackAdapterSettings(gfx::Size(0, kSmallMaxDimension),
                                kMinAspectRatio, kMaxAspectRatio,
                                0.0 /* max_frame_rate */),
      &desired_size);
  EXPECT_EQ(desired_size.width(), 0);
  EXPECT_EQ(desired_size.height(), kSmallMaxDimension);

  // Height is cropped to zero.
  VideoTrackAdapter::CalculateDesiredSize(
      false /* is_rotated */, gfx::Size(kInputWidth, kInputHeight),
      VideoTrackAdapterSettings(gfx::Size(kLargeMaxDimension, 0),
                                kMinAspectRatio, kMaxAspectRatio,
                                0.0 /* max_frame_rate */),
      &desired_size);
  EXPECT_EQ(desired_size.width(), kInputWidth);
  EXPECT_EQ(desired_size.height(), 0);

  // Requires "cropping" of width and height.
  VideoTrackAdapter::CalculateDesiredSize(
      false /* is_rotated */, gfx::Size(kInputWidth, kInputHeight),
      VideoTrackAdapterSettings(
          gfx::Size(kSmallMaxDimension /* max_width */, 0 /* max_height */),
          kMinAspectRatio, kMaxAspectRatio, 0.0 /* max_frame_rate */),
      &desired_size);
  EXPECT_EQ(desired_size.width(), kSmallMaxDimension);
  EXPECT_EQ(desired_size.height(), 0);
}

// Test that large frames are handled correctly.
TEST(VideoTrackAdapterTest, LargeFrames) {
  test::TaskEnvironment task_environment;
  const int kInputWidth = std::numeric_limits<int>::max();
  const int kInputHeight = std::numeric_limits<int>::max();
  const int kMaxWidth = std::numeric_limits<int>::max();
  const int kMaxHeight = std::numeric_limits<int>::max();

  gfx::Size desired_size;

  // If a target size is provided in VideoTrackAdapterSettings, rescaling is
  // allowed and frames must be clamped to the maximum allowed size, even if the
  // target exceeds the system maximum.
  bool success = VideoTrackAdapter::CalculateDesiredSize(
      false /* is_rotated */, gfx::Size(kInputWidth, kInputHeight),
      VideoTrackAdapterSettings(gfx::Size(kMaxWidth, kMaxHeight),
                                0.0 /* max_frame_rate */),
      &desired_size);
  EXPECT_TRUE(success);
  EXPECT_EQ(desired_size.width(), media::limits::kMaxDimension);
  EXPECT_EQ(desired_size.height(), media::limits::kMaxDimension);

  // If no target size is provided in VideoTrackAdapterSettings, rescaling is
  // disabled and |desired_size| is left unmodified.
  desired_size.set_width(0);
  desired_size.set_height(0);
  success = VideoTrackAdapter::CalculateDesiredSize(
      false /* is_rotated */, gfx::Size(kInputWidth, kInputHeight),
      VideoTrackAdapterSettings(), &desired_size);
  EXPECT_FALSE(success);
  EXPECT_EQ(desired_size.width(), 0);
  EXPECT_EQ(desired_size.height(), 0);
}

// Test that regular frames are not rescaled if settings do not specify a target
// resolution.
TEST(VideoTrackAdapterTest, NoRescaling) {
  test::TaskEnvironment task_environment;
  const int kInputWidth = 640;
  const int kInputHeight = 480;

  // No target size,
  gfx::Size desired_size;
  bool success = VideoTrackAdapter::CalculateDesiredSize(
      false /* is_rotated */, gfx::Size(kInputWidth, kInputHeight),
      VideoTrackAdapterSettings(), &desired_size);
  EXPECT_TRUE(success);
  EXPECT_EQ(desired_size.width(), kInputWidth);
  EXPECT_EQ(desired_size.height(), kInputHeight);
}

class VideoTrackAdapterFixtureTest : public ::testing::Test {
 public:
  VideoTrackAdapterFixtureTest()
      : testing_render_thread_("TestingRenderThread"),
        frame_processed_(base::WaitableEvent::ResetPolicy::MANUAL,
                         base::WaitableEvent::InitialState::NOT_SIGNALED),
        test_sii_(base::MakeRefCounted<gpu::TestSharedImageInterface>()) {
  }
  ~VideoTrackAdapterFixtureTest() override = default;

 protected:
  void SetUp() override { testing_render_thread_.Start(); }

  void TearDown() override {
    if (track_added_) {
      PostCrossThreadTask(
          *testing_render_thread_.task_runner(), FROM_HERE,
          CrossThreadBindOnce(&VideoTrackAdapter::RemoveTrack, adapter_,
                              CrossThreadUnretained(null_track_.get())));
    }
    base::WaitableEvent source_deleted;
    testing_render_thread_.task_runner()->PostTask(
        FROM_HERE, base::BindLambdaForTesting([&]() {
          source_deleted.Signal();
        }));
    source_deleted.Wait();
    testing_render_thread_.Stop();
  }

  void CreateAdapter(media::VideoCaptureFormat capture_format,
                     bool is_display_capture = false) {
    // Create the MockMediaStreamVideoSource and VideoTrackAdapter instances on
    // |testing_render_thread_|.
    base::WaitableEvent adapter_created(
        base::WaitableEvent::ResetPolicy::MANUAL,
        base::WaitableEvent::InitialState::NOT_SIGNALED);
    testing_render_thread_.task_runner()->PostTask(
        FROM_HERE, base::BindLambdaForTesting([&]() {
          adapter_ = base::MakeRefCounted<VideoTrackAdapter>(
              platform_support_->GetIOTaskRunner(), is_display_capture);
          adapter_created.Signal();
        }));
    adapter_created.Wait();
  }

  // Create or re-configure the dummy |null_track_| with the given
  // |adapter_settings|.
  void ConfigureTrack(const VideoTrackAdapterSettings& adapter_settings) {
    if (!track_added_) {
      AddTrackInternal(null_track_.get(), adapter_settings);
      track_added_ = true;
    } else {
      PostCrossThreadTask(
          *testing_render_thread_.task_runner(), FROM_HERE,
          CrossThreadBindOnce(&VideoTrackAdapter::ReconfigureTrack, adapter_,
                              CrossThreadUnretained(null_track_.get()),
                              adapter_settings));
    }
  }

  void AddTrackInternal(MediaStreamVideoTrack* track,
                        const VideoTrackAdapterSettings& adapter_settings) {
    MediaStreamVideoSourceCallbacks video_stream_fallbacks;
    video_stream_fallbacks.deliver_frame_cb =
        base::BindRepeating(&VideoTrackAdapterFixtureTest::OnFrameDelivered,
                            base::Unretained(this));
    video_stream_fallbacks.frame_dropped_cb = base::BindRepeating(
        &VideoTrackAdapterFixtureTest::OnFrameDropped, base::Unretained(this));
    video_stream_fallbacks.encoded_frame_cb = base::BindRepeating(
        &VideoTrackAdapterFixtureTest::OnEncodedVideoFrameDelivered,
        base::Unretained(this));
    video_stream_fallbacks.settings_cb = base::BindRepeating(
        &VideoTrackAdapterFixtureTest::OnFrameSettings, base::Unretained(this));
    video_stream_fallbacks.capture_version_cb = base::DoNothing();
    video_stream_fallbacks.format_cb = base::DoNothing();
    PostCrossThreadTask(
        *testing_render_thread_.task_runner(), FROM_HERE,
        CrossThreadBindOnce(&VideoTrackAdapter::AddTrack, adapter_,
                            CrossThreadUnretained(track),
                            std::move(video_stream_fallbacks),
                            adapter_settings));
  }

  void SetFrameValidationCallback(VideoCaptureDeliverFrameCB callback) {
    frame_validation_callback_ = std::move(callback);
  }

  void SetFrameDroppedCallback(VideoCaptureNotifyFrameDroppedCB callback) {
    frame_dropped_callback_ = std::move(callback);
  }

  void SetFrameSettingsCallback(VideoTrackSettingsCallback callback) {
    frame_settings_callback_ = std::move(callback);
  }

  // Deliver |frame| to |adapter_| and wait until OnFrameDelivered signals that
  // it receives the processed frame.
  void DeliverAndValidateFrame(scoped_refptr<media::VideoFrame> frame,
                               base::TimeTicks estimated_capture_time) {
    auto deliver_frame = [&]() {
      PostCrossThreadTask(
          *platform_support_->GetIOTaskRunner(), FROM_HERE,
          CrossThreadBindOnce(&VideoTrackAdapter::DeliverFrameOnVideoTaskRunner,
                              adapter_, frame, estimated_capture_time));
    };

    frame_processed_.Reset();
    // Bounce the call to DeliverFrameOnVideoTaskRunner off
    // |testing_render_thread_| to synchronize with the
    // AddTrackOnVideoTaskRunner / ReconfigureTrackOnVideoTaskRunner that would
    // be invoked through ConfigureTrack.
    testing_render_thread_.task_runner()->PostTask(
        FROM_HERE, base::BindLambdaForTesting(deliver_frame));
    frame_processed_.Wait();
  }

  // Ask |adapter_| to synthesize and deliver a black frame, and wait until
  // either OnFrameDelivered or OnFrameDropped has observed the outcome.
  void DeliverBlackFrameAndValidate(const gfx::Size& size,
                                    media::CaptureVersion capture_version,
                                    base::TimeTicks estimated_capture_time) {
    // |adapter_| is constructed with the IO task runner as its video task
    // runner (see CreateAdapter), mirroring production, so the delivery is
    // posted there.
    auto deliver_frame = [&]() {
      PostCrossThreadTask(
          *platform_support_->GetIOTaskRunner(), FROM_HERE,
          CrossThreadBindOnce(
              &VideoTrackAdapter::DeliverBlackFrameOnVideoTaskRunner, adapter_,
              size, capture_version, estimated_capture_time));
    };

    frame_processed_.Reset();
    // Bounce off |testing_render_thread_| to synchronize with the
    // AddTrackOnVideoTaskRunner / ReconfigureTrackOnVideoTaskRunner that would
    // be invoked through ConfigureTrack, as DeliverAndValidateFrame does.
    testing_render_thread_.task_runner()->PostTask(
        FROM_HERE, base::BindLambdaForTesting(deliver_frame));
    frame_processed_.Wait();
  }

  void OnFrameDelivered(
      scoped_refptr<media::VideoFrame> frame,
      base::TimeTicks estimated_capture_time) {
    if (frame_validation_callback_) {
      frame_validation_callback_.Run(frame, estimated_capture_time);
      frame_processed_.Signal();
    }
  }

  void OnFrameDropped(media::VideoCaptureFrameDropReason reason) {
    if (frame_dropped_callback_) {
      frame_dropped_callback_.Run(reason);
      frame_processed_.Signal();
    }
  }

  void OnFrameSettings(gfx::Size frame_size,
                       double frame_rate,
                       std::optional<gfx::Size> metadata_source_size,
                       std::optional<float> device_scale_factor) {
    if (frame_settings_callback_) {
      frame_settings_callback_.Run(frame_size, frame_rate, metadata_source_size,
                                   device_scale_factor);
      frame_processed_.Signal();
    }
  }

  // Configures a track and an adapter with the given target frame rate,
  // generates `num_frames` frames with 10x10 resolution. The timestamps are
  // generated by calling the lambda function `index_to_timestamp` for each
  // frame. Returns the number of delivered and dropped frames.
  std::tuple<int, int> GenerateAndCountFrames(
      int num_frames,
      std::optional<double> target_frame_rate,
      base::RepeatingCallback<base::TimeDelta(int)> index_to_timestamp) {
    const gfx::Size resolution(10, 10);
    // Any capture format will work. Frames will generated at the
    // `actual_input_frame_rate`.
    const media::VideoCaptureFormat stream_format(
        resolution, /*frame_rate=*/10.0, media::PIXEL_FORMAT_I420);
    CreateAdapter(stream_format);

    VideoTrackAdapterSettings adapter_settings(
        /*target_size=*/std::nullopt,
        /*min_aspect_ratio=*/0.0000,
        /*max_aspect_ratio=*/resolution.width(), target_frame_rate);
    ConfigureTrack(adapter_settings);
    base::WaitableEvent did_process_all_frames;
    int num_delivered = 0;
    int num_dropped = 0;

    SetFrameValidationCallback(base::BindLambdaForTesting(
        [&](scoped_refptr<media::VideoFrame> frame,
            base::TimeTicks estimated_capture_time) {
          num_delivered++;
          if (num_delivered + num_dropped == num_frames) {
            did_process_all_frames.Signal();
          }
        }));
    SetFrameDroppedCallback(
        base::BindLambdaForTesting([&](media::VideoCaptureFrameDropReason) {
          num_dropped++;
          if (num_delivered + num_dropped == num_frames) {
            did_process_all_frames.Signal();
          }
        }));

    for (int i = 0; i < num_frames; ++i) {
      auto frame = CreateTestFrame(
          /*coded_size=*/resolution,
          /*visible_rect=*/
          gfx::Rect(0, 0, resolution.width(), resolution.height()),
          /*natural_size*/ resolution,
          /*storage_type=*/media::VideoFrame::STORAGE_OWNED_MEMORY,
          media::PIXEL_FORMAT_I420,
          /*timestamp=*/index_to_timestamp.Run(i), test_sii_.get());
      DeliverAndValidateFrame(std::move(frame), base::TimeTicks());
    }

    did_process_all_frames.Wait();
    return {num_delivered, num_dropped};
  }

  // Configures a track and an adapter with the given target frame rate,
  // generates `num_frames` frames with 10x10 resolution at the given
  // `actual_input_frame_rate` and returns the number of delivered and dropped
  // frames.
  std::tuple<int, int> GenerateAndCountFrames(
      int num_frames,
      std::optional<double> target_frame_rate,
      double actual_input_frame_rate) {
    auto index_to_timestamp =
        base::BindLambdaForTesting([actual_input_frame_rate](int i) {
          return i * base::Seconds(1.0 / actual_input_frame_rate);
        });
    return GenerateAndCountFrames(num_frames, target_frame_rate,
                                  index_to_timestamp);
  }

  void TestDeliversFrameWithVisibleRectWithEvenOriginAndSize(
      scoped_refptr<media::VideoFrame> frame,
      media::VideoPixelFormat pixel_format,
      gfx::Size desired_size) {
    const gfx::Size coded_size = frame->coded_size();
    const double kFrameRate = 30.0;
    const media::VideoCaptureFormat stream_format(coded_size, kFrameRate,
                                                  pixel_format);
    CreateAdapter(stream_format);

    VideoTrackAdapterSettings cropped_settings(desired_size, kFrameRate);
    ConfigureTrack(cropped_settings);
    auto check_settings =
        [&](scoped_refptr<media::VideoFrame> frame,
            base::TimeTicks estimated_capture_time) {
          EXPECT_FALSE(frame->visible_rect().x() & 1);
          EXPECT_FALSE(frame->visible_rect().y() & 1);
        };
    SetFrameValidationCallback(base::BindLambdaForTesting(check_settings));
    DeliverAndValidateFrame(frame, base::TimeTicks());
  }

  // MoltBrowser: a MoltNet camera's tracks get their frames at the ticks of a
  // grid (moltnet_camera.h). These helpers run the ticks by hand, on the
  // grid's own time (tick n's timestamp is n/30 s).
  void UseManualMoltCameraTicks() {
    base::WaitableEvent done;
    platform_support_->GetIOTaskRunner()->PostTask(
        FROM_HERE, base::BindLambdaForTesting([&] {
          adapter_->SetMoltCameraManualTicksForTesting();
          done.Signal();
        }));
    done.Wait();
  }

  // Hands `frame` to the adapter, which holds it for the next tick.
  void DeliverMoltCameraFrame(scoped_refptr<media::VideoFrame> frame) {
    base::WaitableEvent done;
    // Bounced off |testing_render_thread_| like DeliverAndValidateFrame().
    testing_render_thread_.task_runner()->PostTask(
        FROM_HERE, base::BindLambdaForTesting([&] {
          platform_support_->GetIOTaskRunner()->PostTask(
              FROM_HERE, base::BindLambdaForTesting([&] {
                adapter_->DeliverFrameOnVideoTaskRunner(frame,
                                                        base::TimeTicks());
                done.Signal();
              }));
        }));
    done.Wait();
  }

  // Runs a tick and waits until it ran.
  void MoltCameraTick() {
    base::WaitableEvent done;
    platform_support_->GetIOTaskRunner()->PostTask(
        FROM_HERE, base::BindLambdaForTesting([&] {
          adapter_->MoltCameraTickForTesting();
          done.Signal();
        }));
    done.Wait();
  }

  // Runs a tick and waits for what it hands the track: a frame or a drop.
  void MoltCameraTickAndWait() {
    frame_processed_.Reset();
    platform_support_->GetIOTaskRunner()->PostTask(
        FROM_HERE, base::BindLambdaForTesting(
                       [&] { adapter_->MoltCameraTickForTesting(); }));
    frame_processed_.Wait();
  }

  // A MoltNet camera track of `size` at `frame_rate`, on an adapter whose
  // ticks the test runs.
  void SetUpMoltCameraTrack(const gfx::Size& size, double frame_rate) {
    CreateAdapter(media::VideoCaptureFormat(gfx::Size(1280, 720), 30.0,
                                            media::PIXEL_FORMAT_I420));
    UseManualMoltCameraTicks();
    VideoTrackAdapterSettings settings(size, 0.0,
                                       std::numeric_limits<double>::max(),
                                       frame_rate);
    settings.set_molt_uniform_camera(/*reports_no_resize=*/true);
    ConfigureTrack(settings);
  }

  MOCK_METHOD2(OnEncodedVideoFrameDelivered,
               void(scoped_refptr<EncodedVideoFrame>,
                    base::TimeTicks estimated_capture_time));

  ScopedTestingPlatformSupport<IOTaskRunnerTestingPlatformSupport>
      platform_support_;
  test::TaskEnvironment task_environment_;
  base::Thread testing_render_thread_;
  scoped_refptr<VideoTrackAdapter> adapter_;

  base::test::ScopedFeatureList feature_list;
  base::WaitableEvent frame_processed_;
  VideoCaptureDeliverFrameCB frame_validation_callback_;
  VideoCaptureNotifyFrameDroppedCB frame_dropped_callback_;
  VideoTrackSettingsCallback frame_settings_callback_;

  // For testing we use a nullptr for MediaStreamVideoTrack.
  std::unique_ptr<MediaStreamVideoTrack> null_track_;
  bool track_added_ = false;
  scoped_refptr<gpu::TestSharedImageInterface> test_sii_;
};

TEST_F(VideoTrackAdapterFixtureTest, DeliverFrame_MappableSI) {
  // Attributes for the original input frame.
  const gfx::Size kCodedSize(1280, 960);
  const gfx::Rect kVisibleRect(0, 120, 1280, 720);
  const gfx::Size kNaturalSize(1280, 720);
  const double kFrameRate = 30.0;
  auto mappable_si_frame = CreateTestFrame(
      kCodedSize, kVisibleRect, kNaturalSize,
      media::VideoFrame::STORAGE_MAPPABLE_SHARED_IMAGE, test_sii_.get());

  // Initialize the VideoTrackAdapter to handle MappableSI. NV12 is the
  // only pixel format supported at the moment.
  const media::VideoCaptureFormat stream_format(kCodedSize, kFrameRate,
                                                media::PIXEL_FORMAT_NV12);
  CreateAdapter(stream_format);

  // Keep the desired size the same as the natural size of the original frame.
  VideoTrackAdapterSettings settings_nonscaled(kNaturalSize, kFrameRate);
  ConfigureTrack(settings_nonscaled);
  auto check_nonscaled =
      [&](scoped_refptr<media::VideoFrame> frame,
          base::TimeTicks estimated_capture_time) {
        // We should get the original frame as-is here.
        EXPECT_EQ(frame->storage_type(),
                  media::VideoFrame::STORAGE_MAPPABLE_SHARED_IMAGE);
        EXPECT_EQ(frame->shared_image(), mappable_si_frame->shared_image());
        EXPECT_EQ(frame->coded_size(), kCodedSize);
        EXPECT_EQ(frame->visible_rect(), kVisibleRect);
        EXPECT_EQ(frame->natural_size(), kNaturalSize);
      };
  SetFrameValidationCallback(base::BindLambdaForTesting(check_nonscaled));
  DeliverAndValidateFrame(mappable_si_frame, base::TimeTicks());

  // Scale the original frame by a factor of 0.5x.
  const gfx::Size kDesiredSize(640, 360);
  VideoTrackAdapterSettings settings_scaled(kDesiredSize, kFrameRate);
  ConfigureTrack(settings_scaled);
  auto check_scaled =
      [&](scoped_refptr<media::VideoFrame> frame,
          base::TimeTicks estimated_capture_time) {
        // The original frame should be wrapped in a new frame, with
        // |kDesiredSize| exposed as natural size of the wrapped frame.
        EXPECT_EQ(frame->storage_type(),
                  media::VideoFrame::STORAGE_MAPPABLE_SHARED_IMAGE);
        EXPECT_EQ(frame->shared_image(), mappable_si_frame->shared_image());
        EXPECT_EQ(frame->coded_size(), kCodedSize);
        EXPECT_EQ(frame->visible_rect(), kVisibleRect);
        EXPECT_EQ(frame->natural_size(), kDesiredSize);
      };
  SetFrameValidationCallback(base::BindLambdaForTesting(check_scaled));
  DeliverAndValidateFrame(mappable_si_frame, base::TimeTicks());
}

TEST_F(VideoTrackAdapterFixtureTest,
       DeliverScaledFrameFromDisplayCaptureDevice) {
  // Attributes for the original input frame.
  const gfx::Size kCodedSize(1280, 960);
  const gfx::Rect kVisibleRect(0, 120, 1280, 720);
  const gfx::Size kNaturalSize(1280, 720);
  const double kFrameRate = 30.0;
  auto mappable_si_frame = CreateTestFrame(
      kCodedSize, kVisibleRect, kNaturalSize,
      media::VideoFrame::STORAGE_MAPPABLE_SHARED_IMAGE, test_sii_.get());

  // Create an adapter that has a display capture device as its source.
  const media::VideoCaptureFormat stream_format(kCodedSize, kFrameRate,
                                                media::PIXEL_FORMAT_NV12);
  CreateAdapter(stream_format, /*is_display_capture=*/true);

  // Keep the desired size the same as the natural size of the original frame.
  VideoTrackAdapterSettings settings_nonscaled(kNaturalSize, kFrameRate);
  ConfigureTrack(settings_nonscaled);
  auto check_nonscaled = [&](scoped_refptr<media::VideoFrame> frame,
                             base::TimeTicks estimated_capture_time) {
    // We should get the original frame as-is here.
    EXPECT_EQ(frame->storage_type(),
              media::VideoFrame::STORAGE_MAPPABLE_SHARED_IMAGE);
    EXPECT_EQ(frame->shared_image(), mappable_si_frame->shared_image());
    EXPECT_EQ(frame->coded_size(), kCodedSize);
    EXPECT_EQ(frame->visible_rect(), kVisibleRect);
    EXPECT_EQ(frame->natural_size(), kNaturalSize);
  };
  SetFrameValidationCallback(base::BindLambdaForTesting(check_nonscaled));
  DeliverAndValidateFrame(mappable_si_frame, base::TimeTicks());

  // Scale the original frame by a factor of 0.5x.
  const gfx::Size kDesiredSize(640, 360);
  VideoTrackAdapterSettings settings_scaled(kDesiredSize, kFrameRate);
  ConfigureTrack(settings_scaled);
  auto check_scaled = [&](scoped_refptr<media::VideoFrame> frame,
                          base::TimeTicks estimated_capture_time) {
    // The original frame should be converted and scaled intp a new frame, with
    // |kDesiredSize| being the natural size of this cpnverted frame.
    EXPECT_EQ(frame->storage_type(), media::VideoFrame::STORAGE_OWNED_MEMORY);
    EXPECT_EQ(frame->coded_size(), kDesiredSize);
    EXPECT_EQ(frame->visible_rect().size(), kDesiredSize);
    EXPECT_EQ(frame->natural_size(), kDesiredSize);
  };
  SetFrameValidationCallback(base::BindLambdaForTesting(check_scaled));
  DeliverAndValidateFrame(mappable_si_frame, base::TimeTicks());
}

TEST_F(
    VideoTrackAdapterFixtureTest,
    DeliverScaledFrameFromDisplayCaptureDeviceWithFeatureDisabledCropsFrame) {
  feature_list.InitAndDisableFeature(kScaleFrameForGetDisplayMedia);
  // Attributes for the original input frame.
  const gfx::Size kCodedSize(1280, 960);
  const gfx::Rect kVisibleRect(0, 120, 1280, 720);
  const gfx::Size kNaturalSize(1280, 720);
  const double kFrameRate = 30.0;
  auto mappable_si_frame = CreateTestFrame(
      kCodedSize, kVisibleRect, kNaturalSize,
      media::VideoFrame::STORAGE_MAPPABLE_SHARED_IMAGE, test_sii_.get());

  // Create an adapter that has a display capture device as its source.
  const media::VideoCaptureFormat stream_format(kCodedSize, kFrameRate,
                                                media::PIXEL_FORMAT_NV12);
  CreateAdapter(stream_format, /*is_display_capture=*/true);

  // Keep the desired size the same as the natural size of the original frame.
  VideoTrackAdapterSettings settings_nonscaled(kNaturalSize, kFrameRate);
  ConfigureTrack(settings_nonscaled);
  auto check_nonscaled = [&](scoped_refptr<media::VideoFrame> frame,
                             base::TimeTicks estimated_capture_time) {
    // We should get the original frame as-is here.
    EXPECT_EQ(frame->storage_type(),
              media::VideoFrame::STORAGE_MAPPABLE_SHARED_IMAGE);
    EXPECT_EQ(frame->shared_image(), mappable_si_frame->shared_image());
    EXPECT_EQ(frame->coded_size(), kCodedSize);
    EXPECT_EQ(frame->visible_rect(), kVisibleRect);
    EXPECT_EQ(frame->natural_size(), kNaturalSize);
  };
  SetFrameValidationCallback(base::BindLambdaForTesting(check_nonscaled));
  DeliverAndValidateFrame(mappable_si_frame, base::TimeTicks());

  // Scale the original frame by a factor of 0.5x.
  const gfx::Size kDesiredSize(640, 360);
  VideoTrackAdapterSettings settings_scaled(kDesiredSize, kFrameRate);
  ConfigureTrack(settings_scaled);
  auto check_scaled = [&](scoped_refptr<media::VideoFrame> frame,
                          base::TimeTicks estimated_capture_time) {
    // We should get the original frame as-is here.
    EXPECT_EQ(frame->storage_type(),
              media::VideoFrame::STORAGE_MAPPABLE_SHARED_IMAGE);
    EXPECT_EQ(frame->shared_image(), mappable_si_frame->shared_image());
    EXPECT_EQ(frame->coded_size(), kCodedSize);
    EXPECT_EQ(frame->visible_rect(), kVisibleRect);
    EXPECT_EQ(frame->natural_size(), kDesiredSize);
  };
  SetFrameValidationCallback(base::BindLambdaForTesting(check_scaled));
  DeliverAndValidateFrame(mappable_si_frame, base::TimeTicks());
}

TEST_F(VideoTrackAdapterFixtureTest,
       DeliversHWFrameVisibleRectWithEvenOriginAndWidth) {
  const gfx::Size kCodedSize(1280, 720);
  const gfx::Rect kVisibleRect(0, 0, 1280, 720);
  const gfx::Size kDesiredSize(1277, 720);
  const gfx::Size kNaturalSize(1280, 720);
  TestDeliversFrameWithVisibleRectWithEvenOriginAndSize(
      CreateTestFrame(kCodedSize, kVisibleRect, kNaturalSize,
                      media::VideoFrame::STORAGE_MAPPABLE_SHARED_IMAGE,
                      test_sii_.get()),
      media::PIXEL_FORMAT_NV12, kDesiredSize);
}

TEST_F(VideoTrackAdapterFixtureTest,
       DeliversSWFrameVisibleRectWithEvenOriginAndWidth) {
  const gfx::Size kCodedSize(1280, 720);
  const gfx::Rect kVisibleRect(0, 0, 1280, 720);
  const gfx::Size kDesiredSize(1277, 720);
  const gfx::Size kNaturalSize(1280, 720);
  TestDeliversFrameWithVisibleRectWithEvenOriginAndSize(
      CreateTestFrame(kCodedSize, kVisibleRect, kNaturalSize,
                      media::VideoFrame::STORAGE_OWNED_MEMORY, test_sii_.get()),
      media::PIXEL_FORMAT_I420, kDesiredSize);
}

TEST_F(VideoTrackAdapterFixtureTest,
       DeliversHWFrameVisibleRectWithEvenOriginAndHeight) {
  const gfx::Size kCodedSize(1280, 720);
  const gfx::Rect kVisibleRect(0, 0, 1280, 720);
  const gfx::Size kDesiredSize(1280, 718);
  const gfx::Size kNaturalSize(1280, 720);
  TestDeliversFrameWithVisibleRectWithEvenOriginAndSize(
      CreateTestFrame(kCodedSize, kVisibleRect, kNaturalSize,
                      media::VideoFrame::STORAGE_MAPPABLE_SHARED_IMAGE,
                      test_sii_.get()),
      media::PIXEL_FORMAT_NV12, kDesiredSize);
}

TEST_F(VideoTrackAdapterFixtureTest,
       DeliversSWFrameVisibleRectWithEvenOriginAndHeight) {
  const gfx::Size kCodedSize(1280, 720);
  const gfx::Rect kVisibleRect(0, 0, 1280, 720);
  const gfx::Size kDesiredSize(1280, 718);
  const gfx::Size kNaturalSize(1280, 720);
  TestDeliversFrameWithVisibleRectWithEvenOriginAndSize(
      CreateTestFrame(kCodedSize, kVisibleRect, kNaturalSize,
                      media::VideoFrame::STORAGE_OWNED_MEMORY, test_sii_.get()),
      media::PIXEL_FORMAT_I420, kDesiredSize);
}

TEST_F(VideoTrackAdapterFixtureTest,
       DeliversSWFrameOddVisibleRectWithEvenOriginAndHeight) {
  const gfx::Size kCodedSize(1280, 720);
  const gfx::Rect kVisibleRect(0, 0, 1279, 719);
  const gfx::Size kDesiredSize(1024, 600);
  const gfx::Size kNaturalSize(1280, 720);
  TestDeliversFrameWithVisibleRectWithEvenOriginAndSize(
      CreateTestFrame(kCodedSize, kVisibleRect, kNaturalSize,
                      media::VideoFrame::STORAGE_OWNED_MEMORY, test_sii_.get()),
      media::PIXEL_FORMAT_I420, kDesiredSize);
}

// Tests that we run the |settings_callback| for any additional tracks that
// share a VideoFrameResolutionAdapter with an existing track. This ensures that
// the additional track's default frame_size and frame_rate are updated to match
// incoming frames.
TEST_F(VideoTrackAdapterFixtureTest,
       DeliverPortraitFrame_RunSettingsCallbackForSecondTrack) {
  // Attributes for initial track's incoming frame.
  const gfx::Size kCodedSize(480, 640);
  const gfx::Rect kVisibleRect(0, 0, 480, 640);
  const gfx::Size kNaturalSize(480, 640);
  const double kFrameRate = 30.0;
  auto test_frame =
      CreateTestFrame(kCodedSize, kVisibleRect, kNaturalSize,
                      /*storage_type=*/media::VideoFrame::STORAGE_OWNED_MEMORY,
                      test_sii_.get());

  const media::VideoCaptureFormat stream_format(kCodedSize, kFrameRate,
                                                media::PIXEL_FORMAT_I420);
  CreateAdapter(stream_format);

  // We don't provide a target size for the initial track.
  VideoTrackAdapterSettings adapter_settings(
      /*target_size=*/std::nullopt,
      /*min_aspect_ratio=*/0.0000,
      /*max_aspect_ratio=*/640.0000, kFrameRate);
  ConfigureTrack(adapter_settings);
  // The delivered frame for the first track should have a portrait orientation.
  auto check_portrait =
      [&](scoped_refptr<media::VideoFrame> frame,
          base::TimeTicks estimated_capture_time) {
        // We should get the original frame as-is here.
        EXPECT_EQ(frame->storage_type(),
                  media::VideoFrame::STORAGE_OWNED_MEMORY);
        EXPECT_EQ(frame->coded_size(), kCodedSize);
        EXPECT_EQ(frame->visible_rect(), kVisibleRect);
        EXPECT_EQ(frame->natural_size(), kNaturalSize);
      };
  SetFrameValidationCallback(base::BindLambdaForTesting(check_portrait));
  DeliverAndValidateFrame(test_frame, base::TimeTicks());

  base::WaitableEvent settings_callback_run_;
  // This lambda callback method validates that the |settings_callback|
  // (MediaStreamVideoTrack::SetVideoFrameSettings) is run with the
  // frame_size & frame_rate stored in the adapter's |track_settings_|. These
  // values should have been set when we delivered a frame for the first
  // track.
  auto check_dimensions = [&](gfx::Size frame_size, double frame_rate,
                              std::optional<gfx::Size> metadata_source_size,
                              std::optional<float> device_scale_factor) {
    EXPECT_EQ(frame_size, kNaturalSize);
    EXPECT_EQ(frame_rate, kFrameRate);
    EXPECT_EQ(metadata_source_size, std::nullopt);
    EXPECT_EQ(device_scale_factor, std::nullopt);
    settings_callback_run_.Signal();
  };

  // Add an additional track with the same |adapter_settings| as the first
  // track. Because of this it should share a VideoFrameResolutionAdapter with
  // the first track. The lambda callback method should run as part of the
  // VideoTrackAdapter::AddTrack logic.
  std::unique_ptr<MediaStreamVideoTrack> second_track;
  MediaStreamVideoSourceCallbacks video_stream_fallbacks;
  video_stream_fallbacks.deliver_frame_cb = base::DoNothing();
  video_stream_fallbacks.frame_dropped_cb = base::DoNothing();
  video_stream_fallbacks.encoded_frame_cb = base::DoNothing();
  video_stream_fallbacks.settings_cb =
      base::BindLambdaForTesting(check_dimensions);
  video_stream_fallbacks.capture_version_cb = base::DoNothing();
  video_stream_fallbacks.format_cb = base::DoNothing();
  PostCrossThreadTask(
      *testing_render_thread_.task_runner(), FROM_HERE,
      CrossThreadBindOnce(&VideoTrackAdapter::AddTrack, adapter_,
                          CrossThreadUnretained(second_track.get()),
                          std::move(video_stream_fallbacks), adapter_settings));
  settings_callback_run_.Wait();
}

// Tests that we run the |settings_callback| for a track.
TEST_F(VideoTrackAdapterFixtureTest, SettingsCallback) {
  // Attributes for initial track's incoming frame.
  const gfx::Size kCodedSize(480, 640);
  const gfx::Rect kVisibleRect(0, 0, 480, 640);
  const double kFrameRate = 30.0;
  auto test_frame =
      CreateTestFrame(kCodedSize, kVisibleRect, kCodedSize,
                      /*storage_type=*/media::VideoFrame::STORAGE_OWNED_MEMORY,
                      test_sii_.get());
  media::VideoFrameMetadata metadata;
  metadata.source_size = kCodedSize;
  metadata.device_scale_factor = 2.0f;
  test_frame->set_metadata(metadata);
  const media::VideoCaptureFormat stream_format(kCodedSize, kFrameRate,
                                                media::PIXEL_FORMAT_I420);
  CreateAdapter(stream_format);

  // We don't provide a target size for the initial track.
  VideoTrackAdapterSettings adapter_settings(
      /*target_size=*/std::nullopt,
      /*min_aspect_ratio=*/0.0000,
      /*max_aspect_ratio=*/640.0000, kFrameRate);
  ConfigureTrack(adapter_settings);

  auto check_dimensions = [&](gfx::Size frame_size, double frame_rate,
                              std::optional<gfx::Size> metadata_source_size,
                              std::optional<float> device_scale_factor) {
    EXPECT_EQ(frame_size, kCodedSize);
    EXPECT_EQ(frame_rate, kFrameRate);
    EXPECT_EQ(metadata_source_size, kCodedSize);
    EXPECT_EQ(device_scale_factor, 2.0f);
  };
  SetFrameSettingsCallback(base::BindLambdaForTesting(check_dimensions));
  DeliverAndValidateFrame(test_frame, base::TimeTicks());
}

TEST_F(VideoTrackAdapterFixtureTest, FrameRateReduction) {
  const double kInputFrameRate = 30.0;
  const double kTargetFrameRate = 10.0;
  const int kNumFrames = 1000;

  auto [num_delivered, num_dropped] =
      GenerateAndCountFrames(kNumFrames, kTargetFrameRate, kInputFrameRate);
  EXPECT_EQ(num_delivered + num_dropped, kNumFrames);
  EXPECT_TRUE(base::IsApproximatelyEqual(
      static_cast<double>(num_delivered) / kNumFrames,
      kTargetFrameRate / kInputFrameRate, /*tolerance=*/0.05));
}

TEST_F(VideoTrackAdapterFixtureTest, ArbitraryFrameRateReduction) {
  const double kInputFrameRate = 22.0;
  const double kTargetFrameRate = 10.0;
  const int kNumFrames = 1000;

  auto [num_delivered, num_dropped] =
      GenerateAndCountFrames(kNumFrames, kTargetFrameRate, kInputFrameRate);
  EXPECT_EQ(num_delivered + num_dropped, kNumFrames);
  EXPECT_NEAR(static_cast<double>(num_delivered) / kNumFrames * kInputFrameRate,
              kTargetFrameRate, /*abs_error=*/0.5);
}

TEST_F(VideoTrackAdapterFixtureTest, StreamWithJitterFrameRateReduction) {
  const double kInputFrameRate = 10.0;
  const double kTargetFrameRate = 5.0;
  const int kNumFrames = 1000;

  // Creates a 10 fps timestamp series: 0, 10 ms, 200 ms, 210 ms, ...
  auto index_to_timestamp =
      base::BindLambdaForTesting([kTargetFrameRate](int i) {
        return (i / 2) * base::Seconds(1.0 / kTargetFrameRate) +
               (i % 2) * base::Seconds(0.05 / kTargetFrameRate);
      });
  auto [num_delivered, num_dropped] =
      GenerateAndCountFrames(kNumFrames, kTargetFrameRate, index_to_timestamp);

  EXPECT_EQ(num_delivered + num_dropped, kNumFrames);
  EXPECT_NEAR(static_cast<double>(num_delivered) / kNumFrames * kInputFrameRate,
              kTargetFrameRate, /*abs_error=*/0.5);
}

TEST_F(VideoTrackAdapterFixtureTest, DoNotDropFramesIfFrameRatesMatch) {
  const int kNumFrames = 1000;
  auto [num_delivered, num_dropped] = GenerateAndCountFrames(
      kNumFrames, /*target_frame_rate=*/5.0, /*actual_input_frame_rate=*/5.0);
  EXPECT_EQ(num_delivered, kNumFrames);
  EXPECT_EQ(num_dropped, 0);
}

TEST_F(VideoTrackAdapterFixtureTest, FrameRateReductionSlightMismatch) {
  const double kInputFrameRate = 10.4;
  const double kTargetFrameRate = 10.0;
  const int kNumFrames = 1000;
  auto [num_delivered, num_dropped] =
      GenerateAndCountFrames(kNumFrames, kInputFrameRate, kTargetFrameRate);
  EXPECT_EQ(num_delivered + num_dropped, kNumFrames);
  EXPECT_NEAR(static_cast<double>(num_delivered) / kNumFrames * kInputFrameRate,
              kTargetFrameRate, /*abs_error=*/0.5);
}

TEST_F(VideoTrackAdapterFixtureTest, DoNotDropFramesIfNoTargetFrameRate) {
  const int kNumFrames = 100;
  auto [num_delivered, num_dropped] =
      GenerateAndCountFrames(kNumFrames, /*target_frame_rate=*/std::nullopt,
                             /*actual_input_frame_rate=*/10.0);
  EXPECT_EQ(num_delivered, kNumFrames);
  EXPECT_EQ(num_dropped, 0);
}

TEST_F(VideoTrackAdapterFixtureTest, DropFramesIfTargetFrameRateIsInfinite) {
  const int kNumFrames = 100;
  auto [num_delivered, num_dropped] = GenerateAndCountFrames(
      kNumFrames, /*target_frame_rate=*/std::numeric_limits<double>::infinity(),
      /*actual_input_frame_rate=*/10.0);
  EXPECT_EQ(num_delivered, kNumFrames);
  EXPECT_EQ(num_dropped, 0);
}

TEST_F(VideoTrackAdapterFixtureTest,
       DoNotDropFramesIfTimeBetweenFramesIsNegative) {
  const int kNumFrames = 100;
  auto [num_delivered, num_dropped] =
      GenerateAndCountFrames(kNumFrames, /*target_frame_rate=*/10.0,
                             /*actual_input_frame_rate=*/-10.0);
  EXPECT_EQ(num_delivered, kNumFrames);
  EXPECT_EQ(num_dropped, 0);
}

TEST_F(VideoTrackAdapterFixtureTest,
       DoNotDropFramesIfTimeBetweenFramesIsTooLong) {
  const int kNumFrames = 100;
  auto [num_delivered, num_dropped] =
      GenerateAndCountFrames(kNumFrames, /*target_frame_rate=*/10.0,
                             /*actual_input_frame_rate=*/0.0000001);
  EXPECT_EQ(num_delivered, kNumFrames);
  EXPECT_EQ(num_dropped, 0);
}

TEST_F(VideoTrackAdapterFixtureTest, DropFramesIfTimeBetweenFramesIsTooShort) {
  const int kNumFrames = 100;
  auto [num_delivered, num_dropped] =
      GenerateAndCountFrames(kNumFrames, /*target_frame_rate=*/10.0,
                             /*actual_input_frame_rate=*/10000000.0);
  EXPECT_EQ(num_delivered, 1);
  EXPECT_EQ(num_dropped, kNumFrames - 1);
}

TEST_F(VideoTrackAdapterFixtureTest, DropFramesIfTimeBetweenFramesIsZero) {
  const int kNumFrames = 100;
  auto [num_delivered, num_dropped] = GenerateAndCountFrames(
      kNumFrames, /*target_frame_rate=*/10.0,
      /*actual_input_frame_rate=*/std::numeric_limits<double>::infinity());
  EXPECT_EQ(num_delivered, 1);
  EXPECT_EQ(num_dropped, kNumFrames - 1);
}

// A black frame synthesized when capture is paused must reach the track even
// though a real frame was just delivered and the source has a max frame rate.
// It is a security control rather than a content update, so it must not be
// rate limited, and it must not appear to travel backwards in time.
TEST_F(VideoTrackAdapterFixtureTest, BlackFrameIsDeliveredAndStamped) {
  const gfx::Size kSize(640, 480);
  const double kFrameRate = 30.0;
  CreateAdapter(
      media::VideoCaptureFormat(kSize, kFrameRate, media::PIXEL_FORMAT_I420));
  ConfigureTrack(VideoTrackAdapterSettings(/*target_size=*/std::nullopt,
                                           /*min_aspect_ratio=*/0.0,
                                           /*max_aspect_ratio=*/640.0,
                                           kFrameRate));

  // Deliver a real frame first. This arms the frame rate limiter and
  // establishes the timeline that the black frame has to continue.
  const base::TimeDelta kRealFrameTimestamp = base::Seconds(1);
  auto real_frame =
      CreateTestFrame(kSize, gfx::Rect(kSize), kSize,
                      /*storage_type=*/media::VideoFrame::STORAGE_OWNED_MEMORY,
                      test_sii_.get());
  real_frame->set_timestamp(kRealFrameTimestamp);
  SetFrameValidationCallback(base::BindLambdaForTesting(
      [](scoped_refptr<media::VideoFrame> frame, base::TimeTicks) {}));
  DeliverAndValidateFrame(real_frame, base::TimeTicks());

  // The black frame follows immediately afterwards. A real frame arriving this
  // soon would be discarded as
  // kResolutionAdapterFrameRateIsHigherThanRequested.
  const media::CaptureVersion kCaptureVersion(1, 0);
  scoped_refptr<media::VideoFrame> delivered_frame;
  SetFrameValidationCallback(base::BindLambdaForTesting(
      [&](scoped_refptr<media::VideoFrame> frame, base::TimeTicks) {
        delivered_frame = std::move(frame);
      }));
  SetFrameDroppedCallback(
      base::BindLambdaForTesting([](media::VideoCaptureFrameDropReason reason) {
        ADD_FAILURE() << "Black frame was dropped, reason "
                      << static_cast<int>(reason);
      }));
  DeliverBlackFrameAndValidate(kSize, kCaptureVersion, base::TimeTicks::Now());

  ASSERT_TRUE(delivered_frame);
  EXPECT_EQ(delivered_frame->natural_size(), kSize);
  // Carries the source's capture version, so MediaStreamVideoTrack does not
  // discard it as kOldCaptureVersion.
  EXPECT_EQ(delivered_frame->metadata().capture_version, kCaptureVersion);
  // Continues the timeline rather than restarting at zero.
  EXPECT_GT(delivered_frame->timestamp(), kRealFrameTimestamp);
}

class VideoTrackAdapterEncodedTest : public ::testing::Test {
 public:
  VideoTrackAdapterEncodedTest()
      : render_thread_("VideoTrackAdapterEncodedTest_RenderThread") {
    render_thread_.Start();
    auto source = std::make_unique<MockMediaStreamVideoSource>(
        media::VideoCaptureFormat(), false);
    mock_source_ = source.get();
    web_source_.Initialize(
        blink::WebString::FromAscii("source_id"),
        blink::WebMediaStreamSource::kTypeVideo,
        blink::WebString::FromAscii("DeliverEncodedVideoFrameSource"),
        false /* remote */, std::move(source));
    RunSyncOnRenderThread([&] {
      adapter_ = base::MakeRefCounted<VideoTrackAdapter>(
          platform_support_->GetIOTaskRunner(),
          /*is_video_desktop_capture_type=*/false);
    });
  }

  ~VideoTrackAdapterEncodedTest() override {
    web_source_.Reset();
    WebHeap::CollectAllGarbageForTesting();
  }

  std::unique_ptr<MediaStreamVideoTrack> AddTrack(
      VideoTrackSettingsCallback settings_cb = base::DoNothing(),
      VideoTrackFormatCallback format_cb = base::DoNothing(),
      VideoTrackAdapterSettings adapter_settings =
          VideoTrackAdapterSettings()) {
    auto track = std::make_unique<MediaStreamVideoTrack>(
        mock_source_, WebPlatformMediaStreamSource::ConstraintsOnceCallback(),
        true);

    MediaStreamVideoSourceCallbacks video_stream_fallbacks;
    video_stream_fallbacks.deliver_frame_cb =
        base::BindRepeating(&VideoTrackAdapterEncodedTest::OnFrameDelivered,
                            base::Unretained(this));
    video_stream_fallbacks.frame_dropped_cb = base::DoNothing();
    video_stream_fallbacks.encoded_frame_cb = base::BindRepeating(
        &VideoTrackAdapterEncodedTest::OnEncodedVideoFrameDelivered,
        base::Unretained(this));
    video_stream_fallbacks.settings_cb = std::move(settings_cb);
    video_stream_fallbacks.capture_version_cb = base::DoNothing();
    video_stream_fallbacks.format_cb = std::move(format_cb);
    RunSyncOnRenderThread([&] {
      adapter_->AddTrack(track.get(), std::move(video_stream_fallbacks),
                         adapter_settings);
    });
    return track;
  }

  std::unique_ptr<MediaStreamVideoTrack> AddTrackWithCallbacks(
      MediaStreamVideoSourceCallbacks video_stream_fallbacks,
      VideoTrackAdapterSettings adapter_settings =
          VideoTrackAdapterSettings()) {
    auto track = std::make_unique<MediaStreamVideoTrack>(
        mock_source_, WebPlatformMediaStreamSource::ConstraintsOnceCallback(),
        true);
    RunSyncOnRenderThread([&] {
      adapter_->AddTrack(track.get(), std::move(video_stream_fallbacks),
                         adapter_settings);
    });
    return track;
  }

  template <class Function>
  void RunSyncOnRenderThread(Function function) {
    base::RunLoop run_loop;
    base::OnceClosure quit_closure = run_loop.QuitClosure();
    render_thread_.task_runner()->PostTask(
        FROM_HERE, base::BindLambdaForTesting([&] {
          std::move(function)();
          platform_support_->GetIOTaskRunner()->PostTask(
              FROM_HERE, std::move(quit_closure));
        }));
    run_loop.Run();
  }

  // Delivers `frame` to the adapter on the IO task runner and blocks until it
  // has been fanned out to the tracks, so that the caller can assert on the
  // results immediately afterwards.
  void DeliverFrameAndWait(scoped_refptr<media::VideoFrame> frame) {
    base::RunLoop run_loop;
    base::OnceClosure quit_closure = run_loop.QuitClosure();
    platform_support_->GetIOTaskRunner()->PostTask(
        FROM_HERE, base::BindLambdaForTesting([&]() {
          adapter_->DeliverFrameOnVideoTaskRunner(frame, base::TimeTicks());
          std::move(quit_closure).Run();
        }));
    run_loop.Run();
  }

  MOCK_METHOD2(OnFrameDelivered,
               void(scoped_refptr<media::VideoFrame> frame,
                    base::TimeTicks estimated_capture_time));
  MOCK_METHOD2(OnEncodedVideoFrameDelivered,
               void(scoped_refptr<EncodedVideoFrame>,
                    base::TimeTicks estimated_capture_time));

  // Expects exactly one frame delivery per entry in `expected_rects`, matched
  // on the delivered frame's visible rect. A delivery whose rect is not listed
  // -- including a second copy of one that is -- fails as an unexpected call,
  // and a listed rect that never arrives fails as an unsatisfied expectation.
  //
  // Frames are identified by rect rather than by track because the adapter
  // fans a single input frame out to every track, and the delivery callback
  // does not say which track a frame belongs to. The rects therefore have to
  // be distinct for a test to pin down per-track behaviour.
  void ExpectFramesMatching(const std::vector<gfx::Rect>& expected_rects) {
    for (const gfx::Rect& rect : expected_rects) {
      EXPECT_CALL(*this,
                  OnFrameDelivered(
                      Pointee(Property("visible_rect",
                                       &media::VideoFrame::visible_rect, rect)),
                      _))
          .Times(1);
    }
  }

 protected:
  ScopedTestingPlatformSupport<IOTaskRunnerTestingPlatformSupport>
      platform_support_;
  test::TaskEnvironment task_environment_;
  base::Thread render_thread_;
  WebMediaStreamSource web_source_;
  raw_ptr<MockMediaStreamVideoSource, DanglingUntriaged> mock_source_;
  scoped_refptr<VideoTrackAdapter> adapter_;
};

TEST_F(VideoTrackAdapterEncodedTest, DeliverEncodedVideoFrame) {
  auto track1 = AddTrack();
  auto track2 = AddTrack();
  EXPECT_CALL(*this, OnEncodedVideoFrameDelivered).Times(2);

  base::RunLoop run_loop;
  base::OnceClosure quit_closure = run_loop.QuitClosure();
  platform_support_->GetIOTaskRunner()->PostTask(
      FROM_HERE, base::BindLambdaForTesting([&]() {
        adapter_->DeliverEncodedVideoFrameOnVideoTaskRunner(
            base::MakeRefCounted<MockEncodedVideoFrame>(), base::TimeTicks());
        std::move(quit_closure).Run();
      }));
  run_loop.Run();
  RunSyncOnRenderThread([&] {
    adapter_->RemoveTrack(track1.get());
    adapter_->RemoveTrack(track2.get());
  });
}

TEST_F(VideoTrackAdapterEncodedTest,
       FormatCallbackNotTriggeredOnEveryFrameWhenScaling) {
  const gfx::Size kSourceSize(1280, 720);
  const gfx::Size kTargetSize(640, 360);
  int format_callback_calls = 0;

  auto format_cb =
      base::BindLambdaForTesting([&](const media::VideoCaptureFormat& format) {
        EXPECT_EQ(format.frame_size, kSourceSize);
        format_callback_calls++;
      });

  VideoTrackAdapterSettings adapter_settings(
      kTargetSize, /*min_aspect_ratio=*/0.0, /*max_aspect_ratio=*/640.0,
      /*max_frame_rate=*/std::nullopt);
  auto track = AddTrack(base::DoNothing(), format_cb, adapter_settings);
  EXPECT_CALL(*this, OnFrameDelivered).Times(4);

  for (int i = 0; i < 4; ++i) {
    scoped_refptr<media::VideoFrame> frame =
        media::VideoFrame::CreateZeroInitializedFrame(
            media::PIXEL_FORMAT_I420, kSourceSize, gfx::Rect(kSourceSize),
            kSourceSize, base::Milliseconds(i * 33));
    DeliverFrameAndWait(frame);
  }

  // Flush the render thread where format_cb is posted.
  RunSyncOnRenderThread([&] {});

  // Source resolution did not change across frames, so format_cb must only be
  // invoked once despite the track scaling frames down to 640x360.
  EXPECT_EQ(format_callback_calls, 1);

  RunSyncOnRenderThread([&] { adapter_->RemoveTrack(track.get()); });
}

TEST_F(VideoTrackAdapterEncodedTest, MultiTrackFrameRateCalculation) {
  const gfx::Size kFrameSize(640, 480);
  double track1_fps = 0.0;
  double track2_fps = 0.0;

  auto settings_cb1 = base::BindLambdaForTesting(
      [&](gfx::Size, double frame_rate, std::optional<gfx::Size>,
          std::optional<float>) { track1_fps = frame_rate; });
  auto settings_cb2 = base::BindLambdaForTesting(
      [&](gfx::Size, double frame_rate, std::optional<gfx::Size>,
          std::optional<float>) { track2_fps = frame_rate; });

  auto track1 = AddTrack(settings_cb1);
  auto track2 = AddTrack(settings_cb2);
  EXPECT_CALL(*this, OnFrameDelivered).Times(10);

  for (int i = 0; i < 5; ++i) {
    scoped_refptr<media::VideoFrame> frame =
        media::VideoFrame::CreateZeroInitializedFrame(
            media::PIXEL_FORMAT_I420, kFrameSize, gfx::Rect(kFrameSize),
            kFrameSize, base::Milliseconds(i * 33));
    DeliverFrameAndWait(frame);
  }

  // Flush the render thread where settings_cb is posted.
  RunSyncOnRenderThread([&] {});

  EXPECT_GT(track1_fps, 0.0);
  EXPECT_GT(track2_fps, 0.0);
  EXPECT_NEAR(track1_fps, track2_fps, 0.1);

  RunSyncOnRenderThread([&] {
    adapter_->RemoveTrack(track1.get());
    adapter_->RemoveTrack(track2.get());
  });
}

TEST_F(VideoTrackAdapterEncodedTest, DeliverFrameWithRegionCaptureBounds) {
  auto track1 = AddTrack();
  auto track2 = AddTrack();
  auto track3 = AddTrack();

  const base::Token token1(0x11111111, 0x22222222);
  const base::Token token2(0x33333333, 0x44444444);

  RunSyncOnRenderThread([&] {
    adapter_->SetTrackSubCaptureTarget(track1.get(), token1);
    adapter_->SetTrackSubCaptureTarget(track2.get(), token2);
  });

  ExpectFramesMatching({
      // track1's bounds are (11, 21, 101, 201); I420 chroma subsampling snaps
      // them to even origin and size.
      gfx::Rect(12, 22, 100, 200),
      gfx::Rect(50, 60, 200, 150),  // track2, already even.
      gfx::Rect(0, 0, 1920, 1080),  // track3, uncropped.
  });

  const gfx::Size kFrameSize(1920, 1080);
  scoped_refptr<media::VideoFrame> input_frame =
      media::VideoFrame::CreateZeroInitializedFrame(
          media::PIXEL_FORMAT_I420, kFrameSize, gfx::Rect(kFrameSize),
          kFrameSize, base::Milliseconds(10));

  base::flat_map<base::Token, gfx::Rect> bounds;
  bounds[token1] = gfx::Rect(11, 21, 101, 201);
  bounds[token2] = gfx::Rect(50, 60, 200, 150);
  input_frame->metadata().region_capture_bounds = bounds;

  DeliverFrameAndWait(input_frame);

  RunSyncOnRenderThread([&] {
    adapter_->RemoveTrack(track1.get());
    adapter_->RemoveTrack(track2.get());
    adapter_->RemoveTrack(track3.get());
  });
}

TEST_F(VideoTrackAdapterEncodedTest,
       DeliverFrameWithRegionCaptureBoundsRgbExactAlignment) {
  auto track1 = AddTrack();
  auto track2 = AddTrack();

  const base::Token token1(0x11111111, 0x22222222);

  RunSyncOnRenderThread(
      [&] { adapter_->SetTrackSubCaptureTarget(track1.get(), token1); });

  ExpectFramesMatching({
      // ARGB preserves exact odd coordinates and dimensions without 2-pixel
      // rounding, so track1 sees the bounds rect unmodified.
      gfx::Rect(11, 21, 101, 201),
      gfx::Rect(0, 0, 1920, 1080),  // track2, uncropped.
  });

  const gfx::Size kFrameSize(1920, 1080);
  scoped_refptr<media::VideoFrame> input_frame =
      media::VideoFrame::CreateZeroInitializedFrame(
          media::PIXEL_FORMAT_ARGB, kFrameSize, gfx::Rect(kFrameSize),
          kFrameSize, base::Milliseconds(10));

  base::flat_map<base::Token, gfx::Rect> bounds;
  bounds[token1] = gfx::Rect(11, 21, 101, 201);
  input_frame->metadata().region_capture_bounds = bounds;

  DeliverFrameAndWait(input_frame);

  RunSyncOnRenderThread([&] {
    adapter_->RemoveTrack(track1.get());
    adapter_->RemoveTrack(track2.get());
  });
}

TEST_F(VideoTrackAdapterEncodedTest,
       DeliverFrameWithEmptyOrOffscreenRegionCaptureBounds) {
  auto track1 = AddTrack();
  auto track2 = AddTrack();
  auto track3 = AddTrack();

  const base::Token token1(0x11111111, 0x22222222);
  const base::Token token2(0x33333333, 0x44444444);

  RunSyncOnRenderThread([&] {
    adapter_->SetTrackSubCaptureTarget(track1.get(), token1);
    adapter_->SetTrackSubCaptureTarget(track2.get(), token2);
  });

  // token1 is NOT present in bounds (simulating offscreen / 0px), so track1
  // must not be delivered to at all; a frame for it would show up here as an
  // unexpected call.
  ExpectFramesMatching({
      gfx::Rect(50, 60, 200, 150),  // track2, cropped to token2.
      gfx::Rect(0, 0, 1920, 1080),  // track3, uncropped.
  });

  const gfx::Size kFrameSize(1920, 1080);
  scoped_refptr<media::VideoFrame> input_frame =
      media::VideoFrame::CreateZeroInitializedFrame(
          media::PIXEL_FORMAT_I420, kFrameSize, gfx::Rect(kFrameSize),
          kFrameSize, base::Milliseconds(10));

  base::flat_map<base::Token, gfx::Rect> bounds;
  // token1 is omitted
  bounds[token2] = gfx::Rect(50, 60, 200, 150);
  input_frame->metadata().region_capture_bounds = bounds;

  DeliverFrameAndWait(input_frame);

  RunSyncOnRenderThread([&] {
    adapter_->RemoveTrack(track1.get());
    adapter_->RemoveTrack(track2.get());
    adapter_->RemoveTrack(track3.get());
  });
}

TEST_F(VideoTrackAdapterEncodedTest,
       DeliverFrameWithRegionCaptureBoundsSettingsCallbackOnlyOncePerSize) {
  int track1_settings_calls = 0;
  gfx::Size track1_last_size;
  auto track1_settings_cb = base::BindLambdaForTesting(
      [&](gfx::Size frame_size, double frame_rate,
          std::optional<gfx::Size> metadata_source_size,
          std::optional<float> device_scale_factor) {
        track1_settings_calls++;
        track1_last_size = frame_size;
      });

  int track2_settings_calls = 0;
  gfx::Size track2_last_size;
  auto track2_settings_cb = base::BindLambdaForTesting(
      [&](gfx::Size frame_size, double frame_rate,
          std::optional<gfx::Size> metadata_source_size,
          std::optional<float> device_scale_factor) {
        track2_settings_calls++;
        track2_last_size = frame_size;
      });

  auto track1 = AddTrack(track1_settings_cb);
  auto track2 = AddTrack(track2_settings_cb);

  const base::Token token1(0x11111111, 0x22222222);
  const base::Token token2(0x33333333, 0x44444444);

  RunSyncOnRenderThread([&] {
    adapter_->SetTrackSubCaptureTarget(track1.get(), token1);
    adapter_->SetTrackSubCaptureTarget(track2.get(), token2);
  });

  EXPECT_CALL(*this, OnFrameDelivered).Times(testing::AnyNumber());

  const gfx::Size kFrameSize(1920, 1080);
  base::flat_map<base::Token, gfx::Rect> bounds;
  bounds[token1] = gfx::Rect(10, 20, 100, 200);
  bounds[token2] = gfx::Rect(50, 60, 300, 400);

  auto deliver_frame = [&](base::TimeDelta timestamp) {
    scoped_refptr<media::VideoFrame> frame =
        media::VideoFrame::CreateZeroInitializedFrame(
            media::PIXEL_FORMAT_I420, kFrameSize, gfx::Rect(kFrameSize),
            kFrameSize, timestamp);
    frame->metadata().region_capture_bounds = bounds;
    DeliverFrameAndWait(frame);
  };

  // Deliver frame 1. Both tracks should receive an initial settings callback.
  deliver_frame(base::Milliseconds(33));
  EXPECT_EQ(track1_settings_calls, 1);
  EXPECT_EQ(track1_last_size, gfx::Size(100, 200));
  EXPECT_EQ(track2_settings_calls, 1);
  EXPECT_EQ(track2_last_size, gfx::Size(300, 400));

  // Deliver subsequent frames with the same dimensions. Settings callbacks
  // must NOT be invoked again (no thrashing between tracks).
  deliver_frame(base::Milliseconds(66));
  deliver_frame(base::Milliseconds(99));
  EXPECT_EQ(track1_settings_calls, 1);
  EXPECT_EQ(track2_settings_calls, 1);

  // Now change bounds for track1 only.
  bounds[token1] = gfx::Rect(10, 20, 160, 240);
  deliver_frame(base::Milliseconds(132));
  EXPECT_EQ(track1_settings_calls, 2);
  EXPECT_EQ(track1_last_size, gfx::Size(160, 240));
  EXPECT_EQ(track2_settings_calls, 1);

  RunSyncOnRenderThread([&] {
    adapter_->RemoveTrack(track1.get());
    adapter_->RemoveTrack(track2.get());
  });
}

TEST_F(VideoTrackAdapterEncodedTest,
       SetTrackSubCaptureTargetInheritsSettingsImmediately) {
  int track1_settings_calls = 0;
  gfx::Size track1_last_size;
  auto track1_settings_cb = base::BindLambdaForTesting(
      [&](gfx::Size frame_size, double frame_rate,
          std::optional<gfx::Size> metadata_source_size,
          std::optional<float> device_scale_factor) {
        track1_settings_calls++;
        track1_last_size = frame_size;
      });

  int track2_settings_calls = 0;
  gfx::Size track2_last_size;
  auto track2_settings_cb = base::BindLambdaForTesting(
      [&](gfx::Size frame_size, double frame_rate,
          std::optional<gfx::Size> metadata_source_size,
          std::optional<float> device_scale_factor) {
        track2_settings_calls++;
        track2_last_size = frame_size;
      });

  auto track1 = AddTrack(track1_settings_cb);
  auto track2 = AddTrack(track2_settings_cb);

  const base::Token token1(0x11111111, 0x22222222);

  RunSyncOnRenderThread(
      [&] { adapter_->SetTrackSubCaptureTarget(track1.get(), token1); });

  EXPECT_CALL(*this, OnFrameDelivered).Times(testing::AnyNumber());

  const gfx::Size kFrameSize(1920, 1080);
  base::flat_map<base::Token, gfx::Rect> bounds;
  bounds[token1] = gfx::Rect(10, 20, 100, 200);

  scoped_refptr<media::VideoFrame> frame =
      media::VideoFrame::CreateZeroInitializedFrame(
          media::PIXEL_FORMAT_I420, kFrameSize, gfx::Rect(kFrameSize),
          kFrameSize, base::Milliseconds(33));
  frame->metadata().region_capture_bounds = bounds;

  DeliverFrameAndWait(frame);

  EXPECT_EQ(track1_settings_calls, 1);
  EXPECT_EQ(track1_last_size, gfx::Size(100, 200));
  EXPECT_EQ(track2_settings_calls, 1);
  EXPECT_EQ(track2_last_size, gfx::Size(1920, 1080));

  // Now track2 changes target to token1. It should immediately inherit
  // track1's settings (100, 200) without waiting for a new frame.
  RunSyncOnRenderThread(
      [&] { adapter_->SetTrackSubCaptureTarget(track2.get(), token1); });

  EXPECT_EQ(track2_settings_calls, 2);
  EXPECT_EQ(track2_last_size, gfx::Size(100, 200));

  RunSyncOnRenderThread([&] {
    adapter_->RemoveTrack(track1.get());
    adapter_->RemoveTrack(track2.get());
  });
}

TEST_F(VideoTrackAdapterEncodedTest,
       DeliverFrameDefensiveCheckWhenRegionCaptureBoundsMissing) {
  int track1_deliveries = 0;
  int track2_deliveries = 0;

  MediaStreamVideoSourceCallbacks cb1;
  cb1.deliver_frame_cb =
      base::BindLambdaForTesting([&](scoped_refptr<media::VideoFrame>,
                                     base::TimeTicks) { track1_deliveries++; });
  cb1.frame_dropped_cb = base::DoNothing();
  cb1.encoded_frame_cb = base::DoNothing();
  cb1.settings_cb = base::DoNothing();
  cb1.capture_version_cb = base::DoNothing();
  cb1.format_cb = base::DoNothing();

  MediaStreamVideoSourceCallbacks cb2;
  cb2.deliver_frame_cb =
      base::BindLambdaForTesting([&](scoped_refptr<media::VideoFrame>,
                                     base::TimeTicks) { track2_deliveries++; });
  cb2.frame_dropped_cb = base::DoNothing();
  cb2.encoded_frame_cb = base::DoNothing();
  cb2.settings_cb = base::DoNothing();
  cb2.capture_version_cb = base::DoNothing();
  cb2.format_cb = base::DoNothing();

  auto track1 = AddTrackWithCallbacks(std::move(cb1));
  auto track2 = AddTrackWithCallbacks(std::move(cb2));

  const base::Token token1(0x11111111, 0x22222222);

  RunSyncOnRenderThread([&] {
    // track1 is cropped, track2 is uncropped
    adapter_->SetTrackSubCaptureTarget(track1.get(), token1);
  });

  const gfx::Size kFrameSize(1920, 1080);

  // Deliver a frame WITHOUT region_capture_bounds and WITHOUT
  // region_capture_rect. track1 (cropped) must NOT receive it; track2
  // (uncropped) MUST receive it.
  {
    scoped_refptr<media::VideoFrame> frame =
        media::VideoFrame::CreateZeroInitializedFrame(
            media::PIXEL_FORMAT_I420, kFrameSize, gfx::Rect(kFrameSize),
            kFrameSize, base::Milliseconds(33));
    DeliverFrameAndWait(frame);
  }

  EXPECT_EQ(track1_deliveries, 0);
  EXPECT_EQ(track2_deliveries, 1);

  // Deliver a frame WITH region_capture_rect cropped to a DIFFERENT target
  // (token2). Neither track1 (cropped to token1) nor track2 (uncropped) may
  // receive it.
  const base::Token token2(0x33333333, 0x44444444);
  {
    scoped_refptr<media::VideoFrame> frame =
        media::VideoFrame::CreateZeroInitializedFrame(
            media::PIXEL_FORMAT_I420, gfx::Size(200, 100), gfx::Rect(200, 100),
            gfx::Size(200, 100), base::Milliseconds(66));
    frame->metadata().region_capture_rect = gfx::Rect(0, 0, 200, 100);
    frame->metadata().region_capture_bounds = {
        {token2, gfx::Rect(0, 0, 200, 100)}};
    DeliverFrameAndWait(frame);
  }

  EXPECT_EQ(track1_deliveries, 0);
  EXPECT_EQ(track2_deliveries, 1);

  // Deliver a frame WITH region_capture_rect cropped to token1 (simulating GPU
  // single-target crop to token1). track1 (cropped to token1) MUST receive it;
  // track2 (uncropped) must NOT receive it.
  {
    scoped_refptr<media::VideoFrame> frame =
        media::VideoFrame::CreateZeroInitializedFrame(
            media::PIXEL_FORMAT_I420, gfx::Size(200, 100), gfx::Rect(200, 100),
            gfx::Size(200, 100), base::Milliseconds(99));
    frame->metadata().region_capture_rect = gfx::Rect(0, 0, 200, 100);
    frame->metadata().region_capture_bounds = {
        {token1, gfx::Rect(0, 0, 200, 100)}};
    DeliverFrameAndWait(frame);
  }

  EXPECT_EQ(track1_deliveries, 1);
  EXPECT_EQ(track2_deliveries, 1);

  RunSyncOnRenderThread([&] {
    adapter_->RemoveTrack(track1.get());
    adapter_->RemoveTrack(track2.get());
  });
}

// MoltBrowser: the aspect ratio limit of tracks without one.
constexpr double kNoMaxAspectRatio = std::numeric_limits<double>::max();
// MoltBrowser: a MoltNet camera track's settings (moltnet_camera.h): its
// frames have exactly its target size, larger than the input too, rotated
// with the device. Other tracks' sizes are Chromium's.
TEST(VideoTrackAdapterTest, MoltNetCameraDesiredSizeIsTheTarget) {
  test::TaskEnvironment task_environment;
  VideoTrackAdapterSettings settings(gfx::Size(640, 480), 0.0,
                                     kNoMaxAspectRatio, 30.0);
  gfx::Size desired_size;
  // Chromium's: never larger than the input.
  EXPECT_TRUE(VideoTrackAdapter::CalculateDesiredSize(
      false /* is_rotated */, gfx::Size(320, 240), settings, &desired_size));
  EXPECT_EQ(desired_size, gfx::Size(320, 240));

  settings.set_molt_uniform_camera(/*reports_no_resize=*/false);
  EXPECT_TRUE(settings.molt_uniform_camera());
  EXPECT_FALSE(settings.molt_reports_no_resize());
  for (const gfx::Size input :
       {gfx::Size(320, 240), gfx::Size(1920, 1080), gfx::Size(0, 0)}) {
    EXPECT_TRUE(VideoTrackAdapter::CalculateDesiredSize(
        false /* is_rotated */, input, settings, &desired_size));
    EXPECT_EQ(desired_size, gfx::Size(640, 480));
  }
  EXPECT_TRUE(VideoTrackAdapter::CalculateDesiredSize(
      true /* is_rotated */, gfx::Size(1280, 720), settings, &desired_size));
  EXPECT_EQ(desired_size, gfx::Size(480, 640));

  // The mark is part of the settings: a track adapter keeps a marked track
  // apart from an unmarked one of the same size.
  EXPECT_FALSE(settings ==
               VideoTrackAdapterSettings(gfx::Size(640, 480), 0.0,
                                         kNoMaxAspectRatio, 30.0));
  VideoTrackAdapterSettings no_resize(gfx::Size(640, 480), 0.0,
                                      kNoMaxAspectRatio, 30.0);
  no_resize.set_molt_uniform_camera(/*reports_no_resize=*/true);
  EXPECT_TRUE(no_resize.molt_reports_no_resize());
  EXPECT_FALSE(settings == no_resize);
  VideoTrackAdapterSettings copy = no_resize;
  EXPECT_TRUE(copy == no_resize);
}

// MoltBrowser: a MoltNet camera track's frames (moltnet_camera.h) are new
// I420 frames of exactly the track's size, cropped to its aspect ratio and
// scaled, up as well as down, where Chromium hands the camera's own frame on
// with a smaller visible part (which a VideoFrame from
// MediaStreamTrackProcessor shows as codedWidth and visibleRect). A frame
// that already is one is handed on as it is.
TEST_F(VideoTrackAdapterFixtureTest, MoltNetCameraFramesHaveTheTrackSize) {
  const double kFrameRate = 30.0;
  CreateAdapter(media::VideoCaptureFormat(gfx::Size(1280, 720), kFrameRate,
                                          media::PIXEL_FORMAT_NV12));
  UseManualMoltCameraTicks();
  // Each frame below reaches the track at the next tick.
  auto deliver = [&](scoped_refptr<media::VideoFrame> frame) {
    DeliverMoltCameraFrame(std::move(frame));
    MoltCameraTickAndWait();
  };
  const gfx::Size kSize(640, 480);
  VideoTrackAdapterSettings settings(kSize, 0.0, kNoMaxAspectRatio,
                                     kFrameRate);
  settings.set_molt_uniform_camera(/*reports_no_resize=*/false);
  ConfigureTrack(settings);

  scoped_refptr<media::VideoFrame> delivered;
  SetFrameValidationCallback(base::BindLambdaForTesting(
      [&](scoped_refptr<media::VideoFrame> frame, base::TimeTicks) {
        delivered = std::move(frame);
      }));
  SetFrameDroppedCallback(
      base::BindLambdaForTesting([](media::VideoCaptureFrameDropReason reason) {
        ADD_FAILURE() << "Dropped, reason " << static_cast<int>(reason);
      }));
  auto expect_track_size = [&] {
    ASSERT_TRUE(delivered);
    EXPECT_EQ(delivered->format(), media::PIXEL_FORMAT_I420);
    EXPECT_EQ(delivered->coded_size(), kSize);
    EXPECT_EQ(delivered->visible_rect(), gfx::Rect(kSize));
    EXPECT_EQ(delivered->natural_size(), kSize);
  };
  // Frames 100 ms apart, which the track's 30 fps keeps.
  int frame_number = 0;
  auto next_timestamp = [&] {
    return base::Milliseconds(100) * ++frame_number;
  };

  // An HD camera's NV12 frame in a mappable shared image, part of it visible.
  auto shared_image_frame = CreateTestFrame(
      gfx::Size(1280, 960), gfx::Rect(0, 120, 1280, 720), gfx::Size(1280, 720),
      media::VideoFrame::STORAGE_MAPPABLE_SHARED_IMAGE,
      media::PIXEL_FORMAT_NV12, next_timestamp(), test_sii_.get());
  deliver(shared_image_frame);
  expect_track_size();
  EXPECT_EQ(delivered->storage_type(),
            media::VideoFrame::STORAGE_OWNED_MEMORY);
  // The grid's first tick, not the camera's timestamp.
  EXPECT_EQ(delivered->timestamp(), base::TimeDelta());

  // A small camera's I420 frame, scaled up.
  deliver(CreateTestFrame(gfx::Size(320, 240), gfx::Rect(320, 240),
                          gfx::Size(320, 240),
                          media::VideoFrame::STORAGE_OWNED_MEMORY,
                          media::PIXEL_FORMAT_I420, next_timestamp(),
                          test_sii_.get()));
  expect_track_size();
  EXPECT_EQ(delivered->timestamp(), base::Microseconds(33333));

  // A frame that already is one, its samples as they are.
  auto exact = CreateTestFrame(kSize, gfx::Rect(kSize), kSize,
                               media::VideoFrame::STORAGE_OWNED_MEMORY,
                               media::PIXEL_FORMAT_I420, next_timestamp(),
                               test_sii_.get());
  deliver(exact);
  expect_track_size();
  EXPECT_EQ(delivered->visible_data(0), exact->visible_data(0));

  // The captured surface size and scale Chromium's fake camera puts on its
  // frames (its own mode) don't reach the track, converted or not.
  for (const gfx::Size& camera_size : {gfx::Size(1280, 720), kSize}) {
    auto fake_camera_frame = CreateTestFrame(
        camera_size, gfx::Rect(camera_size), camera_size,
        media::VideoFrame::STORAGE_OWNED_MEMORY, media::PIXEL_FORMAT_I420,
        next_timestamp(), test_sii_.get());
    fake_camera_frame->metadata().source_size = camera_size;
    fake_camera_frame->metadata().device_scale_factor = 1.0;
    deliver(fake_camera_frame);
    expect_track_size();
    EXPECT_FALSE(delivered->metadata().source_size.has_value());
    EXPECT_FALSE(delivered->metadata().device_scale_factor.has_value());
    EXPECT_EQ(fake_camera_frame->metadata().source_size, camera_size);
  }

  // The crop: a 16:9 frame that is grey in its centred 4:3 part and black
  // beside it gives a grey frame.
  auto pillarboxed = CreateTestFrame(
      gfx::Size(1280, 720), gfx::Rect(1280, 720), gfx::Size(1280, 720),
      media::VideoFrame::STORAGE_OWNED_MEMORY, media::PIXEL_FORMAT_I420,
      next_timestamp(), test_sii_.get());
  for (size_t plane = 0; plane < 3; ++plane) {
    base::span<uint8_t> data = pillarboxed->GetWritableVisiblePlaneData(plane);
    const int rows = pillarboxed->GetVisibleRows(plane);
    const int columns = pillarboxed->GetVisibleRowBytes(plane);
    // The grey part, 160 to 1120 in luma samples.
    const int grey_begin = plane == 0 ? 160 : 80;
    const int grey_end = plane == 0 ? 1120 : 560;
    for (int y = 0; y < rows; ++y) {
      for (int x = 0; x < columns; ++x) {
        data[y * pillarboxed->stride(plane) + x] =
            plane != 0 ? 128 : (x >= grey_begin && x < grey_end ? 200 : 0);
      }
    }
  }
  deliver(pillarboxed);
  expect_track_size();
  {
    base::span<const uint8_t> luma = delivered->GetVisiblePlaneData(0);
    int not_grey = 0;
    for (int y = 0; y < delivered->GetVisibleRows(0); ++y) {
      for (int x = 0; x < delivered->GetVisibleRowBytes(0); ++x) {
        not_grey += luma[y * delivered->stride(0) + x] != 200;
      }
    }
    EXPECT_EQ(not_grey, 0);
  }

  // The same frames for a track of another size.
  const gfx::Size kHd(1280, 720);
  VideoTrackAdapterSettings hd(kHd, 0.0, kNoMaxAspectRatio, kFrameRate);
  hd.set_molt_uniform_camera(/*reports_no_resize=*/true);
  ConfigureTrack(hd);
  deliver(CreateTestFrame(gfx::Size(640, 480), gfx::Rect(640, 480),
                          gfx::Size(640, 480),
                          media::VideoFrame::STORAGE_OWNED_MEMORY,
                          media::PIXEL_FORMAT_I420, next_timestamp(),
                          test_sii_.get()));
  ASSERT_TRUE(delivered);
  EXPECT_EQ(delivered->coded_size(), kHd);
  EXPECT_EQ(delivered->visible_rect(), gfx::Rect(kHd));
  EXPECT_EQ(delivered->natural_size(), kHd);
}

// MoltBrowser: a MoltNet camera track's frames carry one colour space,
// BT.601 at limited range (SMPTE 170M), whatever the camera's: samples of
// another encoding are converted (media/base/molt_portable_color.h), those
// already in it are kept.
TEST(VideoTrackAdapterTest, MoltNetCameraFramesHaveOneColorSpace) {
  test::TaskEnvironment task_environment;
  media::VideoFramePool pool;
  media::VideoFrameConverter converter;
  const gfx::Size kSize(640, 480);

  // A camera frame of one colour, in `color_space`.
  auto camera_frame = [](media::VideoPixelFormat format, const gfx::Size& size,
                         const gfx::ColorSpace& color_space,
                         std::array<uint8_t, 4> samples) {
    scoped_refptr<media::VideoFrame> frame = media::VideoFrame::CreateFrame(
        format, size, gfx::Rect(size), size, base::TimeDelta());
    CHECK(frame);
    for (size_t plane = 0; plane < media::VideoFrame::NumPlanes(format);
         ++plane) {
      base::span<uint8_t> data = frame->writable_span(plane);
      const size_t per_element = static_cast<size_t>(
          media::VideoFrame::BytesPerElement(format, plane));
      for (size_t i = 0; i < data.size(); ++i) {
        // I420: one plane per sample; NV12: U and V interleaved in plane 1;
        // ARGB: B, G, R, A in memory.
        data[i] = per_element == 1 ? samples[plane]
                                   : samples[plane + i % per_element];
      }
    }
    frame->set_color_space(color_space);
    return frame;
  };
  // Whether every visible sample of `plane` is `value`.
  auto plane_is = [](const media::VideoFrame& frame, size_t plane,
                     int value) {
    base::span<const uint8_t> data = frame.GetVisiblePlaneData(plane);
    for (int y = 0; y < frame.GetVisibleRows(plane); ++y) {
      for (int x = 0; x < frame.GetVisibleRowBytes(plane); ++x) {
        if (data[static_cast<size_t>(y) * frame.stride(plane) +
                 static_cast<size_t>(x)] != value) {
          return false;
        }
      }
    }
    return true;
  };

  // An HD camera's NV12 in BT.709 (macOS): red, 63, 102, 240 there, becomes
  // 82, 90, 240 (BT.601's own red is 81.5, 90.2, 240).
  auto result = molt_fingerprinting::AdaptUniformCameraFrame(
      camera_frame(media::PIXEL_FORMAT_NV12, gfx::Size(1280, 720),
                   gfx::ColorSpace::CreateREC709(), {63, 102, 240, 0}),
      kSize, pool, converter);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ((*result)->format(), media::PIXEL_FORMAT_I420);
  EXPECT_EQ((*result)->ColorSpace(), gfx::ColorSpace::CreateREC601());
  EXPECT_TRUE(plane_is(**result, 0, 82));
  EXPECT_TRUE(plane_is(**result, 1, 90));
  EXPECT_TRUE(plane_is(**result, 2, 240));

  // A webcam's MJPEG, decoded to full-range BT.601 (JPEG) of the track's very
  // size: converted, white to 235, not handed on.
  scoped_refptr<media::VideoFrame> jpeg =
      camera_frame(media::PIXEL_FORMAT_I420, kSize,
                   gfx::ColorSpace::CreateJpeg(), {255, 128, 128, 0});
  result = molt_fingerprinting::AdaptUniformCameraFrame(jpeg, kSize, pool,
                                                         converter);
  ASSERT_TRUE(result.has_value());
  EXPECT_NE((*result)->visible_data(0), jpeg->visible_data(0));
  EXPECT_EQ((*result)->ColorSpace(), gfx::ColorSpace::CreateREC601());
  EXPECT_TRUE(plane_is(**result, 0, 235));
  EXPECT_TRUE(plane_is(**result, 1, 128));
  EXPECT_TRUE(plane_is(**result, 2, 128));

  // Already BT.601 at limited range (or without a colour space, read as
  // that), of the track's size: the same samples, relabelled.
  for (const gfx::ColorSpace& color_space :
       {gfx::ColorSpace::CreateREC601(), gfx::ColorSpace()}) {
    scoped_refptr<media::VideoFrame> same = camera_frame(
        media::PIXEL_FORMAT_I420, kSize, color_space, {50, 60, 70, 0});
    result = molt_fingerprinting::AdaptUniformCameraFrame(same, kSize, pool,
                                                           converter);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)->visible_data(0), same->visible_data(0));
    EXPECT_EQ((*result)->ColorSpace(), gfx::ColorSpace::CreateREC601());
    EXPECT_TRUE(plane_is(**result, 0, 50));
  }

  // An RGB camera's frame without a colour space: the converter makes it
  // BT.709 YUV, then BT.601. Red again, to a sample.
  result = molt_fingerprinting::AdaptUniformCameraFrame(
      camera_frame(media::PIXEL_FORMAT_ARGB, gfx::Size(1280, 720),
                   gfx::ColorSpace(), {0, 0, 255, 255}),
      kSize, pool, converter);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ((*result)->ColorSpace(), gfx::ColorSpace::CreateREC601());
  const uint8_t luma = (*result)->GetVisiblePlaneData(0)[0];
  const uint8_t cb = (*result)->GetVisiblePlaneData(1)[0];
  const uint8_t cr = (*result)->GetVisiblePlaneData(2)[0];
  EXPECT_NEAR(luma, 81.5, 1.0);
  EXPECT_NEAR(cb, 90.2, 1.0);
  EXPECT_NEAR(cr, 240.0, 1.0);
}

// MoltBrowser: a frame a MoltNet camera track can't read (a texture that
// can't be mapped) never reaches the track, in the camera's own size or
// otherwise; the grid's ticks hand on the newest frame it could read.
TEST_F(VideoTrackAdapterFixtureTest, MoltNetCameraDropsFramesItCannotRead) {
  SetUpMoltCameraTrack(gfx::Size(640, 480), 30.0);
  std::vector<scoped_refptr<media::VideoFrame>> delivered;
  SetFrameValidationCallback(base::BindLambdaForTesting(
      [&](scoped_refptr<media::VideoFrame> frame, base::TimeTicks) {
        delivered.push_back(std::move(frame));
      }));
  SetFrameDroppedCallback(
      base::BindLambdaForTesting([](media::VideoCaptureFrameDropReason reason) {
        ADD_FAILURE() << "Dropped, reason " << static_cast<int>(reason);
      }));
  auto unreadable = [&] {
    return CreateTestFrame(gfx::Size(1280, 720), gfx::Rect(1280, 720),
                           gfx::Size(1280, 720),
                           media::VideoFrame::STORAGE_OPAQUE,
                           media::PIXEL_FORMAT_NV12, base::Milliseconds(100),
                           test_sii_.get());
  };
  // Nothing readable yet: the tick hands on nothing.
  DeliverMoltCameraFrame(unreadable());
  MoltCameraTick();
  EXPECT_TRUE(delivered.empty());

  scoped_refptr<media::VideoFrame> readable = CreateTestFrame(
      gfx::Size(640, 480), gfx::Rect(640, 480), gfx::Size(640, 480),
      media::VideoFrame::STORAGE_OWNED_MEMORY, media::PIXEL_FORMAT_I420,
      base::Milliseconds(133), test_sii_.get());
  DeliverMoltCameraFrame(readable);
  MoltCameraTickAndWait();
  ASSERT_EQ(delivered.size(), 1u);
  // Another unreadable frame leaves the readable one.
  DeliverMoltCameraFrame(unreadable());
  MoltCameraTickAndWait();
  ASSERT_EQ(delivered.size(), 2u);
  EXPECT_EQ(delivered[1]->visible_data(0), readable->visible_data(0));
  EXPECT_EQ(delivered[1]->coded_size(), gfx::Size(640, 480));
}

// MoltBrowser: a MoltNet camera's frames reach its tracks on a fixed grid:
// whatever the camera's own timestamps, rate and jitter, the track's frames
// are stamped n/30 s after the first, with that time as their capture and
// reference time and the estimated capture time, the grid's rate, and none
// of the camera's own times, rate or effects.
TEST_F(VideoTrackAdapterFixtureTest, MoltNetCameraFramesReachTheGrid) {
  SetUpMoltCameraTrack(gfx::Size(640, 480), 30.0);
  struct Delivered {
    scoped_refptr<media::VideoFrame> frame;
    base::TimeTicks estimated_capture_time;
  };
  std::vector<Delivered> delivered;
  SetFrameValidationCallback(base::BindLambdaForTesting(
      [&](scoped_refptr<media::VideoFrame> frame, base::TimeTicks time) {
        delivered.push_back({std::move(frame), time});
      }));
  // A camera's jittery timestamps, with times and details of its own.
  const base::TimeDelta camera_timestamps[] = {
      base::Milliseconds(5000), base::Milliseconds(5047),
      base::Milliseconds(5051), base::Milliseconds(5130)};
  for (const base::TimeDelta& timestamp : camera_timestamps) {
    scoped_refptr<media::VideoFrame> frame = CreateTestFrame(
        gfx::Size(1280, 720), gfx::Rect(1280, 720), gfx::Size(1280, 720),
        media::VideoFrame::STORAGE_OWNED_MEMORY, media::PIXEL_FORMAT_I420,
        timestamp, test_sii_.get());
    const base::TimeTicks camera_time =
        base::TimeTicks() + base::Seconds(100) + timestamp;
    frame->metadata().capture_begin_time = camera_time;
    frame->metadata().capture_end_time = camera_time;
    frame->metadata().reference_time = camera_time;
    frame->metadata().receive_time = camera_time;
    frame->metadata().rtp_timestamp = 12345.0;
    frame->metadata().frame_rate = 24.0;
    frame->metadata().background_blur = media::EffectInfo{.enabled = true};
    DeliverMoltCameraFrame(frame);
    MoltCameraTickAndWait();
  }
  ASSERT_EQ(delivered.size(), 4u);
  const base::TimeTicks first = delivered[0].estimated_capture_time;
  for (size_t i = 0; i < delivered.size(); ++i) {
    const media::VideoFrame& frame = *delivered[i].frame;
    const base::TimeDelta grid = base::Microseconds(
        molt_fingerprinting::moltnet::UniformCameraTickMicroseconds(
            static_cast<int64_t>(i)));
    EXPECT_EQ(frame.timestamp(), grid) << i;
    EXPECT_EQ(delivered[i].estimated_capture_time, first + grid) << i;
    EXPECT_EQ(frame.metadata().capture_begin_time,
              delivered[i].estimated_capture_time);
    EXPECT_EQ(frame.metadata().reference_time,
              delivered[i].estimated_capture_time);
    EXPECT_EQ(frame.metadata().frame_rate, 30.0);
    EXPECT_FALSE(frame.metadata().capture_end_time.has_value());
    EXPECT_FALSE(frame.metadata().receive_time.has_value());
    EXPECT_FALSE(frame.metadata().rtp_timestamp.has_value());
    EXPECT_FALSE(frame.metadata().background_blur.has_value());
    EXPECT_EQ(frame.coded_size(), gfx::Size(640, 480));
  }
}

// MoltBrowser: without a new frame a tick hands the track the newest one
// again, as a new frame on the grid; after a second without one the grid
// stops, and the next frame resumes it.
TEST_F(VideoTrackAdapterFixtureTest, MoltNetCameraRepeatsTheNewestFrame) {
  SetUpMoltCameraTrack(gfx::Size(640, 480), 30.0);
  std::vector<scoped_refptr<media::VideoFrame>> delivered;
  SetFrameValidationCallback(base::BindLambdaForTesting(
      [&](scoped_refptr<media::VideoFrame> frame, base::TimeTicks) {
        delivered.push_back(std::move(frame));
      }));
  scoped_refptr<media::VideoFrame> frame = CreateTestFrame(
      gfx::Size(640, 480), gfx::Rect(640, 480), gfx::Size(640, 480),
      media::VideoFrame::STORAGE_OWNED_MEMORY, media::PIXEL_FORMAT_I420,
      base::Milliseconds(70), test_sii_.get());
  DeliverMoltCameraFrame(frame);
  // Ticks 0 to 30, within a second of the frame: the same pixels each time.
  for (int i = 0; i <= 30; ++i) {
    MoltCameraTickAndWait();
  }
  ASSERT_EQ(delivered.size(), 31u);
  for (size_t i = 0; i < delivered.size(); ++i) {
    EXPECT_EQ(delivered[i]->visible_data(0), frame->visible_data(0));
    EXPECT_EQ(delivered[i]->timestamp(),
              base::Microseconds(
                  molt_fingerprinting::moltnet::UniformCameraTickMicroseconds(
                      static_cast<int64_t>(i))));
  }
  // Tick 31 is more than a second after the frame: the grid stops.
  MoltCameraTick();
  MoltCameraTick();
  EXPECT_EQ(delivered.size(), 31u);
  // The next frame resumes it, on the same grid.
  DeliverMoltCameraFrame(frame);
  MoltCameraTickAndWait();
  ASSERT_EQ(delivered.size(), 32u);
  EXPECT_EQ(delivered.back()->timestamp(),
            base::Microseconds(
                molt_fingerprinting::moltnet::UniformCameraTickMicroseconds(
                    31)));
}

// MoltBrowser: a track that asked for a lower rate keeps the ticks it would
// keep of a camera at the grid's rate, and the others count as discarded
// (MediaStreamTrack.stats), the same on every machine.
TEST_F(VideoTrackAdapterFixtureTest, MoltNetCameraLowerRateKeepsGridTicks) {
  SetUpMoltCameraTrack(gfx::Size(640, 480), 15.0);
  std::vector<base::TimeDelta> delivered;
  std::vector<media::VideoCaptureFrameDropReason> dropped;
  SetFrameValidationCallback(base::BindLambdaForTesting(
      [&](scoped_refptr<media::VideoFrame> frame, base::TimeTicks) {
        delivered.push_back(frame->timestamp());
      }));
  SetFrameDroppedCallback(base::BindLambdaForTesting(
      [&](media::VideoCaptureFrameDropReason reason) {
        dropped.push_back(reason);
      }));
  for (int i = 0; i < 6; ++i) {
    DeliverMoltCameraFrame(CreateTestFrame(
        gfx::Size(640, 480), gfx::Rect(640, 480), gfx::Size(640, 480),
        media::VideoFrame::STORAGE_OWNED_MEMORY, media::PIXEL_FORMAT_I420,
        base::Milliseconds(29 * i), test_sii_.get()));
    MoltCameraTickAndWait();
  }
  EXPECT_EQ(delivered,
            (std::vector<base::TimeDelta>{base::Microseconds(0),
                                          base::Microseconds(66666),
                                          base::Microseconds(133333)}));
  ASSERT_EQ(dropped.size(), 3u);
  for (media::VideoCaptureFrameDropReason reason : dropped) {
    EXPECT_EQ(reason, media::VideoCaptureFrameDropReason::
                          kResolutionAdapterFrameRateIsHigherThanRequested);
  }
}

// MoltBrowser: the camera's own drops (a full buffer pool, a busy machine)
// don't reach a MoltNet camera's tracks: the grid hands them the newest
// frame instead.
TEST_F(VideoTrackAdapterFixtureTest, MoltNetCameraHidesTheCamerasDrops) {
  SetUpMoltCameraTrack(gfx::Size(640, 480), 30.0);
  int delivered = 0;
  SetFrameValidationCallback(base::BindLambdaForTesting(
      [&](scoped_refptr<media::VideoFrame>, base::TimeTicks) {
        ++delivered;
      }));
  SetFrameDroppedCallback(
      base::BindLambdaForTesting([](media::VideoCaptureFrameDropReason reason) {
        ADD_FAILURE() << "Dropped, reason " << static_cast<int>(reason);
      }));
  DeliverMoltCameraFrame(CreateTestFrame(
      gfx::Size(640, 480), gfx::Rect(640, 480), gfx::Size(640, 480),
      media::VideoFrame::STORAGE_OWNED_MEMORY, media::PIXEL_FORMAT_I420,
      base::TimeDelta(), test_sii_.get()));
  MoltCameraTickAndWait();
  base::WaitableEvent done;
  platform_support_->GetIOTaskRunner()->PostTask(
      FROM_HERE, base::BindLambdaForTesting([&] {
        adapter_->OnFrameDroppedOnVideoTaskRunner(
            media::VideoCaptureFrameDropReason::
                kBufferPoolMaxBufferCountExceeded);
        done.Signal();
      }));
  done.Wait();
  MoltCameraTickAndWait();
  EXPECT_EQ(delivered, 2);
}

// MoltBrowser: on its own the grid ticks on time: one frame from the camera,
// and the track gets it again and again, at the grid's timestamps (a tick
// that runs late skips the ones it missed).
TEST_F(VideoTrackAdapterFixtureTest, MoltNetCameraGridTicksOnItsOwn) {
  CreateAdapter(media::VideoCaptureFormat(gfx::Size(640, 480), 30.0,
                                          media::PIXEL_FORMAT_I420));
  VideoTrackAdapterSettings settings(gfx::Size(640, 480), 0.0,
                                     kNoMaxAspectRatio, 30.0);
  settings.set_molt_uniform_camera(/*reports_no_resize=*/true);
  ConfigureTrack(settings);
  base::Lock lock;
  std::vector<base::TimeDelta> timestamps;
  base::WaitableEvent three;
  SetFrameValidationCallback(base::BindLambdaForTesting(
      [&](scoped_refptr<media::VideoFrame> frame, base::TimeTicks) {
        base::AutoLock locked(lock);
        timestamps.push_back(frame->timestamp());
        if (timestamps.size() == 3) {
          three.Signal();
        }
      }));
  DeliverMoltCameraFrame(CreateTestFrame(
      gfx::Size(640, 480), gfx::Rect(640, 480), gfx::Size(640, 480),
      media::VideoFrame::STORAGE_OWNED_MEMORY, media::PIXEL_FORMAT_I420,
      base::Milliseconds(12345), test_sii_.get()));
  three.Wait();
  std::vector<base::TimeDelta> first_three;
  {
    base::AutoLock locked(lock);
    first_three.assign(timestamps.begin(), timestamps.begin() + 3);
  }
  EXPECT_EQ(first_three[0], base::TimeDelta());
  for (size_t i = 1; i < first_three.size(); ++i) {
    const int64_t us = first_three[i].InMicroseconds();
    const int64_t tick =
        molt_fingerprinting::moltnet::UniformCameraTickAtOrAfter(us);
    EXPECT_EQ(molt_fingerprinting::moltnet::UniformCameraTickMicroseconds(tick),
              us);
    EXPECT_GT(first_three[i], first_three[i - 1]);
  }
  // The ticks run on: remove the track before the test's locals go.
  base::WaitableEvent removed;
  testing_render_thread_.task_runner()->PostTask(
      FROM_HERE, base::BindLambdaForTesting([&] {
        adapter_->RemoveTrack(null_track_.get());
        platform_support_->GetIOTaskRunner()->PostTask(
            FROM_HERE,
            base::BindLambdaForTesting([&] { removed.Signal(); }));
      }));
  removed.Wait();
  track_added_ = false;
}

}  // namespace blink
