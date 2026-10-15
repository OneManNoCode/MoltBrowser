# MoltBrowser Architecture

## Overview

MoltBrowser is an AI-native privacy browser built on a Chromium fork (v0.2.5; macOS ships current, Windows/Linux binaries frozen at v0.2.1 but the source is cross-platform). It integrates on-device **and** cloud LLM inference, ad/tracker blocking (MoltShield), privacy routing (MoltNet), one-click import, and a record/replay browser-automation studio (Agent mode) into the browser core.

```
MoltBrowser
 |
 +-- BrowserAIRuntime — on-device (llama.cpp b11641, Metal on macOS, CPU
 |    |   elsewhere) + cloud
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
 |    +-- tor binary: bundled, or on Linux a root-owned /usr/bin|/usr/sbin/tor
 |
 +-- WebAgent (molt_ai/agents) — chat-driven page actions, gated by
 |    molt_ai/security/agent_safety
 |    +-- DOMInterpreter (linear-time, thread pool, 2 MiB cap) /
 |        DOMContentBridge (isolated-world page reads)
 |
 +-- MemoryService (molt_ai/memory) — opt-in, encrypted SQLite store +
 |    in-memory index, HashingEmbedder v3 (Unicode-aware keyword hashing)
 |
 +-- Security (molt_ai/security, molt_ai/common)
 |    +-- agent_safety (http(s)-only, origin scope, credential fields, fenced
 |    |   page text) + automation_security (workflow trust)
 |    +-- WebUINavigationThrottle: web pages can't open chrome:// or molt://
 |    +-- trusted_binary / helper_binary: which tor/OCR/whisper may run
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
 |    |    +-- runtime/                # BrowserAIRuntime, model catalog
 |    |    +-- models/                 # ModelDownloadJob (verified downloads)
 |    |    +-- agents/                 # WebAgent
 |    |    +-- automation/             # Agent mode (record/replay)
 |    |    +-- dom/                    # DOMInterpreter, DOMContentBridge
 |    |    +-- memory/                 # MemoryService, storage, index, embedder
 |    |    +-- security/               # agent_safety, WebUINavigationThrottle
 |    |    +-- common/                 # trusted helper-binary checks
 |    |    +-- fingerprinting/         # session seed + renderer switches
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
- Vendored llama.cpp b11641 (`src/third_party/llama_cpp/README.chromium` lists
  the local patches). Its targets build with exceptions; Chromium code calling
  it doesn't, so no exception may escape a `llama_*` call. On macOS the Metal
  kernels are compiled into `default.metallib` by
  `scripts/build-ggml-metallib.sh`.

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
  GET leg), subresources rewritten in the proxy before they start (same
  origin and path only).
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
- Browser side: a random 32-byte seed per profile, kept in memory, passed to
  renderers with a mode (`farble` for normal/incognito, `uniform` for MoltNet)
  as command-line switches (`molt_ai/fingerprinting/`). The per-site key is
  HMAC-SHA256(seed, top-level site).
- Kill switch: `#molt-fingerprinting-protection` (`MoltFingerprintingProtection`).
  Design, exact surface and known limits: `docs/FINGERPRINTING.md`.

### Memory
- Opt-in; encrypted SQLite store per profile, in-memory index, and
  `HashingEmbedder` v3 (Unicode-aware keyword hashing, not a neural
  embedding). Never records MoltNet browsing.

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
