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

Full history: [`CHANGELOG.md`](CHANGELOG.md). What's next: [`ROADMAP.md`](ROADMAP.md).

- **Coming in v0.3 "Private by default"** (in release testing): signed updates, MoltShield rebuilt on Brave's adblock-rust with tracking-parameter stripping, fingerprinting protection in the engine, MoltNet in its own isolated Tor window with `.onion` support, private-by-default settings, opt-in encrypted AI memory, Gemma 4 support, and locked-down internal pages. See the [changelog](CHANGELOG.md).

Want to contribute? See [`CONTRIBUTING.md`](CONTRIBUTING.md). Issues and
PRs welcome — pick anything from the changelog "Unreleased" section or
propose your own.

---

## What is MoltBrowser?

MoltBrowser is an open-source, AI-native web browser built on a hardened open web engine (Blink + V8) with embedded local LLM inference, browser automation, and privacy-first architecture. Unlike traditional browsers where AI is bolted on as an extension, in MoltBrowser **AI is the core runtime**.

## Key Features

- **Agent mode** — Record a web task once, review it as editable plain-English steps, then run it on demand, in the background, or on a schedule. Per-step screenshots, a Runs history, and label-aware replay that survives page redesigns.
- **On-device + cloud AI** — Run local models (TinyLlama ships in the box; add LLaMA, Qwen, Mistral, Phi via llama.cpp) *or* bring your own key for OpenAI / Anthropic / Gemini and any OpenAI-compatible endpoint. Your choice per chat.
- **Grounded attachments** — Drop in a document or image and MoltBrowser extracts its text on-device and answers from it. With a local model, nothing leaves your machine.
- **MoltShield Privacy** — Built-in tracker blocking and cookie-consent auto-dismissal. (v0.3, in release testing, adds full subresource blocking with community filter lists, tracking-parameter stripping and engine-level fingerprinting protection — see [ROADMAP.md](ROADMAP.md).)
- **MoltNet** — One-click Tor routing that hides your IP address from the sites you visit, with an exit-country selector in the toolbar. (v0.3, in release testing, moves it to a separate Tor profile with `.onion` support and fingerprinting uniformity. It is not yet a replacement for Tor Browser.)
- **Liquid Glass UI** — A visionOS-style frosted-glass new-tab page, AI panel, and settings; an opaque black-glass toolbar with floating pill buttons.
- **One-click import** — Bring your bookmarks and saved passwords over from another browser in a click.
- **Extension-compatible** — Supports standard Chrome web-extension APIs. (Web-standard built-in AI APIs for pages and extensions are planned.)
- **Desktop** — macOS (Apple Silicon) today; Windows and Linux builds track the same source.

## Architecture

```
+--------------------------------------------------+
|                Browser UI Layer                  |
|  Omnibox AI | AI chat (page + side panel)        |
|  Agent mode studio                               |
+--------------------------------------------------+
|           Browser Intelligence Layer             |
| Web agent + Agent mode (record / replay)         |
| Memory service | DOM interpreter | page context  |
+--------------------------------------------------+
|               LLM Runtime Layer                  |
| llama.cpp runtime | model downloads | cloud keys |
+--------------------------------------------------+
|                Model Layer                       |
| Llama | Qwen | Mistral | Phi | Gemma | gpt-oss   |
+--------------------------------------------------+
|              Privacy & Security                  |
| MoltShield | MoltNet | fingerprinting protection |
| agent safety | signed updates                    |
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
    chrome/browser/molt_ai/  # MoltBrowser's own browser-process code
      agents/                # Web agent
      automation/            # Agent mode: recorder, runner, scheduler
      dom/                   # DOM interpreter (page reader)
      fingerprinting/        # Fingerprinting protection (browser side)
      memory/                # Opt-in AI memory
      models/                # Verified model downloads
      runtime/               # On-device LLM runtime (llama.cpp)
      security/              # Agent safety, internal-page guards
      shield/                # MoltShield (adblock-rust)
      tor/                   # MoltNet
      update/                # Signed updates
    third_party/blink/renderer/core/molt_fingerprinting/  # Engine side
    third_party/llama_cpp/   # Vendored llama.cpp
  docs/                      # Documentation
  scripts/                   # Build and setup scripts
  build-system/              # Build configurations
```

## Built-in AI APIs (planned)

Pages and extensions can't call MoltBrowser's AI yet. The plan is the web
standards Chrome is shipping (the Prompt, Summarizer, Writer, Rewriter and
Translator APIs), backed by the on-device model you choose and running fully
offline. See [ROADMAP.md](ROADMAP.md). An earlier `browser.ai.*` sketch was
never wired up and has been removed.

## Download

The macOS and Linux builds are current at **v0.2.5**; the Windows desktop build is at **v0.2.1** and gets rebuilt at the next milestone (its source is already up to date).

| Platform | Version | Download |
|----------|---------|----------|
| **macOS** (Apple Silicon) | **v0.2.5** | [MoltBrowser-macOS-arm64.dmg](https://github.com/OneManNoCode/MoltBrowser/releases/latest/download/MoltBrowser-macOS-arm64.dmg) — signed + notarized, bundles an on-device model |
| **Windows** (10/11 x64) | v0.2.1 | [Setup.exe](https://github.com/OneManNoCode/MoltBrowser/releases/download/v0.2.1/MoltBrowser-Windows-x64-Setup.exe) (installer) · [.zip](https://github.com/OneManNoCode/MoltBrowser/releases/download/v0.2.1/MoltBrowser-Windows-x64.zip) (portable, unzip & run `MoltBrowser.exe`) |
| **Linux** (x64) | **v0.2.5** | [.deb](https://github.com/OneManNoCode/MoltBrowser/releases/latest/download/MoltBrowser-Linux-x64.deb) · [.rpm](https://github.com/OneManNoCode/MoltBrowser/releases/latest/download/MoltBrowser-Linux-x64.rpm) · [.tar.gz](https://github.com/OneManNoCode/MoltBrowser/releases/latest/download/MoltBrowser-Linux-x64.tar.gz) — install with `sudo apt install ./MoltBrowser-Linux-x64.deb` |

Or browse the [releases page](https://github.com/OneManNoCode/MoltBrowser/releases).

> **macOS v0.2.5** introduces **Agent mode** — a built-in web-automation studio: record a task, review it as editable plain-English steps, and run it on demand, in the background, or on a schedule, with a per-step screenshot history. Builds on v0.2.4's Liquid Glass UI + MoltNet fix and v0.2.3's bring-your-own-key cloud models, one-click import, and redesigned AI chat. Linux (.deb / .rpm / .tar.gz) is now built per-release on a self-hosted runner and is current at **v0.2.5**; the Windows parity build follows at the next milestone (the v0.2.5 source is already cross-platform). The macOS DMG ships an on-device model in the download; Linux and Windows fetch one on first run.

## Current Status

| Feature | Status |
|---------|--------|
| Bring-your-own-key cloud models (OpenAI / Anthropic / Gemini + 7 more) | ✅ Complete (v0.2.3) |
| One-click import from any browser (bookmarks + passwords, folders preserved) | ✅ Complete (v0.2.3) |
| Redesigned AI chat (Recents, model picker, attachments, voice, themes) | ✅ Complete (v0.2.3) |
| On-device LLM inference (7 models; Metal GPU on macOS, CPU on Windows/Linux) | ✅ Complete |
| AI Chat (full page + side panel + omnibox) | ✅ Complete |
| MoltShield blocking of tracker/ad navigations + iframes (built-in rules) | ✅ Shipped |
| MoltShield on adblock-rust: full subresource blocking, EasyList/uBO lists, cosmetics, per-site toggle | 🧪 v0.3 (in release testing) |
| Cookie consent popup auto-blocking | ✅ Complete |
| YouTube ad blocking (network + scriptlet) | 🧪 v0.3 (in release testing) |
| Fingerprint protection (engine-level randomization / Tor-window uniformity) | 🧪 v0.3 (in release testing) |
| Tracking-parameter stripping (utm_*, fbclid, gclid, uBO `$removeparam`) | 🧪 v0.3 (in release testing) |
| Agent mode — record → edit → run / schedule web workflows | ✅ Complete (v0.2.5) |
| Agent mode — per-step screenshots + Runs history, Watch/Auto modes | ✅ Complete (v0.2.5) |
| MoltNet Tor routing UI | ✅ Complete |
| MoltNet in its own isolated Tor window, per-site circuits, New Identity, bridges, `.onion` | 🧪 v0.3 (in release testing) |
| MoltNet Tor exit-country selector | ✅ Complete (v0.2.1, all platforms) |
| Memory engine (SQLite, keyword-hash search; real embeddings planned) | ✅ Shipped — opt-in + encrypted in v0.3 |
| Chat export/import, search, keyboard shortcuts | ✅ Complete |
| Model downloading from HuggingFace | ✅ Complete |
| In-app updates from GitHub Releases | ✅ Shipped — offline-signed and verified from v0.3 |
| macOS DMG (signed + notarized + stapled) | ✅ Shipped (v0.2.1) |
| Windows x64 (portable ZIP + NSIS installer) | ✅ Shipped (v0.2.1) |
| Windows Tor / voice / OCR (functional parity) | ✅ Shipped (v0.2.1) |
| Linux deb / rpm / tar.gz | ✅ Shipped (v0.2.5) |
| Android APK scaffolding | 🔨 Scaffolding |
| iOS IPA scaffolding | 🔨 Scaffolding |
| Release automation (macOS + Linux self-hosted; Windows paused) | ✅ Partial |
| PR checks (WebUI JS, strict-CSP smoke test, renderer fuzzing, workflow lint) | ✅ New |
| Private-by-default settings, locked-down internal pages, agent guardrails | 🧪 v0.3 (in release testing) |

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
- **Local AI** — Local models run on your device and their prompts never leave your machine; cloud models are opt-in with your own key
- **MoltShield** — Blocks known tracker and ad domains
- **MoltNet** — Tor routing that hides your IP from sites, with an exit-country selector
- **Open Source** — Full transparency, auditable code

## License

GPLv3 — See [LICENSE](LICENSE) for details.

## Contributing

MoltBrowser is an open-source project. Contributions welcome!
See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

---

Built with conviction by GenEye AI Labs Inc.
*AI is not a feature. AI is the core engine.*
