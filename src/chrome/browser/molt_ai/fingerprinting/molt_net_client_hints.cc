// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_net_client_hints.h"

#include <cstdint>
#include <utility>

#include "chrome/browser/molt_ai/fingerprinting/molt_fingerprinting_browser.h"
#include "chrome/browser/molt_ai/fingerprinting/molt_net_device_values.h"
#include "components/embedder_support/user_agent_utils.h"
#include "services/network/public/cpp/client_hints.h"
#include "third_party/blink/public/common/client_hints/enabled_client_hints.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "url/origin.h"

namespace molt_ai::fingerprinting {

namespace {

using network::mojom::WebClientHintsType;

// MoltNetSendsClientHint() names the generated enum's values.
static_assert(static_cast<int32_t>(WebClientHintsType::kUA) == 8);
static_assert(static_cast<int32_t>(WebClientHintsType::kUAPlatform) == 10);
static_assert(static_cast<int32_t>(WebClientHintsType::kUAMobile) == 12);
static_assert(static_cast<int32_t>(WebClientHintsType::kPrefersColorScheme) ==
              15);
static_assert(static_cast<int32_t>(WebClientHintsType::kSaveData) == 27);
static_assert(static_cast<int32_t>(WebClientHintsType::kPrefersReducedMotion) ==
              28);
static_assert(static_cast<int32_t>(
                  WebClientHintsType::kPrefersReducedTransparency) == 30);

}  // namespace

bool IsWithheldInMoltNet(WebClientHintsType type) {
  return !MoltNetSendsClientHint(static_cast<int32_t>(type));
}

std::unique_ptr<client_hints::ClientHints> CreateClientHints(
    content::BrowserContext* context,
    network::NetworkQualityTracker* network_quality_tracker,
    HostContentSettingsMap* settings_map,
    scoped_refptr<content_settings::CookieSettings> cookie_settings,
    PrefService* local_state) {
  if (ModeForBrowserContext(context) == RendererMode::kUniform) {
    return std::make_unique<MoltNetClientHints>(
        context, network_quality_tracker, settings_map,
        std::move(cookie_settings), local_state);
  }
  return std::make_unique<client_hints::ClientHints>(
      context, network_quality_tracker, settings_map,
      std::move(cookie_settings), local_state);
}

MoltNetClientHints::MoltNetClientHints(
    content::BrowserContext* context,
    network::NetworkQualityTracker* network_quality_tracker,
    HostContentSettingsMap* settings_map,
    scoped_refptr<content_settings::CookieSettings> cookie_settings,
    PrefService* local_state)
    : client_hints::ClientHints(context,
                                network_quality_tracker,
                                settings_map,
                                std::move(cookie_settings),
                                local_state) {}

MoltNetClientHints::~MoltNetClientHints() = default;

bool MoltNetClientHints::IsClientHintWithheld(WebClientHintsType type) {
  return IsWithheldInMoltNet(type);
}

void MoltNetClientHints::GetAllowedClientHintsFromSource(
    const url::Origin& origin,
    blink::EnabledClientHints* client_hints) {
  client_hints::ClientHints::GetAllowedClientHintsFromSource(origin,
                                                             client_hints);
  for (const auto& [type, name] : network::GetClientHintToNameMap()) {
    if (IsWithheldInMoltNet(type)) {
      client_hints->SetIsEnabled(type, false);
    }
  }
}

void MoltNetClientHints::PersistClientHints(
    const url::Origin& primary_origin,
    content::RenderFrameHost* parent_rfh,
    const std::vector<WebClientHintsType>& client_hints) {
  std::vector<WebClientHintsType> sent;
  for (WebClientHintsType type : client_hints) {
    if (!IsWithheldInMoltNet(type)) {
      sent.push_back(type);
    }
  }
  client_hints::ClientHints::PersistClientHints(primary_origin, parent_rfh,
                                                sent);
}

blink::UserAgentMetadata MoltNetClientHints::GetUserAgentMetadata() {
  return embedder_support::GetUserAgentMetadata(/*only_low_entropy_ch=*/true);
}

}  // namespace molt_ai::fingerprinting
