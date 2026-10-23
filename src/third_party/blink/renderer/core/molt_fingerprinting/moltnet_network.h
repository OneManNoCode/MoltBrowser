// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the connection and network quality
// a MoltNet renderer (kUniform) reports. The fixed values are in
// moltnet_network_values.h.
//
// Everything in Blink that depends on the browser's network-quality
// estimate, the operating system's connection type or data saver reads it
// from the process's NetworkStateNotifier (GetNetworkStateNotifier()),
// which content fills from the browser (RenderThreadImpl's
// OnNetworkQualityChanged and OnNetworkConnectionChanged, WebViewImpl's
// SetSaveDataEnabled and SetNetworkQualityWebHoldback). At 152 that is:
//
//   navigator.connection (window and workers): effectiveType, rtt,
//     downlink, saveData, type, downlinkMax, the change and typechange
//     events                                        (modules/netinfo)
//   the ECT, RTT, Downlink and Save-Data request headers
//     (frame_fetch_context.cc, worker_fetch_context.cc; the first three
//     are withheld in MoltNet anyway, moltnet_device.h)
//   prefers-reduced-data                            (media_values.cc)
//   lazy-loading distances of images and iframes
//     (lazy_load_media_observer.cc, lazy_load_frame_observer.cc)
//   the web-font intervention on slow networks
//                                             (remote_font_face_source.cc)
//   low-priority iframes          (frame_resource_fetcher_properties.cc)
//   blocking document.written scripts on 2g
//                                          (document_write_intervention.cc)
//   preload "metadata" on cellular connections  (html_media_element.cc)
//   preloading eligibility under data saver
//                                   (anchor_element_interaction_tracker.cc)
//
// So in kUniform mode Configure() pins that notifier to
// UniformNetworkState() for the life of the process (one hook,
// NetworkStateNotifier::SetUniformNetworkState): every reader above sees the
// fixed values, updates from the browser change nothing, so no change event
// fires, and navigator.connection adds no per-site noise. DevTools' network
// emulation still applies on top while it is on. navigator.onLine and the
// online and offline events still follow the operating system: they don't
// come from the estimate, a site can't change them, and when the network
// goes down every site sees its requests fail at that moment anyway.
//
// The browser side: MoltNet's network context has no network-quality
// estimator (chrome/browser/molt_ai/tor/molt_net_network_context_policy.h),
// so its request scheduling doesn't depend on the estimate either, and
// MoltNet's own requests don't feed the estimate other windows see.
//
// Normal and Incognito windows (kFarble) keep Chromium's behaviour.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_NETWORK_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_NETWORK_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/network/network_state_notifier.h"

namespace blink::molt_fingerprinting {

// What MoltNet renderers report (moltnet_network_values.h), as the state
// NetworkStateNotifier::SetUniformNetworkState() takes: connection type
// "unknown" with an unknown (infinite) maximum bandwidth, effective type
// 4g, HTTP RTT 200 ms, transport RTT 125 ms, downlink 1.6 Mbps, no data
// saver and no web holdback.
CORE_EXPORT NetworkStateNotifier::NetworkState UniformNetworkState();

// Called once by Configure(kUniform), on the main thread before any
// document exists: pins GetNetworkStateNotifier() to UniformNetworkState()
// for the life of the process.
void PinUniformNetworkState();

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_NETWORK_H_
