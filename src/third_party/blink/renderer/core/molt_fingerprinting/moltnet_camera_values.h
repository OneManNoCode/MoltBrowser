// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the formats a MoltNet page sees of
// a camera, whatever modes the device has. Pure and std-only (tested
// standalone, like farbling.h); the Blink side is
// modules/mediastream/moltnet_camera.h, through moltnet_camera_menu.h.
//
// Every MoltNet camera offers formats of one menu: 640x480 and 1280x720, at
// 30 frames a second. Chromium's constraints algorithm chooses among those
// as if they were the camera's own modes, so getCapabilities(), getSettings()
// (width, height, frameRate, aspectRatio, resizeMode) and enumerateDevices()'
// InputDeviceInfo.getCapabilities() describe the menu, and the track
// adapter crops and scales the camera's frames to the size chosen
// ("crop-and-scale", VideoTrackAdapter), also when the page asks for an
// entry's own size (resizeMode "none"). The device itself opens in one of
// its own modes, the smallest that reaches the largest entry where it has one
// (UniformCameraOpenMode()), so that no later request reopens the device.
//
// Every camera shows every entry (ShownUniformCameraModes()), whatever its
// modes: a camera that doesn't reach an entry (a 640x480 camera's 1280x720,
// a 320x240 camera's both) has its frames scaled up to it. So a page sees the
// same entries of the menu on every machine, never a device's own modes, and
// nothing that tells a camera that reaches the larger entry from one that
// doesn't.
//
// A camera's frames reach the page on a fixed grid (UniformCameraGrid): 30 a
// second, stamped with the grid's times, whatever the camera's own rate and
// its jitter.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_CAMERA_VALUES_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_CAMERA_VALUES_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace blink::molt_fingerprinting::moltnet {

// A camera mode or menu entry: a frame size and a frame rate.
struct CameraMode {
  int width = 0;
  int height = 0;
  float frame_rate = 0.0f;
};

// The menu, smallest entry first. Each entry is at least as wide and as tall
// as the one before.
inline constexpr CameraMode kUniformCameraMenu[] = {
    {640, 480, 30.0f},
    {1280, 720, 30.0f},
};

// The menu's frame rate: getSettings().frameRate of a track that doesn't ask
// for less, and getCapabilities().frameRate's maximum.
inline constexpr float kUniformCameraFrameRate = 30.0f;

// A mode runs at the menu's rate from this rate up (the 29.97 fps modes of
// some cameras).
inline constexpr float kUniformCameraFastFrameRate = 29.5f;

// The entries of kUniformCameraMenu a camera with `modes` shows: all of them,
// whatever its modes. Empty only when `modes` has no mode of a positive size
// (no camera to show).
std::span<const CameraMode> ShownUniformCameraModes(
    std::span<const CameraMode> modes);

// The mode, an index into `modes`, a camera opens in for a MoltNet page.
// Among its modes at the menu's rate (all of its modes if none is), the
// smallest that is at least as wide and as tall as the largest entry, at the
// rate closest to the menu's from above (the fastest if none is that fast);
// the largest of them if none is that large, from which the larger entries
// are scaled up. nullopt when `modes` has no mode of a positive size.
std::optional<size_t> UniformCameraOpenMode(std::span<const CameraMode> modes);

// The frame rate a camera opening in `mode` asks for: the menu's, or the
// mode's own if lower.
float UniformCameraOpenFrameRate(const CameraMode& mode);

// The grid a camera's frames reach a page on: kUniformCameraTicksPerSecond
// ticks a second from the first frame of the camera's source. Tick n is n/30
// of a second after tick 0, in whole microseconds (a frame timestamp's unit),
// rounded down: 0, 33333, 66666, 100000, and so on. At each tick the camera's
// tracks get the newest frame the camera sent, again if it sent none since,
// stamped with the tick's time; a track that asked for a lower rate keeps the
// ticks it would keep of a 30 fps camera. Header-only, so that Blink's
// modules use it without a link to this target.
inline constexpr int64_t kUniformCameraTicksPerSecond = 30;
static_assert(kUniformCameraTicksPerSecond == kUniformCameraFrameRate,
              "the grid runs at the menu's rate");

// Tick `tick`'s time after tick 0, in microseconds.
constexpr int64_t UniformCameraTickMicroseconds(int64_t tick) {
  return tick * 1'000'000 / kUniformCameraTicksPerSecond;
}

// The first tick at or after `elapsed` microseconds after tick 0.
constexpr int64_t UniformCameraTickAtOrAfter(int64_t elapsed) {
  return elapsed <= 0
             ? 0
             : (elapsed * kUniformCameraTicksPerSecond + 999'999) / 1'000'000;
}

// How long the grid repeats the newest frame of a camera that sends none:
// then it stops, and resumes at the first tick after the camera's next frame
// (whose timestamp then is a whole number of ticks later).
inline constexpr int64_t kUniformCameraHoldMicroseconds = 1'000'000;

// A camera source's grid, on a monotonic clock in microseconds. The caller
// runs each pending tick (OnTick()) when it is due (NextTickDue()).
class UniformCameraGrid {
 public:
  // A frame of the source arrived at `now`. Returns when the pending tick is
  // due if the grid starts (the first frame's arrival is tick 0, due at once)
  // or resumes (the first tick at or after `now`); nullopt while a tick is
  // pending.
  std::optional<int64_t> OnSourceFrame(int64_t now) {
    last_frame_ = now;
    if (!origin_) {
      origin_ = now;
      next_tick_ = 0;
    } else if (ticking_) {
      return std::nullopt;
    } else {
      next_tick_ = UniformCameraTickAtOrAfter(now - *origin_);
    }
    ticking_ = true;
    return NextTickDue();
  }

  // Runs the pending tick at `now`: returns its number and makes the next one
  // pending (the following tick, or the first still ahead of `now` when this
  // one ran late: the ticks it missed are skipped, as a busy machine drops
  // frames). nullopt when no tick is pending, or when the source sent no
  // frame for kUniformCameraHoldMicroseconds, which stops the grid until its
  // next frame.
  std::optional<int64_t> OnTick(int64_t now) {
    if (!ticking_ || !origin_) {
      return std::nullopt;
    }
    if (now - last_frame_ > kUniformCameraHoldMicroseconds) {
      ticking_ = false;
      return std::nullopt;
    }
    const int64_t tick = next_tick_;
    next_tick_ = std::max(tick + 1, UniformCameraTickAtOrAfter(now - *origin_));
    return tick;
  }

  // When the pending tick is due.
  int64_t NextTickDue() const {
    return origin_.value_or(0) + UniformCameraTickMicroseconds(next_tick_);
  }

  // Tick 0's time.
  std::optional<int64_t> origin() const { return origin_; }

  bool ticking() const { return ticking_; }

 private:
  std::optional<int64_t> origin_;
  int64_t next_tick_ = 0;
  int64_t last_frame_ = 0;
  bool ticking_ = false;
};

}  // namespace blink::molt_fingerprinting::moltnet

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_CAMERA_VALUES_H_
