// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Browser half of MoltBrowser's fingerprinting protection: one session seed
// per BrowserContext, the renderer mode switch, and the channel that carries
// the seed to renderers. The Shields decisions go per document and per
// worker instead (molt_shields_delivery.h).
//
// The seed is 32 bytes from crypto::RandBytes, created the first time a
// renderer of that BrowserContext is launched and kept only in memory (user
// data on the BrowserContext), so it dies with the profile: every browser
// session, every Incognito profile and every MoltNet profile gets its own.
// MoltNet's New Identity destroys and recreates its off-the-record profile
// (MoltNetWindow::NewIdentity), so it also gets a fresh seed, and with it
// fresh canvas/WebGL/audio/hardwareConcurrency values on every site.
//
// The seed never goes on a command line (other local processes can read
// those): InitializeRenderer() sends it over the renderer's channel-
// associated FingerprintingConfiguration interface, ordered before any
// navigation commit for that process (service and shared workers start over
// other pipes and need it only at their first farbled read).

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_BROWSER_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_BROWSER_H_

#include <string>
#include <vector>

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
// renderer processes. Appends --molt-fingerprinting-mode for `host`'s
// BrowserContext, unless the MoltFingerprintingProtection feature is
// disabled, and for MoltNet (kUniform) --disable-skia-runtime-opts. No-op
// for a null host.
void AppendRendererSwitches(content::RenderProcessHost* host,
                            base::CommandLine* command_line);

// Called from ChromeContentBrowserClient::RenderProcessWillLaunch (every
// launch, relaunches included). Unless the mode is kOff, sends the session
// seed.
void InitializeRenderer(content::RenderProcessHost* host);

// kUniform for the MoltNet (Tor) profile, kFarble for every other profile
// (regular, Incognito, Guest), kOff when the feature is disabled.
RendererMode ModeForBrowserContext(content::BrowserContext* context);

// The in-memory session seed of `context`, created on first use.
const SessionSeed& GetSessionSeed(content::BrowserContext* context);

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_BROWSER_H_
