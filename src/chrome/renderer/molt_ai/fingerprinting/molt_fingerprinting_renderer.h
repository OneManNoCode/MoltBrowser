// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Renderer half of MoltBrowser's fingerprinting protection: reads the
// --molt-fingerprinting-mode switch the browser put on this renderer's
// command line, configures Blink, and receives the session seed and the
// Shields-off sites over the channel-associated FingerprintingConfiguration
// interface (chrome/common/molt_ai/fingerprinting/molt_fingerprinting.mojom).

#ifndef CHROME_RENDERER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_RENDERER_H_
#define CHROME_RENDERER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_RENDERER_H_

namespace molt_ai::fingerprinting {

// Call once from ChromeContentRendererClient::RenderThreadStarted (main
// thread, after Blink is initialized, before any frame or worker exists).
// A missing or malformed mode switch leaves protection off and doesn't
// expose the configuration interface. In kUniform (MoltNet) it also pins the
// renderer strings pages can read to en-US (moltnet_locale_strings.h).
void ConfigureRendererProcess();

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_RENDERER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_RENDERER_H_
