// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Renderer half of MoltBrowser's fingerprinting protection: reads the
// --molt-fingerprinting-mode / --molt-fingerprinting-seed switches the browser
// put on this renderer's command line and configures Blink.

#ifndef CHROME_RENDERER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_RENDERER_H_
#define CHROME_RENDERER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_RENDERER_H_

namespace molt_ai::fingerprinting {

// Call once from ChromeContentRendererClient::RenderThreadStarted (main
// thread, after Blink is initialized, before any frame or worker exists).
// Missing or malformed switches leave protection off.
void ConfigureRendererProcess();

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_RENDERER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_RENDERER_H_
