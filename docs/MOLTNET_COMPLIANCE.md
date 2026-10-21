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
10. **Your settings stay out of MoltNet windows.** What a site can see in a
    MoltNet window doesn't depend on how you set up your regular browser.
    MoltNet uses MoltBrowser's defaults instead of your site permissions
    and site exceptions (including sites where you blocked JavaScript or
    turned Shields off), your defaults for permissions and content, your
    font and font-size settings, the default text encoding, Do Not Track,
    your third-party cookie choice, your ad privacy (Privacy Sandbox)
    choices, "Strip tracking parameters", and the accessibility settings a
    page can detect. A site that regular windows stopped from asking for a
    permission (after you dismissed it several times) can ask again.
    Permissions you grant and changes you make in a MoltNet window last
    until New Identity or until the last MoltNet window closes, and nothing
    done in a MoltNet window changes your regular settings.
11. **Your extensions and your regular browsing stay out.** Extensions you
    or an administrator installed don't run in MoltNet windows and can't
    see them, even when they're allowed in Incognito; only the browser's
    built-in extensions (the PDF viewer) work there. Settings an extension
    controls (proxy, privacy, fonts, ...) don't apply in MoltNet either.
    MoltNet also doesn't let sites see what your regular browsing taught
    the browser: sites you watch a lot don't autoplay with sound,
    `navigator.storage.persist()` doesn't reveal your bookmarks or the
    sites you use most, lookalike-domain warnings don't depend on the sites
    you use, and certificate warnings you clicked through in a regular
    window are shown again. Third-party cookies and Shared Storage are
    always off in MoltNet windows, and MoltNet never sends your Windows or
    Kerberos sign-in to a site.
12. **No WebRTC.** As in Tor Browser, pages in a MoltNet window can't open
    WebRTC connections: `RTCPeerConnection` reports that WebRTC is
    unavailable, and the browser gives those pages none of WebRTC's network
    access (no name lookups, no sockets, nothing announced on your local
    network). Video calls and other WebRTC sites don't work there; use a
    regular window for them.
13. **At least as private as Incognito.** Like an Incognito window, a
    MoltNet window adds nothing to your history, downloads list, saved
    passwords, autofill data or recently closed tabs, and its cookies,
    cache and site data never touch the disk. Chromium gives a few of its
    private-window protections to Incognito alone; MoltNet gets those
    too: DevTools opened on a MoltNet page keeps its settings (console
    history, breakpoints) for that session only; your lock screen and
    system media controls show "A site is playing media" instead of what
    a MoltNet page plays; on macOS, a page can't share files (they would
    be copied to your profile folder) and links you open from other apps
    go to a regular window, never into MoltNet; a page using your camera
    or microphone only switches to picture-in-picture by itself if you
    allowed that site; and on Windows, error pages don't offer Windows
    Network Diagnostics, which logs the page's address. A few Incognito
    touches that only change how the window looks are not copied (§5).

## 2. How it is built

**Isolation.** MoltNet windows run in a non-primary off-the-record profile
(`OTRProfileID::CreateUnique("MoltNet::Tor")`) with its own network context.
The Tor proxy and the leak-prevention settings below are written only to that
profile's in-memory preferences. Earlier versions wrote the proxy into the
regular profile and into the browser-wide (local state) settings; on startup
MoltBrowser now removes exactly those old values.

**Settings isolation.** An off-the-record profile normally reads every
setting it hasn't changed itself from the regular profile, and Incognito
also writes a few settings back into it (the third-party cookie mode, the
bookmarks bar, DevTools settings). Site permissions and other content
settings are stored the same way, so that is also how Incognito inherits
them. The MoltNet profile's settings store (`molt_net_isolation.h`) differs
in two ways: it writes nothing back into the regular profile, and when the
profile is created, every setting a page can observe
(`molt_net_isolation_policy.h`: the permission and content-setting
exceptions and defaults of every type, including permission embargo and
MoltShield's per-site switch; fonts and font sizes; default encoding; Do
Not Track; third-party cookie mode; languages; Privacy Sandbox switches;
the accessibility settings pages can detect) is set to MoltBrowser's
default in the profile's own in-memory store, so the regular profile's
value is never read, and changing it later has no effect on MoltNet.
MoltNet's own settings (the proxy and leak controls below, en-US) are
written on top. Settings an administrator's policy or an extension
controls still take precedence; for the proxy and the leak controls that
blocks MoltNet (see *Fail-closed*).

**Fail-closed.** The MoltNet profile's proxy is `socks5://127.0.0.1:<port>` with
no direct fallback, so if Tor stops, pages in MoltNet windows fail with a proxy
error instead of loading directly. A MoltNet window is only opened after (a) Tor
reports an established circuit, (b) the proxy and leak settings are confirmed
to be MoltNet's own (a policy or command-line flag that overrides them
blocks MoltNet with an error; settings controlled by extensions don't reach
MoltNet's profile at all, see below), and (c) the MoltNet profile's network
context itself confirms it would send requests to exactly that SOCKS5 endpoint.
MoltNet windows are never restored after a restart.

**Leak controls on the MoltNet profile.**

| Setting | Why |
|---|---|
| `socks5://` proxy | hostnames are resolved by Tor, not your DNS resolver |
| WebRTC off | `RTCPeerConnection` throws, and the browser gives MoltNet pages no WebRTC sockets, name lookups or local-network (mDNS) announcements; the WebRTC policy `disable_non_proxied_udp` (no UDP, STUN or local addresses) stays set underneath, whatever your regular windows use |
| Network prediction off | no DNS prefetch / preconnect outside the proxy |
| HTTPS-Only mode on | warns before any plain-HTTP page, which the exit relay could read or modify (not for `.onion`, which never leaves Tor) |
| Connections partitioned by top-level site | no socket reuse across sites; enables per-site circuits |
| No proxy bypass (`<-loopback>`) | `localhost`, loopback and link-local addresses go to Tor too, which refuses them: pages can't probe local services |

**WebRTC.** MoltNet turns WebRTC peer connections off, as Tor Browser
does (Chromium 152, read in the code and checked against stand-ins of its
interfaces, not yet tested in a built browser). Keeping WebRTC under
`disable_non_proxied_udp` was not enough. That policy leaves WebRTC only
TCP connections to the page's relay (TURN) servers, made through Tor, but
the *names* a page hands WebRTC (a relay or STUN server given by name, or
a peer's address added with `addIceCandidate()`) were looked up by the
network service's WebRTC socket manager with MoltBrowser's own DNS
resolver, outside Tor (`P2PSocketManager::GetHostAddress`; `.local` names
by multicast DNS on your local network). A page could make your DNS
resolver look up a name it chose, such as `<unique>.attacker.example`, and
whoever runs that name's DNS servers would see your resolver, often your
ISP's. The policy was also only applied by the page's own renderer
process; the socket manager gives any renderer that asks your network
interfaces and plain UDP sockets. Now, decided by the window being a
MoltNet window and nothing else (not the fingerprinting switch):

- *In the browser*, a MoltNet page never reaches the network service's
  WebRTC socket manager or its mDNS responder. It gets a stand-in that
  answers every name lookup with "not found", opens no socket and lists no
  network interface, and its mDNS connection is closed. This holds
  whatever the page's renderer process does.
- *In the page*, MoltNet's renderer processes are told so when they start,
  and `new RTCPeerConnection()` throws `NotSupportedError` ("No
  PeerConnection handler can be created, perhaps WebRTC is disabled?",
  Chromium's usual message) before it reads the configuration, so a page
  fails at once instead of waiting.

`disable_non_proxied_udp` stays set underneath. It is MoltNet's own
setting: it doesn't follow your regular windows' WebRTC setting, and if a
policy overrides it, MoltNet refuses to open (extension-controlled
settings don't apply in MoltNet). Regular and Incognito windows keep
WebRTC. The code is in `chrome/browser/molt_ai/tor/molt_net_webrtc.h`
(hooked into Chromium's frame interface binders and renderer command
lines) and Blink's `RTCPeerConnection::Create()`.

**Other ways a page could make the browser look up a name.** Checked in
the Chromium 152 code for MoltNet windows; none of them reaches your DNS
resolver. `<link rel=dns-prefetch>` and `<link rel=preconnect>` go to the
browser's preconnect service, which off-the-record profiles don't have,
so nothing happens (and with network prediction off, speculative prefetch
and its origin check don't run either). Early-hints preconnects,
WebSockets, `fetch()`, `navigator.sendBeacon()`, `<a ping>`, Reporting
API and Network Error Logging reports all go through the proxy, so the
name goes to Tor inside the SOCKS5 request. WebTransport refuses to
connect through a proxy before it looks anything up. Local Network Access
checks use the address of the connection itself (for a proxied one:
unknown) and look nothing up. The Network Information API reads values the
browser already has. Direct Sockets is only for isolated web apps and
Chrome Apps, which MoltNet windows can't host. Chromium's built-in proxy
exceptions (`localhost`, loopback and link-local addresses) are switched off
for MoltNet (`<-loopback>`): those requests go to Tor too, which refuses
them, so a MoltNet page can't reach or probe services on your machine or
your local network.

**Nothing from the regular profile (extensions and services).** The MoltNet
profile is an off-the-record profile of your regular one, and Chromium
treats every such profile like Incognito: extensions allowed in Incognito
run there, and several of the browser's per-profile services read the
regular profile's data. Whatever a page can observe that comes from there
would be the same in every MoltNet session, after every New Identity, and
would link MoltNet to your regular browsing. So:

- *Extensions.* Only MoltBrowser's component extensions (the PDF viewer)
  are let into MoltNet. Every other one (Web Store, unpacked, sideloaded or
  installed by policy, whatever its "Allow in Incognito" switch and whether
  it runs in *split* or *spanning* incognito mode) is treated as not
  allowed there: MoltNet renderers don't load it, so it has no content
  scripts and pages can't message it or load its web-accessible resources
  (which is how sites detect extensions); it gets no background page or
  service worker, extension pages, toolbar button, side panel, omnibox
  keyword or page override in MoltNet; no events about MoltNet tabs,
  windows, navigations, downloads, cookies, context menus or keyboard
  shortcuts reach it; the tabs and windows APIs and `chrome.debugger`
  don't show it MoltNet windows or pages; it can't open tabs in a MoltNet
  window or get `activeTab` access to one; MoltNet's requests, WebSockets
  and WebTransport are never handed to `webRequest` or
  `declarativeNetRequest`; and the settings it controls (proxy, privacy,
  fonts, ...) don't apply to MoltNet's profile. The decision is in
  `chrome/browser/molt_ai/tor/molt_net_isolation.h`; Chromium's extension
  code asks it through `ExtensionsBrowserClient::IsExtensionKeptOutOfContext`
  and a few hooks marked `MoltBrowser:`.
- *Services.* In MoltNet windows no site has high media engagement
  (autoplay with sound always needs a click or key press, the same for
  everyone); `navigator.storage.persist()` is never granted (it is
  otherwise granted for sites you bookmarked, installed or use often);
  lookalike-domain warnings compare against the built-in list of popular
  sites only, not the sites you use; the "block the JavaScript optimizer on
  unfamiliar sites" setting isn't applied (familiarity comes from your
  history); certificate-warning exceptions ("Proceed (unsafe)") are kept in
  memory for the MoltNet session that made them, and those made in regular
  windows don't apply; third-party cookies are blocked and the Privacy
  Sandbox's Shared Storage is off whatever your regular settings, as in
  Incognito; and the browser never answers a server's authentication
  challenge with your Windows (NTLM) or Kerberos credentials.
- *Checked and not inherited* (Chromium already keeps these per profile):
  HTTPS-First "Continue to site" exceptions, client hints, Storage Access
  and top-level storage access grants, FedCM grants and sign-in status,
  HSTS and the rest of the network state (MoltNet's network context is in
  memory and starts empty), visited-link colouring, media device IDs,
  persistent origin trials, and the extension `cookies` API (which only
  reaches the primary Incognito profile). Push messaging and notifications
  aren't available in off-the-record profiles.

**Nothing from the regular profile (Incognito's protections).** Chromium
has two kinds of off-the-record profile: the primary one, Incognito
(`Profile::IsIncognitoProfile()`), and non-primary ones (DevTools browser
contexts, captive-portal windows, and MoltNet's `MoltNet::Tor-<uuid>`).
Most private-mode behaviour keys on "off the record" and so covers both:
the in-memory storage partition (cookies, site data, HTTP and code cache,
media-license (CDM) storage), and every profile service, since in
Chromium 152 `ProfileSelections` treats all off-the-record profiles alike
(it no longer has a primary-only mode). But some protections test for
Incognito itself. Every use of `IsIncognitoProfile()`,
`IsPrimaryOTRProfile()`, `OTRProfileID::PrimaryID()` and the Incognito
profile type in Chromium 152's `chrome/browser` (156 lines in 91 files,
outside tests and the Android- and ChromeOS-only folders) was checked one
by one, plus the components that get the answer through a delegate (the
omnibox, page info, page-load metrics). Where MoltNet came out less
private than Incognito, it now gets Incognito's behaviour
(`chrome/browser/molt_ai/tor/molt_net_incognito_parity.h`, one hook per
place, marked `MoltBrowser:`):

- *Written to disk or into the regular profile.* DevTools opened on a
  MoltNet page ran in the regular profile, so its settings, including the
  console's command history and breakpoints with the page's script
  addresses, were saved in the regular profile's preferences file and
  outlived New Identity; it now runs in the MoltNet profile, whose settings
  are in memory. Cookies of extension origins would have gone to an
  "Extension Cookies" file in the regular profile's folder (an
  off-the-record profile's folder is its parent's), files a page shares
  on macOS (`navigator.share()`) were copied into that folder, and opening
  `chrome://gcm-internals` in a MoltNet window started a second push
  (GCM) client on the regular profile's store. Extension cookies are now
  kept in memory, file shares are refused after a short random delay
  (what Incognito does, so a page can't tell a refusal from a user
  cancelling), and MoltNet gets no GCM client.
- *Given to the operating system.* The system media controls (lock
  screen, MPRIS on Linux, Now Playing on macOS, the Windows media overlay)
  showed the title, artist or site, album and artwork of what a MoltNet
  page played; they now show a placeholder. On Windows, MoltNet error pages
  offered Windows Network Diagnostics, which logs the address it's run
  with; they no longer do.
- *Window behaviour.* On macOS a link opened from another application went
  to the last active window's profile, so it could land in MoltNet's Tor
  identity; it now opens in a regular window. Automatic picture-in-picture
  (a page using the camera or microphone floating itself over other
  windows when you switch tabs) needs the site to be allowed, as in
  Incognito. And when the last MoltNet window closes, print previews
  still spooling are dropped before the profile is discarded, as for
  Incognito.

Checked and already the same as Incognito (or stricter): history, top
sites, Journeys and history search, visited-link colours, the omnibox's
history, shortcuts and zero-suggest providers, the action predictor,
search-engine keywords learned from sites, favicons; downloads (not
persisted, not shown in regular windows, no source address in the file's
metadata); password and autofill save prompts, autocomplete entries,
address and card import; sync, send-to-your-devices and saved tab groups;
session and tab restore and the recently-closed list (MoltNet windows are
never restored); UKM (off while any off-the-record window is open) and
background tracing; the clipboard (copies from MoltNet pages are marked
to stay out of the system's clipboard history and cloud clipboard);
Handoff and AppleScript on macOS; screenshot protection (on the desktop
Chromium has it for managed data protection only, not for Incognito);
third-party cookies, Shared Storage, ambient authentication and
lookalike warnings (fixed earlier). MoltNet is
stricter than Incognito for HTTPS (HTTPS-Only instead of Incognito's
HTTPS-First), Accept-Language (a fixed en-US instead of your first
language), `prefers-color-scheme` (the uniform dark toolbar's rather than
your system's) and the settings Incognito writes back into the regular
profile (MoltNet writes none). As in Incognito, what you save on purpose
from a MoltNet window goes to your regular profile: bookmarks and
reading-list entries you add, and words you add to the spelling
dictionary. Saved passwords and addresses can be filled into MoltNet
pages when you pick them, as in Incognito; MoltNet never offers to save
new ones.

**Onion addresses.** RFC 7686 reserves `.onion` for Tor, and the Chromium
version MoltBrowser is built on doesn't special-case it, so MoltBrowser does,
in four places outside MoltNet's Tor connection: a navigation check that runs
before any other one stops `.onion` navigations in non-MoltNet windows (the
page shown instead is a browser interstitial, so a website can't press its
button, and the button only acts on a real click); every other request from
a non-MoltNet window (images, scripts, fetches, worker and service-worker
loads, prefetches, downloads, and WebSockets from pages and from dedicated,
shared and service workers) to an onion host, or redirected to one, is refused in the browser
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
or `PATH`. On macOS the bundled copies also carry their own libraries
(`Contents/Resources/ocr/lib/`) and load nothing from outside the app apart
from the operating system's; the build checks this
(`scripts/macho_bundle_deps.py`) and refuses to bundle a helper that would.

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
| Which extensions MoltNet admits (component only; an ID with no loaded extension is kept out), MoltNet's in-memory certificate exceptions | 6 unit tests (`molt_ai_unittests`); standalone harness with g++ ASan/UBSan and clang `-fno-exceptions`: the same tests, the Chromium glue against stand-ins with the 152 signatures (`StatefulSSLHostStateDelegate`, `Extension` locations, OTR profile ids), every hook expression, and 5 mutants all caught | Pass (built standalone; not yet built in the Chromium tree) |
| Extensions, autoplay, `persist()`, lookalike warnings, certificate exceptions, third-party cookies, Shared Storage, ambient authentication in a running browser (§4 steps 16-17) | Needs a Chromium build | **Not yet done** |
| `.onion` blocked and offered in regular windows, no DNS query, no onion request or WebSocket reaching a configured proxy, `Onion-Location` bar (header and `<meta>`), `http://` onion under HTTPS-Only, bridges connecting through the distribution's transports | Needs a Chromium build | **Not yet done** |
| Which settings MoltNet takes from MoltBrowser's defaults (§1.10): the list, prefix matching, look-alike names | 6 unit tests (`molt_ai_unittests`) | Pass (built standalone; not yet built in the Chromium tree) |
| MoltNet's settings store: the regular profile's values and later changes don't show, MoltNet's own values and policy still apply, nothing is written back (an Incognito-style store leaks both ways) | Unit tests (`molt_ai_unittests`), run against stand-ins of Chromium's settings classes; the hook that picks MoltNet's store, checked the same way | Pass (standalone stand-ins); the real-class tests need a Chromium build |
| Site settings with Chromium's real content-settings code: blocked JavaScript, a blocked permission, Shields off and a permission embargo in the regular profile, none of them in MoltNet; changes in a MoltNet window stay there | Unit tests (`molt_ai_unittests`) | Written; need a Chromium build |
| No WebRTC in MoltNet (§1.12): the decision (MoltNet profiles only, whatever fingerprinting protection says), the renderer switch, the stand-in socket manager (no name resolved, including relay names, `.local` names and IP literals; no socket for any socket type; an empty interface list; its connection kept open), the closed mDNS responder, other profiles keeping Chromium's own, and the four hooks in Chromium files | 6 unit tests (`molt_ai_unittests`); standalone harness with clang `-fno-exceptions` and g++ ASan/UBSan, with and without WebRTC support compiled in: the same logic, the Chromium glue and the hooks (taken verbatim from the changed files) against stand-ins with the 152 signatures, 13 mutants all caught | Pass (built standalone; not yet built in the Chromium tree) |
| The same over real mojo connections, and Blink's switch | Unit tests (`molt_ai_unittests`, `blink_unittests`) | Written; need a Chromium build |
| `RTCPeerConnection` throws in a MoltNet window and no relay or peer name is looked up (§4 step 18) | Needs a Chromium build | **Not yet done** |
| Which profiles get Incognito's protections (§1.13): MoltNet profile ids (look-alikes and Chromium's own off-the-record ids refused), the classification, and that every other profile keeps Chromium's answer | 5 unit tests (`molt_ai_unittests`); standalone with g++ ASan/UBSan and clang `-fno-exceptions` | Pass (built standalone; not yet built in the Chromium tree) |
| The hooks themselves (DevTools profile, media-controls metadata, network diagnostics, extension cookies, Web Share files, GCM and Instance ID, links from other apps, automatic picture-in-picture) and the print-preview clean-up | Standalone harness: each hook copied verbatim from the changed file and compiled against stand-ins with the 152 signatures (g++ ASan/UBSan, clang `-fno-exceptions`); MoltNet gets Incognito's result, and regular, Incognito, guest, DevTools, media-router and look-alike profiles get exactly 152's; 20 mutants all caught | Pass (built standalone; not yet built in the Chromium tree) |
| Incognito's protections in a running browser (§4 step 19) | Needs a Chromium build | **Not yet done** |
| Isolation, fail-closed behaviour, leak controls, settings isolation, New Identity, per-site circuits in a running browser | Needs a Chromium build | **Not yet done** |
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
9. **Overrides are refused.** Start MoltBrowser with `--proxy-server=...` (or a
   policy proxy): opening a MoltNet window must fail with an error. A proxy
   set by an extension doesn't reach MoltNet: with one installed and
   allowed in Incognito, MoltNet must open and step 1 must pass.
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
    same from a dedicated worker, a shared worker and a service worker: the
    proxy must log none of them, and
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
15. **Your settings don't follow you.** In a regular window, on a test
    site: block its location permission and its JavaScript, turn Shields off
    for it, dismiss its notification prompt three times; then in Settings
    turn on Do Not Track, set a large font size and a different standard
    font, and allow third-party cookies. In a MoltNet window, on the same
    site: JavaScript runs, `navigator.permissions.query({name:
    'geolocation'})` and `({name: 'notifications'})` say `prompt`,
    `navigator.doNotTrack` is `null` and requests carry no `DNT` header,
    text has the default size and font, a third-party frame can't read or
    set unpartitioned cookies, and MoltShield blocks the site's ads. Change
    a regular setting back while the MoltNet window is open: nothing changes
    in it. Allow a permission and turn Shields off in the MoltNet window:
    neither shows in a regular window, and both are gone after New Identity.
16. **Extensions stay out.** In a regular window install a test extension
    with a content script on `<all_urls>` that marks the page, a
    web-accessible resource, `externally_connectable` for `<all_urls>`,
    `tabs`, `webRequest`, `declarativeNetRequest` (a rule that blocks
    some test URL), `debugger` and a keyboard shortcut, whose background
    logs `chrome.tabs.query({})`, `tabs.onUpdated`, `windows.onFocusChanged`,
    `webRequest.onBeforeRequest` and `chrome.debugger.getTargets()`. Allow
    it in Incognito. In a MoltNet window: the page is not marked,
    `fetch("chrome-extension://<id>/<resource>")` fails,
    `chrome.runtime` is undefined in the page, the blocked URL loads, the
    toolbar shows no extension, the shortcut does nothing, and the logs
    show no MoltNet tab, window id, URL, request or target (focusing a
    MoltNet window logs `-1`). A primary Incognito window must still behave
    as before. Repeat with `"incognito": "split"`. A PDF opens in MoltNet's
    PDF viewer. `chrome-extension://<id>/page.html` typed in a MoltNet
    window shows a blocked page.
17. **No regular-profile carry-over.** In a regular window, watch a video
    with sound on a site until `chrome://media-engagement` lists it as
    high; on that site in a MoltNet window,
    `navigator.getAutoplayPolicy("mediaelement")` must be `"allowed-muted"`.
    Bookmark a site in a regular window; there, in MoltNet,
    `await navigator.storage.persist()` must be `false`. Click through a
    self-signed certificate warning in a regular window; the same site in
    MoltNet must warn again. With third-party cookies allowed in regular
    settings, a cross-site iframe in MoltNet must not get its cookies, and
    `sharedStorage.set()` must reject.
18. **No WebRTC, and no name lookups for it.** Pick two names nobody has
    looked up before (e.g. `wrtc-<random>.example.com`, `<name1>` and
    `<name2>` below). Watch DNS: a logging resolver (e.g. `dnsmasq
    --log-queries`) as your system resolver, or
    `sudo tcpdump -n -l 'port 53 or port 5353'`. In a MoltNet window, open
    a test page with this `<script type="module">`:

    ```js
    try {
      const pc = new RTCPeerConnection({iceServers: [{
          urls: "turn:<name1>:3478?transport=tcp",
          username: "u", credential: "c"}]});
      pc.createDataChannel("x");
      await pc.setLocalDescription(await pc.createOffer());  // TURN lookup
      const peer = new RTCPeerConnection();
      await peer.setRemoteDescription(pc.localDescription);
      await peer.setLocalDescription(await peer.createAnswer());
      for (const host of ["<name2>", crypto.randomUUID() + ".local"]) {
        await peer.addIceCandidate({sdpMid: "0",
            candidate: `candidate:1 1 udp 2122260223 ${host} 9 typ host`});
      }
      console.log("WebRTC available");
    } catch (e) { console.log(e.name, e.message); }
    ```

    The console must show `NotSupportedError No PeerConnection handler can
    be created, perhaps WebRTC is disabled?`, and neither name, nor any
    `.local` name, may appear in the resolver's log or in tcpdump; tcpdump
    must show no UDP from the browser other than to the Tor guard (or
    bridge). browserleaks.com/webrtc must show no IP address at all (it
    can't create a connection).
    Run the same page in a regular window: it prints `WebRTC available`
    and both names are looked up by your resolver, as in any Chromium
    (that is the leak MoltNet closes).
19. **Incognito's protections.** In a MoltNet window:
    - Open DevTools on a page, run `"moltnet-devtools-check"` in the
      console and set a breakpoint in one of the page's scripts. Close
      every MoltNet window and quit MoltBrowser: `grep -c
      moltnet-devtools-check "<profile folder>/Preferences"` prints `0`,
      and the page's script address isn't in that file either. In a
      regular window, DevTools' console history (arrow up) doesn't
      contain the command.
    - Play a video or song on a site that sets its title and artwork
      (`navigator.mediaSession.metadata`). The lock screen and the system
      media controls (`playerctl metadata` on Linux, Now Playing in the
      macOS Control Centre, the Windows media overlay) show "A site is
      playing media" and no title, artist, site or artwork. A regular
      window still shows them.
    - macOS: with the MoltNet window in front, run `open
      https://example.com` in Terminal (or click a link in Mail): it opens
      in a regular window. `navigator.share({files: [new File(["x"],
      "x.txt")]})` on a MoltNet page rejects with `AbortError` after one
      to two seconds, and no `WebShare` folder appears in the profile
      folder.
    - A page that shows your camera and registers for automatic
      picture-in-picture (`navigator.mediaSession.setActionHandler(
      "enterpictureinpicture", ...)`) doesn't open picture-in-picture
      when you switch tabs (in a regular window it does, with a prompt).
    - `chrome://gcm-internals` shows no GCM client, and the profile's
      `GCM Store` folder isn't changed.
    - Windows: an error page (e.g. `https://does-not-exist.invalid`) has
      no "Running Windows Network Diagnostics" link.
    - Print a page, close the MoltNet window while it's printing: no
      crash, and the job finishes or is cancelled.

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
   - Context-menu items that a split-mode extension's Incognito copy adds
     can still appear in a MoltNet page's menu; choosing one does nothing
     (the extension gets no event). Likewise an extension's keyboard
     shortcut is still caught in a MoltNet window (the page doesn't get
     those keys) and does nothing. An event the browser
     only dispatches after a MoltNet profile is gone falls back to
     Chromium's Incognito rules (events are normally sent while it exists).
   - Every Chromium check that gives Incognito alone a protection has been
     reviewed, and MoltNet gets the protection wherever it was less
     private (§2). What it still doesn't copy from Incognito only changes
     the browser's own interface: MoltNet windows don't use Incognito's
     dark window colours or "Incognito" labels (they carry the MoltNet
     button instead); the app menu's profile entry shows your regular
     profile (name, picture, sync status); closing the last MoltNet window
     doesn't warn that downloads in progress will be cancelled, and the
     "relaunch to update" warnings don't count MoltNet windows; *Clear
     browsing data* opens your regular profile's settings (New Identity is
     what clears MoltNet). Policies an administrator or parent sets for
     Incognito don't cover MoltNet windows: turning Incognito off, the
     Incognito-only URL blocklist, extensions required for Incognito
     browsing, and closing a supervised user's Incognito windows (forcing
     Incognito does stop MoltNet from opening).
   - Local servers don't open in MoltNet windows: `localhost`, loopback
     and link-local addresses go to Tor, which refuses them (see §2). Use
     a regular window for a local development server.
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
   - Onion sites are not treated as secure contexts, so the address bar shows
     "Not secure" on `http://` onion sites and some web features stay off
     there. Doing this only in MoltNet windows needs engine changes that
     aren't made yet.
   - The Tor 15.0.15 SHA-256 pins in `scripts/bundle-tor.sh` are empty; release
     builds fail until they are filled in.
   - Bridges have been validated as configuration, and Tor has been seen
     starting the distribution's transports; connecting to the Tor network
     through snowflake/obfs4 has not been tested yet.
   - Settings that apply to the whole browser rather than to your regular
     profile still reach MoltNet windows (§1.10 covers the profile's):
     page colours and your system's high-contrast and accessibility
     settings, MoltShield's on/off switch and its filter lists, and the
     language of MoltBrowser's own interface, which picks MoltBrowser's
     default fonts and text encoding. `prefers-color-scheme` is dark for
     everyone while MoltBrowser's dark toolbar is on (the default); with it
     off, MoltNet windows follow your regular windows' theme.

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
