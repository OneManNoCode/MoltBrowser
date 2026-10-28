// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_camera_menu.h"

#include <span>

namespace blink::molt_fingerprinting {

namespace {

std::span<const moltnet::CameraMode> AsSpan(
    const Vector<moltnet::CameraMode>& modes) {
  return std::span<const moltnet::CameraMode>(modes.data(), modes.size());
}

}  // namespace

Vector<moltnet::CameraMode> UniformCameraMenu(
    const Vector<moltnet::CameraMode>& modes) {
  Vector<moltnet::CameraMode> menu;
  for (const moltnet::CameraMode& entry :
       moltnet::ReachableUniformCameraModes(AsSpan(modes))) {
    menu.push_back(entry);
  }
  return menu;
}

std::optional<UniformCameraOpening> UniformCameraOpeningFor(
    const Vector<moltnet::CameraMode>& modes) {
  const std::optional<size_t> mode =
      moltnet::UniformCameraOpenMode(AsSpan(modes));
  if (!mode || *mode >= modes.size()) {
    return std::nullopt;
  }
  const wtf_size_t index = static_cast<wtf_size_t>(*mode);
  return UniformCameraOpening{
      index, moltnet::UniformCameraOpenFrameRate(modes[index])};
}

}  // namespace blink::molt_fingerprinting
