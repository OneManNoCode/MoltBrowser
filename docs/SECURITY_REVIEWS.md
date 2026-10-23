# Security reviews

Adversarial reviews of MoltBrowser code, newest first. Each entry gives the
scope, the method, the findings by severity and the issues fixed. Issues
that aren't fixed are tracked privately until they are.

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
