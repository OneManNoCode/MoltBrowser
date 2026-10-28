// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the browser decides whether a renderer's microphone stream is
// processed with the code that gives the same samples on every CPU
// (media::AudioProcessingSettings::cpu_independent, media/base/
// molt_portable_media.h), from the renderer process's profile
// (ContentBrowserClient::ShouldProcessAudioCaptureCpuIndependently():
// MoltNet's). The renderer sets the flag in the processing config it sends
// as well (a portable media process's streams), but the audio service, which
// every profile shares, picks the code it runs from the flag, so the
// browser's decision replaces whatever the renderer sent.
//
// At Chromium 156 one path creates a stream with a processing config the
// audio service applies: a frame's microphone, through
// RenderFrameAudioInputStreamFactory (ForwardingAudioStreamFactory,
// AudioInputStreamBroker, audio::StreamFactory::CreateInputStream). Its Core
// asks the embedder once, on the UI thread when it is created, with the
// frame's process's BrowserContext, and applies the answer to the config of
// every stream it creates (MoltApplyCpuIndependentProcessing()). The other
// input streams carry no config, so the service doesn't process them:
// loopback (tab) capture, audio::CreateInputDevice()'s (speech recognition,
// the browser's own microphone preview) and cast mirroring's. A renderer
// that processes a stream itself sends no config either, and computes the
// processing as its own process's flag says.

#ifndef CONTENT_BROWSER_RENDERER_HOST_MEDIA_MOLT_AUDIO_CAPTURE_PROCESSING_H_
#define CONTENT_BROWSER_RENDERER_HOST_MEDIA_MOLT_AUDIO_CAPTURE_PROCESSING_H_

#include "media/mojo/mojom/audio_processing.mojom.h"

namespace content {

// Sets the cpu_independent of `config`, a renderer's processing config for a
// stream (null: the audio service doesn't process the stream), to the
// browser's decision for the renderer's profile.
inline void MoltApplyCpuIndependentProcessing(
    bool cpu_independent,
    media::mojom::AudioProcessingConfig* config) {
  if (config) {
    config->settings.cpu_independent = cpu_independent;
  }
}

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_MEDIA_MOLT_AUDIO_CAPTURE_PROCESSING_H_
