// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_switches.h"

namespace molt_ai::fingerprinting {

namespace {
constexpr char kFarble[] = "farble";
constexpr char kUniform[] = "uniform";
}  // namespace

std::string_view ModeToSwitchValue(RendererMode mode) {
  switch (mode) {
    case RendererMode::kOff:
      return std::string_view();
    case RendererMode::kFarble:
      return kFarble;
    case RendererMode::kUniform:
      return kUniform;
  }
  return std::string_view();
}

RendererMode ModeFromSwitchValue(std::string_view value) {
  if (value == kFarble) {
    return RendererMode::kFarble;
  }
  if (value == kUniform) {
    return RendererMode::kUniform;
  }
  return RendererMode::kOff;
}

}  // namespace molt_ai::fingerprinting
