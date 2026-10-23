# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The network leak tests: which page each one loads, what it must not
cause outside Tor, and what the page itself must report.

Every test gets its own random nonce. Each canary name a test page uses is
`<kind>-<nonce>.<canary domain>` (a name nobody else looks up), and each test
has its own documentation-range IP literals (198.51.100.0/24,
2001:db8::/32), so any lookup or packet outside Tor that carries them is
attributed to exactly one test, whenever it happens.

Assertions are (result key, operator, expected, title). The operators are
evaluated by report.py (`eq`, `ne`, `in`, `not_in`, `truthy`, `falsy`,
`error_is`, `all_fail`, `absent_or_eq`); a result key may be a dotted path.
"""

# Hosts a test's window must not look up even though they are expected
# background traffic otherwise (docs/NETWORK_TRAFFIC.md).
SAFE_BROWSING_HOSTS = ('sb-ssl.google.com', 'safebrowsing.googleapis.com',
                       'safebrowsing.google.com')
COMPONENT_HOSTS = ('update.googleapis.com', '*.gvt1.com', 'dl.google.com',
                   'edgedl.me.gvt1.com', 'optimizationguide-pa.googleapis.com',
                   'www.gstatic.com', 'translate.googleapis.com',
                   'clients2.google.com', 'clients2.googleusercontent.com')
LOCATION_HOSTS = ('www.googleapis.com', 'location.services.mozilla.com')


class LeakTest(object):
    """One page-driven network test."""

    def __init__(self, test_id, title, page, origin='http', target='moltnet',
                 doc='', wait=3.0, assertions=(), forbidden_hosts=(),
                 via_tor_paths=(), actions=(), timeout=45.0,
                 expect_load=True):
        self.id = test_id
        self.title = title
        self.page = page          # file under pages/, served at /t/<nonce>/
        self.origin = origin      # 'http', 'https', 'https_b', 'aia', 'aia_b'
        self.target = target      # 'moltnet' or 'regular'
        self.doc = doc
        self.wait = wait          # seconds to keep watching after the page
        self.assertions = tuple(assertions)
        self.forbidden_hosts = tuple(forbidden_hosts)
        # Paths (with {nonce}) whose arrival at the kit's server proves the
        # request went through Tor. Informational: their absence is not a
        # leak (e.g. network prediction is off, so no prefetch).
        self.via_tor_paths = tuple(via_tor_paths)
        # Harness steps after the page reports done: ('hover', css),
        # ('eval', js), ('gesture_eval', js), ('click', css).
        self.actions = tuple(actions)
        self.timeout = timeout
        self.expect_load = expect_load

    def as_dict(self):
        return {
            'id': self.id, 'title': self.title, 'page': self.page,
            'origin': self.origin, 'target': self.target, 'doc': self.doc,
            'wait': self.wait,
        }


STEP4 = 'MOLTNET_COMPLIANCE.md §4 step 4'

TESTS = (
    LeakTest(
        'onion_http', 'A MoltNet window loads an http:// onion site',
        'basic.html', origin='http',
        doc='MOLTNET_COMPLIANCE.md §4 steps 1, 11',
        assertions=[('loaded', 'eq', True, 'the page loaded (no HTTPS-Only '
                     'interstitial for http:// onion)'),
                    ('secure_context', 'eq', False,
                     'http:// onion is not a secure context (known gap §5.7)')],
        via_tor_paths=['/t/{nonce}/basic.html']),
    LeakTest(
        'onion_https', 'A MoltNet window loads an https:// onion site',
        'basic.html', origin='https',
        doc='MOLTNET_COMPLIANCE.md §4 step 11',
        assertions=[('loaded', 'eq', True, 'the page loaded')],
        via_tor_paths=['/t/{nonce}/basic.html']),
    LeakTest(
        'webrtc', 'WebRTC: STUN/TURN servers, peer and .local candidates',
        'webrtc.html', origin='https', wait=6.0,
        doc='MOLTNET_COMPLIANCE.md §1.12, §4 step 18',
        assertions=[
            ('pc.error', 'eq', 'NotSupportedError',
             'new RTCPeerConnection() throws NotSupportedError'),
            ('pc.message', 'eq', 'No PeerConnection handler can be created, '
             'perhaps WebRTC is disabled?', "with Chromium's message"),
        ]),
    LeakTest(
        'dns_prefetch', 'DNS prefetch, preconnect, speculation rules, hover',
        'dnsprefetch.html', origin='http', wait=5.0,
        doc='MOLTNET_COMPLIANCE.md §2 (other ways to look up a name)',
        actions=[('hover', '#anchor'), ('sleep', 1.0), ('hover', '#anchor2')]),
    LeakTest(
        'prefetch', '<link rel=prefetch/prerender/preload>, speculation rules',
        'prefetch.html', origin='https', wait=5.0,
        doc='MOLTNET_COMPLIANCE.md §2 (network prediction off)',
        via_tor_paths=['/canary/prefetch-{nonce}', '/canary/preload-{nonce}',
                       '/canary/specprefetch-{nonce}',
                       '/canary/specprerender-{nonce}']),
    LeakTest(
        'fonts_favicons', 'Web fonts, favicons, manifest, images, IP literals',
        'fonts.html', origin='https', wait=4.0,
        doc='MOLTNET_COMPLIANCE.md §2 (favicons through MoltNet\'s context)',
        via_tor_paths=['/canary/font-{nonce}.woff2', '/favicon.ico']),
    LeakTest(
        'beacon_ping', 'sendBeacon, keepalive fetch, <a ping>, pagehide',
        'beacon.html', origin='http', wait=4.0,
        doc='MOLTNET_COMPLIANCE.md §2 (sendBeacon and <a ping> through the '
            'proxy)',
        actions=[('eval', 'mlGo()'), ('sleep', 3.0)],
        via_tor_paths=['/canary/beacon-{nonce}', '/canary/ping-{nonce}']),
    LeakTest(
        'csp_reporting', 'CSP report-uri/report-to, Reporting-Endpoints, NEL',
        'csp.html', origin='https', wait=6.0,
        doc='MOLTNET_COMPLIANCE.md §2 (Reporting API and NEL through the '
            'proxy)',
        via_tor_paths=['/canary/csp-{nonce}']),
    LeakTest(
        'websocket', 'WebSockets to a canary name, an IP literal and the '
                     'onion',
        'websocket.html', origin='http', wait=3.0,
        doc='MOLTNET_COMPLIANCE.md §2 (WebSockets through the proxy)',
        assertions=[('ws_same', 'eq', 'open',
                     'a WebSocket to the onion opens (through Tor)')],
        via_tor_paths=['/ws/{nonce}']),
    LeakTest(
        'webtransport', 'WebTransport (HTTP/3 over UDP)',
        'webtransport.html', origin='https', wait=3.0,
        doc='MOLTNET_COMPLIANCE.md §2 (WebTransport refuses a proxy)',
        assertions=[('wt_canary.ok', 'falsy', None,
                     'WebTransport to a canary name fails'),
                    ('wt_ip.ok', 'falsy', None,
                     'WebTransport to an IP literal fails')]),
    LeakTest(
        'workers', 'Service worker, dedicated and shared worker fetches',
        'sw.html', origin='https', wait=4.0, timeout=60.0,
        doc='MOLTNET_COMPLIANCE.md §2; FINGERPRINTING.md (workers)',
        assertions=[('sw.registered', 'eq', True,
                     'the service worker registered')],
        via_tor_paths=['/canary/sw-{nonce}', '/canary/worker-{nonce}',
                       '/canary/shared-{nonce}']),
    LeakTest(
        'download', 'A download (no Safe Browsing ping outside Tor)',
        'download.html', origin='https', wait=8.0,
        doc='MOLTNET_COMPLIANCE.md §1.14, §4 step 20',
        forbidden_hosts=SAFE_BROWSING_HOSTS,
        via_tor_paths=['/dl/{nonce}/molt-leak-test.exe']),
    LeakTest(
        'localhost_lan', 'localhost, loopback, the machine\'s own and LAN '
                         'addresses',
        'localhost.html', origin='http', wait=3.0, timeout=60.0,
        doc='MOLTNET_COMPLIANCE.md §2 (<-loopback>), §5.7',
        assertions=[('fetch', 'all_fail', None,
                     'every local/LAN fetch fails (Tor refuses it)')]),
    LeakTest(
        'quic', 'HTTP/3 and QUIC (Alt-Svc on the onion, a canary name)',
        'quic.html', origin='https', wait=4.0,
        doc=STEP4,
        via_tor_paths=['/canary/quic2-{nonce}']),
    LeakTest(
        'mdns', 'mDNS (.local names)',
        'mdns.html', origin='http', wait=4.0,
        doc='MOLTNET_COMPLIANCE.md §1.12, §2'),
    LeakTest(
        'cert_fetch', 'Certificate fetches (AIA; OCSP/CRL URLs in the '
                      'certificate)',
        'basic.html', origin='aia', wait=8.0, expect_load=False,
        doc='MOLTNET_COMPLIANCE.md §2 (certificate fetches through Tor)',
        via_tor_paths=['/canary/aia-{nonce}.crt']),
    # The same certificate on a second site, so the browser fetches the
    # same AIA URL for it; read with cert_fetch by report.py's
    # identity.cert_fetch_circuits check.
    LeakTest(
        'cert_fetch_b', 'Certificate fetches for a second site (the same '
                        'AIA URL)',
        'basic.html', origin='aia_b', wait=8.0, expect_load=False,
        doc='MOLTNET_COMPLIANCE.md §2 (Certificate fetches), §4 step 8'),
    # Two pages their servers sandbox (opaque origins) on two sites; read
    # by report.py's identity.sandboxed_circuits check.
    LeakTest(
        'sandboxed_a', 'A page its server sandboxes, on one site',
        'sandboxed.html', origin='http', wait=3.0,
        doc='MOLTNET_COMPLIANCE.md §1.4, §4 step 8',
        assertions=[('opaque_origin', 'eq', True,
                     'the page has an opaque origin (CSP sandbox)')],
        via_tor_paths=['/canary/sbx-{nonce}.png']),
    LeakTest(
        'sandboxed_b', 'A page its server sandboxes, on another site',
        'sandboxed.html', origin='https_b', wait=3.0,
        doc='MOLTNET_COMPLIANCE.md §1.4, §4 step 8',
        assertions=[('opaque_origin', 'eq', True,
                     'the page has an opaque origin (CSP sandbox)')],
        via_tor_paths=['/canary/sbx-{nonce}.png']),
    LeakTest(
        'builtin_ai', 'Built-in AI APIs and Translator (no component '
                      'download)',
        'ai.html', origin='https', wait=10.0, timeout=60.0,
        doc='MOLTNET_COMPLIANCE.md §1.14; FINGERPRINTING.md "Built-in AI '
            'APIs"',
        actions=[('gesture_eval', 'mlCreate()'), ('sleep', 2.0)],
        forbidden_hosts=COMPONENT_HOSTS,
        assertions=[
            ('availability.LanguageModel', 'in', ['unavailable', 'absent'],
             'LanguageModel is unavailable'),
            ('availability.Summarizer', 'in', ['unavailable', 'absent'],
             'Summarizer is unavailable'),
            ('availability.Writer', 'in', ['unavailable', 'absent'],
             'Writer is unavailable'),
            ('availability.Rewriter', 'in', ['unavailable', 'absent'],
             'Rewriter is unavailable'),
            ('availability.Translator', 'in', ['unavailable', 'absent'],
             'Translator is unavailable'),
        ]),
    LeakTest(
        'geolocation', 'Geolocation is denied without a prompt or provider',
        'geo.html', origin='https', wait=3.0,
        doc='MOLTNET_COMPLIANCE.md §1.14, §4 step 20',
        forbidden_hosts=LOCATION_HOSTS,
        assertions=[('geo.code', 'eq', 1, 'getCurrentPosition fails with '
                     'PERMISSION_DENIED'),
                    ('geo.ms', 'lt', 3000, 'at once (no prompt)')]),
    # Run by the extension phase, only when the test extension loaded. Its
    # background page looks up exttab-/extreq-<nonce> if it ever sees a
    # MoltNet tab or request.
    LeakTest(
        'extension', 'A test extension allowed in Incognito, in MoltNet',
        'ext.html', origin='http', wait=4.0,
        doc='MOLTNET_COMPLIANCE.md §1.11, §4 step 16',
        assertions=[('marked', 'falsy', None,
                     'its content script did not mark the MoltNet page'),
                    ('runtime', 'falsy', None,
                     'chrome.runtime is not exposed to the page'),
                    ('war.ok', 'falsy', None,
                     'its web-accessible resource can\'t be fetched')]),
)

# Run in a regular window in the same session (MOLTNET_COMPLIANCE.md §4
# step 12): onion names must never be looked up or sent anywhere.
REGULAR_TESTS = (
    LeakTest(
        'regular_onion_navigation', 'Regular window: typing an onion address',
        None, target='regular', wait=3.0, expect_load=False,
        doc='MOLTNET_COMPLIANCE.md §1.9, §4 step 12'),
    LeakTest(
        'regular_onion_subresources', 'Regular window: onion subresources, '
                                      'fetch, WebSocket, workers',
        'onion_regular.html', origin='local', target='regular', wait=4.0,
        timeout=60.0,
        doc='MOLTNET_COMPLIANCE.md §1.9, §4 step 13',
        assertions=[('requests', 'all_fail', None,
                     'every request to the onion host fails')]),
)

ALL_TESTS = TESTS + REGULAR_TESTS


def by_id(test_id):
    for test in ALL_TESTS:
        if test.id == test_id:
            return test
    return None


# MOLTNET_COMPLIANCE.md §4: which steps the kit automates. Used for the
# report's "still manual" list and kept in step with docs/LEAK_TESTING.md.
COMPLIANCE_STEPS = (
    (1, 'You are on Tor', 'partial',
     'check.torproject.org through Tor and the panel\'s Verify (needs an exit '
     'to reach the public internet); the onion tests prove Tor routing '
     'offline'),
    (2, 'Regular windows are untouched', 'partial',
     'a regular window reaches the kit\'s local page directly, not through '
     'Tor; the toolbar label stays manual'),
    (3, 'No IP, DNS, WebRTC or IPv6 leak (leak sites)', 'replaced',
     'the leak suite tests the same mechanisms locally; the public sites stay '
     'an optional manual cross-check'),
    (4, 'Watch the wire', 'automated',
     'every packet is captured in a namespace where only Tor may leave'),
    (5, 'Fail-closed', 'automated',
     'tor is killed: pages must fail with a proxy error; after a restart no '
     'MoltNet window returns'),
    (6, 'Separate identity', 'automated',
     'a regular-profile cookie doesn\'t show in MoltNet; a MoltNet cookie is '
     'gone after the last window closes'),
    (7, 'New Identity', 'automated',
     'new profile, cookies gone, new canvas key, no old page left open, a '
     'second New Identity waits for Tor to switch circuits; the exit IP '
     'check needs the public internet'),
    (8, 'Per-site circuits', 'automated',
     'different SOCKS usernames per site (sandboxed pages included), a '
     'SOCKS username of its own for each certificate fetch, read from the '
     'loopback capture and Tor\'s circuit list'),
    (9, 'Overrides are refused', 'partial',
     '--proxy-server blocks MoltNet; the extension-set proxy case is manual'),
    (10, 'The relays are real', 'automated',
     'the circuit\'s fingerprints are in the consensus Tor downloaded; the '
     'onionoo lookup is optional'),
    (11, 'Onion sites', 'partial',
     'http:// and https:// onions load; the Onion-Location bar is browser UI '
     'and stays manual'),
    (12, 'No onion names outside Tor (regular window)', 'automated',
     'the interstitial shows and no onion name is looked up; DoH is covered '
     'because every lookup lands on the kit\'s resolver'),
    (13, 'No onion names to a proxy (regular window)', 'partial',
     'a logging proxy must see no onion request (image, fetch, redirect, '
     'WebSocket, dedicated and shared worker); the service worker case is '
     'manual'),
    (14, 'Bridges with the distribution\'s transports', 'manual', ''),
    (15, 'Your settings don\'t follow you', 'automated',
     'seeded regular-profile settings (Do Not Track, fonts, language, '
     'third-party cookies, a blocked JavaScript site) must not show in '
     'MoltNet'),
    (16, 'Extensions stay out', 'partial',
     'a test extension allowed in Incognito must not see or mark MoltNet '
     'pages; split mode, shortcuts and the toolbar stay manual'),
    (17, 'No regular-profile carry-over', 'partial',
     'autoplay policy, persist(), third-party cookies and Shared Storage are '
     'read in MoltNet; media engagement, bookmarks and certificate '
     'exceptions need a prepared regular profile'),
    (18, 'No WebRTC, and no name lookups for it', 'automated', ''),
    (19, 'Incognito\'s protections', 'manual',
     'DevTools prefs, media controls, macOS, picture-in-picture, GCM, '
     'printing'),
    (20, 'Nothing goes around Tor', 'partial',
     'downloads, geolocation and the built-in AI are automated; the AI chat, '
     'Cast menu, phone passkeys and "Open all" bookmarks stay manual'),
)
