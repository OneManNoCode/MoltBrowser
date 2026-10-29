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

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_CAMERA_VALUES_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_CAMERA_VALUES_H_

#include <cstddef>
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

}  // namespace blink::molt_fingerprinting::moltnet

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_CAMERA_VALUES_H_
