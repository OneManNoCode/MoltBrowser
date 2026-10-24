// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Autofill and the payment sheet in MoltNet tabs: nothing goes to Google's
// Autofill server, and no address metadata is downloaded
// (molt_net_requests_policy.h: `autofill_server`, `address_metadata`).
//
//  * Autofill's crowdsourcing manager is the one component that talks to
//    the Autofill server: field-type queries for the forms a page shows,
//    and the votes Autofill and the password manager upload after a
//    submission. A MoltNet tab gets one that sends nothing: every query
//    answers "no server predictions" at once, as when the server can't be
//    reached, and every upload is dropped. Local heuristics (and an
//    on-device model) still classify fields; filling saved addresses and
//    passwords works as before.
//  * Address metadata (libaddressinput's rules, from
//    chromium-i18n.appspot.com) is downloaded through the browser-wide
//    network context, outside Tor, and cached in a file every profile
//    shares. A MoltNet tab or payment sheet gets an address normalizer and
//    a region loader with no source and no cache: they load nothing.
//
// Each function returns null for every other profile, and the caller then
// does what Chromium does. The upstream hooks, marked "MoltBrowser:", are in
// ChromeAutofillClient::GetCrowdsourcingManager() and
// ::GetAddressNormalizer() (chrome/browser/ui/autofill/) and
// ChromePaymentRequestDelegate::GetAddressNormalizer() and
// ::GetRegionDataLoader() (chrome/browser/payments/).

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_AUTOFILL_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_AUTOFILL_H_

#include <memory>
#include <string>

#include "base/version_info/channel.h"

namespace autofill {
class AddressNormalizer;
class AutofillClient;
class AutofillCrowdsourcingManager;
class RegionDataLoader;
}  // namespace autofill

namespace content {
class BrowserContext;
}  // namespace content

namespace molt_ai::tor {

// For |client|'s tab in |context|: a crowdsourcing manager that sends
// nothing if |context| is a MoltNet profile, else null.
std::unique_ptr<autofill::AutofillCrowdsourcingManager>
MoltNetCrowdsourcingManagerFor(content::BrowserContext* context,
                               autofill::AutofillClient* client,
                               version_info::Channel channel);

// For a tab or payment sheet in |context|: an address normalizer that
// loads no rules (so normalizes nothing: filling uses what matches without
// them, the payment sheet keeps the address as entered) if |context| is a
// MoltNet profile or unknown (null: the frame is gone), else null. It lives
// as long as the browser.
autofill::AddressNormalizer* MoltNetAddressNormalizerFor(
    content::BrowserContext* context);

// For the payment sheet's address editor in |context|: a new region data
// loader that loads nothing (the editor then offers a text field for the
// region) if |context| is a MoltNet profile or unknown, else null. Like
// Chromium's, it deletes itself once it has answered.
autofill::RegionDataLoader* MoltNetRegionDataLoaderFor(
    content::BrowserContext* context,
    const std::string& app_locale);

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_AUTOFILL_H_
