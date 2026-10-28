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
   stays with that site, and what one site uses of a limit sites share
   (connections, the circuits Tor builds, storage, blob memory, renderer
   processes) doesn't show to another (§2 *Shared network state*). Pages
   visited in MoltNet windows are not added to MoltBrowser's on-device
   memory index.
3. **New Identity.** Ends the current identity: at once the browser
   refuses its navigations and its network contexts are sent the end.
   From the moment a network context has it, it starts no request and
   sends nothing more to Tor, whichever page, worker or pipe a request
   comes from, and it stops what it had in flight. Once every context has
   it, the identity's pages stop (their renderer processes end); once
   each confirms the refusal, the identity's connections are dropped
   again and its MoltNet windows close, with any DevTools window open on
   their pages. Nothing open in a
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
   never put on a site's circuit. It counts against the connection limits
   of the site whose connection needed it.
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
    regular window for them. Screen, window and tab capture
    (`getDisplayMedia()`) is refused in MoltNet as if you had cancelled the
    picker, with no picker shown, and its permission always reads "denied"
    ([details](FINGERPRINTING.md)).
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
  is an empty proxy list, and the context ends with it (*After an
  identity ends*): every request it is then asked to make fails before
  it connects (`ERR_ABORTED`; one routed anyway, `ERR_NO_SUPPORTED_PROXIES`),
  `localhost` included, and no later configuration replaces it.
- MoltNet watches every setting (b) reads (the proxy, proxy override
  rules, WebRTC IP handling, network prediction, HTTPS-Only mode, Safe
  Browsing, blob URL partitioning) and Incognito's availability (below),
  in its own profile and in the regular profile it was made from, and
  checks again whenever one changes. If MoltNet would now refuse to open,
  it ends that identity as New Identity ends the old one: its network
  contexts refuse every request from the moment each has the end, its
  pages stop once they all have it (below), it drops its connections,
  its windows and the DevTools
  windows open on its pages close whatever they show (§1.3; a window
  still open two seconds later is closed by force, its pages stopped, and
  again up to five times), and
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
leaves outside Tor, and once a network context has the end it sends
nothing more, through Tor either. The end reaches the network before
anything else of the identity learns of it
(`molt_net_identity_end_policy.h`, `MoltNetRouting::WhenNetworkHasEnd()`
and `WhenIdentityRefuses()`):

1. At once, the identity is marked ended: the browser refuses its
   navigations, whatever stage they are at, and every request it holds
   for the identity's pages (`MoltNetEndedIdentityThrottle`, *After an
   identity ends*).
2. At once, each of its network contexts (every storage partition's) is
   sent the refusal and, on the same pipe, a request to answer once it is
   applied. A context applies it and ends in one task (*After an identity
   ends*): from then on it starts nothing, whichever pipe a page's or the
   browser's request comes on, and writes nothing more to Tor.
3. Until every context has answered, nothing else happens to the
   identity. Its pages keep running for that one round trip; what they
   send meanwhile is refused by the context, or stopped at its end.
4. Then its renderer processes end, so its pages, frames and workers make
   no further request.
5. MoltNet asks each network context how it would route a request, every
   10 ms, until each answers with the refusal; then it drops the
   identity's connections again, detaches DevTools, closes the windows and
   goes on with New Identity.

What a request already wrote to Tor before the end can't be recalled: it
finishes or fails on its Tor connection, as a request already sent when a
tab closes does (the end closes the connection). If the network doesn't
answer within 5 seconds (the network service gone or hung), the end
carries on: a network context the service makes again gets the refusal
from the start and starts ended, and New Identity still waits until the
identity's profile, and with it every network context, is destroyed.
What it can't guarantee: that nothing at all is sent between a setting's
change and the end.
What a server already received can't be recalled. MoltNet also reacts when Chromium
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
| Incognito off (`IncognitoModeAvailability` 1, `IncognitoEnabled` false, a supervised user, Windows parental controls) | doesn't open | doesn't open: "Incognito is turned off (IncognitoModeAvailability or parental controls), and MoltNet windows follow it". If the setting changes while MoltNet is open, MoltNet ends (*Fail-closed*). Windows parental controls are read again each time a MoltNet window opens (below) |
| Incognito off, with `IncognitoModeUrlAllowlist` | opens; only the listed sites load | the same |
| Incognito only (`IncognitoModeAvailability` 2) | the only window that opens | opens (regular windows don't) |
| Isolated Mode replaces Incognito (Chromium 156) | an Isolated Mode window instead | doesn't open: "Your administrator replaced Incognito with Isolated Mode, and MoltNet windows follow it". Chromium reads this setting once per run, so it can't change while MoltNet is open |
| `IncognitoModeUrlBlocklist`, `IncognitoModeUrlAllowlist` | apply | apply |
| Extensions required in Incognito (`MandatoryExtensionsForIncognitoNavigation`) | every navigation blocked while a listed extension is missing or not allowed in Incognito | doesn't open while the policy lists any extension: MoltNet windows run none (*Nothing from the regular profile (extensions and services)*), so a required one could never run there: "Incognito requires extensions (MandatoryExtensionsForIncognitoNavigation), and MoltNet windows run none". If the policy starts to list one while MoltNet is open, MoltNet ends |
| Data-control rules (`DataControlsRules`) with an `incognito` source or destination condition | match Incognito tabs | match MoltNet windows too; text copied in a MoltNet window is labelled Incognito's for content analysis, so it carries no URL of the MoltNet page into a regular window's report |

The open check reads it with the other settings ((b) above), and the
live check watches the availability setting and the required-extensions
policy. Windows offers no notification when its parental controls
change, and Chromium reads them once per run; MoltNet reads them again,
uncached and off the UI thread, each time one of its windows opens
(New Identity's included), and doesn't open while they require activity
reporting (`molt_net_parental_controls.h`, through
`MoltReadWinParentalControlsNow()` in `chrome/browser/win/parental_controls.cc`).
Data-control rules count MoltNet as Incognito (`chrome_rules_service.cc`,
`chrome_clipboard_context.cc`). Two Chromium checks treated
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
- *In the network service*, MoltNet's network context makes no WebRTC
  socket manager either: one asked of it (no path in MoltBrowser does)
  gets the same kind of stand-in, which keeps the renderer's connection
  open, as the renderer would ask again at once for a closed one
  (`services/network/molt_refusing_p2p_socket_manager.h`; *Circuits*
  below).

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
Chrome Apps, which MoltNet windows can't host, and MoltNet's network
context makes no raw socket anyway (*Circuits*). Chromium's built-in proxy
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
| Background Fetch's Local Network Access check (`BackgroundFetchLocalNetworkAccess`, on at 156) | Background Fetch requests get a URL loader factory from the service worker's storage partition, MoltNet's for a MoltNet page, so they go through Tor, keyed by the site that started them (*Shared network state*); the new check reads the URL or the connection's address only; a request resumed or taken up again without the worker's factory is checked too (*Downloads and Background Fetch*) |
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
  to the administrator's data-control rules (for pasted content). The
  data-control conditions, the extensions required in Incognito,
  Incognito's availability and its URL lists all cover MoltNet
  (*Incognito's availability*, above).

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

Every connection MoltNet's network context makes comes from its own HTTP
network session, which carries the password, the per-site socket pools
and the identity's end (*After an identity ends*). The network service
has one kind of socket that builds a session of its own from Chromium's
defaults instead (`ProxyResolvingClientSocketFactory`: at Chromium 156
made for GCM, which the browser asks of the browser-wide context and of a
profile's default partition, and for WebRTC's TCP sockets): through the
same proxy, but with the fixed password, shared pools, and no end. No
MoltNet feature uses it, and a network context with any of MoltNet's
session settings makes none: asked for the factory, it closes the pipe,
and asked for a WebRTC socket manager, it binds a stand-in that reaches
nothing (`services/network/molt_own_session_sockets_policy.h`, hooked
into `NetworkContext`). The network context's raw sockets have neither a
session nor the proxy: TCP connected, server and bound sockets, UDP and
restricted UDP sockets, the socket factory that hands out the same, and
the multicast DNS responder, which the browser asks for Direct Sockets,
Chrome Apps' socket APIs, Cast and WebRTC's local names. No MoltNet window
has any of these, and MoltNet's network context refuses them too: those
that wait for an answer get `ERR_ACCESS_DENIED`, with no address and no
data pipes, and the others' pipes are closed. Nor does that context look
up a name itself: the network context's own name lookups
(`ResolveHost()`, `CreateHostResolver()`) use its host resolver, the
system's or Chromium's built-in one with your secure-DNS servers, outside
Tor. At Chromium 156 the browser asks for them for net-internals' DNS
lookup, DevTools' network targets, the error page's DNS probes and the
preconnect service, none of them for a MoltNet page's names; MoltNet's
context answers every lookup, from any source, with `ERR_ACCESS_DENIED`
and closes a host resolver's pipe, so only Tor resolves names for it.

**Certificate fetches.** To check a site's certificate the browser may
download a missing intermediate certificate (AIA) and, with online
revocation checks on, an OCSP response or a CRL, from addresses the
certificate names. Chromium files each such download under the address's
own site, whichever site's certificate needed it, and MoltNet's per-site
circuits follow that key. In MoltNet each one gets a one-off key instead:
nothing is kept in the HTTP cache for it, it opens its own connection on
its own Tor circuit, and two identical downloads in flight at the same
time are made separately. The key names the site whose connection needed
the check, with a nonce of its own: the TLS connection gives its
verification its site, and the verification carries it through the
certificate verifier service to its fetcher (`net/cert/molt_cert_fetch_site.h`,
a `[MinVersion=1]` field of the verifier's request), so the download counts
against that site's connections (in a small pool of the site's own, in
a part of the connection budget kept for such pools, so that it never
waits for a slot that connections waiting for their checks hold, the
site's or any other site's) and circuit-building slots like the site's own
connections, and shares nothing with them
(`services/cert_verifier/cert_net_url_loader/molt_cert_fetch_isolation.h`).
A verification that names no site gets a key derived from nothing, in the
certificate-fetch pool of 4 those keys share. The switch is set per network context, for
MoltNet's only (`molt_net_network_context_policy.h`, read by the
certificate verifier's fetcher in `services/cert_verifier/`); every other
profile keeps Chromium's behaviour. Verifying without downloading
intermediates, as Firefox and Tor Browser can with a preloaded list of
them, isn't available: Chromium 156's root store carries roots and their
constraints, not intermediates, so a site that leaves out its
intermediate would fail.

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
| HTTP cache | keyed by top-level site and frame site; one-off keys (certificate fetches, opaque origins) are never cached. Chromium 152 files a built-in list of well-known scripts and styles under one entry shared by every site where cookie settings allow full cookie access; MoltNet's context never does. **Size per site**: Chromium's in-memory cache has one size limit and evicts the least recently used entries of any site; MoltNet's context gives each site 10 MiB of its own, whose entries are evicted only by that site's own use or after 30 minutes unused, found on a list of the site's own entries (*HTTP cache* below) | gone (in memory, with the context) |
| Code cache | none: off-the-record profiles have no code cache | — |
| Idle sockets, HTTP/2 sessions | keyed by top-level site; each site's connections also go to Tor with that site as SOCKS username, so on circuits of their own (§1.4) | closed, then gone |
| Connection limits | **per site**. Chromium gives each proxy one pool of connections, 128 at most for all sites together (the `MaxConnectionsPerProxy` policy changes the number; at 156 the pool may also grow past it by a random amount), so a site that kept many connections open made other sites' requests wait, and could count how many the others held. MoltNet's context gives each site (the top-level site, for its first-party and third-party requests alike; a page its server sandboxed counts against the site it came from) pools of its own: 48 connections for HTTP and 16 for WebSockets, whatever the policy says and with no random room, with Chromium's 6 per host inside. A certificate fetch counts against the site whose connection needed it (*Certificate fetches*), in a pool of 4 of that site's own, apart from its 48, and in a part of the budget kept for those pools: the connections waiting for their certificate checks hold slots of the 48 and of the budget's other part, and the fetches the checks wait for never need one of them. A credentialless iframe's one-off keys count in its site's 48. The one-off keys no site can be named for share one pool of 8, and their certificate fetches one of 4. A synchronous `XMLHttpRequest`, which Chromium lets past every limit (its thread waits for it, so it must not wait behind the page's own requests), skips only the 6 per host in MoltNet's pools: it counts against its site and waits for the site's 48 and the budget like any other request, and once the site holds all 48 it may go past them by 4 at most, which come out of the margin MoltNet leaves Tor (*Tor's capacity*), not out of the budget other sites share. The pools start no backup connections (Chromium's second attempt at a slow connection, which a proxy on the same machine doesn't need) | closed, then gone |
| Connections to Tor, all sites together | **within a budget** of 768, which fits eleven sites' full shares, inside what MoltNet's single Tor process always accepts (*Tor's capacity* below); 48 of it are kept for the certificate-fetch pools, the other 720 serve every other pool. The budget is the Tor process's, shared by two identities alive at once (a new one while the old one closes) | closed, then gone |
| Circuits Tor builds at a time | **within a budget**: a site's SOCKS handshakes in progress, each of which may make Tor build a circuit, count 16 at most (one for each, three for an onion address), all sites together 192, which fits eleven sites' full shares, below Tor's limit of 256 for all of them; a freed slot goes to the waiting site with the fewest in progress (*Tor's capacity*) | — |
| Downloads the browser makes for a page, Background Fetch | **keyed by the site they come from**. A Background Fetch is made by the browser's download service; it carries the network key of the site that started it (its service worker's or its page's), so its connections, cache partition, cookies and Tor circuit are that site's (off-the-record profiles at 156 filed it under its destination, as a top-level site of its own; regular profiles already keyed it by the site). A download a page starts without navigating (a link with `download`, "Save link as", "Save image as", a dragged link) is keyed by the frame it came from, a resumed download by the download's first request, and the resources "Save page as" fetches by the frame they belong to; with no frame left, by a key of its own. A download retried from the download bubble or the downloads page keeps the key of the download it retries. A navigation that turns into a download keeps its navigation's key. A Background Fetch request also carries the fetch's credentials mode (in every profile) and its origin as initiator, as the site's own `fetch()` would | gone |
| Background Fetch scheduling | **per site**. Chromium runs 2 Background Fetch registrations and 2 of their downloads at a time for all sites together, and a registration stays active until it finishes, so one site's slow fetches kept every other site's from starting; MoltNet gives each site those limits of its own, whatever other sites run: no total for all sites together, which a party embedded on many sites could fill (past 128 downloads for all sites, the memory valve watches each further one); the in-memory download service that makes the requests queues none of them (*Downloads and Background Fetch* below) | gone (with the profile) |
| Keepalive requests (`fetch()` with `keepalive`, `sendBeacon()`, pings) | **per site**. The network service lets 2048 be in flight for every profile and site together (and 256 per page), so a site that used them up made another site's, or a regular window's, fail. MoltNet's context counts each site's on their own, 2048 for the site and 256 per page, and none of them toward another context's | none sent from the moment the identity ends, also those the browser holds for a page (*After an identity ends* below) |
| Storage quota and eviction (IndexedDB, Cache Storage, OPFS, service worker scripts) | **per site**: each site's storage keys together may always use 60 MiB, whatever other sites store, and that is the only limit a write meets; nothing is evicted to make room. Past 600 MiB for all sites together, MoltNet watches the machine's memory apart from any write and ends the identity if it runs short (*Storage* below, [FINGERPRINTING.md](FINGERPRINTING.md#moltnet-storage-limits)). Chromium checks writes against each storage key's quota only and, every half hour, evicts the least recently used data of any site until all sites use 70% of a pool. A quota check costs what the site's own storage keys cost, whatever other sites store (*Storage* below) | gone (in memory) |
| Blob memory (`Blob`, `Response.blob()`, `canvas.toBlob()`, recordings) | **per renderer process**: each process's blobs may use 200 MiB whatever the others hold, and that is the only limit a blob meets; a Background Fetch's response, which the browser holds in memory as it downloads and then as a blob, counts to the site that started it from its first byte. Past 800 MiB for all together, MoltNet watches the machine's memory as for storage (*Blob memory* below). Chromium holds all of a profile's blobs within one limit | gone (in memory) |
| `localStorage` and `sessionStorage` caches | **per site**: the storage service keeps the areas it has opened in a cache and, at 156, purges every site's unused ones once all areas together pass one limit (50 areas or 20 MiB); MoltNet's partition keeps them per top-level site, an unused one that holds nothing purged only by that site's own use or after 30 minutes unused, and has no database: an area that holds items stays until they are deleted or the identity ends, and opening one waits on nothing another site queued (*DOM storage caches* below) | gone (in memory) |
| `localStorage` and `sessionStorage` contents | **per site**: Chromium limits each area (an origin's, and for `sessionStorage` a tab's) to 10 MiB, so a site's subdomains and tabs together had no limit, each area committed to the partition's one database at a rate of its own, and a page could send as many writes as it liked; in MoltNet each site's areas together hold one area's limit, for each kind, counting what each item and area takes in memory, nothing is committed (no database), a site's writes come out of a write allowance of its own, and the storage service handles each site's messages in turn (*DOM storage totals*, *The storage service's sequence, in turn per site* below) | gone (in memory) |
| Renderer processes | **one tab each** (a tab and the windows it opened with an opener): a process that serves one is never given to another (*Renderer processes* below). Chromium lets a site's frames and workers share a process across tabs | ended with the identity |
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
`ClientSocketPool`): the certificate-fetch pools ask its part of 48, every
other pool the part of 720, so a certificate fetch never waits for a slot
held by a connection that is itself waiting for a certificate check, even
with every site's pools full of them (a temporary stall, until the
connections time out, otherwise). A request it holds back waits in its pool, as for
the pool's own limit, and gets a connection when one is released
anywhere: the waiting site using the fewest goes first, and no site using
more may take one before it. While the budget is full, MoltNet closes an
idle connection of the site using the most (or its idle HTTP/2 sessions,
as Chromium does for a pool at its own limit), if that site uses more
than one more than the waiting site; a connection in use is never closed.
Requests Chromium lets past a pool's limits (`LOAD_IGNORE_LIMITS`, which a
page sets with a synchronous `XMLHttpRequest`, so that the thread waiting
for it never waits behind the page's own connections to the same host)
skip only the limit per host in MoltNet's pools: they count against their
site, wait for its pool and the budget like any other request (and are
woken like any other when either has room), and once the site's pool
holds its whole limit they may go past it by 4
(`kMoltIgnoreLimitsOverflow` in `molt_per_site_socket_pool_policy.h`,
applied by `TransportClientSocketPool`). Those few aren't counted in the
budget: only connections within a pool's limit are, so at most 15 sites'
HTTP pools (of either identity) can be full at once, and at most 60
connections go past the limits in all, out of the margin left for Tor
(*Tor's capacity*). A site would need 52 connections of its own held at
once to make one of its synchronous requests wait for them. The pools
start no backup connect jobs, and a pool that drops connect jobs on its
own (a network change, a refreshed group) gives their slots back to the
budget. The budgets belong to the one Tor process
(`MoltTorCapacity` in `molt_connection_budget.h`), not to a network
context, and count a site per identity.

What a page can still observe of them: its own site's limits (requests
beyond 6 connections to one host, 48 for the site or 16 WebSockets wait,
and beyond 16 connections being set up at once they start in turn), and
nothing of another site's use while fewer than eleven other sites hold
their full share (ten full sites, with every unattributed connection in
use, still leave a site its whole share; the same holds for certificate
fetches in their part of the budget). Past that, a site below its own limits can see that the
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

**HTTP cache.** MoltNet's HTTP cache is in memory (Chromium's
`MemBackendImpl`), keyed by top-level site and frame site. Chromium gives
it one size limit, 2% of the machine's memory up to 50 MiB, and when a
write passes it, evicts the least recently used entries of any site, also
on memory pressure. A site could then push another site's entries out
and, timing its own reloads, tell what the others had cached. In MoltNet's
context (the `[MinVersion=1]` field `molt_per_site_http_cache`) each
top-level site's entries, parsed from the cache key, have a share of
their own: 10 MiB, with entries of 5 MiB at most. A write that takes a
site past its share evicts that site's own least recently used entries
(down to 90% of the share), never another's. It finds them on a list of
the site's own entries, kept in order of use beside Chromium's list of
all entries, so how long it takes depends on the site's own entries
only, never on how many other sites hold
(`net/disk_cache/memory/molt_mem_backend_site_lru.h`). An entry unused
for 30 minutes is dropped, when it is next opened or by a sweep that runs
every minute whatever sites do. The sweep goes site by site, from each
site's least recently used entry to its first one used within the 30
minutes, in steps of at most 64 entries of one site, each step a task of
its own: what a site's entries cost to sweep is spent in that site's
steps, and another site's request waits for one step at most, however
many entries the sites have expiring (in a harness with 100,000 entries of
other sites expiring at once, the longest task went from 28 ms to 65 µs).
When an entry was last used is read from a monotonic clock, the one each
site's list is ordered by, so a wall clock set back or forward neither
holds expiry back nor drops entries early. Finding a site's record is a
hash lookup in a table made once with room for 4096 sites (more than one
person visits within the 30 minutes an entry is kept), with a hash seeded
per table: adding a site never rehashes the table, so it costs the same
however many other sites there are (a table that grows took up to 20 µs
to add the site that made it rehash at 2,357 sites; this one stays under
half a microsecond); a site past the cap gets no entry, as a write that
doesn't fit. All sites together may use 120 MiB (twelve
shares); a write that would pass that is not stored, and nothing is
evicted for it. A new entry's key counts like data written into it: the
site makes room from its own unused entries first, and an entry that
still doesn't fit isn't made (its response isn't cached), so the entries
a site's requests hold open, long addresses included, never take it past
its share or the cache past its total (`MemBackendImpl::CreateEntry()`).
Memory pressure doesn't evict either. 120 MiB is 3% of
the 4 GiB machine MoltNet's storage limits are sized for (Chromium's own
rule gives 2%, capped at 50 MiB). What a page can still observe: its own
site's share; and, once other sites hold more than eleven shares' worth
between them (each entry kept at most 30 minutes unused), that a new
entry of its own isn't cached
(`net/disk_cache/memory/molt_mem_backend_budget_policy.h`, applied by
`MemBackendImpl` through `HttpCache::MoltSetPerSiteBudget()`).

**Tor's capacity.** All of MoltNet's sites share one Tor process, and
so do two identities alive at once (a new one while the old one closes):
MoltNet's budgets belong to that process, not to one identity's network
context. MoltBrowser's torrc states `ConnLimit 1000`: Tor refuses to start unless
the system lets it open that many file descriptors, and keeps 32 of them
for itself, so a Tor that runs accepts at least 968 connections, on every
system (checked with Tor 0.4.9: with its descriptor limit at exactly
1000, it answered 965 SOCKS connections and then logged "Failing because
we have 967 connections already"). MoltNet's budget of 768 leaves 200 of
those: at most 60 for synchronous requests past their sites' limits
(*Shared network state*), and at least 128 for Tor's own connections (its
listeners, the control connection, its guard or bridge connections, which
also carry its directory fetches, and its files), so no page can make Tor
turn away other sites' connections. Eleven sites at their full share,
each past it by all it may, still leave Tor its 128.

Circuit building is budgeted the same way. A SOCKS handshake in progress
is what makes Tor build a circuit for a stream that can't use an existing
one, and Tor builds at most `MaxClientCircuitsPending` circuits at a time
for all clients together; with that many pending, a new stream waits for
one of them. MoltNet counts each handshake in progress by the circuits it
may need: one, or three for an onion address (its descriptor,
introduction and rendezvous circuits). A site may have 16 in progress
(the keys no site can be named for 8 together), all sites together 192,
which fits eleven sites' full shares, as the connection budget does; the
torrc sets `MaxClientCircuitsPending 256`, which leaves 56, after those
8, for Tor's own circuits (preemptive ones, at most 14 kept ready, and
the measurement circuits of its build-time estimate; MoltBrowser checks
at build time that at least 32 stay free). While the budget is full, the
next slot freed goes to the waiting site with the fewest in progress, so
a site starting its first circuit waits only until its handshake's
weight is free (one handshake finishing, up to three for an onion
address), never behind a greedy site
(`net/socket/molt_per_site_socket_pool_policy.h`: `MoltWeightedSlots`).
Tor Browser leaves `ConnLimit` and `MaxClientCircuitsPending` at Tor's
defaults (1000 and 32) and lets Firefox's connection limits apply: 900
connections for all sites together, 6 per server.

MoltNet keeps Tor's own estimate of how long a circuit may take to build
(`LearnCircuitBuildTimeout`, on by default, as in Tor Browser): Tor
measures its circuits and abandons one much slower than most. A fixed
`CircuitBuildTimeout` would make every client the same, but one short
enough to help fails on slow networks and bridges, and one long enough
for them keeps streams waiting on stuck circuits; Tor Browser sets none.
The estimate is Tor's, not a page's: a page can't read it, and every
site's circuits feed it alike.

What a page can still observe of Tor's capacity: its bandwidth and its
circuit building are shared, in Tor itself. A site downloading at full
speed slows other sites' traffic on the same guard connection (Tor sends
a quiet circuit's cells before a busy one's, which keeps the delay small,
not zero), and a circuit takes longer to build while Tor builds many
others (its CPU, the guard's queue), so a page timing its first
connection can tell that other sites are opening circuits; Tor Browser
shares this the same way. Tor's memory for
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

**After an identity ends.** When an identity ends (New Identity, a
setting the live check refuses, the memory valve), its network context
refuses every request from that moment (§2 *Fail-closed*). A refusing
proxy configuration alone only makes a request fail once the network
service routes it, and messages on different pipes have no order: a
request a page sent on its URL loader factory just before the end could
reach the network service just after it, and a request in flight keeps
its connection. So MoltNet's network context (the `[MinVersion=1]` field
`molt_stop_all_when_refusing`) ends in the task that applies the refusal
(`ProxyConfigServiceMojo` runs it after its observers), in this order
(`services/network/molt_ended_identity_policy.h`,
`NetworkContext::MoltEndIdentity()`):

1. It marks itself ended. From then on it starts nothing a page or the
   browser asks of it, whichever pipe the request comes on: a URL loader
   (`URLLoaderFactory`, a page's factories and the browser's alike)
   completes at once with `ERR_ABORTED`, and a WebSocket, a WebTransport
   session, a preconnect, a prefetch, a host resolution or a new host
   resolver is refused. Only the browser's question of how it would route
   a request is answered, with the refusal (*Fail-closed*, step 5).
2. Its connections to Tor refuse every write: a SOCKS5 connection's data
   and handshake, and a SOCKS connect job's start, fail with
   `ERR_ABORTED` (`net/socket/molt_identity_end.h`, one signal for the
   context's network session, read by `SOCKS5ClientSocket` and
   `SOCKSConnectJob`). Whatever a request in flight still had to send
   stays unsent.
3. Its URL loader factories are closed, its WebSockets and WebTransport
   sessions dropped, every request in flight cancelled, and all its
   connections closed.

A network context made for the identity after its end (the network
service restarted) starts ended. A model that hands the end and a page's
requests to the network context on separate pipes, in 3000 random
interleavings, checks that nothing starts and nothing is written after
the end, and that the order of the steps matters
(`molt_net_end_ordering_unittest.cc`). Some requests are held by the
browser for a page and may outlive it: keepalive requests, beacons and
`fetchLater` (forwarded and followed through redirects by the browser's
keepalive loaders), Background Fetch, a download, Save Page As. The
browser asks `ContentBrowserClient::HasNetworkIdentityEnded()` (MoltNet:
`molt_net_ended_identity_throttle.cc`) before it starts or retries a
keepalive request or follows its redirect, starts a Background Fetch
request, a download or a saved resource.

**Storage.** What MoltNet's sites store (IndexedDB, Cache Storage, the
origin-private file system, service worker scripts) is held in memory.
Each site's storage keys together (its own and those of what it embeds;
a credentialless iframe's key, which Chromium gives its own origin's site
as top-level site wherever it is embedded, counts with the other keys of
its page's nonce instead: `storage/browser/quota/molt_storage_budget.h`)
may always use 60 MiB, whatever the other sites store, and that is the
only limit a write meets: no write fails because of a total other sites
could fill, and none reads the machine's memory, so whether it succeeds
says nothing about other sites or the machine. Nothing is evicted to make
room (Chromium evicts the least recently used data of any site every half
hour once all sites use 70% of its pool). Memory is bounded by the
**memory valve** instead: once all sites together store more than 600
MiB (or hold more than 800 MiB of blobs), the floors every machine
MoltNet runs on holds, the quota system and the blob storage tell it as
they grow, and it reads the machine's available memory on a worker
thread, apart from any write, at most every 200 ms. If less than 1 GiB is
available it ends the whole identity, the same way New Identity does, and
tells the user why (`chrome/browser/molt_ai/tor/molt_net_memory_valve*`).
That is what an out-of-memory crash would otherwise do, uncontrolled and
to the regular windows too. What a group of sites can learn this way:
roughly how much memory was free, once, at the cost of the identity it
tracks the user in; no other site's write fails meanwhile. The details,
what a page can still observe and what it costs:
[FINGERPRINTING.md](FINGERPRINTING.md#moltnet-storage-limits).

Storage under a page's nonce. A credentialless iframe's storage keys carry
its page's nonce, which no later page has, so what such frames store
(`localStorage`, `sessionStorage`, IndexedDB, Cache Storage, the
origin-private file system) can never be read again once the page goes.
Chromium 156 kept it all the same: until the profile ended off the record,
and on disk until a sweep, once per session, of keys unused for a day. A
page that reloaded itself, each time with new credentialless frames that
stored something, made MoltNet hold a new group of storage, with its
records, for every reload. In MoltNet, a tab notes the nonces its pages'
frames use, and when a page goes, its nonces' storage goes with it: their
DOM storage areas and records in the storage service, in each nonce's own
turn, their buckets in the quota system, with the usage counted for them,
and their storage keys' media-license data
(`chrome/browser/molt_ai/tor/molt_net_nonce_storage.{h,cc}`, a tab helper
for MoltNet's tabs; `DOMStorageContext::MoltDropNonce()`, the storage
controls' `MoltDropNonce()`, `QuotaManagerImpl::MoltDropNonce()` and
`CdmStorageDataModel::DeleteDataForStorageKey()`). So do their cookies.
Such frames keep cookies in partitions of their own, under the nonce, one
for each site their documents and navigations reach; Chromium 156 kept
those in memory until the profile ended, with no total for partitioned
cookies, so a page that reloaded itself added partitions each time. The
tab helper notes each storage key's partition, and the partition of each
URL a navigation under the nonce requests (as it starts and at each
redirect: the response may set cookies there, whether the navigation
commits or not); when the page goes, those partitions' cookies are deleted
(`CookieManager::DeleteCookies()`, partitioned cookies only), and again a
minute later, past the 30 seconds a keepalive request may outlive its page.
In the harness, 1,000 reloads of a page left 3,000 such partitions (5,000
cookies) before and none now, the user's other cookies untouched. In
MoltNet's partition, the clipboard's salt for a storage key with a nonce
(`StoragePartitionImpl::GetPartitionUUIDPerStorageKey()`, a table entry per
key for the partition's life at 156) is derived from the key under a
secret made once per run, so the table keeps no entry per page. The rest
of what 156 keys by a nonce needs nothing: it is never kept for one (HTTP
cache entries, shared dictionaries), goes with its document (blob URLs,
Web Locks), or sits in caches of fixed size that no page can reach again
once the nonce's page is gone (idle connections, TLS sessions, server
properties, CORS preflight results); the inventory is in
`molt_net_nonce_storage_policy.h`. A
top-level site holds such storage for at most 32 of its pages at once
(`molt_net_nonce_storage_policy.h`; a page its server sandboxed, whose
origin is opaque, counts to the site it came from, so no site's pages count
against another's): past that, the oldest page's is dropped
and its further writes are refused (its quota is what it holds, its DOM
storage takes no writes) until that page goes; its cookies and
media-license data are deleted then too, and what its frames set
afterwards goes when the page goes. What a page can still
observe: its own frames' storage, while it lives. What it costs: a page
with credentialless frames on a site with 32 newer such pages open loses
what its frames stored there, as if it had been reloaded.

What a quota check costs. A write asks the quota system first, on the
quota manager's one sequence, which every site's checks share; the answer
needs the usage of the site's storage keys. Chromium keeps each bucket's
usage in a cache but no total per site, so the answer took a pass over
every bucket of every site (each client asked for the ones not cached),
and a site that made many storage keys or buckets slowed every other
site's checks. MoltNet's usage cache keeps a total per site (grouped as
the budget groups storage keys: a credentialless iframe's with its page's
nonce) and one for all sites, moved by each change to a cached bucket's
usage; a
change it can't take in (a bucket not cached yet, a change of unknown
size) marks that bucket's storage key for its site, and a check asks
again only for its own site's marked keys before reading its total
(`storage/browser/quota/molt_storage_budget.h`, kept by
`ClientUsageTracker` and `UsageTracker` and read by `QuotaManagerImpl`'s
quota answer, for MoltNet's budget only). One pass over everything is
made when the usage cache starts counting, when MoltNet's partition is
empty. So a check costs what the site's own storage keys cost; the
answers are the same. A site's marked keys are asked for one refresh at
a time: a check that comes while its site's refresh runs waits for it and
for one more, of what was marked meanwhile, so two checks at once both
read the refreshed total (one of them could read the total while the
other's refresh was still asking, and answer with room the site didn't
have); and a refresh of the site's own follows 10 seconds after a mark,
so marks don't stay for sites that stop checking
(`molt_storage_usage_counts.h`: `MoltSiteRefreshes`).

**Blob memory.** Blobs (`new Blob()`, `Response.blob()`,
`canvas.toBlob()`, recordings, files a page assembles) are held in memory
in MoltNet, with no disk to page them to. Chromium keeps all of a
profile's blobs within one limit (800 MiB in MoltNet), so a page that
filled it made other sites' blob creation fail. MoltNet counts blob memory
per renderer process, the one whose blob registry created the blob: each
may use 200 MiB whatever the others hold, and that is the only limit a
blob meets; past 800 MiB for all together the memory valve watches the
machine (*Storage* above) (`storage/browser/blob/molt_blob_budget_policy.h`,
applied by `BlobMemoryController`; the process is the one
`BlobRegistryImpl` was bound for). A blob the browser makes for a site, a
Background Fetch's response held by the download service, counts to a key
of that site's own (taken from the site by a keyed hash with a secret of
the browser session, never a renderer process's, so no table of sites is
kept), not to the browser's key that its other blobs share
(`MoltBlobSiteKeyFromDigest()`, set by `BlobTaskProxy`); in MoltNet no other blob the
browser makes serves a page. A Background Fetch's response counts from its
first byte: the download service holds it in memory until it is complete,
and asks for room under the site's key before it reads more of it
(`BlobTaskProxy::MoltHoldMemory()`, used by `InMemoryDownloadImpl`, the
same rule as a blob's: `MoltBlobHoldFor()` in `molt_blob_budget_policy.h`).
Past the site's share the fetch fails, as for a network error (the
registration's `failureReason` is `fetch-error`), and what the buffers
hold counts toward the valve's floor like any blob. A renderer process serves one tab (*Renderer processes*), so
a share is one site's in one tab. A blob that doesn't fit fails for the
page that made it, as in Chromium when its limit is reached; nothing
another page holds is removed. What a page can still observe: its own
process's use, and nothing of the machine's memory short of ending the
identity (*Storage* above). What it costs: nothing for a page whose blobs
stay within 200 MiB.

**DOM storage caches.** `localStorage` and `sessionStorage` are limited
to 10 MiB per storage area on every machine, unchanged. The storage
service keeps each area it has opened in a cache and, at 156, purges the
unused ones (no page has them open) of every site when a new area is
opened and all areas together pass one limit per partition (50 areas or
20 MiB on desktop; every open on a low-end device), so one site opening
areas (its subdomains, or frames it embeds, each have one) could push
other sites' unused areas out and time its own reloads. MoltNet's
partition keeps the cache per top-level site (a frame's areas count to
the site embedding it): opening an area may purge only unused areas of
its own site, once that site has more than 12 areas or 5 MiB cached (a
quarter of Chromium's limits for a whole partition), and an unused
`localStorage` area leaves the cache after 30 minutes unused, checked
every minute whatever sites do; `sessionStorage` areas also go with their
tab. The low-end-device rule and `PurgeMemory()` don't apply there.
MoltNet's partition keeps no database (below): an area that holds an
item, of any size, is never purged, since its items are nowhere else; only
areas that hold nothing leave the cache, and a page that opens one again
finds it empty, as it was. So there an area that holds nothing goes as
soon as no page has it open, in a task in its site's turn (the purge on
opening and the sweep above are for a partition kept per site with a
database): the cache holds exactly the areas in use and those holding
items, nothing that opens or sweeps looks at other sites' areas (both
purges walked every site's areas), and whether an area was still cached
says nothing about other sites (`MoltDomStorageDropsUnusedArea()`)
(`components/services/storage/dom_storage/molt_dom_storage_site_policy.h`;
the storage controls' `MoltKeepCachesPerSite()`, which
`DOMStorageContextWrapper` sends when
`ContentBrowserClient::ShouldKeepDomStorageCachesPerSite()` says so, for
MoltNet). The renderer's own cache of areas is per process, and a MoltNet
process serves one site in one tab (*Renderer processes*).

No database. Chromium keeps an in-memory partition's areas in an
in-memory database too, one for each kind, on a sequence of its own:
every area commits its changes there, and reads its items from there
when it opens (a page's first read of an area waits for it). That read
waited for every commit queued before it, other sites' included; the
per-site commit rate bounded each site's commits, but not those of many
sites together. The database only held a second copy of what the areas
hold, read back for areas purged from the cache. MoltNet's partition
runs without one, the storage service's own mode for a database that
can't be opened (an area starts empty and holds its items itself), and
keeps every area that holds an item, as above: opening an area waits on
nothing another site queued, nothing is ever committed, and what a site
stores is held once instead of twice, within its total
(`MoltDomStorageKeepsNoDatabase()`, checked by `LocalStorageImpl` and
`SessionStorageImpl` before they open a database). What it costs:
nothing a page can see; what the partition holds is still bounded by each
site's total and the memory valve.

**DOM storage totals.** Chromium limits `localStorage` and
`sessionStorage` to 10 MiB per area: one per origin, and for
`sessionStorage` one per tab. A site with many subdomains, frames it
embeds or tabs could store as much as it liked in all, held in memory in
MoltNet, and each area committed to the partition's one database at a
rate of its own. In MoltNet's partition each site's areas together (an
area counts to its storage key's top-level site, so a frame's to the site
embedding it; a credentialless iframe's to its page's nonce, and one under
an opaque top-level site to itself, as neither key names the site it is
embedded in: `molt_dom_storage_site.h`; the same groups decide the cache
purges above, the write allowance below and Background Fetch's
scheduling) hold at most one area's limit, 10 MiB and 100 KiB, for
`localStorage` and for `sessionStorage` apart
(`components/services/storage/dom_storage/molt_dom_storage_site_policy.h`
and `molt_dom_storage_site_budget.h`, applied by `StorageAreaImpl`
through its delegate, `LocalStorageImpl` and `SessionStorageImpl`). An
area counts not only its items' keys and values, as Chromium's limit per
area does, but also what holding them takes in the storage service: 112
bytes for each item (its node in the area's map, 80 bytes, and the
rounding of its key's and its value's memory to the allocator's 16-byte
slots) and 2 KiB for each area that holds an item (its objects in the
storage service, each with a copy of the storage key), measured at
Chromium 156 on 64-bit Linux (`MoltDomStorageCountedBytes()`). So a site
storing many tiny items, or a few in many areas, holds no more memory
than its total says: without it, 10 MiB of one-byte items took about
1 GiB. With
a database a site's areas would also commit at one area's rate together
(60 commits and 10 MiB an hour); MoltNet's partition has none (*DOM
storage caches* above). A tab's `sessionStorage`, copied into a tab it
opened, counts again once Chromium copies it for a change (it then holds
as much again), even past the site's total, and the site's areas then
grow no more until they shrink. A write that would take the site past its
total is refused by the storage service, as one past an area's own limit
is, and changes nothing there; for `localStorage` the page's copy goes
back to what the area holds, as Chromium's renderer does for any write
the storage service refuses. Past five sites' worth for all sites together, of either kind,
the memory valve hears of each further MiB (*Storage* above:
`MoltDomStorageGrowthObserver`, which `DOMStorageContextWrapper` sets on
both storage controls from
`ContentBrowserClient::GetDomStorageGrowthBeyondFloorCallback()`, for
MoltNet). What a page can still observe: its own site's total. What it
costs: a site of one origin in one tab stores what Chromium lets it,
unless it fills its 10 MiB with more than about 900 items (a site's areas
of one kind hold at most about 94,000 items); a site that spreads its
storage over subdomains or tabs gets one area's worth in all, and a
`sessionStorage` write refused for the total is kept only in that page's
own copy (Chromium's renderer doesn't hear of a
refused `sessionStorage` write), so it is gone after a reload.

Writes per site. Each write a page sends is work on the storage service's
sequence, which every site's storage operations share: the change, and a
notification carrying the value to every renderer with the area open.
Chromium limits how often an area commits, not how many writes it takes,
so a page rewriting a large value in a loop kept the sequence busy for
every other site (their reads, including a page's first read of an area,
which waits). In MoltNet's partition a site's writes, for each kind, come
out of a write allowance of its own: up to 1000 writes and one area's
limit in bytes (10 MiB and 100 KiB) at once, refilled at 100 writes and
1 MiB a second (`MoltDomStorageMayWrite()`, asked by
`StorageAreaImpl::Put()` before it changes anything). A write past it
fails as one past the area's quota does, never as a bad message; for
`localStorage` the page's value reverts. The numbers are well above what
pages do with `localStorage`, which is synchronous and small: a few to a
few hundred keys written as a page loads, state saved on each change
while a user types or plays (a game saving 8 KiB every frame, 60 a
second, never meets it), and rarely more than a few hundred KiB in all;
pages that keep more use IndexedDB. What a page can still observe: its
own site's allowance. What it costs: a page that writes faster than that
for long loses the writes past it (the last write that fits stays).
Deleting and clearing take no allowance (the storage interface gives them
no failure to report, and a delete only removes what a write added). A
refused write, a removal or a `clear()` still costs the storage service
its handling; what keeps a page sending them in a loop from delaying other
sites is that the storage service's sequence is taken in turn per site
(below).

In every profile, a write the storage service refuses leaves the area as
it was: `StorageAreaImpl::Put()` puts back the value it had taken out on
every refusal (at Chromium 156, a refusal for an area's own limit
didn't).

**The storage service's sequence, in turn per site.** Every message a
page sends its `localStorage` and `sessionStorage` areas is handled on the
storage service's one sequence, and Chromium's mojo bindings post one task
there for each message waiting on an area's pipe, all at once. A page
sending messages in a tight loop (writes past its allowance, which are
refused but still handled, `clear()`, removals) queued thousands of
tasks, and another site's read (a page's first read of an area is
synchronous) waited for all of them: one such page kept the sequence about
half busy, two saturated it. In MoltNet's partition each area's pipes (the
pages', and its observers') are bound to a task runner of the area's site,
whose tasks wait in that site's queue, and the sequence runs one task at a
time from the queues in turn: the site whose next task would start
earliest in a virtual time goes first, and each task moves its site's
virtual time on by the time it took, a microsecond at least: the thread's
processor time where the platform has it, so that a task during which the
operating system ran other threads isn't charged that time (start-time
fair queueing:
`components/services/storage/dom_storage/molt_dom_storage_fair_queue.h`
and `molt_dom_storage_fair_scheduler.h`, used by `StorageAreaImpl`,
`SessionStorageAreaImpl`, `LocalStorageImpl` and `SessionStorageImpl`).
Everything still runs on the partition's thread (below), each site's
messages in the order they came; only the order between sites changes. A
site that had nothing queued, and has used no more than its share of the
thread, goes next, after the task in service; sites with work queued share
the thread's time equally, however many messages or areas each has. In a
harness that reproduces mojo's dispatch around the shipped scheduler, a
polite site's read every 10 ms waited behind 50,000, 100,000 and 200,000
messages of one, two and four flooding sites (73, 159 and 262 ms) in
Chromium's order, and behind one or two (64 µs, as without a flood) in
MoltNet's.

A thread of its own. At Chromium 156 the storage service (a utility
process on the desktop, the browser's IO thread on Android or with
`--single-process`) makes every partition's `localStorage` and
`sessionStorage` on its one sequence: a regular window's page flooding
its storage delayed MoltNet's reads, one task for one, and MoltNet's pages
delayed the regular window's, a timing channel between identities. The
browser now binds a partition kept per site in memory (MoltNet's) through
`StorageService.MoltBindPerSiteLocalStorageControl()` and
`MoltBindPerSiteSessionStorageControl()` (`DOMStorageContextWrapper`), and
the storage service gives each of its two controls a dedicated thread from
the thread pool, which runs nothing else, so it never waits for a pool
worker either (`components/services/storage/dom_storage/
molt_dom_storage_own_thread.{h,cc}`). Every identity has its own; regular
profiles keep the storage service's sequence. In the harness, with
MoltNet's scheduler on the storage sequence as before, MoltNet's polite
read waited behind about 33,000 messages of one flooding regular page
(38 ms at the median) and a regular page's behind one MoltNet task; on
its own thread, behind none of the other side's (17 µs at the median, as
without a flood; what stays is the machine's: the processor, and the
storage service's IO thread, which reads every pipe's messages).

The control pipe. The browser's messages to a partition (binding an area
for a page, deleting a site's storage, creating, cloning and deleting a
tab's `sessionStorage` namespace, usage, flushes) come on one pipe, and
were handled as they came, one task each, ahead of the sites' turns: a page
making the browser bind areas in a loop (frames of many subdomains) held
up every other site. In MoltNet's partition that pipe takes a turn of its
own, and each message that names a storage key (`BindStorageArea()`,
`DeleteStorage()`) is handled in its site's turn; its turn counts its
tasks at an eighth of their time, as it carries every site's messages and
only reads them (`MoltDomStorageChargeFor()`); and each `sessionStorage`
namespace's pipes (a tab's renderers' `Clone()` calls) take a turn of
their own too (`MoltDomStorageNamespaceGroup()`). A message that names a
storage key is handled after the control messages that came before it,
and in order with its site's other messages. In the harness, with the
browser forwarding binds as fast as it could, another site's read waited
behind about 24,000 of them (39 ms at the median) before, behind none
(21 µs) now.

A control message that reads sites' areas keeps their order too. What a
partition stores (`GetUsage()`, which Clear-Site-Data and the settings read
before they delete) is listed site by site, each site's areas in its own
turn, after the messages it queued before: a write queued in a site's turn
when the listing came was missed at 156, and so was not deleted by the
clear that followed; now it is listed. A deletion that names one storage
key (Clear-Site-Data's, which a site can send on any response) reads that
key's areas only, in its site's turn, and, over all time, that key's
buckets only; at 156 each one listed every site's areas, in one Mojo
reply, and every bucket (`MoltGetUsageForStorageKey()` on both storage
controls; `content/browser/browsing_data/molt_one_key_deletion_policy.h`).
In the harness, a Clear-Site-Data in a partition of 100,000 areas took
4,250 tasks and 108 ms of listing before, 2 tasks and 32 µs now. The
callers that want every site's data (the site settings and site data
dialogs, clearing browsing data, counting it) are the browser's own and
keep Chromium's listing. A tab's `sessionStorage` copied at
once (a duplicated tab), or deleted while tabs it opened wait for their
copy, waits for a turn of each site with areas in it (or a bind or deletion
for it queued), so the copy holds what those sites wrote before, for one
second at most: a site whose turn hasn't come by then (one with a backlog
of its own making) is copied as its areas are, and its writes still queued
then land in the source only, as Chromium allows for a copy the browser
makes (`kImmediate`: "strict ordering against mutations is irrelevant");
a deletion of the source's areas asked for before the copy applies to the
copy at once (`molt_dom_storage_waits.h`). Round 30's copy waited for the
whole backlog: in the harness, a site with 100,000 writes queued held a
duplicated tab's copy, and the binds of its pages of every site, for
100 s; now for 1 s. The
control messages that name the namespaces involved (a bind of the copy,
the source's deletion) wait for it, in the order they came, and messages
for other namespaces don't. A renderer's own copy (`window.open()`) was
already ordered after its writes by the renderer. A copy the deletion
still owes to a tab whose namespace was populated meanwhile (a `Clone()`
of it on another namespace's pipe) is dropped when the deletion runs, as
Chromium drops it while handling a message.

Long tasks. One task of a site is how long another site's message may
wait. Where it could be long, the work is split
(`molt_dom_storage_split_work.h`): a change carrying 64 KiB or more to an
area open in more than one tab notifies each tab's renderer in a task of
its own, in order with the area's other notifications (a 10 MiB write to
an area open in eight tabs serialized 180 MiB in one task, 13.5 ms in the
harness; now 2.2 ms for the write and 1.5 ms for each notification), and a
big map (a `clear()`, an area or a tab's namespace going) is freed at most
1024 items or 1 MiB a task (a clear of 90,000 items took 10 ms in one
task; now 19 µs and steps of 0.2 ms). Walks over the whole partition are
split too: the listing above reaches 16 sites a task in the control's turn
and lists 64 of a site's areas a task in that site's turn (in the
harness, 100,000 areas were walked in one task of about 50 ms; now in
tasks of 0.1 to 2 ms, a read sent between two steps waiting for at most
16 of them; for the browser's own callers that want every area, the
reply, one message listing every area that holds an item, is still built
in one task, as at 156), and the memory dump reports the
partition's totals, kept as they change, without walking or naming
areas. So is the work on a tab's
`sessionStorage` namespace, which holds an area per storage key its pages
used, possibly thousands, and which a site can make the browser copy (a
popup it opens) and delete (the popup closed) at will: a copy of more than
64 keys registers and clones 64 a task, the copy holding binds until it is
complete, as a copy the renderer is still to make does; a deletion frees
areas up to 64, or 1024 items or 1 MiB, a task, then forgets its keys 64 a
task; each job after the jobs before it that name one of its namespaces
(those run one at a time, in the order they came), in its namespace's own
turn, the messages naming its namespaces waiting for it; gathering a
namespace's sites for a copy that waits for their turns takes 64 areas a
task. Round 30 ran one job at a time for the whole partition, in the
control's turn, so a tab's job waited for every other tab's before it: in
the harness, a tab's two-step copy queued behind another tab's
thousand-step copy finished after 100 ms, now after 0.4 ms
(`molt_dom_storage_waits.h`). In the harness, with
Chromium 156's copy and deletion and the new ones on stand-ins with 156's
shapes, a namespace of 5,000 areas took 6 to 9 ms of processor time to
copy and 1 to 1.5 ms to delete, in one task each; now no task took more
than 0.25 ms
(`molt_dom_storage_split_work.h`, `SessionStorageImpl::MoltCloneStep()`,
`MoltDeleteStep()`). A namespace that fits one task is copied or deleted
at once, as at 156.

What a page can still observe: the task in service when its message
arrives (one message of another site, or one control message: at most the
decoding and handling of a write of an area's whole limit with one
notification, or a read of a whole area, a few milliseconds), the
machine, and, if it uses more than its share, that it shares the thread.
A page's own first opening of an area waits for the browser's control
messages before it, which the browser forwards one at a time. What it
costs: one more task per message, two threads per MoltNet identity, and a
task per tab for a big change.

**Shared work queues.** Sites also share the browser's work: a site that
keeps a part of the browser busy slows what another site asks of it, and
the other site may time that. What MoltNet can bound per site, it does:
each site has connection pools and shares of the connection and
circuit-building budgets of its own, the next free slot going to the
waiting site that uses the fewest (*Shared network state*, *Tor's
capacity*); its own keepalive count, HTTP cache share, quota, blob share
and renderer processes (one tab each); quota checks that cost what its
own storage keys cost (*Storage*); Background Fetch limits of its own
(*Downloads and Background Fetch*); and, for `localStorage` and
`sessionStorage`, its own total and write allowance, with no database to
wait behind (*DOM storage caches*, *DOM storage totals*), and its turn on
the identity's own storage thread, which no other identity or regular
window shares (*The storage service's sequence, in turn per site*), so
the work its messages cause the storage service is bounded, and no read
waits for another site's queued messages or commits. What every site still
shares, in MoltNet as in
any browser, Tor Browser included: the processor and the GPU process; the
browser's main and IO threads, which pass every page's messages; the
network service's thread, which runs every request's network code (a
site's many requests delay another's by the time that code takes, not by
the network's); the identity's storage thread, one task at a time (a
message of another site in service when a page's own arrives), and the
storage service's IO thread; and Tor's own process (*Tor's
capacity*). Threads of their own for each site would take
a network and a storage service per site, which no browser has; what a
site can measure through them is how busy the machine is, as any page can
with its own timers.

**Records per site.** Each limit above keeps a record for each site (its
share, count, turn, allowance or pools), made when the site first needs
one, and adding one doesn't cost more because other sites have records:
the HTTP cache's table of sites is made with room for its cap (*HTTP
cache*); every other record (DOM storage's turns, totals and write
allowances, Background Fetch's, the keepalive counts, the socket pools and
their budgets, blob and quota counts) is kept in a balanced tree, which
never rehashes: adding a site takes one comparison per level of the tree.
In a harness, adding a site to such a tree took under half a microsecond
at 4,096 sites, as at none; a hash table left to grow took up to 20 µs to
add the site that made it rehash.

A record also goes once it limits nothing, so records don't pile up for
sites that come once (a page under a fresh nonce, a third party framed
once): a DOM storage total at zero, a write allowance once it is full
again and a commit rate once it no longer delays (a few due ones are
dropped at each storage operation), a keepalive count with nothing
registered or in flight, a quota site's stale marks once refreshed, and a
nonce's records when its page goes (*Storage*). A record is dropped only
in the state a new one starts in, so dropping it changes no limit: a site
can't reset its allowance or rate by going quiet, as a quiet site's has
refilled anyway. Blob shares need no table of sites at all (*Blob
memory*). In a harness, 10,000 sites that each wrote once and cleared left
10,000 commit rates and write allowances for the life of the identity
before; two hours later none (`molt_dom_storage_site_policy.h`:
`MoltDomStorageExpiringRecords`; `molt_keepalive_partitions.h`:
`ForgetIfIdle()`).

**Renderer processes.** Chromium lets a site's frames and workers share a
renderer process across tabs: an iframe takes the process its site
already has in another tab, service and shared workers the same, and any
suitable process is reused once the process limit is reached. A third
party framed by many sites then runs in one process for all of them, and
whatever is counted per process (the blob memory above, the renderer's
own memory and caches) is shared by those sites. In MoltNet a renderer
process for web content serves one BrowsingInstance (a tab and the
windows it opened with an opener, which can reach each other's frames
anyway): `RenderProcessHostImpl::IsSuitableHost()` refuses a process that
serves, or recently served, another one
(`content/browser/renderer_host/molt_browsing_instance_processes{,_policy}.h`,
through `ContentBrowserClient::ShouldKeepBrowsingInstancesInOwnProcesses()`
and `molt_net_process_policy.h`). Browser pages and the built-in PDF
viewer keep Chromium's sharing. Tor Browser keeps its processes apart by
first-party site in the same way. What it costs: more renderer
processes, one per site per tab, and a service worker in a process of
its own.

**Downloads and Background Fetch.** Some requests a page causes are made
by the browser, not the page, and Chromium gives some of them no network
key of their own, so they were filed under their destination, as if the
destination were a top-level site: its connections, cache partition and
cookies, and in MoltNet its Tor circuit (the SOCKS username is the
top-level site). Each now carries the key of where it came from:

- A Background Fetch (`registration.backgroundFetch.fetch()`): the key of
  the service worker that started it, or of the page, the same as their
  own requests. Content gives the download service that key with each
  request; off the record, the in-memory download service now puts it on
  the request instead of only its cookie site
  (`components/download/internal/background_service/molt_registering_site_isolation.h`,
  hooked into `in_memory_download.cc`). This applies to every
  off-the-record profile: the in-memory service exists only for them,
  Background Fetch is its only client there, and regular profiles already
  send the same key. The request also carries the fetch's credentials
  mode and its origin as initiator, as the site's own `fetch()` would: the
  in-memory download service sent every request with credentials and no
  initiator, so a fetch that asked for none sent cookies
  (`MoltCarryFetchCredentialsAndInitiator()`). Regular profiles' download
  service, which makes Background Fetch requests through
  `download::CreateResourceRequest()`, did the same; that now gives every
  profile's Background Fetch requests the fetch's credentials mode
  (`components/download/internal/common/molt_fetch_credentials.h`), so a
  fetch with `credentials: 'omit'`, or same-origin credentials to another
  origin, sends no cookies. The fetch's icon is loaded by the page
  itself. With Local Network Access for Background Fetch (on by default at
  156), a fetch's requests go through a URL loader factory made for its
  service worker, which carries the worker's client security state, so a
  public site's fetch can't reach the user's local network without the
  site's permission. A request made without that factory went out on the
  partition's default one, which carries none and checks nothing: one the
  download resumed itself after its response stopped early (which a
  server can cause at will), and one taken up again after the browser
  restarted. Now such a request of a page gets a factory that checks Local
  Network Access for its origin, as a worker served from a public address
  would (`content/browser/download/molt_resumed_fetch_{policy,factory}.h`,
  used by `DownloadManagerImpl`; a worker served from the local network
  is held to that stricter policy); and after a restart a fetch whose
  registration isn't loaded yet, which the download service took up
  without any of the page's parameters, waits for them and then starts
  again with them, or fails after five minutes if its registration is gone
  (`components/download/internal/background_service/molt_restored_fetch.h`).
  A take-up that comes while the download service is still asking for the
  parameters (the answer on its way, when asking again did nothing) is
  remembered, and the parameters are asked for again as soon as that
  answer comes without them; before, the take-up was lost and the fetch
  failed after its five minutes. Both in every profile.
- A download a page starts without navigating (a link with `download`,
  "Save link as", "Save image as" or "Save video as" in a top-level page, a
  link dragged out): the key of the frame it came from, as that frame's
  own requests have; a resumed download: its first request's key; the
  resources "Save page as" fetches: the key of the frame they belong to.
  With no frame left and nothing to resume, a one-off key. Such a request
  keeps its first party on redirects (`content/browser/download/
  molt_download_isolation{,_policy}.h`, hooked into
  `DownloadManagerImpl::BeginDownloadInternal()` and
  `SaveFileManager::SaveURL()`; MoltNet only, through
  `ContentBrowserClient::ShouldKeepDownloadsWithTheirSite()`).
- A download retried from the download bubble or the downloads page: the
  key, and the initiator, of the download it retries. Chromium makes the
  retry a new download of its URL, from the bubble with no frame, from the
  downloads page with that page's frame, and with no key
  (`chrome/browser/download/molt_retried_download_isolation.h`, hooked
  into both; a retry never takes the downloads page for the frame that
  started it, and one of a download that had no key gets a one-off one).
- Not changed: a navigation that turns into a download keeps its
  navigation's key (a link opened in the page leads to its destination's
  site, as in any browser), and saving a page or a frame keeps the key of
  that page or frame.

What a page can still observe: nothing of another site's downloads. What
it costs: a file saved with "Save link as" from another site's link no
longer carries that site's first-party cookies, as in Tor Browser, so a
download that needs a sign-in on that site may fail from the link; open
the link instead.

Background Fetch scheduling. Chromium runs a partition's Background
Fetches through one scheduler: at most 2 registrations active at a time
(from different storage keys) and 2 of their requests downloading, for all
sites together, and a registration stays active until it finishes,
however slowly its server sends. A site that started two slow fetches
(from its origin and a subdomain) kept every other site's from starting,
for as long as it liked, and the other site could time it. In MoltNet the
scheduler keeps those limits for each top-level site (a frame's service
worker counts to the site embedding it): a site's registration becomes
active as soon as its own site has room, and its requests start whatever
other sites run: a site's registrations wait in the order they came, one
per storage key active, and a request that ends starts its own site's
next. There is no total for all sites together: a party embedded on many
sites (a third-party service worker under each) could fill one and keep
every other site's fetches waiting. What the downloads hold is bounded
instead by each site's blob share (*Blob memory*) and, past 128 downloads
running for all sites, by the memory valve, which hears of each further
one and reads the machine's memory as for storage
(`content/browser/background_fetch/molt_background_fetch_site_policy.h`,
applied by `BackgroundFetchScheduler`, MoltNet only, through
`ContentBrowserClient::ShouldScheduleBackgroundFetchesPerSite()` and
`GetBackgroundFetchGrowthBeyondFloorCallback()`). A request's end costs
what its own site holds: in the harness, 2 steps whatever the number of
other sites, where finding the next site had walked every waiting site
(43 to 11,939 steps for 10 to 4,000 of them). The in-memory download
service that makes the requests ran 2 downloads at a time, 4 active or
paused and 15 waiting for all sites; it now has no limit of its own, so
it queues nothing and the scheduler alone decides
(`components/download/internal/background_service/molt_in_memory_download_limits.h`;
in every off-the-record profile, where Background Fetch is its only
client and starts at most 2 at a time outside MoltNet). What a page can
still observe: its own site's limits, and nothing of other sites' fetches;
past the floor, as for storage, roughly how much memory the machine has,
at the cost of the identity. What it costs: nothing for a site, which gets
Chromium's limits for a whole browser.

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
| Certificate fetches counted against their site (§2 *Certificate fetches*): the site goes from the TLS connection through the verifier (also across the verifier service's interface, a `[MinVersion=1]` field) to the fetcher, which gives the fetch that site's key with a nonce of its own; it counts against that site's pools and handshake slots, merges with nothing, and a fetch naming no site keeps a one-off key; other profiles unchanged | Unit tests (`molt_ai_unittests`: the scoped site, the key, the request comparison and the mojo round trip); standalone harness: the fetcher's hooked code against stand-ins with the 156 shapes, 10 tests, 12 mutants all caught; the changed files type-checked against Chromium 156's headers (`tools/compile_check`) | Pass (built standalone; not yet built in the Chromium tree) |
| Certificate fetches on circuits of their own, no captive-portal check, no NTLM or Negotiate, in a running browser (§4 steps 8 and 20) | Needs a Chromium build | **Not yet done** |
| Per-site network state (§2 *Shared network state*): MoltNet's context has no certificate-verification cache or merging, no shared HTTP cache entries, HSTS for top-level navigations only, its HTTP auth cache keyed by site and no Private State Tokens, also after the policy or the anti-abuse setting changes; every other profile keeps 152's behaviour; a new identity's context gets all of it again | Unit tests (`molt_ai_unittests`, with the real parameter structs); standalone harness: the hooks taken verbatim from `network_context.cc`, `transport_security_state.{h,cc}`, `shared_resource_checker.h` and `profile_network_context_service.cc` against stand-ins with the 152 shapes (g++ ASan/UBSan, clang `-fno-exceptions`); the changed files type-checked against Chromium 152's headers (`tools/compile_check`) | Pass (built standalone; not yet built in the Chromium tree) |
| MoltNet doesn't open while `PartitionConnectionsByNetworkIsolationKey`, `SplitCacheByNetworkIsolationKey` or `ThirdPartyStoragePartitioning` is off, says which, and checks before Tor starts and before the profile is created; Chromium's default-on switches count as on | Unit tests (`molt_ai_unittests`, the feature names checked against Chromium's); the same harness with the glue from `molt_net_routing.cc` and `molt_net_window.cc`, verbatim; 15 tests in the harness, 30 mutants of the hooks and policies all caught | Pass (built standalone; not yet built in the Chromium tree) |
| The same in a running browser: a verification for every new TLS connection, the switches refused (§4 steps 8 and 9) | Needs a Chromium build; the leak kit's `identity.partitioning_override_refused` | **Not yet done** |
| The Chromium 156 network re-check (§2 *New network paths at Chromium 156*): MoltNet's context sends no TLS client certificate and remembers no choice, the same for every server and every site, and a request that reaches the browser is answered with no certificate and no picker; no DNS-over-HTTPS probes and no device-bound sessions in MoltNet's context; MoltNet refused while proxy override rules are set; MoltNet's renderers get the fixed network quality once and only "online" or "offline" as the connection; every other profile and context keeps Chromium's behaviour | Unit tests (`molt_ai_unittests`); standalone harness: the hooks taken verbatim from `ssl_client_socket.{h,cc}`, `network_context.cc`, `chrome_content_browser_client.cc`, `molt_net_routing.cc` and the two observers (the `profile_network_context_service.cc` hook for device-bound sessions through its policy's tests) in `content/browser/network/`, and the real `molt_uniform_network_state.h`, against stand-ins with the 156 shapes, including Chromium's client-certificate restart (g++ ASan/UBSan, clang `-fno-exceptions`): 22 policy and 17 hook tests, 17 mutants all caught; the changed files type-checked against Chromium 156's headers (`tools/compile_check`) | Pass (built standalone; not yet built in the Chromium tree) |
| The same in a running browser (§4 steps 9 and 20) | Needs a Chromium build; the leak kit's `net.client_cert.*` | **Not yet done** |
| MoltNet fails closed while it is open (§2 *Fail-closed*): the check before opening runs again whenever a setting it reads changes (a policy's proxy, override rules, a leak control, blob URL partitioning, in the MoltNet profile or the regular one); a refusal ends that identity once, as New Identity does, with the open check's message; a window about to open checks again; MoltNet's network contexts only ever get MoltNet's own proxy configuration (none before it: requests wait), whatever the settings say; every other profile and the system's network context keep Chromium's | Unit tests (`molt_ai_unittests`: the check and its messages, the parser for MoltNet's proxy setting, the configuration rule), built standalone with g++ ASan/UBSan and clang `-fno-exceptions`, 11 mutants of the policy all caught; standalone harness: `molt_net_live_guard.cc` compiled unchanged against stand-ins of `Profile`, `PrefService`, `PrefChangeRegistrar` and `ProfileObserver` with Chromium's destruction order (g++ ASan/UBSan, clang `-fno-exceptions`): which profiles are watched, which settings run the check, one end per identity, the regular profile's changes, both profiles' destruction, two identities of one profile; 10 tests, 10 mutants all caught; the changed files (`molt_net_routing.cc`, `molt_net_window.cc`, `molt_net_live_guard.cc`, `proxy_config_monitor.cc`) type-checked against Chromium 156's headers (`tools/compile_check`) | Pass (built standalone; not yet built in the Chromium tree) |
| The same in a running browser: a proxy policy set while a MoltNet window is open closes it, nothing leaves outside Tor meanwhile, MoltNet then refuses to open, and opens again once the policy is gone (§4 step 9) | Needs a Chromium build; the leak kit's `identity.policy_change_closes` | **Not yet done** |
| Ending an identity (§1.3, §2 *Fail-closed*): from that moment its network context refuses every request (`localhost` included), whatever configuration comes later; its windows and the DevTools windows on its pages close, also a page asking "Leave site?" with DevTools open (docked or in a window of its own), and a window that stays open is closed by force; New Identity changes nothing if one still stays | Unit tests (`molt_ai_unittests`: the configuration rule); standalone simulation: the code of `molt_net_window.cc` verbatim against stand-ins (windows whose unload processing shows "Leave site?" and lets DevTools intercept it, DevTools agents, renderer processes, Tor) on a simulated clock, g++ ASan/UBSan, 7 tests, 10 mutants caught; standalone harness: `RefuseAllProxyConfig()` verbatim with Chromium 156's proxy-rule and bypass-rule code verbatim, 3 tests, 3 mutants caught; standalone harness: the `proxy_config_monitor.cc` hooks and `MoltNetRouting`'s `EndIdentity()`, `HasIdentityEnded()`, `AddIdentityEndedCallback()` and `ProxyConfigFor()` verbatim, next to Chromium 156's monitor, against stand-ins (g++ ASan/UBSan, clang `-fno-exceptions`), 6 tests, 10 mutants caught; the changed files (`molt_net_window.cc`, `molt_net_routing.cc`, `proxy_config_monitor.cc`) type-checked against Chromium 156's headers (`tools/compile_check`) | Pass (built standalone; not yet built in the Chromium tree) |
| The end in step with the network service (§2 *Fail-closed*): the pages stop (since round 26 once every network context has the end, so that nothing they send reaches the network unrefused: *Nothing started or written after the end* below); DevTools and the windows wait until every network context confirms the refusal; the connections dropped after it leave none a request opened before it; a hung network service or a lost profile doesn't hold the end; a restarted network service's context refuses from the start; navigations of an ended identity are refused | Unit tests (`molt_ai_unittests`: the confirmation rule, 5 tests, built standalone); standalone simulation: `RefusalCheck`, `WhenIdentityRefuses()`, `FlushNetworkState()`, `StopPages()`, `CloseEndedWindows()` and `EndIdentityAndCloseWindows()` verbatim against a network service whose pipes are read in random interleavings with random delays (g++ ASan/UBSan, clang `-fno-exceptions`), 400 interleavings per test, 8 tests, 15 mutants all caught; the same simulation shows round 23's order letting requests made after the end out; the changed files type-checked against Chromium 156's headers (`tools/compile_check`) | Pass (built standalone; not yet built in the Chromium tree) |
| MoltNet's languages (§1.10, §2 *Settings isolation*): MoltNet's list sits above the profile's own settings, so neither the `ForcedLanguages` policy, nor the chosen languages it reads from the regular profile, nor `--accept-lang` change it, also after Chromium's language code rebuilds a profile's list; `ReduceAcceptLanguageEnabled` is ignored for MoltNet only | Unit tests (`molt_ai_unittests`): the list and the policy rule (3 tests, built standalone), and MoltNet's settings store with Chromium's real `LanguagePrefs` and a managed `ForcedLanguages`; standalone harness: `FixMoltNetLanguages()` verbatim against a stand-in that layers the settings stores as Chromium 156 does and rewrites the list as `LanguagePrefs` does, 4 tests, 4 mutants caught; the two `chrome_content_browser_client.cc` hooks verbatim next to Chromium 156's code, 2 tests, 2 mutants caught (g++ ASan/UBSan, clang `-fno-exceptions`); the changed files (`molt_net_pref_isolation.cc`, `molt_net_routing.cc`, `chrome_content_browser_client.cc`) type-checked against Chromium 156's headers (`tools/compile_check`) | Partly: the standalone tests pass; the settings-store tests need a Chromium build |
| Incognito's availability (§2): MoltNet refuses to open, with its message, while Incognito is off or replaced by Isolated Mode, opens while it is forced; the live check watches the setting, in both profiles; Chromium's window check and Incognito's URL lists treat MoltNet as Incognito | Unit tests (`molt_ai_unittests`, the live policy's 20 tests, built standalone with g++ ASan/UBSan and clang `-fno-exceptions`); standalone harness: `molt_net_live_guard.cc` compiled unchanged against stand-ins, 11 tests, 11 mutants caught, among them the availability setting; standalone harness: the `CanOpenBrowser()` and URL-list hooks verbatim next to Chromium 156's code, for regular, Incognito, Isolated Mode, DevTools, look-alike and MoltNet profiles, 5 tests, 4 mutants caught; `incognito_mode_prefs.cc`, `chrome_policy_blocklist_service_factory.cc`, `molt_net_routing.cc` and `molt_net_window.cc` type-checked against Chromium 156's headers (`tools/compile_check`) | Pass (built standalone; not yet built in the Chromium tree) |
| The other policies for Incognito (§2 *Incognito's availability*): MoltNet doesn't open while extensions are required in Incognito, and the live check watches the policy; data-control rules' `incognito` conditions match MoltNet and copied text is labelled Incognito's (no URL); Windows parental controls are read again, uncached and on a COM thread, before each MoltNet window starts Tor | Unit tests (`molt_ai_unittests`: the live policy, built standalone); standalone harnesses: the data-control and clipboard functions as Chromium 156 has them and as hooked, the settings read and the watch list verbatim, 4 tests, 9 mutants all caught; the uncached read and the open step verbatim, 4 tests, 8 mutants, 7 caught (the last only differs off Windows, where the unit test catches it); the changed files type-checked against Chromium 156's headers (`tools/compile_check`), except `parental_controls.cc` (Windows headers) | Partly: the standalone tests pass; `parental_controls.cc` needs a Windows build |
| Blob memory per renderer process and renderer processes per tab (§2 *Blob memory*, *Renderer processes*): a tracker framed by two tabs gets two processes and two shares (156: one process, one limit, the greedy frame leaving the other nothing); a process is shared within a tab, and again once its tab's ids are cleaned up; browser pages and the PDF viewer, and other profiles, share as Chromium; greedy processes, one or a group of three, leave a polite one its whole share, without asking the machine; beyond the floor the machine decides for every process alike, and a burst can't pass the reserve; freed and shrunk blobs keep their process; the transport strategy follows the process's room | Unit tests (`molt_ai_unittests`: the budget and process policies, built standalone); standalone harness: `BlobMemoryController`'s quota code as Chromium 156 has it and as hooked, `ChildProcessSecurityPolicyImpl::MoltHostsOtherBrowsingInstance()`, the `IsSuitableHost()` hook and the embedder's rule verbatim, the policies compiled, against stand-ins with Chromium 156's subframe process reuse (g++ ASan/UBSan, clang `-fno-exceptions`): 9 tests, 23 mutants all caught; the changed `storage/` and blob registry files type-checked against Chromium 156's headers (`tools/compile_check`; `render_process_host_impl.cc` and `child_process_security_policy_impl.cc` can't be checked there, their helper header was) | Partly: the standalone tests pass; the process model needs a Chromium build |
| The same in a running browser: a tracker iframe on two tabs in two processes, blob creation in one tab unaffected by another tab filling its share (§4 step 21) | Needs a Chromium build | **Not yet done** |
| Per-site connection limits (§2 *Shared network state*): each site gets pools of its own in MoltNet's context, of fixed sizes whatever `MaxConnectionsPerProxy` says, with Chromium's per-host limit; flushes, idle-socket clean-up and `chrome://net-internals` reach every pool, which is named after the proxy, never the site; unused pools go; other contexts and connections without a proxy keep the one shared pool | Unit tests (`molt_ai_unittests`): the size rule (4 tests, built standalone) and Chromium's real `HttpNetworkSession` and pools (4 tests); standalone harness: `client_socket_pool_manager_impl.cc` and the new headers verbatim against stand-ins of the pools (g++ ASan/UBSan, clang `-fno-exceptions`), 5 tests, 11 mutants caught; the changed `net/` and `services/network/` files type-checked against Chromium 156's headers (`tools/compile_check`) | Partly: the standalone tests pass; the real-pool tests need a Chromium build |
| Connection budget and per-site shares (§2 *Shared network state*, *Tor's capacity*): a site's first-party and third-party keys, and its sandboxed pages, share its pools; the one-off keys share one small pool; each pool's limit is fixed (no random room); all pools of both types stay within 768; with the budget full, the next freed connection goes to the waiting site using the fewest and an idle connection of a larger site is closed for it (idle HTTP/2 sessions too, busy ones never); a cancelled or self-limited waiter holds nobody back; one greedy site never makes a polite one wait, and ten full sites still leave a site its whole share | Unit tests (`molt_ai_unittests`): the policy (13 tests, built standalone with g++ ASan/UBSan) and the real `HttpNetworkSession` and pools (7 tests); standalone harness: `molt_connection_budget.h`, `molt_per_site_socket_pools.h` and the policy compiled verbatim, `ClientSocketPool`'s hooked functions and `TransportClientSocketPool`'s new methods included verbatim into a stand-in pool with 156's slot logic (requests, connect jobs, idle sockets, releases, stalled groups, HTTP/2 sessions as higher-layered pools), g++ ASan/UBSan and clang `-fno-exceptions`: 12 tests, 16 mutants, 15 caught (the last, waking the largest site first, changes nothing: the admission rule still lets the smallest in first). Round 24: the budgets shared by two identities of one Tor (per identity and site), certificate fetches counted against their site in a pool of the site's own that its waiting connections can't fill, synchronous `XMLHttpRequest` held to its site's limits but not its host's (six stalled connections to a host don't hold it), going past a full site's pool by 4 at most outside the budget (a greedy site's take nothing from a polite one), woken like any waiting request, no backup connect jobs, dropped connect jobs given back: 26 tests, 46 mutants, 45 caught (the same equivalent one; the hooks' call sites in `transport_client_socket_pool.cc`, which the harness transcribes, are checked word for word against the worktree); the changed `net/` files type-checked against Chromium 156's headers (`tools/compile_check`) | Partly: the standalone tests pass; the real-pool tests need a Chromium build |
| SOCKS handshake slots (§2 *Tor's capacity*): a site's 17th handshake waits, without a connection to Tor and with no timer, and reports "resolving host" so the pool starts no backup job; another site's doesn't wait; a finished, failed or dropped handshake gives its slot back; a dropped waiting job leaves the queue; a job's raised priority moves it up; only MoltNet's sessions and a loopback SOCKS5 proxy are limited | Standalone harness: `socks_connect_job.{h,cc}` and `molt_connection_budget.h` compiled unchanged against stand-ins of `ConnectJob` (156's base behaviour), the connection to the proxy and the SOCKS5 socket (g++ ASan/UBSan, clang `-fno-exceptions`): 5 tests, 8 mutants caught (the harness found that a waiting job first reported itself as idle, which would have started backup jobs; fixed); unit tests (`molt_ai_unittests`, `MoltSocksHandshakeLimiter` on a real task environment, 2 tests) | Partly: the standalone tests pass; the unit tests need a Chromium build |
| Tor's capacity (§2 *Tor's capacity*): the torrc states `ConnLimit 1000` and `MaxClientCircuitsPending 256`; MoltNet's budget, the 64 connections synchronous requests may take past their sites' limits and 128 for Tor itself fit what Tor accepts; the handshake budget (192, an onion address counting three) plus Tor's own circuits fits the pending-circuit limit, and a freed slot goes to the waiting site with the fewest; one greedy site, or a group, leaves a polite site its first circuit at once while the budget has room, and next in line when it is full | Unit tests (`TorrcBuilderTest`, the pool policy's 17 tests, among them the weighted slots', built standalone with g++ ASan/UBSan and clang `-fno-exceptions`); the SOCKS harness, 7 tests, 14 mutants all caught; `tor --verify-config` with Tor 0.4.9.11 on the torrc `BuildTorrc()` writes (also with `MaxClientCircuitsPending 256`); Tor 0.4.9.11 under a descriptor limit of exactly 1000 (only a local bridge, so nothing left the machine): 965 SOCKS connections answered, then "Failing because we have 967 connections already"; the leak kit reads both options from the torrc MoltBrowser started Tor with (`identity.tor_capacity`) | Pass |
| Keepalive limits per site (§2 *Keepalive requests*): in MoltNet one site's 2048 in flight leave another site's, and a regular window's, alone, and a regular profile's leave MoltNet's alone; the per-page limits (256, 512 KiB) still hold; a sandboxed page counts against its site; a context's counts go with it and its factories and loaders, destroyed after it, find them gone | Standalone harness: `keepalive_statistics_recorder.{h,cc}`, `molt_keepalive_partitions.h` and `molt_keepalive_site.h` compiled verbatim, the hooked parts of `URLLoaderFactory` (constructor, destructor, `CreateLoaderAndStart()`'s limit checks) and `NetworkContext` (constructor, destructor) included verbatim into stand-ins with 156's ownership (g++ ASan/UBSan, clang `-fno-exceptions`): 5 tests, 8 mutants caught; unit tests (`molt_ai_unittests`: the registry, 3 tests, built standalone; the network context policy); the changed `services/network/` files type-checked against Chromium 156's headers (`tools/compile_check`) | Partly: the standalone tests pass; the network service's own suites need a Chromium build |
| Storage per site (§2 *Storage*, [FINGERPRINTING.md](FINGERPRINTING.md#moltnet-storage-limits)) | See FINGERPRINTING.md | Partly |
| Certificate fetches never wait behind the connections that need them (§2 *Certificate fetches*, *Shared network state*): fetches go to certificate-fetch pools of their own (4 per site, 4 for the keys of no site) in a part of the budget of their own (48 of the 768); a credentialless iframe's keys use their site's pool; with eleven or more sites each holding its whole share, every connection waiting for a certificate check, the fetches still go out at once and the checks finish without waiting for a timeout, and a polite site still gets its share; Tor's capacity checks (`static_assert`s) hold for both parts | Unit tests (`molt_ai_unittests`): the policy and the real `HttpNetworkSession` and pools; standalone harness (the budget harness of round 24, the pool code verbatim against stand-ins with 156's slot logic): 32 tests, 54 mutants, 53 caught (the same equivalent one as before); the certificate fetcher's flag (10 tests, 14 mutants caught); the changed files type-checked against Chromium 156's headers (`tools/compile_check`) | Partly: the standalone tests pass; the real-pool tests need a Chromium build |
| HTTP cache per site (§2 *HTTP cache*): a site's entries are evicted only by its own writes (its own least recently used, down to 90% of its share) or after 30 minutes unused (on open and by the sweep); a write past the total is not stored and evicts nothing; memory pressure and Chromium's size changes don't evict under the budget; the site is read from the cache key (first-party, cross-site, credentialless and opaque forms); other contexts keep Chromium's cache | Unit tests (`molt_ai_unittests`): the policy (built standalone) and Chromium's real `MemBackendImpl`; standalone harness: the hooked `MemBackendImpl` code verbatim against a model of its entries, 12 tests (one greedy site and a group against a polite one), 20 mutants caught; type-checked against Chromium 156's headers | Partly: the real-backend tests need a Chromium build |
| Nothing sent after an identity ends (§2 *After an identity ends*): the refusal cancels every request the network context has in flight and closes its connections; keepalive requests, beacons and `fetchLater`, Background Fetch, downloads and saved resources are not started, retried or redirected by the browser once the identity has ended; other profiles unchanged | Unit tests (`molt_ai_unittests`, with Chromium's real `ProxyConfigServiceMojo`); standalone harnesses: the refusal and teardown order (7 tests, 11 mutants caught), the browser's checks against stand-ins with 156's loader shapes (10 tests; without the checks, the held requests are shown still going out); type-checked against Chromium 156's headers | Partly: the real-service tests need a Chromium build |
| Nothing started or written after the end, whichever pipe (§2 *After an identity ends*, *Fail-closed*): a network context ends in the task that applies the refusal; from then on its URL loader factories (a page's and the browser's), WebSockets, WebTransport, preconnects, prefetches and host resolutions start nothing and its SOCKS5 connections write nothing; then its factories close, its sockets and sessions drop, its requests in flight are cancelled and its connections closed; a context made after the end starts ended; the browser stops the identity's pages only once every context has the end (or after 5 seconds) | Unit tests (`molt_ai_unittests`): a model of the browser and the network context that hands the end and a page's requests over separate pipes in 3000 random interleavings, with the real step orders of `molt_ended_identity_policy.h` and `molt_net_identity_end_policy.h` (nothing starts or is written after the end; round 25's order, refusing writes late and a hung context are shown), 15 tests built standalone (g++ ASan/UBSan, clang `-fno-exceptions`), 15 mutants, 14 caught (the last removes a `static_assert` the tests repeat); Chromium's real `NetworkService` and `NetworkContext` (a loader, a WebSocket, a preconnect and a host resolution after the end, a context made after it) and `SOCKS5ClientSocket` (writes and a handshake after the end); the changed files type-checked against Chromium 156's headers (`tools/compile_check`); the leak kit's `identity.nothing_after_end_race` | Partly: the standalone tests pass; the real-service tests and the kit need a Chromium build |
| A Background Fetch's response counted as it downloads (§2 *Blob memory*): the download service asks for room under the site's key before it reads more, past the share the fetch fails and its room is given back, and the valve sees the buffers; other profiles and fetches without a site key unchanged | Unit tests (`molt_ai_unittests`: the rule, and `BlobStorageContext` with shares; `components_unittests`: `InMemoryDownloadTest.MoltNet*`, the real `InMemoryDownloadImpl`); standalone harness: `InMemoryDownloadImpl`'s and `BlobTaskProxy`'s hooked code verbatim against stand-ins with 156's shapes, the main and IO sequences run in random order (g++ ASan/UBSan, clang `-fno-exceptions`), 7 tests, 18 mutants, 16 caught (the other two only matter for a download retried or resumed in memory, which 156 doesn't do); the changed files type-checked against Chromium 156's headers (`tools/compile_check`) | Partly: the standalone tests pass; the download suites need a Chromium build |
| `localStorage` and `sessionStorage` totals per site (§2 *DOM storage totals*): a site's areas, over its subdomains, frames and tabs, hold one area's limit together, for each kind; a refused write fails as one past an area's own limit (never as a bad message) and keeps the old value; deletes and clears give room back, deleted areas and maps theirs; a `sessionStorage` copy counts when made; one commit rate per site; the valve hears of each MiB past five sites' worth; other partitions keep Chromium's limits | Unit tests (`molt_ai_unittests`: the policy, 8 tests built standalone; Chromium's real `LocalStorageImpl` and `SessionStorageImpl` in memory); standalone harness: `StorageAreaImpl`'s `Put()`, `Delete()`, `DeleteAll()` and fork code verbatim with the budget functions of `LocalStorageImpl`, `SessionStorageImpl` and `SessionStorageDataMap` (random writes over many subdomains, keys-only and full maps; g++ ASan/UBSan), 9 tests, 24 mutants, 23 caught (the last asks the budget for writes that don't grow, which it allows); the changed files type-checked against Chromium 156's headers (`tools/compile_check`) | Partly: the standalone tests pass; the real-service tests need a Chromium build |
| No write depends on other sites or the machine; the memory valve (§2 *Storage*, *Blob memory*): a site's quota and a process's blob room are its own share only, whatever others hold and whatever the machine has; past the floors the embedder hears of each write, below them never; the valve reads memory on a worker at most every 200 ms, one reading at a time, ends the identity once, on the UI thread, below 1 GiB, never for an unknown reading; a Background Fetch response counts to its site's key | Unit tests (`molt_ai_unittests`): the three policies (built standalone); standalone harnesses: the quota gatherer and evictor (7 tests, 12 mutants caught), `BlobMemoryController`'s quota code as 156 has it and as hooked (11 tests, 24 mutants caught), the valve itself (6 tests, also under ThreadSanitizer, with growth reported from six threads at once), the Background Fetch blob key (with the download harness, 40 mutants caught); type-checked against Chromium 156's headers | Partly: the standalone tests pass; the full-browser behaviour needs a Chromium build |
| `localStorage` and `sessionStorage` caches per site (§2 *DOM storage caches*): another site opening any number of areas, in any tab, never purges a site's unused areas, and a site's purges happen at the same steps whatever others do; a site past its own limits purges only its own unused areas; unused `localStorage` areas leave after 30 minutes unused, areas in use never; `PurgeMemory()` and the low-end rule don't apply; the wrapper asks for it on both controls, also after the storage service restarts, for MoltNet only | Unit tests (`molt_ai_unittests`): the policy (built standalone) and Chromium's real `LocalStorageImpl` in memory; standalone harness: the cache code of `LocalStorageImpl`, `SessionStorageImpl` and `SessionStorageNamespaceImpl`, Chromium 156's and as hooked, verbatim, the wrapper's and the embedder's hooks, 11 tests, 30 mutants caught; type-checked against Chromium 156's headers | Partly: the real-service tests need a Chromium build |
| Background Fetch scheduled per site (§2 *Downloads and Background Fetch*): a site's two slow fetches never keep another site's from starting; each site gets 2 active registrations (one per storage key, in the order they came) and 2 downloads, whatever other sites run, with no total for all sites (past 128 running, the memory valve hears of each further one); a request's end costs what its own site holds; a finished or aborted registration frees its site's slots; the in-memory download service sets no limit of its own; other profiles keep Chromium's scheduler | Unit tests (`molt_ai_unittests`: the policy, 8 tests, built standalone here, and the profile switch); standalone harness: the scheduler's 156 code paths (`OnRegistrationCreated()`, `DidCompleteRequest()`, `FinishJob()`, `OnRegistrationLoadedAtStartup()`) as the base had them and as hooked, against the real policy: at the base the 65th site waited and a request's end took 43 to 11,939 steps for 10 to 4,000 other sites; now every site starts and an end takes 2; the hooks type-checked against Chromium 156's headers | Partly: the scheduler in a running browser needs a Chromium build |
| A page's nonce storage goes with the page (§2 *Storage*): its credentialless frames' DOM storage, records and quota buckets are dropped when it goes; a site holds at most 32 pages' such storage, the oldest page's dropped and closed (writes refused) until it goes, a sandboxed page counting to the site it came from; other nonces and other profiles unchanged | Unit tests (`molt_ai_unittests`: the bound and the site a page counts to, 5 tests, built standalone; `MoltNetDomStorageTest.ANoncesStorageGoesWithItsPage` on the real `LocalStorageImpl`); standalone harness: `LocalStorageImpl`'s drop and closing verbatim against stand-ins with 156's shapes, 1,000 reloads of a page under fresh nonces left 1,000 areas and their records at the base, none now; the changed files type-checked against Chromium 156's headers | Partly: the tab helper and the quota manager's drop need a Chromium build |
| Records per site go once they limit nothing (§2 *Records per site*): DOM storage totals, write allowances and commit rates, keepalive recorders, quota stale marks; a dropped record is in a new one's state, so no limit changes | Unit tests (`molt_ai_unittests`: the expiring records, the keepalive registry, the stale marks, built standalone); standalone harnesses on the real headers, base and round 30: 10,000 sites' DOM storage records kept for the identity's life before, none two hours later now; 10,001 keepalive recorders before, 1 (the open page's) now; type-checked against Chromium 156's headers | Pass (built standalone); the network service's suites need a Chromium build |
| DOM storage walks, order and namespaces in short tasks (§2 *The storage service's sequence*): `GetUsage()` lists the same areas site by site in their turns, after a site's queued writes; the memory dump names no area; a tab's copy at once waits for its sites' turns and holds their writes, with the messages naming it held in order; a namespace copied or deleted a chunk a task, the copy the same (every key, each area sharing its map, a write meanwhile forking it), binds of the copy waiting, the source's deletion after the copy, a copy owed to a namespace populated meanwhile dropped; other profiles unchanged | Standalone harnesses: `LocalStorageImpl`'s walk verbatim with the real fair queue (100,000 areas: one task of about 50 ms before, tasks of 0.1 to 2 ms now, the reply aside; a write queued in its site's turn listed now, missed before); `SessionStorageImpl`'s and `SessionStorageNamespaceImpl`'s copy and deletion, 156's and round 30's verbatim, with 156's metadata (7 tests: a namespace of 5,000 areas copied in one task of 6 to 9 ms before, tasks of at most 0.25 ms now; writes during a copy; a nonce dropped during a copy; a deletion after a copy; and, with the namespace's waiting copies verbatim too, 4 tests of the copies a deletion owes: one owed to a populated namespace dropped outside a message and reported in one, 2 mutants caught); `MoltNetDomStorageTest.ACopyOfManyAreasIsCompleteWhenBound` and `ACopyOwedToAPopulatedNamespaceIsDropped` on the real host; type-checked against Chromium 156's headers | Partly: the real-service tests need a Chromium build |
| Quota checks of one site at once (§2 *Storage*): a check waits for its site's refresh under way and one more, so two checks at once both see the refreshed total; marks refreshed 10 s after, without a check | Unit tests (`molt_ai_unittests`: the refreshes, built standalone; `ConcurrentChecksDontOvershoot` on the real `QuotaManagerImpl`); standalone harness: `UsageTracker`'s refresh, the base's and round 30's verbatim: the base let the second of two checks through on the stale total, now both are refused; 1,000 sites' marks kept before, none after their settle time now | Partly: the real quota manager test needs a Chromium build |
| Local Network Access for Background Fetch requests made without the worker's factory (§2 *Downloads and Background Fetch*), every profile: a request resumed after its response stopped early, or taken up again after a restart, is checked for its origin as a worker served from a public address; a fetch taken up before its registration loaded waits for its parameters, then starts with them; other downloads unchanged | Unit tests (`molt_ai_unittests`: the choice and the wait, built standalone where std-only); standalone harness: 156's flow around the real decisions, at 156, with the restart of round 30's review only, and now: every request of a page's fetch goes out with Local Network Access checked, or waits; type-checked against Chromium 156's headers | Partly: the download suites need a Chromium build |
| `localStorage` and `sessionStorage` without a database, and a write allowance per site (§2 *DOM storage caches*, *DOM storage totals*): MoltNet's partitions open no database; areas and maps holding items (also of zero bytes) are never purged and keep them, those holding nothing still are; usage reports what areas hold; a site's writes past its allowance fail (never as a bad message) while another site's all go, and refill with time; other profiles keep their database and take every write. In every profile a write refused for an area's quota keeps the old value | Unit tests (`molt_ai_unittests`: the policy, built standalone here; Chromium's real `LocalStorageImpl`, `SessionStorageImpl` and `StorageAreaImpl`); standalone harness: the storage service's sequence with a greedy site or 20 sites flooding writes against a polite site's first read, and real pages' write patterns against the allowance; type-checked against Chromium 156's headers; the leak kit's `dom_storage_rate` and `regular_dom_storage_rate` | Partly: the real-service tests need a Chromium build |
| Quota checks per site (§2 *Storage*): a site's storage keys share its share, another site keeps its own; a cached change is counted with no question to a client; a polite site's check asks for none of a greedy site's new buckets, the greedy site's own check for its own; the floor still heard; other profiles keep Chromium's code path | Unit tests (`molt_ai_unittests`: the counting templates, built standalone here, with 100000 keys of a greedy site; Chromium's real `QuotaManagerImpl` with a mock quota client); type-checked against Chromium 156's headers | Partly: the real quota-manager tests need a Chromium build |
| Storage groups of keys that don't name their site, and HTTP cache entries' keys (§2 *Storage*, *DOM storage totals*, *HTTP cache*): a credentialless iframe's key counts with its page's nonce, never with its own site's pages or its frames under another page, for the quota share and the `localStorage` and `sessionStorage` totals, cache groups and commit rates; an area under an opaque top-level site counts on its own; a new HTTP cache entry's key counts against its site's share and the total, the site making room from its own unused entries, so entries held open never take it past either | Unit tests (`molt_ai_unittests`): the policies (built standalone), real storage keys (`molt_net_storage_groups_unittest.cc`), Chromium's real `LocalStorageImpl` and `MemBackendImpl`; standalone harnesses: the grouping headers verbatim against a storage key with 156's rules (a tracker's credentialless frames on two pages, the quota a frame reads), 5 tests, 6 mutants caught; the HTTP cache harness of round 25 with `CreateEntry()` verbatim, 17 tests, 25 mutants caught; round 26's DOM storage totals harness re-run on the changed code; type-checked against Chromium 156's headers | Partly: the real-service tests need a Chromium build |
| Background Fetch requests with the fetch's credentials mode and initiator, retried downloads with their download's key (§2 *Downloads and Background Fetch*): a fetch that asked for no credentials, or same-origin ones for another origin, sends no cookies, in every profile (regular profiles' requests through `download::CreateResourceRequest()`: a test sends one to a server with a regular profile's network context and its cookie, and with `credentials: 'omit'` the server sees none); a client without the site's key is unchanged; a retry from the bubble or the downloads page has the key and initiator of the download it retries, never the downloads page's, and a one-off key when there is none; offline items and other profiles unchanged | Unit tests (`molt_ai_unittests`: the request, 3 tests; the choice, 7 tests, built standalone); the standalone harness below, extended: the retry code of `DownloadBubbleUIController` and `DownloadsDOMHandler` as Chromium 156 has it and as hooked, `molt_retried_download_isolation.h` verbatim, 16 tests, 33 mutants, 32 caught (the last, the bubble's retry source, changes nothing: a bubble retry has no frame); the changed files type-checked against Chromium 156's headers (`tools/compile_check`) | Partly: the standalone tests pass; the download suites need a Chromium build |
| Downloads and Background Fetch keyed by their site (§2 *Downloads and Background Fetch*): a Background Fetch has its service worker's or page's key (also a third-party worker's, under each embedding site), not its server's, in every off-the-record profile; a download from a frame embedded on two sites has each site's key, "Save image as" the page's, a resumed download its first request's, "Save page as"'s resources their frame's, none of them a first party that follows redirects; with nothing to go by a one-off key; regular profiles unchanged | Standalone harness: `molt_registering_site_isolation.h` and `molt_download_isolation{,_policy}.h` compiled verbatim, the request set-up of `InMemoryDownloadImpl::SendRequest()` and `SaveFileManager::SaveURL()` as Chromium 156 has them and as hooked, `DownloadManagerImpl::BeginDownloadInternal()`'s hook, `download::CreateResourceRequest()`, `StorageKey::ToPartialNetIsolationInfo()` and the network service's choice of a request's isolation (`url_loader_util::GetIsolationInfo()`) included verbatim, against stand-ins of origins, sites and `IsolationInfo` (g++ ASan/UBSan, clang `-fno-exceptions`): the key before and after for each case, 10 tests, 17 mutants caught; unit tests (`molt_ai_unittests`: the choice, 5 tests, built standalone; the profile rule); the changed files type-checked against Chromium 156's headers (`tools/compile_check`); the leak kit's `identity.background_fetch_circuits` | Partly: the standalone tests pass; the download suites need a Chromium build |
| HTTP cache evictions per site cost only the site's own entries (§2 *HTTP cache*): each site's list holds its entries in the order of Chromium's list, children (sparse data) included; a site's eviction walks only its own list, skipping entries in use and the children of an entry it evicts, keeps the same entries Chromium's list with other sites skipped would, and costs the same whatever other sites hold; entries made before the limits are put in their sites' lists; the sweep stops at the first entry used within the 30 minutes | Unit tests (`molt_ai_unittests`: the lists with a stand-in entry, and Chromium's real `MemBackendImpl`: sparse data, entries made before the limits, the sweep past an entry in use); standalone harness: `molt_mem_backend_site_lru.h` verbatim with Chromium 156's `base::LinkedList`, around a model of `MemEntryImpl` and `MemBackendImpl`'s hooks (parents, children, references, doom), 200 random runs of 3000 operations against round 27's eviction (same entries kept, each site's list the global list by site; g++ ASan/UBSan), 6 mutants caught; one site's evicting write with 0 to 1,000,000 older entries of other sites: 3 to 4 µs (17 entries looked at) against round 27's 3 µs to 34 ms (17 to 1,000,017); type-checked against Chromium 156's headers (`tools/compile_check`) | Partly: the standalone tests pass; the real-backend tests need a Chromium build |
| `localStorage` and `sessionStorage` totals count each item and area held (§2 *DOM storage totals*): 112 bytes per item, 2 KiB per area holding one, a new key always counted; a site with a full area has room for some hundred tiny items in another, not tens of thousands | Unit tests (`molt_ai_unittests`: the policy, built standalone here; Chromium's real `LocalStorageImpl`); standalone harness: `StorageAreaImpl`'s `Put()`, `Delete()`, `DeleteAll()` and fork code verbatim with the budget functions (round 26's, now checking after every random operation that each site's count is what its areas' items and the areas themselves take; g++ ASan/UBSan), 11 tests, 10 mutants caught; the sizes read from Chromium 156's headers with its clang (`StorageAreaImpl` 352 bytes, `blink::StorageKey` 272, the map locator 320, the map's node 80); a measurement of the map's allocations (glibc: 128 to 160 bytes for an item of a 4-byte key and a value of up to 24 bytes); type-checked against Chromium 156's headers | Partly: the real-service tests need a Chromium build |
| The storage service's sequence in turn per site (§2 *The storage service's sequence, in turn per site*): a site with nothing queued goes after the task in service, also against twenty flooding sites; busy sites share the time equally whatever their tasks cost; a site that used more waits its turn; an idle site gets no credit; every task counts; each site's tasks keep their order; idle sites are forgotten; `localStorage` and `sessionStorage` areas' pipes bound to their site's runner in MoltNet's partition only | Unit tests (`molt_ai_unittests`: the queue, 8 tests built standalone here, 6 mutants caught; Chromium's real `LocalStorageImpl` and `SessionStorageImpl` with a flood of 1,500 writes and 1,500 `clear()`s against another site's read); standalone harness: `molt_dom_storage_fair_scheduler.h` and `molt_dom_storage_fair_queue.h` verbatim against stand-ins with Chromium 156's `SequencedTaskRunner` shape, on a storage sequence of its own with pipes read as mojo 156's `Connector` reads them, real areas and the real write allowance, flooding sites (refused writes and `clear()`s) against a polite site's reads: in Chromium's order the read waited behind 50,000 to 200,000 messages, in MoltNet's behind one or two (also under ThreadSanitizer); type-checked against Chromium 156's headers | Partly: the standalone tests pass; the real-service tests need a Chromium build |
| No sockets outside MoltNet's network context's own session (§2 *Circuits*): a context with the SOCKS isolation password, per-site pools or the identity's end makes no `ProxyResolvingClientSocketFactory` (its pipe closed) and binds a renderer's WebRTC socket manager to one that resolves nothing, opens nothing and stays connected; it makes no raw socket (TCP connected, server and bound sockets and restricted UDP sockets answer `ERR_ACCESS_DENIED` with no address and no data pipes; UDP sockets, the socket factory and the mDNS responder have their pipes closed); it looks up no name itself (`ResolveHost()` answers `ERR_ACCESS_DENIED` from any source, `CreateHostResolver()` closes its pipe); other contexts unchanged | Every creator of the factory at Chromium 156 found in its source (the network context's factory, for GCM; the WebRTC socket manager; tests and a GCM tool), every raw socket entry point of the network context's 156 interface, and the browser's callers of its name lookups; unit tests (`molt_ai_unittests`: the rule, and Chromium's real `NetworkContext` and network service, a MoltNet context against another binding local sockets); type-checked against Chromium 156's headers | Partly: the real-service tests need a Chromium build |
| MoltNet's DOM storage on a thread of its own (§2 *The storage service's sequence, in turn per site*): a partition kept per site in memory is bound through `MoltBindPerSiteLocalStorageControl()` / `MoltBindPerSiteSessionStorageControl()` and runs on a dedicated thread with its own scheduler; other partitions keep the storage sequence; a regular page's flood doesn't delay MoltNet's reads, nor the reverse | Unit tests (`molt_ai_unittests`: the per-site partitions on threads of their own, through their control pipes; a disabled timing test of the real classes for the build machine); standalone harness: the shipped scheduler verbatim against stand-ins with 156's shapes, the storage sequence and MoltNet's thread as real threads, pipes read as mojo 156's `Connector` reads them: a regular page flooding (one or two) against MoltNet's polite read every 10 ms, and two MoltNet pages flooding against a regular page's read: before, the read waited behind about 33,000 regular messages (38 ms at the median), or one MoltNet task; on MoltNet's own thread behind none of the other side's (17 µs); ThreadSanitizer clean; type-checked against Chromium 156's headers | Partly: the standalone tests pass; the real-service tests and the timing check need a Chromium build |
| The control pipe in its own turn (§2 *The storage service's sequence, in turn per site*): `BindStorageArea()` and `DeleteStorage()` handled in their site's turn, after the control messages before them; the namespaces' lifecycle in the control's turn, counted at an eighth of its time; each namespace's pipes in a turn of their own; the scheduler counts a task's processor time | Unit tests (`molt_ai_unittests`: binding 1,000 areas of one site through the control pipe against another site's read, with the real `LocalStorageImpl`; the weights and the queue, built standalone here); the harness above with a browser binding areas in a loop: at 200,000 binds a second both orders keep up; flat out, another site's read waited behind about 24,000 binds (39 ms) before and none (21 µs) now, while a page's own bind waits for the binds before it in the pipe (55 ms before, 119 ms now, with a flood no browser forwards) | Partly: the standalone tests pass; the real-service tests need a Chromium build |
| Long tasks split (§2 *The storage service's sequence, in turn per site*): a change of 64 KiB or more to an area open in more than one tab notifies each in a task of its own, in order with the area's other notifications; big maps freed 1,024 items or 1 MiB a task; smaller changes and one tab as Chromium | Unit tests (`molt_ai_unittests`: three observers hear every change in order, a gone one nothing; a clear of 900 items; the rules built standalone here); the harness above: one 10 MiB write over a 10 MiB value with eight tabs open went from one task of 13.5 ms (180 MiB copied) to 2.2 ms and 1.5 ms per tab, with one tab unchanged (3 ms); a clear of 90,000 items from one task of 10 ms to 19 µs and steps of 0.2 ms | Partly: the standalone tests pass; the real-service tests need a Chromium build |
| `localStorage` and `sessionStorage` areas holding nothing go when unused (§2 *DOM storage caches*): kept per site without a database, an area left unused that holds nothing goes in its site's turn, one in use or holding an item stays; no purge on opening and no sweep there; with a database the per-site purge and sweep as before | Unit tests (`molt_ai_unittests`: real `LocalStorageImpl` and `SessionStorageImpl`; the purge and expiry tests on a partition on disk, which keeps a database; the rule built standalone here); type-checked against Chromium 156's headers | Partly: the real-service tests need a Chromium build |
| The HTTP cache's sweep per site, its monotonic clock and its table of sites (§2 *HTTP cache*): the sweep goes site by site, at most 64 entries of one site a task; expiry reads a monotonic clock, also when the wall clock goes back or forward; adding a site never rehashes the table; a site past 4,096 gets no entry | Unit tests (`molt_ai_unittests`: Chromium's real `MemBackendImpl`; the site lists with a stand-in entry, built standalone here, 6 tests under ASan/UBSan); standalone harness: the lists with Chromium 156's real `base::LinkedList` and a model of `MemBackendImpl` and `MemEntryImpl` with both clocks: 200 random runs of 3,000 operations keep the same entries as round 28's sweep; with the wall clock an hour back, round 28's sweep left 100 expired entries, this one none; with 100,000 other sites' entries expiring, the longest sweep task went from 28 ms to 65 µs; 9 rehashes against none while adding 4,096 sites | Partly: the standalone tests pass; the real-backend tests need a Chromium build |
| What else a page's nonce keys goes with it (§2 *Storage*): its frames' cookies (each storage key's partition and that of each URL their navigations request; partitioned cookies only, and again a minute later) and media-license data are deleted when the page goes, and when its group is closed past its site's bound; in MoltNet's partition a nonce key's clipboard salt is derived, not kept; the inventory of what else 156 keys by a nonce (`molt_net_nonce_storage_policy.h`); other profiles unchanged | Unit tests (`molt_ai_unittests`: the bound, the site a page counts to and the second deletion's timing against the keepalive timeout, 6 tests, built standalone with g++ ASan/UBSan and clang `-fno-exceptions`); standalone harnesses: 156's cookie code verbatim (`CookiePartitionKey::FromNetworkIsolationKey()`, `FromStorageKeyComponents()`, `StorageKey::ToCookiePartitionKey()`, the network service's `DeletionFilterToInfo()`, `CookieDeletionInfo::Matches()`, `CookieMonster::DeleteMatchingCookies()`) against stand-ins, 4 tests: the tab helper's partitions are the ones the network stack stores in, 1,000 reloads left 3,000 partitions (5,000 cookies) before and none now, the user's other cookies kept (a filter without `partitioned_state_only` deletes every unpartitioned cookie), a late keepalive response's cookie taken by the second deletion; `GetPartitionUUIDPerStorageKey()` at 156 and hooked, 3 tests: 10,000 reloads kept 10,000 entries before and none now, each salt the same for its key and different from other keys' | Partly: the tab helper in a running browser needs a Chromium build |
| A deletion of one storage key reads that key's usage only (§2 *The storage service's sequence, in turn per site*): Clear-Site-Data, and any deletion naming one key, lists that key's `localStorage` and `sessionStorage` areas in its site's turn and, over all time, its buckets; the same filter decides what goes; the callers that want every site's data, and other profiles, unchanged | Unit tests (`molt_ai_unittests`: the policy, built standalone; `MoltNetDomStorageTest.AUsageOfOneKeyListsThatKeyOnly`, `WithoutCachesPerSiteAUsageOfOneKeyIsFiltered` and `MoltNetSessionStorageTest.AUsageOfOneKeyListsItsNamespaces` on the real implementations); standalone harness: `LocalStorageImpl`'s listing verbatim with the real fair queue, a Clear-Site-Data in a partition of 100,000 areas: 4,250 tasks and 108 ms of listing before, 2 tasks and 32 µs now, after the site's queued writes and listing nothing else | Partly: the real-service tests and `StoragePartitionImpl`'s hooks need a Chromium build |
| A Background Fetch taken up while its parameters are asked for (§2 *Downloads and Background Fetch*), every profile: the take-up is remembered and the parameters asked for again as soon as the answer without them comes, instead of the fetch failing after five minutes; other downloads unchanged | Unit tests (`molt_ai_unittests`: the remembered take-ups, 5 tests, built standalone); standalone harness: `ControllerImpl::StartDownload()`, `UpdateDriverState()`, `PrepareToStartDownload()` and `OnDownloadReadyToStart()` verbatim, before and now, against stand-ins with 156's shapes and Background Fetch's answers as at 156, 7 tests: before, a take-up during the question was lost and the fetch failed after five minutes; now it starts as soon as the answer comes | Partly: the download service's suites need a Chromium build |
| `sessionStorage` jobs and copies independent of other tabs and sites (§2 *The storage service's sequence, in turn per site*): a namespace job waits only for the jobs before it that name one of its namespaces and takes its steps in its namespace's turn; a copy made at once waits one second at most for its sites' turns, a deletion asked for before it applying to the copy; jobs of one namespace keep their order and the control messages naming them still wait; other profiles unchanged | Unit tests (`molt_ai_unittests`: the job order and the waits, 6 tests, built standalone with g++ ASan/UBSan and clang `-fno-exceptions`; `MoltNetDomStorageTest.CopiesOfTwoTabsRunSideBySide` on the real host); standalone harness: round 31's and this round's `SessionStorageImpl` job code and `MoltPerSiteSessionStorage` copy waits verbatim, with the real fair queue, in virtual time, 11 tests: a tab's two-step copy behind another tab's thousand-step copy finished after 100.2 ms before, 0.4 ms now; a duplicated tab's copy behind a site's 100,000 queued writes ran after 100 s before, 1.001 s now, without the area a deletion asked for before; a site with a few writes queued is copied with them, as before; 5 mutants caught | Partly: the real-host test needs a Chromium build |
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
| 7 | automated (exit IP only online) | `identity.new_identity`, `identity.session_circuits`, `identity.nothing_held_after_end`, `identity.nothing_after_end_race` |
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
   With a page that requests a URL of your test server every 100 ms and
   sends a beacon on `pagehide`, press *New Identity*: the server sees no
   request after the moment you pressed it (allow one already on its
   way), and none comes in after the new window opens. The kit makes the
   old page start one request of each kind (fetch, keepalive, beacon,
   image, WebSocket, prefetch) as soon as it sees one refused: none may
   reach its server (`identity.nothing_after_end_race`).
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
   cache). A Background Fetch started on two sites for the same file
   goes out with each site's username; so does a file saved with "Save
   link as" from a frame of one site embedded on two others.
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
   Set `MandatoryExtensionsForIncognitoNavigation` to any extension id
   while a MoltNet window is open: MoltNet ends and then refuses to open
   with "Incognito requires extensions
   (MandatoryExtensionsForIncognitoNavigation), and MoltNet windows run
   none"; remove it and MoltNet opens. With a `DataControlsRules` rule
   blocking pastes whose source is Incognito, copy text in a MoltNet
   window and paste it into a regular one: the paste is blocked. On
   Windows, turn on activity reporting for the account in the family
   settings while the browser runs: the next MoltNet window refuses to
   open, without a restart.
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
    "moltnet-shared", "moltnet-shared3" and "moltnet-shared4".) With Tor's
    log at `Log info`,
    in MoltNet windows:
    - A test page on site A that opens 1000 long-polling requests to 200
      hosts of its own, and a page on site B in another tab: B loads at
      once. `chrome://net-internals/#sockets` shows A's pool at 48 (its
      WebSockets' at 16). Tor's log shows no "Failing because we have"
      line.
    - Pages of twelve sites that each connect to 100 onion addresses at
      once, and then a page of a thirteenth site: Tor's log shows no
      "already have 256 general-purpose client circuits pending" line,
      and the thirteenth site's first connection starts at the next
      handshake that finishes (it may take a little longer than alone).
    - A page on site A that keeps 2048 `fetch(..., {keepalive: true})`
      requests in flight (to a server that answers slowly) from nine
      pages: the 2049th fails; site B's, and a regular window's, still
      go out.
    - Write to IndexedDB until `QuotaExceededError`: about 60 MiB per
      site, a subdomain's iframe on that site counting toward it; another
      site still gets its 60 MiB. Store a little on one site, then fill
      nine others: after more than 35 minutes (Chromium evicts every half
      hour) the first site's data is still there. Fill twelve sites to
      their 60 MiB: a thirteenth still gets its 60 MiB (on a machine with
      at least 2 GiB of memory free). On a machine with less than 1 GiB
      available once they pass 600 MiB, the MoltNet windows close with the
      message that the computer ran out of memory.
    - A tracker page framed in two tabs: `chrome://process-internals`
      shows two processes for it. In one tab it makes 10 MiB blobs until
      one fails (about 200 MiB); in the other tab it still gets 200 MiB,
      and so does a page of another site. Five tabs that each fill
      200 MiB: a sixth still gets its blobs.
    - A page on site A loads 8 MB of images from its own server, then a
      page on site B loads 200 MB of images from its own: A reloaded (in
      DevTools, *Disable cache* off) is still served from the cache.
    - A page on site A stores a value in `localStorage` from 20 subdomain
      iframes, then site B's page opens 100 subdomain iframes that each
      store one: in a `chrome://tracing` memory-infra dump the
      LocalStorage cache still holds A's areas (its `cache_size` doesn't
      drop).
    - A page that sends `fetch(url, {keepalive: true})` to a slow server
      every 100 ms, then *New Identity*: the server's log shows no
      request after the moment of the press.
    - A page on site A stores 3 MiB of `localStorage` from each of five
      subdomain iframes: the fourth one's write doesn't stick (it is gone
      after a reload); site B's page still stores 3 MiB from each of
      three iframes of its own. In a regular window all five of A's are
      kept.
    - A service worker of site A starts a Background Fetch of a 300 MB
      file: it fails (`failureReason` `fetch-error`) once about 200 MiB
      have arrived; one of 150 MB completes, and so does site B's of
      150 MB at the same time.

## 5. What MoltNet does *not* do

1. **It is not Tor Browser.** MoltNet windows letterbox the page and report
   the same time zone (UTC), language (en-US), screen size, pixel ratio, CPU
   count, memory, WebGL limits and installed fonts (MoltBrowser's bundled
   set; a build without it, such as a development build, uses a list per
   operating system, and MoltNet's panel says so) for everyone, and canvas, WebGL and audio readback carry per-site noise
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
     supervised user, and Windows parental controls, read again as each
     MoltNet window opens), Incognito's URL allow and block lists, the
     extensions Incognito requires (MoltNet, which runs none, doesn't open
     while any is required) and the "Incognito" conditions of data-control
     rules (§2 *Incognito's availability*). Chromium 156's Enterprise Isolated Mode, which a policy can put
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

**Background Fetch** requests are made with the fetch's credentials mode,
as the site's own `fetch()` would be: one with `credentials: 'omit'`, or
same-origin credentials to another origin, sends no cookies. Chromium's
download service made every Background Fetch request with credentials
(§2 *Downloads and Background Fetch*).

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
