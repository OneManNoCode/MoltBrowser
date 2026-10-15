// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_switches.h"

#include "base/strings/string_number_conversions.h"

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

std::string EncodeSeed(base::span<const uint8_t, kSessionSeedSize> seed) {
  return base::HexEncodeLower(seed);
}

std::optional<SessionSeed> DecodeSeed(std::string_view hex) {
  if (hex.size() != 2 * kSessionSeedSize) {
    return std::nullopt;
  }
  SessionSeed seed;
  if (!base::HexStringToSpan(hex, seed)) {
    return std::nullopt;
  }
  return seed;
}

}  // namespace molt_ai::fingerprinting
