# MoltNet leak test report

**Verdict: FAIL**  (12 FAIL, 0 ERROR, 171 PASS, 85 SKIP, 2 INFO)

| | |
|---|---|
| Run | sample |
| Started | 2025-10-09T08:53:20+00:00 |
| Browser | MoltBrowser 0.3.0 |
| Window under test | moltnet |
| OS (as pages see it) | Linux |
| Kit | 1.0 (moltleak-report/1) |

Statuses: FAIL = a promise is broken (or traffic needs triage); ERROR = the kit could not run the check; SKIP = not applicable or not run; INFO = recorded without a verdict.

## harness

| Status | Check | Reference |
|---|---|---|
| FAIL | Nothing left the namespace except Tor's own connections | docs/LEAK_TESTING.md "How the capture works" |
| PASS | The packet capture recorded traffic |  |
| PASS | The seeded regular-profile settings took effect (control for the settings checks) | MOLTNET_COMPLIANCE.md §4 step 15 |
| PASS | Tor (and only Tor) reached the network | MOLTNET_COMPLIANCE.md §4 step 4 ("only the Tor guard (or bridge) should be contacted") |
| PASS | A MoltNet window opened (Tor bootstrapped, routing verified) | MOLTNET_COMPLIANCE.md §2 Fail-closed |
| SKIP | The test extension loaded and ran in a regular window (control for the extension test) | MOLTNET_COMPLIANCE.md §4 step 16 |

- **FAIL** Nothing left the namespace except Tor's own connections
  - escaped: tcp 10.231.7.2 -> 203.0.113.99:80 (1 packets)
- **PASS** The packet capture recorded traffic
  - all: 74 frames, 74 IP, 0 malformed
  - firewall log: 12 frames
  - Tor connections: 2 frames
  - loopback packets: 34
- **PASS** The seeded regular-profile settings took effect (control for the settings checks)
  - regular window: {"doNotTrack": "1", "fontSize": "24px", "js_ran_on_blocked": false, "languages": ["de-DE", "de"]}
- **PASS** Tor (and only Tor) reached the network
  - tcp 192.0.2.50:9001, 1 connection(s)
  - tcp 192.0.2.51:443, 1 connection(s)
- **PASS** A MoltNet window opened (Tor bootstrapped, routing verified)
  - openWindow -> {"running": true}
  - MoltNet page target in its own browser context after 12.5s
- **SKIP** The test extension loaded and ran in a regular window (control for the extension test)
  - --no-extension

## network

| Status | Check | Reference |
|---|---|---|
| FAIL | Built-in AI APIs and Translator (no component download): nothing outside Tor | MOLTNET_COMPLIANCE.md §1.14; FINGERPRINTING.md "Built-in AI APIs" |
| FAIL | A download (no Safe Browsing ping outside Tor): nothing outside Tor | MOLTNET_COMPLIANCE.md §1.14, §4 step 20 |
| FAIL | localhost, loopback, the machine's own and LAN addresses: nothing outside Tor | MOLTNET_COMPLIANCE.md §2 (<-loopback>), §5.7 |
| FAIL | No leak-shaped traffic outside the tests (onion names, mDNS, LAN, STUN, QUIC, canaries) | MOLTNET_COMPLIANCE.md §4 step 4 |
| FAIL | Every lookup or connection outside Tor is explained | triage: a new background host goes into leakkit/background.py and docs/NETWORK_TRAFFIC.md; anything else is a leak |
| FAIL | WebRTC: STUN/TURN servers, peer and .local candidates: nothing outside Tor | MOLTNET_COMPLIANCE.md §1.12, §4 step 18 |
| PASS | A MoltNet page's forms cause no Autofill server query or address-rule download, also with the queries on | MOLTNET_COMPLIANCE.md §2 Requests outside Tor, §4 step 20 |
| PASS | Built-in AI APIs and Translator (no component download): LanguageModel is unavailable | MOLTNET_COMPLIANCE.md §1.14; FINGERPRINTING.md "Built-in AI APIs" |
| PASS | Built-in AI APIs and Translator (no component download): Summarizer is unavailable | MOLTNET_COMPLIANCE.md §1.14; FINGERPRINTING.md "Built-in AI APIs" |
| PASS | Built-in AI APIs and Translator (no component download): Writer is unavailable | MOLTNET_COMPLIANCE.md §1.14; FINGERPRINTING.md "Built-in AI APIs" |
| PASS | Built-in AI APIs and Translator (no component download): Rewriter is unavailable | MOLTNET_COMPLIANCE.md §1.14; FINGERPRINTING.md "Built-in AI APIs" |
| PASS | Built-in AI APIs and Translator (no component download): Translator is unavailable | MOLTNET_COMPLIANCE.md §1.14; FINGERPRINTING.md "Built-in AI APIs" |
| PASS | Certificate fetches (AIA; OCSP/CRL URLs in the certificate): nothing outside Tor | MOLTNET_COMPLIANCE.md §2 (certificate fetches through Tor) |
| PASS | Certificate fetches for a second site (the same AIA URL): nothing outside Tor | MOLTNET_COMPLIANCE.md §2 (Certificate fetches), §4 step 8 |
| PASS | DNS prefetch, preconnect, speculation rules, hover: nothing outside Tor | MOLTNET_COMPLIANCE.md §2 (other ways to look up a name) |
| PASS | localhost, loopback, the machine's own and LAN addresses: every local/LAN fetch fails (Tor refuses it) | MOLTNET_COMPLIANCE.md §2 (<-loopback>), §5.7 |
| PASS | A MoltNet window loads an http:// onion site: the page loaded (no HTTPS-Only interstitial for http:// onion) | MOLTNET_COMPLIANCE.md §4 steps 1, 11 |
| PASS | A MoltNet window loads an http:// onion site: http:// onion is not a secure context (known gap §5.7) | MOLTNET_COMPLIANCE.md §4 steps 1, 11 |
| PASS | A MoltNet window loads an http:// onion site: nothing outside Tor | MOLTNET_COMPLIANCE.md §4 steps 1, 11 |
| PASS | A page its server sandboxes, on one site: the page has an opaque origin (CSP sandbox) | MOLTNET_COMPLIANCE.md §1.4, §4 step 8 |
| PASS | A page its server sandboxes, on one site: nothing outside Tor | MOLTNET_COMPLIANCE.md §1.4, §4 step 8 |
| PASS | A page its server sandboxes, on another site: the page has an opaque origin (CSP sandbox) | MOLTNET_COMPLIANCE.md §1.4, §4 step 8 |
| PASS | A page its server sandboxes, on another site: nothing outside Tor | MOLTNET_COMPLIANCE.md §1.4, §4 step 8 |
| PASS | WebRTC: STUN/TURN servers, peer and .local candidates: new RTCPeerConnection() throws NotSupportedError | MOLTNET_COMPLIANCE.md §1.12, §4 step 18 |
| PASS | WebRTC: STUN/TURN servers, peer and .local candidates: with Chromium's message | MOLTNET_COMPLIANCE.md §1.12, §4 step 18 |
| SKIP | mDNS (.local names) | MOLTNET_COMPLIANCE.md §1.12, §2 |

- **FAIL** Built-in AI APIs and Translator (no component download): nothing outside Tor
  - unexplained during this test: dns cdn.example.net A x1 at +65.5s: a name looked up outside Tor that no list explains [firewall log]
- **FAIL** A download (no Safe Browsing ping outside Tor): nothing outside Tor
  - LEAK dns sb-ssl.google.com A x1 at +45.0s: looked up outside Tor during this test: Safe Browsing download checks (§2) [firewall log]
  - not requested (info): /dl/dddddd4444/molt-leak-test.exe
- **FAIL** localhost, loopback, the machine's own and LAN addresses: nothing outside Tor
  - LEAK local 127.0.0.1:47321 (GET / HTTP/1.1) x1 at +55.0s: a page reached a service on this machine (the loopback canary) [firewall log, loopback canary]
- **FAIL** No leak-shaped traffic outside the tests (onion names, mDNS, LAN, STUN, QUIC, canaries)
  - dns bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbd.onion A x1 at +5.0s: an onion name was looked up outside Tor [firewall log]
  - multicast 224.0.0.251 (joined multicast group 224.0.0.251) x1 at +8.0s: a socket joined the mDNS group [firewall log]
  - mdns printer.local PTR x1 at +9.0s: multicast DNS on the local network [firewall log]
- **FAIL** Every lookup or connection outside Tor is explained
  - dns weird.example.org A x1 at +6.0s: a name looked up outside Tor that no list explains [firewall log]
- **FAIL** WebRTC: STUN/TURN servers, peer and .local candidates: nothing outside Tor
  - LEAK dns stun-bbbbbb2222.moltleak.test A x1 at +21.0s: a test page's canary name was looked up outside Tor [firewall log, kit resolver]
  - LEAK dns stun-bbbbbb2222.moltleak.test AAAA x1 at +21.1s: a test page's canary name was looked up outside Tor [firewall log]
  - LEAK udp 198.51.100.12:3478 x1 at +22.0s: a packet to a test page's IP literal outside Tor [firewall log]
- **PASS** A MoltNet page's forms cause no Autofill server query or address-rule download, also with the queries on
  - flags: --enable-features=AutofillServerCommunication,ReduceAcceptLanguage
  - form page: loaded
  - no connection to content-autofill.googleapis.com, chromium-i18n.appspot.com, through Tor or outside it, during the phase
- **PASS** Built-in AI APIs and Translator (no component download): LanguageModel is unavailable
  - availability.LanguageModel = "unavailable"
- **PASS** Built-in AI APIs and Translator (no component download): Summarizer is unavailable
  - availability.Summarizer = "unavailable"
- **PASS** Built-in AI APIs and Translator (no component download): Writer is unavailable
  - availability.Writer = "unavailable"
- **PASS** Built-in AI APIs and Translator (no component download): Rewriter is unavailable
  - availability.Rewriter = "unavailable"
- **PASS** Built-in AI APIs and Translator (no component download): Translator is unavailable
  - availability.Translator = "unavailable"
- **PASS** Certificate fetches (AIA; OCSP/CRL URLs in the certificate): nothing outside Tor
  - sent to Tor inside SOCKS5: aia-kkkkkk1212.moltleak.test:80 (username 'opaque\\x20null\\x20[internally:\\x20(44dd)\\x20anonymous]')
  - sent to Tor inside SOCKS5: aia-kkkkkk1212.moltleak.test:80 (username 'opaque\\x20null\\x20[internally:\\x20(66ff)\\x20anonymous]')
  - reached the kit's page server (through Tor): /canary/aia-kkkkkk1212.crt
- **PASS** DNS prefetch, preconnect, speculation rules, hover: nothing outside Tor
  - sent to Tor inside SOCKS5: anchor-cccccc3333.moltleak.test:443 (username 'http://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad.onion')
- **PASS** localhost, loopback, the machine's own and LAN addresses: every local/LAN fetch fails (Tor refuses it)
  - fetch = {"gateway": {"error": "TypeError", "ok": false}, "loopback4": {"error": "TypeError", "ok": false}}
- **PASS** A MoltNet window loads an http:// onion site: the page loaded (no HTTPS-Only interstitial for http:// onion)
  - loaded = true
- **PASS** A MoltNet window loads an http:// onion site: http:// onion is not a secure context (known gap §5.7)
  - secure_context = false
- **PASS** A MoltNet window loads an http:// onion site: nothing outside Tor
  - reached the kit's page server (through Tor): /t/aaaaaa1111/basic.html
- **PASS** A page its server sandboxes, on one site: the page has an opaque origin (CSP sandbox)
  - opaque_origin = true
- **PASS** A page its server sandboxes, on one site: nothing outside Tor
  - not requested (info): /canary/sbx-iiiiii9999.png
- **PASS** A page its server sandboxes, on another site: the page has an opaque origin (CSP sandbox)
  - opaque_origin = true
- **PASS** A page its server sandboxes, on another site: nothing outside Tor
  - not requested (info): /canary/sbx-jjjjjj0000.png
- **PASS** WebRTC: STUN/TURN servers, peer and .local candidates: new RTCPeerConnection() throws NotSupportedError
  - pc.error = "NotSupportedError"
- **PASS** WebRTC: STUN/TURN servers, peer and .local candidates: with Chromium's message
  - pc.message = "No PeerConnection handler can be created, perhaps WebRTC is disabled?"
- **SKIP** mDNS (.local names)
  - not selected

## regular-window

| Status | Check | Reference |
|---|---|---|
| FAIL | Regular window: no onion request reaches a configured proxy | MOLTNET_COMPLIANCE.md §4 step 13 |
| PASS | Regular window: typing an onion address: no onion name looked up, nothing leaked | MOLTNET_COMPLIANCE.md §1.9, §4 step 12 |
| PASS | Regular window: an onion address shows the "Tor onion site" page | MOLTNET_COMPLIANCE.md §1.9, §4 step 12 |

- **FAIL** Regular window: no onion request reaches a configured proxy
  - the proxy saw 1 request(s)
  - ONION via proxy: GET http://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad.onion/x
- **PASS** Regular window: an onion address shows the "Tor onion site" page
  - title: 'Tor onion site'
  - url: http://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad.onion/

## identity

| Status | Check | Reference |
|---|---|---|
| PASS | Certificate fetches made for two sites use SOCKS usernames of their own (a circuit per fetch) | MOLTNET_COMPLIANCE.md §2 (Certificate fetches), §4 step 8 |
| PASS | Fail-closed: with Tor killed, pages fail with a proxy error | MOLTNET_COMPLIANCE.md §4 step 5 |
| PASS | IncognitoModeAvailability 2 (Incognito only) keeps MoltNet open and lets it open | MOLTNET_COMPLIANCE.md §2 Incognito, §4 step 9 |
| PASS | IncognitoModeAvailability 1 set while MoltNet is open closes every MoltNet window, its DevTools window and a page waiting on "Leave site?" included; MoltNet then refuses to open, saying why, until the policy is gone | MOLTNET_COMPLIANCE.md §2 Incognito, §4 step 9 |
| PASS | A language policy set while MoltNet is open (ForcedLanguages, ReduceAcceptLanguageEnabled) changes neither navigator.languages nor Accept-Language in MoltNet, and MoltNet stays open | MOLTNET_COMPLIANCE.md §1.3, §4 step 9 |
| PASS | New Identity: new profile, cookies gone, new farbling seed | MOLTNET_COMPLIANCE.md §4 step 7 |
| PASS | No MoltNet window comes back after a restart | MOLTNET_COMPLIANCE.md §4 step 5 |
| PASS | Nothing the browser holds for a page is sent after its identity ends (fetchLater, a pagehide beacon) | MOLTNET_COMPLIANCE.md §2 After an identity ends |
| PASS | A --proxy-server override blocks MoltNet | MOLTNET_COMPLIANCE.md §4 step 9 |
| PASS | Turning off per-site partitioning (connections, HTTP cache, third-party storage) blocks MoltNet | MOLTNET_COMPLIANCE.md §4 step 9 |
| PASS | Different sites use different SOCKS usernames (per-site circuits) | MOLTNET_COMPLIANCE.md §1.4, §4 step 8 |
| PASS | A proxy policy set while MoltNet is open closes its windows, nothing leaves outside Tor meanwhile, and MoltNet refuses to open until the policy is gone | MOLTNET_COMPLIANCE.md §2 Fail-closed, §4 step 9 |
| PASS | The panel's relays are in the consensus Tor downloaded | MOLTNET_COMPLIANCE.md §4 step 10 |
| PASS | Sandboxed pages (opaque origins) on two sites use different SOCKS usernames | MOLTNET_COMPLIANCE.md §1.4, §4 step 8 |
| PASS | Separate identity: cookies don't cross, and go with the last window | MOLTNET_COMPLIANCE.md §4 step 6 |
| PASS | A site visited again in a new MoltNet session (after the last window closed, after New Identity) uses new SOCKS credentials, so new circuits | MOLTNET_COMPLIANCE.md §1.2-§1.4, §4 steps 6 and 7 |
| PASS | Tor runs with the shared capacity MoltNet is sized for (ConnLimit, MaxClientCircuitsPending) | MOLTNET_COMPLIANCE.md §2 (Tor's capacity) |
| SKIP | A Background Fetch goes to Tor with its site's SOCKS username (its site's circuit, not its destination's) | MOLTNET_COMPLIANCE.md §2 (Shared network state), §4 step 8 |
| SKIP | check.torproject.org says the MoltNet window uses Tor | MOLTNET_COMPLIANCE.md §4 step 1 |

- **PASS** Certificate fetches made for two sites use SOCKS usernames of their own (a circuit per fetch)
  - cert_fetch -> certificate fetches with SOCKS usernames ['opaque\\x20null\\x20[internally:\\x20(33cc)\\x20anonymous]', 'opaque\\x20null\\x20[internally:\\x20(44dd)\\x20anonymous]']
  - cert_fetch_b -> certificate fetches with SOCKS usernames ['opaque\\x20null\\x20[internally:\\x20(55ee)\\x20anonymous]', 'opaque\\x20null\\x20[internally:\\x20(66ff)\\x20anonymous]']
- **PASS** Fail-closed: with Tor killed, pages fail with a proxy error
  - tor killed: [4242]
  - navigation after the kill: net::ERR_SOCKS_CONNECTION_FAILED
- **PASS** IncognitoModeAvailability 2 (Incognito only) keeps MoltNet open and lets it open
  - 2 of 2 MoltNet page(s) still open 12 s after the policy was written
  - openWindow with the policy -> {"running": true}; a MoltNet page opened: True
- **PASS** IncognitoModeAvailability 1 set while MoltNet is open closes every MoltNet window, its DevTools window and a page waiting on "Leave site?" included; MoltNet then refuses to open, saying why, until the policy is gone
  - flags: --auto-open-devtools-for-tabs
  - 2 MoltNet page(s) open, 1 of them DevTools (in a window of its own: True); "Leave site?" prompt: beforeunload
  - every one of them closed 5.5 s after the policy was written
  - openWindow with the policy -> {"error": "Incognito is turned off (IncognitoModeAvailability or parental controls), and MoltNet windows follow it"}; MoltNet pages: 0
  - openWindow once the policy was removed -> {"running": true}; a MoltNet page opened: True
- **PASS** A language policy set while MoltNet is open (ForcedLanguages, ReduceAcceptLanguageEnabled) changes neither navigator.languages nor Accept-Language in MoltNet, and MoltNet stays open
  - policy {"ForcedLanguages": ["ja"], "ReduceAcceptLanguageEnabled": false}
  - moltnet page: before {"accept_language": "en-US", "languages": ["en-US", "en"]}, after {"accept_language": "en-US", "languages": ["en-US", "en"]}
  - regular page: before {"accept_language": "de-DE", "languages": ["de-DE", "de"]}, after {"accept_language": "ja,de-DE;q=0.9,de;q=0.8", "languages": ["ja", "de-DE", "de"]}
  - the regular page changed and MoltNet's did not
- **PASS** New Identity: new profile, cookies gone, new farbling seed
  - newIdentity -> {"running": true}
  - cookie gone after New Identity
  - canvas readback changed (new farbling seed)
  - no page of the old identity left (one asked "Leave site?": True)
  - a second New Identity right after the first finished 10.4 s later, once Tor had switched circuits
- **PASS** No MoltNet window comes back after a restart
  - 0 MoltNet page(s) after relaunching with the same profile
- **PASS** Nothing the browser holds for a page is sent after its identity ends (fetchLater, a pagehide beacon)
  - set up in the old page: {"fetch_later": true}
  - none reached the kit's server
- **PASS** A --proxy-server override blocks MoltNet
  - launched with --proxy-server=http://127.0.0.1:18888
  - openWindow -> {"error": "A command-line flag controls the proxy."}
  - MoltNet pages: 0
- **PASS** Turning off per-site partitioning (connections, HTTP cache, third-party storage) blocks MoltNet
  - --disable-features=PartitionConnectionsByNetworkIsolationKey: openWindow -> {"error": "MoltNet keeps each site's connections, cache and storage apart, and a command-line flag or field trial has turned that off for the whole browser (PartitionConnectionsByNetworkIsolationKey),; MoltNet pages: 0
  - --disable-features=SplitCacheByNetworkIsolationKey: openWindow -> {"error": "MoltNet keeps each site's connections, cache and storage apart, and a command-line flag or field trial has turned that off for the whole browser (SplitCacheByNetworkIsolationKey), so MoltNe; MoltNet pages: 0
  - --disable-features=ThirdPartyStoragePartitioning: openWindow -> {"error": "MoltNet keeps each site's connections, cache and storage apart, and a command-line flag or field trial has turned that off for the whole browser (ThirdPartyStoragePartitioning), so MoltNet ; MoltNet pages: 0
- **PASS** Different sites use different SOCKS usernames (per-site circuits)
  - aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad.onion -> SOCKS usernames ['http://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad.onion', 'https://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad.onion', 'opaque\\x20null\\x20[internally:\\x20(11aa)\\x20derived\\x20from\\x20http://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad.onion]', 'opaque\\x20null\\x20[internally:\\x20(33cc)\\x20anonymous]', 'opaque\\x20null\\x20[internally:\\x20(55ee)\\x20anonymous]']
  - bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbd.onion -> SOCKS usernames ['https://bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbd.onion', 'opaque\\x20null\\x20[internally:\\x20(22bb)\\x20derived\\x20from\\x20https://bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbd.onion]']
- **PASS** A proxy policy set while MoltNet is open closes its windows, nothing leaves outside Tor meanwhile, and MoltNet refuses to open until the policy is gone
  - policy {"ProxyMode": "direct"} written to the managed-policy directory
  - MoltNet's 1 page(s) closed 6.5 s after the policy was written (Chromium reads a changed policy file about 5 s later)
  - openWindow with the policy -> {"error": "A policy or command-line flag controls the proxy, so MoltNet can't guarantee Tor routing"}; MoltNet pages: 0
  - the page's canary went through Tor 0 time(s) before the policy and 0 time(s) after it
  - no lookup or packet of the canary outside Tor
  - openWindow once the policy was removed -> {"running": true}; a MoltNet page opened: True
- **PASS** The panel's relays are in the consensus Tor downloaded
  - guard g1 192.0.2.50 AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA in consensus: True
  - middle m1 192.0.2.60 BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB in consensus: True
  - exit e1 192.0.2.70 CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC in consensus: True
- **PASS** Sandboxed pages (opaque origins) on two sites use different SOCKS usernames
  - sandboxed_a -> SOCKS usernames ['opaque\\x20null\\x20[internally:\\x20(11aa)\\x20derived\\x20from\\x20http://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad.onion]']
  - sandboxed_b -> SOCKS usernames ['opaque\\x20null\\x20[internally:\\x20(22bb)\\x20derived\\x20from\\x20https://bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbd.onion]']
- **PASS** Separate identity: cookies don't cross, and go with the last window
  - a cookie set for the onion in the regular profile is not visible in MoltNet
  - after the last MoltNet window closed, its cookie is gone
  - canvas readback on two sites: different
- **PASS** A site visited again in a new MoltNet session (after the last window closed, after New Identity) uses new SOCKS credentials, so new circuits
  - identity, session 0 -> 1: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad.onion, 1 and 1 set(s) of credentials, none shared
  - new_identity, session 0 -> 1: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad.onion, 1 and 1 set(s) of credentials, none shared
  - new_identity, session 1 -> 2: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad.onion, 1 and 1 set(s) of credentials, none shared
- **PASS** Tor runs with the shared capacity MoltNet is sized for (ConnLimit, MaxClientCircuitsPending)
  - ConnLimit 1000 (expected 1000)
  - MaxClientCircuitsPending 256 (expected 256)
- **SKIP** A Background Fetch goes to Tor with its site's SOCKS username (its site's circuit, not its destination's)
  - no Background Fetch request reached Tor (see the bgfetch tests' page results)
- **SKIP** check.torproject.org says the MoltNet window uses Tor
  - --offline

## uniformity

| Status | Check | Reference |
|---|---|---|
| FAIL | fonts.visible is uniform | FINGERPRINTING.md "Fonts" |
| FAIL | nav.hardwareConcurrency is uniform | FINGERPRINTING.md "Surface covered" |
| FAIL | win.innerWidth is uniform | FINGERPRINTING.md "Letterboxing (MoltNet)" |
| PASS | ai.LanguageModel is uniform | FINGERPRINTING.md "Built-in AI APIs" |
| PASS | ai.Rewriter is uniform | FINGERPRINTING.md "Built-in AI APIs" |
| PASS | ai.Summarizer is uniform | FINGERPRINTING.md "Built-in AI APIs" |
| PASS | ai.Translator is uniform | FINGERPRINTING.md "Built-in AI APIs" |
| PASS | ai.Writer is uniform | FINGERPRINTING.md "Built-in AI APIs" |
| PASS | audio.base_latency is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | audio.cadence_burst_frames is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | audio.cadence_interval_ms is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | audio.channel_count is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | audio.channel_count_3 is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | audio.max_channel_count is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | audio.output_latency is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | audio.output_timestamp_ok is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | audio.playback_latency_max is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | audio.playback_underruns is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | audio.render_quantum_hardware is uniform | FINGERPRINTING.md "Re-checked at Chromium 156" |
| PASS | audio.sample_rate is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | audio.script_processor_buffer is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | autoplay.mediaelement is uniform | MOLTNET_COMPLIANCE.md §1.11 |
| PASS | cookies.third_party is uniform | MOLTNET_COMPLIANCE.md §1.11 |
| PASS | cpu.wasm_relaxed_simd is uniform | FINGERPRINTING.md "MoltNet: CPU features" |
| PASS | css.default_font_size is uniform | FINGERPRINTING.md "MoltNet: settings from the regular profile" |
| PASS | doc.characterSet_undeclared is uniform | FINGERPRINTING.md "MoltNet: the browser's UI language" |
| PASS | eme.clearkey is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | eme.playready is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | eme.widevine is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | fonts.generic_map is uniform | FINGERPRINTING.md "Fonts" |
| PASS | fonts.local_hidden is uniform | FINGERPRINTING.md "Fonts" |
| PASS | fonts.moltnet_set is uniform | FINGERPRINTING.md "Fonts" |
| PASS | form.numberMessage is uniform | FINGERPRINTING.md "MoltNet: the browser's UI language" |
| PASS | form.validationMessage is uniform | FINGERPRINTING.md "MoltNet: the browser's UI language" |
| PASS | hdr.accept-language is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | hdr.client_hints is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | hdr.dnt is uniform | FINGERPRINTING.md "MoltNet: settings from the regular profile" |
| PASS | hdr.sec-gpc is uniform | MOLTNET_COMPLIANCE.md §6 |
| PASS | intl.locale is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | intl.number is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | js.enabled_on_blocked_site is uniform | MOLTNET_COMPLIANCE.md §4 step 15 |
| PASS | mc.av1_2160p60 is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | mc.encrypted is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | mc.vp8_360p is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | mc.vp9_1080p is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | mc.webrtc_decoding is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | mc.webrtc_encoding is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | media.audiooutput is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | media.clock_running is uniform | FINGERPRINTING.md "MoltNet: media playback clock" |
| PASS | media.device_blank is uniform | FINGERPRINTING.md "MoltNet: cameras, microphones and outputs" |
| PASS | media.device_order is uniform | FINGERPRINTING.md "MoltNet: cameras, microphones and outputs" |
| PASS | media.devices is uniform | FINGERPRINTING.md "MoltNet: cameras, microphones and outputs" |
| PASS | media.gum is uniform | FINGERPRINTING.md "MoltNet: cameras, microphones and outputs" |
| PASS | media.gum_overconstrained is uniform | FINGERPRINTING.md "MoltNet: cameras, microphones and outputs" |
| PASS | media.sink_default is uniform | FINGERPRINTING.md "MoltNet: media playback clock" |
| PASS | mouse.screen_equals_client is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | mq.any_hover_none is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | mq.any_pointer_coarse is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | mq.color_bits is uniform | FINGERPRINTING.md "Re-checked at Chromium 156" |
| PASS | mq.color_gamut_p3 is uniform | FINGERPRINTING.md "Re-checked at Chromium 156" |
| PASS | mq.color_gamut_rec2020 is uniform | FINGERPRINTING.md "Re-checked at Chromium 156" |
| PASS | mq.color_gamut_srgb is uniform | FINGERPRINTING.md "Re-checked at Chromium 156" |
| PASS | mq.device_size_matches is uniform | FINGERPRINTING.md "Letterboxing (MoltNet)" |
| PASS | mq.dynamic_range_high is uniform | FINGERPRINTING.md "Re-checked at Chromium 156" |
| PASS | mq.hover_hover is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | mq.monochrome is uniform | FINGERPRINTING.md "Re-checked at Chromium 156" |
| PASS | mq.pointer_coarse is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | mq.pointer_fine is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | mq.resolution_1dppx is uniform | FINGERPRINTING.md "Device pixel ratio" |
| PASS | mq.video_dynamic_range_high is uniform | FINGERPRINTING.md "Re-checked at Chromium 156" |
| PASS | mq.viewport_exact is uniform | FINGERPRINTING.md "Letterboxing (MoltNet)" |
| PASS | nav.connection is uniform | FINGERPRINTING.md "MoltNet: connection and network quality" |
| PASS | nav.connection_changes is uniform | FINGERPRINTING.md "MoltNet: connection and network quality" |
| PASS | nav.cpuPerformance is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | nav.deviceMemory is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | nav.doNotTrack is uniform | FINGERPRINTING.md "MoltNet: settings from the regular profile" |
| PASS | nav.globalPrivacyControl is uniform | MOLTNET_COMPLIANCE.md §6 |
| PASS | nav.language is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | nav.languages is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | nav.maxTouchPoints is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | perf.heap_cached is uniform | FINGERPRINTING.md "MoltNet: performance.memory" |
| PASS | perf.heap_rounded is uniform | FINGERPRINTING.md "MoltNet: performance.memory" |
| PASS | perf.jsHeapSizeLimit is uniform | FINGERPRINTING.md "MoltNet: performance.memory" |
| PASS | perm.geolocation is uniform | FINGERPRINTING.md "Known gaps": a documented giveaway |
| PASS | perm.window-management is uniform | FINGERPRINTING.md "MoltNet: touches, screens, orientation and form controls" |
| PASS | pixels.video_frame_format is uniform | FINGERPRINTING.md "MoltNet: pixels and encoded bytes" |
| PASS | recorder.mime_type is uniform | FINGERPRINTING.md "MoltNet: MediaRecorder" |
| PASS | screen.availHeight is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | screen.availLeft is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | screen.availTop is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | screen.availWidth is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | screen.colorDepth is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | screen.height is uniform | FINGERPRINTING.md "Letterboxing (MoltNet)" |
| PASS | screen.isExtended is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | screen.orientation.angle is uniform | FINGERPRINTING.md "MoltNet: touches, screens, orientation and form controls" |
| PASS | screen.orientation.type is uniform | FINGERPRINTING.md "MoltNet: touches, screens, orientation and form controls" |
| PASS | screen.pixelDepth is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | screen.width is uniform | FINGERPRINTING.md "Letterboxing (MoltNet)" |
| PASS | screenDetails.error is uniform | FINGERPRINTING.md "MoltNet: touches, screens, orientation and form controls" |
| PASS | scrollbar.box is uniform | FINGERPRINTING.md "Letterboxing (MoltNet)" |
| PASS | scrollbar.overlay is uniform | FINGERPRINTING.md "Re-checked at Chromium 156" |
| PASS | sharedStorage.set is uniform | MOLTNET_COMPLIANCE.md §1.11 |
| PASS | storage.enforced_quota is uniform | FINGERPRINTING.md "MoltNet: storage limits" |
| PASS | storage.persist is uniform | MOLTNET_COMPLIANCE.md §1.11 |
| PASS | storage.quota_over_usage is uniform | FINGERPRINTING.md "Re-checked at Chromium 156" |
| PASS | text.metrics_whole is uniform | FINGERPRINTING.md "Text metrics (MoltNet)" |
| PASS | touch.createEvent is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | touch.ontouchstart is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | tz.date_string_utc is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | tz.offset_jan is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | tz.offset_jul is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | tz.resolved is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | ua.reduced is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | uad.high_entropy is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | webcodecs.decoder_hardware is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | webcodecs.encoder_hardware is uniform | FINGERPRINTING.md "MoltNet: audio, DRM and media capabilities" |
| PASS | webgl.debug_renderer_info is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| PASS | webgl.extensions is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| PASS | webgl.p.ALIASED_POINT_SIZE_RANGE is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| PASS | webgl.p.MAX_TEXTURE_SIZE is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| PASS | webgl.p.MAX_VARYING_VECTORS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| PASS | webgl.p.MAX_VIEWPORT_DIMS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| PASS | webgl.precision.FRAGMENT_SHADER.HIGH_FLOAT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| PASS | webgpu.adapter is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| PASS | webrtc.error is uniform | MOLTNET_COMPLIANCE.md §1.12 |
| PASS | win.devicePixelRatio is uniform | FINGERPRINTING.md "Device pixel ratio" |
| PASS | win.innerHeight is uniform | FINGERPRINTING.md "Letterboxing (MoltNet)" |
| PASS | win.outerHeight is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | win.outerWidth is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | win.screenX is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | win.screenY is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | worker.connection is uniform | FINGERPRINTING.md "MoltNet: connection and network quality" |
| SKIP | webgl.p.ALIASED_LINE_WIDTH_RANGE is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.p.MAX_COMBINED_TEXTURE_IMAGE_UNITS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.p.MAX_CUBE_MAP_TEXTURE_SIZE is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.p.MAX_FRAGMENT_UNIFORM_VECTORS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.p.MAX_RENDERBUFFER_SIZE is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.p.MAX_TEXTURE_IMAGE_UNITS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.p.MAX_TEXTURE_MAX_ANISOTROPY_EXT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.p.MAX_VERTEX_ATTRIBS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.p.MAX_VERTEX_TEXTURE_IMAGE_UNITS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.p.MAX_VERTEX_UNIFORM_VECTORS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.p.SUBPIXEL_BITS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.precision.FRAGMENT_SHADER.HIGH_INT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.precision.FRAGMENT_SHADER.LOW_FLOAT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.precision.FRAGMENT_SHADER.LOW_INT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.precision.FRAGMENT_SHADER.MEDIUM_FLOAT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.precision.FRAGMENT_SHADER.MEDIUM_INT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.precision.VERTEX_SHADER.HIGH_FLOAT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.precision.VERTEX_SHADER.HIGH_INT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.precision.VERTEX_SHADER.LOW_FLOAT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.precision.VERTEX_SHADER.LOW_INT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.precision.VERTEX_SHADER.MEDIUM_FLOAT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl.precision.VERTEX_SHADER.MEDIUM_INT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.debug_renderer_info is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.extensions is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.ALIASED_LINE_WIDTH_RANGE is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.ALIASED_POINT_SIZE_RANGE is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_3D_TEXTURE_SIZE is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_ARRAY_TEXTURE_LAYERS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_COLOR_ATTACHMENTS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_COMBINED_TEXTURE_IMAGE_UNITS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_COMBINED_UNIFORM_BLOCKS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_CUBE_MAP_TEXTURE_SIZE is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_DRAW_BUFFERS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_ELEMENTS_INDICES is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_ELEMENTS_VERTICES is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_ELEMENT_INDEX is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_FRAGMENT_INPUT_COMPONENTS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_FRAGMENT_UNIFORM_BLOCKS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_FRAGMENT_UNIFORM_COMPONENTS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_FRAGMENT_UNIFORM_VECTORS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_PROGRAM_TEXEL_OFFSET is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_RENDERBUFFER_SIZE is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_SAMPLES is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_SERVER_WAIT_TIMEOUT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_TEXTURE_IMAGE_UNITS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_TEXTURE_LOD_BIAS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_TEXTURE_MAX_ANISOTROPY_EXT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_TEXTURE_SIZE is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_UNIFORM_BLOCK_SIZE is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_UNIFORM_BUFFER_BINDINGS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_VARYING_COMPONENTS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_VARYING_VECTORS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_VERTEX_ATTRIBS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_VERTEX_OUTPUT_COMPONENTS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_VERTEX_TEXTURE_IMAGE_UNITS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_VERTEX_UNIFORM_BLOCKS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_VERTEX_UNIFORM_COMPONENTS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_VERTEX_UNIFORM_VECTORS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MAX_VIEWPORT_DIMS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.MIN_PROGRAM_TEXEL_OFFSET is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.SUBPIXEL_BITS is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.p.UNIFORM_BUFFER_OFFSET_ALIGNMENT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.precision.FRAGMENT_SHADER.HIGH_FLOAT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.precision.FRAGMENT_SHADER.HIGH_INT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.precision.FRAGMENT_SHADER.LOW_FLOAT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.precision.FRAGMENT_SHADER.LOW_INT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.precision.FRAGMENT_SHADER.MEDIUM_FLOAT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.precision.FRAGMENT_SHADER.MEDIUM_INT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.precision.VERTEX_SHADER.HIGH_FLOAT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.precision.VERTEX_SHADER.HIGH_INT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.precision.VERTEX_SHADER.LOW_FLOAT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.precision.VERTEX_SHADER.LOW_INT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.precision.VERTEX_SHADER.MEDIUM_FLOAT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.precision.VERTEX_SHADER.MEDIUM_INT is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | webgl2.samples_rgba8 is uniform | FINGERPRINTING.md "WebGL and WebGPU in MoltNet windows" |
| SKIP | Zoom level 125% could be set | docs/LEAK_TESTING.md "Zoom" |
| INFO | Regular-window control: values that are already uniform outside MoltNet on this machine (their MoltNet check proves less here) |  |

- **FAIL** fonts.visible is uniform
  - initial\|dsf=1.0\|window=initial\|zoom=100\|moltnet: actual ["DejaVu Sans", "Liberation Sans", "Arial", "Ubuntu"], expected a subset of the 55 allowed (not on the list: DejaVu Sans, Ubuntu)
- **FAIL** nav.hardwareConcurrency is uniform
  - initial\|dsf=1.0\|window=initial\|zoom=100\|moltnet: actual 8, expected 4
- **FAIL** win.innerWidth is uniform
  - window\|dsf=1.0\|window=1280x800\|zoom=100\|moltnet: actual 1401, expected a multiple of 200, at most 1400
- **INFO** Regular-window control: values that are already uniform outside MoltNet on this machine (their MoltNet check proves less here)
  - mq.pointer_fine

## background

| Status | Check | Reference |
|---|---|---|
| FAIL | No host NETWORK_TRAFFIC.md says must not appear | NETWORK_TRAFFIC.md "Pass criteria" |
| INFO | Background lookups outside Tor (expected, MOLTNET_COMPLIANCE.md §5.6; DNS level only) | NETWORK_TRAFFIC.md |

- **FAIL** No host NETWORK_TRAFFIC.md says must not appear
  - dns accounts.google.com A x1 at +4.0s: must not appear: no Google sign-in [firewall log]
- **INFO** Background lookups outside Tor (expected, MOLTNET_COMPLIANCE.md §5.6; DNS level only)
  - dns update.googleapis.com A x1 at +3.0s: component updater (NETWORK_TRAFFIC.md §1) [firewall log, kit resolver] during phase:circuit

## Tor's connections

- tcp 192.0.2.50:9001 (1)
- tcp 192.0.2.51:443 (1)

## Still manual (MOLTNET_COMPLIANCE.md §4)

| Step | | Kit | Note |
|---|---|---|---|
| 1 | You are on Tor | partial | check.torproject.org through Tor and the panel's Verify (needs an exit to reach the public internet); the onion tests prove Tor routing offline |
| 2 | Regular windows are untouched | partial | a regular window reaches the kit's local page directly, not through Tor; the toolbar label stays manual |
| 3 | No IP, DNS, WebRTC or IPv6 leak (leak sites) | replaced | the leak suite tests the same mechanisms locally; the public sites stay an optional manual cross-check |
| 4 | Watch the wire | automated | every packet is captured in a namespace where only Tor may leave |
| 5 | Fail-closed | automated | tor is killed: pages must fail with a proxy error; after a restart no MoltNet window returns |
| 6 | Separate identity | automated | a regular-profile cookie doesn't show in MoltNet; a MoltNet cookie is gone after the last window closes |
| 7 | New Identity | automated | new profile, cookies gone, new canvas key, no old page left open, a second New Identity waits for Tor to switch circuits, nothing the browser held for the old page (fetchLater, a pagehide beacon) sent; the exit IP check needs the public internet |
| 8 | Per-site circuits | automated | different SOCKS usernames per site (sandboxed pages included), a SOCKS username of its own for each certificate fetch, its site's for a Background Fetch, read from the loopback capture and Tor's circuit list |
| 9 | Overrides are refused | partial | --proxy-server and each partitioning switch block MoltNet; a proxy policy set while MoltNet is open closes its windows; the extension-set proxy case is manual |
| 10 | The relays are real | automated | the circuit's fingerprints are in the consensus Tor downloaded; the onionoo lookup is optional |
| 11 | Onion sites | partial | http:// and https:// onions load; the Onion-Location bar is browser UI and stays manual |
| 12 | No onion names outside Tor (regular window) | automated | the interstitial shows and no onion name is looked up; DoH is covered because every lookup lands on the kit's resolver |
| 13 | No onion names to a proxy (regular window) | partial | a logging proxy must see no onion request (image, fetch, redirect, WebSocket, dedicated and shared worker); the service worker case is manual |
| 14 | Bridges with the distribution's transports | manual |  |
| 15 | Your settings don't follow you | automated | seeded regular-profile settings (Do Not Track, fonts, language, third-party cookies, a blocked JavaScript site) must not show in MoltNet |
| 16 | Extensions stay out | partial | a test extension allowed in Incognito must not see or mark MoltNet pages; split mode, shortcuts and the toolbar stay manual |
| 17 | No regular-profile carry-over | partial | autoplay policy, persist(), third-party cookies and Shared Storage are read in MoltNet; media engagement, bookmarks and certificate exceptions need a prepared regular profile |
| 18 | No WebRTC, and no name lookups for it | automated |  |
| 19 | Incognito's protections | manual | DevTools prefs, media controls, macOS, picture-in-picture, GCM, printing |
| 20 | Nothing goes around Tor | partial | downloads, geolocation, the built-in AI and Autofill's server queries are automated; the AI chat, Cast menu, phone passkeys, "Open all" bookmarks and address formats stay manual |
