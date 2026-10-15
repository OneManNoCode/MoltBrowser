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
   from bridges.torproject.org.
7. **Exit country (optional).** You can pin the exit to a country. This shrinks
   the set of exits you share with other Tor users, which **reduces anonymity**;
   the choice is not saved and resets when MoltBrowser restarts.

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
| HTTPS-Only mode on | warns before any plain-HTTP page, which the exit relay could read or modify |
| Connections partitioned by top-level site | no socket reuse across sites; enables per-site circuits |

**MoltBrowser's own Tor.** MoltNet runs only the Tor bundled with MoltBrowser
(a different Tor binary requires the explicit `--moltnet-tor-binary=<path>`
switch; there is no Homebrew/PATH fallback). Tor picks its own SOCKS and control
ports (`auto`), writes them to a private file, and every control connection uses
SAFECOOKIE authentication, checking Tor's proof (`SERVERHASH`) before sending
ours, so a program squatting on a port cannot impersonate Tor. Tor is told to
exit when MoltBrowser exits (`__OwningControllerProcess` + `TAKEOWNERSHIP`).
A system-wide `torrc-defaults` is ignored. Pasted bridge lines are validated
strictly (only obfs4/snowflake, fixed character sets) so they cannot inject
other Tor options.

**Supply chain.** Release builds refuse to bundle a Tor download whose SHA-256
is not pinned in `scripts/bundle-tor.sh`. The bump to Tor 15.0.15 left the pins
empty on purpose (the Tor archive was unreachable when the version was bumped),
so release builds fail until they are filled in using the gpg-verified steps
documented in that script.

## 3. What has been verified, and how

| Check | How | Result |
|---|---|---|
| Control-protocol parsing, SAFECOOKIE HMAC, torrc generation, bridge validation | 22 unit tests (`molt_ai_unittests`); HMAC vectors computed independently with Python `hmac` and `openssl` | Pass (built standalone; not yet built in the Chromium tree) |
| The generated torrc (incl. bridges + exit pin) is valid | `tor --verify-config`, Tor 0.4.9.11 | Pass |
| SAFECOOKIE against a real Tor; wrong cookie rejected via SERVERHASH; `TAKEOWNERSHIP` makes Tor exit when MoltBrowser's connection closes; `SETCONF`/`RESETCONF`/`NEWNYM` accepted | Standalone harness around the shipped control-connection code, Tor 0.4.9.11 | Pass |
| Tor accepts MoltBrowser's SOCKS5 username/password greeting and records a per-site username | Byte-for-byte replay against Tor 0.4.9.11 | Pass |
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

## 5. What MoltNet does *not* do

1. **It is not Tor Browser.** MoltNet windows still look like MoltBrowser on
   your real operating system. There is no fingerprinting uniformity: no
   letterboxing, and screen size, fonts, canvas/WebGL, time zone, language and
   the OS in the user agent are your real ones. Sites can recognise and link
   your visits by fingerprint. If your safety depends on not being identified,
   use Tor Browser.
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
   - Linux builds don't bundle Tor yet (use `--moltnet-tor-binary`), and the
     Windows packaging doesn't yet ship the pluggable transports, so bridges
     there need a manual install.
   - The Tor 15.0.15 SHA-256 pins in `scripts/bundle-tor.sh` are empty; release
     builds fail until they are filled in.
   - Bridges have been validated as configuration only; connecting through
     snowflake/obfs4 has not been tested yet.

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

**Global Privacy Control** is not offered: the Chromium version MoltBrowser is
built on has no `Sec-GPC` / `navigator.globalPrivacyControl` support, and
adding it correctly needs engine changes we haven't made.

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
