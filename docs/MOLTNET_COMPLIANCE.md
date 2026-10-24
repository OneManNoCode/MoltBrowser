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
   window closes. Its connections use Tor circuits of its own: the next
   MoltNet windows never reuse them, also for a site both visit (§2
   *Circuits*). Within one identity, what a site leaves in MoltNet's
   network state (cache, connections, certificate checks, sign-ins, HSTS)
   stays with that site (§2 *Shared network state*). Pages visited in
   MoltNet windows are not added to MoltBrowser's on-device memory index.
3. **New Identity.** Ends the current identity: from that moment its
   network context refuses every new request, and its MoltNet windows
   close, with any DevTools window open on their pages. Nothing open in a
   window keeps it open: not a "Leave site?" prompt, a sign-in prompt, an
   alert, print preview, a download's "Save as" dialog or a file chooser.
   A window still open two seconds later is closed by force (its pages
   are stopped). New Identity drops the old connections and DNS cache, and
   waits until the MoltNet profile is discarded (fresh cookies, cache and
   storage), so nothing of the old identity can still send a request. Only
   then it asks Tor for new circuits (`SIGNAL NEWNYM`) and waits until Tor
   confirms that it has switched, before it opens a fresh MoltNet window.
   Tor switches at most once every ten seconds, so a New Identity soon after
   another one (or after changing the exit country) takes up to ten seconds.
   If Tor rejects the request or doesn't confirm it, or a window of the old
   identity doesn't close, you see an error and no new window opens.
   Whatever happens with that request, the next MoltNet window doesn't use
   the old identity's circuits (§2 *Circuits*).
4. **One circuit per site.** Each MoltNet connection tells Tor which top-level
   site it belongs to (as the SOCKS username), so Tor puts different sites on
   different circuits and exits (`IsolateSOCKSAuth`). The SOCKS password
   is random and new for each MoltNet identity, so a site gets new circuits
   with each one. A site's embedded
   third-party content shares that site's circuit, as in Tor Browser. A page
   its server sandboxes (an opaque origin) gets a circuit of its own, and so
   does each download the browser makes while it checks a site's
   certificate (a missing intermediate certificate, a revocation check):
   such a fetch is never cached, never shared with another site's, and
   never put on a site's circuit.
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
    choices, "Strip tracking parameters", the accessibility settings a
    page can detect, and whether you hid the warning Chromium shows on
    pages that use XSLT. A site that regular windows stopped from asking for a
    permission (after you dismissed it several times) can ask again, and a
    site you let open an application without asking ("Always allow") in a
    regular window asks again in a MoltNet window.
    Permissions you grant and changes you make in a MoltNet window last
    until New Identity or until the last MoltNet window closes, and nothing
    done in a MoltNet window changes your regular settings. Nor does the
    language of MoltBrowser's interface show: text the browser puts in
    pages (form messages such as "Please fill out this field.", "Choose
    File", the date picker) is in US English in MoltNet windows, and the
    default fonts and text encoding are the same for everyone on your
    operating system. Your languages don't show either, nor languages an
    administrator sets: whatever your language settings, the
    `ForcedLanguages` and `ReduceAcceptLanguageEnabled` policies or the
    `--accept-lang` flag say, MoltNet pages get en-US in
    `navigator.languages` and `Accept-Language` (with fingerprinting
    protection off, the default list for MoltBrowser's interface
    language).
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
    window are shown again. Third-party cookies are always off in MoltNet
    windows (Shared Storage: removed upstream at 156, so no window has
    it). MoltNet never sends your Windows or
    Kerberos sign-in to a site, and a site that asks for a password gets
    the Basic or Digest kind of sign-in only, never Windows (NTLM) or
    Kerberos (Negotiate) sign-in, also with a name and password you type
    in.
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
    allowed that site; on Windows, error pages don't offer Windows
    Network Diagnostics, which logs the page's address; and no WebRTC
    logs are kept (they would be saved in your profile folder and
    uploaded). A few Incognito touches that only change how the window
    looks are not copied (§5). MoltNet also follows the settings that
    decide whether private windows may open at all: when an administrator
    or parental controls turn Incognito off, or replace it with Isolated
    Mode, MoltNet doesn't open either, and closes if it is open; when they
    allow Incognito windows only, MoltNet still opens. Incognito's URL
    allow and block lists apply in MoltNet windows too (§2 *Incognito's
    availability*).
14. **Nothing a MoltNet window does goes around Tor.** What the browser
    does for a MoltNet window goes through Tor too, or doesn't happen.
    The AI chat in a MoltNet window uses on-device models only: a cloud AI
    provider would get your API key, which says who you are, with every
    page you ask about. Pages can't use the built-in AI APIs
    (`LanguageModel`, `Summarizer`, `Writer`, `Rewriter`, `Translator`,
    `LanguageDetector`): they report "unavailable" in MoltNet windows
    ([design](design/WEB_AI_APIS.md)). Translating a page ("Translate this
    page") is off in MoltNet windows. A translated page can see the
    language you read, which MoltNet hides behind its uniform en-US, so
    Translate offers nothing there. If you open it from the menu, it says
    why, and it won't translate the page
    ([design](design/PAGE_TRANSLATION.md) §5). The chat's sandbox tab opens in a
    MoltNet window, its actions and links stay in its own window, and it
    can't set up page
    watchers or saved plans (they would run later outside Tor). Safe
    Browsing, Google's image descriptions, location, Cast, signing in with
    a passkey on your phone and shopping features (price insights,
    discounts) are off in MoltNet windows, and bookmarks you open there
    with "Open all" stay there. Autofill doesn't ask Google's Autofill
    server about the forms on MoltNet pages or send it anything, and
    neither Autofill nor a page's payment sheet downloads address formats:
    fields are recognised on your device, and your saved addresses and
    passwords still fill. MoltNet doesn't check whether your network
    wants you to sign in first (a captive portal, as on hotel or airport
    Wi-Fi) and never opens such a sign-in page: Tor can't reach it, so sign
    in to the network in a regular window. A site that asks for a TLS
    client certificate (a smart card, an ID card, a company certificate)
    gets none in a MoltNet window, at once and the same way on every site:
    MoltNet never shows the certificate picker, never sends a certificate
    and remembers no choice (§2 *Shared network state*). MoltNet's
    connection never probes your secure-DNS (DNS-over-HTTPS) provider
    either. What still happens outside Tor is listed in §5.7.

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
(`molt_net_pref_isolation_policy.h`: the permission and content-setting
exceptions and defaults of every type, including permission embargo and
MoltShield's per-site switch; fonts and font sizes; default encoding; Do
Not Track; third-party cookie mode; languages; Privacy Sandbox switches;
the accessibility settings pages can detect; the sites allowed to open an
application without asking; "Use a web service to help resolve navigation
errors", which switches on captive-portal checks; and, new at Chromium
156, "Never show again" on the XSLT deprecation banner Blink puts into
pages, the Global Privacy Control setting and the AtMemory keyboard
shortcut, whose keypress the page then doesn't receive) is set to MoltBrowser's
default in the profile's own in-memory store, so the regular profile's
value is never read, and changing it later has no effect on MoltNet.
MoltNet's own settings (the proxy and leak controls below) are written on
top. Its languages (en-US) sit one level higher, in a store above the
profile's own settings (the level where Chromium keeps the settings
extensions control, which MoltNet doesn't take;
`molt_net_language_policy.h`), because Chromium rewrites a profile's
language list itself: whenever it reads a profile's language settings
(after each page's language detection, for one), it rebuilds
`Accept-Language` from the `ForcedLanguages` policy and your chosen
languages, which an off-the-record profile reads from the regular one
(and `--accept-lang` sets). None of that reaches the level above, so
MoltNet pages get the same list whatever your settings and policies say.
MoltNet also ignores the `ReduceAcceptLanguageEnabled` policy and behaves
as without it (`chrome_content_browser_client.cc`). Settings an
administrator's policy controls still take precedence; for the proxy and
the leak controls that blocks MoltNet, and a policy that arrives while
MoltNet is open ends it (see *Fail-closed*).

**Fail-closed.** The MoltNet profile's proxy is `socks5://127.0.0.1:<port>` with
no direct fallback, so if Tor stops, pages in MoltNet windows fail with a proxy
error instead of loading directly. A MoltNet window is only opened after (a) Tor
reports an established circuit, (b) the proxy and leak settings are confirmed
to be MoltNet's own (a policy or command-line flag that overrides them
blocks MoltNet with an error, and so does the ProxyOverrideRules policy,
whose rules would apply on top of MoltNet's proxy and could send some
destinations elsewhere or DIRECT; settings controlled by extensions don't
reach MoltNet's profile at all, see below), and blob URLs are partitioned
by site (a policy that turns that off, PartitionedBlobUrlUsage, blocks
MoltNet too), (c) the MoltNet profile's network
context itself confirms it would send requests to exactly that SOCKS5
endpoint, and (d) the browser-wide switches that keep sites' connections,
HTTP cache and third-party storage apart are on (*Shared network state*).
MoltNet windows are never restored after a restart.

These checks also hold while MoltNet is open, not only when a window
opens. Of what (b) checks, policies can change while the browser runs: a
cloud policy refresh, a changed Group Policy, configuration profile or
policy file reaches MoltNet's profile at once, because its settings store
shares the regular profile's policy store. Chromium would then hand a
policy's proxy settings to MoltNet's network context, where they outrank
MoltNet's own Tor proxy, and override rules would apply on top of it. So:

- MoltNet's network context only ever gets MoltNet's own proxy
  configuration, exactly the SOCKS5 proxy MoltNet set, with no bypass and
  no override rules. Any other one (a policy's proxy, override rules, the
  system's settings, none at all) is never sent to it: it keeps routing
  to Tor. A network context that hasn't had MoltNet's configuration yet
  starts with none, and its requests wait for it
  (`MoltNetRouting::ProxyConfigFor()`, asked by Chromium's
  `ProxyConfigMonitor`). Once its identity has ended (below, and New
  Identity), the network context gets a configuration whose only route
  is an empty proxy list: every request it is then asked to make fails
  before it connects (`ERR_NO_SUPPORTED_PROXIES`), `localhost` included,
  and no later configuration replaces it.
- MoltNet watches every setting (b) reads (the proxy, proxy override
  rules, WebRTC IP handling, network prediction, HTTPS-Only mode, Safe
  Browsing, blob URL partitioning) and Incognito's availability (below),
  in its own profile and in the regular profile it was made from, and
  checks again whenever one changes. If MoltNet would now refuse to open,
  it ends that identity as New Identity ends the old one: its network
  context refuses every request from that moment, it drops its
  connections, its windows and the DevTools windows open on its pages
  close whatever they show (§1.3; a window still open two seconds later
  is closed by force, its pages stopped, and again up to five times), and
  its profile is discarded. It opens no new window, and a dialog gives
  the reason, with the same message as when MoltNet refuses to open. A
  window that was about to open checks again before it appears
  (`molt_net_live_guard.h`, `molt_net_live_policy.h`,
  `molt_net_window.cc`).
- What can't change while the browser runs: command-line flags
  (`--proxy-server` and the like) and the switches (d) checks (they are
  `base::Feature`s, fixed at startup). A change only the regular profile
  sees, such as an extension taking over its proxy, doesn't reach
  MoltNet's settings and leaves MoltNet open, as it would let MoltNet
  open. Tor stopping is not a settings change: pages fail with a proxy
  error, as above.

What this guarantees for requests in flight at that moment: none of them
leaves outside Tor. A request already sent finishes or fails on its Tor
connection (MoltNet drops its connections at once, and the rest go with
the profile). From the moment the identity ends, its network context
refuses new requests, so one a page makes while its window closes (a
`pagehide` beacon, a keepalive fetch) fails instead of going out. The
refusal reaches the network service as any configuration change does: a
request that got there first still goes out, through Tor. What it can't
guarantee: that nothing at all is sent after the change. What a server
already received can't be recalled. MoltNet also reacts when Chromium
applies a policy, not when an administrator sets it: a cloud policy
arrives at its next refresh, a policy file once Chromium has read it.

**Incognito's availability.** A MoltNet window is a private window, so
it opens only where an Incognito window may. MoltNet asks Chromium's
`IncognitoModePrefs::GetIncognitoModeType()` about the regular profile,
which combines the `IncognitoModeAvailability` policy (for the machine
or the profile; the deprecated `IncognitoEnabled` maps to it), a
supervised user's settings, Windows parental controls and Chromium 156's
Isolated Mode setting:

| Setting | Incognito | MoltNet |
|---|---|---|
| Incognito available (`IncognitoModeAvailability` 0, the default) | opens | opens |
| Incognito off (`IncognitoModeAvailability` 1, `IncognitoEnabled` false, a supervised user, Windows parental controls) | doesn't open | doesn't open: "Incognito is turned off (IncognitoModeAvailability or parental controls), and MoltNet windows follow it". If the setting changes while MoltNet is open, MoltNet ends (*Fail-closed*) |
| Incognito off, with `IncognitoModeUrlAllowlist` | opens; only the listed sites load | the same |
| Incognito only (`IncognitoModeAvailability` 2) | the only window that opens | opens (regular windows don't) |
| Isolated Mode replaces Incognito (Chromium 156) | an Isolated Mode window instead | doesn't open: "Your administrator replaced Incognito with Isolated Mode, and MoltNet windows follow it". Chromium reads this setting once per run, so it can't change while MoltNet is open |
| `IncognitoModeUrlBlocklist`, `IncognitoModeUrlAllowlist` | apply | apply |

The open check reads it with the other settings ((b) above), and the
live check watches the availability setting. Two Chromium checks treated
MoltNet's profile unlike Incognito's and now don't: which windows may
open (`IncognitoModePrefs::CanOpenBrowser()`, which refused MoltNet's
windows while Incognito was forced and let them open while it was off),
and which URL lists apply (`chrome_policy_blocklist_service_factory.cc`,
which applied the general URL lists to MoltNet but left Incognito's
out).

**Leak controls on the MoltNet profile.**

| Setting | Why |
|---|---|
| `socks5://` proxy | hostnames are resolved by Tor, not your DNS resolver |
| WebRTC off | `RTCPeerConnection` throws, and the browser gives MoltNet pages no WebRTC sockets, name lookups or local-network (mDNS) announcements; the WebRTC policy `disable_non_proxied_udp` (no UDP, STUN or local addresses) stays set underneath, whatever your regular windows use |
| Network prediction off | no DNS prefetch / preconnect outside the proxy |
| HTTPS-Only mode on | warns before any plain-HTTP page, which the exit relay could read or modify (not for `.onion`, which never leaves Tor) |
| Connections partitioned by top-level site | no socket reuse across sites; enables per-site circuits |
| No proxy bypass (`<-loopback>`) | `localhost`, loopback and link-local addresses go to Tor too, which refuses them: pages can't probe local services |
| Safe Browsing off | its download and password-reuse pings and list checks would leave outside Tor (see *Requests outside Tor* below) |

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
the Chromium 152 code for MoltNet windows, and each one again in the
Chromium 156 code (with the new 156 paths in the next paragraph); none of
them reaches your DNS resolver. `<link rel=dns-prefetch>` and `<link
rel=preconnect>` go to the browser's preconnect service, which off-the-record profiles don't have,
so nothing happens (and with network prediction off, speculative prefetch
and its origin check don't run either). Early-hints preconnects,
WebSockets, `fetch()`, `navigator.sendBeacon()`, `<a ping>`, Reporting
API and Network Error Logging reports all go through the proxy, so the
name goes to Tor inside the SOCKS5 request. WebTransport refuses to
connect through a proxy before it looks anything up. Local Network Access
checks use the address of the connection itself (for a proxied one:
unknown) and look nothing up. The Network Information API reads values the
browser sends; MoltNet's renderers get fixed ones (*Shared network
state*, network quality). Direct Sockets is only for isolated web apps and
Chrome Apps, which MoltNet windows can't host. Chromium's built-in proxy
exceptions (`localhost`, loopback and link-local addresses) are switched off
for MoltNet (`<-loopback>`): those requests go to Tor too, which refuses
them, so a MoltNet page can't reach or probe services on your machine or
your local network.

**New network paths at Chromium 156.** Between 152 and 156 Chromium added
or changed these ways to resolve a name or open a connection. Each one was
read in the 156 code for MoltNet windows (not yet tested in a built
browser):

| Path (Chromium 156) | For a MoltNet window |
|---|---|
| A target network for a tab's requests and WebSockets (`ContentBrowserClient::MaybeSetTargetNetwork()`, the target network on the URL loader factory and on each WebSocket) | Only tabs created with a target network have one: Android's multi-network Custom Tabs. Desktop tabs never do. A target network only picks the interface; the request still goes to the proxy |
| The target address space a renderer sends with each WebSocket | Feeds the Local Network Access check only: it changes no route and looks nothing up. The WebSocket goes through MoltNet's context and Tor |
| The renderer-accessible HTTP cache (`NetworkContext::RegisterHttpCacheClient()`) | Only for an on-disk cache with the SQL backend, never for a storage partition kept in memory. MoltNet's is in memory, so it has none |
| New QUIC hints (`AddWildcardQuicHints()`, `SetTryQuicByDefault()`) | No browser code calls them. QUIC never runs behind a SOCKS5 proxy: the attempt fails before it looks anything up |
| The HTTP stream pool (Happy Eyeballs v3) | Direct connections only; all of MoltNet's go to its proxy |
| Device-bound sessions (on by default on Windows and macOS) | Off in MoltNet's network context: sites use their ordinary cookies, as in Tor Browser. A session binds a site's cookies to a key the operating system's secure hardware (a TPM, the Secure Enclave) makes, so whether a site can set one up differs between machines, and the key isn't kept in MoltNet's memory. (Its requests, also 156's proactive refresh, would go through MoltNet's context) |
| Connection-Allowlist embedded enforcement (a base feature on by default at 156, and an origin trial) | Only narrows what a framed document may connect to; it sends the embedder's requirement in the frame's own request, through MoltNet's context, and looks nothing up |
| Background Fetch's Local Network Access check (`BackgroundFetchLocalNetworkAccess`, on at 156) | Background Fetch requests get a URL loader factory from the service worker's storage partition, MoltNet's for a MoltNet page, so they go through Tor; the new check reads the URL or the connection's address only |
| Prefetch, preconnect and prerender | Every prefetch the browser makes checks the network-prediction setting first, which MoltNet turns off (156 adds a way past the data- and battery-saver checks, not past that one). The search-engine preconnector also runs in off-the-record profiles but checks the same setting; search prefetch and the new search preloading check it or don't run off the record. 156's direct preconnect path only skips a pre-resolve, and off-the-record profiles still have no preconnect service for pages' hints. A navigation's own preconnect goes to the proxy |
| Local Network Access | 156 changes only its logging. The checks read the URL or the connection's address and look nothing up |
| Proxy routes from enterprise provisioning domains (`EnableDynamicRouteFetching`, on in Chromium's field-trial testing config) | Only a regular profile's proxy settings can get them: an off-the-record profile has no provisioning-domain service |
| Proxy override rules (the `ProxyOverrideRules` policy) | MoltNet doesn't open while they are set (*Fail-closed*) |
| DNS-over-HTTPS probes | MoltNet's network context starts none (next paragraph) |
| Connect prediction for address sorting (`ConnectPredictor`) | Only for names resolved locally, which MoltNet pages' names never are |

**DNS-over-HTTPS probes.** When secure DNS has servers (yours, an
administrator's, or the automatic upgrade of a system resolver that offers
DNS-over-HTTPS), Chromium's network service probes them for every network
context it creates and again after each network change. A probe is a DoH
request sent with the proxy bypassed, after the DoH server's own name has
been looked up with the system resolver; from MoltNet's context that would
be a connection from your own address each time a MoltNet identity
starts. MoltNet's network context starts no probes
(`molt_net_network_context_policy.h`, read in `network_context.cc`). It
never needs DoH: Tor resolves MoltNet's names.

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
  windows don't apply; third-party cookies are blocked whatever your
  regular settings, as in Incognito (the Privacy Sandbox's Shared Storage,
  which MoltNet also switched off: removed upstream at 156); and the
  browser never answers a server's authentication
  challenge with your Windows (NTLM) or Kerberos credentials. MoltNet's
  network context goes further: it offers servers the Basic and Digest
  authentication schemes only, never NTLM or Negotiate (Kerberos), even
  with credentials you type in, whatever the browser-wide settings and
  policies allow (Tor Browser doesn't offer them either). So Negotiate
  never looks a server's name up with your system's resolver, and
  nothing about your computer goes into an authentication answer
  (`molt_net_network_context_policy.h`; the network service applies it
  in `network_context.cc`).
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
From Chromium 156 a policy can make the primary one an *Enterprise
Isolated Mode* session instead of Incognito
(`IsEnterpriseIsolatedModeProfile()`; `IsPrimaryOTRProfileWithRegularParent()`
is true for either). Most private-mode behaviour keys on "off the record"
and so covers MoltNet too: the in-memory storage partition (cookies, site
data, HTTP and code cache, media-license (CDM) storage), and every profile
service, since `ProfileSelections` treats all off-the-record profiles
alike (it has no primary-only mode; 156's Isolated Mode setting applies to
that session alone). But some protections test for the primary profile
itself. Every use of `IsIncognitoProfile()`, `IsPrimaryOTRProfile()`,
`IsPrimaryOTRProfileWithRegularParent()`, `IsEnterpriseIsolatedModeProfile()`,
`OTRProfileID::PrimaryID()` and the Incognito profile type in Chromium 156's
`chrome/browser` (229 lines in 104 files, outside tests and the Android-
and ChromeOS-only folders; 158 lines in 95 files at 152, counted the same
way) was checked, plus the components, `content/` and `extensions/` code
that gets the answer through a delegate (the omnibox, page info,
page-load metrics, translate, password protection), and every 156 change
that adds or removes an off-the-record check. The first audit was at
Chromium 152; at 156 every site that is new or changed was checked again
(below, *Re-checked at Chromium 156*), and each 152 site that is still
there was confirmed to be still hooked or still rightly left alone. Where
MoltNet came out less private than Incognito, it now gets Incognito's
behaviour (`chrome/browser/molt_ai/tor/molt_net_incognito_parity.h`, one
hook per place, marked `MoltBrowser:`; where Chromium's check is
`IsPrimaryOTRProfileWithRegularParent()`, the hook keeps it and adds
MoltNet, so Isolated Mode keeps upstream's behaviour):

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
  cancelling), and MoltNet gets no GCM client. WebRTC text logs (for an
  extension's logging API, or a site an administrator allowed to collect
  them) would have been written to the regular profile's "WebRTC Logs"
  folder and uploaded through the browser-wide connection, outside Tor;
  as in Incognito, MoltNet keeps none (from the 156 audit).
- *Given to pages.* Chromium's Reduce Accept-Language negotiation (off by
  default, behind a flag) picks the language it sends a site from your
  language list. Incognito uses a generic list built from your first
  language alone, the same list its `Accept-Language` header uses; MoltNet
  sends that header too, and now negotiates from it as well, not from
  every language you set (from the 156 audit).
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
third-party cookies, ambient authentication and lookalike warnings
(fixed earlier; Shared Storage: removed upstream at 156). MoltNet is
stricter than Incognito for HTTPS (HTTPS-Only instead of Incognito's
HTTPS-First), Accept-Language (a fixed en-US instead of your first
language), `prefers-color-scheme` (dark for everyone while MoltBrowser's
dark toolbar is on, the default, where Incognito follows your system; see
§5.7) and the settings Incognito writes back into the regular
profile (MoltNet writes none). As in Incognito, what you save on purpose
from a MoltNet window goes to your regular profile: bookmarks and
reading-list entries you add, and words you add to the spelling
dictionary. Saved passwords and addresses can be filled into MoltNet
pages when you pick them, as in Incognito; MoltNet never offers to save
new ones.

*Re-checked at Chromium 156.* Upstream changed about 50 of these checks
from `IsIncognitoProfile()` to `IsPrimaryOTRProfileWithRegularParent()`,
added Isolated Mode checks, and added a few new sites. For MoltNet both
predicates are false, so a changed check behaves as it did at 152; each
was decided again all the same:

- *Hooked, unchanged:* the media-controls placeholder, Windows Network
  Diagnostics, automatic picture-in-picture, macOS file sharing,
  third-party cookies, bookmarks opened from a MoltNet window (and their
  metrics) and the shopping tab features all sit on a changed check;
  their hooks keep upstream's check and add MoltNet. The extension
  cookies, GCM, DevTools and macOS link hooks sit on checks 156 left as
  they were.
- *Newly hooked:* WebRTC text logs (`webrtc_logging_controller.cc`) and
  Reduce Accept-Language (`reduce_accept_language_factory.cc`), above.
  Both checks were Incognito-only at 152 too; the re-check gave MoltNet
  Incognito's behaviour.
- *Already the same for MoltNet:* the device attributes API (only for
  isolated web apps an administrator or developer installed, which
  off-the-record profiles can't reach: they have no web-app registry on
  the desktop); managed configuration (now any
  off-the-record profile); HTTPS-First in Incognito (MoltNet has
  HTTPS-Only); the print previews dropped when the last window closes
  (MoltNet does that itself); muting a site from the tab strip (writes to
  MoltNet's own in-memory site settings); the `/.well-known/change-password`
  helper (not created for MoltNet, so it looks nothing up); Reading
  mode's voice-pack status (MoltNet gets only its own).
- *Window and menu only, or metrics:* the recent-tabs menu, the app menu
  and its new 156 layout, the profile and avatar menus, Customize Chrome,
  the clear-browsing-data and history dialogs Incognito shows instead of
  those pages, the warning about downloads when the last window closes,
  the New Tab page's title, the split-view New Tab page, the pin,
  default-browser, PDF and startup infobars and prompts, feature
  promotions, the memory saver bubble, the Journeys and side-panel pin
  buttons, the support tool, themes and colours (including Isolated
  Mode's light scheme), and histograms.
- *Not on the desktop:* the Lens and open-tab omnibox checks (Android),
  digital goods and the supervised-user observer (ChromeOS), page info's
  automatic picture-in-picture row (Android).
- *Isolated Mode only:* every new `IsEnterpriseIsolatedModeProfile()` and
  "Isolated Mode replaces Incognito" check (menus, the New Tab page,
  enterprise connectors, data protection, the enterprise URL lookup, the
  profile type). MoltNet is never that session; its profile type stays
  "other off-the-record profile".
- *Gone:* the Privacy Sandbox delegate's Incognito check (Shared Storage:
  removed upstream at 156, with the last Privacy Sandbox web APIs) and page
  info's unused Incognito flag for cookies.
- *Policies for Incognito:* 156 adds a second "Incognito" source condition
  to the administrator's data-control rules (for pasted content). Like the
  other data-control conditions, it doesn't cover MoltNet (§5.7).
  Incognito's availability and its URL lists do (*Incognito's
  availability*, above).

The off-the-record checks 156 adds or removes keep covering MoltNet: new
features test `IsOffTheRecord()` (AtMemory, Windows' MediaFoundation
media-license module, which 156 refuses off the record, the media-storage
salt, now kept in memory), and autocomplete entries are still never saved
from an off-the-record form. Three new profile settings reach pages and
are now among those MoltNet takes from MoltBrowser's defaults (§1.10): the
XSLT deprecation banner's "Never show again", the Global Privacy Control
setting behind 156's flags, and the AtMemory keyboard shortcut.

**Requests outside Tor.** A MoltNet page's own requests go through
MoltNet's network context, and so does most of what the browser does for
it. But some browser features send what they get through another context:
the browser-wide (system) one, the regular profile's (Chromium hands
several services of an off-the-record profile to its regular profile), or
a service's own. For a MoltNet window that would be a request outside Tor,
from your own IP address. We went through the candidates in the Chromium
152 code and MoltBrowser's own, and again in the 156 code, together with
every browser-process service a MoltNet page can trigger (not yet tested
in a built browser):

- *Through MoltNet's context, so through Tor:* favicons (the page's
  renderer downloads them); certificate fetches (each network context
  gives its certificate verifier its own loader, so missing intermediate
  certificates are fetched through Tor, and so would online revocation
  checks, which are off by default; in MoltNet each such fetch also gets
  a one-off network key, see *Certificate fetches* below); downloads and
  "Save page as"; the
  check for a `www.` address after a certificate name error; payment
  method manifests; FedCM; Lens and "Search image with ..." (they open
  in the MoltNet window); the AI chat's `/pdf` download, its model
  downloads and its web agent, which acts in the chat's own tab (with a
  model that sees images, the agent's screenshots of that tab go only to
  the on-device model: no request at all, docs/design/AGENT_ACTIONS.md
  §2.6); DevTools opened on a MoltNet page (the resources it loads come
  through the inspected tab's context); the email-verification step of
  FedCM, whose DNS TXT lookup is an HTTPS request through the page's
  context; and Background Fetch downloads. Google's services among them
  reach Google only when the page or you choose them, and then on the
  page's circuit like any other request: signing in with Google on a site
  (FedCM), Google Pay as a page's payment method (its payment manifest),
  Lens and "Search image with Google", and searches, if Google is your
  search engine.
- *Never sent from an off-the-record profile* (Chromium's own rules):
  address-bar suggestions, including the on-focus ones; adding search
  engines a page announces; Google's spelling service; real-time and
  hash-prefix real-time Safe Browsing, client-side phishing detection and
  Safe Browsing reports; the password leak check; optimization hints;
  certificate-transparency (SCT) auditing; push and notifications; the
  page info "About this site" and merchant-trust lookups. Network
  prediction is off (see above), so nothing is prefetched or prerendered
  either.
- *Not in MoltBrowser builds:* they need a Google API key or a Google
  sign-in, which MoltBrowser doesn't ship (docs/NETWORK_TRAFFIC.md):
  password affiliations, Google's speech recognition, the
  on-device model of Chrome's built-in AI (Chrome builds only), and
  Autofill's Google-account features (Google Payments cards and their
  card art, Wallet passes, plus addresses, one-time codes from Gmail,
  Autofill's AI features).
- *On the device, no request at all:* translating a page. Translate runs
  on MoltBrowser's on-device model and its language list is built in, so
  Google's translate script (which came through the browser-wide context)
  and language list are never requested, with or without an API key. In
  MoltNet windows page translation is off altogether, because a translated
  page can see your language (docs/design/PAGE_TRANSLATION.md §5).
- *Fixed for MoltNet*, decided by the window being a MoltNet window
  and nothing else (`chrome/browser/molt_ai/tor/molt_net_requests_policy.h`):
  - **Cloud AI.** MoltBrowser's AI chat (side panel and molt://ai) in a
    MoltNet window sent your prompt, with the page text, PDF, attachment
    and conversation it carried, to the cloud provider you connected. It
    used MoltNet's context, but every request carries your API key, which
    names your account, so the provider could tie each MoltNet page to
    you. In MoltNet windows the chat lists only on-device models, and a
    cloud model is refused with "Cloud AI is off in MoltNet windows. Use
    an on-device model." On-device models work as before.
  - **The chat's other ways out.** Its `/sandbox` command opened the page
    in an Incognito window, outside Tor; from MoltNet it now opens a
    MoltNet window. Its actions (open, click, type) and the links in its
    replies went to the last active window when the chat was in the side
    panel, which could be a regular window; they now go to the chat's own
    window and never cross between MoltNet and regular windows. Page
    watchers and saved plans are stored with your other workflows and run
    later by the regular profile, which would fetch the watched MoltNet
    page again and again outside Tor; a MoltNet window can't create them.
    (The AI memory never records MoltNet pages, §1.2.)
  - **Safe Browsing** is off in MoltNet windows. Its pings for downloaded
    programs (the download URL, the pages that led to it, the file's name
    and SHA-256) and for a saved password typed on another site (the page
    URL), and its full-hash checks, go through Safe Browsing's own network
    context, which an off-the-record profile shares with the regular
    profile. Without a Google API key Safe Browsing gives no verdicts
    anyway (docs/SAFE_BROWSING_DECISION.md); downloads of dangerous file
    types still warn, which is local. A policy that sets Safe Browsing
    blocks MoltNet like the other leak controls.
  - **Image descriptions** ("Get image descriptions from Google", for
    screen readers) uploaded the URLs and pixels of a page's images
    through the regular profile's context. MoltNet pages get no
    descriptions.
  - **Location.** `navigator.geolocation` asked the system's location
    service (on Linux, Google's, through the browser-wide context, with
    the Wi-Fi networks around you). In MoltNet windows it is denied
    without a prompt, and no location provider starts.
  - **Cast** and the Presentation and Remote Playback APIs (device
    discovery on your network; a Cast device loads what it is sent from
    your own network) are off in MoltNet windows, as when a policy turns
    off Cast.
  - **Passkeys on a phone.** Signing in with a passkey on your phone
    (hybrid) and Google Password Manager passkeys reach their relay
    servers through the browser-wide context, and the phone connects to
    the same relay from your network. MoltNet pages don't offer them;
    security keys and your computer's own passkeys still work.
  - **Shopping features.** An off-the-record tab gets the regular
    profile's shopping service, and Chromium keeps its price-insight and
    discount helpers out of Incognito only. With a Google API key it would
    look MoltNet pages' URLs up through the regular profile. MoltNet tabs
    don't report to it.
  - **"Open all" bookmarks** from a MoltNet window: once a bookmark that
    can't open off the record (Settings, History) opened in a regular
    window, the rest followed it there, outside Tor. They now stay in
    MoltNet, as Incognito's stay in Incognito.
  - **Autofill's server.** When a page shows a form, Autofill asks
    Google's Autofill server what kind of data each field takes, with
    signatures of the form and its fields (hashes of the form's target
    host and of the form and field names): the same for that form on
    every site and in every session. After a submission, Autofill and the
    password manager upload votes (what the fields turned out to be;
    Autofill sends none from an off-the-record profile). The queries
    went through MoltNet's context, on the page's circuit, but to Google,
    which could tie together the forms one MoltNet user loads, across
    sites and identities. MoltBrowser turns the Autofill server off for
    every window by default (docs/NETWORK_TRAFFIC.md), but a command-line
    flag turns it back on. MoltNet windows send it nothing either way: no
    query, no vote. Fields are still recognised on the device, and your
    saved addresses and passwords still fill.
  - **Address formats.** To fill a state or region list it can't match
    directly, Autofill downloads the country's address rules
    (libaddressinput's, from `chromium-i18n.appspot.com`), and so does a
    page's payment sheet (Payment Request API) when you pick or edit a
    shipping address. That download went through the browser-wide
    context, outside Tor, named the address's country, and the rules were
    kept in a file every profile shares. MoltNet windows download none
    and read none from that file: a region that needs the rules isn't
    filled, the payment sheet keeps the address as you entered it, and its
    address editor asks for the region as text.

  The binders for image descriptions and location are in
  `molt_net_request_binders.h` (registered with the WebRTC ones); the
  rest are one-line hooks marked `MoltBrowser:` in
  `media_router_feature.cc`, `chrome_authenticator_request_delegate.cc`,
  `tab_features.cc`, `tab_helpers.cc` and `bookmark_utils_desktop.cc`,
  and in MoltBrowser's chat handler and `molt_net_routing.cc`; the
  Autofill ones in `chrome_autofill_client.cc` and
  `chrome_payment_request_delegate.cc`, through `molt_net_autofill.h`.
- *Browser-wide, not caused by MoltNet pages:* MoltShield's list updates
  and MoltBrowser's update check (§5.6) use the regular profile's
  context. DNS-over-HTTPS probes run in the browser's other network
  contexts; MoltNet's starts none (above), and resolves no page's names
  itself (Tor does, inside SOCKS5). MoltBrowser's error pages don't probe
  DNS (off by default, and in MoltNet the setting it follows, "Use a web
  service to help resolve navigation errors", is pinned off), and Tor
  failures are proxy errors, not DNS errors.
- *Still outside Tor:* see §5.7.

**Circuits.** Tor keeps connections with different SOCKS5 credentials on
different circuits (`IsolateSOCKSAuth`, on by default). MoltNet's
connections carry two: the username is the connection's top-level site
(per-site circuits, §1.4), and the password is a random 128-bit value made
for the MoltNet profile's network context when that context is created
(`molt_net_network_context_policy.h`, read by the SOCKS connection code in
`net/socket/socks_connect_job.cc`). Every MoltNet identity has a profile
and so a network context of its own: a new one after New Identity, and a
new one when MoltNet windows open again after the last one closed. So a
new identity never shares a circuit, and so an exit, with an earlier one,
also for a site both visit, even within the ten minutes or so for which
Tor would otherwise keep using a circuit for that site, and whether or not
Tor has applied New Identity's request yet. New Identity still asks Tor
for new circuits (`SIGNAL NEWNYM`): Tor then puts no new connection on a
circuit that has already carried one, which covers a connection of the
old identity that is still being set up inside Tor, and clears its own
caches (resolved names, onion-service descriptors). Regular and Incognito
windows don't use Tor; a SOCKS5 proxy you set up for them on your own
computer gets the per-site username and a fixed password, as before.

**Certificate fetches.** To check a site's certificate the browser may
download a missing intermediate certificate (AIA) and, with online
revocation checks on, an OCSP response or a CRL, from addresses the
certificate names. Chromium files each such download under the address's
own site, whichever site's certificate needed it, and MoltNet's per-site
circuits follow that key. In MoltNet each one gets a one-off key instead
(`net::IsolationInfo::CreateTransient()`): nothing is kept in the HTTP
cache for it, it opens its own connection on its own Tor circuit, and two
identical downloads in flight at the same time are made separately. The
switch is set per network context, for MoltNet's only
(`molt_net_network_context_policy.h`, read by the certificate verifier's
fetcher in `services/cert_verifier/`); every other profile keeps
Chromium's behaviour.

**Shared network state.** Within one MoltNet identity (between two New
Identity presses), what one site causes in MoltNet's network context must
not show to another site. Tor Browser's rule is first-party isolation:
everything keyed by the top-level site. Chromium 152 keys most of a
network context's state by the request's network key (the top-level site,
for some state also the frame's site), but not all of it, and three of the
switches that turn the keying on apply to the whole browser. We went
through each cache and pool a 152 network context keeps, and again
through a 156 one, which adds the renderer-accessible HTTP cache and
shared ("pervasive") compression dictionaries (read in the code, not yet
measured in a built browser):

| State | In MoltNet's network context | After New Identity |
|---|---|---|
| HTTP cache | keyed by top-level site and frame site; one-off keys (certificate fetches, opaque origins) are never cached. Chromium 152 files a built-in list of well-known scripts and styles under one entry shared by every site where cookie settings allow full cookie access; MoltNet's context never does | gone (in memory, with the context) |
| Code cache | none: off-the-record profiles have no code cache | — |
| Idle sockets, HTTP/2 sessions | keyed by top-level site; each site's connections also go to Tor with that site as SOCKS username, so on circuits of their own (§1.4) | closed, then gone |
| Connection limits | **per site**. Chromium gives each proxy one pool of connections, 128 at most for all sites together (the `MaxConnectionsPerProxy` policy changes the number; at 156 the pool may also grow past it by a random amount), so a site that kept many connections open made other sites' requests wait, and could count how many the others held. MoltNet's context gives each site (the top-level site, for its first-party and third-party requests alike; a page its server sandboxed counts against the site it came from) pools of its own: 48 connections for HTTP and 16 for WebSockets, whatever the policy says and with no random room, with Chromium's 6 per host inside. The one-off keys no site can be named for (certificate fetches) share one pool of 8 | closed, then gone |
| Connections to Tor, all sites together | **within a budget** of 768, which fits eleven sites' full shares, inside what MoltNet's single Tor process always accepts (*Tor's capacity* below) | closed, then gone |
| Circuits Tor builds at a time | **at most 16 per site**: a site's SOCKS handshakes in progress, each of which may make Tor build a circuit, are limited, and Tor's limit for all sites together is 64 (*Tor's capacity*) | — |
| Keepalive requests (`fetch()` with `keepalive`, `sendBeacon()`, pings) | **per site**. The network service lets 2048 be in flight for every profile and site together (and 256 per page), so a site that used them up made another site's, or a regular window's, fail. MoltNet's context counts each site's on their own, 2048 for the site and 256 per page, and none of them toward another context's | gone |
| Storage quota and eviction (IndexedDB, Cache Storage, OPFS, service worker scripts) | **per site**: each site's storage keys together may use 60 MiB, all sites together 600 MiB, and nothing is evicted to make room ([FINGERPRINTING.md](FINGERPRINTING.md#moltnet-storage-limits)). Chromium checks writes against each storage key's quota only and, every half hour, evicts the least recently used data of any site until all sites use 70% of a pool | gone (in memory) |
| TLS session resumption | keyed by top-level site | gone |
| QUIC sessions and server configurations | not used: QUIC doesn't go through a SOCKS5 proxy | — |
| Certificate verification results | none kept. Chromium keeps each network context's results for up to 30 minutes, keyed by certificate, hostname and flags only, and merges identical verifications in flight; MoltNet's context verifies every certificate on its own, also on resumed connections | — |
| Intermediate certificates (AIA) | none kept: a missing one is downloaded for that verification only, with a one-off key (*Certificate fetches*), and the next verification downloads it again. The network stack puts no site's certificate into the system's certificate stores | — |
| Revocation (CRLSet, CRL, OCSP) | the CRLSet is one list for everyone, from component updates; online CRL and OCSP checks are off unless a policy turns them on, and then each download is a one-off fetch; stapled OCSP responses belong to their connection | — |
| Trust stores | Chromium's root store, the system's stores and policy certificates; nothing a site does adds to them | — |
| Server properties (alternative services, HTTP/2 support, connection statistics) | keyed by top-level site; in memory (only regular profiles save them) | gone |
| HTTP authentication | server credentials keyed by top-level site, always (Chromium 152 doesn't split them by default, and the `GloballyScopeHTTPAuthCacheEnabled` policy can stop it; neither reaches MoltNet); Basic and Digest only (§2 above) | gone |
| HSTS | entries a response sets are stored by host. In MoltNet they upgrade top-level navigations only (Chromium's `HstsTopLevelNavigationsOnly`, for MoltNet's context), so a page's subresource requests can't read what other sites' responses stored; HTTPS-Only mode upgrades every plain-HTTP navigation anyway. Expect-CT no longer exists | gone (in memory) |
| Host-name cache | keyed by top-level site at Chromium 156 (`SplitHostCacheByNetworkAnonymizationKey`; at 152 Chromium's field-trial testing config turned it off for every profile, and 156's no longer does), and MoltNet pages' host names are never looked up locally anyway: SOCKS5 sends them to Tor | cleared, then gone |
| CORS preflight results | keyed by the request's network key | gone |
| Compression dictionaries | keyed by frame origin and top-level site. Chromium 156 adds one store shared by every site for the dictionaries of the well-known scripts and styles above (`PervasiveSharedDictionaries`, on in Chromium's field-trial testing config, so in MoltBrowser builds); a dictionary goes there only from a response filed under the shared cache entries, which MoltNet's context never uses, so its shared store stays empty | gone |
| Renderer-accessible HTTP cache (Chromium 156) | none: only for on-disk caches, and MoltNet's is in memory | — |
| Device-bound sessions (Chromium 156, Windows and macOS) | none: MoltNet's context has no session service | — |
| TLS client-certificate choices | none kept. Chromium remembers the certificate (or "no certificate") picked for a server by host and port, for every site; MoltNet's context answers every server's request with no certificate, at once, the same way on every site, and the browser shows no picker and applies no auto-select policy for MoltNet pages | — |
| Cookies | third-party cookies blocked; partitioned cookies keyed by top-level site | gone |
| Private State Tokens | blocked: an issuer's tokens can be detected and redeemed on any site, by design | — |
| Reporting API, Network Error Logging | not built into MoltBrowser (`enable_reporting = false`) | — |
| Topics, Shared Storage, Protected Audience, Attribution Reporting | removed upstream at 156: Chromium 156 has none of these web APIs | — |
| Related Website Sets | one list for the whole browser, from component updates or policy; sites can't change it | — |
| Preconnect and prediction | off-the-record profiles have no preconnect or prediction service, and network prediction is off | — |
| Network-quality estimate | **fixed**: none kept. The network service keeps one estimate for the whole browser; MoltNet's context has no estimator, so its requests don't feed it and its request scheduling doesn't follow it, and MoltNet's pages read one fixed connection and network quality instead ([FINGERPRINTING.md](FINGERPRINTING.md#moltnet-connection-and-network-quality)). The browser doesn't send MoltNet's renderer processes the real estimate or the operating system's connection type either: they get the same fixed values once, when they start, and after that only "offline" and "online" (`content/browser/network/molt_uniform_network_state.h`) | — |

New Identity flushes the old identity's connections and host-name cache,
then waits until its profile, and with it the network context and
everything in the table kept there, has been destroyed (§1.3); the next
identity's network context starts empty and gets MoltNet's settings again.

The connection limits in detail: `net/socket/molt_per_site_socket_pool_policy.h`
(the numbers and the decisions), `molt_per_site_socket_pools.h` and
`molt_connection_budget.h`, used by `ClientSocketPoolManagerImpl` and
`SOCKSConnectJob` for MoltNet's context only (a `[MinVersion=1]` field of
its network context parameters); every other context, and connections
that don't go through a proxy, keep Chromium's one pool. A site's pools
ask the context's budget before each new connection (a gate in
`ClientSocketPool`). A request it holds back waits in its pool, as for
the pool's own limit, and gets a connection when one is released
anywhere: the waiting site using the fewest goes first, and no site using
more may take one before it. While the budget is full, MoltNet closes an
idle connection of the site using the most (or its idle HTTP/2 sessions,
as Chromium does for a pool at its own limit), if that site uses more
than one more than the waiting site; a connection in use is never closed.

What a page can still observe of them: its own site's limits (requests
beyond 6 connections to one host, 48 for the site or 16 WebSockets wait,
and beyond 16 connections being set up at once they start in turn), and
nothing of another site's use while fewer than eleven other sites hold
their full share. Past that, a site below its own limits can see that the
budget is full (its new connections wait for one to be released), and a
site holding idle connections while it is the largest can see one closed.
Filling the budget takes many sites' connections held open at the same
time: a page that navigates its tab through sites of its own leaves the
earlier ones' connections idle, and those are the first closed.

What they cost: a pool is a small object and holds connections only while
they are used or idle; a pool with no connection left that nobody has
asked for in 60 seconds is dropped the next time a pool is made. A site
has 48 HTTP connections instead of the 128 Chromium gives all sites
through a proxy together (with HTTP/2 a site needs one per server, and
through Tor a site's streams share its circuit, so more connections in
parallel don't add bandwidth), and sets up 16 at a time.

**Tor's capacity.** All of MoltNet's sites share one Tor process.
MoltBrowser's torrc states `ConnLimit 1000`: Tor refuses to start unless
the system lets it open that many file descriptors, and keeps 32 of them
for itself, so a Tor that runs accepts at least 968 connections, on every
system (checked with Tor 0.4.9: with its descriptor limit at exactly
1000, it answered 965 SOCKS connections and then logged "Failing because
we have 967 connections already"). MoltNet's budget of 768 leaves 200 of
those to Tor's own connections (guards, directory fetches, the control
port, its files), so no page can make Tor turn away other sites'
connections. Each site's SOCKS handshakes in progress are limited to 16
(the one-off keys' to 8 together): a handshake is what makes Tor build a
circuit for a stream that can't use an existing one, and Tor builds at
most `MaxClientCircuitsPending` circuits at a time for all sites together.
MoltBrowser sets that to 64, twice Tor's default, so that no one site can
fill it: 16 connections that each need at most three new circuits at a
time (an onion service's descriptor, introduction and rendezvous
circuits) make 48, and the certificate fetches' 8 more make 56. Tor
Browser leaves both at Tor's defaults (1000 and 32) and lets Firefox's
connection limits apply: 900 connections for all sites together, 6 per
server.

What a page can still observe of Tor's capacity: its bandwidth and its
circuit building are shared. A site downloading at full speed slows other
sites' traffic on the same guard connection (Tor sends a quiet circuit's
cells before a busy one's, which keeps the delay small, not zero), and a
group of sites that each start 16 connections needing new circuits can
fill Tor's pending-circuit limit, so that another site's first
connection waits until one of those circuits is built. Tor's memory for
queued cells is bounded per circuit by its flow control; if it ever runs
short (`MaxMemInQueues`, which MoltNet leaves at Tor's default, a share of
the machine's memory), Tor closes the circuits whose queued data is
oldest: a site that stopped reading before one that reads. Tor Browser
shares all of these the same way.

**Keepalive requests.** Requests that may outlive their page (`fetch()`
with `keepalive`, `navigator.sendBeacon()`, `<a ping>`) are limited by
the network service: a new one fails (`ERR_INSUFFICIENT_RESOURCES`, a
network error in the page) once 2048 are in flight, once 256 are in
flight for its page (top-level frame), or once that page's hold 512 KiB
of URLs and headers. Chromium counts the 2048 for the whole network
service, every profile and site together, so one site using them up made
another site's requests fail, between MoltNet sites and between MoltNet
and regular windows. In MoltNet's network context each site (a page's
top-level site; a page its server sandboxed counts against the site it
came from) has a count of its own with the same three limits, and none
of MoltNet's requests count toward another context's
(`services/network/molt_keepalive_partitions.h`, hooked into
`KeepaliveStatisticsRecorder`, `URLLoaderFactory` and `NetworkContext` for
contexts with the `[MinVersion=1]` field `molt_per_site_keepalive_limits`).
What a page can still observe: its own site's use, and its own pages'.
What it costs: a small count for each site visited, kept until the
identity ends.

**Storage.** What MoltNet's sites store (IndexedDB, Cache Storage, the
origin-private file system, service worker scripts) is held in memory.
Each site's storage keys together (its own and those of what it embeds)
may use 60 MiB, all sites together 600 MiB, and nothing is evicted to make
room: Chromium evicts the least recently used data of any site every half
hour once all sites use 70% of its pool. The details, what a page can
still observe and what it costs:
[FINGERPRINTING.md](FINGERPRINTING.md#moltnet-storage-limits).

The certificate-verification choice in detail: a cache keyed by top-level
site would meet the rule too, but Chromium 152's verifier interface doesn't
carry the network key (a certificate, a hostname and flags), so keying it
would change a core network interface and every caller. Clearing the cache
at New Identity only, which is what happens anyway, wouldn't help within
an identity. So MoltNet's context has no result cache and no merging. What
that costs: a verification of a typical chain (a P-256 leaf, P-384
intermediate and root) took about 2 ms of CPU on a test machine (OpenSSL;
Chromium's verifier adds the CT checks and a hop to its verifier thread),
once per new or resumed TLS connection; a connection through Tor waits
hundreds of milliseconds for the circuit and the handshake. A server that
leaves out its intermediate certificate costs more: its certificate is
downloaded again, on a circuit of its own, for each new connection to it.

**What can turn the partitioning off.** The keying above follows
`base::Feature`s and settings that a command-line flag
(`--disable-features`), `chrome://flags`, an enterprise policy or a field
trial can change. In Chromium 152 and 156 `chrome://flags` has no entry for
any of them. MoltBrowser has no field-trial server: builds without Chrome
branding download no variations seed unless started with
`--variations-server-url`, and they apply Chromium's field-trial testing
config, which turns the connection and HTTP-cache partitioning on (and, at
152, the host-name cache split off; 156's config leaves that on). A seed
or other trials can still come in through command-line switches or a
changed `Local State` file. So MoltNet reads the effective
state once the browser has started, whatever set it, and per switch:

| Switch | What it keys by site | MoltNet |
|---|---|---|
| `PartitionConnectionsByNetworkIsolationKey` | idle sockets, HTTP/2 and QUIC sessions, TLS resumption, server properties, and with them each site's SOCKS username and so its circuit | Set for the whole browser only. While it is off, MoltNet doesn't open: "MoltNet keeps each site's connections, cache and storage apart, and a command-line flag or field trial has turned that off for the whole browser (…)", naming the switch |
| `SplitCacheByNetworkIsolationKey` | the HTTP cache; without it, one-off keys are cached too | Whole browser only; MoltNet doesn't open while it is off. (A MoltNet context without a cache would keep sites apart too, but would also make MoltNet users differ from each other) |
| `ThirdPartyStoragePartitioning` | a third-party frame's storage, BroadcastChannel, Web Locks, shared and service workers | Whole browser only; MoltNet doesn't open while it is off |
| `SplitAuthCacheByNetworkIsolationKey`, policy `GloballyScopeHTTPAuthCacheEnabled` | HTTP authentication | Per network context: always split for MoltNet |
| `CacheSharingForPervasiveResources` | shared HTTP cache entries for well-known scripts | Per network context: never for MoltNet |
| `HstsTopLevelNavigationsOnly` | which requests HSTS upgrades | MoltNet's context always behaves as with it on |
| `CacheCertVerification` | adding results to the verification cache | MoltNet's context has no such cache |
| `SplitHostCacheByNetworkAnonymizationKey` | the host-name cache | On at 156 (off in 152 builds, by the testing config); MoltNet pages' names aren't looked up locally either way |
| `PrivateStateTokens` | nothing: an issuer's tokens work on every site | Blocked per network context for MoltNet, whatever it and the anti-abuse setting say |
| Policy `PartitionedBlobUrlUsage` (a profile setting, not a `base::Feature`) | blob URLs: whether a third party's blob URL made on one site can be fetched or opened by the same third party on another | Per profile, and a policy can change it while the browser runs. MoltNet doesn't open while it is off ("A policy turns off the partitioning of blob URLs by site (PartitionedBlobUrlUsage), so MoltNet can't keep sites apart"), and ends if it is turned off while MoltNet is open (*Fail-closed*) |

The first three, like every `base::Feature`, are fixed once the browser
has started, so they can't change while MoltNet is open. The policies
that once turned third-party storage partitioning off for some or all
sites (`DefaultThirdPartyStoragePartitioningSetting`,
`ThirdPartyStoragePartitioningBlockedForOrigins`) were supported up to
Chromium 145; 156 has neither.

`PartitionSSLSessionsByNetworkIsolationKey`,
`EnableCrossSiteFlagNetworkIsolationKey` and the other per-state switches
of earlier Chromium versions no longer exist in 152: that state follows
`PartitionConnectionsByNetworkIsolationKey` or is always keyed. The check
runs before Tor starts and again before the MoltNet profile is created
(`molt_net_partitioning_policy.h`, read in `molt_net_routing.cc`); the
per-context settings are in `molt_net_network_context_policy.h`, applied by
hooks marked `MoltBrowser:` in `network_context.cc`,
`transport_security_state.{h,cc}`, `shared_resource_checker.h`,
`ssl_client_socket.{h,cc}` and `profile_network_context_service.cc`.
Regular and Incognito windows follow
the flags and Chromium's defaults as before.

**Captive portals.** Chromium checks for a captive portal (a network that
wants you to sign in first) after certificate errors and slow loads, if
"Use a web service to help resolve navigation errors" is on, and opens the
portal's sign-in page in a tab or a small window of the same profile. In
MoltNet that check would go through Tor, where no network sign-in page can
be reached. The setting is pinned off for MoltNet (MoltBrowser's default;
your regular profile's choice isn't read), so MoltNet never checks, and
whatever asks for a sign-in page in a MoltNet window (a check a policy
switched on, or the "Connect" button of the page Chromium shows on
Windows when Windows itself reports a captive portal) opens nothing
(`chrome_security_blocking_page_factory.cc`). Sign in to the network in a
regular window.

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
| New Identity reports success only once Tor has applied `NEWNYM` (Tor answers `250 OK` at once and, within ten seconds of the previous one, applies it later); replies and events read correctly from one connection | Unit tests of the control-protocol framing (`molt_ai_unittests`); standalone harness around the shipped control-connection code and the New Identity signal, Tor 0.4.9.11: an immediate and a rate-limited `NEWNYM`, an event arriving with a reply; 2 mutants caught | Pass (built standalone; not yet built in the Chromium tree) |
| New Identity's order: the old windows close although a page asks "Leave site?", `NEWNYM` only once the old profile is destroyed, the new window only once Tor has switched, no other MoltNet window meanwhile, nothing changed if an old window stays open | Standalone simulation: the New Identity code of `molt_net_window.cc`, verbatim, against stand-ins (profiles, windows, the profile destroyer, Tor) on a simulated clock, g++ ASan/UBSan; 8 tests, 5 mutants caught | Pass (built standalone; not yet built in the Chromium tree) |
| Sandboxed pages (opaque top-level origins) get per-site SOCKS usernames | The username code of `socks_connect_job.cc`, verbatim, against stand-ins with the 152 shapes; 3 tests, a mutant caught | Pass (built standalone; not yet built in the Chromium tree) |
| Each MoltNet network context gets a random SOCKS password (§2 *Circuits*): the same for all of its connections, a new one for every context, the username still per site; other profiles get none (the fixed password, as before) | Unit tests (`molt_ai_unittests`, with the real parameter structs); standalone harness: the shipped code of the whole path (the `ProfileNetworkContextService` policy, the network service hook, `HttpNetworkSession::CreateCommonConnectJobParams()`, `CommonConnectJobParams`, the SOCKS connect job and the SOCKS5 socket's credential check) verbatim against stand-ins with the 152 shapes (g++ ASan/UBSan, clang `-fno-exceptions`), 7 tests, 8 mutants caught | Pass (built standalone; not yet built in the Chromium tree) |
| Certificate fetches in MoltNet (§2 *Certificate fetches*): the switch is set for MoltNet's network contexts only; with it, the certificate fetcher gives every fetch a fresh one-off key and never merges two fetches (the URL rules unchanged), without it Chromium's behaviour; each one-off key gets a SOCKS username of its own | Unit tests (`molt_ai_unittests`, with the real parameter structs); standalone harness: the shipped `cert_net_fetcher_url_loader.cc` compiled unchanged against stand-ins with the 152 shapes (g++ ASan/UBSan, clang `-fno-exceptions`), 6 tests, 5 mutants caught; the `ProfileNetworkContextService` hook, verbatim, against the 152 profile kinds; the SOCKS username code, verbatim | Pass (built standalone; not yet built in the Chromium tree) |
| HTTP authentication in MoltNet (§2): Basic and Digest only, also before the network service has its browser-wide values and after they change, whatever policies allow; other contexts unchanged | Standalone harness: Chromium 152's `HttpAuthPreferences`, the network context's code verbatim and the scheme check of `HttpAuthHandlerRegistryFactory` verbatim (g++ ASan/UBSan, clang `-fno-exceptions`); 5 tests, 7 mutants caught | Pass (built standalone; not yet built in the Chromium tree) |
| No captive-portal check or sign-in page in MoltNet (§2 *Captive portals*): the setting is pinned and the regular profile's value isn't read; `OpenLoginTab()` opens nothing for MoltNet and stays Chromium's for regular, Incognito, DevTools, guest and look-alike profiles | Unit tests (`molt_ai_unittests`); standalone: the real pinning code against a settings stand-in (a mutant caught), the hook verbatim against stand-ins with the 152 signatures | Pass (built standalone; not yet built in the Chromium tree) |
| `.onion` detection, v3 address and `Onion-Location` validation | 6 unit tests (`molt_ai_unittests`) | Pass (built standalone; not yet built in the Chromium tree) |
| Helper-binary checks (bundled copy; root-owned system Tor on Linux; rejects user-owned, group/world-writable, setuid, symlink escapes) | 13 unit tests incl. real-filesystem cases (`molt_ai_unittests`) | Pass (built standalone; not yet built in the Chromium tree) |
| Bridges with system transports: which `/usr/bin` paths are tried, the `ClientTransportPlugin` lines (absolute path for a system transport), exec-path validation (no blanks, `..`, bare names or torrc injection), which transports a set of bridges needs, the install messages | 7 unit tests (`molt_ai_unittests`), with ASan/UBSan | Pass (built standalone; not yet built in the Chromium tree) |
| Tor runs the distribution's transports from the torrc MoltBrowser writes | `tor --verify-config`, then a short run of Tor 0.4.9.11 with Ubuntu 24.04's `obfs4proxy` 0.0.14 and `snowflake-client` 2.5.1 at absolute paths: both complete the managed-proxy handshake and are registered, and Tor connects to them (no bridge was reachable from the test machine) | Pass |
| Settings names the transport to install | jsdom smoke test of molt://ai-settings (`tools/webui_checks`) | Pass |
| Onion request URLs (any scheme, look-alikes refused), `<meta>` `Onion-Location` rules, validation of the page's answer | Unit tests (`molt_ai_unittests`) | Written; need a Chromium build |
| The script that reads `<meta http-equiv="onion-location">` | Run under jsdom against 13 documents (`<head>` vs `<body>`, letter case, look-alikes, missing `content`, more than one, length cap) | Pass |
| Which extensions MoltNet admits (component only; an ID with no loaded extension is kept out), MoltNet's in-memory certificate exceptions | 6 unit tests (`molt_ai_unittests`); standalone harness with g++ ASan/UBSan and clang `-fno-exceptions`: the same tests, the Chromium glue against stand-ins with the 152 signatures (`StatefulSSLHostStateDelegate`, `Extension` locations, OTR profile ids), every hook expression, and 5 mutants all caught | Pass (built standalone; not yet built in the Chromium tree) |
| Extensions, autoplay, `persist()`, lookalike warnings, certificate exceptions, third-party cookies, Shared Storage absent (removed upstream at 156), ambient authentication in a running browser (§4 steps 16-17) | Needs a Chromium build | **Not yet done** |
| `.onion` blocked and offered in regular windows, no DNS query, no onion request or WebSocket reaching a configured proxy, `Onion-Location` bar (header and `<meta>`), `http://` onion under HTTPS-Only, bridges connecting through the distribution's transports | Needs a Chromium build | **Not yet done** |
| Which settings MoltNet takes from MoltBrowser's defaults (§1.10): the list, prefix matching, look-alike names | 6 unit tests (`molt_ai_unittests`) | Pass (built standalone; not yet built in the Chromium tree) |
| MoltNet's settings store: the regular profile's values and later changes don't show, MoltNet's own values and policy still apply, nothing is written back (an Incognito-style store leaks both ways) | Unit tests (`molt_ai_unittests`), run against stand-ins of Chromium's settings classes; the hook that picks MoltNet's store, checked the same way | Pass (standalone stand-ins); the real-class tests need a Chromium build |
| Site settings with Chromium's real content-settings code: blocked JavaScript, a blocked permission, Shields off and a permission embargo in the regular profile, none of them in MoltNet; changes in a MoltNet window stay there | Unit tests (`molt_ai_unittests`) | Written; need a Chromium build |
| No WebRTC in MoltNet (§1.12): the decision (MoltNet profiles only, whatever fingerprinting protection says), the renderer switch, the stand-in socket manager (no name resolved, including relay names, `.local` names and IP literals; no socket for any socket type; an empty interface list; its connection kept open), the closed mDNS responder, other profiles keeping Chromium's own, and the four hooks in Chromium files | 6 unit tests (`molt_ai_unittests`); standalone harness with clang `-fno-exceptions` and g++ ASan/UBSan, with and without WebRTC support compiled in: the same logic, the Chromium glue and the hooks (taken verbatim from the changed files) against stand-ins with the 152 signatures, 13 mutants all caught | Pass (built standalone; not yet built in the Chromium tree) |
| The same over real mojo connections, and Blink's switch | Unit tests (`molt_ai_unittests`, `blink_unittests`) | Written; need a Chromium build |
| `RTCPeerConnection` throws in a MoltNet window and no relay or peer name is looked up (§4 step 18) | Needs a Chromium build | **Not yet done** |
| Which profiles get Incognito's protections (§1.13): MoltNet profile ids (look-alikes and Chromium's own off-the-record ids refused), the classification, and that every other profile, 156's Isolated Mode session included, keeps Chromium's answer | 6 unit tests (`molt_ai_unittests`); standalone with g++ ASan/UBSan and clang `-fno-exceptions` | Pass (built standalone; not yet built in the Chromium tree) |
| The hooks themselves (DevTools profile, media-controls metadata, network diagnostics, extension cookies, Web Share files, GCM and Instance ID, links from other apps, automatic picture-in-picture) and the print-preview clean-up | Standalone harness: each hook copied verbatim from the changed file and compiled against stand-ins with the 152 signatures (g++ ASan/UBSan, clang `-fno-exceptions`); MoltNet gets Incognito's result, and regular, Incognito, guest, DevTools, media-router and look-alike profiles get exactly 152's; 20 mutants all caught | Pass (built standalone; not yet built in the Chromium tree) |
| The hooks added by the 156 audit (WebRTC text logs, Reduce Accept-Language) and the three settings pinned at 156 | Read against Chromium 156's code; the pin list's unit tests pass standalone; the hooks and the settings-store tests need the 156 tree | Partly (not yet built in the Chromium tree) |
| Incognito's protections in a running browser (§4 step 19) | Needs a Chromium build | **Not yet done** |
| What MoltNet windows may cause outside Tor (§1.14): the per-feature decision (MoltNet profiles only), the chat's window rule and sandbox target, cloud AI and saved workflows refused in the chat, Safe Browsing among the leak controls, the image-description and location stand-ins, and the hooks in `MediaRouterEnabled()`, WebAuthn's `ConfigureDiscoveries()`, the tab's commerce helpers and bookmarks' "Open all" | 6 unit tests (`molt_ai_unittests`); standalone harness with clang `-fno-exceptions` and g++ ASan/UBSan: the same tests, the chat handler's helpers, the Safe Browsing leak-control check and every upstream hook (taken verbatim from the changed files) against stand-ins with the 152 signatures, 26 mutants all caught | Pass (built standalone; not yet built in the Chromium tree) |
| The image-description and location stand-ins over real mojo pipes | 5 unit tests (`molt_ai_unittests`) | Written; need a Chromium build |
| Nothing outside Tor from the chat, downloads, location, Cast, passkeys or bookmarks in a running browser (§4 step 20) | Needs a Chromium build | **Not yet done** |
| Certificate fetches on circuits of their own, no captive-portal check, no NTLM or Negotiate, in a running browser (§4 steps 8 and 20) | Needs a Chromium build | **Not yet done** |
| Per-site network state (§2 *Shared network state*): MoltNet's context has no certificate-verification cache or merging, no shared HTTP cache entries, HSTS for top-level navigations only, its HTTP auth cache keyed by site and no Private State Tokens, also after the policy or the anti-abuse setting changes; every other profile keeps 152's behaviour; a new identity's context gets all of it again | Unit tests (`molt_ai_unittests`, with the real parameter structs); standalone harness: the hooks taken verbatim from `network_context.cc`, `transport_security_state.{h,cc}`, `shared_resource_checker.h` and `profile_network_context_service.cc` against stand-ins with the 152 shapes (g++ ASan/UBSan, clang `-fno-exceptions`); the changed files type-checked against Chromium 152's headers (`tools/compile_check`) | Pass (built standalone; not yet built in the Chromium tree) |
| MoltNet doesn't open while `PartitionConnectionsByNetworkIsolationKey`, `SplitCacheByNetworkIsolationKey` or `ThirdPartyStoragePartitioning` is off, says which, and checks before Tor starts and before the profile is created; Chromium's default-on switches count as on | Unit tests (`molt_ai_unittests`, the feature names checked against Chromium's); the same harness with the glue from `molt_net_routing.cc` and `molt_net_window.cc`, verbatim; 15 tests in the harness, 30 mutants of the hooks and policies all caught | Pass (built standalone; not yet built in the Chromium tree) |
| The same in a running browser: a verification for every new TLS connection, the switches refused (§4 steps 8 and 9) | Needs a Chromium build; the leak kit's `identity.partitioning_override_refused` | **Not yet done** |
| The Chromium 156 network re-check (§2 *New network paths at Chromium 156*): MoltNet's context sends no TLS client certificate and remembers no choice, the same for every server and every site, and a request that reaches the browser is answered with no certificate and no picker; no DNS-over-HTTPS probes and no device-bound sessions in MoltNet's context; MoltNet refused while proxy override rules are set; MoltNet's renderers get the fixed network quality once and only "online" or "offline" as the connection; every other profile and context keeps Chromium's behaviour | Unit tests (`molt_ai_unittests`); standalone harness: the hooks taken verbatim from `ssl_client_socket.{h,cc}`, `network_context.cc`, `chrome_content_browser_client.cc`, `molt_net_routing.cc` and the two observers (the `profile_network_context_service.cc` hook for device-bound sessions through its policy's tests) in `content/browser/network/`, and the real `molt_uniform_network_state.h`, against stand-ins with the 156 shapes, including Chromium's client-certificate restart (g++ ASan/UBSan, clang `-fno-exceptions`): 22 policy and 17 hook tests, 17 mutants all caught; the changed files type-checked against Chromium 156's headers (`tools/compile_check`) | Pass (built standalone; not yet built in the Chromium tree) |
| The same in a running browser (§4 steps 9 and 20) | Needs a Chromium build; the leak kit's `net.client_cert.*` | **Not yet done** |
| MoltNet fails closed while it is open (§2 *Fail-closed*): the check before opening runs again whenever a setting it reads changes (a policy's proxy, override rules, a leak control, blob URL partitioning, in the MoltNet profile or the regular one); a refusal ends that identity once, as New Identity does, with the open check's message; a window about to open checks again; MoltNet's network contexts only ever get MoltNet's own proxy configuration (none before it: requests wait), whatever the settings say; every other profile and the system's network context keep Chromium's | Unit tests (`molt_ai_unittests`: the check and its messages, the parser for MoltNet's proxy setting, the configuration rule), built standalone with g++ ASan/UBSan and clang `-fno-exceptions`, 11 mutants of the policy all caught; standalone harness: `molt_net_live_guard.cc` compiled unchanged against stand-ins of `Profile`, `PrefService`, `PrefChangeRegistrar` and `ProfileObserver` with Chromium's destruction order (g++ ASan/UBSan, clang `-fno-exceptions`): which profiles are watched, which settings run the check, one end per identity, the regular profile's changes, both profiles' destruction, two identities of one profile; 10 tests, 10 mutants all caught; the changed files (`molt_net_routing.cc`, `molt_net_window.cc`, `molt_net_live_guard.cc`, `proxy_config_monitor.cc`) type-checked against Chromium 156's headers (`tools/compile_check`) | Pass (built standalone; not yet built in the Chromium tree) |
| The same in a running browser: a proxy policy set while a MoltNet window is open closes it, nothing leaves outside Tor meanwhile, MoltNet then refuses to open, and opens again once the policy is gone (§4 step 9) | Needs a Chromium build; the leak kit's `identity.policy_change_closes` | **Not yet done** |
| Ending an identity (§1.3, §2 *Fail-closed*): from that moment its network context refuses every request (`localhost` included), whatever configuration comes later; its windows and the DevTools windows on its pages close, also a page asking "Leave site?" with DevTools open (docked or in a window of its own), and a window that stays open is closed by force; New Identity changes nothing if one still stays | Unit tests (`molt_ai_unittests`: the configuration rule); standalone simulation: the code of `molt_net_window.cc` verbatim against stand-ins (windows whose unload processing shows "Leave site?" and lets DevTools intercept it, DevTools agents, renderer processes, Tor) on a simulated clock, g++ ASan/UBSan, 7 tests, 10 mutants caught; standalone harness: `RefuseAllProxyConfig()` verbatim with Chromium 156's proxy-rule and bypass-rule code verbatim, 3 tests, 3 mutants caught; standalone harness: the `proxy_config_monitor.cc` hooks and `MoltNetRouting`'s `EndIdentity()`, `HasIdentityEnded()`, `AddIdentityEndedCallback()` and `ProxyConfigFor()` verbatim, next to Chromium 156's monitor, against stand-ins (g++ ASan/UBSan, clang `-fno-exceptions`), 6 tests, 10 mutants caught; the changed files (`molt_net_window.cc`, `molt_net_routing.cc`, `proxy_config_monitor.cc`) type-checked against Chromium 156's headers (`tools/compile_check`) | Pass (built standalone; not yet built in the Chromium tree) |
| MoltNet's languages (§1.10, §2 *Settings isolation*): MoltNet's list sits above the profile's own settings, so neither the `ForcedLanguages` policy, nor the chosen languages it reads from the regular profile, nor `--accept-lang` change it, also after Chromium's language code rebuilds a profile's list; `ReduceAcceptLanguageEnabled` is ignored for MoltNet only | Unit tests (`molt_ai_unittests`): the list and the policy rule (3 tests, built standalone), and MoltNet's settings store with Chromium's real `LanguagePrefs` and a managed `ForcedLanguages`; standalone harness: `FixMoltNetLanguages()` verbatim against a stand-in that layers the settings stores as Chromium 156 does and rewrites the list as `LanguagePrefs` does, 4 tests, 4 mutants caught; the two `chrome_content_browser_client.cc` hooks verbatim next to Chromium 156's code, 2 tests, 2 mutants caught (g++ ASan/UBSan, clang `-fno-exceptions`); the changed files (`molt_net_pref_isolation.cc`, `molt_net_routing.cc`, `chrome_content_browser_client.cc`) type-checked against Chromium 156's headers (`tools/compile_check`) | Partly: the standalone tests pass; the settings-store tests need a Chromium build |
| Incognito's availability (§2): MoltNet refuses to open, with its message, while Incognito is off or replaced by Isolated Mode, opens while it is forced; the live check watches the setting, in both profiles; Chromium's window check and Incognito's URL lists treat MoltNet as Incognito | Unit tests (`molt_ai_unittests`, the live policy's 20 tests, built standalone with g++ ASan/UBSan and clang `-fno-exceptions`); standalone harness: `molt_net_live_guard.cc` compiled unchanged against stand-ins, 11 tests, 11 mutants caught, among them the availability setting; standalone harness: the `CanOpenBrowser()` and URL-list hooks verbatim next to Chromium 156's code, for regular, Incognito, Isolated Mode, DevTools, look-alike and MoltNet profiles, 5 tests, 4 mutants caught; `incognito_mode_prefs.cc`, `chrome_policy_blocklist_service_factory.cc`, `molt_net_routing.cc` and `molt_net_window.cc` type-checked against Chromium 156's headers (`tools/compile_check`) | Pass (built standalone; not yet built in the Chromium tree) |
| Per-site connection limits (§2 *Shared network state*): each site gets pools of its own in MoltNet's context, of fixed sizes whatever `MaxConnectionsPerProxy` says, with Chromium's per-host limit; flushes, idle-socket clean-up and `chrome://net-internals` reach every pool, which is named after the proxy, never the site; unused pools go; other contexts and connections without a proxy keep the one shared pool | Unit tests (`molt_ai_unittests`): the size rule (4 tests, built standalone) and Chromium's real `HttpNetworkSession` and pools (4 tests); standalone harness: `client_socket_pool_manager_impl.cc` and the new headers verbatim against stand-ins of the pools (g++ ASan/UBSan, clang `-fno-exceptions`), 5 tests, 11 mutants caught; the changed `net/` and `services/network/` files type-checked against Chromium 156's headers (`tools/compile_check`) | Partly: the standalone tests pass; the real-pool tests need a Chromium build |
| Connection budget and per-site shares (§2 *Shared network state*, *Tor's capacity*): a site's first-party and third-party keys, and its sandboxed pages, share its pools; the one-off keys share one small pool; each pool's limit is fixed (no random room); all pools of both types stay within 768; with the budget full, the next freed connection goes to the waiting site using the fewest and an idle connection of a larger site is closed for it (idle HTTP/2 sessions too, busy ones never); a cancelled or self-limited waiter holds nobody back; one greedy site never makes a polite one wait, and eleven full sites still leave room | Unit tests (`molt_ai_unittests`): the policy (13 tests, built standalone with g++ ASan/UBSan) and the real `HttpNetworkSession` and pools (7 tests); standalone harness: `molt_connection_budget.h`, `molt_per_site_socket_pools.h` and the policy compiled verbatim, `ClientSocketPool`'s hooked functions and `TransportClientSocketPool`'s new methods included verbatim into a stand-in pool with 156's slot logic (requests, connect jobs, idle sockets, releases, stalled groups, HTTP/2 sessions as higher-layered pools), g++ ASan/UBSan and clang `-fno-exceptions`: 12 tests, 16 mutants, 15 caught (the last, waking the largest site first, changes nothing: the admission rule still lets the smallest in first); the changed `net/` files type-checked against Chromium 156's headers (`tools/compile_check`) | Partly: the standalone tests pass; the real-pool tests need a Chromium build |
| SOCKS handshake slots (§2 *Tor's capacity*): a site's 17th handshake waits, without a connection to Tor and with no timer, and reports "resolving host" so the pool starts no backup job; another site's doesn't wait; a finished, failed or dropped handshake gives its slot back; a dropped waiting job leaves the queue; a job's raised priority moves it up; only MoltNet's sessions and a loopback SOCKS5 proxy are limited | Standalone harness: `socks_connect_job.{h,cc}` and `molt_connection_budget.h` compiled unchanged against stand-ins of `ConnectJob` (156's base behaviour), the connection to the proxy and the SOCKS5 socket (g++ ASan/UBSan, clang `-fno-exceptions`): 5 tests, 8 mutants caught (the harness found that a waiting job first reported itself as idle, which would have started backup jobs; fixed); unit tests (`molt_ai_unittests`, `MoltSocksHandshakeLimiter` on a real task environment, 2 tests) | Partly: the standalone tests pass; the unit tests need a Chromium build |
| Tor's capacity (§2 *Tor's capacity*): the torrc states `ConnLimit 1000` and `MaxClientCircuitsPending 64`; MoltNet's budget plus 200 fits what Tor accepts; no one site can fill the pending-circuit limit | Unit tests (`TorrcBuilderTest`, built standalone); `tor --verify-config` with Tor 0.4.9.11 on the torrc `BuildTorrc()` writes; Tor 0.4.9.11 under a descriptor limit of exactly 1000 (only a local bridge, so nothing left the machine): 965 SOCKS connections answered, then "Failing because we have 967 connections already"; the leak kit reads both options from the torrc MoltBrowser started Tor with (`identity.tor_capacity`) | Pass |
| Keepalive limits per site (§2 *Keepalive requests*): in MoltNet one site's 2048 in flight leave another site's, and a regular window's, alone, and a regular profile's leave MoltNet's alone; the per-page limits (256, 512 KiB) still hold; a sandboxed page counts against its site; a context's counts go with it and its factories and loaders, destroyed after it, find them gone | Standalone harness: `keepalive_statistics_recorder.{h,cc}`, `molt_keepalive_partitions.h` and `molt_keepalive_site.h` compiled verbatim, the hooked parts of `URLLoaderFactory` (constructor, destructor, `CreateLoaderAndStart()`'s limit checks) and `NetworkContext` (constructor, destructor) included verbatim into stand-ins with 156's ownership (g++ ASan/UBSan, clang `-fno-exceptions`): 5 tests, 8 mutants caught; unit tests (`molt_ai_unittests`: the registry, 3 tests, built standalone; the network context policy); the changed `services/network/` files type-checked against Chromium 156's headers (`tools/compile_check`) | Partly: the standalone tests pass; the network service's own suites need a Chromium build |
| Storage per site (§2 *Storage*, [FINGERPRINTING.md](FINGERPRINTING.md#moltnet-storage-limits)) | See FINGERPRINTING.md | Partly |
| The same in a running browser: language policies change nothing in MoltNet; Incognito turned off closes MoltNet with DevTools open in a window of its own and an unanswered "Leave site?", MoltNet refuses to open, and opens again once allowed; Incognito only keeps it open (§4 step 9) | Needs a Chromium build; the leak kit's `identity.language_policy_ignored`, `identity.incognito_off_closes`, `identity.incognito_forced_allows` | **Not yet done** |
| No Autofill server traffic and no address-format downloads in MoltNet (§2 *Requests outside Tor*): no query and no vote (Autofill's or the password manager's), also with `AutofillServerCommunication` turned on; no address rules downloaded or read from the shared file, for Autofill or the payment sheet; every other profile keeps Chromium's | Unit tests (`molt_ai_unittests`); `molt_net_autofill.cc` and `chrome_payment_request_delegate.cc` type-checked against Chromium 156's headers, and the two hooked functions of `chrome_autofill_client.cc` too (copied verbatim into a stand-in of the class: the checker can't generate one header the whole file needs) | Pass (built standalone; not yet built in the Chromium tree) |
| The same in a running browser (§4 step 20) | Needs a Chromium build; the leak kit's `net.autofill_server` | **Not yet done** |
| Isolation, fail-closed behaviour, leak controls, settings isolation, New Identity, per-site circuits in a running browser | Needs a Chromium build | **Not yet done** |
| An automated leak and uniformity test of a built browser (§4, FINGERPRINTING.md) | `tools/leak_test/` ([LEAK_TESTING.md](LEAK_TESTING.md)): its analysis checked against a recorded sample run; the harness run against stock Chromium, where it finds the WebRTC, DNS-prefetch, mDNS, onion and LAN leaks that MoltNet closes | Kit ready; **not yet run on a MoltBrowser build** |
| Independent third-party audit | — | **Not done** |

## 4. Check it yourself (do this on every release build)

Do all of this in a MoltNet window unless the step says otherwise.

**Most of it is automated.** On Linux, `sudo tools/leak_test/leaktest.sh
run --browser <moltbrowser>` runs a MoltNet window in a network namespace
where only Tor's traffic may leave, captures every packet, drives the steps
below through the DevTools pipe and writes a report
([LEAK_TESTING.md](LEAK_TESTING.md)). Each step says what the kit covers.
"Automated" means the kit checks the whole step; "partial" means the rest
of the step is still done by hand as written; "manual" means the kit doesn't
help. The last column names the report's checks.

| Step | Kit | Checks |
|---|---|---|
| 1 | partial (needs an exit to the public internet) | `moltnet.on_tor` |
| 2 | partial | regular-window tests reach the kit's local pages directly |
| 3 | replaced | the network tests below exercise the same mechanisms locally |
| 4 | automated | `harness.firewall`, every `net.*` check |
| 5 | automated | `identity.fail_closed`, `identity.no_restore` |
| 6 | automated | `identity.separate`, `identity.session_circuits` |
| 7 | automated (exit IP only online) | `identity.new_identity`, `identity.session_circuits` |
| 8 | automated | `identity.per_site_circuits`, `identity.sandboxed_circuits`, `identity.cert_fetch_circuits` |
| 9 | partial | `identity.override_refused`, `identity.partitioning_override_refused`, `identity.policy_change_closes`, `identity.language_policy_ignored`, `identity.incognito_off_closes`, `identity.incognito_forced_allows` |
| 10 | automated (offline, against Tor's consensus) | `identity.relays_real` |
| 11 | partial | `net.onion_http.*`, `net.onion_https.*` |
| 12 | automated | `net.regular_onion_*`, `regular.onion_interstitial` |
| 13 | partial (no service worker) | `regular.proxy_no_onion` |
| 14 | manual | |
| 15 | automated (seeded settings) | `uniform.nav.doNotTrack`, `uniform.css.default_font_size`, `uniform.hdr.accept-language`, `uniform.cookies.third_party`, `uniform.js.enabled_on_blocked_site`, `uniform.perm.geolocation` |
| 16 | partial | `net.extension.*` |
| 17 | partial | `uniform.autoplay.mediaelement`, `uniform.storage.persist`, `uniform.cookies.third_party`, `uniform.sharedStorage.set` |
| 18 | automated | `net.webrtc.*` |
| 19 | manual | |
| 20 | partial | `net.download.*`, `net.geolocation.*`, `net.builtin_ai.*`, `net.client_cert.*`, `net.autofill_server` |

1. **You are on Tor.** (Kit: partial.) Open `check.torproject.org`: it must say you are using
   Tor and show an IP that isn't yours. The panel's *Verify* must show the
   same exit IP.
2. **Regular windows are untouched.** (Kit: partial; the toolbar label is manual.) In a regular window, the same page must
   show your normal IP and "not using Tor"; the toolbar shows "MoltNet", not
   "Tor".
3. **No IP, DNS, WebRTC or IPv6 leak.** (Kit: replaced by its network tests; these sites remain an optional cross-check.) ipleak.net, browserleaks.com/ip,
   browserleaks.com/webrtc, dnsleaktest.com (extended), test-ipv6.com: none may
   show your real IPv4/IPv6 address, your ISP's DNS resolver, or a local IP.
4. **Watch the wire (decisive).** (Kit: automated, for the whole run.) While loading a site in a MoltNet window:
   `sudo tcpdump -n host <that site's IP>` must show nothing, and
   `sudo tcpdump -n udp port 53 or udp port 443` must show nothing caused by the
   page; only the Tor guard (or bridge) should be contacted.
5. **Fail-closed.** (Kit: automated by killing Tor; turning MoltNet off from a regular window is manual.) Turn MoltNet off from a regular window, or kill the `tor`
   process: pages in an open MoltNet window must fail with a proxy error,
   never load directly. Restart MoltBrowser: no MoltNet window comes back.
6. **Separate identity.** (Kit: automated.) Log in to a site in a regular window; open it in a
   MoltNet window: you must not be logged in. Set a cookie in a MoltNet window,
   close all MoltNet windows, open a new one: the cookie must be gone. Open
   the same site again in it: its connections must carry a SOCKS password
   that the closed windows didn't use (new circuits; the kit compares a
   digest of the passwords in its capture), and an IP-echo site normally
   shows a different exit IP than before.
7. **New Identity.** (Kit: automated.) Note the exit IP, press *New Identity*: the MoltNet windows
   close, a new one opens, cookies are gone, and the exit IP normally changes.
   As in step 6, the same site's connections carry a new SOCKS password.
   Click into a page that asks "Leave site?" (e.g. one with an unsent form)
   and press *New Identity*: its window closes without asking. Again with
   DevTools open on that page (docked, then in a window of its own), and
   with a page showing an alert (`alert(1)` in the console), a sign-in
   prompt (a test server that answers `401` with `WWW-Authenticate:
   Basic`), print preview, a download's "Save as" dialog ("Ask where to
   save each file" on) or a file chooser: every MoltNet window and DevTools
   window closes, without asking, and the new window opens. Press *New
   Identity* twice in a row: the second new window opens only about ten
   seconds after the first (Tor switches circuits at most that often).
8. **Per-site circuits.** (Kit: automated, from the SOCKS usernames in the capture.) Open two different sites in MoltNet tabs and compare
   the exit IP each one sees (e.g. two different IP-echo sites): they should
   normally differ. In Tor's log with `SafeLogging 0` + `Log info`, streams for
   different sites carry different SOCKS usernames, also for pages served
   with `Content-Security-Policy: sandbox` (opaque origins). Certificate
   fetches too: open two sites whose servers leave out the intermediate
   certificate and whose certificates name the same AIA URL. The two
   fetches of that URL carry two different usernames, neither of them a
   site's, and the server that holds the intermediate gets two full
   requests. Certificate checks aren't shared either: with
   `chrome://net-export` running, open a site that embeds an image from a
   third-party HTTPS host, then a second site that embeds the same host;
   the log shows a certificate verification for each site's connection to
   it (a regular window verifies it once and answers the second from its
   cache).
9. **Overrides are refused.** (Kit: the `--proxy-server` half, the partitioning switches, and a proxy, a language and an Incognito policy set while MoltNet is open; the extension half and the URL allow list are manual.) Start MoltBrowser with `--proxy-server=...` (or a
   policy proxy): opening a MoltNet window must fail with an error. A proxy
   set by an extension doesn't reach MoltNet: with one installed and
   allowed in Incognito, MoltNet must open and step 1 must pass. Start it
   with `--disable-features=PartitionConnectionsByNetworkIsolationKey`,
   then with `SplitCacheByNetworkIsolationKey` and with
   `ThirdPartyStoragePartitioning` instead: each time, opening a MoltNet
   window must fail with an error that names the switch, and regular
   windows must work. Set the `ProxyOverrideRules` policy (any rule, e.g.
   one that sends `*.example.com` `DIRECT`) and restart: opening a MoltNet
   window must fail with the proxy error, and the MoltNet panel must say
   that the proxy is overridden. Then the same while MoltNet is open:
   open a MoltNet window, keep a page in it loading something (a test
   page that fetches a URL of your test server every second), and set a
   proxy policy (on Linux, write `{"ProxyMode": "direct"}` to a file in
   `/etc/chromium/policies/managed/`, which Chromium reads a few seconds
   later; on Windows and macOS, the `ProxyMode` policy, then
   `chrome://policy` → *Reload policies*). Every MoltNet window must
   close, also one whose page asks "Leave site?" with DevTools open on it
   (as for New Identity), a dialog must give the proxy error, no request
   from it may go out directly (watch tcpdump as in step 4; your test
   server sees the page's requests only through Tor until the window
   closes), and opening a MoltNet window must now fail with that error.
   Remove the policy: MoltNet opens again. Repeat with
   `ProxyOverrideRules`, with `WebRtcIPHandling`, and with
   `PartitionedBlobUrlUsage` set to false (the error names it). Then
   `IncognitoModeAvailability`: set to `1` while a MoltNet window is open,
   every MoltNet window and DevTools window must close and MoltNet must
   refuse to open with "Incognito is turned off (IncognitoModeAvailability
   or parental controls), and MoltNet windows follow it"; with
   `IncognitoModeUrlAllowlist` listing a test site as well, MoltNet opens
   and only that site loads; set to `2`, MoltNet stays open and opens.
   Last, set `ForcedLanguages` to `["ja"]` and `ReduceAcceptLanguageEnabled`
   to false while a MoltNet window is open: it stays open, and
   `navigator.languages` and the `Accept-Language` header there don't
   change (regular windows follow the policy).
10. **The relays are real.** (Kit: automated against the consensus Tor downloaded.) *Copy route evidence* in the panel, then look up
    each fingerprint on metrics.torproject.org or
    `onionoo.torproject.org/details?lookup=<fingerprint>`.
11. **Onion sites.** (Kit: the `http://` and `https://` loads; the Onion-Location bar is manual.) In a MoltNet window, open an onion address both as
    `https://` and as `http://`: both must load, with no HTTPS-Only warning
    for `http://`. Open `https://www.torproject.org`: a bar must offer the
    onion version, and nothing may change until you click it. A test page
    served over `https` without the header but with `<meta
    http-equiv="onion-location" content="http://<v3 onion>/">` in its
    `<head>` must get the same bar; with two such elements, or with the
    element in `<body>`, no bar.
12. **No onion names outside Tor (regular window).** (Kit: automated; every lookup goes to the kit's resolver, so DoH needs no separate check.) Run
    `sudo tcpdump -n -l port 53 | grep -i onion`, then type an onion address
    in a regular window's address bar and press Enter: the "Tor onion site"
    page must appear and tcpdump must print nothing. The button must open the
    address in a MoltNet window. With DNS-over-HTTPS on, no DoH request may be
    made for it (check `chrome://net-export`).
13. **No onion names to a proxy (regular window).** (Kit: automated with its logging proxy for the image, fetch, redirect, WebSocket, dedicated and shared worker cases; the service worker case is manual.) Start a logging proxy
    (e.g. `mitmdump -p 8888`) and MoltBrowser with
    `--proxy-server=http://127.0.0.1:8888` (MoltNet then refuses to open,
    see step 9; this step is about regular windows). Open a test page that
    loads `<img src="http://<onion>/x.png">`, calls
    `fetch("http://<onion>/")`, loads an image from a URL that redirects to
    an onion address, opens `new WebSocket("ws://<onion>/")` and does the
    same from a dedicated worker, a shared worker and a service worker: the
    proxy must log none of them, and
    DevTools must show the requests failing with `ERR_BLOCKED_BY_CLIENT`.
14. **Bridges with your distribution's transports (Linux).** (Kit: manual.) Install
    `obfs4proxy` (or `lyrebird`) and `snowflake-client`, paste obfs4 bridges
    from bridges.torproject.org under *Custom bridges*, and turn MoltNet on:
    `~/.moltbrowser/tor/torrc` must contain `ClientTransportPlugin obfs4 exec
    /usr/bin/...`, Tor's log (`~/.moltbrowser/tor/tor.log`) must show it
    using the bridge, and step 1 must pass. Repeat with snowflake bridges.
    Remove the package: settings must name the package to install, and
    turning MoltNet on must fail with that message rather than connect
    without bridges.
15. **Your settings don't follow you.** (Kit: automated with settings seeded into the regular profile: Do Not Track, font size and family, language, third-party cookies, a site with JavaScript and location blocked. Notifications, Shields, application links and changing a setting while the window is open are manual.) In a regular window, on a test
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
    On a test page with a link to an application's URL scheme (e.g.
    `zoommtg:`; not `mailto:`, which Chromium always opens), tick "Always
    allow ... to open links of this type" in a regular window and open it;
    the same link on the same site in a MoltNet window must ask again
    before opening the application.
16. **Extensions stay out.** (Kit: partial: content script, web-accessible resource, `chrome.runtime`, `tabs` and `webRequest` events; the rest is manual.) In a regular window install a test extension
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
17. **No regular-profile carry-over.** (Kit: partial: it reads the MoltNet values, but doesn't prepare media engagement, a bookmark or a certificate exception.) In a regular window, watch a video
    with sound on a site until `chrome://media-engagement` lists it as
    high; on that site in a MoltNet window,
    `navigator.getAutoplayPolicy("mediaelement")` must be `"allowed-muted"`.
    Bookmark a site in a regular window; there, in MoltNet,
    `await navigator.storage.persist()` must be `false`. Click through a
    self-signed certificate warning in a regular window; the same site in
    MoltNet must warn again. With third-party cookies allowed in regular
    settings, a cross-site iframe in MoltNet must not get its cookies, and
    `window.sharedStorage` must be absent (Shared Storage: removed
    upstream at 156).
18. **No WebRTC, and no name lookups for it.** (Kit: automated, with the TURN and STUN servers also given as IP literals.) Pick two names nobody has
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
19. **Incognito's protections.** (Kit: manual.) In a MoltNet window:
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
    - With the `WebRtcDiagnosticLogCollectionAllowedForOrigins` policy
      listing a test site, start WebRTC diagnostic logging from that site
      in a MoltNet window: nothing appears under `<profile folder>/WebRTC
      Logs` and nothing is uploaded (in a regular window a log appears).
    - With `IncognitoModeUrlBlocklist` listing a test site, it doesn't
      load in a MoltNet window, as in Incognito (a regular window loads
      it).
    - With `--enable-features=ReduceAcceptLanguage`, fingerprinting
      protection off and two or more languages in your settings, a site
      that answers with `Avail-Language` gets the same language from a
      MoltNet window as from an Incognito one.
    - On a page that uses XSLT, click "Never show again" on the
      deprecation banner in a regular window: the banner still shows in
      a MoltNet window; clicking it there doesn't hide it in a new
      MoltNet session.
20. **Nothing goes around Tor.** (Kit: the download, geolocation, the built-in AI APIs and Autofill's server queries; the AI chat, Cast, passkeys, bookmarks and address formats are manual.) Keep
    `sudo tcpdump -n 'not host <Tor guard or bridge IP>' and not port 53`
    and a DNS log (as in step 18) running, connect an AI provider in a
    regular window (molt://ai-settings) and make it the default model,
    then in a MoltNet window:
    - Open the AI chat side panel: the model list shows no cloud model,
      and a message sent with the cloud default says "Cloud AI is off in
      MoltNet windows. Use an on-device model." `/sandbox
      https://check.torproject.org` opens a MoltNet window that says you
      are using Tor. Asking it to watch the page is refused. With a
      regular window focused last, a link in a reply still opens in the
      MoltNet window.
    - Download a small `.exe` or `.dmg` from a test site: no lookup of
      `sb-ssl.google.com` in the DNS log, nothing in tcpdump.
    - In the console, `navigator.geolocation.getCurrentPosition(p =>
      console.log(p), e => console.log(e.code, e.message))` prints `1 User
      denied Geolocation` without a prompt.
    - The ⋮ menu has no *Cast…* entry and a video's controls have no cast
      button.
    - On webauthn.io, *Authenticate* doesn't offer a phone or tablet.
    - Make a bookmark folder with `chrome://settings` first and two sites
      after it, then *Open all* from the MoltNet window: Settings opens in
      a regular window, both sites in the MoltNet window.
    - With "Use a web service to help resolve navigation errors" on in a
      regular window, open `https://expired.badssl.com`: the certificate
      warning, no request to `www.gstatic.com/generate_204` or
      `connectivitycheck.gstatic.com` (in Tor's log either), and no
      sign-in tab or window. On Windows behind a real captive portal the
      page may offer *Connect*; it opens nothing.
    - A test server that answers `401` with `WWW-Authenticate: Negotiate`,
      `NTLM` and `Basic` gets a Basic sign-in prompt; one that offers only
      `Negotiate` and `NTLM` gets no prompt (its 401 page shows), and on
      Windows the DNS log shows no lookup of its name.
    - A test server that asks for a TLS client certificate without
      requiring one (e.g. `openssl s_server -verify 1`), with a client
      certificate installed in your system or browser: the page loads
      with no picker, the server sees an empty certificate, and each new
      connection to it, under any site, sees exactly one handshake (in a
      regular window the first one is dropped and repeated after the
      picker). One that requires a certificate (`-Verify 1`) shows a TLS
      error page.
    - Set secure DNS to a provider in Settings (e.g. Cloudflare) and
      restart, then open a MoltNet window, and press *New Identity*:
      tcpdump shows no connection to that provider's addresses outside
      Tor while it happens.
    - Start MoltBrowser with `--enable-features=AutofillServerCommunication`
      and Tor's log at `SafeLogging 0` + `Log info`. In a MoltNet window,
      open a page with an address form and a sign-in form, fill and submit
      them: Tor's log shows no stream to `content-autofill.googleapis.com`
      (in a regular window the same page queries it). With a saved
      address whose state is spelled differently from the page's state
      list (e.g. "Calif." against "California"), fill the form in a
      MoltNet window: no lookup or stream of `chromium-i18n.appspot.com`,
      in tcpdump or in Tor's log.
    tcpdump must show nothing caused by these steps (the browser's own
    background traffic, §5.6, can appear), and the DNS log no lookup of
    any site you used. In a regular window the same download shows the
    `sb-ssl.google.com` lookup, as in Chromium.
21. **Shared limits stay per site.** (Kit: Tor's options and the storage
    quota; the rest is manual, see `docs/BUILD_MACHINE_CHECKLIST.md`,
    "moltnet-shared".) With Tor's log at `Log info`, in MoltNet windows:
    - A test page on site A that opens 1000 long-polling requests to 200
      hosts of its own, and a page on site B in another tab: B loads at
      once. `chrome://net-internals/#sockets` shows A's pool at 48 (its
      WebSockets' at 16). Tor's log shows no "Failing because we have"
      line.
    - A page that connects to 100 onion addresses or unusual ports at
      once: Tor's log may say it waits for pending circuits; a page of
      another site opened meanwhile loads (its first connection may take
      a little longer).
    - A page on site A that keeps 2048 `fetch(..., {keepalive: true})`
      requests in flight (to a server that answers slowly) from nine
      pages: the 2049th fails; site B's, and a regular window's, still
      go out.
    - Write to IndexedDB until `QuotaExceededError`: about 60 MiB per
      site, a subdomain's iframe on that site counting toward it; another
      site still gets its 60 MiB. Store a little on one site, then fill
      nine others: after more than 35 minutes (Chromium evicts every half
      hour) the first site's data is still there.

## 5. What MoltNet does *not* do

1. **It is not Tor Browser.** MoltNet windows letterbox the page and report
   the same time zone (UTC), language (en-US), screen size, pixel ratio, CPU
   count, memory, WebGL limits and installed fonts (per operating system)
   for everyone, and canvas, WebGL and audio readback carry per-site noise
   (docs/FINGERPRINTING.md). But the operating system is visible (in the
   user agent and through the fonts it ships), the protection hasn't been
   built and tested against real fingerprinting scripts yet, and Tor
   Browser's design has had years of public review that MoltNet's hasn't.
   Assume sites may still recognise and link your visits. If your safety
   depends on not being identified, use Tor Browser.
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
   - A few things a MoltNet window can cause still happen outside Tor
     (the audit is in §2, *Requests outside Tor*; it was a code review,
     not yet a capture on a built browser):
     - When a site in a MoltNet window has an expired or not-yet-valid
       certificate and the browser hasn't checked its clock recently, it
       may ask Google's time server (`clients2.google.com`) for the time.
       The request carries no address or page data, but its timing could
       tell a watcher that you just met a certificate error.
     - On Windows 11 the system's own passkey dialog can offer your phone;
       Windows makes that connection itself.
     - The AI settings page, opened in a MoltNet window, checks a cloud
       provider's API key through Tor, but the key still names your
       account. Use a regular window for settings.

     (Translating a page used to download Google's translate script
     outside Tor if you gave MoltBrowser your own `GOOGLE_API_KEY`. It
     now runs on your device and sends nothing, and it is off in MoltNet
     windows.)
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
     what clears MoltNet). Of the policies an administrator or parent sets
     for Incognito, MoltNet follows whether Incognito may open (also for a
     supervised user) and Incognito's URL allow and block lists (§2
     *Incognito's availability*), but not extensions required for
     Incognito browsing or the "Incognito" conditions of data-control
     rules. Chromium 156's Enterprise Isolated Mode, which a policy can put
     in Incognito's place, applies to that session; a MoltNet window is
     never one, and MoltNet doesn't open while Isolated Mode replaces
     Incognito.
   - Local servers don't open in MoltNet windows: `localhost`, loopback
     and link-local addresses go to Tor, which refuses them (see §2). Use
     a regular window for a local development server.
   - Sites that require a TLS client certificate (a smart card, an ID
     card, a company or government certificate) don't open in MoltNet
     windows: MoltNet never sends one (§2 *Shared network state*). Sites
     that only ask for one open without it. Use a regular window for
     them.
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
     settings, and MoltShield's on/off switch and its filter lists.
     `prefers-color-scheme` is dark for everyone while MoltBrowser's dark
     toolbar is on (the default); with it off, MoltNet windows follow your
     regular windows' theme.

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
