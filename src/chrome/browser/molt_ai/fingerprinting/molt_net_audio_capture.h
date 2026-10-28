// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's microphone streams, browser side: the browser decides from the
// profile whether a renderer's streams are processed with the code that
// gives the same samples on every CPU (media::AudioProcessingSettings::
// cpu_independent; media/base/molt_portable_media.h), and sets it in the
// processing config of each stream it creates for the renderer, whatever the
// renderer sent.
//
// Hooks (each marked "MoltBrowser:"):
//
//   content  RenderFrameAudioInputStreamFactory::Core (a frame's microphone
//              streams; asks on the UI thread when it is created, with the
//              frame's process's BrowserContext, and applies the answer to
//              each stream's config:
//              content/browser/renderer_host/media/
//              molt_audio_capture_processing.h), through
//              ContentBrowserClient::ShouldProcessAudioCaptureCpuIndependently
//                               -> ProcessesAudioCaptureCpuIndependently
//
// ChromeContentBrowserClient passes the profile's renderer mode
// (ModeForBrowserContext(): kUniform for MoltNet, kOff while the kill switch
// is off), which also decides whether the profile's renderers are portable
// media processes, so the browser and the renderer agree.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_AUDIO_CAPTURE_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_AUDIO_CAPTURE_H_

#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_switches.h"

namespace molt_ai::fingerprinting {

// Whether the microphone streams of a profile whose renderers run in `mode`
// are processed CPU-independently: MoltNet's (kUniform).
constexpr bool ProcessesAudioCaptureCpuIndependently(RendererMode mode) {
  return mode == RendererMode::kUniform;
}

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_AUDIO_CAPTURE_H_
