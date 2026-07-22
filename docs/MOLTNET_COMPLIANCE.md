# How we verify MoltNet's Tor routing

*MoltBrowser transparency page. Last reviewed 2026-07.*

MoltNet routes your whole browser through the 3-hop Tor network. Rather than ask
you to take that on faith, we ran an internal check and are publishing exactly
what we tested, what we found, how you can verify it yourself — and what MoltNet
does **not** protect you from.

We've tried hard not to oversell. Tor routing hides your IP address behind three
relays run by different volunteers, so no single one sees both who you are and
where you go. It is **not** total anonymity. The limitations in §4 are as
important as the evidence in §2.

> **How we talk about it.** We don't call MoltNet a "VPN," and we avoid
> *anonymous*, *untraceable*, and *military-grade* — they'd be misleading.
> MoltNet is **private IP routing over the Tor network**: it hides your IP and
> location. That's the honest description.

## What we found — in one paragraph

In our testing, MoltNet does what it says at the layers we checked: the three
relays it shows are real, currently-running Tor relays; your browser's traffic
egresses through a Tor exit; and if Tor goes down the browser **errors instead
of quietly connecting directly**. The known leak paths (WebRTC, DNS, prefetch,
HTTP/3) are closed in the shipped configuration. We have **not** yet commissioned
an independent third-party audit (§5 says what that would add), and §3 shows how
you can verify our claims yourself, without trusting us.

| Check | Result |
|---|---|
| The relays shown are real, published, running Tor relays | ✅ Confirmed |
| Your browsing egresses through a Tor exit | ✅ Confirmed |
| Fail-closed — Tor down means an error, never a direct connection | ✅ Confirmed |
| Leak-prevention settings applied (WebRTC / DNS / prefetch / DoH) | ✅ Confirmed |
| Independent third-party audit + full wire-level capture | ⏳ Not yet done — see §5 |

## 1. What MoltNet actually offers

1. **Your whole browser through 3-hop Tor** (guard → middle → exit) — not one
   proxied tab. *Scope note: Guest windows and secondary private windows are the
   exception (§4).*
2. **A visible route, with a country you can pick.** The panel names the three
   relays your traffic uses — nickname, country, IP.
3. **Leak-safe and fail-closed.** Steps to stop your IP leaking via WebRTC, DNS,
   or speculative connections; if Tor is unavailable the browser errors.
4. **Distributed trust, on purpose.** Real 3-hop Tor: no single relay sees both
   your IP and your destination. That's why we don't call it a VPN.

Tor picks your relays randomly and rotates them, so your circuit differs every
session. (Any specific relay you see in the panel can be looked up in the public
Tor directory — see §3.)

## 2. What we tested, and what we found

**2(a) The relays it shows are real, running Tor nodes.** We looked up each
relay's fingerprint in the Tor Project's *public* relay directory (onionoo) and
compared it to the panel. All three were genuine, currently-published, running
nodes; nickname, country, IP, and role all matched exactly (the guard carries
Tor's Guard flag, the exit the Exit flag). Nothing was fabricated. *(This proves
the relays are real; 2(b)/2(c) add that your traffic used them.)*

**2(b) Your browsing actually exits through Tor.** We loaded
`check.torproject.org` in the browser with MoltNet on; it reported `IsTor: true`
and a Tor exit IP, not the real one.

**2(c) It's fail-closed — nothing slips out directly.** With MoltNet on, a fresh
page load made **no direct connection** to the site — every connection went to
the local Tor entry (`127.0.0.1:9050`). With Tor stopped, the browser **errored**
rather than connecting directly. Routing only switches on once Tor confirms a
working circuit.

**2(d) The leak-prevention settings are actually on.** Verified on disk: proxy
(browser + system) = `socks5://127.0.0.1:9050`; WebRTC =
`disable_non_proxied_udp`; prefetch/preconnect = disabled; Secure DNS (DoH) =
off. The four classic proxy-leak holes are closed; DNS resolves through Tor.

**2(e) Old connections are dropped when you turn it on.** Enabling MoltNet closes
existing connections so they re-open through Tor instead of lingering on the
direct path. *(Added after our first test caught a few background connections
lingering briefly — they now drop to zero on enable.)*

## 3. Don't trust us — verify it yourself

The website tests are necessary but not enough on their own; the decisive checks
are the packet capture and the fail-closed test.

- **Check 0 — the relays are real.** In the MoltNet panel, click *⧉ Copy route
  evidence*, then:
  `curl 'https://onionoo.torproject.org/details?lookup=<FINGERPRINT>&fields=nickname,country_name,or_addresses,flags,running'`
  — GOOD if it returns `running=true` and the same nickname/country/IP with the
  expected flag. (Or the friendly page: metrics.torproject.org/rs.html.)
- **Check 1 — you're on Tor.** Visit `check.torproject.org` → it says you're on
  Tor with an IP that isn't yours. "New identity" should change the exit IP.
- **Checks 2–5 — no IP/DNS/WebRTC/IPv6 leak.** ipleak.net, browserleaks.com/ip,
  dnsleaktest.com (Extended), browserleaks.com/webrtc, test-ipv6.com — GOOD if
  none shows your real home/ISP IP or IPv6 and WebRTC reveals no local IP. (DNS
  resolving via the exit's upstream is fine; your *own* ISP resolver appearing
  is the problem.)
- **Check 6 — watch the wire (decisive).**
  `sudo tcpdump -n host <a-site-you-just-visited>` → expect ZERO packets;
  `sudo tcpdump -n udp port 53` → expect ZERO plain DNS. Only the Tor guard (or
  the local Tor entry) should be contacted.
- **Check 7 — fail-closed.** Quit Tor and try to browse → pages fail with a proxy
  error; the browser does not quietly reconnect directly.

## 4. What MoltNet does *not* do — read this part

Tor routing hides your IP address. It is not total anonymity.

1. **It doesn't make you anonymous to sites you log into.** Sign in or type your
   name/email/phone and the site knows who you are, Tor or not.
2. **It is not Tor Browser** (the biggest one). MoltNet hides your *IP*, but
   MoltBrowser is a normal Chromium-based browser with **no fingerprinting
   defenses** — your browser fingerprint can still make you recognizable and link
   your sessions. If your safety depends on being un-trackable, use the Tor
   Browser, which is built for that.
3. **The exit relay can see unencrypted traffic.** Tor encrypts between hops; the
   exit decrypts before the site. On plain `http://` the exit could read/tamper —
   always prefer HTTPS, even on Tor.
4. **It won't beat a global observer** who can watch both ends and correlate
   timing. Tor doesn't defend against that.
5. **Some sites block or CAPTCHA Tor** (shared exit IPs).
6. **It's slower** — traffic bounces through volunteer relays worldwide.
7. **Guest windows and secondary private windows aren't covered.** Whole-browser
   routing covers your normal windows and background traffic, but not a Guest
   window or a second incognito context.

**Where it genuinely helps:** real, free, distributed-trust Tor routing built
into the browser — the guard sees your IP but not your destination; the exit sees
your destination but not your IP. For hiding your IP and location from the sites
you visit and from your ISP, that's a strong, honest tool.

## 5. The honest bottom line

Based on our own testing, MoltNet's 3-hop Tor routing works as described: the
relays are real, your traffic exits through Tor, it fails closed rather than
leaking, and the known leak paths are shut. We fixed the one issue we found (a
few background connections lingering briefly on enable). We're publishing this so
the claim isn't just marketing.

We want to be equally clear about what this **isn't**: our own internal review,
not an independent third-party audit. A formal external audit would add wire-level
packet capture across every browser context, pin the observed exit to the exact
relay, and check background/OS traffic while idle. We haven't done that yet; when
we do, we'll publish it here too.

---

*The MoltBrowser team. Relay details come from the Tor Project's public directory
(onionoo / metrics.torproject.org). Leak-test sites are run by third parties.
MoltBrowser is not affiliated with or endorsed by the Tor Project. Powered by Tor.*
