# MoltBrowser

**The AI-Native Browser for the Agent Era**

Built by [GenEye AI Labs Inc.](https://github.com/OneManNoCode)

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Changelog](https://img.shields.io/badge/changelog-keep--a--changelog-orange)](CHANGELOG.md)
[![Devblog](https://img.shields.io/badge/devblog-daily%20updates-green)](website/updates/devblog/)

---

## What's new

We push to `main` every day. The latest visible work:

- **2026-05-12** — AI-grouped history: `/history` clusters your reading into topic cards, sub-50ms, all local. ([post](website/updates/devblog/2026-05-12-grouped-history.md))
- **2026-05-12** — Form filler agent: encrypted local profile, `/fill` autofills any web form, nothing ever syncs. ([post](website/updates/devblog/2026-05-12-form-filler.md))
- **2026-05-12** — PDF chat: side panel reads PDFs via AX-tree snapshot, fully local. ([post](website/updates/devblog/2026-05-12-pdf-chat.md))
- **2026-05-12** — Universal cookie killer, tab triage, page watchers, agent inbox tray. ([post](website/updates/devblog/2026-05-12-four-agent-features.md))
- **2026-05-11** — Page-content chunking, memory grounding, new action verbs.
- **2026-05-10** — Side panel grounded in the active tab, LLM emits actions directly.
- **2026-05-08** — Personal Vector Memory: encrypted on-device semantic index.

Full history: [`CHANGELOG.md`](CHANGELOG.md) — Narrative behind each entry: [`website/updates/devblog/`](website/updates/devblog/).

Want to contribute? See [`CONTRIBUTING.md`](CONTRIBUTING.md). Issues and
PRs welcome — pick anything from the changelog "Unreleased" section or
propose your own.

---

## What is MoltBrowser?

MoltBrowser is an open-source, AI-native web browser built on a Chromium fork with embedded local LLM inference, autonomous browsing agents, and privacy-first architecture. Unlike traditional browsers where AI is bolted on as an extension, in MoltBrowser **AI is the core runtime**.

## Key Features

- **Embedded Local LLMs** — Run LLaMA, Qwen, Mistral, and Phi models directly in the browser via llama.cpp. No cloud required.
- **Autonomous Agent Mode** — AI agents that can browse, extract data, fill forms, and complete multi-step research tasks.
- **Chrome-Compatible** — Full Chrome extension support, identical UI patterns, zero learning curve for Chrome users.
- **MoltShield Privacy** — Tracker blocking, fingerprint mitigation, cookie management, and anti-detection systems.
- **MoltNet Privacy Mesh** — Tor-level IP privacy through decentralized routing.
- **Multi-AI Sidebar** — Built-in AI panel supporting multiple models for summarization, code generation, and research.
- **Persona System** — Customizable AI personas (Researcher, Developer, Lawyer, etc.) that modify AI reasoning.
- **browser.ai.* APIs** — Extension APIs for developers to build AI-powered browser extensions.
- **Cross-Platform** — Windows, macOS, Linux, Android, iOS.

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
| Blink | V8 | Network Stack | Chromium Base       |
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

## Current Status (v0.1.0)

| Feature | Status |
|---------|--------|
| On-device LLM inference (6 models, Metal/Vulkan GPU) | ✅ Complete |
| AI Chat (full page + side panel + omnibox) | ✅ Complete |
| MoltShield ad/tracker blocking (ABP filter engine) | ✅ Complete |
| Cookie consent popup auto-blocking | ✅ Complete |
| YouTube ad blocking (network + scriptlet) | ✅ Complete |
| Fingerprint protection (Canvas, WebGL, Audio) | ✅ Complete |
| Agent browser automation (CLICK, SCROLL, NAVIGATE) | ✅ Complete |
| Agent testing UI (`chrome://molt-ai-agent/`) | ✅ Complete |
| MoltNet privacy routing UI | ✅ Complete |
| Persona system (5 built-in + custom) | ✅ Complete |
| Memory engine (SQLite + embeddings) | ✅ Complete |
| Chat export/import, search, keyboard shortcuts | ✅ Complete |
| Model downloading from HuggingFace | ✅ Complete |
| Sparkle/WinSparkle auto-update | ✅ Complete |
| macOS DMG + notarization pipeline | ✅ Ready |
| Windows NSIS installer | ✅ Ready |
| Linux deb/rpm/AppImage/Flatpak | ✅ Ready |
| Android APK scaffolding | ✅ Scaffolding |
| iOS IPA scaffolding | ✅ Scaffolding |
| CI/CD GitHub Actions (5-platform matrix) | ✅ Complete |

### Testing Pages

| URL | Purpose |
|-----|---------|
| `chrome://molt-ai/` | Full-page AI chat with model switching |
| `chrome://molt-ai-chat/` | Side panel AI chat |
| `chrome://molt-ai-settings/` | Settings + MoltNet privacy controls |
| `chrome://molt-ai-agent/` | Agent testing & automation |

## Privacy

MoltBrowser is built privacy-first:
- **No telemetry** — Zero data collection by default
- **Local AI** — Models run on your device, prompts never leave your machine
- **MoltShield** — Blocks trackers, mitigates fingerprinting
- **MoltNet** — Tor-level IP privacy through decentralized routing
- **Open Source** — Full transparency, auditable code

## License

GPLv3 — See [LICENSE](LICENSE) for details.

## Contributing

MoltBrowser is an open-source project. Contributions welcome!
See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

---

Built with conviction by GenEye AI Labs Inc.
*AI is not a feature. AI is the core engine.*
