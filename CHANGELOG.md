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

## [Unreleased]

(In-flight on `main` — will be folded into the next versioned build.)

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

[unreleased]: https://github.com/OneManNoCode/MoltBrowser/compare/bd52774...HEAD
