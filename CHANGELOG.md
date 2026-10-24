# MoltBrowser Changelog

All notable changes to MoltBrowser are tracked here. Each release ships
to `main` and the public daily build at
[github.com/OneManNoCode/MoltBrowser](https://github.com/OneManNoCode/MoltBrowser).

The project is GPL-3.0 licensed — contributions, issues, and forks are
all welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for the dev setup.

Format roughly follows [Keep a Changelog](https://keepachangelog.com/).
For longer narrative posts behind each entry see
[`website/updates/devblog/`](website/updates/devblog/).

---

## [Unreleased] — v0.3.0 "Private by default" (in release testing)

The result of a full security and privacy audit (October 2026). Everything
below is code complete and in release testing; it ships once it passes a
full build and on-device tests. Detail: [ROADMAP.md](ROADMAP.md).

### Security
- The automations page (`molt://ai-automation`) checks every message from
  its page before using it, and changes a workflow's trust only on a real
  click, never for an agent-run draft whose sites aren't confirmed yet.
- **Current Chromium.** Rebased from a March 2026 trunk build (148.0.7729.0,
  which had none of the M148 security fixes) onto Extended Stable
  152.0.7977.169, then onto **156.0.8078.48** when Chrome's Extended Stable
  channel moved from M152 to M156 (October 2026); from now on each Chrome
  security release ships within 7 days (`docs/REBASING.md`).
- MoltBrowser's encrypted stores (API keys, password vault, form-filler
  profile, chat history, AI memory) moved from the synchronous OSCrypt,
  which Chromium 152 removed, to `os_crypt_async`. Existing files open
  unchanged. On Linux the XDG secret portal key (Flatpak/Snap) counts as a
  keyring key; a keyring unlocked after startup needs a browser restart.
- **Signed updates.** The updater only installs an installer whose size and
  SHA-256 match a release manifest signed with GenEye's offline Ed25519 key.
  No downgrades; the hash is re-checked at install; on macOS the new app
  must carry GenEye's code signature. Runbook: `docs/SIGNED_UPDATES.md`.
- **Locked-down internal pages.** Every `molt://` page runs with no inline
  script and Trusted Types enforced. AI output can no longer inject markup
  into the chat panel. The chat page never holds saved passwords; revealing
  one happens in Settings behind Touch ID / the system password.
- **Agent safety.** The web agent, chat actions and Agent-mode replay only
  open http(s) pages, stay on the sites you approved, treat page text as
  data, and ask before submitting forms or touching password/card fields.
  Recorded workflows store vault references instead of what you typed, and
  per-workflow trust is shown and changed in the Agent studio.
- **Agent decisions in a fixed format.** The web agent and Agent mode's AI
  steps now get their answers from the on-device model as JSON it can only
  write in the expected shape, and the browser checks each answer again.
  The agent takes one action at a time on an element it listed from the
  page (never typing into a password field), and stops after a few
  unusable answers instead of guessing. "AI decision" reads SUCCESS or
  FAIL exactly ("unsuccessful" used to count as success). "AI extract"
  still saves a JSON object your later steps can read, but only one that
  passes checks on its size, depth and field names (typed fields when the
  workflow lists them), never the model's raw text; field lists in
  imported workflows are capped and checked. Page text can still
  influence what the model chooses, so the site, form and password rules
  above still apply to every action.
- **The AI chat no longer acts on text in its replies.** It used to run
  any `[[ACTION ...]]` line in a reply, so a page you asked it to
  summarize or translate could make it click, type or navigate. Replies
  are now only text (old saved chats show such lines as text). When you
  type a request like "click the login button", the on-device model picks
  one action from the elements on the page in the same checked format as
  the agent; MoltBrowser describes it and, in Ask first mode, runs it only
  after you allow it. In Auto mode it still asks before another site, a
  form submission or a payment field, and it never types into password
  fields. Choosing dropdown options, drag, hover, right-click and waits
  are no longer model actions (the `/` commands still have them), and
  page actions need an on-device model.
- **`/fill ai` and `/paste` show every value before filling.** They used
  to send your profile or clipboard to whichever model the chat used (a
  cloud model too), cut a field-to-value map out of its reply and write
  it into the page. Now the on-device model picks values for the page's
  text fields (never a password field, at most 8 at a time) in the same
  checked format as page actions; MoltBrowser lists each field and value
  and fills nothing until you allow it, in Auto mode too. `/fill ai` had
  never run at all: plain `/fill` answered it.
- **Safer secrets and cloud keys.** Vault autofill checks the site and
  requires https; keys and passwords aren't stored without a real system
  keychain; cloud endpoints must use https; the Gemini key is no longer put
  in URLs.
- **Web pages can't open internal pages.** A site could open `molt://`
  pages (settings, or the AI chat with a prompt already filled in) through
  `target=_blank`, `window.open` or a middle-click; only the browser UI and
  other internal pages can open them now.
- **Helper programs can't be swapped.** Tor, OCR and voice only run from the
  app's own folder or, on Linux, a root-owned system copy; never from
  Homebrew, `/usr/local` or PATH. The updater launches its install helpers
  by full path.
- **On-device AI runs in a sandbox.** Local models now load and run in their
  own sandboxed process (Chromium's on-device model sandbox) instead of
  inside the browser, so a malicious model file or crafted page text can't
  take over the browser through a bug in the model loader or tokenizer. The
  browser checks everything that process sends back and stops it if it
  misbehaves, hangs or uses far more memory than the model needs. On Windows
  the installer gives the sandbox read access to the install folder; the
  portable ZIP can't run local models (cloud models work).
- **Hostile pages and model files can't crash the browser.** The agent's
  page reader parses in linear time, off the UI thread, and reads at most
  2 MiB of HTML (a crafted page could hang it for minutes or crash it).
  llama.cpp's GGUF and tokenizer fixes are in (see On-device AI), and a
  malformed model's tokenizer returns an error instead of aborting.
- **Documents are read in a sandbox.** PDFs opened with `/pdf` and files
  attached in the AI chat (PDF, Word, Office, e-book, CSV) are parsed in a
  locked-down helper process, one per document, with no file or network
  access: a malformed or malicious file can crash only that process, and a
  parser stuck for 30 seconds is stopped (60 seconds when it renders a
  scanned PDF's pages for OCR).
- **OCR and voice helpers are sandboxed.** tesseract (scanned PDFs,
  attached images) and whisper (voice) now see only their own files, the
  input and a private scratch folder, get no network, and are stopped after
  1 minute per image or PDF page (3 minutes for a whole scanned PDF) and 5
  minutes for audio. A malicious file that takes one over can't reach your
  files or the internet, and one that makes it hang no longer ties up the
  browser. Linux uses Landlock and seccomp (kernels older than 5.13 get the
  network block only); macOS uses the system sandbox and won't run OCR or
  voice without it; Windows gets the time limit and a job object for now.
  Voice transcription runs on the CPU.
- **macOS: OCR ships with its own libraries.** The bundled tesseract still
  pointed at the build machine's Homebrew libraries, so on other Macs it
  couldn't start (and the sandbox wouldn't let it load them anyway). The
  app now carries every library tesseract and whisper need inside its own
  folder, and the build fails rather than ship a helper that loads code
  from outside the app.
- The AI library no longer forks the browser and starts a debugger found on
  PATH (with permission to read the browser's memory) when it hits an
  internal error.
- On Linux without a system keyring, secrets are never written under
  Chromium's built-in fallback key; MoltBrowser asks you to install or
  unlock GNOME Keyring or KWallet. Existing data stays readable and moves to
  the keyring on the next save. A form-filler profile that can't be
  decrypted (keyring locked, another machine's key) is never overwritten;
  you can start over and the old file is kept as a backup.
- Malformed messages to internal pages no longer crash the browser; AI
  automation steps no longer freeze it.
- Hardening of the built-in AI APIs' consent prompt, error answers and
  limits, the web agent's screenshot masks and page scripts, and the build
  provenance verifier, after an adversarial review
  (`docs/SECURITY_REVIEWS.md`).
- MoltNet hardening after an adversarial review of its isolation
  (`docs/SECURITY_REVIEWS.md`): New Identity finishes the old identity and
  waits for Tor to confirm new circuits before opening its window, MoltNet
  pages always ask before opening an external application, sandboxed
  pages get their own Tor circuits, and MoltNet never takes the regular
  profile's zoom levels.
- Further hardening of the built-in AI APIs (sessions stop when a switch,
  a policy or the site's answer turns them off; timing kept per site; the
  consent prompt ignores rapid presses), the web agent's screenshots (none
  for pages with frames of another origin), MoltNet text metrics on Linux
  desktops without anti-aliasing, and the release tools' checks
  (`docs/SECURITY_REVIEWS.md`).
- More MoltNet hardening from the same review: each certificate fetch
  (missing intermediate certificates, revocation checks) gets its own
  one-off network key, so it is never cached or shared with another site's
  and has its own Tor circuit; MoltNet windows don't check for captive
  portals or open a network sign-in page; and they offer sites Basic and
  Digest authentication only, never NTLM or Negotiate (Kerberos).
- Hardening of on-device AI after a review of October's changes
  (`docs/SECURITY_REVIEWS.md`): the browser accepts a GPU failure report
  only from a model that runs on the GPU; from a model on the CPU it counts
  like a crash of that model.
- More hardening from the same review: each MoltNet identity uses Tor
  circuits of its own (a random SOCKS password per identity), also for a
  site visited again after New Identity or after the last MoltNet window
  closed; GPU inference on Linux (off by default) can read only the few
  files about the GPU that its driver library needs; page translation
  can't be thrown off by a page's named form controls, its time limit also
  ends a translation stuck on an unresponsive page, it now translates
  textarea placeholders and text directly in `display: contents`
  elements, and asking for one in a MoltNet window leaves that window's
  languages alone; the agent's plan shows more invisible characters
  (soft hyphen, Hangul fillers, variation selectors, tag characters).
- More MoltNet hardening: within one identity, MoltNet's network state
  stays with the site that caused it. Each connection's certificate is
  verified on its own instead of from a cache shared by every site, no
  HTTP cache entry is shared between sites, HSTS applies to navigations
  only, sign-in credentials are cached per site, and Private State Tokens
  are off. MoltNet doesn't open while a command-line flag or field trial
  turns off the browser-wide partitioning of connections, the HTTP cache
  or third-party storage, and says which (`docs/MOLTNET_COMPLIANCE.md`).
- MoltNet hardening, re-checked at Chromium 156: a site that asks a
  MoltNet window for a TLS client certificate gets none, at once and the
  same way on every site, without the certificate picker or an
  administrator's auto-select policy, and nothing is remembered. Sites
  that require one don't open in MoltNet windows; use a regular window.
- MoltNet hardening: MoltNet's network connection runs no secure-DNS
  (DNS-over-HTTPS) probes; Tor resolves every name a MoltNet window
  looks up.
- MoltNet hardening: MoltNet doesn't open while the `ProxyOverrideRules`
  policy is set, since its rules would apply on top of MoltNet's Tor
  proxy, and the MoltNet panel shows the proxy as overridden.
- MoltNet hardening: MoltNet windows don't use device-bound sessions
  (Chromium 156, Windows and macOS), which tie a site's sign-in to a key
  in your computer's secure hardware; sites use their ordinary cookies
  there, as in Tor Browser.
- MoltNet hardening: the browser tells MoltNet's page processes the same
  fixed connection type and network quality their pages report, instead
  of your own connection's, and only whether you are online after that.
- MoltNet hardening: MoltNet's settings checks also hold while it is
  open. If a policy sets a proxy or proxy override rules, overrides one of
  MoltNet's leak controls or turns off blob URL partitioning while MoltNet
  windows are open, they close, their identity is discarded as with New
  Identity, and a dialog says why; MoltNet's connection keeps using only
  its own Tor proxy until then. MoltNet also doesn't open while a policy
  turns blob URL partitioning off.
- MoltNet hardening: MoltNet windows send nothing to Google's Autofill
  server (no form queries, no votes) and download no address formats;
  fields are still recognised on your device, and saved addresses and
  passwords still fill.

### Privacy
- **MoltNet in its own window.** Tor browsing opens in a separate,
  throwaway profile that never shares cookies or storage with your regular
  browsing and never falls back to a direct connection. Per-site Tor
  circuits, a real New Identity, HTTPS-Only, bridge (obfs4/snowflake)
  support, and no more third-party IP lookup.
- **Private-by-default settings.** Third-party cookies blocked, HTTPS-First,
  WebRTC limited to the public interface, network prediction off,
  connections partitioned per site, and requests carry the stock Chromium
  brand so MoltBrowser users don't stand out.
- **AI memory is opt-in** (off by default, existing users included), with a
  pause switch and retention setting, encrypted at rest, and never recording
  MoltNet browsing. Chat history is per profile, encrypted, and never saved
  from incognito.
- **`@ai` prompts stay on your device.** What you type after `@ai` in the
  address bar used to be sent to your search engine for suggestions as you
  typed, passed to extensions that watch the address bar, and kept in the
  address bar's prediction and shortcut data. Now it reaches only the local AI prompt row, in every
  window, also when typed in full-width (`＠ａｉ`) with an East Asian
  input method. A leading `?` means "search the web" (Chromium's forced-search
  shortcut) and goes to your search engine; pasted `?...` text no longer
  offers "Ask MoltBrowser AI" for text the search engine already has.
- Cloud AI replies are labeled with the provider, and each provider asks for
  consent once, listing what context is sent.
- **Fingerprinting protection in the engine.** In normal and incognito
  windows, canvas, WebGL and Web Audio readback get tiny per-site,
  per-session noise, and `hardwareConcurrency` varies per site, so a
  fingerprint taken on one site doesn't match another or tomorrow's. MoltNet
  windows also look the same for everyone: UTC, en-US, letterboxed screen
  size, 4 cores, no WebGL renderer string. Limits are listed in
  `docs/FINGERPRINTING.md`. Kill switch:
  `chrome://flags/#molt-fingerprinting-protection`. It also covers WebGL2
  pack-buffer, float and half-float readback, canvas uploads to WebGPU and
  `AudioWorklet` input; MoltNet windows also farble canvas video exports
  (`captureStream`, `VideoFrame`). Turning Shields off for a site turns the
  noise off there too, and the session seed is no longer visible on renderer
  command lines. Chromium 152's HTML-in-canvas origin trial, which would let
  a site read rendered page content back through WebGPU without the noise,
  stays off.
- Third-party shared workers (a script's worker started from its iframes)
  now get the fingerprinting noise of the site they run under, like the
  iframes themselves, instead of a new random one each time the worker
  starts: their canvas, WebGL and audio values match the site's pages and
  still don't link across sites.
- **Sites can't list your fonts.** In MoltNet windows pages only see the
  fonts your operating system ships with, the same for every MoltNet user;
  in normal and incognito windows they see those plus a small random share
  of your other fonts (1 in 16, different for every site and session).
  This covers CSS, canvas text, `document.fonts.check()`, `local()` fonts
  and `queryLocalFonts()`. Generic fonts (serif, sans-serif, ...) and web
  fonts are unaffected; Shields off shows all your fonts to that site.
- **MoltNet measures text alike on every machine of an OS.** On Linux,
  MoltNet windows lay text out without hinting, as on high-density
  screens, so text sizes no longer depend on your desktop's font settings
  or screen scale, and emoji come from Noto Color Emoji when it is
  installed. On every OS, canvas `measureText()` values are rounded to
  whole pixels (coarser for very large text), so sub-pixel differences
  between machines don't show; drawing is unchanged. Text can look a
  little softer on standard-density Linux screens. Element sizes aren't
  rounded, and font versions still differ between OS releases
  (`docs/FINGERPRINTING.md`, "Text metrics").
- **Global Privacy Control** is on: every request says `Sec-GPC: 1` and
  `navigator.globalPrivacyControl` is true
  (`chrome://flags/#enable-global-privacy-control` turns it off).
- **`.onion` sites.** They open only in MoltNet windows, where `http://`
  onion sites load under HTTPS-Only. Regular windows never look up, load or
  search-suggest an onion address; they offer to open it in MoltNet. A
  site's Onion-Location is offered as a bar, never followed automatically.
- **Linux MoltNet uses your distribution's `tor`** (packages now recommend
  it), with a clear "install the tor package" message when it's missing.
  Bridges work with the distribution's `obfs4proxy`/`lyrebird` and
  `snowflake-client`, and Settings says which package a bridge needs.
- In regular windows that use a proxy server, images, scripts, downloads and
  WebSockets to `.onion` addresses are blocked before they reach the proxy.
  Sites can announce their onion version with `<meta http-equiv=
  "onion-location">` as well as the header.
- **Less background traffic to Google.** Autofill no longer sends form
  signatures, Translate no longer downloads its ranking model, error pages
  no longer probe Google DNS or gstatic, and the password leak check (which
  can't work without a Google API key) is off by default. Chromium's
  built-in experiment config can no longer switch these, or features
  MoltBrowser turned off, back on. Everything the browser still contacts on
  its own is listed in `docs/NETWORK_TRAFFIC.md`, and the privacy policy
  now names each host.
- **Translate runs on your device.** "Translate this page" now uses your
  on-device AI model, like the web's Translator API: the page's text never
  leaves the device, Google's translate script and language list are never
  requested, and Translate offers the same 20 languages for everyone. It
  translates only when you click (or for a language you set to "always
  translate"), visible text first, with progress and Stop in the bubble,
  and "Show original" restores the page exactly. Without a model it says
  so and links to `molt://ai`; nothing is downloaded. Page translation is
  off in MoltNet windows, as in Tor Browser: a translated page can see the
  language you read, which MoltNet hides. There, Translate offers nothing
  and, opened from the menu, says why. (Before, Translate couldn't run
  without a Google API key, and with one it sent the page to Google.)
  `docs/design/PAGE_TRANSLATION.md`.
- **MoltNet windows describe the same GPU for everyone.** WebGL there
  reports fixed limits (8192-pixel textures, for example), a fixed list of
  23 common extensions and fixed shader precision, never more than the GPU
  really has; WebGPU finds no adapter, so sites fall back to WebGL. Normal
  and incognito windows are unchanged.
- **MoltNet windows letterbox the page.** The page area is rounded down to
  200 x 100-pixel steps (at most 1400 x 900) and centred, with margins in
  the toolbar colour, so a page's size no longer gives away your window,
  screen or toolbars; it is exactly the screen size MoltNet reports, and so
  are the `device-width`/`device-height` media queries. Fullscreen stays
  letterboxed. MoltNet pages also report a pixel ratio of 1 and pick their
  images for it (canvases and images can look softer on high-density
  screens), and they open at 100% zoom instead of following your regular
  zoom settings.
- **More of MoltNet looks the same for everyone.** Touches report their
  position relative to the page, like mouse clicks, and no finger size or
  pressure; sites can't see your other monitors (`getScreenDetails()` is
  always denied, without a prompt, and moving the window between screens
  goes unnoticed); `screen.orientation` matches the uniform screen size;
  date, time and number fields use US English formats whatever your
  system's language and regional settings.
- **MoltNet windows don't show the browser's language.** Text the browser
  puts in pages that sites can read (form messages such as "Please fill
  out this field.", "Choose File", the default "Details" and "Submit"
  labels, date-field placeholders) is in US English in MoltNet windows,
  and so is the language assumed for text that doesn't declare one (which
  decided the look of Chinese and Japanese characters). The default fonts,
  minimum font size and text encoding there are fixed for your operating
  system instead of following MoltBrowser's language. Regular windows and
  the browser's own interface keep your language.
- **Your settings stay out of MoltNet windows.** Sites in a MoltNet window
  no longer see how you set up your regular browser: not your site
  permissions or blocked sites (including Shields off and sites that may
  no longer ask for a permission), your content defaults (such as blocked
  JavaScript), font and font-size settings, default encoding, Do Not
  Track, third-party cookie choice, ad privacy choices or detectable
  accessibility settings. MoltNet uses MoltBrowser's defaults for them, the
  same for everyone and after every New Identity; what you change in a
  MoltNet window lasts for that session, and nothing done there changes
  your regular settings.
- **Fingerprinting noise costs less.** Canvas and WebGL readback,
  half-float WebGL reads and (in MoltNet windows) canvas video frames get
  their noise 2-4x faster: a 720p camera-like frame takes 5 ms of CPU
  instead of 15 ms on a 2.1 GHz test VM. The noise itself is exactly the
  same, so no site sees different values.
- **Your extensions and regular browsing stay out of MoltNet.** Extensions
  you installed no longer run in MoltNet windows or see their tabs,
  requests and events, even when allowed in Incognito (the built-in PDF
  viewer still works), and settings they control (proxy, privacy, fonts)
  don't apply there. MoltNet also stops showing sites what your regular
  browsing taught the browser: sites you watch a lot don't autoplay with
  sound, `navigator.storage.persist()` no longer reveals bookmarked or
  favourite sites, lookalike-domain warnings ignore the sites you use, and
  certificate warnings you clicked through in a regular window come back.
  Third-party cookies are always off in MoltNet (Shared Storage no longer
  exists in Chromium 156), and it never sends your Windows or Kerberos
  sign-in.
- **No WebRTC in MoltNet windows**, as in Tor Browser. A page could give
  WebRTC a relay server or a peer by name and make your own DNS resolver
  (often your ISP's) look that name up outside Tor, which could tie a
  MoltNet window to your network. Now `RTCPeerConnection` reports WebRTC
  as unavailable there, and the browser gives MoltNet pages no WebRTC name
  lookups, sockets or local-network (mDNS) announcements, even if a page's
  process ignores that. Video calls don't work in MoltNet windows; regular
  and incognito windows are unchanged.
- **MoltNet pages can't reach your machine or local network.** Requests
  to `localhost`, loopback and link-local addresses used to skip Tor, so a
  page could probe the services running on your computer or devices on
  your network. They now go to Tor, which refuses them, as in Tor Browser
  (use a regular window for a local development server).
- **MoltNet windows get Incognito's protections.** Several of Chromium's
  private-window protections only covered Incognito itself. Now, in
  MoltNet too: DevTools settings (including console history) stay in the
  MoltNet session instead of your regular profile; your lock screen and
  system media controls show "A site is playing media" instead of what a
  MoltNet page plays; files a page shares (macOS) or extension cookies are
  never written to your profile folder; links you open from other apps
  (macOS) go to a regular window, never into MoltNet; automatic
  picture-in-picture needs a site you allowed; and Windows Network
  Diagnostics isn't offered on MoltNet error pages (it logs the address).
- **MoltNet's Incognito protections, reviewed again for Chromium 156.**
  Every private-window check in Chromium 156 was checked for MoltNet
  windows. MoltNet now also keeps no WebRTC logs (they would be saved in
  your profile folder and uploaded), picks the language it asks a site
  for from the same short list as Incognito, and doesn't take three
  settings new in 156 from your regular windows: "Never show again" on
  the XSLT warning, the Global Privacy Control setting and the AtMemory
  keyboard shortcut.
- **Nothing a MoltNet window does goes around Tor.** Some browser features
  sent what a MoltNet window gave them outside Tor, or to someone who knows
  who you are. Now the AI chat in a MoltNet window uses on-device models
  only (a cloud provider would get your API key with every page), its
  sandbox tab opens in MoltNet instead of Incognito, its actions and links
  stay in its own window, and it can't set up page watchers that would
  later fetch the page outside Tor. Safe Browsing (which sent download
  URLs and file hashes to Google from your own IP), Google image
  descriptions, location, Cast, phone passkeys and shopping lookups are
  off in MoltNet windows, and "Open all" bookmarks there no longer spill
  into a regular window. What is still outside Tor is listed in
  `docs/MOLTNET_COMPLIANCE.md` §5.7.
- **MoltNet hides your touchscreen, memory, CPU and exact system.** MoltNet
  pages now all see the same mouse-and-keyboard desktop: no touch points, a
  fine pointer that can hover and no touch-event detection (your
  touchscreen still works), 8 GB of memory and an unknown CPU class. Sites
  can no longer ask for your screen's pixel density, viewport, memory,
  network speed, exact OS version, CPU architecture or exact browser
  version through client hints: MoltNet only sends the basic browser,
  platform and preference hints, and `navigator.userAgentData` leaves those
  details blank. New MoltNet windows open at the largest letterbox size
  that fits your screen (1400 x 900 where there is room), so pages start
  without margins.
- **Your Shields-off sites stay in the browser.** Page processes used to
  get your whole list of sites with Shields off, to know where to switch
  the fingerprinting noise off. Now the browser decides for each page and
  each worker as it loads and tells only that page's process, before any
  of its scripts run, so a compromised page process can't read the list.
  Anything that isn't told is protected. Turning Shields off or on applies
  from the next page load (the Shields toggle reloads the page); a page
  that Back restores from memory keeps what it had. Sandboxed pages now
  follow their site's Shields setting too.
- **Zooming no longer shows in MoltNet.** A zoomed MoltNet page used to
  report a size that gave the zoom level away (1120 pixels wide at 125%
  instead of 1400), and with it an accessibility need. The letterbox now
  rounds the page's size as the page itself measures it, at every zoom level
  and screen scale, and the screen size and mouse and touch positions follow
  it, so a zoomed page looks like any other. Zooming in makes the page a
  step smaller, like a smaller window; zooming out past 1400 x 900 makes it
  smaller on screen rather than showing more of it. At 33% and 67% zoom,
  and on a few screen scales, a page can be one step smaller than the window
  allows.
- **Scrollbars and page sizes no longer show the zoom in MoltNet.** A
  page's scrollbar used to be as wide in screen pixels at every zoom (and
  15 or 17 pixels depending on the OS), so measuring it gave away the zoom
  level and the OS. In MoltNet windows scrollbars that take room now
  measure 16 page pixels on every OS (12 when a page asks for thin ones)
  and zoom with the page, so the measurement no longer changes with the
  zoom level (bar a pixel at a few levels; see `docs/FINGERPRINTING.md`).
  Page-size media queries, `vw`/`vh` units and `visualViewport` also
  report whole pixels, so `(width > 1400px)` no longer matches a
  1400-pixel page at 90% zoom. Regular and incognito windows are
  unchanged.
- **One connection speed for every MoltNet user.** `navigator.connection`
  in MoltNet pages and workers used to show the browser's estimate of your
  network's speed, which follows your own connection and changes as pages
  load. It now reads the same for everybody (4g, 200 ms, 1.6 Mbps, no data
  saver) and never changes; lazy loading, web fonts and media preload in
  MoltNet behave the same way for everyone, and MoltNet's request
  scheduling and traffic no longer depend on or feed that estimate.
  Regular and incognito windows are unchanged.
- **One display for every MoltNet user.** The `color-gamut`,
  `dynamic-range`, `video-dynamic-range`, `color` and `monochrome` media
  queries in MoltNet pages now describe the same standard (sRGB, 8-bit,
  standard dynamic range) screen on every machine, matching the colour
  depth MoltNet already reported; Chromium 156 changed how it classifies
  wide-gamut displays. Pages that choose HDR video or wide-gamut images by
  media query serve their standard versions there. Regular and incognito
  windows are unchanged.
- **One scrollbar style per platform in MoltNet.** MoltNet pages now get
  overlay scrollbars on macOS and classic ones on Windows and Linux,
  whatever your system's scroll bar setting says or whether a mouse is
  connected, and changing that setting no longer changes MoltNet pages'
  layout. On a Mac set to always show scroll bars, MoltNet windows show
  them while scrolling. Regular and incognito windows follow
  your setting as before.
- **One audio device for every MoltNet user.** Web Audio in MoltNet pages
  now describes the same output device on every machine: a 48 kHz stereo
  device with a 10 ms buffer and a 24 ms output latency (sample rate,
  latencies, output timestamps, playback statistics, channel count), and
  pages' audio runs at 48 kHz, converted for your device when it uses
  another rate. Surround audio plays as stereo in MoltNet windows. Regular
  and incognito windows are unchanged.
- **Web Audio keeps its own time in MoltNet.** MoltNet pages' Web Audio now
  renders on the same fixed schedule on every machine (480 frames every
  10 ms at 48 kHz), whatever your output device's buffer and timing, and a
  machine without an output device answers like any other (no error
  events, one listed output device). Your device plays the result through
  a short buffer, which adds about 13 to 32 ms of audio latency in MoltNet
  windows. Regular and incognito windows are unchanged.
- **No DRM in MoltNet.** MoltNet pages are told that no media key system
  exists, Clear Key included, whichever ones the build or the operating
  system offers, and a MoltNet window no longer asks which ones there are.
  Regular and incognito windows are unchanged.
- **The same media answers for every MoltNet user.** Media Capabilities in
  MoltNet pages answers from the codecs built into MoltBrowser (video is
  smooth and never power efficient; WebRTC queries are unsupported, as
  WebRTC is off there) instead of your graphics hardware and what you
  played before, and WebCodecs uses MoltBrowser's own software codecs, so
  `"prefer-hardware"` isn't supported in MoltNet windows. Regular and
  incognito windows are unchanged.

### MoltShield
- **Rebuilt on Brave's adblock-rust** with EasyList, EasyPrivacy and uBlock
  Origin lists (~151k rules, daily updates). Now covers scripts, images,
  XHR/fetch, WebSockets and beacons, not just pages and frames; adds
  cosmetic filtering, scriptlets, a per-site Shields toggle and blocked
  counts. ~6 µs per request (the old matcher took 5–17 ms).
- Typing a tracker company's site (e.g. hotjar.com) no longer lands on a
  blocked page.
- **Tracking parameters are stripped** (fbclid, gclid, msclkid, utm_* and
  uBlock Origin's `$removeparam` rules, honoring its exceptions) from links
  you open, before the request leaves the browser, and from page requests,
  including their redirects. Turn it off in molt://ai-settings ("Strip
  tracking parameters"); kill switch: `chrome://flags/#molt-shield-removeparam`.
- **Procedural cosmetic filters work** (`:has-text()`, `:upward()`,
  `:matches-css()`, `:xpath()`, `:remove()`, `:style()` and more). They
  run in an isolated world; list text is vetted and never executed.
- The cookie-banner killer only ever clicks "reject", and only inside known
  consent platforms; an opt-in uBlock "Cookie Notices" list covers the rest.
  Scriptlet arguments were audited and can't break out into page script.

### On-device AI
- Context windows sized to the model and your RAM (was a fixed 2k); the
  system prompt is never cut; multi-turn chat reuses the prompt cache.
- Reasoning models show only the answer; Stop cancels only its own chat;
  models unload when idle or under memory pressure; CPUs too old for
  on-device AI get a clear message instead of a crash.
- Model downloads are hash-verified and resume correctly (resuming used to
  corrupt files); every model shows its licence with a link, and Llama/Gemma
  terms must be accepted before download.
- Page context reaches the side-panel chat and always matches the tab's
  current page (it used to answer from the previous page after you
  navigated).
- **llama.cpp updated to b11641**: Gemma 4 support (Gemma 4 E4B joins the
  catalog once its download is pinned) and upstream fixes for GGUF and
  tokenizer parsing (integer overflows, size checks, out-of-bounds reads).
- **GPU inference on Linux and Windows, built but off by default.** With the
  build switch `molt_llama_vulkan`, on-device models run on the GPU through
  llama.cpp's Vulkan backend, as many layers as fit in its memory, still in
  the model's sandbox. The CPU takes over when there is no usable GPU
  (software renderers and virtual GPUs are skipped), when the GPU can't hold
  the model, or after a GPU crash or reset, which no longer counts against
  the model. Shaders are compiled at build time with Chromium's own glslang.
  It stays off until tested on real GPUs; on Windows it runs on the CPU for
  now (`docs/design/SANDBOXED_INFERENCE.md` §11).
- **GPU inference remembers drivers that fail, and compiles less.** In the
  builds with GPU inference, a graphics driver that fails three times in a
  month is no longer used for on-device AI until it changes (retried after
  30 days, after a MoltBrowser update, or from molt://ai-settings, which
  also gains "Use the GPU for on-device AI": Automatic or Off). The GPU's
  compiled pipelines are kept between loads, checked before reuse and
  deleted with a model's data (`docs/design/SANDBOXED_INFERENCE.md`
  §11.10–11.12).
- **On-device AI on older x86 processors.** Models no longer need AVX2:
  the best kernels the processor supports are picked when a model loads,
  AVX on Intel Sandy Bridge/Ivy Bridge and AMD Bulldozer-family chips,
  SSE4.2 on 2008-2010 Core chips and on Atom, Celeron and Pentium chips
  without AVX. AVX2 processors run exactly the code they ran before. Only
  processors without SSE4.2 (Core 2, Phenom and Athlon II, Atoms before
  2013) still get the "use a cloud model" message
  (`docs/design/SANDBOXED_INFERENCE.md` §12).
- Fixed a crash on the first on-device reply in official Linux builds: a
  Control Flow Integrity check tripped on llama.cpp's internal calls. Found
  and fixed in an emulated official build; to be confirmed on a real one.
- Memory search matches words regardless of case in Latin, Greek, Cyrillic
  and Armenian scripts; existing memories are re-indexed once.
- Gemma 4's reasoning is hidden while it streams, so you see only the answer.
- Reader commands (`/summarize`, `/tldr`, `/simplify`, `/eli5`, `/receipt`,
  `/chapters`, `/translate`) always read the page you're on, not the one you
  just left.
- **Web AI APIs on your own model.** Sites can use the Prompt API
  (`LanguageModel`), `Summarizer`, `Writer` and `Rewriter` on your
  on-device model, never Google's, after you allow them in a bar at the top
  of the tab; a site never starts a model download. They
  report only coarse limits, run one page request at a time beside your
  chat, and are off in MoltNet.
- **Sites using on-device AI.** Your Allow / Block answers for the web AI
  APIs are remembered per profile (Incognito keeps its own until it
  closes), and closing the bar three times stops a site asking for a week.
  Settings lists the sites you allowed, blocked or paused, with Remove
  (which also ends that site's sessions) and a switch for the APIs; Clear
  browsing data and a profile reset clear the answers. `LanguageDetector`,
  whose model needs Google, now reports "unavailable" for everyone.
- **Search memory by meaning.** molt://memory finds a page from what it was
  about, in your own words and language, with an optional on-device search
  model (Qwen3-Embedding 0.6B, Apache 2.0, about 640 MB, downloaded when you
  ask) running in the AI sandbox. Memory is re-indexed in the background,
  only while Memory is on; keyword matching stays the fallback. The model is
  offered once its download is pinned.
- **`@memory` in the address bar.** Type `@memory` and what you remember to
  see the best few pages from your AI memory (title and URL); Enter opens
  `molt://memory/?q=…` with that search. Results by matching words come at
  once and are re-ranked by meaning when the search model is already
  loaded (typing never loads it). The text you type after `@memory` goes
  to no search engine, extension or server, and is never preconnected,
  prefetched or kept in the address bar's shortcut and prediction data.
  Not in Incognito, Guest or MoltNet windows; nothing is searched while
  Memory is off or paused.
- **Structured answers for sites.** A page's
  `LanguageModel.prompt(..., {responseConstraint})` JSON Schema is now
  enforced: the on-device model can only write JSON that matches it, and the
  page gets the answer only once it parses. Common schemas are supported
  (types, properties, required, items, enums, small numeric and length
  bounds, anyOf); others, and regular expressions, fail with
  `NotSupportedError`. The schema is read only inside the AI sandbox.
- **Translator API on your own model.** A page's `Translator` now
  translates with your on-device model, never Google's TranslateKit, and a
  page can no longer make the browser download translation components. It
  serves a fixed set of 20 languages (the same for everyone), asks with the
  same bar and remembers the same answer as the other web AI APIs, returns
  only the translation, handles long text piece by piece, and is off in
  MoltNet. The `TranslatorAPIAllowed` policy still turns it off.
- **Images for the Prompt API.** With a model that sees images (Qwen3-VL
  2B, Apache 2.0, offered once its download is pinned), a site you allowed
  can ask your on-device model about images: `LanguageModel` with
  `expectedInputs` `"image"`, up to four per prompt. Images are decoded by
  Chromium's own decoders and scaled in the browser; the AI sandbox gets
  only raw pixels within fixed limits, never an image file. Whether your
  model sees images doesn't change what `availability()` tells a site.
  The model downloads together with its image component, each file
  checked against its pinned hash, and counts as downloaded only when
  both are complete.
- **The web agent can see the page.** With a model that sees images, the
  agent also gets a screenshot of the visible part of the tab it works in,
  at each step, so it can pick icon-only buttons and find its way on
  visual pages. Only http(s) pages on the sites the run may use, never
  `molt://` or `chrome://` pages; password, payment, one-time-code and
  autofilled fields and embedded frames are greyed out first, and each
  element the agent can act on is tagged with its id. The screenshot stays
  on your device, isn't saved, and doesn't change the rules: the agent
  still acts only on listed elements, and asks before forms, other sites
  and payment fields. Text-only models work as before; turn it off at
  `chrome://flags/#molt-agent-screenshots`.
- **The web agent can use a site's own tools.** On a page that offers
  tools for AI agents (WebMCP, in Chromium 152's origin trial), the agent
  sees them next to the page's buttons and fields and can call one
  instead of clicking through forms: only the page's own tools, on the
  sites the run may use, with arguments checked against the tool's
  description of them. A tool the site doesn't mark as read-only, a form
  tool, and one that takes a payment card or a one-time code ask you
  first, showing every argument; tools that take a password or a file are
  never offered. What a tool answers is treated as page text. Turn it off
  at `chrome://flags/#molt-agent-site-tools`.
- **Site tools in frames, and in the AI chat.** The agent also sees the
  tools of frames that belong to the page's own site (exactly its origin;
  never another site's frame, or a frame inside one), and a call runs only
  in the very page or frame that listed the tool; when it asks you, it
  shows the frame's address. In the AI chat, a typed request on such a
  page can propose one of the site's tools: MoltBrowser shows the tool,
  the frame and every argument, and runs it only after you click Allow,
  in Auto mode too and for tools the site calls read-only. The same switch
  turns both off.
- **The web agent plans first.** Before it acts, the on-device model writes
  a short plan for the task (at most 6 steps, each with how to tell it's
  done), shown above the agent's steps with the current one marked. The
  agent follows it step by step and has it rewritten when the model asks,
  when actions keep failing or when a step drags on; the third time a plan
  fails, the run stops with a clear message. A plan never lets the agent do
  more: it is treated as page-influenced text, and every action is still
  checked and asks you exactly as before. If the model can't write a usable
  plan, the agent works as it did without one. Turn it off at
  `chrome://flags/#molt-agent-planner`.
- **Teach once, run again: save an agent run as a workflow.** After the web
  agent finishes a task, "Save as automation" in the chat's agent bar keeps
  the steps that worked as a draft Agent-mode workflow. Anything the agent
  typed that looks like a password, code, key or card number isn't saved:
  the workflow asks you for it each time it runs (card and one-time-code
  fields you enter yourself). A draft can't run until you have reviewed it
  in Agent mode and confirmed the sites it may use; it then starts
  untrusted, so it won't submit forms until you trust it. Runs that used a
  site's own tool, and runs in MoltNet windows, can't be saved. Workflow
  runs also no longer save the values you type when you start them.

### Fixed
- **OCR of scanned PDFs works.** A PDF with no text layer (`/pdf`, or a PDF
  attached in the AI chat) was handed to tesseract as a PDF, which it can't
  read: it decodes images through leptonica, which writes PDFs but doesn't
  read them. So this fallback never produced text. Now the sandboxed
  document parser renders the first 20 pages with PDFium (200 dpi, at most
  25 megapixels a page; no JavaScript, no system fonts) and tesseract reads
  those images one page at a time. The browser checks every page image
  before tesseract sees it. The text comes back with `--- Page N ---`
  headers and says when pages were left out; one 3-minute limit covers the
  whole PDF.
- **The AI chat's commands work in the side panel.** The web agent, `/fill`,
  `/vault fill`, `/vault find`, `/triage`, `/ask-tabs`, `/trackers`,
  `/reputation`, `/hops` and `/js` looked for the chat's window in a way
  that only finds a chat opened as a tab, so in the side panel they
  answered "no owning browser" or found nothing. They now act on the page
  in the chat's own window (never another window, never the chat itself;
  MoltNet windows stay apart), and the agent no longer starts on the chat
  when it is opened as a tab.
- **Typing into multi-line text boxes works.** The agent's and the chat's
  page actions failed on a `<textarea>` (a message or address box) and
  only filled single-line fields.

### Engineering
- **MoltNet leak test kit** (`tools/leak_test/`, `docs/LEAK_TESTING.md`):
  one command runs a built browser's MoltNet window in a network namespace
  where only Tor's traffic may leave. It captures every packet and every
  name lookup. It drives 21 leak tests (WebRTC, DNS prefetch, prefetch and
  prerender, fonts and favicons, beacons and pings, CSP/NEL reports,
  WebSocket, WebTransport, workers, downloads, localhost and LAN, QUIC,
  mDNS, certificate fetches, the built-in AI APIs, geolocation, extensions,
  and onion addresses in regular windows), the identity and fail-closed
  steps, and the uniformity checks at several window sizes, zoom levels and
  scale factors. It writes a pass/fail report with evidence, plus JSON; a
  `diff` command compares two machines or two locales. It covers most of
  MOLTNET_COMPLIANCE.md §4. The kit's own tests run in CI.
- Chromium is pinned in one place (`chromium_version.txt`); copied upstream
  files are tracked as a patch series with rebase tooling (`docs/REBASING.md`).
- One ordered build-machine runbook from a fresh sync to a signed release
  draft (`docs/BUILD_MACHINE_CHECKLIST.md`): what each merged pull request
  still needs (compile hot spots, unit tests and their known failures,
  fuzzers, device checks, the leak kit, offline signing), tagged with the
  pull request it came from, and the owner's open decisions.
- CI: actions pinned to commits, least-privilege tokens, new WebUI checks
  (fuzzed markdown renderer, jsdom smoke test of every page) and linting;
  releases are drafts until signed and publish checksums + build provenance.
- Releases: every installer ships an SPDX 2.3 SBOM. It lists Chromium, each
  third-party component of the build, MoltBrowser's vendored llama.cpp and
  Rust crates, and every bundled helper file with its pin and SHA-256
  (`docs/SBOM.md`). New tooling builds apt and dnf/yum repositories that are
  signed offline with a separate archive key (`docs/LINUX_REPOSITORIES.md`).
  The `.deb` and tarball now install root-owned files, and the `.deb` uses
  xz, which Debian 11 can unpack.
- Releases: signed build provenance. Each platform build records what it
  was built from (commit, Chromium, patches, GN args, vendored crates,
  bundled models) and how (a builder label, never the hostname; toolchain
  versions; build times) as an in-toto / SLSA v1 statement covering its
  installers, SBOMs and checksums. The owner signs the statements offline
  with the release key, and publishing and CI refuse a release without
  them. Anyone can check one with `molt_provenance.py verify`
  (`docs/SIGNED_UPDATES.md`, "Build provenance").
- Releases: every release script and workflow uploads the installers
  under the names the in-app updater downloads
  (`MoltBrowser-macOS-arm64.dmg`, `MoltBrowser-Linux-x64.{deb,rpm,tar.gz}`),
  from one list CI checks against the updater; `release.sh` and
  `release-linux.sh` used versioned names it never finds. The macOS release
  workflow now builds like a local release: it bundles Tor, whisper and
  tesseract, adds the Metal library that on-device models need and signs
  the framework's GPU libraries. A helper that can't be bundled or signed
  stops the release instead of shipping without it. Linux builds in its
  own out dir, `out/MoltBrowser-linux`, so it no longer replaces the macOS
  build in a shared checkout.
- Full GPLv3 licence text, llama.cpp licence notice, Code of Conduct,
  issue/PR templates; unused code and Sparkle leftovers removed, including
  the never-wired `browser.ai.*` stub (whose permission check allowed every
  caller), the prompt router, personas, the old agent engine and the model
  manager.
- **Fuzzing** (`docs/FUZZING.md`): 34 libFuzzer targets cover MoltBrowser's
  own parsers and validators of untrusted input: page HTML and text, the
  built-in AI APIs' arguments, everything the sandboxed inference service
  sends back, model-file metadata, JSON Schemas (each grammar also checked
  in llama.cpp's own grammar engine), agent actions, imported workflows,
  PDFs and .docx files, canvas/audio noise and font names, Tor's control
  port and bridge lines, and signed release manifests. They check each
  parser's promises, not just "no crash", build as Chromium `fuzzer_test`
  targets and standalone with clang (`tools/fuzz/molt_fuzz.py`), and CI
  runs a fixed-seed smoke pass on every pull request. They found two bugs,
  both fixed: a control character inside an angle-bracket look-alike got
  it past the agents' untrusted-text sanitizer, and an imported workflow
  with an out-of-range timestamp hit undefined behaviour.
- **Compile check without a checkout** (`tools/compile_check/`): one
  command type-checks MoltBrowser's own C++ and the upstream files it
  changes (537 files, Blink included) against Chromium 152's real headers
  with Chromium's own clang and plugins, generating the mojom, protobuf,
  buildflag and other headers a build makes. Its first runs found compile
  errors in about 40 files that earlier checks against hand-written
  stand-ins had missed, all fixed; among them, WebGL2 `readPixels` into a
  buffer now farbles the pixels by reading them back, as 152 has no buffer
  mapping.
- Consistency hardening: the site tools' approvals, the agent's plan, the
  chat's proposals and form fills, and saved agent runs (their step labels
  and the secret checks that read them) share one list of the characters
  they show as U+FFFD (bidi controls, zero-width and invisible characters,
  line separators, variation selectors, tag characters), the planner's
  wider one, with one test table (`agents/hidden_chars.h`).

---

## 2026-07-12 — v0.2.5: Agent mode

MoltBrowser learns to *do* things, not just talk about them. The headline is
**Agent mode** — a built-in web-automation studio that lives in the side panel.

### Added — Agent mode

- **Record → review → run → schedule.** Click **Agent mode** and the right panel
  becomes a studio (it swaps with AI chat; your page stays on the left). Do a
  task on the page and every action is captured as a clear, editable step —
  "Click *Sale*", "Type … in the search field", "Scroll". Save it, run it on
  demand or with different values, or put it on a schedule.
- **See every run, step by step — with screenshots.** Each run keeps a per-step
  timeline with a screenshot of what the page looked like at each step, so it's
  obvious what happened and exactly which step something failed on.
- **Two run modes.** *Watch it run* opens a window so you can see each step
  happen live, or *run in background* keeps it minimized and out of your way.
- **Reliable replay.** Workflows find the right element by its on-screen label —
  not a brittle CSS path — so recordings survive page redesigns. Verified
  end-to-end on real multi-step shopping flows.
- **Recorded-target thumbnails.** Every recorded step shows a picture of exactly
  what you clicked, so a workflow is easy to review and edit later.
- Each saved workflow card shows the site's **favicon + hostname**, so it's
  obvious which website it's for.

### Changed

- **Opaque black-glass toolbar.** The toolbar is now a solid black-tinted glass
  finish with floating pill buttons (replacing the earlier translucent/vibrancy
  treatment). A single violet **AI mode** button (on-device AI) sits alongside
  **Agent mode**, and the currently-open one is lit by a subtle ring.
- **MoltNet VPN.** The privacy-routing control is relabelled *MoltNet VPN* and
  moved to a left-cluster glass popover that shows the exit-country flag.
- **Import** moved into the bookmarks UI (a two-tab Bookmarks | Import popover).
- **AI chat panel.** Copy/paste now works in the chat box; the attachment button
  extracts text from documents and images on-device and answers from them; chat
  opens on your last-used model; the close button moved inside the panel.

### Platforms

- **macOS (Apple Silicon)** ships v0.2.5, signed + notarized, bundling an
  on-device model. **Windows and Linux** binaries remain at v0.2.1 for now — the
  v0.2.5 source is already cross-platform and will get parity builds at the next
  cross-platform milestone.

---

## 2026-07-09 — v0.2.4: Liquid Glass, MoltNet Tor actually works, and a de-Googled toolbar

A design + fixes release on top of v0.2.3.

### Fixed

- **MoltNet (Tor) works again.** Every prior build shipped a `tor` binary that
  couldn't launch — the packager dropped its `libevent` dylib, so
  `tor` died at startup and privacy routing never started. Now bundled and
  verified end-to-end: choosing an exit country actually routes your traffic
  through a relay in that country (tested US / DE / NL).
- **Removed the Google "Ask" button.** Chromium's new AI-Mode omnibox
  entrypoint routed to Google's AI overview; it's disabled. MoltBrowser stays
  Google-free.

### Added / Changed

- **Liquid Glass UI.** The AI chat panel, full-page chat, and settings adopt a
  visionOS-style frosted-glass treatment — floating translucent panels with
  luminous edges over a soft ambient backdrop. Respects light/dark themes and
  reduced-motion.
- **Native glass toolbar** (macOS vibrancy) with floating pill buttons.
- **Exactly-centered address bar**, Safari-style, without hiding any toolbar
  button.
- **MoltNet toolbar control** now shows live connection status, an Enable
  toggle, and the exit-country list with flags + full names; a new **Import**
  toolbar button jumps straight to browser migration.
- The chat **model picker** leads with **"Connect a cloud model"** (OpenAI /
  Anthropic / Gemini, bring-your-own-key), above the local models.

---

## 2026-07-08 — v0.2.3: on-device models work again on macOS + BYO-key cloud models, a redesigned AI chat, and one-click browser import

The headline is a **fix**: on-device model loading works again on macOS.
v0.2.2 shipped without the compiled Metal shader library
(`default.metallib`), so llama.cpp fell back to JIT-compiling shaders under
the hardened runtime and every model failed to load. The release pipeline
now compiles and seals `default.metallib` into the app framework before
signing — and hard-fails if it can't — so this can't ship broken again.

On top of that fix, a large batch of work that had been landing on `main`.

### Added

- **Bring-your-own-key cloud models** — connect frontier models (OpenAI,
  Anthropic, Google Gemini, plus OpenRouter, xAI/Grok, DeepSeek, Groq,
  Mistral, Perplexity, and any OpenAI-compatible endpoint) with your own API
  key. Keys are validated, then encrypted on-device (OSCrypt —
  Keychain/DPAPI/libsecret) and never leave your machine except to the
  provider you chose. The chat model picker keeps local
  ("Local · Private 🔒") and cloud ("Cloud · via your key ☁️") models
  clearly separated so the privacy boundary stays visible; on-device models
  stay fully local and need no key.
- **One-click import from any browser** — bring bookmarks (with their folder
  structure preserved) and saved passwords over from Chrome, Edge, Brave,
  Opera, Vivaldi, Chromium, Safari, or Firefox in one click, read directly
  from the source profile — no manual HTML/CSV export.
- **Redesigned AI chat** — a Recents drawer with persistent conversations, a
  composer model picker, file attachments (PDF/DOCX/TXT/images with on-device
  text extraction), editable + resubmittable messages, live voice dictation,
  and three themes.
- **Toolbar exit-country control** — the MoltNet Tor exit-country selector
  moved to a native toolbar globe menu next to the address bar.

### Fixed

- **macOS on-device model loading** — the `default.metallib` regression above.
- Per-model GGUF chat templates — replies no longer leak `<|user|>`-style
  template markers on non-Zephyr models.
- Fresh-conversation context isolation — a new chat no longer bleeds prior
  browsing-memory or page content into unrelated prompts.

### Changed

- Settings de-Googled — removed upstream "You and Google"/Chrome-branded
  onboarding surfaces in favor of MoltBrowser-native settings.

---

## 2026-06-22 — v0.2.1: full feature parity across macOS, Linux, and Windows 🎉

All three desktop platforms were rebuilt and now ship the **same feature set**.
Windows reaches functional parity (Tor/MoltNet, OCR, and voice are no longer
stubbed) and gains a native installer, and a new MoltNet **Tor exit-country
selector** lands on all three. Assets on the
[v0.2.1 release](https://github.com/OneManNoCode/MoltBrowser/releases/tag/v0.2.1):
`MoltBrowser-macOS-arm64.dmg` (883 MB, signed + notarized + stapled),
`MoltBrowser-Linux-x64.{deb,rpm,tar.gz}` (141/194/195 MB), and
`MoltBrowser-Windows-x64.zip` (702 MB portable) +
`MoltBrowser-Windows-x64-Setup.exe` (672 MB NSIS installer).

### Added

- **MoltNet Tor exit-country selector (all platforms)** — pick an exit country
  from the AI side panel; the app writes `ExitNodes {cc}` + `StrictNodes 1` into
  the Tor `torrc` and reloads, routing traffic through an exit relay in that
  country. Onion-routed and privacy-preserving — handy for reaching
  geo-restricted content. Note this is Tor, not a classic VPN: it's slower, only
  countries that host exit relays are selectable, and some sites block Tor.
- **Windows functional parity** — Tor/MoltNet, OCR, and voice transcription now
  work on Windows (previously stubbed):
  - **Tor/MoltNet** — POSIX sockets ported to winsock
    (`WSAStartup`/`closesocket`); bundles the Tor Expert Bundle 15.0.15
    (`tor.exe` + geoip).
  - **OCR** — bundles Tesseract 5.4.0 (+ `eng`/`osd` traineddata).
  - **Voice transcription** — bundles whisper.cpp v1.9.1 + the `ggml-tiny.en`
    model.
  These three already worked on macOS and Linux; this release brings Windows up
  to the same level.
- **Native Windows installer** — `MoltBrowser-Windows-x64-Setup.exe` (NSIS),
  alongside the existing portable ZIP.

### Build / infrastructure

- **All three platforms rebuilt 2026-06-22** from a shared Chromium checkout.
- **Windows is built on a self-hosted GitHub Actions runner** (replacing the
  old Docker cross-compile path). depot_tools/gn/ninja run in native
  PowerShell; source + build live in a short root `C:\cr` to dodge Windows
  MAX_PATH; toolchain is VS 2022 Build Tools + the Windows SDK "Debugging
  Tools". Full play-by-play in `docs/BUILD_PROGRESS.md`.
- **~40 Windows-portability fixes** to molt_ai code that had been POSIX-only:
  `FilePath::value()` is `std::wstring` on Windows (→ `AsUTF8Unsafe()` /
  `FromUTF8Unsafe()`); `std::ofstream` → `base::WriteFile`/`AppendToFile`;
  POSIX sockets in tor/voice/ocr ported to winsock; llama.cpp exceptions via
  `/EHsc` on clang-cl; the fork's `base::ListValue`/`base::DictValue` names.
- **Packaging lessons** (full notes in `docs/BUILD_PROGRESS.md`): the Windows
  ZIP must use forward-slash entries (built with 7-Zip — `.NET ZipFile` writes
  backslashes and `Compress-Archive` is too slow); NSIS is fetched by extracting
  its 7-Zip SFX with `7zr` off a direct SourceForge mirror; on macOS the bundled
  tor/ocr/whisper binaries under `Contents/Resources/` are codesigned explicitly
  with the hardened runtime because `codesign --deep` skips `Resources/`.
- **Link fixes:** added the missing `ggml-backend-dl.cpp`; restored the
  `build_with_tflite_lib` model-service BUILD.gn blocks that had been wrongly
  removed.

### Notes

- Only the macOS DMG bundles an on-device model (TinyLlama) in the download;
  Linux and Windows download a model on first run.

---

## 2026-05-12 — AI-grouped history (`/history`)

Personal Vector Memory gets a face. `/history` in the side panel pulls
your recent reading from MemoryService and clusters it into topic
cards by title-keyword overlap. No LLM round-trip in the hot path; the
whole thing is sub-50ms in the WebUI JS.
([devblog](website/updates/devblog/2026-05-12-grouped-history.md))

### Added
- IPC `listMemoryDocs(limit)` returning `{docs:[{doc_id, url, title,
  visited_at_unix, word_count, host}]}` — a thin pass-through to
  `MemoryService::ListRecent`.
- `/history [limit]` slash command. Default 200, max 2000.
- Greedy Jaccard-overlap clusterer in the WebUI JS: tokenize titles,
  drop stop-words, join cluster if `|A ∩ B| / |A ∪ B| >= 0.20`,
  otherwise start new cluster. Label = top-2 most-frequent tokens.
- Cluster cards render as `<details>` elements; top-3 open by
  default, the rest collapsed.

---

## 2026-05-12 — Form filler agent (encrypted local profile, no cloud)

A form filler whose entire universe is one local file. `/profile` opens
an inline editor; `/fill` autofills the active page's form.
([devblog](website/updates/devblog/2026-05-12-form-filler.md))

### Added
- New module `chrome/browser/molt_ai/profile/` with `MoltProfileStore`
  reading/writing `~/.moltbrowser/profile.enc` (OSCrypt-encrypted JSON).
- Chat IPCs `getMoltProfile`, `saveMoltProfile`, `runFormFill`.
- Slash commands `/profile` (inline editor in the side panel,
  14 fields) and `/fill` (autofills the active tab from the saved
  profile).
- Heuristic field matcher (regex-based; substring search against
  `name | id | placeholder | autocomplete | aria-label | <label>`).
  Dispatches `input` and `change` events so SPA frameworks see the
  update. Never overwrites an existing user value.
- Each filled control gets `data-molt-filled="<key>"` so future
  visual-confirmation UI can paint a halo.

### Notes
- v1 is intentionally deterministic; no LLM round-trip in the fill
  path. v2 will route unmatched fields through the local LLM using
  the same selector-recovery prompt template the automation runner
  uses.
- We skip `type="password"`, `type="file"`, and disabled / readonly
  controls.
- Nothing syncs. By design.

---

## 2026-05-12 — PDF chat

The side-panel chat now reads PDFs. Same `__moltSetTabContext` contract
as every other tab, just routed through the accessibility tree.
([devblog](website/updates/devblog/2026-05-12-pdf-chat.md))

### Added
- When the active tab's MIME type is `application/pdf`, the side panel
  calls `WebContents::RequestAXTreeSnapshot` and flattens the resulting
  tree's `kName` attributes into the same 50 KB text payload used for
  regular pages. PDFium publishes glyph runs as AX leaf nodes, so the
  text fidelity is the same as Chromium's built-in find-in-PDF.
- Chat-side icon flips from "chain link" to "page glyph" with the
  label "Chatting with PDF: …" when the active context is a PDF.
- PDFs run entirely local — the file never leaves the machine.

### Changed
- `AiChatSidePanelWebView` grew one new dispatch arm and a
  `FlattenAxTreeText` helper. No behavioural change for non-PDF tabs.

---

## 2026-05-12 — Universal cookie killer, tab triage, page watchers, agent inbox

Four agent/UX features built on the side-panel + automation + memory
stacks. ([`bd52774`](https://github.com/OneManNoCode/MoltBrowser/commit/bd52774))

### Added
- **Universal cookie-modal killer** — Per-tab `WebContentsObserver`
  injects a 3-stage script that auto-clicks "reject all" on OneTrust,
  Cookiebot, TrustArc, Quantcast, Didomi, CookieYes, Osano, Sourcepoint,
  Usercentrics, Klaro, plus a text-based fallback that scans visible
  buttons for "reject all" / "decline all" / "only necessary".
- **Tab triage** — Chat slash command `/triage list | close-inactive |
  close-domain <host> | bookmark-inactive | pin-active`. Backed by two
  new WebUI IPCs (`listTabsInWindow`, `triageActOnTabs`) that
  enumerate and bulk-act on tabs in the side-panel's owning Browser.
- **Page watchers** — Chat slash command `/watch <url> <selector>
  [interval] [name]` builds an `INTERVAL`-triggered Script
  (`NAVIGATE → WAIT_FOR → EXTRACT → NOTIFY`) and saves it via
  `AutomationStorage`. The existing scheduler picks it up and fires an
  OS notification every N seconds with the current value.
- **Agent inbox** — Process-wide `AgentInboxRegistry` holds one row per
  in-flight background automation. A 3-second side-panel poller renders
  a live "Running agents" tray with a pulsing dot, script name, step
  counter, and status note. Hides when nothing's running.

---

## 2026-05-11 — Phase 3 follow-ups

Closed six Phase-2 gaps in one batch.
([`5d9702f`](https://github.com/OneManNoCode/MoltBrowser/commit/5d9702f))

### Added
- Page-content chunking at sentence boundaries with keyword-overlap
  ranking — beats blind head-truncation on long articles.
- Memory query pre-fetch before every LLM prompt, top-3 hits prepended
  to the system message so cross-page recall ("what was that REIT
  article I read last week") works.
- Hover, right-click, drag, wait, wait-for action types in the
  slash-command dispatcher.

---

## 2026-05-10 — Side panel grounded in the active tab

Phase 2: page-content grounding + LLM-emit-actions + new action verbs.
([`beafb03`](https://github.com/OneManNoCode/MoltBrowser/commit/beafb03),
 [`56538d5`](https://github.com/OneManNoCode/MoltBrowser/commit/56538d5),
 [`5db8470`](https://github.com/OneManNoCode/MoltBrowser/commit/5db8470))

### Added
- `TabStripModelObserver` in the side panel pushes the active tab's
  URL + title + 50 KB of innerText to the chat every time the user
  switches tabs.
- LLM can emit `[[ACTION verb:args]]` tokens; the side panel parses
  them and runs the same `runMoltAction` IPC the user gets via
  slash commands. The model now drives the page directly.
- New action verbs: `click`, `type`, `scroll`, `navigate`, `select`,
  `hover`, `right-click`, `drag`, `wait`, `wait-for`.

---

## 2026-05-08 — Personal Vector Memory

Encrypted on-device semantic index of every page you read, with
sub-millisecond cosine search.
([`1cfa58c`](https://github.com/OneManNoCode/MoltBrowser/commit/1cfa58c),
 [`3e2df81`](https://github.com/OneManNoCode/MoltBrowser/commit/3e2df81))

### Added
- `MemoryService` (profile-scoped `KeyedService`): hashing-trick
  embedder, contiguous flat index, SQLite storage encrypted at rest
  with `OSCrypt`. Sub-millisecond query latency on 10k pages.
- Privacy gate: excludes `chrome://`, `data:`, file-scheme, incognito,
  and a user-editable domain blocklist.
- UI at `molt://memory` for stats, recent docs, delete-by-domain, and
  clear-all.

---

## 2026-05-05 — Automation manager + audit observability

Day 6: run history, retry-from-failed-step, AI token accounting.
([`d3b3de6`](https://github.com/OneManNoCode/MoltBrowser/commit/d3b3de6))

### Added
- Per-script `RunRecord` ring buffer (last 50 runs) drives the manager
  UI's sparkline + activity timeline.
- "Retry from failed step" button picks up at the recorded
  `last_failed_step_index` instead of restarting the whole script.
- Cumulative AI token totals surface per-script "is this prompt
  getting too expensive" diagnostics.

---

## 2026-05-04 — Web automation engine v1

Days 3–5: schedule editor, manual create / import / export, trust UX.
([`11ef4be`](https://github.com/OneManNoCode/MoltBrowser/commit/11ef4be))

### Added
- Record / replay / schedule any web workflow.
- `~/.moltbrowser/automations/*.molt` JSON-on-disk script format.
- Cron and interval triggers with timezone support, missed-run
  catch-up on browser launch.
- Trust levels (`CASUAL` / `APPROVED` / `TRUSTED` / `ADMIN`) with
  per-action approval gates.

---

[unreleased]: https://github.com/OneManNoCode/MoltBrowser/compare/v0.2.5...HEAD
