# MoltNet leak test report

**Verdict: FAIL**  (12 FAIL, 0 ERROR, 97 PASS, 84 SKIP, 2 INFO)

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
  - all: 18 frames, 18 IP, 0 malformed
  - firewall log: 12 frames
  - Tor connections: 2 frames
  - loopback packets: 6
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
| PASS | Built-in AI APIs and Translator (no component download): LanguageModel is unavailable | MOLTNET_COMPLIANCE.md §1.14; FINGERPRINTING.md "Built-in AI APIs" |
| PASS | Built-in AI APIs and Translator (no component download): Summarizer is unavailable | MOLTNET_COMPLIANCE.md §1.14; FINGERPRINTING.md "Built-in AI APIs" |
| PASS | Built-in AI APIs and Translator (no component download): Writer is unavailable | MOLTNET_COMPLIANCE.md §1.14; FINGERPRINTING.md "Built-in AI APIs" |
| PASS | Built-in AI APIs and Translator (no component download): Rewriter is unavailable | MOLTNET_COMPLIANCE.md §1.14; FINGERPRINTING.md "Built-in AI APIs" |
| PASS | Built-in AI APIs and Translator (no component download): Translator is unavailable | MOLTNET_COMPLIANCE.md §1.14; FINGERPRINTING.md "Built-in AI APIs" |
| PASS | DNS prefetch, preconnect, speculation rules, hover: nothing outside Tor | MOLTNET_COMPLIANCE.md §2 (other ways to look up a name) |
| PASS | localhost, loopback, the machine's own and LAN addresses: every local/LAN fetch fails (Tor refuses it) | MOLTNET_COMPLIANCE.md §2 (<-loopback>), §5.7 |
| PASS | A MoltNet window loads an http:// onion site: the page loaded (no HTTPS-Only interstitial for http:// onion) | MOLTNET_COMPLIANCE.md §4 steps 1, 11 |
| PASS | A MoltNet window loads an http:// onion site: http:// onion is not a secure context (known gap §5.7) | MOLTNET_COMPLIANCE.md §4 steps 1, 11 |
| PASS | A MoltNet window loads an http:// onion site: nothing outside Tor | MOLTNET_COMPLIANCE.md §4 steps 1, 11 |
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
| PASS | Fail-closed: with Tor killed, pages fail with a proxy error | MOLTNET_COMPLIANCE.md §4 step 5 |
| PASS | New Identity: new profile, cookies gone, new farbling seed | MOLTNET_COMPLIANCE.md §4 step 7 |
| PASS | No MoltNet window comes back after a restart | MOLTNET_COMPLIANCE.md §4 step 5 |
| PASS | A --proxy-server override blocks MoltNet | MOLTNET_COMPLIANCE.md §4 step 9 |
| PASS | Different sites use different SOCKS usernames (per-site circuits) | MOLTNET_COMPLIANCE.md §1.4, §4 step 8 |
| PASS | The panel's relays are in the consensus Tor downloaded | MOLTNET_COMPLIANCE.md §4 step 10 |
| PASS | Separate identity: cookies don't cross, and go with the last window | MOLTNET_COMPLIANCE.md §4 step 6 |
| SKIP | check.torproject.org says the MoltNet window uses Tor | MOLTNET_COMPLIANCE.md §4 step 1 |

- **PASS** Fail-closed: with Tor killed, pages fail with a proxy error
  - tor killed: [4242]
  - navigation after the kill: net::ERR_SOCKS_CONNECTION_FAILED
- **PASS** New Identity: new profile, cookies gone, new farbling seed
  - newIdentity -> {"running": true}
  - cookie gone after New Identity
  - canvas readback changed (new farbling seed)
- **PASS** No MoltNet window comes back after a restart
  - 0 MoltNet page(s) after relaunching with the same profile
- **PASS** A --proxy-server override blocks MoltNet
  - launched with --proxy-server=http://127.0.0.1:18888
  - openWindow -> {"error": "A command-line flag controls the proxy."}
  - MoltNet pages: 0
- **PASS** Different sites use different SOCKS usernames (per-site circuits)
  - aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad.onion -> SOCKS usernames ['http://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad.onion']
  - bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbd.onion -> SOCKS usernames ['https://bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbd.onion']
- **PASS** The panel's relays are in the consensus Tor downloaded
  - guard g1 192.0.2.50 AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA in consensus: True
  - middle m1 192.0.2.60 BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB in consensus: True
  - exit e1 192.0.2.70 CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC in consensus: True
- **PASS** Separate identity: cookies don't cross, and go with the last window
  - a cookie set for the onion in the regular profile is not visible in MoltNet
  - after the last MoltNet window closed, its cookie is gone
  - canvas readback on two sites: different
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
| PASS | autoplay.mediaelement is uniform | MOLTNET_COMPLIANCE.md §1.11 |
| PASS | cookies.third_party is uniform | MOLTNET_COMPLIANCE.md §1.11 |
| PASS | css.default_font_size is uniform | FINGERPRINTING.md "MoltNet: settings from the regular profile" |
| PASS | doc.characterSet_undeclared is uniform | FINGERPRINTING.md "MoltNet: the browser's UI language" |
| PASS | form.numberMessage is uniform | FINGERPRINTING.md "MoltNet: the browser's UI language" |
| PASS | form.validationMessage is uniform | FINGERPRINTING.md "MoltNet: the browser's UI language" |
| PASS | hdr.accept-language is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | hdr.client_hints is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | hdr.dnt is uniform | FINGERPRINTING.md "MoltNet: settings from the regular profile" |
| PASS | hdr.sec-gpc is uniform | MOLTNET_COMPLIANCE.md §6 |
| PASS | intl.locale is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | intl.number is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | js.enabled_on_blocked_site is uniform | MOLTNET_COMPLIANCE.md §4 step 15 |
| PASS | mouse.screen_equals_client is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | mq.any_hover_none is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | mq.any_pointer_coarse is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | mq.device_size_matches is uniform | FINGERPRINTING.md "Letterboxing (MoltNet)" |
| PASS | mq.hover_hover is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | mq.pointer_coarse is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | mq.pointer_fine is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | mq.resolution_1dppx is uniform | FINGERPRINTING.md "Device pixel ratio" |
| PASS | nav.cpuPerformance is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | nav.deviceMemory is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | nav.doNotTrack is uniform | FINGERPRINTING.md "MoltNet: settings from the regular profile" |
| PASS | nav.globalPrivacyControl is uniform | MOLTNET_COMPLIANCE.md §6 |
| PASS | nav.language is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | nav.languages is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | nav.maxTouchPoints is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | perm.geolocation is uniform | FINGERPRINTING.md "Known gaps": a documented giveaway |
| PASS | perm.window-management is uniform | FINGERPRINTING.md "MoltNet: touches, screens, orientation and form controls" |
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
| PASS | sharedStorage.set is uniform | MOLTNET_COMPLIANCE.md §1.11 |
| PASS | storage.persist is uniform | MOLTNET_COMPLIANCE.md §1.11 |
| PASS | touch.createEvent is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | touch.ontouchstart is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | tz.date_string_utc is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | tz.offset_jan is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | tz.offset_jul is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | tz.resolved is uniform | FINGERPRINTING.md "Surface covered" |
| PASS | ua.reduced is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
| PASS | uad.high_entropy is uniform | FINGERPRINTING.md "MoltNet: no touchscreen, client hints, memory and CPU" |
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
  - initial\|dsf=1.0\|window=initial\|zoom=100\|moltnet: actual ["DejaVu Sans", "Liberation Sans", "Arial", "Ubuntu"], expected a subset of the 19 allowed (not on the list: Ubuntu)
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
| 7 | New Identity | automated | new profile, cookies gone, new canvas key; the exit IP check needs the public internet |
| 8 | Per-site circuits | automated | different SOCKS usernames per site, read from the loopback capture and Tor's circuit list |
| 9 | Overrides are refused | partial | --proxy-server blocks MoltNet; the extension-set proxy case is manual |
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
| 20 | Nothing goes around Tor | partial | downloads, geolocation and the built-in AI are automated; the AI chat, Cast menu, phone passkeys and "Open all" bookmarks stay manual |
