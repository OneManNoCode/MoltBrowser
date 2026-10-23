// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_websocket.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "chrome/browser/molt_ai/shield/molt_shield_request_utils.h"
#include "chrome/browser/molt_ai/shield/molt_shield_service.h"
#include "chrome/browser/molt_ai/shield/molt_shield_site_settings.h"
#include "chrome/common/molt_ai/shield/molt_shield_features.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "net/http/http_request_headers.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/websocket.mojom.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace molt_ai::shield {

namespace {

using WebSocketFactory = content::ContentBrowserClient::WebSocketFactory;

void OnWebSocketChecked(
    content::GlobalRenderFrameHostId frame_id,
    WebSocketFactory factory,
    GURL url,
    std::vector<network::mojom::HttpHeaderPtr> headers,
    mojo::PendingRemote<network::mojom::WebSocketHandshakeClient>
        handshake_client,
    mojo::PendingRemote<network::mojom::WebSocketAuthenticationHandler>
        auth_handler,
    mojo::PendingRemote<network::mojom::TrustedHeaderClient> header_client,
    CheckResult result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  MoltShieldService::Get()->RecordCheck(result, kTypeWebSocket);
  if (result.should_block) {
    RecordBlockedForFrame(content::RenderFrameHost::FromID(frame_id));
    // Dropping the endpoints fails the renderer's pending handshake.
    return;
  }
  std::move(factory).Run(url, std::move(headers), std::move(handshake_client),
                         std::move(auth_handler), std::move(header_client));
}

void CheckThenConnect(
    content::GlobalRenderFrameHostId frame_id,
    url::Origin initiator,
    WebSocketFactory factory,
    const GURL& url,
    std::vector<network::mojom::HttpHeaderPtr> headers,
    mojo::PendingRemote<network::mojom::WebSocketHandshakeClient>
        handshake_client,
    mojo::PendingRemote<network::mojom::WebSocketAuthenticationHandler>
        auth_handler,
    mojo::PendingRemote<network::mojom::TrustedHeaderClient> header_client) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  std::optional<CheckInput> input =
      MakeCheckInput(url, initiator, kTypeWebSocket, "GET");
  if (!input) {
    std::move(factory).Run(url, std::move(headers),
                           std::move(handshake_client),
                           std::move(auth_handler), std::move(header_client));
    return;
  }
  MoltShieldService::Get()->Check(
      std::move(*input),
      base::BindOnce(&OnWebSocketChecked, frame_id, std::move(factory), url,
                     std::move(headers), std::move(handshake_client),
                     std::move(auth_handler), std::move(header_client)));
}

}  // namespace

bool ShouldInterceptWebSocket(content::RenderFrameHost* frame) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  // Kill switch (chrome://flags/#molt-shield-websocket or
  // --disable-molt-shield): never intercept, so content (or the extensions
  // proxy) creates the socket exactly as without MoltShield.
  if (!molt_shield::IsWebSocketEnabled() || !frame) {
    return false;
  }
  MoltShieldService* service = MoltShieldService::Get();
  if (!service->IsEnabled()) {
    return false;
  }
  const url::Origin& origin = frame->GetLastCommittedOrigin();
  if (!origin.opaque() && origin.scheme() != url::kHttpScheme &&
      origin.scheme() != url::kHttpsScheme) {
    return false;  // WebUI, extensions, ...
  }
  return AreShieldsUpForSite(
      frame->GetBrowserContext(),
      frame->GetOutermostMainFrame()->GetLastCommittedURL());
}

WebSocketFactory MaybeWrapWebSocketFactory(content::RenderFrameHost* frame,
                                           WebSocketFactory factory) {
  if (!ShouldInterceptWebSocket(frame)) {
    return factory;
  }
  return base::BindOnce(&CheckThenConnect, frame->GetGlobalId(),
                        frame->GetLastCommittedOrigin(), std::move(factory));
}

void ConnectWebSocket(
    WebSocketFactory factory,
    const GURL& url,
    const std::optional<std::string>& user_agent,
    mojo::PendingRemote<network::mojom::WebSocketHandshakeClient>
        handshake_client,
    mojo::PendingRemote<network::mojom::TrustedHeaderClient> header_client) {
  // Mirrors content::WebSocketConnectorImpl::Connect()'s direct path.
  std::vector<network::mojom::HttpHeaderPtr> headers;
  if (user_agent) {
    headers.push_back(network::mojom::HttpHeader::New(
        net::HttpRequestHeaders::kUserAgent, *user_agent));
  }
  std::move(factory).Run(url, std::move(headers), std::move(handshake_client),
                         mojo::NullRemote(), std::move(header_client));
}

}  // namespace molt_ai::shield
