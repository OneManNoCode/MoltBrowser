# MoltBrowser Architecture

## Overview

MoltBrowser is an AI-native privacy browser built on a Chromium fork. It integrates on-device LLM inference (via llama.cpp), ad/tracker blocking (MoltShield), privacy routing (MoltNet), and autonomous browser agents into the browser core.

```
MoltBrowser
 |
 +-- BrowserAIRuntime (llama.cpp, Metal/Vulkan GPU)
 |    +-- PromptRouter (persona-aware routing)
 |    +-- ModelManager (download, load, switch GGUF models)
 |    +-- ModelDownloader (HuggingFace Hub, SimpleURLLoader)
 |
 +-- WebUI Pages (chrome:// pages)
 |    +-- chrome://molt-ai/        (full-page AI chat)
 |    +-- chrome://molt-ai-chat/   (side panel chat)
 |    +-- chrome://molt-ai-settings/ (config + MoltNet UI)
 |    +-- chrome://molt-ai-agent/  (agent testing)
 |
 +-- MoltShield (privacy engine)
 |    +-- ABP filter parser (EasyList, uBlock, Fanboy)
 |    +-- Cookie consent auto-blocking
 |    +-- YouTube ad blocking (network + cosmetic + scriptlet)
 |    +-- Fingerprint protection (Canvas, WebGL, Audio)
 |    +-- URL tracking param stripping
 |    +-- Cookie policy enforcement
 |
 +-- MoltNet (privacy routing)
 |    +-- Tor integration (runtime, SOCKS5)
 |    +-- Circuit management (SIGNAL NEWNYM, exit country)
 |
 +-- AgentEngine (browser automation)
 |    +-- ReAct loop (GOAL -> PLAN -> ACT -> OBSERVE -> REASON)
 |    +-- ActionValidator (security sandbox)
 |    +-- DOMInterpreter (page structure extraction)
 |    +-- DOMContentBridge (live tab JS execution)
 |
 +-- MemoryEngine (SQLite + vector search)
 |    +-- Short-term session memory
 |    +-- Long-term persistent memory
 |    +-- Hash-based embeddings (MiniLM upgrade path)
 |
 +-- PersonaSystem (5 built-in + custom)
 |    +-- JSON persistence (~/.moltbrowser/personas.json)
 |
 +-- OmniboxAI (@ai prefix, ? prefix)
 |    +-- AiPromptProvider -> chrome://molt-ai/?q=...
 |
 +-- UpdateManager (cross-platform)
      +-- Sparkle (macOS)
      +-- WinSparkle (Windows)
      +-- LinuxUpdateIntegration (AppImage/Flatpak/deb/rpm)
```

## Directory Structure

```
repo/
 +-- src/                              # Git-tracked source (copied to chromium/src for build)
 |    +-- chrome/browser/molt_ai/      # Core AI modules
 |    |    +-- runtime/                # BrowserAIRuntime, PromptRouter
 |    |    +-- models/                 # ModelManager, ModelDownloader
 |    |    +-- agents/                 # AgentEngine, ActionValidator
 |    |    +-- dom/                    # DOMInterpreter, DOMContentBridge
 |    |    +-- memory/                 # MemoryEngine (SQLite)
 |    |    +-- personas/               # PersonaSystem
 |    |    +-- security/               # ActionValidator
 |    |    +-- shield/                 # MoltShield (ad/tracker blocker)
 |    |    +-- update/                 # UpdateManager, Sparkle, WinSparkle, Linux
 |    |    +-- side_panel/             # AI chat side panel coordinator
 |    |    +-- api/                    # browser.ai JavaScript API
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
 |    +-- third_party/llama_cpp/       # LLM inference engine (GGUF)
 |    +-- components/omnibox/          # AI omnibox provider
 |
 +-- chromium/                         # Full Chromium source (gitignored)
 +-- configs/                          # Platform build configs (.gn files)
 +-- scripts/                          # Build, package, release scripts
 +-- build/docker/                     # Docker build environments
 +-- installer/                        # NSIS (Windows), desktop files (Linux)
 +-- branding/                         # App icon, iconset
 +-- website/                          # Landing page, downloads.json
 +-- update/                           # Sparkle appcast, release notes
 +-- dist/                             # Build artifacts (gitignored)
```

## Key Technical Decisions

### On-Device Inference
- **llama.cpp** for LLM inference — supports 100+ model architectures
- **Metal** GPU acceleration on macOS (Apple Silicon)
- **Vulkan** GPU acceleration on Linux/Windows/Android
- **CoreML** for iOS Neural Engine
- Models stored as GGUF files in `~/.moltbrowser/models/`
- 6 pre-configured models (TinyLlama 1.1B to Gemma 2 9B)

### WebUI Architecture
- All UI is inline HTML/CSS/JS in C++ raw strings (no separate resource files)
- Communication via `chrome.send()` → C++ `WebUIMessageHandler`
- C++ → JS via `FireWebUIListener()` / `ResolveJavascriptCallback()`
- Dark theme with gradient branding throughout

### MoltShield
- Full ABP filter syntax parser (network + cosmetic + scriptlet rules)
- 132 built-in blocked domains (O(1) hash lookup)
- YouTube-specific blocking (network intercept + scriptlet injection)
- Cookie consent auto-blocking (80+ CSS rules + auto-decline JS)
- URL tracking parameter stripping (utm_*, gclid, fbclid, etc.)
- Fingerprint protection (Canvas noise, WebGL spoofing, AudioBuffer noise)
- URLLoaderThrottle intercepts all network requests before they proceed

### Agent System
- ReAct loop: up to 20 iterations, 2-minute timeout
- Actions: CLICK, SCROLL, NAVIGATE, FILL_FORM, TYPE_TEXT, EXTRACT_DATA, OPEN_TAB, CLOSE_TAB, WAIT
- All actions go through ActionValidator (domain whitelist, no credentials)
- DOMContentBridge uses `ExecuteJavaScriptInIsolatedWorld` for safe page interaction

### Memory
- SQLite database at `~/.moltbrowser/memory.db`
- In-memory cache for fast reads, synced with SQLite on writes
- Hash-based embeddings (384-dim) for basic semantic search
- Upgrade path to MiniLM (all-MiniLM-L6-v2) via ONNX Runtime

### Cross-Platform
- **macOS**: Metal GPU, Sparkle auto-update, DMG packaging, notarization
- **Windows**: Vulkan GPU, WinSparkle auto-update, NSIS installer
- **Linux**: Vulkan GPU, package manager detection, deb/rpm/AppImage/Flatpak
- **Android**: Vulkan GPU, WebView-based AI chat, APK packaging
- **iOS**: CoreML + Metal, WKWebView, IPA packaging

## Build Configurations

| Platform | Config File | GPU Backend | Update Mechanism |
|----------|-------------|-------------|------------------|
| macOS arm64 | `configs/macos-arm64.gn` | Metal | Sparkle |
| macOS x64 | `configs/macos-x64.gn` | Metal | Sparkle |
| Linux x64 | `configs/linux-x64.gn` | Vulkan | AppImage/Flatpak/apt |
| Windows x64 | `configs/windows-x64.gn` | Vulkan | WinSparkle |
| Android arm64 | `configs/android-arm64.gn` | Vulkan | Play Store |
| Android arm | `configs/android-arm.gn` | CPU only | Play Store |
| iOS arm64 | `configs/ios-arm64.gn` | CoreML+Metal | App Store |
| iOS sim | `configs/ios-sim-arm64.gn` | CPU only | N/A |

## WebUI Pages

| URL | Purpose | Handler |
|-----|---------|---------|
| `chrome://molt-ai/` | Full-page AI chat | MoltAIChatHandler |
| `chrome://molt-ai-chat/` | Side panel chat | MoltAIChatHandler |
| `chrome://molt-ai-settings/` | Configuration + MoltNet | MoltAISettingsHandler |
| `chrome://molt-ai-agent/` | Agent testing | (via chrome.send) |

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
