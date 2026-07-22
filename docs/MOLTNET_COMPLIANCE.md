# MoltNet Compliance Validation Report
### Whole-Browser Tor Routing — Evidence Validation

**Prepared for:** MoltBrowser founder (for onward delivery to a cybersecurity / compliance reviewer)
**Date of evidence capture:** 2026-07-22
**Scope:** Validation of MoltBrowser's "MoltNet" feature claim that browser traffic is routed through the live 3-hop Tor network in a leak-safe manner, with a transparent route panel.
**Evidence basis:** This report is grounded entirely in (a) live onionoo relay-directory lookups, (b) in-browser observation of `check.torproject.org`, (c) captured browser socket/proxy behavior, and (d) inspection of the applied profile/local-state preferences. Claims not supported by that evidence are explicitly marked as *not tested here*.

**Language note:** Consistent with Tor Project trademark guidance and consumer-protection precedent, this report avoids the words *anonymous*, *untraceable*, *military-grade*, and *VPN*. MoltNet is described as **private IP routing over the Tor network**, not as anonymity.

---

## 1. What MoltNet Claims

MoltNet, as presented in the MoltBrowser route panel, asserts four things:

1. **Whole-browser routing through 3-hop Tor.** All browser traffic (not a single proxied tab) is carried over a standard Tor circuit: **guard → middle → exit**.
2. **Pick-your-exit / transparent circuit.** The panel names the three relays it is using, by nickname, country, and IPv4 address, so the user can see the specific path their traffic takes.
3. **Leak-safe by design.** The browser does not leak the origin IP via WebRTC, DNS, speculative connections, or a direct fallback path; it is **fail-closed** (if Tor is unavailable, connections error rather than silently going direct).
4. **Distributed-trust routing (deliberately not a "VPN").** Because the path is real 3-hop Tor, no single relay sees both the user's origin IP and their destination.

The circuit under test, as displayed by the panel:

| Position | Nickname | Country | IPv4 | Fingerprint |
|---|---|---|---|---|
| Origin (pre-Tor) | — | — | `76.68.224.242` | (user's apparent origin IP) |
| Guard | `fr13nds` | Ireland | `188.165.4.146` | `B369F97F3A75CCDCC219B3B7823370010614E956` |
| Middle | `anarchosyndical5` | Canada | `158.69.0.227` | `BCDA2D73F03228680A310619C8CD3E23F7EA92D8` |
| Exit | `NTH113R4` | Netherlands | `192.42.116.113` | `3CD664053567A1EBAC410598A4FA634AF9C1FA59` |

The remainder of this report tests whether these claims are substantiated.

---

## 2. Evidence That the Claim Is Genuine

Each check below states **what was checked**, **what was observed**, and a **confidence** rating.

### 2(a) The named relays are real, published, running, and match the panel

**Check.** Each of the three fingerprints was looked up on the Tor Project's public relay-directory API (onionoo), one request per fingerprint:

```
GET https://onionoo.torproject.org/details?lookup=<FP>&fields=nickname,country_name,or_addresses,exit_addresses,flags,running,last_seen
```

**Observed.**

- **Guard `fr13nds`** (`B369F97F…E956`): onionoo returns `nickname='fr13nds'`, `country_name='Ireland'`, `or_addresses=['188.165.4.146:8080', '[2001:41d0:24e:fe00::32]:8080']` — IPv4 **matches** the panel. Flags include **Guard** (plus Fast, HSDir, Running, Stable, V2Dir, Valid). `running=true`, `last_seen='2026-07-22 18:00:00'`. No `exit_addresses` field (correct for a non-exit relay).
- **Middle `anarchosyndical5`** (`BCDA2D73…92D8`): `nickname='anarchosyndical5'`, `country_name='Canada'`, `or_addresses=['158.69.0.227:9001']` — IPv4 **matches**. Flags `[Fast, Guard, Running, Stable, V2Dir, Valid]` with **no Exit flag** (consistent with guard/middle use only). `running=true`, `last_seen='2026-07-22 18:00:00'`.
- **Exit `NTH113R4`** (`3CD66405…FA59`): `nickname='NTH113R4'`, `country_name='Netherlands'`, `or_addresses=['192.42.116.113:9003', '[2001:67c:e60:c0c:192:42:116:113]:9003']` — IPv4 **matches**. Flags include **Exit** (plus Fast, Running, Stable, Valid). `running=true`, `last_seen='2026-07-22 18:00:00'`. Its `exit_addresses=['192.42.116.113']`.

**Result.** All three relays are genuine, currently published, and running, and every field the panel displays — nickname, country, OR IPv4, and the role-critical flag (Guard on the guard, Exit on the exit, no-Exit on the middle) — matches onionoo exactly.

**Confidence: High** for "the panel is naming real relays that are capable of the claimed roles."
**Limitation (important):** onionoo is a *public* directory. Anyone can look up these relays whether or not they carried this user's traffic, and onionoo does not encode circuit *position* — "guard/middle/exit" are per-circuit roles, not relay properties. This check confirms **capability and identity**, not that **this user's traffic traversed this exact ordered circuit**. That attribution rests on §2(b) and §2(c).

### 2(b) The exit is a real Tor exit and the browser page-path is Tor

**Check.** `check.torproject.org/api/ip` was loaded **inside the browser under audit**.

**Observed.** Returned `{"IsTor":true,"IP":"192.42.116.17"}`.

**Result.** The Tor Project's own endpoint confirms the browser's page-fetch path egressed through a Tor exit at that moment. The reported egress IP `192.42.116.17` is **not** the panel exit's own address (`192.42.116.113`) but sits in the **same `192.42.116.0/24` operator block** — a known multi-relay exit operator. Per the onionoo data, the panel exit `NTH113R4` egresses from its own OR address `192.42.116.113`; `.17` is therefore a *different fingerprint within the same operator /24*, not this exit's egress.

**Confidence: High** that the browser's page traffic exited via Tor; **Medium** on positive positional attribution to `NTH113R4` specifically, because the observed egress (`.17`) is an operator-sibling rather than a byte-for-byte match to the panel exit (`.113`). A formal audit should reconcile this (see §5).

### 2(c) Fail-closed behavior and no-direct-leak socket proof

**Check.** With MoltNet ON, a fresh navigation to `example.org` was performed while observing the browser's outbound sockets; the Tor daemon was then made unavailable to test fallback behavior.

**Observed.**
- The new navigation made **no direct external TCP connection**; the only outbound socket was to **`127.0.0.1:9050`** (the local Tor SOCKS listener).
- The proxy is a **fixed `socks5://127.0.0.1:9050`** with **no DIRECT fallback and no PAC script**. With Tor down, the browser returns **`ERR_PROXY_CONNECTION_FAILED`** — it errors rather than silently connecting directly.
- Routing is applied **only after** the daemon reports `GETINFO status/circuit-established=1`, so traffic is not released before a circuit exists.

**Result.** The browser is **fail-closed**: destination traffic is emitted only via the local SOCKS proxy, and loss of Tor produces a hard error, not a clearnet leak.

**Confidence: High**, based on directly observed socket behavior and the fixed-proxy / no-fallback configuration.

### 2(d) The leak-prevention configuration is actually applied

**Check.** The active profile prefs and local-state were inspected to confirm the anti-leak settings are in force (not merely intended).

**Observed.**

| Setting | Value | Purpose |
|---|---|---|
| `profile.proxy` | `socks5://127.0.0.1:9050` | All profile traffic via Tor SOCKS |
| `local_state.proxy` | `socks5://127.0.0.1:9050` | System network context via Tor SOCKS |
| `webrtc.ip_handling_policy` | `disable_non_proxied_udp` | Prevents WebRTC/STUN from opening a non-proxied path that could reveal the real IP |
| `net.network_prediction_options` | `2` (disabled) | Disables speculative prefetch/preconnect that could bypass the proxy |
| `dns_over_https.mode` | `off` | Prevents an independent DoH resolver path; name resolution goes remotely through Tor via SOCKS5 |

**Result.** The four classic proxy-leak holes — WebRTC UDP, speculative connections, DoH, and local DNS — are closed in the applied configuration. Because the proxy is `socks5://` (remote DNS), hostname resolution occurs at the Tor exit rather than locally.

**Confidence: High** that the settings are applied. These are **leak mitigations**, not anonymity guarantees (see §4).

### 2(e) Connection teardown on enable

**Check.** Whether pre-existing, pre-Tor keep-alive connections are severed when MoltNet is turned on (so long-lived sockets do not keep leaking outside Tor).

**Observed.** Pre-enable keep-alive connections to Google, Anthropic, and Microsoft **drained to 0** after enabling; MoltNet now calls **`CloseAllConnections`** on enable.

**Result.** Toggling routing ON tears down connections that were established over the direct path, rather than allowing them to persist outside Tor.

**Confidence: High** for the observed teardown of the sampled connections.

---

## 3. How to Reproduce This Independently (Hand to a Security Team)

The following battery lets a reviewer confirm the claim **without trusting MoltBrowser**. A genuine PASS requires **every layer** to pass simultaneously; the browser-visible sites (Tests 2–5) are necessary but **not sufficient** — the decisive evidence is host-side packet capture (Test 6) and fail-closed behavior (Test 7). This mirrors the Whonix *Dev/Leak_Tests* methodology.

### Test 0 — Relay reality (onionoo)
For each fingerprint shown in the panel:
```
curl 'https://onionoo.torproject.org/details?lookup=B369F97F3A75CCDCC219B3B7823370010614E956&fields=nickname,country_name,or_addresses,flags,running,last_seen'
```
Repeat for `BCDA2D73F03228680A310619C8CD3E23F7EA92D8` and `3CD664053567A1EBAC410598A4FA634AF9C1FA59`.
**PASS:** each returns `running=true`, a recent `last_seen`, the nickname/country/OR-IP shown in the panel, and the expected flag (Guard on the guard, Exit on the exit).

### Test 1 — Tor confirmation (in the browser under audit)
Load `https://check.torproject.org/api/ip`.
**PASS:** `"IsTor":true` **and** the returned IP is not your ISP/public IP.
**Rigorous:** trigger a new circuit ("New Identity") and reload — the exit IP must **rotate**, confirming a live Tor path rather than a hard-coded banner.

### Test 2 — IP leak
Load `https://ipleak.net` and `https://browserleaks.com/ip`.
**PASS:** every reported address (HTTP source IP, any JavaScript-detected IP, `X-Forwarded-For`) equals a Tor exit IP; your real public/ISP IP appears **nowhere**. Any appearance of the true public IP = **FAIL**.

### Test 3 — DNS leak
Run `https://dnsleaktest.com` **Extended Test** (~50 hostnames), plus `ipleak.net`'s DNS detection and `https://browserleaks.com/dns`.
**PASS:** the resolvers observed geolocate to the exit's upstream, not to you; **your ISP/hotel/corporate resolver never appears.** Note: a large share of Tor exit DNS legitimately terminates at public resolvers (notably Google) — that is exit-side upstream, **not** a host leak. Your **local** resolver appearing is the actual FAIL.

### Test 4 — WebRTC leak
Load `https://browserleaks.com/webrtc`.
**PASS (for a Tor-routed general browser):** no ICE candidate reveals the real host — no RFC1918 private IP, no real public IPv4/IPv6; any public candidate equals the Tor exit. **FAIL:** any real private or public host IP appears. (MoltNet sets `disable_non_proxied_udp`, which is the setting that should produce this PASS.)

### Test 5 — IPv6 leak
Load `https://test-ipv6.com` and check the IPv6 rows on `ipleak.net`/`browserleaks`.
**PASS:** no reachable public IPv6 / no IPv6 endpoint reveals a real address. **FAIL:** a real IPv6 address appears alongside the Tor IPv4 exit (classic dual-stack bypass).

### Test 6 — Packet-level ground truth (decisive; requires host root)
While browsing, capture on the host NIC:
```
sudo tcpdump -n -i <nic>
```
Then, having visited a known destination, prove the machine never contacted it directly:
```
sudo tcpdump -n -i <nic> host <a_destination_you_visited>   # expect ZERO packets
sudo tcpdump -n -i <nic> udp port 53                        # expect ZERO plaintext DNS
sudo tcpdump -n -i <nic> udp port 443                       # expect ZERO QUIC/HTTP3 to destinations
```
**PASS:** the only outbound TCP SYNs go to the configured Tor guard/entry (here, expected to be the guard `188.165.4.146` on its ORPort, or the local `127.0.0.1:9050` SOCKS listener if capturing loopback); zero packets to visited destinations; zero UDP :53; zero destination QUIC. Absence of destination packets on the wire is the proof that traffic exits only through Tor.

### Test 7 — Fail-closed
Stop the Tor process/daemon (e.g. `sudo systemctl stop tor@default` or terminate the bundled tor), then try to browse and reload the leak sites, with `tcpdump` running.
**PASS:** the browser **hard-errors** (here: `ERR_PROXY_CONNECTION_FAILED`) and emits **zero** direct destination packets. A browser that still loads pages with Tor dead is **FAIL (fail-open).**

### Rigorous extras a formal audit should add
- **Teardown-on-enable:** open a long-lived direct connection (websocket/SSE/keep-alive), enable MoltNet, and confirm via capture that the old socket is RST/torn down rather than allowed to continue. (MoltNet's `CloseAllConnections` is the mechanism to verify.)
- **Background/non-page traffic:** capture at the OS level while the browser is idle to confirm auto-updater, telemetry, crash reporting, Safe-Browsing, OCSP/CRL, extension updates, captive-portal probes, and NTP are Tor-routed or blocked. Any non-guard SYN here is a real leak.
- **Per-context coverage:** re-run Tests 1–6 in private/OTR windows, extra profiles, and service-worker contexts, and check Tor **stream isolation** (distinct contexts should use separate circuits). **Note the disclosed scope gap in §4.**

---

## 4. Honest Limitations — What Tor Routing Does *Not* Protect Against

MoltNet hides the origin IP behind a 3-hop circuit. It is **not** full anonymity, and must not be marketed as such. The following limitations are inherent to Tor and/or specific to MoltNet's stock-Chromium base.

1. **Tor alone is not anonymity.** Signing into a site, or entering a name/email/phone, makes you identifiable to that site regardless of routing; persistent cookies and saved logins link Tor traffic back to you at the application layer. *(Tor Project, "Am I totally anonymous if I use Tor?")*
2. **User-side deanonymization vectors persist.** Torrent clients ignore proxy settings and send the real IP to trackers; some plugins can be manipulated into revealing the IP; documents downloaded over Tor and opened while online can fetch resources outside Tor. Routing does not close these. *(Same source.)*
3. **The exit node sees plaintext and can modify it.** Tor encryption is hop-by-hop; the exit decrypts before the destination. For non-HTTPS connections the exit can read and inject content — so HTTPS remains essential even over Tor. Real-world malicious-exit incidents (e.g., 2020 HTTPS-downgrade clusters targeting cryptocurrency) are documented by third-party researchers. Tor moves the last-mile eavesdropper from your ISP to an unknown exit operator.
4. **End-to-end correlation is out of scope.** An adversary who can observe both entry (guard) and exit can correlate timing to deanonymize; Tor is low-latency and does not pad traffic. "Tor does not defend against such a threat model." *(Tor Project, attacks-on-onion-routing.)*
5. **Fingerprinting — the largest MoltNet-specific gap.** MoltNet is **stock Chromium** and ships **no fingerprint hardening** (no `resistFingerprinting`, no letterboxing, no UA normalization — verified by codebase inspection of `src/chrome/browser/molt_ai`). Canvas hash, font list, window size, real user-agent, timezone, and WebGL can still uniquely identify and **link** the user across sessions even though the exit IP is Tor's. **MoltNet provides IP-level location privacy, not the fingerprint-level protection of Tor Browser, and must never be described as equivalent to Tor Browser.**
6. **Sites CAPTCHA or block Tor exits.** Because many users share exit IPs, sites (Cloudflare and others) assign exit IPs high threat scores and present CAPTCHAs, temporary blocks, or full Tor blocks. This is an availability/usability limitation to disclose. *(Tor Project, tbb-44.)*
7. **Speed is inherently slower.** Traffic bounces through volunteer relays across the world; the 3-hop overhead and exit congestion mean higher latency and lower throughput than a direct connection. *(Tor Project, tbb-22.)*
8. **Scope caveat (MoltNet-specific).** Per the routing security review, **whole-browser and system-network-context routing are covered, but guest windows and non-primary off-the-record windows are NOT covered by the proxy pref.** The report should not claim "all browser traffic is routed" without this qualifier.

**Where MoltNet is genuinely stronger:** it uses real 3-hop Tor (`socks5://127.0.0.1:9050` to the local Tor daemon), inheriting Tor's **distributed-trust** property — the guard sees your IP but not your destination, the exit sees your destination but not your IP. That is structurally stronger than a single-hop commercial service where one company sees both — which is exactly why the feature is deliberately **not** called a "VPN."

---

## 5. Compliance Verdict

**On the evidence captured, MoltNet's claim that it "routes the whole browser through 3-hop Tor, leak-safe" is substantiated at the layers tested.** The three panel relays are real, published, running Tor relays whose nickname, country, OR IPv4, and role-flags match the panel exactly (onionoo, all `last_seen 2026-07-22 18:00:00`); the browser's page path egresses through a Tor exit (`check.torproject.org` → `IsTor:true`, egress `192.42.116.17` in the exit operator's `192.42.116.0/24`); the browser is **fail-closed** (fixed `socks5://127.0.0.1:9050`, no DIRECT/PAC fallback, `ERR_PROXY_CONNECTION_FAILED` when Tor is down, routing gated on `circuit-established=1`); the leak-prevention prefs are actually applied (`webrtc.ip_handling_policy=disable_non_proxied_udp`, `network_prediction_options=disabled`, `dns_over_https.mode=off`, remote DNS via SOCKS5); and pre-existing direct connections are torn down on enable (`CloseAllConnections`, sampled keep-alives drained to 0). The positioning is honest: real distributed-trust Tor routing, not anonymity, and deliberately not called a "VPN."

**What a formal audit would still want to test**, and which this run did not fully establish: (1) **host-side packet capture** (`tcpdump`/Wireshark) to prove at the wire level that the machine opens TCP only to the guard `188.165.4.146` (or loopback SOCKS), emits zero UDP :53, and zero destination QUIC — the decisive ground-truth layer; (2) **positional attribution** of the observed egress — `check.torproject.org` returned `192.42.116.17`, an operator-sibling of the panel exit `192.42.116.113`, which a rigorous audit should reconcile to confirm the exact circuit; (3) **background/OS-service coverage** (updater, telemetry, OCSP, Safe-Browsing, NTP, extension updates) captured while idle; (4) **per-context coverage** including the disclosed **guest / non-primary OTR scope gap**, plus IPv6, WebRTC, and DNS re-tests in each context and Tor stream isolation; and (5) an explicit **"MoltNet is not Tor Browser"** statement in product copy covering the **fingerprint-hardening gap** (item 5 above), since that is the most likely point of over-inference. With those additions the audit would move from "substantiated at the layers observed" to a complete, wire-level PASS.

---

*Provenance:* onionoo lookups, `check.torproject.org` observation, socket/proxy capture, and pref inspection performed 2026-07-22 on the dev-installed build. Tor Project citations are first-party support-page guidance; malicious-exit incidents are third-party/press reports and are cited as such. MoltNet codebase claims were verified against the working tree in `src/chrome/browser/molt_ai` on 2026-07-22 and should be re-verified against the exact release build/tag the final compliance report describes.