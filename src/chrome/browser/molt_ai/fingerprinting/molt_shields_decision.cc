// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_shields_decision.h"

#include "chrome/browser/molt_ai/shield/molt_shield_site_exceptions.h"
#include "net/base/schemeful_site.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/scheme_host_port.h"

namespace molt_ai::fingerprinting {

namespace {

bool AreShieldsDownForSite(RendererMode mode,
                           const HostContentSettingsMap* map,
                           const net::SchemefulSite& site) {
  // MoltNet keeps farbling where Shields are off: there the point is to look
  // like every other MoltNet user. With protection off there is nothing to
  // decide.
  if (mode != RendererMode::kFarble || !map || site.opaque()) {
    return false;
  }
  // The site's own URL, as for a page on the site: the site-scoped exception
  // the MoltShield UI writes ("https://[*.]example.com") counts; a narrower
  // one (a single subdomain) doesn't, because the farbling key is the whole
  // site's. Non-http(s) sites never have Shields down.
  return shield::HasShieldsDownException(*map, site.GetURL());
}

}  // namespace

ShieldsDecision DecideForTopFrameOrigin(RendererMode mode,
                                        const HostContentSettingsMap* map,
                                        const url::Origin& top_frame_origin) {
  ShieldsDecision decision;
  // A CSP-sandboxed top-level document is still its site's page; Blink takes
  // the same precursor (SecurityOrigin::GetOriginOrPrecursorOriginIfOpaque).
  const url::SchemeHostPort& tuple =
      top_frame_origin.GetTupleOrPrecursorTupleIfOpaque();
  if (!tuple.IsValid()) {
    return decision;
  }
  const net::SchemefulSite site(url::Origin::Create(tuple.GetURL()));
  if (site.opaque()) {
    return decision;
  }
  decision.top_level_site = site.Serialize();
  decision.shields_down = AreShieldsDownForSite(mode, map, site);
  return decision;
}

bool AreShieldsDownForWorker(RendererMode mode,
                             const HostContentSettingsMap* map,
                             const blink::StorageKey& storage_key) {
  return AreShieldsDownForSite(mode, map, storage_key.top_level_site());
}

}  // namespace molt_ai::fingerprinting
