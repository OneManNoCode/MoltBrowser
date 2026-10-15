// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Browser half of MoltBrowser's fingerprinting protection: one session seed
// per BrowserContext and the renderer switches that carry it.
//
// The seed is 32 bytes from crypto::RandBytes, created the first time a
// renderer of that BrowserContext is launched and kept only in memory (user
// data on the BrowserContext), so it dies with the profile: every browser
// session, every Incognito profile and every MoltNet profile gets its own.
// MoltNet's New Identity destroys and recreates its off-the-record profile
// (MoltNetWindow::NewIdentity), so it also gets a fresh seed, and with it
// fresh canvas/WebGL/audio/hardwareConcurrency values on every site.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_BROWSER_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_BROWSER_H_

#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_switches.h"

namespace base {
class CommandLine;
}  // namespace base

namespace content {
class BrowserContext;
class RenderProcessHost;
}  // namespace content

namespace molt_ai::fingerprinting {

// Called from ChromeContentBrowserClient::AppendExtraCommandLineSwitches for
// renderer processes. Appends --molt-fingerprinting-mode and
// --molt-fingerprinting-seed for `host`'s BrowserContext, unless the
// MoltFingerprintingProtection feature is disabled. No-op for a null host.
void AppendRendererSwitches(content::RenderProcessHost* host,
                            base::CommandLine* command_line);

// kUniform for the MoltNet (Tor) profile, kFarble for every other profile
// (regular, Incognito, Guest), kOff when the feature is disabled.
RendererMode ModeForBrowserContext(content::BrowserContext* context);

// The in-memory session seed of `context`, created on first use.
const SessionSeed& GetSessionSeed(content::BrowserContext* context);

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_BROWSER_H_
