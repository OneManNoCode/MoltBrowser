// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// WebSocket coverage for MoltShield. WebSockets don't go through
// URLLoaderFactory, so tracker sockets (e.g. session-replay and ad
// beacons over wss://) were never checked. ChromeContentBrowserClient asks
// to intercept WebSocket creation when MoltShield applies, and wraps
// content's WebSocketFactory so the handshake only starts after the URL
// passes the blocker (checked on MoltShield's engine sequence). A blocked
// socket's handshake client is dropped, which the page sees as a failed
// connection (error + close 1006), the same as other blockers.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_WEBSOCKET_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_WEBSOCKET_H_

#include <optional>
#include <string>

#include "content/public/browser/content_browser_client.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/websocket.mojom-forward.h"
#include "url/gurl.h"

namespace content {
class RenderFrameHost;
}  // namespace content

namespace molt_ai::shield {

// UI thread. True if MoltShield wants to vet WebSockets from `frame`.
bool ShouldInterceptWebSocket(content::RenderFrameHost* frame);

// UI thread. Returns `factory` wrapped with a MoltShield check if
// ShouldInterceptWebSocket(frame), else `factory` unchanged.
content::ContentBrowserClient::WebSocketFactory MaybeWrapWebSocketFactory(
    content::RenderFrameHost* frame,
    content::ContentBrowserClient::WebSocketFactory factory);

// Connects without any other interceptor, passing the same headers content
// would (User-Agent override only).
void ConnectWebSocket(
    content::ContentBrowserClient::WebSocketFactory factory,
    const GURL& url,
    const std::optional<std::string>& user_agent,
    mojo::PendingRemote<network::mojom::WebSocketHandshakeClient>
        handshake_client);

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_WEBSOCKET_H_
