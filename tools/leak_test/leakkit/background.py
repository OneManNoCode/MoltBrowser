# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The browser's own background traffic, from docs/NETWORK_TRAFFIC.md.

Not caused by MoltNet windows (MOLTNET_COMPLIANCE.md §5.6), so a lookup of
an EXPECTED host outside Tor is reported, not failed, unless a test
forbids it during its window (a Safe Browsing ping during a MoltNet
download, a component download after a built-in AI call). A host in
MUST_NOT_APPEAR fails wherever it shows up (NETWORK_TRAFFIC.md "Pass
criteria"). Any other name looked up outside Tor fails as "unexplained"
until someone has looked at it: either it is new background traffic
(add it here with the reason, and to NETWORK_TRAFFIC.md) or it is a leak.

Patterns: an exact host, or "*.suffix" for every name under suffix.

The traffic capture (bgcapture.py) uses the full table in
NETWORK_TRAFFIC.md section 6 instead (hostlist.py). tests/
test_traffic_capture.py checks that these lists agree with it: every
EXPECTED pattern is a row the doc doesn't forbid, and every MUST_NOT_APPEAR
pattern a row it forbids (or allows only after a user action).
"""

EXPECTED = (
    ('update.googleapis.com', 'component updater (NETWORK_TRAFFIC.md §1)'),
    ('*.gvt1.com', 'component downloads, spell-check dictionaries (§1)'),
    ('dl.google.com', 'component downloads (§1)'),
    ('clients2.google.com', 'network time; Web Store updates (§1, §3)'),
    ('safebrowsing.googleapis.com', 'Safe Browsing list updates (§1)'),
    ('sb-ssl.google.com', 'Safe Browsing download checks (§2)'),
    ('duckduckgo.com', 'default search engine suggestions (§4)'),
    ('api.github.com', 'MoltBrowser update check (§5)'),
    ('github.com', 'MoltBrowser update check (§5)'),
    ('objects.githubusercontent.com', 'MoltBrowser update download (§5)'),
    ('release-assets.githubusercontent.com', 'MoltBrowser update (§5)'),
    ('raw.githubusercontent.com', 'MoltShield list fallback (§5)'),
    ('easylist.to', 'MoltShield filter lists (§5)'),
    ('ublockorigin.github.io', 'MoltShield filter lists (§5)'),
    ('secure.fanboy.co.nz', 'MoltShield optional lists (§5)'),
    ('cloudflare-dns.com', 'MoltBrowser\'s DNS TXT resolver prefix '
                           '(chrome_content_browser_client.cc)'),
)

MUST_NOT_APPEAR = (
    ('content-autofill.googleapis.com', 'autofill crowdsourcing is off'),
    ('passwordsleakcheck-pa.googleapis.com', 'the password leak check is '
                                             'off'),
    ('clientservices.googleapis.com', 'variations seed: not fetched'),
    ('accounts.google.com', 'no Google sign-in'),
    ('optimizationguide-pa.googleapis.com', 'needs a Google API key'),
    ('mtalk.google.com', 'push messaging starts only on subscription'),
    ('gemini.google.com', 'Gemini in Chrome needs a Google account'),
    ('connectivitycheck.gstatic.com', 'captive-portal probe is off'),
    ('www.google.com', 'AI Mode eligibility and other Google calls are '
                       'off'),
    ('translate.googleapis.com', 'Translate runs on the device'),
    ('translate-pa.googleapis.com', 'Translate runs on the device'),
)

# Public resolvers a browser can reach by IP for DNS over HTTPS / TLS.
DOH_IPS = (
    '1.1.1.1', '1.0.0.1', '8.8.8.8', '8.8.4.4', '9.9.9.9', '149.112.112.112',
    '208.67.222.222', '208.67.220.220', '94.140.14.14', '94.140.15.15',
    '185.228.168.9', '2606:4700:4700::1111', '2606:4700:4700::1001',
    '2001:4860:4860::8888', '2001:4860:4860::8844', '2620:fe::fe',
)


def matches(host, pattern):
    host = host.lower().rstrip('.')
    if pattern.startswith('*.'):
        suffix = pattern[1:]
        return host.endswith(suffix) and len(host) > len(suffix)
    return host == pattern


def lookup(host, table):
    for pattern, why in table:
        if matches(host, pattern):
            return pattern, why
    return None


def is_forbidden_in(host, patterns):
    return any(matches(host, p) for p in patterns)
