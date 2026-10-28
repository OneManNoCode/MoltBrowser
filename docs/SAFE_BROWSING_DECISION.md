# S17: Safe Browsing decision memo

**Status:** decision needed. **Audience:** GenEye lead. **Related:**
[NETWORK_TRAFFIC.md](NETWORK_TRAFFIC.md) (P16).

## Where we are

All of this was read in Chromium source at 156.0.8078.48 (the 156.0.8078.74
respin changed none of it); file references are in NETWORK_TRAFFIC.md and in
[What changed at Chromium 156](#what-changed-at-chromium-156) below.

- The macOS arm64, Linux and Windows configs build the full desktop Safe
  Browsing service (`safe_browsing_mode = 1`). It is on by default as
  "Standard protection".
- No API key is configured, so the key is the placeholder `dummytoken`.
  - List updates still go to `safebrowsing.googleapis.com` with that key, and
    we expect Google to reject them. At 156 they use the V5 protocol (the
    field-trial testing config's `SafeBrowsingLocalListsUseSBv5` study).
    The local blocklist therefore stays empty, and every URL check returns
    "safe".
  - For downloaded programs and archives, the browser still sends **the
    download URL, the referrer chain, and the file hash** to
    `sb-ssl.google.com`, and gets no verdict back.
  - Hash-prefix real-time lookups, which is how Chrome's Standard protection
    works today, are compiled for Chrome-branded builds only.
- **Result:** MoltBrowser has no phishing or malware protection from Safe
  Browsing. Settings still says "Standard protection", and the browser still
  leaks download URLs to Google for nothing in return.
- **What does work locally:**
  - Dangerous-file-type download warnings. These come from File Type
    Policies (delivered by the component updater) and don't depend on the
    Safe Browsing setting: `chrome/browser/download/download_target_determiner.cc:872-950`.
  - Lookalike-domain warnings (likely).
  - HTTPS-First.
  - MoltShield's uBlock "Badware risks" list. It only blocks badware
    *subresources and frames*: MoltShield never blocks a top-level page
    (`src/chrome/browser/molt_ai/shield/molt_shield_navigation_throttle.cc:118-121`),
    so it cannot stop you landing on a phishing page.

## What changed at Chromium 156

Facts only; the decision stays the owner's. Paths are relative to the
Chromium tree at 156.0.8078.48 (unchanged at 156.0.8078.74).

**Hosts and requests a default MoltBrowser window makes.**

| Request | Host and path | Sent | When | At 156 |
|---|---|---|---|---|
| List updates | `safebrowsing.googleapis.com/v5/hashLists:batchGet?$req=...&key=dummytoken` | The list names and their (empty) versions, no URL | 1–5 min after start; a failure backs off 15–30 min, doubling to 24 h | **New**: V5 instead of V4 (`/v4/threatListUpdates:fetch`), because the testing config's study turns `SafeBrowsingLocalListsUseSBv5` on (`components/safe_browsing/core/common/features.cc:392`; `core/browser/db/v5_update_protocol_manager.cc:36-54,187-199`) |
| Full-hash check | `safebrowsing.googleapis.com/v5/hashes:search` (V4: `/v4/fullHashes:find`) | 4-byte hash prefixes of the URL's expressions | Only when a URL matches a local prefix: never, with empty lists | V5 path (`v5_search_hashes_util.cc:225-243`) |
| Download check | `sb-ssl.google.com/safebrowsing/clientreport/download?key=dummytoken` | **The download URL, the referrer chain, the file name, size, SHA-256 and signature details** | Each download of a type Safe Browsing checks | Unchanged (`chrome/browser/safe_browsing/download_protection/download_protection_delegate_desktop.cc:27`) |
| Password reuse report | `sb-ssl.google.com/safebrowsing/clientreport/login` | The reuse event and the page URL | Typing a saved password on another site | Unchanged (`password_protection_service_base.cc:44`) |
| Client-side phishing ping | `sb-ssl.google.com/safebrowsing/clientreport/phishing` | The page URL and its phishing features | Only with the phishing model, which comes from the Optimization Guide (needs an API key); the testing config's component delivery of models lists only the geolocation permission model, and MoltBrowser turns that delivery off | **Likely** (`client_side_detection_service_base.cc:42`; `optimization_guide/core/delivery/prediction_model_component_configs.cc`) |

**Hash-prefix real-time lookups** (how Chrome's Standard protection checks
URLs in real time) are still compiled for Chrome-branded builds only
(`hashprefix_realtime/hash_realtime_utils.cc:20-31,106-109`). At 156 they go
through an Oblivious HTTP relay, `ohttp-relay-safebrowsing-chrome.google.fastly-edge.com`
(it was `google-ohttp-relay-safebrowsing.fastly-edge.com`), with the
gateway's key fetched from `www.gstatic.com/ohttp_gateway/hpke_public_keys/v1/sbc_prod`
(`core/common/features.cc:360-375`). The relay sees the user's IP but not
the request; Google's gateway sees the request (hash prefixes) but not the
IP. Turning this on in MoltBrowser would need a change to the branding
check (an upstream hook), a relay that agrees to carry the traffic (the
Fastly relay is run for Google), and a key, as the inner request still
carries one.

**Enhanced protection** at 156 adds real-time URL checks
(`safebrowsing.google.com/safebrowsing/clientreport/realtime`), suspicious-
site warnings on top of them (`SuspiciousSiteWarnings`, turned on by the
testing config), client-side scam detection with a server model, extension
telemetry and SCT auditing reports. None of it runs without Enhanced
protection, and SCT auditing also needs Certificate Transparency, which is
off without Chrome branding (`chrome/browser/ssl/sct_reporting_service.cc:217-235`).

**What each option would send at 156.**

| | Sent to Google | Sent to GenEye | Browser work at 156 |
|---|---|---|---|
| A. GenEye proxy | GenEye's IP; the V5 list requests and, on a local match, hash prefixes; download checks only if they stay on and are proxied too | Every user's IP and the same requests | The V5 list URL is a literal (`v5_update_protocol_manager.cc:191`), the hash-search URL uses `kSbV5UrlPrefix` (`sb_protocol_manager_util.cc:43,178-180`), the V4 URLs use `kSbV4UrlPrefix`: pointing them at a proxy is a hook in each place, or one in V4 plus turning the V5 study off with an override. An OHTTP relay (as Chrome does for real-time lookups) would hide the IP from GenEye too, but the protocol for lists is plain HTTPS |
| B. Google key in the browser | Every user's IP; the V5 list requests; hash prefixes on a local match; download checks (the download URL and referrers) unless turned off | Nothing | A key that applies only to Safe Browsing: at 156 any configured key still turns on the key-gated services (Optimization Guide hints and models, password affiliations, location, CSD's phishing model through the Optimization Guide); the services gated on *Chrome's own* key (Private AI, Web Store info) stay off with another key |
| C. Open lists via MoltShield | Nothing | Nothing | Unchanged since the memo |
| D. Off | Nothing (no list updates, no download checks) | Nothing | Unchanged: the `safebrowsing.enabled` default; the V5 study then has nothing to update |

## Options

| | A. GenEye proxy (Brave-style) | B. Google API key in the browser | C. Open lists via MoltShield | D. Off, and say so |
|---|---|---|---|---|
| How it works | The browser talks to a GenEye relay (e.g. `sb.geneye.ai`). The relay adds GenEye's key and forwards to Google. Users' IPs and cookies never reach Google | The browser calls Google Safe Browsing (V4 lists, or V5) directly with a GenEye key | MoltShield downloads open malware and phishing lists and checks URLs on the device | Default Safe Browsing to off, and tell users plainly |
| Protection | Close to Chrome's list-based protection. Hash prefixes are checked only on a local match | Same as A | Weaker. Smaller lists, update lag of hours, no download reputation | None from Safe Browsing (the same as today, in practice) |
| What Google learns | GenEye's IP and request counts. Hash prefixes when a URL matches a local prefix. No user IPs | **Every user's IP** and update cadence, hash prefixes on matches, and full download URLs unless download pings stay off | Nothing | Nothing |
| New hosts | `sb.geneye.ai` | Google SB hosts (already listed) | List publishers (below) | Removes `safebrowsing.googleapis.com` and `sb-ssl.google.com` |
| Browser work | Small: point the V4 URL prefix at the relay (at 156 also the V5 URLs, or keep V4: see [What changed at Chromium 156](#what-changed-at-chromium-156)), leave the key out of the binary, keep download pings off. About 1–2 days | Small, but the key must apply **only** to Safe Browsing. Setting `google_api_key` in GN would also turn on every key-gated service in NETWORK_TRAFFIC.md (optimization hints and models, password affiliations, leak checks, Translate, location, speech). About 1–2 days | Catalog entries: hours. Real phishing protection needs a top-level check plus a warning page with "proceed anyway": about 3–5 days | One pref default (`safebrowsing.enabled = false` in `molt_privacy_prefs.cc`) plus copy changes: under a day |
| Running cost | A relay (serverless or a small VM). Low at today's scale; grows with users × the update calls each makes (the server sets the interval; ~30 min would mean ~48 a day). Needs abuse controls (an open-source client can't truly authenticate), monitoring and on-call | No server. A key baked into an open-source binary will be extracted and abused, and if the quota runs out, protection silently stops for everyone | None: the lists are public files | None |
| Legal | **Blocking question.** As far as we know, Google's Safe Browsing API terms limit it to non-commercial use and point commercial users to the paid Web Risk API, which has a different protocol, so the relay would also have to translate. Check the current wording. GenEye is a company, so legal must decide | Same terms problem | Check each list's licence and its sources' terms before shipping | None |
| Fits the privacy policy? | Yes, if disclosed | No: it contradicts "no Safe Browsing calls" and sends user IPs to Google | Yes | Yes, once the policy says it plainly |

**Option C through the existing catalog.** This is a description only; it is
not implemented, because MoltShield is being changed in another workstream.
`src/chrome/browser/molt_ai/shield/filter_list_catalog.cc` already takes
`{id, title, url, fallback_url, trusted, enabled_by_default}` entries, fetches
them through the profile network (Tor in MoltNet), and compiles them into
the same adblock-rust engine. uBlock Origin ships two suitable lists. The
URLs below are taken from uBO's `assets/assets.json`:

```
{"urlhaus-1", "Online Malicious URL Blocklist (URLhaus)",
 "https://malware-filter.gitlab.io/urlhaus-filter/urlhaus-filter-ag-online.txt",
 "https://curbengh.github.io/malware-filter/urlhaus-filter-ag-online.txt",
 /*trusted=*/false, /*enabled_by_default=*/true},      // on by default in uBO
{"curben-phishing", "Phishing URL Blocklist",
 "https://malware-filter.gitlab.io/phishing-filter/phishing-filter.txt",
 "https://curbengh.github.io/phishing-filter/phishing-filter.txt",
 /*trusted=*/false, /*enabled_by_default=*/false},     // off by default in uBO
```

Adding the URLhaus entry blocks known malware URLs loaded as subresources or
frames, with no other changes. That is the cheap part.

Phishing protection needs more. Phishing pages are top-level pages, which the
navigation throttle never blocks today. The throttle would need to check
main-frame navigations against lists marked "security" only, and show an
interstitial instead of an empty page. Security lists should also refresh
more often than daily: the upstream lists rebuild about twice a day.

Before shipping, also:
- check list sizes against `tools/shield_bench`;
- check each list's licence and the terms of its data sources (we could not
  fetch them from this environment);
- add the new hosts to NETWORK_TRAFFIC.md and the privacy policy.

## Recommendation

1. **Now (v0.3.0): D.** Default Safe Browsing to off, and say so in Settings
   copy and the privacy policy. This is the only option that is honest today
   and costs nothing. It stops the download-URL leak and removes a protection
   claim the browser doesn't deliver. Nothing that works today is lost:
   local file-type warnings stay. On the build machine, confirm that Safety
   Tips and lookalike warnings still show with Safe Browsing off.
2. **Next (v0.3.x): C** as the real fix. First the URLhaus entry, which is
   cheap. Then the top-level security interstitial and the phishing list.
   Nothing about your browsing leaves the device, which matches the
   product's promise.
3. **Only if C proves too weak** (measure against a public phishing feed),
   **and legal clears Google's terms or a Web Risk contract: A.** Never B.

Proposed privacy-policy wording for D, for the lead to integrate:

> **Phishing and malware protection.** MoltBrowser does not use Google Safe
> Browsing, so it does not check the sites you visit or the files you
> download against Google's lists. MoltShield blocks known malware and
> tracking resources on the pages you visit. It does not yet warn you
> before you open a phishing site. We are adding on-device phishing
> protection based on open lists.
