// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/renderer/molt_ai/fingerprinting/molt_fingerprinting_renderer.h"

#include <optional>

#include "base/command_line.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_switches.h"
#include "third_party/blink/public/web/web_molt_fingerprinting.h"

namespace molt_ai::fingerprinting {

void ConfigureRendererProcess() {
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  const RendererMode mode =
      ModeFromSwitchValue(command_line.GetSwitchValueASCII(switches::kMode));
  if (mode == RendererMode::kOff) {
    return;
  }
  const std::optional<SessionSeed> seed =
      DecodeSeed(command_line.GetSwitchValueASCII(switches::kSeed));
  if (!seed) {
    return;
  }
  blink::WebMoltFingerprinting::Configure(
      mode == RendererMode::kUniform
          ? blink::WebMoltFingerprinting::Mode::kUniform
          : blink::WebMoltFingerprinting::Mode::kFarble,
      *seed);
}

}  // namespace molt_ai::fingerprinting
