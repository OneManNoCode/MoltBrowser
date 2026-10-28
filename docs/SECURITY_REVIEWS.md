# Security reviews

Adversarial reviews of MoltBrowser code, newest first. Each entry gives the
scope, the method, the findings by severity and the issues fixed. Issues
that aren't fixed are tracked privately until they are.

## 2026-10-28: PR #74 (October 2026)

**Scope.** The code merged in pull request #74 (round 30) that the
previous review didn't cover, in this order of priority:

1. MoltNet's Background Fetch with no total for all sites: each site's
   share, the memory valve told of each download past the floor, the
   in-memory download service with no limits of its own (in every
   off-the-record profile), the scheduler's new per-site queues and its
   hooks, starvation, counts that could drift or overflow, the controllers'
   lifetime; whether one site can hold unbounded memory or tasks before the
   valve acts, or end an identity at will.
2. Storage under a page's nonce: the tab helper's tracking (frames as they
   are made and as they commit, the page going, pages in the back/forward
   cache and prerendered ones), the bound of 32 groups per site, the drop
   in the storage service and the quota manager and who can ask for it,
   closed groups.
3. DOM storage in short turns: the usage walk and the memory dump, a tab's
   `sessionStorage` copied or deleted a chunk a task, control messages held
   and the order they are replayed in, reentrancy, a namespace or area that
   goes during a job, a message never replayed.
4. Per-site records forgotten: whether a site can reset its write
   allowance or commit rate by emptying its storage, or make records come
   and go to cost processor time; keepalive recorders and quota marks.
5. Local Network Access for Background Fetch requests made without the
   worker's factory, and fetches taken up after a restart, in every
   profile: the factory, the wait for a fetch's parameters, its deadline
   and the second request for them, with the previous review's restart; a
   permission prompt out of context; downloads made to fail for other
   sites.
6. MoltNet's microphone processing: the per-instance scope in WebRTC (other
   instances on one thread, components made later or on other threads),
   the setting carried over Mojo from either kind of profile, the residual
   echo estimator left out, the portable math calls and WebRTC's other
   vector code (the signal processing library, PFFFT).
7. The leak test kit: the microphone launch's flags and input file, and
   how the harness handles what pages answer.
8. The build tools: the compile check's result cache (stale or planted
   results), the HSTS generator built and run on the host, the paths
   `clean` removes.

**Method.** Each candidate traced from its attacker (web page, a group of
sites, a compromised renderer, a local process of the user who runs the
leak kit, a source edited during a check) through MoltBrowser's code and
the upstream Chromium 156 code at the pinned revision, and WebRTC's and
PFFFT's at the revisions it pins (read from the release's source tarball);
fixes tested with the leak kit's test suite and new tests that put links,
hard links, FIFOs and a moved run directory where the harness writes,
with a mutant set each test catches, run as root and unprivileged; the
nonce policy's unit tests built standalone (system gtest, g++
ASan/UBSan) with mutants they catch; the namespace harness of the
previous round (`SessionStorageImpl`'s and `SessionStorageNamespaceImpl`'s
copy and deletion verbatim, 156's and this branch's, clang ASan/UBSan)
with new tests of a copy owed to a populated namespace, in a message and
outside one, and mutants they catch, and a unit test of the storage
service's per-site host, which needs a Chromium build; the compile
check's test suite with a new test that edits a source while it
compiles, and mutants it catches; and the compile check against Chromium
156's headers.

**Findings.** 14: 3 low, 11 informational. Fixed: the three low and one
informational.

**Fixed.**

- The leak test kit: the harness, which runs as root, writes into the run
  directory it gives the invoking user only through a descriptor of that
  directory opened when it is made: no symbolic link is followed at any
  step of a path, only regular files with a single link are written or
  emptied, and owners and modes are set on the open file
  (`tools/leak_test/leakkit/rundir.py`). That covers the microphone
  launch's input file, every launch's and server's log, the profile's
  settings and the run's manifest; the servers' certificates and the test
  extension are made in the root-owned directory under `/run` and copied
  in.
- MoltNet: a page its server sandboxed (an opaque origin) counts toward the
  bound of the site its origin came from on how many pages keep storage
  under their nonce, rather than toward one bound that every site's
  sandboxed pages shared.
- MoltNet's DOM storage: a tab's `sessionStorage` deletion makes the
  copies still owed to tabs it opened, in a task of its own after its
  sites' turns. A copy owed to a namespace that was populated meanwhile is
  now dropped there without reporting a message, none being handled, as
  Chromium drops it while handling one (`session_storage_impl.cc`).
- The compile check: a file's result is stored under the key made from
  what preprocessing read, and the compile reads the working tree again.
  A result is now stored only if no C-family file of the working tree
  changed (size, modification time or inode) from before its key was made
  to the end of its compile (`ccheck/check.py`, `ccheck/sources.py`).

## 2026-10-27: PR #73 (October 2026)

**Scope.** The code merged in pull request #73 (rounds 28 and 29) that
the previous review didn't cover, in this order of priority:

1. MoltNet's DOM storage on threads of its own: the storage service's new
   binding methods, who can call them and for which partitions, the
   threads' lifetime and shutdown; the fair scheduler's charge in
   processor time and the control pipe's weight; notifications and frees
   split into short tasks (their order, an area that goes in the middle,
   reentrancy); unused empty areas dropped in their site's turn, and a
   binding that arrives meanwhile; and the in-memory HTTP cache's sweep in
   steps, its monotonic stamps, the table of sites made once with a seeded
   hash, its cap on sites, and the sweep's position kept across steps.
2. The previous review's fixes: what a resumed Background Fetch request
   carries, in every profile and on every path a download is resumed or
   started again, a later browser session included; the refusal of name
   lookups and host resolvers for MoltNet; and when the Background Fetch
   scheduler looks at the registrations waiting to become active.
3. MoltNet's processor requirement: every way into MoltNet; the JPEG XL
   decoder's portable mode (which processes turn it on, and when);
   half-float conversions, time-stretched media audio and its window's
   cosine.
4. The compile check's changes to sources: Windows paths and time
   functions, `raw_ptr` fields, special members defined out of the class
   body in headers, and the virtual hooks moved into `.cc` files, in
   component builds too.
5. The build tools as a supply chain: what the compile check and the
   static GN check download, how it is checked and unpacked, and what of
   it runs on the machine; the Linux repository tool's check of rpm
   signatures.
6. Regular profiles: that each MoltNet-only path is taken only in MoltNet.

**Method.** Each candidate traced from its attacker (web page,
third-party frame, a site's server, a group of sites, a download source,
the build machine's network) through MoltBrowser's code and the upstream
Chromium 156 code at the pinned revision, and V8's and jxl-rs's at the
revisions it pins; fixes tested with a standalone harness (system gtest,
g++ ASan/UBSan) that compiles the download service's changed functions
verbatim against stand-ins with 156's shapes, with a mutant set each test
catches, a unit test on the download service's real request parameters,
a DLL and an executable linked with Chromium's clang-cl and lld-link as a
Windows component build links them, the tools' own test suites and a
fetch of the pinned release's tarball, and the compile check against
Chromium 156's headers.

**Findings.** 13: 3 low, 10 informational. Fixed: the three low and two
informational.

**Fixed.**

- Background Fetch, every profile: the download service keeps a page's
  fetch across browser sessions, but not its initiator or network
  isolation, which Background Fetch gives back when the download starts
  again in a later session. A download whose item the browser kept from
  the earlier session was then resumed without them, as a request the
  browser makes itself (first-party at its destination, with
  credentials); it now starts again from the beginning with them, as a
  request with a body already did. A download started afresh in a later
  session keeps its first party on redirects, as in its first session.
  Other clients' downloads are unchanged.
- The build tools: Chromium's source tarball, which the static GN check
  and the compile check stream and whose scripts and DEPS files run on the
  machine, is used only if it matches a SHA-256 pinned in the repository,
  and none of its files is written outside its directory. The compile
  check builds its cxxbridge with the dependency versions the crate's lock
  file pins, not the newest ones crates.io has.
- Builds: the Blob registry's delegate is exported, so in a Windows
  component build libraries other than the storage one link against its
  method defined in the `.cc` file; and the web AI translation header's
  include of the inference engine's `raw_ptr` exclusion is allowed by its
  DEPS rules, which checkdeps had refused.

## 2026-10-26: PR #72 and round 28 (October 2026)

**Scope.** The code merged in pull request #72 (round 27) and round 28's
MoltNet changes, in this order of priority:

1. Background Fetch scheduled per site in MoltNet: the scheduler's
   containers while it activates and starts requests, the controllers'
   lifetime when a registration is aborted, a job finishes or its service
   worker goes away, starvation and whether one site can keep another's
   fetches from starting, the in-memory download service's limits, and
   regular profiles staying as Chromium has them.
2. DOM storage in MoltNet: the mode without a database, the write
   allowance, the storage service's sequence taken in turn per site
   (reentrancy, tasks of an area or site that is gone, the order callers
   rely on, a task that never runs), the per-item and per-area overhead
   in the totals, the groups of storage keys (nonces, opaque top-level
   sites), and the restore of a refused write's old value, which runs in
   every profile.
3. The in-memory HTTP cache's list per site: the second link in every
   entry on each doom, eviction and destruction path, the walks during
   eviction and the idle sweep, the check on a new entry's key, and sparse
   data's child entries.
4. The quota system's usage counted per site: after deletions, eviction
   and the end of an identity, and what a quota answer reads.
5. MoltNet's network context and sockets outside its own session: the
   socket factory with a session of its own, the WebRTC socket manager,
   raw sockets and multicast DNS, and the rest of the network context's
   interface at Chromium 156 (name lookups and host resolvers,
   WebTransport, proxy lookups, preconnects), with the browser features
   that ask for them.
6. What a Background Fetch request carries in every profile (initiator,
   network isolation, credentials, `Sec-Fetch-*`, `Origin` and `Referer`,
   SameSite and third-party cookie decisions, CORS, Local Network
   Access), on each path: the download service of regular profiles, the
   in-memory one of private profiles, registrations restored at startup,
   requests resumed or retried, and MoltNet's one-off key for a retried
   download.

**Method.** Each candidate traced from its attacker (web page, third-party
frame, a site and a server working together, a group of sites, local
user) through MoltBrowser's code and the upstream Chromium 156 code at the
pinned revision, through to the network service's request, header and
cookie decisions; fixes tested with standalone harnesses (system gtest,
g++ ASan/UBSan, clang `-fno-exceptions`) that compile the new header and
the changed scheduler function verbatim against stand-ins with 156's
shapes, with a mutant set each test catches, unit tests on Chromium's
real download request and network context, and the compile check against
Chromium 156's headers.

**Findings.** 10: 1 medium, 2 low, 7 informational. Fixed: the medium and
both low.

**Fixed.**

- Background Fetch, every profile: a request whose response stops early
  and is resumed carries its first request's initiator, network isolation
  and credentials mode, and keeps its first party on redirects. In a
  regular profile the download item that resumes it used none of them,
  so the resumed request was made as one the browser starts itself:
  first-party at its destination, with credentials whatever the fetch
  asked for. Other downloads resume as before.
- MoltNet: the network context looks up no name itself. Its name lookups
  and host resolvers, which some browser pages and services ask for, used
  the context's own host resolver outside Tor; they are now refused, so
  only Tor resolves names for MoltNet.
- MoltNet's Background Fetch scheduler looks at the registrations waiting
  to become active only when one is made or one finishes, not on every
  request's start and end, so a site keeping many waiting doesn't add to
  the work every site's requests cause in the browser.

## 2026-10-26: PRs #70 and #71 (October 2026)

**Scope.** The code merged in pull requests #70 and #71 (rounds 25 and
26), in this order of priority:

1. The end of a MoltNet identity: every way a request, a socket write or
   a name lookup could still leave once the end reaches a network context
   (URL loader factories, WebSockets, WebTransport, preconnects,
   prefetches, host resolution, the SOCKS refusal of writes and
   handshakes), contexts made after the end and after a network service
   restart, the browser's wait for every context and its 5-second
   deadline, the requests the browser holds for pages (keepalive,
   `fetchLater`, Background Fetch, downloads, Save Page As), New Identity
   while an end is under way, and a new identity's starting state.
2. The per-site budgets: the in-memory HTTP cache's shares, the
   `localStorage` and `sessionStorage` totals, cache groups and commit
   rates, the Background Fetch buffer counted against blob shares and its
   site keys, the storage quota share, and the memory valve: integer
   ranges, map growth, entry and area lifetimes, thread affinity, stale
   callbacks, what one site can make another see or lose, and who can
   make the valve end an identity.
3. The certificate-fetch pools and the split connection budget, and their
   checks against Tor's capacity.
4. The de-Google changes: the component registration guards and their
   switch, the on-device model manifest asked for on demand, the feature
   overrides, and the traffic-capture kit (its TLS key, the namespace set
   up as root, the parsing of what pages send).
5. Portable math and display capture in MoltNet: other ways to a picker or
   a capture (workers, frames, Permissions Policy, extensions), and the
   remaining C library calls a page can observe.
6. The one-click rule on every MoltBrowser page, with the handlers PR #71
   added.

**Method.** Each candidate traced from its attacker (web page, a third
party's script, a group of sites, a compromised WebUI script, local
environment) through MoltBrowser's code and the upstream Chromium 156 code
at the pinned revision; fixes tested with standalone harnesses (system
gtest, g++ ASan/UBSan, clang `-fno-exceptions`) that compile the hooked
code verbatim against stand-ins with 156's shapes, a mutant set each new
test catches, the earlier rounds' harnesses re-run on the changed code,
and the compile check against Chromium 156's headers.

**Findings.** 9: 1 medium, 2 low, 6 informational. Fixed: the medium and
both low.

**Fixed.**

- A credentialless iframe's storage key has a nonce and, in Chromium, its
  own origin's site as top-level site wherever it is embedded. MoltNet's
  per-site storage budgets now count such a key with the other keys of
  its nonce only (the credentialless frames of one page), not with that
  site: the storage quota share, and the `localStorage` and
  `sessionStorage` totals, cache groups and commit rates. A
  `localStorage` or `sessionStorage` area under an opaque top-level site
  counts on its own.
- MoltNet's in-memory HTTP cache counts a new entry's key against its
  site's share and the total like the data written into it: the site
  makes room from its own unused entries first, and an entry that still
  doesn't fit isn't made, so entries a site holds open can't take it past
  its share or the cache past its total.
- The `localStorage` and `sessionStorage` totals forget an area a page
  cleared safely when its site's other areas were forgotten first (their
  data deleted, for example by a `Clear-Site-Data` header): the site's
  entry, already gone, is no longer looked up.

## 2026-10-25: PRs #67-#69 (October 2026)

**Scope.** The code merged in pull requests #67 to #69 (rounds 22 to 24),
in this order of priority:

1. MoltNet's routing and isolation: the per-site socket pools, the
   connection and SOCKS handshake budgets shared through Tor's capacity,
   certificate fetches that carry their site, Background Fetch and download
   isolation (retries included), the end of an identity (the navigation
   throttle, the renderer shutdown, the refuse-all proxy configuration and
   its confirmation) and New Identity, keepalive and blob limits per site,
   one BrowsingInstance per renderer process, and the Incognito policies
   MoltNet now follows.
2. Memory safety and thread affinity in the new browser and network code:
   budgets and counts, raw pointers to pools, gates and the shared Tor
   capacity, the certificate-fetch thread-local, blob budget keys, the
   per-thread font cache invalidation, and checks reachable from renderer
   input.
3. Agent mode: fallback locators (the probe, the pin, weak matches and the
   question to the user), site-tool steps in recorded workflows, the tools of
   frames of other sites (the per-pair permission, counting without
   listing, the per-call question, the secret check on arguments), and the
   new WebUI handlers.
4. On-device AI: audio input for the Prompt API (the browser's checks, the
   service's re-checks, the projector allow-list read from the GGUF header,
   the local llama.cpp patch), the packed Vulkan shaders and the pipeline
   cache data, and the Windows GPU warm-up before the sandbox locks down.
5. MoltNet's bundled fonts: the read-only handles sent to renderers, the
   manifest, fetch and pin scripts, the SFNT reader in `tools/fonts`, and
   what happens when the set is missing, damaged or late.
6. A check of the new uniform values against cheap consistency probes
   (`navigator.deviceMemory` against the heap and data limits, the audio
   clock against `performance.now()`, the bundled fonts against each OS's
   text rendering).

**Method.** Each candidate traced from its attacker (web page, a group of
sites, a compromised WebUI script, an imported workflow, model or font
file, local environment) through MoltBrowser's code, the vendored llama.cpp
and the upstream Chromium 156 code at the pinned revision; fixes tested
with standalone harnesses (system gtest, `-fno-exceptions`) and a mutant
each new test catches, the Python tool tests, a libFuzzer run of every new
parser's target, and the compile check against Chromium 156's headers.

**Findings.** 7: 1 low, 6 informational. Fixed: the low and 1 of the
informational.

**Fixed.**

- Every MoltBrowser page that takes a click as the user's approval
  consumes the page's user activation (`molt_ai/common/molt_user_click.h`):
  the AI chat's agent "Allow", running a confirmed chat proposal, saving
  an agent run, the Agent studio's and the automation page's trust and
  "Confirm sites" buttons, the GPU setting and its "Try the GPU again" on
  `molt://ai-settings`, and the onion page's "Open in a MoltNet window",
  as the answers to a run's questions and to another site's frame's tools
  already did: one click allows at most one of these, whatever the page's
  script sends after it.
- The MoltNet popover's settings that decide where MoltNet traffic goes
  (exit country, on/off, mode) each need, and use up, one real click on the
  page, so a script there can't pin an exit country. Opening a MoltNet
  window and New Identity only add privacy and stay ungated. The page
  sends exactly one such message per click and none on load. The same
  settings in `molt://ai-settings` (MoltNet switch, exit country, bridges)
  and the AI chat handler's exit-country message follow the same rule.
- The font manifest's checks match whole values (a value with a final
  newline no longer passes).

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

**Fixed since.** The other seven (both medium, the other low and the four
informational), in a follow-up the same day:

- MoltNet: the connections of each MoltNet identity carry a SOCKS password
  of their own, random for each MoltNet network context, so Tor never puts
  a new identity on an earlier one's circuits, after New Identity or once
  the last MoltNet window has closed, whether or not Tor has applied New
  Identity's request for new circuits (which it still gets).
- GPU inference on Linux (off by default): the inference service's GPU
  broker grants the sysfs files libdrm reads about the GPU, each by name,
  instead of the GPU's sysfs directory recursively.
- Page translation: the script reaches every DOM member through its
  prototype, so a page's named form controls or named elements can't stand
  in for them, and a slot whose check fails no longer ends the write; the
  15-minute deadline also ends a job waiting for a write the page doesn't
  answer; and a translation asked for in a MoltNet window no longer adds
  its language to that window's language list.
- The agent planner shows the soft hyphen, the Hangul fillers, the
  variation selectors and the tag characters as U+FFFD too.

Two functional issues found on the way are fixed as well: a textarea's
placeholder is now written back, and text directly inside a
`display: contents` element is translated.

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
