# Security reviews

Adversarial reviews of MoltBrowser code, newest first. Each entry gives the
scope, the method, the findings by severity and the issues fixed. Issues
that aren't fixed are tracked privately until they are.

## 2026-10-23: PRs #47-#52 (October 2026)

**Scope.** The code merged in pull requests #47 to #52, in this order of
priority:

1. Page translation on the device (PR #48): reading a page's text in
   Translate's isolated world and writing the model's translations back
   (markup and attribute injection, nodes the page moves or swaps,
   named-property shadowing, `translate="no"`, the work a hostile page can
   cause, "Show original"), Google's translate script and language list,
   and translation being off in MoltNet windows.
2. The web agent's planner (PR #47): the plan parser, the step numbers in
   actions, REPLAN, what a plan written from page text can change, and
   the plan's display.
3. MoltNet (PRs #49 and #51): the New Identity sequence, certificate
   fetches, HTTP authentication schemes, captive portals, the pinned
   preferences and the SOCKS username of sandboxed pages.
4. GPU inference (PR #52): the Linux sandbox additions and the switch that
   enables them, the exception boundary around llama.cpp, how GPU failures
   are told apart from model crashes, and the shader build tools.
5. The address bar's full-width `@ai` and `@memory` (PR #47) and the
   follow-ups of PR #50.

**Method.** Each candidate traced from its attacker (web page, model
output, model file, network, local user) through MoltBrowser's code, the
vendored llama.cpp and the upstream Chromium 152 code at the pinned
revision; fixes tested with the standalone stand-in harnesses (system
gtest, GCC `-Werror`, clang ASan/UBSan, `-fno-exceptions`) with a mutant
that each new test catches, a syntax pass of the browser code against
declaration stand-ins with the 152 signatures, and the libFuzzer target of
the code touched.

**Findings.** 8: 2 medium, 2 low, 4 informational. Fixed: 1 of the low.

**Fixed.**

- On-device AI: the browser accepts a GPU failure report (the inference
  service's `kBackendFailed`) only from a model that runs on the GPU. From
  a model on the CPU, where llama.cpp doesn't throw, it is a protocol
  violation that counts against the model like a crash, so the crash-loop
  guard applies and the GPU stays available to other models.

## 2026-10-23: MoltNet's network isolation and linkability

**Scope.** MoltNet, MoltBrowser's Tor mode, against the claims in
`docs/MOLTNET_COMPLIANCE.md`:

1. Traffic that could leave a MoltNet window, or be caused by one, outside
   Tor: name resolution, UDP and QUIC, fetches the browser makes for a page
   (certificates, reporting, favicons, downloads, captive-portal checks,
   ...), workers and keepalive requests, extensions, internal pages, and
   MoltBrowser's own features (AI chat and agent, automation, memory,
   translation).
2. State that could link a MoltNet identity to the regular profile, one
   MoltNet session to the next, or one site to another within a session:
   network state and caches, settings and per-site exceptions, Tor circuit
   isolation and New Identity, MoltBrowser's own stores.
3. The Chromium 152 behaviour the compliance document relies on, and the
   coverage of the leak-test kit (`tools/leak_test/`).

**Method.** Each candidate traced from its trigger (page script, user
action, background task) through MoltBrowser's code and the upstream
Chromium 152 code at the pinned revision to the socket or the shared
state; Tor's control-port behaviour checked against Tor 0.4.9.11; fixes
tested with the standalone stand-in harnesses (gtest, ASan/UBSan,
`-fno-exceptions`) and mutants, and with new leak-kit cases checked against
its recorded sample run.

**Findings.** 9: 4 medium, 5 low. Fixed: 3 of the medium and 3 of the low.

**Fixed.**

- New Identity: the old identity's windows close even if a page asks
  "Leave site?", Tor is asked for new circuits only once the old profile is
  destroyed, and the new window opens only once Tor confirms that it has
  switched (`650 SIGNAL NEWNYM`), which can be up to ten seconds later. No
  other MoltNet window opens meanwhile.
- A MoltNet page always gets the prompt before an external application
  opens: the regular profile's "Always allow" exemptions are no longer read.
- Pages served sandboxed (opaque top-level origins) get per-site Tor
  circuits.
- MoltNet pages start at 100% zoom and never take the regular profile's
  zoom levels, also with fingerprinting protection switched off.
- The Tor control connection waits with `poll()` instead of `select()` on
  POSIX, and frames replies and asynchronous events separately.

**Fixed since.** The other three (the remaining medium and two low), in a
follow-up the same day:

- Certificate fetches made while checking a MoltNet site's certificate
  (missing intermediate certificates, revocation checks) each get a
  one-off network key: never cached, never merged with another fetch, and
  on a Tor circuit of their own.
- MoltNet windows don't check for captive portals and never open a
  network sign-in page.
- MoltNet windows offer sites Basic and Digest authentication only, never
  NTLM or Negotiate (Kerberos).

## 2026-10-22: PRs #26-#45 (October 2026)

**Scope.** The code merged in pull requests #26 to #45, in this order of
priority:

1. What any web page reaches: the built-in AI APIs (`molt_ai/web_ai/`:
   the manager and its binder, sessions, the consent prompt and remembered
   answers, limits and the scheduler, image input, what a page can learn),
   the web agent's site tools (WebMCP), page content entering the agent and
   the chat (screenshots, element snapshots, chat proposals, fencing) and
   the MoltNet fingerprinting changes (scrollbars, viewport lengths, text
   metrics, zoom letterboxing).
2. What a hostile model or a prompt-injected page reaches: every place a
   model reply becomes an action, a URL, markup, a file or a stored record.
3. Crafted input: `molt://memory/?q=`, the omnibox's `@memory` / `@ai`
   handling, imported workflows, and the build provenance and release
   verification tools.
4. The WebUI pages added or changed in those pull requests.

**Method.** Code reading with an attacker named for each candidate (web
page, model, file, network, local user), tracing input to outcome; the
upstream Chromium 152 code the changes rely on, read at the pinned
revision; regression tests against the standalone stand-in harnesses
(gtest), jsdom tests of the page scripts, the provenance test suite, and
the libFuzzer targets of the code touched.

**Findings.** 14: 1 medium, 8 low, 5 informational. Fixed: the medium and
6 of the low.

**Fixed.**

- The web agent's screenshots: the browser counts the frames the observed
  document owns and takes no screenshot when the page script that finds
  what to mask didn't find all of them.
- The built-in AI APIs: `create()` answers every user-specific unavailable
  reason alike; the consent bar ignores Allow and Block for half a second
  after it appears; a document's pending `create()` calls are bounded.
- The web agent's page scripts read a form's controls, attributes and
  action through the prototypes' accessors (site tools, the click probe).
- `molt_provenance.py verify` no longer reports success for files it
  doesn't cover, and compares a given statement file with its signed copy.

**Fixed since:** the remaining findings, in the follow-up to this review:

- The web agent takes no screenshot of a page with a frame of another
  origin anywhere in it (the browser counts them), nor while a frame of the
  tab has just navigated or gone away.
- The built-in AI APIs: how long `availability()` and `create()` take no
  longer depends on other sites' calls (device facts kept per site);
  sessions check the switches, the policies and the site's answer on every
  call and end when one turns them off; the consent bar ignores repeated
  rapid presses, as Chromium's dialogs do.
- MoltNet's uniform text rendering on Linux applies whatever the desktop's
  anti-aliasing setting.
- The release tools check names, versions and digests as whole ASCII
  values.
- The address bar's `@ai` and `@memory` shortcuts also match their
  full-width forms, as an East Asian input method types them (PR #47).
