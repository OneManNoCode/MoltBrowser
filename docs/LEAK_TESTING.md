# Leak testing MoltNet on a built browser

`tools/leak_test/` turns most of the manual checks in
[MOLTNET_COMPLIANCE.md §4](MOLTNET_COMPLIANCE.md#4-check-it-yourself-do-this-on-every-release-build)
and the "Verification" sections of [FINGERPRINTING.md](FINGERPRINTING.md)
into automated pass/fail checks. Run it on the build machine (Linux) against
a built MoltBrowser. One command produces `report.md` (for people) and
`report.json` (send it back with the build's version).

It doesn't replace reading the code. It checks that what the code is meant
to do is what the built browser does.

**Status.** The kit has not yet been run against a MoltBrowser build. Its
analysis is tested against a recorded sample run and against real captures
(`tools/leak_test/tests/`). The harness itself (namespace, firewall,
captures, the kit's resolver and canaries, driving the browser over the
DevTools pipe, every test page, the report) was run end to end against
stock Chromium 141, as a regular window, once on its own and once behind a
real tor in the namespace's Tor cgroup. As it should, it found the leaks
MoltNet closes and stock Chromium doesn't: WebRTC STUN/TURN to an IP
literal, the TURN and STUN names and `.local` names looked up, the mDNS
group joined, DNS prefetch, an onion name looked up from a regular window,
and LAN and loopback fetches. Behind the proxy it showed every other
canary name going to tor inside SOCKS5. The MoltNet-only parts (opening
the window through `chrome://molt-net/`, the identity phases, the onion
services) need MoltBrowser and access to the Tor network, so the first run
on the build machine is also their first test.

## Run it

```bash
sudo tools/leak_test/leaktest.sh run --browser /path/to/moltbrowser
#   ... about 10-20 minutes ...
#   verdict: PASS  (0 FAIL, 0 ERROR, ...)
#   report: ./moltleak-runs/20261022-153000-a1b2c3/report.md
```

`--browser` is the executable: `chromium/src/out/MoltBrowser-linux/chrome` in a build tree, or
`/opt/moltbrowser/moltbrowser` from the `.deb`. Run it with `sudo` from your
own account: the kit needs root for the network namespace, the firewall and
the capture, but the browser, Tor and every server it starts run as you
(`$SUDO_USER`, or `--user NAME`). The browser never runs as root.

Useful options (`leaktest.sh run --help` lists them all):

| Option | Default | What for |
|---|---|---|
| `--target moltnet\|regular` | `moltnet` | `regular` tests a plain window: a check of the kit itself, works with any Chromium |
| `--display auto\|xvfb\|current\|headless` | `auto` (Xvfb if installed) | where the windows go; Xvfb gives a known screen size |
| `--screen 1920x1080` | | the Xvfb screen |
| `--window-sizes`, `--zooms`, `--dsf` | 5 sizes; 50,90,125,200 %; 1,2 | the uniformity matrix (each scale factor is one browser launch) |
| `--locale de_DE.UTF-8`, `--timezone Asia/Tokyo` | the caller's | run under another OS locale and time zone, then `diff` |
| `--tests`, `--phases` | all | run a subset |
| `--offline` | | skip the two checks that need a Tor exit to reach the public internet |
| `--tor /path/to/tor` | bundled, else `/usr/bin/tor` | the tor MoltNet runs |
| `--browser-arg FLAG` | | an extra browser flag (repeatable) |

Rebuild a report from a recorded run (no root; for example after updating
the background-host list): `tools/leak_test/leaktest.sh report <run dir>`.

Compare two runs, for example the same build on two machines, or one machine
with two locales:

```bash
sudo tools/leak_test/leaktest.sh run --browser ... --out /tmp/a
sudo tools/leak_test/leaktest.sh run --browser ... --out /tmp/b \
     --locale de_DE.UTF-8 --timezone Asia/Tokyo
tools/leak_test/leaktest.sh diff /tmp/a/*/report.json /tmp/b/*/report.json
```

`diff` compares the MoltNet fingerprints recorded under the same label,
scale factor, window size and zoom. A difference in a promised value fails.
A difference in a value nothing promises (`[info]`, for example the storage
quota or the number of speech voices) is listed for you to look at. Farbled
values (canvas, audio) are not compared, because they change every session
by design.

### What it needs

Linux 5.13 or later with cgroup v2 (every current distribution; on a
hybrid v1/v2 system it uses `/sys/fs/cgroup/unified`), nftables 1.0 or
later (for `socket cgroupv2`; Ubuntu 22.04, Debian 12, Fedora 36 and
later), Python 3.8 or later (standard library only), and these packages:

| Tool | Package (Debian/Ubuntu) | For |
|---|---|---|
| `ip`, `nsenter`, `unshare`, `setpriv` | `iproute2`, `util-linux` | the namespace and dropping to your user |
| `nft` | `nftables` | the firewall (kernel `nft_socket` with cgroup v2 support, Linux 5.x) |
| `tcpdump` (with `nflog` support) | `tcpdump` | the captures |
| `tor` | `tor` | the kit's own onion services; also MoltNet's Tor on Linux |
| `openssl` | `openssl` | test certificates |
| `Xvfb` (optional) | `xvfb` | a display of known size |
| `iptables` (only if present) | | Docker and ufw set the `FORWARD` policy to DROP; the kit adds two rules for its link and removes them |

`tshark` is not needed: the kit reads the captures itself. If firewalld
manages forwarding, put the kit's link in the trusted zone for the run, or
Tor can't connect and MoltNet won't open (the report says so).

### What depends on the public internet

Only the Tor network. The test pages are served by the kit's own Tor as two
onion services, so a MoltNet window reaches them through Tor and back without
any exit relay, DNS or other server. Two optional checks use an exit to
reach `check.torproject.org` (step 1 and the exit IP before and after New
Identity); `--offline` skips them. No test page loads anything from the
public internet. Canary names use the reserved `.test` domain, so a lookup
that does reach the internet through an exit (as it should, inside Tor)
asks nobody in particular.

## How it works

```
 host                                     moltleak-<id> network namespace
 ----                                     -------------------------------
 kit's tor (2 onion services) <--Tor-->   MoltBrowser (cgroup "browser")
   -> page server on 127.0.0.1               | SOCKS5 on loopback
                                             v
 masquerade <== veth ==================   MoltNet's tor (cgroup "tor")
                                          kit resolver 127.0.0.1:53, loopback
                                          canary, local page server, Xvfb
                                          (cgroup "kit")
                                          nftables: only cgroup "tor" may
                                          leave; everything else is logged
                                          (nflog) and dropped
                                          tcpdump: any, nflog:1, nflog:2
```

* **Only Tor may leave.** The browser runs in its own network namespace.
  Its firewall lets out only sockets that belong to the run's `tor` cgroup
  (nftables `socket cgroupv2`). MoltBrowser starts Tor itself, as you, so
  the kit tells Tor apart by cgroup, not by user or address. The kit passes
  `--moltnet-tor-binary=` a root-owned wrapper, in a root-owned directory
  under `/run`. The wrapper moves itself into the `tor` cgroup and runs the
  real tor (the bundled one, or the distribution's) with the same
  arguments. Tor's pluggable transports are its children, so they stay in
  that cgroup. The switch already exists for exactly this (a non-bundled
  tor, with the same ownership checks), so the kit adds nothing to the
  product. Tor's traffic is NATed out through the host.
* **Every name lookup is recorded.** The browser gets a private mount
  namespace where `/etc/resolv.conf` is `nameserver 127.0.0.1` and
  `/etc/nsswitch.conf` says `hosts: files dns`. Chromium's own resolver and
  `getaddrinfo()` both send every lookup to the kit's resolver inside the
  namespace, which logs the name and answers NXDOMAIN. Without this, glibc
  could hand a lookup to systemd-resolved or avahi over a Unix socket, and
  those would ask from outside the namespace, where no capture sees it. DNS,
  DoT, mDNS and LLMNR packets to anywhere else are logged and dropped.
* **Three captures.** `capture/all.pcap` is everything on every interface
  of the namespace, loopback included. It shows the browser's SOCKS5
  requests to Tor: the host name and the per-site username of every
  connection. `capture/blocked.pcap` (nflog group 1) is every packet the
  firewall logged: drops, lookups, IGMP/MLD joins, and connections to the
  loopback canary. `capture/tor.pcap` (nflog group 2) is the first packet
  of each connection Tor opened. The analysis cross-checks them: a packet
  that left the namespace without belonging to a Tor connection fails the
  run ("escaped"), whether or not the firewall logged it.
* **Canaries.** Each test gets a random nonce. Its pages use names like
  `stun-<nonce>.moltleak.test` and IP literals from the documentation
  ranges reserved for that test (`198.51.100.N`, `2001:db8::N`). Any lookup
  or packet outside Tor that carries them is a leak of exactly that test,
  even if it shows up minutes later (Reporting-API reports are batched). A
  loopback canary listens on a port of the namespace's loopback and its own
  address, and the namespace's gateway stands in for a LAN neighbour.
* **Driving the browser.** The kit launches the browser with
  `--remote-debugging-pipe` and speaks the DevTools protocol over the two
  inherited pipe descriptors. Unlike `--remote-debugging-port`, a pipe
  opens no port: no other local process and no web page can reach it. It
  opens MoltNet the way the toolbar popover does: it loads
  `chrome://molt-net/` in a regular tab and calls the page's own
  `sendWithPromise('moltnet.openWindow')`. That goes through the whole
  production path (`MoltNetWindow::Open`: Tor bootstrap, profile, routing
  check). It then attaches to the new page target in the MoltNet browser
  context. **No new switch was needed, and none was added.** New Identity,
  the circuit panel and *Verify* are driven the same way.
* **A fresh identity per run.** The profile (`--user-data-dir`) and `$HOME`
  (MoltNet's Tor state lives in `~/.moltbrowser/tor`) are directories of
  the run, so your own profile and Tor state are never touched. The
  browser's environment is emptied except for `HOME`, `USER`, `PATH`,
  `LANG`/`LC_ALL`, `TZ`, the display and `XDG_RUNTIME_DIR`. Proxy variables
  in particular are removed.

Teardown removes the namespace, the link, the host NAT table, the iptables
rules, the cgroups and the wrapper. It also restores `ip_forward`, even
after an error or Ctrl+C.

## The checks

Status: **FAIL**, a promise is broken or traffic needs triage; **ERROR**,
the kit couldn't run the check (the run is INCOMPLETE); **SKIP**, not run or
not applicable; **INFO**, recorded without a verdict.

### Harness (is the run itself valid?)

| Check | Proves |
|---|---|
| The capture recorded traffic | the captures exist and parse |
| Nothing left the namespace except Tor's connections | the firewall held (cross-checked against the full capture) |
| Tor (and only Tor) reached the network | MoltNet's Tor connected; its destinations (guards, directory mirrors) are listed |
| A MoltNet window opened | the production open path worked (the error text if not) |
| The seeded settings took effect | control for step 15: the regular window shows the seeded Do Not Track, font size and blocked JavaScript |
| The test extension loaded and ran | control for step 16: the extension marks a regular page and is allowed in Incognito |

### Network: nothing a MoltNet page does leaves outside Tor

Each test loads a page in the MoltNet window. A test **fails** if any lookup
or packet outside Tor carries its nonce or IP literal, or if anything
leak-shaped (mDNS, multicast, LAN, STUN/TURN, QUIC, the loopback canary, an
onion name) or unexplained happens during its window. A test also fails if
a host it forbids is looked up during its window. Evidence also lists what
went *into* Tor: the SOCKS5 requests carrying the test's names, and the
paths that reached the onion server.

| Test | Page does | Must hold |
|---|---|---|
| `onion_http`, `onion_https` | load from the kit's onion | loads; `http://` onion without an HTTPS-Only interstitial |
| `webrtc` | `RTCPeerConnection` with STUN/TURN by name and IP literal, a peer candidate by name, a `.local` candidate | throws `NotSupportedError` with Chromium's message; no lookup, no UDP, no mDNS (step 18) |
| `dns_prefetch` | `dns-prefetch`, `preconnect` (markup and script), speculation rules, hovering links | nothing looked up (no preconnect service off the record, prediction off) |
| `prefetch` | `prefetch`, `prerender`, `preload`, `modulepreload`, speculation rules | nothing outside Tor; same-origin fetches show whether they happened at all (info) |
| `fonts_favicons` | web fonts, favicon, touch icon, manifest, stylesheet, images, `srcset`, IP-literal images | nothing outside Tor (favicons through MoltNet's context) |
| `beacon_ping` | `sendBeacon`, keepalive `fetch`, a `pagehide` beacon, `<a ping>` | nothing outside Tor |
| `csp_reporting` | CSP `report-uri`/`report-to`, report-only, `Reporting-Endpoints`, `NEL`, COOP report-only | nothing outside Tor, also for reports sent later |
| `websocket` | `ws://`/`wss://` to canary names and an IP literal; one to the onion | nothing outside Tor; the onion one opens |
| `webtransport` | WebTransport to a canary name and an IP literal | fails, nothing looked up, no UDP |
| `workers` | a service worker, a dedicated and a shared worker fetch canaries and open WebSockets | nothing outside Tor |
| `download` | an `.exe` download | no Safe Browsing lookup (`sb-ssl.google.com`, ...) (step 20) |
| `localhost_lan` | `127.0.0.1`, `[::1]`, `localhost`, the machine's own address, the gateway, `192.168.x`, `10.x`, `169.254.169.254` | every fetch fails; the loopback canary sees nothing |
| `quic` | `Alt-Svc: h3` on the onion, fetches again, a canary name | no UDP/QUIC |
| `mdns` | `.local` names | no mDNS, no IGMP/MLD join of the mDNS group |
| `cert_fetch` | an https onion whose certificate lacks its intermediate, with AIA, OCSP and CRL URLs | no lookup of those names outside Tor; whether AIA came through Tor is shown |
| `cert_fetch_b` | the same certificate on the second onion site (so the same AIA URLs) | nothing outside Tor; read with `cert_fetch` by `identity.cert_fetch_circuits` |
| `client_cert` | the basic page from an https onion port (8444) whose server asks for a TLS client certificate without requiring one, and logs every handshake | nothing outside Tor; `net.client_cert.handshakes`: every handshake completes and none carries a certificate, so the browser neither asked the user nor connected again. In a regular window Chromium drops the first handshake to ask and connects again; the check reports that as information (step 20) |
| `builtin_ai` | `availability()` and `create()` (with a user gesture) of every built-in AI API | `LanguageModel`, `Summarizer`, `Writer`, `Rewriter`, `Translator` unavailable; no component or model download host looked up |
| `geolocation` | `getCurrentPosition()` | error 1 at once (no prompt); no location provider host |
| `extension` | a test extension allowed in Incognito (content script, web-accessible resource, `tabs`, `webRequest`) | the MoltNet page isn't marked, `chrome.runtime` is absent, the resource can't be fetched, and its background never sees a MoltNet tab or request (it would look up a canary name if it did) (step 16) |
| `regular_onion_navigation` | a **regular** window opens an onion address | the "Tor onion site" page; no lookup of the name (step 12) |
| `regular_onion_subresources` | a regular page loads images, `fetch`, a WebSocket and a worker fetch from an onion host | all fail; no lookup (step 12); with a proxy configured, the proxy sees none (step 13) |

Two further network checks cover the whole run. Leak-shaped traffic outside
any test fails. So does any lookup or connection outside Tor that no list
explains. **Background traffic** of the regular profile (component updater,
update check, MoltShield lists, ...: [NETWORK_TRAFFIC.md](NETWORK_TRAFFIC.md),
listed in `leakkit/background.py`) is reported, not failed. A host
NETWORK_TRAFFIC.md says must not appear fails. To triage an unexplained name:
if it is new background traffic of the regular profile, add it to
`leakkit/background.py` and NETWORK_TRAFFIC.md with the reason, then rebuild
the report. Anything else is a leak.

### Identity and fail-closed

| Check | How | Step |
|---|---|---|
| Separate identity | a cookie set for the onion in the regular profile (DevTools `Storage.setCookies`) doesn't show in MoltNet; a MoltNet cookie is gone after its last window closed and MoltNet reopened in a new browser context; the onion site's SOCKS5 connections after MoltNet reopened carry credentials the earlier session didn't use, so new circuits (`identity.session_circuits`; the kit keeps a digest of each SOCKS password, never the password) | 6 |
| New Identity | `moltnet.newIdentity`: a new browser context, the cookie gone, the canvas readback changed (new farbling seed); no page of the old identity left open, although it asked "Leave site?" with a user activation; a second New Identity right after the first opens its window only once Tor can have switched circuits again (at least 8 s later; Tor applies NEWNYM at most every 10 s); the same site's SOCKS5 credentials differ before and after each New Identity (`identity.session_circuits`); exit IP before/after if online | 7 |
| Per-site circuits | the two onion sites' SOCKS5 connections carry different usernames, and every one carries one; two pages their servers sandbox (`Content-Security-Policy: sandbox`, opaque origins) on two sites use different usernames too (`identity.sandboxed_circuits`); the certificate fetches made for the two certificate tests use usernames of their own, none shared between the two sites and none a page used, and the second site's check makes a fetch of its own (`identity.cert_fetch_circuits`) | 8 |
| The relays are real | the panel's guard, middle and exit fingerprints (`moltnet.getCircuit`) are in the consensus MoltNet's Tor downloaded | 10 |
| On Tor | the panel's *Verify* and `check.torproject.org/api/ip` in the MoltNet tab (online) | 1 |
| Fail-closed | the `tor` cgroup is killed; the next MoltNet navigation fails with a proxy/SOCKS error and nothing leaves directly | 5 |
| No restore | a relaunch with the same profile shows no MoltNet page | 5 |
| Overrides refused | a relaunch with `--proxy-server=` (the kit's logging proxy): `openWindow` returns an error and no MoltNet window opens; the same launch runs step 13 | 9, 13 |
| Partitioning switches refused | one relaunch per switch MoltNet needs on, each with `--disable-features=` for it (`PartitionConnectionsByNetworkIsolationKey`, `SplitCacheByNetworkIsolationKey`, `ThirdPartyStoragePartitioning`): `openWindow` returns an error that names the switch, and no MoltNet window opens (`identity.partitioning_override_refused`, phase `partitioning_override`) | 9 |

### Uniformity (FINGERPRINTING.md)

`pages/static/fp.js` collects the values in a MoltNet window.
`leakkit/expectations.py` holds what each must be, with the doc section it
comes from. The values are collected once in full, then the geometry again
at every window size (DevTools `Browser.setWindowBounds`), at every zoom
level, and in one browser launch per device scale factor
(`--force-device-scale-factor`):

* **Letterboxing** (every record): `innerWidth`/`innerHeight` a multiple of
  200 x 100, at most 1400 x 900; `screen.*`, `outer*` equal to them;
  `availLeft/Top`, `screenX/Y` 0; `devicePixelRatio` 1 and the `resolution`
  query; `device-width/height` match; colour depth 24; `isExtended` false;
  orientation from the uniform screen, angle 0; mouse `screenX/Y` equal to
  `clientX/Y`; the `width`/`height` media queries and `vw`/`vh` exactly
  `innerWidth` x `innerHeight` (strict comparisons included); a classic
  scrollbar 16 CSS pixels wide (0 with overlay scrollbars; 15 or 17 by
  design at 33% and 67% zoom, which the default zoom list leaves out);
  overlay scrollbars on macOS only, classic ones on Linux and Windows.
* **Once per launch:** UTC (also `Date` strings), en-US (`Intl`, numbers,
  `navigator.language(s)`, `Accept-Language`), 4 cores, 8 GB,
  `cpuPerformance` 0, `navigator.connection` (4g, rtt 200, downlink 1.6,
  no data saver, in the page and in a dedicated worker, and no `change`
  event while the page runs), a mouse-only desktop (touch points,
  pointer/hover queries, touch-event detection), blank high-entropy `userAgentData`, only
  the allowed client hints reaching the server (`Accept-CH` + `Critical-CH`
  asked for all of them), no `WEBGL_debug_renderer_info`, every WebGL 1/2
  limit at its baseline (or below it on a GPU that lacks it; never above),
  ranges, viewport, shader precision formats and sample counts, extensions
  within the allow-list, no WebGPU adapter, installed fonts within the
  OS's allow-list (Linux in the kit; other OSes are compared between runs),
  en-US form messages, `windows-1252` for undeclared pages, 16px default
  font size, no Do Not Track, GPC on, `window-management` denied and
  `getScreenDetails()` rejected, `persist()` false, autoplay
  `allowed-muted`, no Shared Storage (removed upstream at 156; the kit
  also accepts an older build's rejected `set()`), no third-party cookie,
  WebRTC off, the built-in AI APIs unavailable, the reduced user agent,
  an sRGB display with a standard dynamic range and 8 bits per colour (the
  `color-gamut`, `dynamic-range`, `video-dynamic-range`, `color` and
  `monochrome` queries), a storage quota reported as 10 GiB over the usage,
  and `renderQuantumSize` 128 for `renderSizeHint: "hardware"`; one audio
  device (`AudioContext` at 48 kHz, `baseLatency` for every `latencyHint`
  and at a page's 44.1 kHz, `outputLatency` 24 ms, 2 channels at most and
  `channelCount` 3 refused, the default `ScriptProcessorNode` size, the
  output timestamp, `playbackStats`), no key system (Clear Key, Widevine,
  PlayReady, and Media Capabilities with a `keySystemConfiguration`), Media
  Capabilities' fixed answers for VP8, VP9 and AV1 video and for WebRTC,
  and WebCodecs refusing `prefer-hardware`. Codec support (Media
  Capabilities for Opus, H.264 and HEVC, `canPlayType()`,
  `MediaSource.isTypeSupported()`, WebCodecs without a hardware
  preference) follows the build and is compared between runs.
* **Recorded, not promised** (shown when two runs differ): media device
  kinds, storage quota, JS heap limit, speech voices, battery, keyboard
  layout, WebRTC codecs, `system-ui` width, the test page's own scrollbar
  width.

A regular-window fingerprint is recorded as a control. The report lists the
values that are already uniform outside MoltNet on this machine (for
example, a machine with exactly 4 cores), because there the MoltNet check
proves less.

**Settings from the regular profile (step 15).** Before the first launch the
kit seeds the regular profile's `Preferences` with Do Not Track on, a 24px
default font and DejaVu Serif as the standard font, German
`Accept-Language`, third-party cookies allowed, and JavaScript and location
blocked for the https onion (and, as the control, for a local page). The
MoltNet checks above must still read MoltBrowser's defaults, and the
fingerprint page itself runs on the onion where JavaScript is "blocked".

**Zoom.** DevTools has no page-zoom command, so the kit sends Ctrl+= / Ctrl+-
key events with a native key code (which DevTools passes on to the browser's
shortcuts; checked with stock Chromium 141: 90% and 125% took effect) and
reads the zoom back (`Page.getLayoutMetrics`). If the zoom didn't change,
the level is reported as SKIP ("check this level by hand").

## What is still manual

| Step (MOLTNET_COMPLIANCE.md §4) | Status |
|---|---|
| 1 You are on Tor | automated online; the regular-window "not using Tor" half is manual |
| 2 Regular windows are untouched | partial: the toolbar label is manual |
| 3 Leak-test sites | replaced by the leak suite; optional cross-check |
| 9 Overrides refused | the extension-set proxy case is manual |
| 11 Onion sites | the Onion-Location bar is browser UI: manual |
| 14 Bridges | manual (needs the distribution's transports and bridge lines) |
| 16 Extensions | split mode, keyboard shortcuts, the toolbar and the PDF viewer are manual |
| 17 Regular-profile carry-over | media engagement, bookmarks and certificate exceptions need a prepared profile: manual |
| 19 Incognito's protections | manual (DevTools prefs, media controls, macOS, picture-in-picture, GCM, printing) |
| 20 Nothing goes around Tor | the AI chat, Cast menu, phone passkeys and "Open all" are manual |

Also manual: running on macOS and Windows (the kit's capture is Linux only;
the uniformity pages work anywhere a browser can load them), touchscreens,
multiple monitors, and the fingerprinting suites named in FINGERPRINTING.md
(CreepJS, FingerprintJS).

## Limits of the method

* Unix sockets aren't covered by a network namespace. The kit closes the
  name-resolution path (nsswitch). Printing (CUPS) and D-Bus services still
  act from outside. The Incognito-parity steps that involve them stay manual.
* The firewall drops what isn't Tor's, so the kit sees the **first** packet
  of a leak (a lookup, a SYN, a STUN request), not what would have followed.
  NETWORK_TRAFFIC.md's full capture of background traffic, with NetLog and
  request bodies, stays its own procedure. The kit's background list is
  DNS-level.
* A leak through a browser process that waits until after the run, or that
  needs a real user gesture the DevTools pipe can't give, is not seen.
* `--ignore-certificate-errors-spki-list` (the kit's test certificate for
  the https onion) is the one launch flag that changes behaviour under
  test. It names only the kit's own key. The AIA test uses a different key
  on purpose, so that certificate fetching stays real.

## Files

| Path | What |
|---|---|
| `tools/leak_test/leaktest.sh`, `leaktest.py` | entry point (`run`, `report`, `diff`) |
| `leakkit/harness.py` | the run: setup, browser launches, tests, phases |
| `leakkit/netns.py`, `launch_in_ns.sh` | namespace, firewall, cgroups, launching as the user |
| `leakkit/services.py` | page server, resolver, loopback canary, logging proxy |
| `leakkit/cdp.py` | DevTools protocol over the pipe |
| `leakkit/pcapread.py`, `dissect.py` | capture reader and dissectors (untrusted input; never raise) |
| `leakkit/traffic.py`, `background.py` | classification, attribution |
| `leakkit/catalog.py` | the network tests and the §4 step map |
| `leakkit/expectations.py`, `uniformity.py` | promised values, comparison, run diff |
| `leakkit/report.py` | report.json / report.md |
| `pages/` | the test pages (templated per test) |
| `tests/` | unit tests and the recorded sample run (`make_sample_run.py`) |

The kit's own tests need no root and no browser. CI runs them
(`.github/workflows/lint.yml`, job `leak-test-kit`):

```bash
python3 -I -m unittest discover -s tools/leak_test/tests -t tools/leak_test -v
```
