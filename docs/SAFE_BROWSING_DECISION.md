# S17: Safe Browsing decision memo

**Status:** decision needed. **Audience:** GenEye lead. **Related:**
[NETWORK_TRAFFIC.md](NETWORK_TRAFFIC.md) (P16).

## Where we are

All of this was read in Chromium source at the pin; file references are in
NETWORK_TRAFFIC.md.

- The macOS arm64, Linux and Windows configs build the full desktop Safe
  Browsing service (`safe_browsing_mode = 1`). It is on by default as
  "Standard protection".
- No API key is configured, so the key is the placeholder `dummytoken`.
  - List updates still go to `safebrowsing.googleapis.com` with that key, and
    we expect Google to reject them. The local blocklist therefore stays
    empty, and every URL check returns "safe".
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

## Options

| | A. GenEye proxy (Brave-style) | B. Google API key in the browser | C. Open lists via MoltShield | D. Off, and say so |
|---|---|---|---|---|
| How it works | The browser talks to a GenEye relay (e.g. `sb.geneye.ai`). The relay adds GenEye's key and forwards to Google. Users' IPs and cookies never reach Google | The browser calls Google Safe Browsing (V4 lists, or V5) directly with a GenEye key | MoltShield downloads open malware and phishing lists and checks URLs on the device | Default Safe Browsing to off, and tell users plainly |
| Protection | Close to Chrome's list-based protection. Hash prefixes are checked only on a local match | Same as A | Weaker. Smaller lists, update lag of hours, no download reputation | None from Safe Browsing (the same as today, in practice) |
| What Google learns | GenEye's IP and request counts. Hash prefixes when a URL matches a local prefix. No user IPs | **Every user's IP** and update cadence, hash prefixes on matches, and full download URLs unless download pings stay off | Nothing | Nothing |
| New hosts | `sb.geneye.ai` | Google SB hosts (already listed) | List publishers (below) | Removes `safebrowsing.googleapis.com` and `sb-ssl.google.com` |
| Browser work | Small: point the V4 URL prefix at the relay, leave the key out of the binary, keep download pings off. About 1–2 days | Small, but the key must apply **only** to Safe Browsing. Setting `google_api_key` in GN would also turn on every key-gated service in NETWORK_TRAFFIC.md (optimization hints and models, password affiliations, leak checks, Translate, location, speech). About 1–2 days | Catalog entries: hours. Real phishing protection needs a top-level check plus a warning page with "proceed anyway": about 3–5 days | One pref default (`safebrowsing.enabled = false` in `molt_privacy_prefs.cc`) plus copy changes: under a day |
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
