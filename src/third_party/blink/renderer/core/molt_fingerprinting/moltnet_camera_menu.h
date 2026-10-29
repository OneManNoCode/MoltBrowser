// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the camera menu's rules
// (moltnet_camera_values.h, in the :farbling target) for the Blink side of
// MoltNet's cameras, modules/mediastream/moltnet_camera.h, which reaches them
// through the core component.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_CAMERA_MENU_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_CAMERA_MENU_H_

#include <optional>

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_camera_values.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink::molt_fingerprinting {

// The menu entries a camera with `modes` shows, smallest first
// (moltnet::ShownUniformCameraModes(): all of them). Empty only when `modes`
// has no mode of a positive size.
CORE_EXPORT Vector<moltnet::CameraMode> UniformCameraMenu(
    const Vector<moltnet::CameraMode>& modes);

// The mode a camera opens in for a MoltNet page, as an index into its modes,
// and the frame rate it asks for.
struct UniformCameraOpening {
  wtf_size_t mode = 0;
  float frame_rate = 0.0f;
};

// The opening of a camera with `modes` (moltnet::UniformCameraOpenMode() and
// UniformCameraOpenFrameRate()). nullopt when `modes` has no mode of a
// positive size.
CORE_EXPORT std::optional<UniformCameraOpening> UniformCameraOpeningFor(
    const Vector<moltnet::CameraMode>& modes);

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_CAMERA_MENU_H_
