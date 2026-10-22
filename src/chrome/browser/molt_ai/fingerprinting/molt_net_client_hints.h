// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's client hints on navigation requests. MoltNet windows send only
// the low-entropy user-agent hints (Sec-CH-UA, -Mobile, -Platform),
// Save-Data and the preference hints (molt_net_device_values.h); every
// device, network-quality and high-entropy user-agent hint is withheld,
// whatever the site asks for. Blink does the same for subresource requests
// (moltnet_device.h in core/molt_fingerprinting).
//
// Where. Content computes the hints of navigation requests (documents,
// iframes, prefetches, the Critical-CH and ACCEPT_CH-frame restarts, and
// the list it gives the network service) in
// content/browser/client_hints/client_hints.cc, and enables a hint from
// three sources: what the profile's ClientHintsControllerDelegate persisted
// or added, a prerendering page's Accept-CH, and an iframe's allow
// attribute / delegate-ch container policy, which enables hints for the
// iframe's navigation without asking the delegate. So filtering in the
// delegate alone isn't enough. The hook in client_hints.cc asks the
// delegate per hint instead (ClientHintsControllerDelegate::
// IsClientHintWithheld, added for this; false by default), and a withheld
// hint is never enabled, whichever source enabled it.
//
// The delegate. ClientHintsFactory creates every profile's delegate through
// CreateClientHints() (one-line hook in client_hints_factory.cc); MoltNet
// profiles get MoltNetClientHints, which also never persists a withheld
// hint and reports only the low-entropy user-agent metadata.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_CLIENT_HINTS_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_CLIENT_HINTS_H_

#include <memory>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "components/client_hints/browser/client_hints.h"
#include "services/network/public/mojom/web_client_hints_types.mojom-shared.h"

class HostContentSettingsMap;
class PrefService;

namespace content {
class BrowserContext;
class RenderFrameHost;
}  // namespace content

namespace content_settings {
class CookieSettings;
}  // namespace content_settings

namespace network {
class NetworkQualityTracker;
}  // namespace network

namespace url {
class Origin;
}  // namespace url

namespace molt_ai::fingerprinting {

// What ClientHintsFactory builds for `context` (same arguments as
// client_hints::ClientHints): a MoltNetClientHints for MoltNet profiles
// while fingerprinting protection is on (RendererMode::kUniform), the
// stock client_hints::ClientHints otherwise.
std::unique_ptr<client_hints::ClientHints> CreateClientHints(
    content::BrowserContext* context,
    network::NetworkQualityTracker* network_quality_tracker,
    HostContentSettingsMap* settings_map,
    scoped_refptr<content_settings::CookieSettings> cookie_settings,
    PrefService* local_state);

// Whether MoltNet withholds `type` (MoltNetSendsClientHint() is false).
bool IsWithheldInMoltNet(network::mojom::WebClientHintsType type);

class MoltNetClientHints final : public client_hints::ClientHints {
 public:
  MoltNetClientHints(
      content::BrowserContext* context,
      network::NetworkQualityTracker* network_quality_tracker,
      HostContentSettingsMap* settings_map,
      scoped_refptr<content_settings::CookieSettings> cookie_settings,
      PrefService* local_state);
  MoltNetClientHints(const MoltNetClientHints&) = delete;
  MoltNetClientHints& operator=(const MoltNetClientHints&) = delete;
  ~MoltNetClientHints() override;

  // content::ClientHintsControllerDelegate:
  bool IsClientHintWithheld(network::mojom::WebClientHintsType type) override;
  // The stored and additional hints, minus the withheld ones.
  void GetAllowedClientHintsFromSource(
      const url::Origin& origin,
      blink::EnabledClientHints* client_hints) override;
  // A site's Accept-CH for a withheld hint isn't even remembered (for the
  // MoltNet session).
  void PersistClientHints(const url::Origin& primary_origin,
                          content::RenderFrameHost* parent_rfh,
                          const std::vector<network::mojom::WebClientHintsType>&
                              client_hints) override;
  // Only the low-entropy values (brands, mobile, platform), as Blink's
  // navigator.userAgentData reports in MoltNet.
  blink::UserAgentMetadata GetUserAgentMetadata() override;
};

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_CLIENT_HINTS_H_
