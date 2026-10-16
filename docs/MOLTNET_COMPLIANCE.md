# How MoltNet routes through Tor, and how to check it

*MoltBrowser transparency page. Last reviewed 2026-10.*

MoltNet sends the traffic of **MoltNet windows** through the 3-hop Tor network.
This page describes the design, what has and has not been verified, how you can
check it yourself, and what MoltNet does **not** protect you from.

> **How we talk about it.** We don't call MoltNet a "VPN," and we avoid
> *anonymous*, *untraceable*, and *military-grade*. MoltNet is **private IP
> routing over the Tor network for the windows you open with it**. It hides your
> IP address and location from the sites you visit in those windows and from
> your network. It is not Tor Browser (§5).

## Status in one paragraph

MoltNet was redesigned in October 2026. It no longer routes your whole browser:
it opens **separate MoltNet windows** in an isolated profile, and only those
windows use Tor. The redesign has been reviewed in code and its Tor control-port
layer has been tested against a real Tor (§3), but **the new design has not yet
been tested end-to-end in a built browser**. The July 2026 test results that
used to be on this page were for the old whole-browser design and no longer
apply. Until the checks in §4 have been run on a release build, treat the
claims below as the design intent, not as measured results.

## 1. What MoltNet does

1. **Separate Tor windows.** "Enable MoltNet" starts MoltBrowser's own Tor and
   opens a MoltNet window. Every tab in a MoltNet window goes through Tor.
   Your regular windows and your normal private (incognito) windows are never
   routed through Tor, and they are never touched by MoltNet's proxy settings.
2. **A separate identity.** MoltNet windows use their own off-the-record
   profile: cookies, cache and site storage are kept in memory, separate from
   your regular and incognito windows, and discarded when the last MoltNet
   window closes. Pages visited in MoltNet windows are not added to MoltBrowser's
   on-device memory index.
3. **New Identity.** Closes the MoltNet windows, drops their connections and
   DNS cache, discards the MoltNet profile (fresh cookies, cache and storage),
   asks Tor for new circuits (`SIGNAL NEWNYM`, and the reply is checked) and
   opens a fresh MoltNet window. If Tor rejects the request, you see an error.
4. **One circuit per site.** Each MoltNet connection tells Tor which top-level
   site it belongs to (as the SOCKS username), so Tor puts different sites on
   different circuits and exits (`IsolateSOCKSAuth`). A site's embedded
   third-party content shares that site's circuit, as in Tor Browser.
5. **A visible route.** The MoltNet panel shows the guard, middle and exit relay
   of the current circuit (nickname, country, IP, fingerprint), read from Tor's
   own control port. It never looks up or shows your real IP. In a MoltNet
   window, *Verify* fetches `check.torproject.org` through Tor.
6. **Bridges.** If Tor is blocked or watched where you are, Settings → MoltNet
   offers the built-in snowflake or obfs4 bridges, or bridge lines you paste
   from bridges.torproject.org. Bridges need a *pluggable transport*: the
   copy that comes with MoltBrowser or, on Linux, your distribution's
   `obfs4proxy` (or `lyrebird`) and `snowflake-client` packages. Settings
   tells you which one to install when it's missing, and MoltNet won't
   connect without the bridges you chose. Linux packages don't include the
   built-in bridge list, so there you paste bridges (*Custom bridges*).
7. **Exit country (optional).** You can pin the exit to a country. This shrinks
   the set of exits you share with other Tor users, which **reduces anonymity**;
   the choice is not saved and resets when MoltBrowser restarts.
8. **Onion services (`.onion`).** In a MoltNet window, onion sites load through
   Tor like any other site; `http://` onion addresses are not upgraded or
   blocked by HTTPS-Only, because an onion connection is already encrypted
   end to end. When an `https://` site announces an onion version
   (`Onion-Location` header, or `<meta http-equiv="onion-location">` in the
   page's `<head>`), a bar offers *Open .onion version*; MoltBrowser never
   switches on its own.
9. **Onion addresses stay out of regular windows.** A regular or incognito
   window can't reach onion services, so it doesn't try: opening a `.onion`
   address there shows "This is a Tor onion site" with a button to open it in
   a MoltNet window (nothing opens until you click). The onion name is never
   looked up in DNS (also when a page in a regular window tries to load
   something from an onion address) and is not sent to your search engine's
   suggestions while you type it. If your regular windows use a proxy
   server, onion names aren't sent to it either: images, scripts, other
   requests, downloads and WebSockets to onion addresses are refused before
   they reach the network. While MoltNet is running, sites that announce an
   onion version also get an *Open in MoltNet window* bar in regular
   windows.

## 2. How it is built

**Isolation.** MoltNet windows run in a non-primary off-the-record profile
(`OTRProfileID::CreateUnique("MoltNet::Tor")`) with its own network context.
The Tor proxy and the leak-prevention settings below are written only to that
profile's in-memory preferences. Earlier versions wrote the proxy into the
regular profile and into the browser-wide (local state) settings; on startup
MoltBrowser now removes exactly those old values.

**Fail-closed.** The MoltNet profile's proxy is `socks5://127.0.0.1:<port>` with
no direct fallback, so if Tor stops, pages in MoltNet windows fail with a proxy
error instead of loading directly. A MoltNet window is only opened after (a) Tor
reports an established circuit, (b) the proxy and leak settings are confirmed
to be MoltNet's own (a policy, extension or command-line flag that overrides
them blocks MoltNet with an error), and (c) the MoltNet profile's network
context itself confirms it would send requests to exactly that SOCKS5 endpoint.
MoltNet windows are never restored after a restart.

**Leak controls on the MoltNet profile.**

| Setting | Why |
|---|---|
| `socks5://` proxy | hostnames are resolved by Tor, not your DNS resolver |
| WebRTC `disable_non_proxied_udp` | WebRTC can't open direct UDP that would reveal your IP |
| Network prediction off | no DNS prefetch / preconnect outside the proxy |
| HTTPS-Only mode on | warns before any plain-HTTP page, which the exit relay could read or modify (not for `.onion`, which never leaves Tor) |
| Connections partitioned by top-level site | no socket reuse across sites; enables per-site circuits |

**Onion addresses.** RFC 7686 reserves `.onion` for Tor, and the Chromium
version MoltBrowser is built on doesn't special-case it, so MoltBrowser does,
in four places outside MoltNet's Tor connection: a navigation check that runs
before any other one stops `.onion` navigations in non-MoltNet windows (the
page shown instead is a browser interstitial, so a website can't press its
button, and the button only acts on a real click); every other request from
a non-MoltNet window (images, scripts, fetches, worker and service-worker
loads, prefetches, downloads, and WebSockets from pages and dedicated
workers) to an onion host, or redirected to one, is refused in the browser
process before it reaches the network service, which matters when a proxy
server would otherwise resolve the name; the host resolver refuses every
`.onion` name (no DNS, DNS-over-HTTPS, mDNS or hosts-file lookup, which also
covers preconnects); and the address bar doesn't send onion addresses to the
search engine's suggestion service. In MoltNet windows the onion name goes
to Tor inside the SOCKS5 request and is never resolved locally.
`Onion-Location` is honored only on `https` pages that aren't onion sites
themselves, only once per response, and only for a syntactically strict
absolute `http(s)` URL whose host is a v3 onion address. A response without
the header may say the same with exactly one `<meta
http-equiv="onion-location">` in its `<head>`, read after the page is parsed
by a script in an isolated world (page scripts can't change how it reads
the page) and checked by the same rules; if the header is present, `<meta>`
elements are ignored.

**MoltBrowser's own Tor.** MoltNet runs only the Tor bundled with MoltBrowser
or, on Linux (whose packages don't bundle Tor), the distribution's `tor`
package at `/usr/bin/tor` or `/usr/sbin/tor`, and that one only if `root` owns
the file and every folder above it and none of them can be changed by other
users. A different Tor binary requires the explicit
`--moltnet-tor-binary=<path>` switch (owned by you or `root`, not writable by
others); there is no Homebrew/PATH fallback. If no usable Tor is found,
MoltNet says so; on Linux: "Install the tor package". MoltNet starts its own
Tor process as you, with its own configuration and data folder, so a system
Tor's settings and its `tor` service (which Debian enables) are not used. Tor
picks its own SOCKS and control ports (`auto`), writes them to a private file,
and every control connection uses SAFECOOKIE authentication, checking Tor's
proof (`SERVERHASH`) before sending ours, so a program squatting on a port
cannot impersonate Tor. Tor is told to exit when MoltBrowser exits
(`__OwningControllerProcess` + `TAKEOWNERSHIP`). A system-wide
`torrc-defaults` is ignored. Pasted bridge lines are validated strictly (only
obfs4/snowflake, fixed character sets) so they cannot inject other Tor
options. The pluggable transports bridges need follow the same rule as Tor:
the copy bundled with MoltBrowser or, on Linux, the distribution's
`/usr/bin/lyrebird` or `/usr/bin/obfs4proxy` (obfs4) and
`/usr/bin/snowflake-client` (snowflake), each only if `root` owns the file
and every folder above it; Tor is told to run that exact file by its full
path. If the transport your bridges need is missing, MoltNet doesn't start
Tor (it never falls back to connecting without bridges) and says which
package to install. The same ownership checks apply to the OCR (tesseract) and
voice (whisper) helpers: the bundled copy, or on Linux a `root`-owned
`/usr/bin/tesseract` or `/usr/bin/whisper-cli`; never Homebrew, `/usr/local`
or `PATH`.

**Supply chain.** On Linux, Tor comes from your distribution's signed packages
(MoltBrowser's `.deb`/`.rpm` declare `Recommends: tor`); MoltBrowser doesn't
pin its version. Elsewhere, release builds refuse to bundle a Tor download
whose SHA-256 is not pinned in `scripts/bundle-tor.sh`. The bump to Tor 15.0.15
left the pins empty on purpose (the Tor archive was unreachable when the
version was bumped), so release builds fail until they are filled in using the
gpg-verified steps documented in that script.

## 3. What has been verified, and how

| Check | How | Result |
|---|---|---|
| Control-protocol parsing, SAFECOOKIE HMAC, torrc generation, bridge validation | 22 unit tests (`molt_ai_unittests`); HMAC vectors computed independently with Python `hmac` and `openssl` | Pass (built standalone; not yet built in the Chromium tree) |
| The generated torrc (incl. bridges + exit pin) is valid | `tor --verify-config`, Tor 0.4.9.11 | Pass |
| SAFECOOKIE against a real Tor; wrong cookie rejected via SERVERHASH; `TAKEOWNERSHIP` makes Tor exit when MoltBrowser's connection closes; `SETCONF`/`RESETCONF`/`NEWNYM` accepted | Standalone harness around the shipped control-connection code, Tor 0.4.9.11 | Pass |
| Tor accepts MoltBrowser's SOCKS5 username/password greeting and records a per-site username | Byte-for-byte replay against Tor 0.4.9.11 | Pass |
| `.onion` detection, v3 address and `Onion-Location` validation | 6 unit tests (`molt_ai_unittests`) | Pass (built standalone; not yet built in the Chromium tree) |
| Helper-binary checks (bundled copy; root-owned system Tor on Linux; rejects user-owned, group/world-writable, setuid, symlink escapes) | 13 unit tests incl. real-filesystem cases (`molt_ai_unittests`) | Pass (built standalone; not yet built in the Chromium tree) |
| Bridges with system transports: which `/usr/bin` paths are tried, the `ClientTransportPlugin` lines (absolute path for a system transport), exec-path validation (no blanks, `..`, bare names or torrc injection), which transports a set of bridges needs, the install messages | 7 unit tests (`molt_ai_unittests`), with ASan/UBSan | Pass (built standalone; not yet built in the Chromium tree) |
| Tor runs the distribution's transports from the torrc MoltBrowser writes | `tor --verify-config`, then a short run of Tor 0.4.9.11 with Ubuntu 24.04's `obfs4proxy` 0.0.14 and `snowflake-client` 2.5.1 at absolute paths: both complete the managed-proxy handshake and are registered, and Tor connects to them (no bridge was reachable from the test machine) | Pass |
| Settings names the transport to install | jsdom smoke test of molt://ai-settings (`tools/webui_checks`) | Pass |
| Onion request URLs (any scheme, look-alikes refused), `<meta>` `Onion-Location` rules, validation of the page's answer | Unit tests (`molt_ai_unittests`) | Written; need a Chromium build |
| The script that reads `<meta http-equiv="onion-location">` | Run under jsdom against 13 documents (`<head>` vs `<body>`, letter case, look-alikes, missing `content`, more than one, length cap) | Pass |
| `.onion` blocked and offered in regular windows, no DNS query, no onion request or WebSocket reaching a configured proxy, `Onion-Location` bar (header and `<meta>`), `http://` onion under HTTPS-Only, bridges connecting through the distribution's transports | Needs a Chromium build | **Not yet done** |
| Isolation, fail-closed behaviour, leak controls, New Identity, per-site circuits in a running browser | Needs a Chromium build | **Not yet done** |
| Independent third-party audit | — | **Not done** |

## 4. Check it yourself (do this on every release build)

Do all of this in a MoltNet window unless the step says otherwise.

1. **You are on Tor.** Open `check.torproject.org`: it must say you are using
   Tor and show an IP that isn't yours. The panel's *Verify* must show the
   same exit IP.
2. **Regular windows are untouched.** In a regular window, the same page must
   show your normal IP and "not using Tor"; the toolbar shows "MoltNet", not
   "Tor".
3. **No IP, DNS, WebRTC or IPv6 leak.** ipleak.net, browserleaks.com/ip,
   browserleaks.com/webrtc, dnsleaktest.com (extended), test-ipv6.com: none may
   show your real IPv4/IPv6 address, your ISP's DNS resolver, or a local IP.
4. **Watch the wire (decisive).** While loading a site in a MoltNet window:
   `sudo tcpdump -n host <that site's IP>` must show nothing, and
   `sudo tcpdump -n udp port 53 or udp port 443` must show nothing caused by the
   page; only the Tor guard (or bridge) should be contacted.
5. **Fail-closed.** Turn MoltNet off from a regular window, or kill the `tor`
   process: pages in an open MoltNet window must fail with a proxy error,
   never load directly. Restart MoltBrowser: no MoltNet window comes back.
6. **Separate identity.** Log in to a site in a regular window; open it in a
   MoltNet window: you must not be logged in. Set a cookie in a MoltNet window,
   close all MoltNet windows, open a new one: the cookie must be gone.
7. **New Identity.** Note the exit IP, press *New Identity*: the MoltNet windows
   close, a new one opens, cookies are gone, and the exit IP normally changes.
8. **Per-site circuits.** Open two different sites in MoltNet tabs and compare
   the exit IP each one sees (e.g. two different IP-echo sites): they should
   normally differ. In Tor's log with `SafeLogging 0` + `Log info`, streams for
   different sites carry different SOCKS usernames.
9. **Overrides are refused.** Start MoltBrowser with `--proxy-server=...` (or an
   extension/policy proxy): opening a MoltNet window must fail with an error.
10. **The relays are real.** *Copy route evidence* in the panel, then look up
    each fingerprint on metrics.torproject.org or
    `onionoo.torproject.org/details?lookup=<fingerprint>`.
11. **Onion sites.** In a MoltNet window, open an onion address both as
    `https://` and as `http://`: both must load, with no HTTPS-Only warning
    for `http://`. Open `https://www.torproject.org`: a bar must offer the
    onion version, and nothing may change until you click it. A test page
    served over `https` without the header but with `<meta
    http-equiv="onion-location" content="http://<v3 onion>/">` in its
    `<head>` must get the same bar; with two such elements, or with the
    element in `<body>`, no bar.
12. **No onion names outside Tor (regular window).** Run
    `sudo tcpdump -n -l port 53 | grep -i onion`, then type an onion address
    in a regular window's address bar and press Enter: the "Tor onion site"
    page must appear and tcpdump must print nothing. The button must open the
    address in a MoltNet window. With DNS-over-HTTPS on, no DoH request may be
    made for it (check `chrome://net-export`).
13. **No onion names to a proxy (regular window).** Start a logging proxy
    (e.g. `mitmdump -p 8888`) and MoltBrowser with
    `--proxy-server=http://127.0.0.1:8888` (MoltNet then refuses to open,
    see step 9; this step is about regular windows). Open a test page that
    loads `<img src="http://<onion>/x.png">`, calls
    `fetch("http://<onion>/")`, loads an image from a URL that redirects to
    an onion address, opens `new WebSocket("ws://<onion>/")` and does the
    same from a dedicated worker: the proxy must log none of them, and
    DevTools must show the requests failing with `ERR_BLOCKED_BY_CLIENT`.
14. **Bridges with your distribution's transports (Linux).** Install
    `obfs4proxy` (or `lyrebird`) and `snowflake-client`, paste obfs4 bridges
    from bridges.torproject.org under *Custom bridges*, and turn MoltNet on:
    `~/.moltbrowser/tor/torrc` must contain `ClientTransportPlugin obfs4 exec
    /usr/bin/...`, Tor's log (`~/.moltbrowser/tor/tor.log`) must show it
    using the bridge, and step 1 must pass. Repeat with snowflake bridges.
    Remove the package: settings must name the package to install, and
    turning MoltNet on must fail with that message rather than connect
    without bridges.

## 5. What MoltNet does *not* do

1. **It is not Tor Browser.** MoltNet windows report the same time zone
   (UTC), language (en-US), rounded screen size, CPU count and WebGL renderer
   for everyone, and canvas, WebGL and audio readback carry per-site noise
   (docs/FINGERPRINTING.md). But the window itself isn't letterboxed, fonts
   and the OS in the user agent are your real ones, and the protection hasn't
   been tested against real fingerprinting scripts yet. Assume sites may still
   recognise and link your visits. If your safety depends on not being
   identified, use Tor Browser.
2. **It doesn't hide who you are from sites you log into.**
3. **The exit relay can see unencrypted traffic.** HTTPS-Only mode warns you
   before plain HTTP; don't click through.
4. **It won't beat an observer who watches both ends** and correlates timing.
5. **Some sites block Tor**, and Tor is slower.
6. **Only MoltNet windows are covered.** Regular windows, incognito windows,
   browser background traffic (updates, component downloads, Safe Browsing
   lookups made by the browser itself), and apps opened from a MoltNet window
   (downloads opened externally, `mailto:` and other external links) do not go
   through Tor.
7. **Known gaps we have not closed yet:**
   - Browser-level services in MoltNet windows have not been audited
     individually: Safe Browsing's real-time hash-prefix checks, the AI side
     panel (cloud AI providers), and other features may make requests outside
     the MoltNet profile's network context.
   - Requests to `localhost` / loopback addresses bypass the proxy (they stay on
     your machine, but a page can probe local services).
   - If Tor stops, another local program could take over its old port; MoltNet
     windows would then talk to that program. Close MoltNet windows when you
     turn MoltNet off.
   - Linux packages use the distribution's Tor and its pluggable-transport
     packages, but don't include Tor Browser's built-in bridge list, so on
     Linux you paste bridges yourself. Older `snowflake-client` packages
     (Ubuntu 24.04 ships 2.5.1) may not understand every parameter of
     current snowflake bridge lines. The sandboxed Flatpak can't use a
     system Tor or its transports at all. The Windows packaging doesn't yet
     ship the pluggable transports either, so bridges there need a manual
     install.
   - If your regular windows use a proxy server, a WebSocket that a shared
     worker or service worker opens to an onion address still reaches that
     proxy: the browser can't tell which profile such a worker belongs to.
     Everything else to onion addresses is refused outside MoltNet.
   - Onion sites are not treated as secure contexts, so the address bar shows
     "Not secure" on `http://` onion sites and some web features stay off
     there. Doing this only in MoltNet windows needs engine changes that
     aren't made yet.
   - The Tor 15.0.15 SHA-256 pins in `scripts/bundle-tor.sh` are empty; release
     builds fail until they are filled in.
   - Bridges have been validated as configuration, and Tor has been seen
     starting the distribution's transports; connecting to the Tor network
     through snowflake/obfs4 has not been tested yet.

## 6. Privacy defaults for regular windows

Independently of MoltNet, regular profiles now start from more private
defaults. These are *defaults*: if you already chose a different value, yours
is kept, and you can change any of them in Settings.

- Third-party cookies blocked.
- WebRTC only uses the default public network interface (no local or
  VPN-hidden interface addresses in WebRTC candidates).
- Network prediction (prefetch / preconnect / prerender) off.
- Do Not Track off — sites ignore it and it is one more fingerprinting signal.
- HTTPS-First (balanced) mode on, once, for profiles that never changed the
  HTTPS-First setting.
- Connections partitioned by top-level site.
- The `Sec-CH-UA` brand list says "Chromium", like other Chromium-based
  browsers, instead of advertising MoltBrowser.

**Global Privacy Control** is on: every request carries `Sec-GPC: 1` and
`navigator.globalPrivacyControl` is true, in normal and MoltNet windows alike
(Chromium 152's implementation, which MoltBrowser enables by default in
`molt_feature_overrides.cc`; `chrome://flags/#enable-global-privacy-control`
turns it off). It is a browser-wide switch, so MoltNet windows send it too: it
tells sites that the user is a MoltBrowser (or Brave, or GPC-enabled) user,
which is a small, fixed trait shared by everyone using MoltBrowser.

## 7. Bottom line

MoltNet's new design keeps Tor browsing in separate windows with their own
identity, fails closed, and only talks to MoltBrowser's own, authenticated Tor.
It hides your IP address in those windows; it does not make you anonymous the
way Tor Browser does. The control-port layer has been tested against a real
Tor; the browser-level behaviour has not yet been tested on a build, and no
independent audit has been done. We will publish those results here when we
have them.

---

*The MoltBrowser team. Relay details come from the Tor Project's public
directory (onionoo / metrics.torproject.org). Leak-test sites are run by third
parties. MoltBrowser is not affiliated with or endorsed by the Tor Project.*
