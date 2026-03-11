# MoltBrowser

**The AI-Native Browser for the Agent Era**

Built by [GenEye AI Labs Inc.](https://github.com/OneManNoCode)

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
