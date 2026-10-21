# MoltBrowser Architecture

## Overview

MoltBrowser is an AI-native privacy browser built on a Chromium fork (v0.2.5; macOS ships current, Windows/Linux binaries frozen at v0.2.1 but the source is cross-platform). It integrates on-device **and** cloud LLM inference, ad/tracker blocking (MoltShield), privacy routing (MoltNet), one-click import, and a record/replay browser-automation studio (Agent mode) into the browser core.

```
MoltBrowser
 |
 +-- BrowserAIRuntime — on-device + cloud
 |    +-- MoltInferenceService: llama.cpp b11641 (Metal on macOS, CPU
 |    |   elsewhere) in a sandboxed utility process (kOnDeviceModelExecution),
 |    |   one per loaded model; the browser validates everything it sends
 |    +-- Model catalog + ModelDownloadJob (pinned revision, size, SHA-256)
 |    +-- CloudProvider (molt_ai/cloud) — OpenAI / Anthropic / Gemini +
 |    |    any OpenAI-compatible endpoint; keys in molt_ai/keys (keys.enc)
 |
 +-- WebUI Pages (user-facing molt:// scheme, rewritten to chrome:// internally
 |    to hide the chrome:// scheme from users)
 |    +-- molt://ai         (full-page AI chat)
 |    +-- molt://ai-chat    (side panel chat)
 |    +-- molt://ai-agent   (Agent mode studio — record/run/schedule)
 |    +-- molt://ai-settings (config + MoltNet UI)
 |    +-- molt://home       (local Liquid-Glass new tab)
 |    +-- molt://net, molt://import, molt://ai-memory, molt://ai-update
 |
 +-- Agent mode — record/replay automation studio (molt_ai/automation)
 |    +-- AutomationRecorder (captures actions as plain-English steps + labels)
 |    +-- AutomationRunner (replays; label-aware matching, per-step screenshots)
 |    +-- AutomationScheduler (cron/interval) + AutomationStorage (.molt files)
 |    +-- AutomationBackgroundBrowser (throwaway run window; Watch/Auto modes)
 |    +-- Side panel: agent_side_panel_web_view (swaps with AI chat)
 |
 +-- Import (molt_ai/import) — bookmarks + passwords from other browsers
 |    +-- Safe Storage keychain decrypt; entry in the bookmarks UI
 |
 +-- MoltShield (privacy engine, molt_ai/shield) — see "MoltShield" below
 |    +-- Brave's adblock-rust: EasyList, EasyPrivacy, uBlock Origin lists
 |    +-- Subresource/frame/WebSocket/beacon blocking, cosmetic + procedural
 |    |   filters, scriptlets, tracking-parameter stripping
 |    +-- Cookie consent auto-reject (known consent platforms only)
 |
 +-- Fingerprinting protection (Blink core/molt_fingerprinting + molt_ai/
 |    fingerprinting) — per-site, per-session farbling; MoltNet uniformity
 |
 +-- MoltNet (Tor windows, molt_ai/tor)
 |    +-- Dedicated off-the-record "MoltNet::Tor" profile per session; the
 |    |   regular profile is never proxied; window opens only after its
 |    |   network context proves it routes via the bundled tor's SOCKS port
 |    +-- Per-site circuits (SOCKS5 username = top-frame site)
 |    +-- SAFECOOKIE control auth + TAKEOWNERSHIP; New Identity discards the
 |    |   profile, flushes connections/DNS, NEWNYM
 |    +-- Bridges (obfs4 / snowflake); see docs/MOLTNET_COMPLIANCE.md
 |    +-- .onion only in MoltNet windows (OnionNavigationThrottle + the host
 |    |   resolver refuse it elsewhere); Onion-Location offered as an infobar
 |    +-- tor binary: bundled, or on Linux a root-owned /usr/bin|/usr/sbin/tor;
 |    |   pluggable transports likewise (lyrebird/obfs4proxy, snowflake-client)
 |    +-- OnionBlockingURLLoaderFactory: no .onion requests outside MoltNet,
 |    |   even through a user proxy
 |
 +-- WebAgent (molt_ai/agents) — chat-driven page actions, gated by
 |    molt_ai/security/agent_safety
 |    +-- DOMInterpreter (linear-time, thread pool, 2 MiB cap) /
 |        DOMContentBridge (isolated-world page reads)
 |
 +-- Document parsing — /pdf downloads and chat attachments
 |    +-- DocumentParser service (chrome/services/molt_ai/document_parser):
 |    |   PDF text layer, anydoc (Office/e-book/CSV -> Markdown), .docx text,
 |    |   PDF pages -> PNG for OCR (PDFium C API, no V8, no system fonts);
 |    |   in a kService-sandboxed utility process, one per document
 |    +-- DocumentParserClient (molt_ai/document_parser): launch, 30 s
 |    |   timeout (60 s to render pages) + kill, untrusted-reply validation
 |    |   (UTF-8; page limits and PNG structure, checked on a copy)
 |    +-- OcrService (molt_ai/ocr): tesseract on attached images and on the
 |        rendered pages of a scan (tesseract can't read PDFs), sandboxed
 |
 +-- MemoryService (molt_ai/memory) — opt-in, encrypted SQLite store +
 |    in-memory index, HashingEmbedder v3 (Unicode-aware keyword hashing)
 |
 +-- Security (molt_ai/security, molt_ai/common)
 |    +-- agent_safety (http(s)-only, origin scope, credential fields, fenced
 |    |   page text) + automation_security (workflow trust)
 |    +-- WebUINavigationThrottle: web pages can't open chrome:// or molt://
 |    +-- trusted_binary / helper_binary: which tor/OCR/whisper may run
 |    +-- helper_sandbox: tesseract/whisper confined (Landlock + seccomp,
 |        Seatbelt, job object) with a deadline
 |
 +-- OmniboxAI (@ai prefix, ? prefix)
 |    +-- AiPromptProvider -> chrome://molt-ai/?q=...
 |
 +-- UpdateManager (cross-platform, molt_ai/update)
      +-- GitHub Releases as transport; trust = offline Ed25519-signed
      |   release manifest (release_manifest.*, update_signing_keys.cc)
      +-- Size + SHA-256 checked after download and again at install;
          no downgrades; macOS also requires GenEye's Team ID code signature
          (docs/SIGNED_UPDATES.md)
```

## Directory Structure

```
repo/
 +-- src/                              # Git-tracked source (copied to chromium/src for build)
 |    +-- chrome/browser/molt_ai/      # Core AI modules
 |    |    +-- runtime/                # BrowserAIRuntime, model catalog, client
 |    |    |                           #   of the inference service
 |    |    |                           #   (InferenceProcessHost, StreamValidator)
 |    |    +-- models/                 # ModelDownloadJob (verified downloads)
 |    |    +-- agents/                 # WebAgent
 |    |    +-- automation/             # Agent mode (record/replay)
 |    |    +-- dom/                    # DOMInterpreter, DOMContentBridge
 |    |    +-- document_parser/        # client of the sandboxed parser service
 |    |    +-- ocr/                    # OcrService: tesseract on images and
 |    |    |                           #   rendered PDF pages
 |    |    +-- memory/                 # MemoryService, storage, index, embedder
 |    |    +-- security/               # agent_safety, WebUINavigationThrottle
 |    |    +-- common/                 # helper binaries: trust checks, sandbox
 |    |    +-- fingerprinting/         # session seed (mojo), mode, Shields-off sites
 |    |    +-- privacy/                # private-by-default prefs, feature overrides
 |    |    +-- shield/                 # MoltShield (ad/tracker blocker)
 |    |    +-- tor/                    # MoltNet, .onion, Onion-Location
 |    |    +-- update/                 # UpdateManager, signed release manifests
 |    |    +-- side_panel/             # AI chat side panel coordinator
 |    |    +-- android/                # Android scaffolding
 |    |    +-- ios/                    # iOS scaffolding
 |    +-- chrome/browser/ui/webui/molt_ai/  # WebUI pages
 |    |    +-- molt_ai_ui.*            # chrome://molt-ai/
 |    |    +-- molt_ai_chat_ui.*       # chrome://molt-ai-chat/
 |    |    +-- molt_ai_chat_handler.*  # WebUI message handler (bridge JS<>C++)
 |    |    +-- molt_ai_settings_ui.*   # chrome://molt-ai-settings/
 |    |    +-- molt_ai_agent_ui.*      # chrome://molt-ai-agent/
 |    +-- chrome/services/molt_ai/document_parser/  # utility-process parsers
 |    |    +-- pdf/, anydoc/           # PDF scraper, anydoc cxx bridge
 |    |    +-- pdf_page_renderer.*     # PDF pages -> PNG with PDFium (for OCR)
 |    +-- chrome/services/molt_ai/inference/  # sandboxed on-device inference
 |    |                                #   service (llama.cpp engine, mojom;
 |    |                                #   engine/: std-only budget, prompt and
 |    |                                #   output filters)
 |    +-- chrome/utility/services.cc  # registers both services
 |    +-- chrome/common/
 |    |    +-- webui_url_constants.h   # URL registrations
 |    +-- third_party/llama_cpp/       # LLM inference engine (GGUF), see README.chromium
 |    +-- third_party/blink/renderer/core/molt_fingerprinting/  # farbling engine
 |    +-- components/omnibox/          # AI omnibox provider
 |
 +-- chromium/                         # Full Chromium source (gitignored)
 +-- configs/                          # Platform build configs (.gn files)
 +-- scripts/                          # Build, package, release scripts
 +-- build/docker/                     # Docker build environments
 +-- installer/                        # NSIS (Windows), desktop files (Linux)
 +-- branding/                         # App icon, iconset
 +-- website/                          # Landing page, downloads.json
 +-- dist/                             # Build artifacts (gitignored)
```

## Key Technical Decisions

### On-Device Inference
- **llama.cpp** for LLM inference — supports 100+ model architectures
- **Metal** GPU acceleration on macOS (Apple Silicon)
- CPU inference on Linux/Windows today (Vulkan planned)
- Models stored as GGUF files in `~/.moltbrowser/models/`, downloaded with a
  pinned revision, size and SHA-256
- **Sandboxed.** llama.cpp runs only in a utility process,
  `molt_ai.mojom.MoltInferenceService` (`chrome/services/molt_ai/inference`),
  launched with Chromium's `kOnDeviceModelExecution` sandbox (Seatbelt with
  the Metal rules on macOS, the GPU-level seccomp policy and broker on Linux,
  LPAC on Windows; memory limits scale with RAM). GN visibility makes it the
  only code that can link llama.cpp. GGUF parsing, tokenization, the chat
  template, the context budget, special-token neutralization, the reasoning
  filters and stop sequences all run there. Design and threat model:
  `docs/design/SANDBOXED_INFERENCE.md`.
- One process per loaded model. The browser opens the GGUF and passes a
  read-only handle (the sandbox can't open files). Unloading (explicit, after
  10 idle minutes, or on critical memory pressure) ends the process, which
  frees the weights, the KV cache and any Metal buffers at once.
- The browser side (`BrowserAIRuntime` + `InferenceProcessHost`, UI thread)
  treats the process as hostile: `StreamValidator` checks UTF-8, sizes,
  counts, rates and message order and re-applies stop sequences; loads and
  generations have deadlines and a liveness timeout; a footprint watchdog
  and a crash-loop guard limit what a misbehaving model can do. A violation
  kills the process. `BrowserAIRuntime`'s blocking API is unchanged for its
  callers (chat, `WebAgent`, Agent-mode automation).
- Windows: the installer grants the LPAC capability SIDs on the install
  directory (as Chrome's setup does); the portable ZIP can't run on-device
  models. Android/iOS: not built (on-device AI reports itself unavailable).
- Vendored llama.cpp b11641 (`src/third_party/llama_cpp/README.chromium` lists
  the local patches). Its targets build with exceptions; the service code
  calling it doesn't, so no exception may escape a `llama_*` call (it would
  kill the inference process). On macOS the Metal kernels are compiled into
  `default.metallib` by `scripts/build-ggml-metallib.sh`; the sandboxed
  process loads it from the framework's `Resources/`.

### Document parsing (sandboxed)
- Untrusted documents (PDFs downloaded for `/pdf`, files attached in the AI
  chat) are parsed only in a utility process under the `kService` sandbox
  (no file system, no network): `molt_ai.mojom.DocumentParser` in
  `chrome/services/molt_ai/document_parser/` runs the PDF text-layer scraper
  (C++ and zlib), anydoc (Rust; a panic ends that process) and the .docx
  flat-text fallback, and returns only valid UTF-8 within its own size
  limits. It is registered in `chrome/utility/services.cc`; no
  browser-process target links the parsers.
- It also renders PDF pages for OCR (`RenderPdfPages`,
  `pdf_page_renderer.*`): the first 20 pages (`kMaxRenderPages`) at up to
  200 dpi, each scaled down to at most 10000 px a side and 25 MP, encoded as
  PNG (`gfx::PNGCodec`), 64 MiB of PNG per reply at most; all limits are
  mojom constants. PDFium is driven through its C API rather than `//pdf`
  (whose wrappers create a V8 isolate per call), so `kService` is enough on
  every platform: no V8 isolate or form-fill environment (no JavaScript, no
  JIT memory, which `kService` refuses on Windows and macOS), an empty font
  path list (no font directory scans and, on Windows under win32k
  lockdown, no GDI; unembedded fonts use PDFium's built-in ones), and AGG
  into BGRA bitmaps with `FPDF_NO_NATIVETEXT` (no CoreGraphics or GDI
  text). An encrypted, damaged or page-less PDF is a clean failure.
- The browser side, `molt_ai/document_parser/DocumentParserClient`, launches
  one process per call, so documents never share a parser. A crash or kill
  fails the call; so does a timeout (30 s; 60 s to render pages), which also
  terminates the process. Replies are untrusted: text that is not valid
  UTF-8 fails the call, and text is cut to the caller's budget. Rendered
  pages must keep every mojom limit, come in page order and be well-formed
  PNGs of their stated size (signature, chunk structure, IHDR); each is
  copied out of the reply before the check, since a large `BigBuffer` is
  shared memory. The browser never decodes them. The chat handler keeps
  base64 decoding, size limits and plain-text files on a worker thread.
- OCR of a scan (no text layer): tesseract can't read PDFs (leptonica has no
  PDF reader), so `molt_ai/ocr/OcrService::OcrPdf` has the parser render the
  pages, then OCRs each PNG in the helper sandbox below, joining the text
  with `--- Page N ---` headers, cut to the caller's budget and sanitized.
  One deadline (180 s) covers rendering and every page; a page gets at most
  60 s, and pages not reached are left out (the text says so). A PDF that
  crashed or hung the text parser is never passed on to rendering or OCR.

### Helper programs (sandboxed OCR and speech-to-text)
- tesseract (attached images, and the rendered pages of scanned PDFs) and
  whisper-cli (voice) read untrusted files, so they only run through
  `molt_ai/common/helper_sandbox.h` (`RunSandboxedHelper`), after
  `helper_binary.h` picked the binary. Per call: a private scratch directory
  holds the input and output and is the only writable place (also the
  working directory, HOME and TMPDIR); the helper's own directory, its
  tessdata or model and the input are readable; no network; nothing of the
  browser's environment. A deadline kills the helper and its process group
  (60 s for an image or a PDF page, all of a PDF within 120 s, 300 s for
  audio), output is read up to 256 KiB (tesseract) / 1 MiB (whisper), and
  files it leaves are read back only if they are plain single-link files
  (no symlink or hard link out of the sandbox). Callers get `timed out` / `sandbox unavailable`
  errors in the same result structs and JSON as before.
- Linux, between fork and exec (`base::LaunchOptions::pre_exec_delegate`,
  `helper_sandbox_linux.*`, prepared in the parent): a new session (no
  controlling terminal), no_new_privs, Landlock (read+execute /usr, /lib,
  /lib64 and the helper's directory; read /etc, /sys/devices/system/cpu,
  /proc/self and the policy's paths; read+write the scratch directory, which
  can't hold symlinks or run what is written there; ABI 4+: no TCP; ABI 6+:
  no abstract UNIX sockets, no signals out), then seccomp-bpf (x86_64,
  aarch64; allow by default): socket() other than AF_UNIX and connect()
  fail with EACCES; io_uring, leaving the process group (setsid/setpgid),
  ptrace, process_vm_*, pidfd_getfd, bpf, perf_event_open, userfaultfd, the
  keyring calls, namespaces and mounts are refused. Kernels without Landlock (before 5.13) keep the seccomp layer
  only (files unconfined); the browser logs the layers once. A layer the
  kernel has but that fails to apply stops the run.
- macOS: `/usr/bin/sandbox-exec` with a deny-by-default Seatbelt profile
  (`seatbelt_profile.*`; paths only as `-D` parameters): system libraries,
  the helper's directory and inputs readable, scratch read-write, no
  network, Mach lookups or IOKit. Fail-closed: without sandbox-exec, or if
  it can't apply the profile, the helper doesn't run.
- Windows: a job object (killed on close, UI restrictions) and the deadline
  only. No file or network confinement yet; that needs an AppContainer and
  ACLs on the bundle and scratch directories, as the inference sandbox will.
- whisper runs on the CPU (`-ng`): the sandboxes give it no GPU.

### WebUI Architecture
- All UI is inline HTML/CSS/JS in C++ raw strings (no separate resource files)
- Communication via `chrome.send()` → C++ `WebUIMessageHandler`
- C++ → JS via `FireWebUIListener()` / `ResolveJavascriptCallback()`
- Dark theme with gradient branding throughout

### MoltShield
- Engine: Brave's adblock-rust (MPL-2.0) behind a cxx bridge
  (`molt_ai/shield/adblock/`), domain resolution via Chromium's
  registry_controlled_domains. Lives on a dedicated sequence; rebuilds compile
  off-thread and are swapped in; serialized engine cached for fast startup.
- Lists: EasyList, EasyPrivacy, uBlock Origin filters/privacy/badware/
  quick-fixes (Fanboy lists opt-in), fetched through the profile network
  context (Tor under MoltNet), daily with jitter, ETag + sha256 tracked.
- Coverage: proxying URLLoaderFactory (all subresources, worker/service-worker
  loads), NavigationThrottle (ad frames collapsed; top-level documents never
  blocked), WebSocket interception, keepalive throttle.
- Cosmetic filtering + uBO scriptlets at document start via a renderer
  `CosmeticFilterAgent` (user stylesheet + main-world script), plus
  procedural filters (`:has-text()`, `:upward()`, ...) parsed and vetted in
  Rust (`adblock/procedural.rs`) and run by a JS engine in an isolated world
  (`chrome/renderer/molt_ai/shield/procedural_filters_js.h`, MPL-2.0, adapted
  from brave-core).
- Tracking-parameter stripping (`$removeparam` + a built-in list):
  navigations via `MoltShieldNavigationInterceptor` (an internal 307 on every
  GET leg), subresources rewritten in the proxy before they start and on
  server-redirect legs (same origin and path only, loop-guarded). Per-profile
  switch in molt://ai-settings (pref `molt_shield.remove_tracking_params`).
- Per-site Shields toggle (content setting), per-tab counts, breakage report in
  molt://ai-settings; consent-banner auto-reject (`molt_ai/consent/`).
- Rust side tested/benchmarked standalone in `tools/shield_bench/`.
- Kill switches (`chrome/common/molt_ai/shield/molt_shield_features.h`): each
  hook has its own `base::Feature`, enabled by default and checked where the
  hook is installed. Disabled means the hook is not installed at all (no proxy,
  throttle, tab observer or renderer agent), not that it allows everything.
  Changes in chrome://flags take effect after a relaunch.

  | chrome://flags | Feature (`--disable-features=`) | Turns off |
  |---|---|---|
  | `#molt-shield-request-proxy` | `MoltShieldRequestProxy` | the proxying URLLoaderFactory (subresources, workers) |
  | `#molt-shield-frame-blocking` | `MoltShieldFrameBlocking` | ad-iframe blocking in the NavigationThrottle |
  | `#molt-shield-websocket` | `MoltShieldWebSocket` | WebSocket interception (`WillInterceptWebSocket` stays false) |
  | `#molt-shield-keepalive` | `MoltShieldKeepAlive` | the keepalive (beacon) URLLoaderThrottle |
  | `#molt-shield-cosmetics` | `MoltShieldCosmetics` | cosmetic filters + scriptlets, including the renderer `CosmeticFilterAgent` |
  | `#molt-shield-consent-killer` | `MoltShieldConsentKiller` | the consent-banner auto-reject |
  | `#molt-shield-removeparam` | `MoltShieldRemoveParam` | tracking-parameter stripping (navigations and subresources) |

  `--disable-molt-shield` turns off all seven for one run (it is copied to
  renderers), and the engine and list updater never start. Example:
  `--disable-features=MoltShieldRequestProxy,MoltShieldWebSocket`.
  molt://ai-settings shows which hooks are off. The flags entries are one macro
  in `molt_ai/shield/molt_shield_about_flags.h` expanded in `about_flags.cc`.

### Agent mode (shipping — record/replay automation, v0.2.5)
- Lives in `src/chrome/browser/molt_ai/automation/`. `AutomationRecorder`
  captures user actions as editable plain-English steps (with element labels +
  record-time thumbnails); `AutomationRunner` replays them with **label-aware
  element matching** (finds a control by its on-screen text/aria-label, not a
  brittle CSS path) so recordings survive page redesigns.
- Per-step screenshots via `CopyFromSurface` + a Runs history; **Watch** (visible
  window) vs **Auto** (minimized background) run modes; click-that-navigates
  settle (waits for load after click/type). `AutomationScheduler` runs workflows
  on a cron/interval; `AutomationStorage` persists one `.molt` JSON per workflow
  under `~/.moltbrowser/automations/`.
- UI is the studio at `molt://ai-agent`, hosted in the side panel
  (`molt_ai/side_panel/agent_side_panel_web_view`), sharing one `kContent` panel
  with AI chat (opening one swaps the other).

### Fingerprinting protection
- Engine side in Blink: `third_party/blink/renderer/core/molt_fingerprinting/`
  (pure noise maths in `farbling.*`, glue in `molt_fingerprinting.*`), called
  from short `MoltBrowser:` hooks in the canvas, WebGL, Web Audio, navigator,
  screen, window and mouse-event code.
- Browser side: a random 32-byte seed per profile, kept in memory and sent
  to each renderer over a channel-associated mojo interface at launch, with a
  mode (`farble` for normal/incognito, `uniform` for MoltNet) and the profile's
  Shields-off sites, which are left unfarbled (`molt_ai/fingerprinting/`). The
  per-site key is HMAC-SHA256(seed, top-level site); service workers and
  first-party shared workers use their site's key.
- Canvas frames leaving as video (`captureStream`, `new VideoFrame(canvas)`)
  are only farbled in MoltNet windows: farbling every frame would slow video
  calls with canvas effects (`FarblesVideoExports()`).
- Kill switch: `#molt-fingerprinting-protection` (`MoltFingerprintingProtection`).
  Design, exact surface and known limits: `docs/FINGERPRINTING.md`.

### Memory
- Opt-in; encrypted SQLite store per profile, in-memory index, and
  `HashingEmbedder` v3 (Unicode-aware keyword hashing, not a neural
  embedding). Never records MoltNet browsing.

### Encrypted stores
- keys.enc (cloud API keys), vault.enc (passwords), profile.enc (form-filler
  profile), saved chat conversations and the memory database all encrypt
  through `molt_ai/keys/molt_os_crypt`: a thread-safe shim over Chromium's
  `os_crypt_async` Encryptor, fetched once by `MoltOSCryptStartupFactory`
  when the first profile is created. Until it arrives every store fails
  closed (nothing is ever written in plaintext); chat history and memory
  wait for it.
- The secret stores (keys, vault, profile) also require an OS-backed key:
  on Linux a keyring ("v11") or secret portal ("v12") key, never Chromium's
  hard-coded fallback ("v10"); macOS Keychain; Windows DPAPI. Chat history
  and memory accept any key.

### Cross-Platform
- **macOS**: Metal GPU, signed in-app updates, DMG packaging, notarization
- **Windows**: CPU inference today (Vulkan planned), signed in-app updates, NSIS installer
- **Linux**: CPU inference today (Vulkan planned), signed in-app updates via deb/rpm
- **Android**: Vulkan GPU, WebView-based AI chat, APK packaging
- **iOS**: CoreML + Metal, WKWebView, IPA packaging

## Build Configurations

| Platform | Config File | GPU Backend | Update Mechanism |
|----------|-------------|-------------|------------------|
| macOS arm64 | `configs/macos-arm64.gn` | Metal | In-app (signed manifest) |
| macOS x64 | `configs/macos-x64.gn` | Metal | In-app (signed manifest) |
| Linux x64 | `configs/linux-x64.gn` | CPU (Vulkan planned) | In-app (signed manifest) |
| Windows x64 | `configs/windows-x64.gn` | CPU (Vulkan planned) | In-app (signed manifest) |
| Android arm64 | `configs/android-arm64.gn` | Vulkan | Play Store |
| Android arm | `configs/android-arm.gn` | CPU only | Play Store |
| iOS arm64 | `configs/ios-arm64.gn` | CoreML+Metal | App Store |
| iOS sim | `configs/ios-sim-arm64.gn` | CPU only | N/A |

## WebUI Pages

User-facing URLs use the `molt://` scheme (rewritten to `chrome://` internally so
the `chrome://` scheme is never shown to users — a hard product rule).

| URL | Purpose | Handler |
|-----|---------|---------|
| `molt://ai` | Full-page AI chat (on-device + cloud) | MoltAIChatHandler |
| `molt://ai-chat` | Side panel AI chat | MoltAIChatHandler |
| `molt://ai-agent` | Agent mode studio — record / run / schedule | AgentStudioHandler (`molt_ai_agent_ui`) |
| `molt://ai-settings` | Configuration + MoltNet + cloud keys + import | MoltAISettingsHandler |
| `molt://home` | Local Liquid-Glass new tab page | (static WebUI) |
| `molt://net`, `molt://import` | MoltNet / Import glass popovers | MoltNet / MoltImport handlers |
| `molt://ai-memory`, `molt://ai-update` | Memory browser / update UI | respective handlers |

## Settings

Stored in `~/.moltbrowser/settings.json`:
```json
{
  "max_tokens": 512,
  "temperature": 0.7,
  "top_p": 0.9,
  "top_k": 40,
  "max_history_messages": 16,
  "default_model": "tinyllama-1.1b",
  "system_prompt": ""
}
```
