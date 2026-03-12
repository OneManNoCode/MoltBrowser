# MoltBrowser — Build Progress Tracker

> **Purpose**: Persistent session state for the CTO Agent. If a session disconnects, resume by reading this file first. It captures exactly where we left off, what's running, what failed, and what's next.

---

## Session State

**Last Updated**: 2025-03-12 (Day 2)
**Current Phase**: Day 2 — BUILD SYSTEM + FEATURES IN PROGRESS
**Overall Status**: 🔄 Incremental build running with llama.cpp + AI modules + branding changes

---

## Environment

| Item | Value |
|------|-------|
| Hardware | Apple M4 Pro, 24GB RAM, 512GB SSD |
| OS | macOS 26.2 (Tahoe) |
| Compiler | Apple Clang 17.0.0 |
| Xcode | Installed at /Applications/Xcode.app |
| Python | 3.14.2 |
| Git | 2.52.0 |
| Ninja | 1.13.2 |
| CMake | 4.2.3 |
| gh CLI | 2.88.0, authenticated as OneManNoCode |
| GitHub Repo | https://github.com/OneManNoCode/MoltBrowser |

## Directory Layout

```
/Users/raj/Desktop/MoltBrowser/
├── repo/                              # Git repo (OneManNoCode/MoltBrowser)
│   ├── src/                           # Standalone AI module source (CMake build)
│   │   ├── molt_ai/                   # All AI modules
│   │   ├── moltshield/               # Privacy protection
│   │   ├── moltnet/                  # Privacy network
│   │   └── CMakeLists.txt            # Standalone build
│   ├── sdk/                           # Extension SDK + sample
│   ├── scripts/                       # setup.sh, configure.sh, build.sh
│   ├── docs/                          # Reports + this file
│   ├── build/                         # CMake standalone build output (gitignored)
│   └── chromium/                      # Chromium source tree (gitignored, 28GB+)
│       └── src/
│           ├── chrome/browser/molt_ai/  # AI modules (Chromium-path includes)
│           │   ├── runtime/             # BrowserAIRuntime + PromptRouter
│           │   ├── dom/                 # DOMInterpreter + DOMContentBridge (NEW)
│           │   ├── agents/              # AgentEngine
│           │   ├── security/            # ActionValidator
│           │   ├── memory/              # MemoryEngine
│           │   ├── personas/            # PersonaSystem
│           │   ├── models/              # ModelManager + ModelDownloader (NEW)
│           │   ├── api/                 # BrowserAIAPI
│           │   ├── shield/              # MoltShield (NEW - in Chromium tree)
│           │   ├── omnibox/             # AiPromptProvider source (copy)
│           │   └── side_panel/          # AiChatSidePanelCoordinator (NEW)
│           ├── components/omnibox/browser/
│           │   └── ai_prompt_provider.* # AI Omnibox Provider (NEW)
│           ├── third_party/llama_cpp/   # llama.cpp (cloned, depth=1)
│           │   └── BUILD.gn             # GN wrapper (NEW - split source_sets)
│           └── out/MoltBrowser/         # GN build output directory
├── depot_tools/                       # Chromium build tools
└── *.docx / *.html                    # Spec documents
```

---

## Completed Tasks

### Day 1 (2025-03-11)

- [x] Build tools installed, GitHub repo cloned
- [x] Chromium source fetched (28GB), gclient runhooks
- [x] llama.cpp cloned into third_party/
- [x] 10 AI modules implemented in C++ (all compiling standalone)
- [x] Standalone CMake build verified
- [x] AI modules copied into Chromium tree, include paths updated
- [x] GN build configured (29,414 targets)
- [x] Extension SDK (Manifest V3 sample)
- [x] Chromium build SUCCEEDED: 33,619 steps, 3h9m, 0 errors, 642MB binary
- [x] 3 commits pushed to GitHub

### Day 2 (2025-03-12)

- [x] **llama.cpp BUILD.gn wrapper** — Split into 8 source_sets to avoid duplicate .o:
  - `ggml_c`, `ggml_cpp`, `ggml_cpu_c`, `ggml_cpu_cpp`
  - `ggml_cpu_arch_arm`, `ggml_cpu_arch_arm_c`
  - `ggml_metal`, `ggml_metal_objc`
  - `llama_cpp` (main library with `-fexceptions`)
  - `llama_common` (utilities)
- [x] **Wired molt_ai into chrome/browser/BUILD.gn** — Added `//chrome/browser/molt_ai` dep
- [x] **Branding: BRANDING file** — GenEye AI Labs, MoltBrowser, com.geneyeailabs.MoltBrowser
- [x] **Branding: chromium_strings.grd** — Bulk replaced "Chromium" → "MoltBrowser" in user-facing strings
- [x] **Branding: User Agent** — Changed "Chromium" → "MoltBrowser" in user_agent_utils.cc
- [x] **Branding: Info.plist** — Updated Extension/Shortcut descriptions
- [x] **Google removal: Avatar button** — Hidden in toolbar_view.cc (show_avatar_toolbar_button = false)
- [x] **Google removal: NTP** — Disabled OneGoogleBar, logo, middle slot promo in new_tab_page_ui.cc
- [x] **AI Omnibox Provider** — `components/omnibox/browser/ai_prompt_provider.*`
  - Detects "@ai " and "?" prefixes
  - Creates NAVSUGGEST match to `chrome://molt-ai/?q=<prompt>`
  - Relevance 1500 (top of dropdown)
  - Registered in autocomplete_controller.cc
- [x] **AI Side Panel** — `chrome/browser/molt_ai/side_panel/`
  - `AiChatSidePanelCoordinator` — manages lifecycle
  - `AiChatSidePanelWebView` — loads chrome://molt-ai-chat/
  - TODO: Register with SidePanelRegistry (needs SidePanelEntryId macro change)
- [x] **DOMContentBridge** — `chrome/browser/molt_ai/dom/dom_content_bridge.*`
  - Bridges DOMInterpreter to content::WebContents
  - Async HTML/text extraction via JS execution in isolated world
  - Element inspection at coordinates
- [x] **ModelDownloader** — `chrome/browser/molt_ai/models/model_downloader.*`
  - HuggingFace URL builder for all 6 model IDs
  - Download registry mapping model_id → {repo, filename}
  - TODO: Wire to Chromium's SimpleURLLoader for actual HTTP download
- [x] **MoltShield (Chromium tree)** — `chrome/browser/molt_ai/shield/molt_shield.*`
  - 50+ tracker domains in default blocklist
  - URL tracking parameter stripping (utm_*, fbclid, gclid, etc.)
  - Cookie policy enforcement (4 levels)
  - Canvas/WebGL/Audio fingerprint protection JavaScript
  - Referrer sanitization
  - 4 protection levels (OFF, STANDARD, STRICT, AGGRESSIVE)
- [x] **Fixed raw_ptr violations** — browser_ai_api.h, agent_engine.h

## Currently Running

- **BUILD SUCCEEDED** — MoltBrowser.app (641MB) compiled with 0 errors
  - 408 incremental steps, 1m17s
  - All llama.cpp + AI modules + branding changes compiled clean

## Pending Tasks (Day 3)

- [ ] Verify MoltBrowser launches with new branding
- [ ] Test AI Omnibox "@ai " prefix detection
- [ ] Register AI Side Panel with SidePanelRegistry
- [ ] Create chrome://molt-ai/ WebUI for prompt handling
- [ ] Create chrome://molt-ai-chat/ WebUI for sidebar chat
- [ ] Wire ModelDownloader to Chromium's SimpleURLLoader
- [ ] Wire MoltShield to Chromium's network request interceptor
- [ ] Implement AI inference pipeline (llama.cpp → BrowserAIRuntime)
- [ ] Model download UI in settings/toolbar

---

## GN Build Arguments

```gn
is_debug = false
target_cpu = "arm64"
google_api_key = ""
google_default_client_id = ""
google_default_client_secret = ""
is_chrome_branded = false
is_component_build = false
symbol_level = 0
enable_extensions = true
use_system_xcode = true
chrome_pgo_phase = 0
treat_warnings_as_errors = false
```

---

## Build Errors Encountered & Fixes

### Day 1 Fixes
1. **PGO Profile Missing** → `chrome_pgo_phase = 0`
2. **Safe Browsing Deps** → Omit `safe_browsing_mode`
3. **Metal Toolchain** → `xcodebuild -downloadComponent MetalToolchain`
4. **sysctlbyname** → Add `<sys/sysctl.h>` include
5. **sleep_for** → Add `<thread>` include

### Day 2 Fixes
6. **Duplicate .o files** → Split ggml into 8 separate source_sets
7. **std::unique_ptr (Metal)** → Add `#include <memory>` to ggml-metal.cpp
8. **getenv undeclared** → Add `#include <cstdlib>` to llama-graph.h
9. **throw with exceptions disabled** → `configs -= ["no_exceptions"]` + `configs += [":llama_exceptions"]`
10. **std::advance** → Add `#include <iterator>` to llama-adapter.cpp
11. **raw_ptr violation** → Use `raw_ptr<T>` in browser_ai_api.h, agent_engine.h
12. **ARC conflict** → `configs -= ["enable_arc"]` + `-fno-objc-arc` for ObjC sources
13. **std::unique_ptr (backend-dl)** → Add `#include <memory>` to ggml-backend-dl.h
14. **getenv (backend-reg)** → Add `#include <cstdlib>` to ggml-backend-reg.cpp
15. **try in ggml_cpp** → Add `configs -= ["no_exceptions"]` to ggml_cpp source_set
16. **std::transform** → Add `#include <algorithm>` to persona_system.cc
17. **partition_alloc/raw_ptr.h** → Add `//base` dep to all molt_ai source_sets
18. **try/catch in prompt_router.cc** → Replace with exception-free PII detection
19. **std::sort (llama-quant.cpp)** → Add `#include <algorithm>`
20. **std::milli (agent_engine.cc)** → Add `#include <ratio>`
21. **std::milli (browser_ai_runtime.cc)** → Add `#include <ratio>`
22. **try/catch in agent_engine.cc** → Replace with strtol-based parsing
23. **GGML_VERSION/GGML_COMMIT** → Add defines to llama_cpp_config

---

## Key Commands Reference

```bash
# Set PATH for Chromium tools
export PATH="/Users/raj/Desktop/MoltBrowser/repo/depot_tools:$PATH"

# GN configure
cd /Users/raj/Desktop/MoltBrowser/repo/chromium/src
gn gen out/MoltBrowser

# Build (background)
nohup autoninja -C out/MoltBrowser chrome > /Users/raj/Desktop/MoltBrowser/build_day2.log 2>&1 &

# Check build errors
grep "error:" /Users/raj/Desktop/MoltBrowser/build_day2.log

# Run browser
open out/MoltBrowser/Chromium.app
```

---

## Metrics

| Metric | Day 1 | Day 2 |
|--------|-------|-------|
| Lines of C++ code | 5,246 | ~8,000+ |
| New files created | 35 | 50+ |
| Static libraries | 10 | 20+ (incl. llama.cpp) |
| GN targets | 29,414 | 29,436 |
| Build result | 33,619 steps, 3h9m | 408 incr. steps, 1m17s |
| App size | 642MB | 641MB |
| GitHub commits | 3 | 4 |
| Budget spent | $0.00 | $0.00 |

---

*This file is the source of truth for session continuity. Update after every significant milestone.*
