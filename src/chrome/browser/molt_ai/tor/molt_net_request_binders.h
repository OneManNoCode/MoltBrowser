// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The frame binders that keep two browser services away from MoltNet frames
// (molt_net_requests_policy.h). Both are reached from a page's frame and
// send what they get outside Tor:
//
//   * image_annotation::mojom::Annotator ("Get image descriptions from
//     Google", for screen readers). Chrome binds it to the
//     AccessibilityLabelsService of the frame's profile, which for an
//     off-the-record profile is the original profile's
//     (chrome_browser_interface_binders.cc BindImageAnnotator), and that
//     service uploads the URLs and pixels of the page's images through the
//     regular profile's network context.
//   * blink::mojom::GeolocationService (navigator.geolocation). After the
//     permission check, content asks the device service's location
//     provider, which uses the system's location service: on Linux,
//     Google's, through the system network context, with the Wi-Fi
//     networks around the machine.
//
// A MoltNet frame gets stand-ins that answer at once and reach nothing:
// every image description fails (the renderer reports "no description")
// and every location request is denied without a prompt (the page gets
// PERMISSION_DENIED). Registered from RegisterMoltNetFrameBinders()
// (molt_net_webrtc.h), after content's and Chrome's own binders, which
// content lets the embedder override. Needs no Profile or content
// implementation, so molt_ai_unittests drives it over real mojo pipes.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_REQUEST_BINDERS_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_REQUEST_BINDERS_H_

#include "chrome/browser/molt_ai/tor/molt_net_requests_policy.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "services/image_annotation/public/mojom/image_annotation.mojom-forward.h"
#include "third_party/blink/public/mojom/geolocation/geolocation_service.mojom-forward.h"

namespace content {
class RenderFrameHost;
}  // namespace content

namespace molt_ai::tor {

// Replaces the binder of each interface `access` refuses (and adds one where
// none was registered, so a renderer that asks anyway isn't killed). Leaves
// `map` alone for every other profile's access.
//   * !image_descriptions: image_annotation::mojom::Annotator is bound to
//     BindRefusingImageAnnotator().
//   * !geolocation: blink::mojom::GeolocationService is bound to
//     BindRefusingGeolocationService().
void ApplyMoltNetRequestAccessToFrameBinders(
    const MoltNetRequestAccess& access,
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map);

// An Annotator that answers every AnnotateImage() with
// AnnotateImageError::kFailure and never asks for the image's pixels.
void BindRefusingImageAnnotator(
    mojo::PendingReceiver<image_annotation::mojom::Annotator> receiver);

// A GeolocationService that answers every CreateGeolocation() with
// PermissionStatus::DENIED and binds no Geolocation (the pending receiver is
// dropped), so no location provider is ever started.
void BindRefusingGeolocationService(
    mojo::PendingReceiver<blink::mojom::GeolocationService> receiver);

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_REQUEST_BINDERS_H_
