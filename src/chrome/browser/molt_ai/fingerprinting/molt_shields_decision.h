// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Fingerprinting protection's Shields decision, browser side: does a
// document or worker get farbled, given the user's MoltShield setting for its
// top-level site ("Shields off", molt_shield_site_exceptions.h)?
//
// The browser decides per document, when it commits (ReadyToCommitNavigation,
// molt_shields_decision_observer.h), and per service or shared worker, when
// it starts one (ContentBrowserClient::AreMoltShieldsDownForWorker), and
// sends only the answer, only to the renderer that gets the document or the
// worker. No renderer is told the profile's list of Shields-off sites, and a
// renderer can't ask about a site: the browser answers for what it commits.
//
// Pure functions over a HostContentSettingsMap (no profile), so they can be
// unit-tested in molt_ai_unittests.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_SHIELDS_DECISION_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_SHIELDS_DECISION_H_

#include <string>

#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_switches.h"

class HostContentSettingsMap;

namespace blink {
class StorageKey;
}  // namespace blink

namespace url {
class Origin;
}  // namespace url

namespace molt_ai::fingerprinting {

// What a document is told (molt_fingerprinting.mojom, ShieldsDecision).
struct ShieldsDecision {
  // MoltShield is off for `top_level_site`: don't farble.
  bool shields_down = false;
  // The site the decision was made for, as net::SchemefulSite serializes it
  // ("https://example.com"); empty when there is none. The renderer only
  // applies the decision to a document whose own top-level site this is.
  std::string top_level_site;
};

// The decision for a document committing in a frame tree whose top frame
// has (for a main frame: will have) `top_frame_origin`, in a profile whose
// renderers run in `mode`, with content settings `map` (may be null).
//
// The site is `top_frame_origin`'s, or its precursor's when it is opaque (a
// CSP-sandboxed top-level page is still its site's page). Shields can only
// be down in kFarble mode (MoltNet farbles regardless), for an http(s) site
// whose own URL has an explicit Shields-down exception: the site-scoped one
// the MoltShield UI writes (shield::HasShieldsDownException).
ShieldsDecision DecideForTopFrameOrigin(RendererMode mode,
                                        const HostContentSettingsMap* map,
                                        const url::Origin& top_frame_origin);

// The decision for a service or shared worker with `storage_key`: Shields
// down for the storage key's top-level site, the site the worker farbles
// with. False for an opaque top-level site and outside kFarble mode.
bool AreShieldsDownForWorker(RendererMode mode,
                             const HostContentSettingsMap* map,
                             const blink::StorageKey& storage_key);

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_SHIELDS_DECISION_H_
