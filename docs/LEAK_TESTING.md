# Leak testing MoltNet on a built browser

`tools/leak_test/` turns most of the manual checks in
[MOLTNET_COMPLIANCE.md §4](MOLTNET_COMPLIANCE.md#4-check-it-yourself-do-this-on-every-release-build)
and the "Verification" sections of [FINGERPRINTING.md](FINGERPRINTING.md)
into automated pass/fail checks. Run it on the build machine (Linux) against
a built MoltBrowser. One command produces `report.md` (for people) and
`report.json` (send it back with the build's version).

It doesn't replace reading the code. It checks that what the code is meant
to do is what the built browser does.

The kit also captures a **regular** window's own background traffic and
checks it against [NETWORK_TRAFFIC.md](NETWORK_TRAFFIC.md):
`leaktest.sh traffic` ([below](#regular-window-traffic-capture)). Its
network pieces (the sinkhole route, the firewall rule, the resolver, the
sinkhole and the analysis of what tcpdump logged) have been run with the
real kernel in a scratch namespace (`tests/test_traffic_capture.py`,
root only); the first run with a browser is the build machine's.

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
by design. A few values legitimately differ between x86-64 and arm64 (the
bits a Float16 NaN is stored with): they are compared only between runs on
the same architecture, the kit host's, which `report.md` shows; between an
x86-64 and an arm64 run a difference there is listed as information.

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
* **A policy set while the browser runs.** For two launches (phases
  `policy_live` and `policy_live_incognito`) the kit mounts a directory of
  the run (root-owned, under
  `/run/moltleak-<id>/`) over `/etc/chromium/policies/managed`, where Linux
  builds without Chrome branding read mandatory policies, in the browser's
  private mount namespace only, and writes a policy file there while
  MoltNet is open. If that directory doesn't exist on the host, the kit
  makes it (empty) for the run and removes it afterwards; policy files the
  host has there are hidden from that launch and never touched.

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
| `bgfetch_a`, `bgfetch_b` | on each onion site, a service worker registration starts a Background Fetch of a name of the test's own (`bgf-<nonce>.<canary domain>`), which the browser's download service fetches | nothing outside Tor; `identity.background_fetch_circuits`: the fetch goes to Tor with its page's site's SOCKS username, not one of its destination |
| `dom_storage_rate` | writes 3000 `localStorage` items as fast as it can, then counts what is left once the storage service has answered | the burst (1000) is kept, no more than the burst and what refilled while the page wrote (100 a second), not all 3000: the writes past the site's write rate failed and reverted (MOLTNET_COMPLIANCE.md §2, *DOM storage totals*) |
| `client_cert` | the basic page from an https onion port (8444) whose server asks for a TLS client certificate without requiring one, and logs every handshake | nothing outside Tor; `net.client_cert.handshakes`: every handshake completes and none carries a certificate, so the browser neither asked the user nor connected again. In a regular window Chromium drops the first handshake to ask and connects again; the check reports that as information (step 20) |
| `builtin_ai` | `availability()` and `create()` (with a user gesture) of every built-in AI API | `LanguageModel`, `Summarizer`, `Writer`, `Rewriter`, `Translator` unavailable; no component or model download host looked up |
| `geolocation` | `getCurrentPosition()` | error 1 at once (no prompt); no location provider host |
| `display_capture` | `getDisplayMedia()` with audio and with `preferCurrentTab` (with a user gesture), getUserMedia()'s `chromeMediaSource: 'screen'` (recorded), the `display-capture` permission in the page and a worker | `NotAllowedError` "Permission denied by user" within 2 s (no picker); the permission is `denied` in both |
| `extension` | a test extension allowed in Incognito (content script, web-accessible resource, `tabs`, `webRequest`) | the MoltNet page isn't marked, `chrome.runtime` is absent, the resource can't be fetched, and its background never sees a MoltNet tab or request (it would look up a canary name if it did) (step 16) |
| `regular_onion_navigation` | a **regular** window opens an onion address | the "Tor onion site" page; no lookup of the name (step 12) |
| `regular_onion_subresources` | a regular page loads images, `fetch`, a WebSocket and a worker fetch from an onion host | all fail; no lookup (step 12); with a proxy configured, the proxy sees none (step 13) |
| `regular_dom_storage_rate` | the `dom_storage_rate` page in a **regular** window | all 3000 writes are kept: regular windows have no write rate |

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
| Nothing held after the end | before New Identity the old page calls `fetchLater()` (deferred two minutes) and sends a beacon on `pagehide`: both are sent only once the page closes, after its identity ended, and neither may reach the kit's page server (`identity.nothing_held_after_end`; with no `fetchLater()` in the page, only the beacon) | 7 |
| Nothing after the end, racing it | while New Identity runs, the old page keeps three fetches to the kit's server going, one after another; the first one refused (the end cancels what is in flight and refuses what follows) makes it start one request of each kind (fetch, keepalive fetch, beacon, image, WebSocket, prefetch) and say so on the console, which the harness reads over DevTools. None may reach the kit's server (`identity.nothing_after_end_race`). Not judged (SKIP) if the page saw a refusal before New Identity began (Tor or the server failing) or closed before it saw one. Online, the phase first takes the old page back to the kit's server from the exit check, so these and the held requests go to the kit | 7 |
| Per-site circuits | the two onion sites' SOCKS5 connections carry different usernames, and every one carries one; two pages their servers sandbox (`Content-Security-Policy: sandbox`, opaque origins) on two sites use different usernames too (`identity.sandboxed_circuits`); the certificate fetches made for the two certificate tests use usernames of their own, none shared between the two sites and none a page used, and the second site's check makes a fetch of its own (`identity.cert_fetch_circuits`); a Background Fetch, which the browser's download service makes, uses the username of the site that started it (`identity.background_fetch_circuits`; SKIP if no Background Fetch reached Tor, for example because the permission was refused) | 8 |
| The relays are real | the panel's guard, middle and exit fingerprints (`moltnet.getCircuit`) are in the consensus MoltNet's Tor downloaded | 10 |
| Tor's shared capacity | the torrc MoltBrowser started its Tor with sets `ConnLimit 1000` and `MaxClientCircuitsPending 256`, the capacity MoltNet's connection and circuit budgets are sized against (`identity.tor_capacity`). The limits themselves (each site's connections, the Tor process's budget of 768, each site's 16 SOCKS handshake slots within Tor's 192, an onion address counting three, each site's keepalive requests, storage and blob memory) would take hundreds of connections or thousands of requests through Tor to reach, too slow and too uneven for a kit run: they are checked on the build machine (`docs/BUILD_MACHINE_CHECKLIST.md`, "moltnet-shared") | 10 |
| On Tor | the panel's *Verify* and `check.torproject.org/api/ip` in the MoltNet tab (online) | 1 |
| Fail-closed | the `tor` cgroup is killed; the next MoltNet navigation fails with a proxy/SOCKS error and nothing leaves directly | 5 |
| No restore | a relaunch with the same profile shows no MoltNet page | 5 |
| Overrides refused | a relaunch with `--proxy-server=` (the kit's logging proxy): `openWindow` returns an error and no MoltNet window opens; the same launch runs step 13 | 9, 13 |
| Partitioning switches refused | one relaunch per switch MoltNet needs on, each with `--disable-features=` for it (`PartitionConnectionsByNetworkIsolationKey`, `SplitCacheByNetworkIsolationKey`, `ThirdPartyStoragePartitioning`): `openWindow` returns an error that names the switch, and no MoltNet window opens (`identity.partitioning_override_refused`, phase `partitioning_override`) | 9 |
| A policy set while MoltNet is open | a relaunch with the run's policy directory mounted (above): with a MoltNet page fetching a canary name every 250 ms, the kit writes `{"ProxyMode": "direct"}`. Every MoltNet page must close within 60 s (Chromium reads a changed policy file about 5 s later), the canary must never be looked up outside Tor, `openWindow` must then fail, and once the policy file is removed MoltNet must open again (`identity.policy_change_closes`, phase `policy_live`) | 9 |
| No Autofill server query | in the same launch, with `--enable-features=AutofillServerCommunication` (MoltBrowser turns the queries off by default), a MoltNet page with an address and a sign-in form: no connection to `content-autofill.googleapis.com` or `chromium-i18n.appspot.com`, through Tor (the capture's SOCKS5 requests) or outside it, during the phase (`net.autofill_server`) | 20 |
| Language policies | in the same launch (with `ReduceAcceptLanguage` on, so that the policy below changes the regular header), once MoltNet has opened again: the kit writes `{"ForcedLanguages": ["ja"], "ReduceAcceptLanguageEnabled": false}`, then loads a page in a MoltNet tab and in a regular tab twice (each load runs the page's language detection, after which Chromium rebuilds a profile's languages). The regular page's `navigator.languages` or `Accept-Language` must change (else the policy did not arrive: ERROR), MoltNet's must not, no forced language may show in MoltNet, and MoltNet must stay open (`identity.language_policy_ignored`, phase `policy_live`) | 9 |
| Incognito availability | a second launch with `--auto-open-devtools-for-tabs` and DevTools set to open undocked (the kit writes the dock state into the regular profile's `Preferences`; MoltNet's DevTools reads it through): MoltNet opens with its DevTools in a window of its own, the page asks "Leave site?" on a navigation away (user activation, prompt left unanswered). The kit writes `{"IncognitoModeAvailability": 1}`: every MoltNet page, DevTools included, must close within 60 s, `openWindow` must then fail with an error that names Incognito, and open again once the file is removed. Then `{"IncognitoModeAvailability": 2}` (Incognito only): MoltNet must stay open and open (`identity.incognito_off_closes`, `identity.incognito_forced_allows`, phase `policy_live_incognito`) | 9 |

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
  within the allow-list, no WebGPU adapter, fonts only from MoltBrowser's
  bundled font set on every OS (no other family visible, every name of the
  set visible, each generic keyword measuring like the set's family for
  it; per-script fallback widths and the set's `local()` faces compared
  between runs, across OSes too),
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
  output timestamp, `playbackStats`, and when it renders: with a user
  gesture so that a context may start, `currentTime` polled for 300 ms
  shows a burst of 3 or 4 render quanta every 10 ms, the uniform device's
  clock), an `audiooutput` entry in `enumerateDevices()` before any
  permission, no key system (Clear Key, Widevine,
  PlayReady, and Media Capabilities with a `keySystemConfiguration`), Media
  Capabilities' fixed answers for VP8, VP9 and AV1 video and for WebRTC,
  and WebCodecs refusing `prefer-hardware`. Codec support (Media
  Capabilities for Opus, H.264 and HEVC, `canPlayType()`,
  `MediaSource.isTypeSupported()`, WebCodecs without a hardware
  preference) follows the build and is compared between runs.
* **MediaRecorder, `local()` and storage limits** (once per launch): the
  type MediaRecorder reports for a canvas stream by default and for WebM,
  Matroska and MP4 (VP8, VP8 and VP9 from the build's own encoders); no
  `src: local()` face of a family outside the allow-list loads, installed
  or not (the result is checked, not the time: such a face is never looked
  up, so it costs what a missing one costs); the quota the browser enforces
  for the page's site, its budget of 60 MiB (all its storage keys
  together), read through DevTools (`Storage.getUsageAndQuota`). Writing until `QuotaExceededError` would
  find the same limit but takes too long for the kit, so the configured
  limit is checked instead. What `isTypeSupported()` accepts and which
  listed faces `local()` finds follow the build and the OS and are
  compared between runs.
* **CPU features** (once per launch): a WebAssembly module with a relaxed
  SIMD instruction (`f32x4.relaxed_madd`) fails `WebAssembly.validate()`.
  Compared between runs: a hash of 27 `Math` results (`Math.pow` and
  `Math.tanh` among them), a 64 x 32 sRGB frame, as RGBA and as I420, read
  back in Display P3 (`VideoFrame.copyTo()`), and a quarter second of
  stereo Vorbis decoded by WebCodecs' `AudioDecoder` (`cpu.*`,
  `pixels.p3_readback`, `audio.decoded_vorbis`).
* **What pages can learn about the processor** (once per launch): a
  WebAssembly module with one plain SIMD instruction (`f32x4.add`)
  validates (`cpu.wasm_simd`: MoltNet opens only on processors where it
  does). `Float16Array` and `DataView.setFloat16()` store NaNs with
  payloads, computed at run time, as the canonical NaN with their sign,
  before and after V8's optimizing compilers take the storing functions
  (`cpu.float16_nan_canonical`, checked on x86-64; arm64's optimized code
  keeps payload bits, the same on every arm64 CPU, so there it is compared
  between arm64 runs, as the bits themselves, `cpu.float16_nan`, are on
  both). Compared between runs: a 96 x 64 lossy JPEG XL image (made for the
  kit) decoded by WebCodecs' `ImageDecoder`, as the frame's planes and as
  RGBA (`pixels.jxl`; `unsupported` in a build without JPEG XL), and a
  32-bit float WAV played by an `<audio>` element at `playbackRate` 1.5 with
  its pitch kept (time stretching) into Web Audio and recorded by an
  `AudioWorklet` (`audio.stretched`: a hash of 0.75 s from the first sound,
  with the 8 low mantissa bits cleared because a worklet's input is
  farbled there; the stretching's choices change far more; with a user
  gesture, after the page's collection, like the media clock).
* **The microphone** (a launch of its own, [below](#the-microphone-launch)).
* **Portable math** (once per launch), compared between runs: `Math`'s
  `sin`, `tan`, `atan` and `atan2` at arguments where x86-64 and arm64
  builds of V8's math code differed, and a sweep of 2000 more
  (`cpu.math_cross_arch`); sixteen CSS math functions in registered
  custom properties, each amplified with `mod()`, `rem()` or `round()` so
  that its last bits show in the serialized value (`cpu.css_math`);
  `DOMMatrix`, `SVGMatrix`, CSSOM `toMatrix()`, canvas `getTransform()` and
  an SVG path's arcs (`cpu.dommatrix`); Biquad and IIR filter frequency
  responses and a compressor's `reduction` after rendering a sawtooth
  (`audio.math`).
* **Geometry** (once per launch), compared between runs: where the edges
  of 40 canvas arcs and rotated ellipses (`isPointInPath()`) and of a
  stroked curve with every join, cap and a dash pattern
  (`isPointInStroke()`) lie, found a float ulp at a time, and where
  `elementFromPoint()` finds the edges of `clip-path` shapes (a rounded
  polygon, `circle(closest-side)`, an ellipse, `shape()` with an arc) and
  `corner-shape` corners, a layout unit at a time (`geom.hit_testing`);
  SVG shapes' `getTotalLength()` and `getPointAtLength()` and the boxes of
  elements along six `offset-path`s (`geom.path_length`); the start of
  every line of inline boxes laid out around six `shape-outside` floats
  (gradients with colour hints and in OKLab, an SVG image with a blur, a
  gamma transfer, a hue rotation and a light turned into alpha, a rounded
  polygon, a margin-box with radii and `shape-margin`)
  (`geom.shape_outside`); and `contrast-color()` for 87 colours around
  its threshold (`geom.contrast_color`, `absent` without it).
* **Recorded, not promised** (shown when two runs differ): media device
  kinds, storage quota, JS heap limit, speech voices, battery, keyboard
  layout, WebRTC codecs, `system-ui` width, the test page's own scrollbar
  width, the processed microphone track's sample rate.

### The microphone launch

Phase `mic_processing`, one browser launch of its own (it needs flags that
would change the other checks), checks [FINGERPRINTING.md "MoltNet: the
microphone's processing"](FINGERPRINTING.md#moltnet-the-microphones-processing).
The kit writes a fixed WAV into the run directory (`leakkit/micinput.py`:
44.1 kHz stereo, the format of Chromium's fake capture device, so it needs
no conversion; 2 s of silence, a 30 ms burst, 3.5 s of two synthetic
voices, 1 s of silence; integer arithmetic only, the same bytes on every
machine) and starts the browser with `--use-fake-device-for-media-stream`,
`--use-file-for-fake-audio-capture=<the WAV>%noloop` and
`--use-fake-ui-for-media-stream` (the permission is granted without a
prompt). In a MoltNet window, `mic.html` asks for the microphone with echo
cancellation, noise suppression and gain control, reads the processed 10 ms
buffers through `MediaStreamTrackProcessor` (exact samples: `AudioData` is
not farbled), finds the first buffer whose peak exceeds 0.05 (the burst;
the processing starts at the file's start whenever the page starts
listening) and hashes the left channel of 200 buffers from there. It does
that twice, each time with a new stream.

| Key | Must |
|---|---|
| `audio.mic_processed_stable` | be true: the two captures agree (otherwise the hash can't compare machines; a gap in the buffers' timestamps or no burst is reported instead of a hash) |
| `audio.mic_settings` | show echo cancellation, noise suppression and gain control on |
| `audio.mic_processed` | be the same in two runs (`diff`), on every CPU and OS: compare an x86-64 machine without AVX2 (or a VM started with `-cpu Nehalem`) with a recent one, and with an arm64 one |

The fake device reads the file from the audio service, which on Linux runs
without a sandbox by default (`AudioServiceSandbox` is off there at 156); if
the build turns that sandbox on, the device delivers silence and the check
reports `silent`. A regular window (`--target regular`) records the same
values for comparison: there they differ between CPUs with and without AVX2.
The first run on the build machine is also the probe's first test.

A regular-window fingerprint is recorded as a control. The report lists the
values that are already uniform outside MoltNet on this machine (for
example, a machine with exactly 4 cores), because there the MoltNet check
proves less.

**Settings from the regular profile (step 15).** Before the first launch the
kit seeds the regular profile's `Preferences` with Do Not Track on, a 24px
default font and DejaVu Serif as the standard font, German
`Accept-Language` (and the same list as the user's selected languages,
which Chromium rebuilds `Accept-Language` from), third-party cookies
allowed, and JavaScript and location
blocked for the https onion (and, as the control, for a local page). The
MoltNet checks above must still read MoltBrowser's defaults, and the
fingerprint page itself runs on the onion where JavaScript is "blocked".

**Zoom.** DevTools has no page-zoom command, so the kit sends Ctrl+= / Ctrl+-
key events with a native key code (which DevTools passes on to the browser's
shortcuts; checked with stock Chromium 141: 90% and 125% took effect) and
reads the zoom back (`Page.getLayoutMetrics`). If the zoom didn't change,
the level is reported as SKIP ("check this level by hand").

## Regular-window traffic capture

`leaktest.sh traffic` records what a **regular** MoltBrowser window
contacts on its own, and checks it against
[NETWORK_TRAFFIC.md](NETWORK_TRAFFIC.md). It needs no Tor and no internet:
nothing it does leaves the machine.

```bash
sudo tools/leak_test/leaktest.sh traffic --browser chromium/src/out/MoltBrowser-linux/chrome
#   ... about 17 minutes ...
#   verdict: PASS  (0 FAIL, 0 ERROR, ...)
#   report: ./moltleak-runs/20261025-153000-traffic-a1b2c3/report.md
```

It runs as `run` does (root for the namespace, the firewall and the
captures; the browser and the servers as you) and needs the same packages,
except `tor`.

| Option | Default | What for |
|---|---|---|
| `--idle SECONDS` | 600 | how long the browser is left alone after its first run |
| `--first-run-wait`, `--restart-idle`, `--update-wait`, `--final-wait` | 45, 120, 45, 20 | the other steps' lengths |
| `--skip-browse` | | leave out the scripted browse |
| `--hosts FILE` | `docs/network_traffic_hosts.json` | another allow-list |
| `--display`, `--screen`, `--locale`, `--timezone`, `--browser-arg`, `--user`, `--out` | | as for `run` |

**The steps.** A fresh profile (a directory of the run), launched as a user
would: no `--no-first-run` and no start URL, so the first run and the
start-up tabs are the browser's own.

| Step | What happens |
|---|---|
| `first_run` | the first launch; waits 45 s |
| `idle` | nothing for 10 minutes |
| `home` | opens the new tab page |
| `form` | fills a shipping-address form and a sign-in form with made-up values (DevTools `Input.insertText`) and submits the sign-in |
| `translate` | opens a page in French (Translate offers itself; in MoltBrowser on the device) |
| `spellcheck` | types misspelt words into a text area (on Linux the dictionary downloads the first time) |
| `download` | downloads a small `.exe` (Safe Browsing's download check is expected here, and only here) |
| `dns_error` | opens a name under `.invalid`, which the kit's resolver doesn't resolve (the error page's DNS probe must not happen) |
| `cert_error` | opens a local page whose certificate expired in 2020 (network time may be asked; the captive-portal check must not happen) |
| `shutdown`, `restart` | closes the browser, starts it again with the same profile, idle for 2 minutes |
| `update_check` | opens `molt://ai-update`, which asks GitHub for the latest release |
| `final` | waits for late requests, then closes the browser |

**How nothing gets out.**

* The kit's resolver (`leakkit/sinkhole.py`, `fakedns`) is the only one the
  browser can use (the namespace's `resolv.conf` and `nsswitch.conf`, as for
  `run`). It answers each name with an address of its own in
  `198.18.0.0/15` (the benchmarking range), so a connection's address says
  which name it was for. `AAAA`, `HTTPS` and every other record type get an
  empty answer; names under `.invalid` get NXDOMAIN. Each query and answer is
  logged.
* That range is routed to the namespace's loopback as local addresses. The
  sinkhole (`leakkit/sinkhole.py`, `sinkhole`) listens on ports 80 and 443
  there and accepts TLS with the kit's test key, which the browser accepts
  through `--ignore-certificate-errors-spki-list` (the same key and flag as
  `run`'s test pages). It logs the server name, the HTTP method and path
  and a few headers (an API key or a cookie only by its length; never a
  body), answers `503` and closes. From the component updater's check
  (`POST /service/update2/json`) it keeps one thing besides: the IDs of
  the components the check names (`update_apps`).
* The firewall logs the first packet of every flow to the range (nflog,
  prefix `sinkhole`), so a connection to a port the sinkhole doesn't
  listen on is recorded too. Everything else is logged and dropped as in
  `run` (DNS to any other server, connections to an address the browser
  had without asking the resolver, multicast joins). Nothing is allowed
  out: a packet that leaves the namespace anyway fails the run
  ("escaped").

**The checks** (`leakkit/bganalysis.py`). Each lookup and connection is
put in a step by its time and compared with
[NETWORK_TRAFFIC.md section 6](NETWORK_TRAFFIC.md#6-machine-readable-host-list),
through `docs/network_traffic_hosts.json` (`leakkit/hostlist.py`; the run
refuses to start if the JSON doesn't match the doc).

| Check | Fails when |
|---|---|
| `traffic.forbidden` | a host the doc marks `forbidden` was looked up or contacted |
| `traffic.not_listed` | a host the doc doesn't list (also an mDNS join) |
| `traffic.user_only` | a host the doc allows only after a user action the capture never takes |
| `traffic.outside_its_step` | a step-bound host outside its step (the download check outside `download`) |
| `traffic.unexpected_port` | a listed host on a port the doc doesn't list |
| `traffic.components` | the component updater's check names a component from the doc's component table (section 6: off in MoltBrowser, or asked for only on demand, like the on-device model manifest); SKIP when no update check with readable IDs reached the sinkhole; the evidence lists every check's IDs |
| `traffic.harness.escaped` | anything left the namespace |
| `traffic.harness.capture`, `.steps`, `.update_check` | ERROR: the captures are empty, a step didn't run, or the update check never reached the sinkhole (the control that proves the capture works) |
| `traffic.expected`, `traffic.not_seen`, `traffic.kit` | INFO: what was expected and seen, what the doc expects that this run didn't see, and the kit's own pages |

Each failure names the time since the start, the step, the host, the port,
the TLS server name and request line, and a likely source: the doc's own
"Why" for a listed host, otherwise a guess from the host, the path and the
step (a startup timer when it came within 75 s of a launch). `report.md`
also has a timeline of everything. Send back `report.json` with the
build's version.

To triage a failure: if MoltBrowser needs it, add it to NETWORK_TRAFFIC.md
(the detail section and the section 6 table, with the reason), run
`tools/leak_test/leaktest.sh hosts --write`, and rebuild the report with
`leaktest.sh report <run dir>`; otherwise turn it off
(`molt_feature_overrides.cc`, a pref default) and run the capture again.

**Limits.** Nothing answers, so the capture sees the first request of each
service, not what an answer would lead to: no component or update
download, no second request that depends on the first. It doesn't see
request bodies (NETWORK_TRAFFIC.md's full capture with mitmproxy does).
Unix-socket services (D-Bus, CUPS) act outside the namespace, as for `run`.
It doesn't type into the address bar (DuckDuckGo's suggestions) or sign
in, and it runs on Linux only.

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
| 20 Nothing goes around Tor | the AI chat, Cast menu, phone passkeys, "Open all" and address formats (a saved address to fill) are manual |

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
  The regular profile's background traffic is the traffic capture's job
  (above), which sees server names and request lines; the background list
  `run` uses is DNS-level (`leakkit/background.py`, kept in line with
  NETWORK_TRAFFIC.md by the kit's tests).
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
| `leakkit/micinput.py` | the microphone launch's WAV |
| `leakkit/report.py` | report.json / report.md |
| `leakkit/bgcapture.py`, `sinkhole.py` | the regular-window traffic capture: its steps, resolver and sinkhole |
| `leakkit/bganalysis.py`, `hostlist.py` | its comparison with NETWORK_TRAFFIC.md and its report; the allow-list (`hosts [--write]`) |
| `pages/` | the test pages (templated per test; `bg_*.html` for the capture; `mic.html` for the microphone launch) |
| `tests/` | unit tests and the recorded sample run (`make_sample_run.py`) |

The kit's own tests need no root and no browser. CI runs them
(`.github/workflows/lint.yml`, job `leak-test-kit`):

```bash
python3 -I -m unittest discover -s tools/leak_test/tests -t tools/leak_test -v
```
