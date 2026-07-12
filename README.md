# MoltBrowser

**The AI-Native Browser for the Agent Era**

Built by [GenEye AI Labs Inc.](https://geneye.ai/moltbrowser)

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Changelog](https://img.shields.io/badge/changelog-keep--a--changelog-orange)](CHANGELOG.md)
[![Devblog](https://img.shields.io/badge/devblog-daily%20updates-green)](website/updates/devblog/)

---

## What's new

- **v0.2.5 — Agent mode.** A built-in web‑automation studio in the side panel: record a task once, review it as clear, editable plain‑English steps, then run it on demand, in the background, or on a schedule. Every run keeps a step‑by‑step history with a screenshot of each step, and each recorded step shows a thumbnail of exactly what you clicked.
- **v0.2.4** — visionOS‑style Liquid Glass UI, a local glass new‑tab page, an exactly‑centered address bar, and a fix that got MoltNet privacy routing launching again.
- **v0.2.3** — bring‑your‑own‑key cloud models (OpenAI / Anthropic / Gemini + any OpenAI‑compatible endpoint), one‑click import of bookmarks + passwords, and a redesigned AI chat.

Full history: [`CHANGELOG.md`](CHANGELOG.md).

Want to contribute? See [`CONTRIBUTING.md`](CONTRIBUTING.md). Issues and
PRs welcome — pick anything from the changelog "Unreleased" section or
propose your own.

---

## What is MoltBrowser?

MoltBrowser is an open-source, AI-native web browser built on a hardened open web engine (Blink + V8) with embedded local LLM inference, browser automation, and privacy-first architecture. Unlike traditional browsers where AI is bolted on as an extension, in MoltBrowser **AI is the core runtime**.

## Key Features

- **Agent mode** — Record a web task once, review it as editable plain-English steps, then run it on demand, in the background, or on a schedule. Per-step screenshots, a Runs history, and label-aware replay that survives page redesigns.
- **On-device + cloud AI** — Run local models (TinyLlama ships in the box; add LLaMA, Qwen, Mistral, Phi via llama.cpp) *or* bring your own key for OpenAI / Anthropic / Gemini and any OpenAI-compatible endpoint. Your choice per chat.
- **Grounded attachments** — Drop in a document or image and MoltBrowser extracts its text on-device and answers from it — nothing leaves your machine.
- **MoltShield Privacy** — Tracker blocking, fingerprint mitigation, cookie management, and cookie-consent auto-dismissal.
- **MoltNet VPN** — Onion-routed IP privacy with an exit-country selector: pick the country your traffic exits through, right from the toolbar.
- **Liquid Glass UI** — A visionOS-style frosted-glass new-tab page, AI panel, and settings; an opaque black-glass toolbar with floating pill buttons.
- **One-click import** — Bring your bookmarks and saved passwords over from another browser in a click.
- **Extension-compatible** — Supports standard web-extension APIs, plus `browser.ai.*` APIs for AI-powered extensions.
- **Desktop** — macOS (Apple Silicon) today; Windows and Linux builds track the same source.

## Architecture

```
+--------------------------------------------------+
|                Browser UI Layer                  |
|  Omnibox AI | Sidebar Agent | Page AI | Personas |
+--------------------------------------------------+
|           Browser Intelligence Layer             |
| Agent Engine | Task Planner | Memory Engine      |
| DOM Interpreter | Tab Context Manager            |
+--------------------------------------------------+
|               LLM Runtime Layer                  |
| llama.cpp runtime | model manager | prompt router |
+--------------------------------------------------+
|                Model Layer                       |
| LLaMA 3 | Qwen 2 | Mistral | Phi-3 | Gemma     |
+--------------------------------------------------+
|              Privacy & Security                  |
| MoltShield | MoltNet | AI Sandbox                |
+--------------------------------------------------+
|             Browser Core Engine                  |
| Blink | V8 | Network Stack | Open Web Engine     |
+--------------------------------------------------+
```

## Supported Models (GGUF)

| Model | Size | RAM Required |
|-------|------|-------------|
| TinyLlama 1.1B | ~0.6GB | 2GB |
| Phi-3.5 Mini 3.8B | ~2.2GB | 4GB |
| Mistral 7B | ~4.1GB | 6GB |
| LLaMA 3.1 8B | ~4.3GB | 6GB |
| Qwen2.5 7B | ~4.4GB | 6GB |
| Gemma 2 9B | ~5.5GB | 8GB |

## Hardware Requirements

| Tier | RAM | GPU | Experience |
|------|-----|-----|-----------|
| Basic | 4GB | None | Cloud models only |
| Standard | 8GB | None | Phi-3 Mini |
| Recommended | 16GB | 6GB+ VRAM | Full model stack |
| Power User | 32GB+ | 12GB+ VRAM | All models, multi-model |

## Building from Source

### Prerequisites
- Python 3.8+
- Git
- Ninja build system
- CMake 3.16+
- 100GB+ free disk space
- 16GB+ RAM

### Build Steps

```bash
# Clone the repository
git clone https://github.com/OneManNoCode/MoltBrowser.git
cd MoltBrowser

# Fetch Chromium source and dependencies
./scripts/setup.sh

# Configure build
./scripts/configure.sh

# Build MoltBrowser
./scripts/build.sh
```

### macOS (Apple Silicon)
```bash
# GPU acceleration uses Metal API automatically
./scripts/build.sh --platform=mac --arch=arm64
```

## Project Structure

```
moltbrowser/
  chromium/src/              # Chromium source tree
    chrome/browser/molt_ai/  # Core AI runtime modules
      agents/                # Agent Engine, Task Planner
      dom/                   # DOM Interpreter
      memory/                # Memory Engine, vector store
      models/                # Model Manager, downloader
      personas/              # Persona system
      security/              # AI Sandbox, action validator
      ui/                    # AI sidebar, model manager UI
    third_party/llama_cpp/   # Vendored llama.cpp
  docs/                      # Documentation
  scripts/                   # Build and setup scripts
  sdk/                       # Extension SDK
  build-system/              # Build configurations
```

## browser.ai.* Extension API

```javascript
// Run a prompt against local LLM
const response = await browser.ai.run("Explain this code", {
  model: "llama3-8b",
  maxTokens: 500
});

// Get page summary
const summary = await browser.ai.pageSummary();

// Run an autonomous agent task
const result = await browser.ai.agentTask("Find the best price for iPhone 16");

// Extract structured data from page
const data = await browser.ai.extractData("table.pricing");

// List available models
const models = await browser.ai.getModels();
```

## Download

The macOS build is current at **v0.2.5**; the Windows and Linux desktop builds are at **v0.2.1** and get rebuilt per-platform at each milestone (their source is already up to date).

| Platform | Version | Download |
|----------|---------|----------|
| **macOS** (Apple Silicon) | **v0.2.5** | [MoltBrowser-macOS-arm64.dmg](https://github.com/OneManNoCode/MoltBrowser/releases/latest/download/MoltBrowser-macOS-arm64.dmg) — signed + notarized, bundles an on-device model |
| **Windows** (10/11 x64) | v0.2.1 | [Setup.exe](https://github.com/OneManNoCode/MoltBrowser/releases/download/v0.2.1/MoltBrowser-Windows-x64-Setup.exe) (installer) · [.zip](https://github.com/OneManNoCode/MoltBrowser/releases/download/v0.2.1/MoltBrowser-Windows-x64.zip) (portable, unzip & run `MoltBrowser.exe`) |
| **Linux** (x64) | v0.2.1 | [.deb](https://github.com/OneManNoCode/MoltBrowser/releases/download/v0.2.1/MoltBrowser-Linux-x64.deb) · [.rpm](https://github.com/OneManNoCode/MoltBrowser/releases/download/v0.2.1/MoltBrowser-Linux-x64.rpm) · [.tar.gz](https://github.com/OneManNoCode/MoltBrowser/releases/download/v0.2.1/MoltBrowser-Linux-x64.tar.gz) |

Or browse the [releases page](https://github.com/OneManNoCode/MoltBrowser/releases).

> **macOS v0.2.5** introduces **Agent mode** — a built-in web-automation studio: record a task, review it as editable plain-English steps, and run it on demand, in the background, or on a schedule, with a per-step screenshot history. Builds on v0.2.4's Liquid Glass UI + MoltNet fix and v0.2.3's bring-your-own-key cloud models, one-click import, and redesigned AI chat. Windows and Linux parity builds follow at the next cross-platform milestone (the v0.2.5 source is already cross-platform). The macOS DMG ships an on-device model in the download; Linux and Windows fetch one on first run.

## Current Status

| Feature | Status |
|---------|--------|
| Bring-your-own-key cloud models (OpenAI / Anthropic / Gemini + 7 more) | ✅ Complete (v0.2.3) |
| One-click import from any browser (bookmarks + passwords, folders preserved) | ✅ Complete (v0.2.3) |
| Redesigned AI chat (Recents, model picker, attachments, voice, themes) | ✅ Complete (v0.2.3) |
| On-device LLM inference (6 models, Metal/Vulkan GPU) | ✅ Complete |
| AI Chat (full page + side panel + omnibox) | ✅ Complete |
| MoltShield ad/tracker blocking (ABP filter engine) | ✅ Complete |
| Cookie consent popup auto-blocking | ✅ Complete |
| YouTube ad blocking (network + scriptlet) | ✅ Complete |
| Fingerprint protection (Canvas, WebGL, Audio) | ✅ Complete |
| Agent mode — record → edit → run / schedule web workflows | ✅ Complete (v0.2.5) |
| Agent mode — per-step screenshots + Runs history, Watch/Auto modes | ✅ Complete (v0.2.5) |
| MoltNet Tor routing UI | ✅ Complete |
| MoltNet Tor exit-country selector | ✅ Complete (v0.2.1, all platforms) |
| Persona system (5 built-in + custom) | ✅ Complete |
| Memory engine (SQLite + embeddings) | ✅ Complete |
| Chat export/import, search, keyboard shortcuts | ✅ Complete |
| Model downloading from HuggingFace | ✅ Complete |
| Sparkle/WinSparkle auto-update | ✅ Complete |
| macOS DMG (signed + notarized + stapled) | ✅ Shipped (v0.2.1) |
| Windows x64 (portable ZIP + NSIS installer) | ✅ Shipped (v0.2.1) |
| Windows Tor / voice / OCR (functional parity) | ✅ Shipped (v0.2.1) |
| Linux deb / rpm / tar.gz | ✅ Shipped (v0.2.1) |
| Android APK scaffolding | 🔨 Scaffolding |
| iOS IPA scaffolding | 🔨 Scaffolding |
| CI/CD GitHub Actions (macOS native, Linux Docker, Windows self-hosted) | ✅ Complete |

### Testing Pages

| URL | Purpose |
|-----|---------|
| `molt://ai` | Full-page AI chat with model switching |
| `molt://ai-chat` | Side panel AI chat |
| `molt://ai-settings` | Settings + MoltNet privacy controls |
| `molt://ai-agent` | Agent mode — record, run & schedule web automations |

## Privacy

MoltBrowser is built privacy-first:
- **No telemetry** — Zero data collection by default
- **Local AI** — Models run on your device, prompts never leave your machine
- **MoltShield** — Blocks trackers, mitigates fingerprinting
- **MoltNet** — Onion-routed IP privacy with a Tor exit-country selector
- **Open Source** — Full transparency, auditable code

## License

GPLv3 — See [LICENSE](LICENSE) for details.

## Contributing

MoltBrowser is an open-source project. Contributions welcome!
See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

---

Built with conviction by GenEye AI Labs Inc.
*AI is not a feature. AI is the core engine.*
