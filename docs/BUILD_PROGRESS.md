# MoltBrowser — Build Progress Tracker

> **Purpose**: Persistent session state for the CTO Agent. If a session disconnects, resume by reading this file first. It captures exactly where we left off, what's running, what failed, and what's next.

---

## Session State

**Last Updated**: 2026-06-22
**Current Phase**: ✅ SHIPPED — v0.2.1 at full feature parity on macOS, Linux, and Windows
**Overall Status**: All three desktop platforms rebuilt 2026-06-22 and republished on the [v0.2.1 release](https://github.com/OneManNoCode/MoltBrowser/releases/tag/v0.2.1). Windows reached functional parity (Tor/OCR/voice) and gained an NSIS installer; a MoltNet Tor exit-country selector shipped on all three. See "Milestone: cross-platform parity rebuild" immediately below.

---

## Milestone: cross-platform parity rebuild (v0.2.1 — 2026-06-22)

All three desktop platforms were rebuilt and now ship the **same feature set**.
Final assets on v0.2.1: `MoltBrowser-macOS-arm64.dmg` (883 MB, Developer ID
signed + notarized + stapled, bundles a TinyLlama on-device model),
`MoltBrowser-Linux-x64.{deb,rpm,tar.gz}` (141/194/195 MB),
`MoltBrowser-Windows-x64.zip` (702 MB portable) +
`MoltBrowser-Windows-x64-Setup.exe` (672 MB NSIS installer). Only macOS bundles
the model in the download; Linux/Windows fetch one on first run.

**New cross-platform feature — MoltNet Tor exit-country selector.** Users pick an
exit country from the AI side panel; the app writes `ExitNodes {cc}` +
`StrictNodes 1` into the Tor `torrc` and reloads, routing traffic through an exit
relay in that country. It's Tor (not a classic VPN): slower, limited to countries
that host exit relays, and some sites block Tor — framed accordingly in-product.

**Windows brought to functional parity** (these three were stubbed in the earlier
0.2.1 preview and already worked on macOS/Linux):
- **Tor/MoltNet** — POSIX sockets ported to winsock (`WSAStartup`/`closesocket`);
  bundles the Tor Expert Bundle 15.0.15 (`tor.exe` + geoip).
- **OCR** — bundles Tesseract 5.4.0 (+ `eng`/`osd` traineddata).
- **Voice transcription** — bundles whisper.cpp v1.9.1 + the `ggml-tiny.en` model.
- **Native NSIS installer** added alongside the portable ZIP.

### Build lessons learned this round

- **Windows ZIP entries must use forward slashes.** Build the zip with **7-Zip**
  — .NET `ZipFile` writes backslash separators (some tools mis-extract), and
  `Compress-Archive` is too slow on a tree this size.
- **NSIS fetch on the runner** — extract the NSIS 7-Zip SFX with **`7zr`** off a
  direct `master.dl.sourceforge.net` mirror (the redirecting download URLs were
  unreliable in CI).
- **macOS environment drift fixed** (shared Chromium checkout used for both mac
  and linux):
  - `gclient` `target_os` must list **both `mac` and `linux`** on the shared
    checkout, or one platform's deps go missing.
  - Use **depot_tools' bundled `python3`**, not Homebrew 3.14, to run the build
    scripts.
  - Set `enable_supervised_users=true` and `safe_browsing_mode=1` to match the
    Linux/Windows configs.
  - The Xcode **Metal Toolchain** must be downloaded via
    `xcodebuild -downloadComponent MetalToolchain`.
  - **Codesign the bundled tor/ocr/whisper binaries explicitly** (with the
    hardened runtime) under `Contents/Resources/` — `codesign --deep` skips
    `Resources/`, so they'd otherwise ship unsigned and break notarization.

---

## Milestone: Windows + 3-platform release (v0.2.1 — 2026-06-20)

MoltBrowser now ships on **macOS, Linux, and Windows**. Assets on v0.2.1:
`MoltBrowser-macOS-arm64.dmg` (signed + notarized), `MoltBrowser-Linux-x64.{deb,rpm,tar.gz}`,
and `MoltBrowser-Windows-x64.zip` (546 MB portable — unzip & run `MoltBrowser.exe`).

**Windows is built on a self-hosted GitHub Actions runner** (the user's Windows
laptop, host `DSRINIVAS-REM`) via `.github/workflows/release-windows-selfhosted.yml`.
Free GitHub-hosted runners (4 vCPU, 6 h cap) cannot compile Chromium in time;
self-hosted has no cap. The pipeline runs entirely in **native PowerShell** (NOT
Git Bash, which mangles `/c/` paths handed to depot_tools):

1. Locate `git` + add to PATH (the runner's PATH can predate the Git install).
2. Install + bootstrap depot_tools (`gclient --version`).
3. Ensure VS 2022 Build Tools (C++ workload) + the Windows SDK **Debugging Tools**
   (`dbghelp.dll`); export `vs2022_install` + `GYP_MSVS_OVERRIDE_PATH` so
   `vs_toolchain.py` finds VS under 64-bit `Program Files`.
4. Sync Chromium (pinned `51a413be…`) into short root **`C:\cr`** to dodge Windows
   MAX_PATH; skip on warm runs; `C:\cr` persists across runs.
5. `gn gen` with `configs/windows-x64.gn`.
6. `autoninja -C out/MoltBrowser -j <cores/4> chrome` — the laptop is RAM-limited,
   so `-j` is capped to avoid swap-thrash, plus keep-awake via
   `SetThreadExecutionState`. The final `chrome.dll` link is RAM-bound and slow
   (~10 h through swap) but **completes — a long link is not a hang**.
7. Package portable zip + upload via `softprops/action-gh-release`.

**Windows-portability fixes (molt_ai was written POSIX-only)** — full list in the
CTO-agent memory `windows_selfhosted_build.md`. Highlights: `FilePath::value()` is
`std::wstring` on Windows (→ `AsUTF8Unsafe()`/`FromUTF8Unsafe()`); `std::ofstream`
→ `base::WriteFile`/`AppendToFile`; POSIX headers/sockets/`access()` in
tor/voice/ocr guarded + stubbed (those 3 are non-functional on the Windows
preview); llama.cpp exceptions via `/EHsc` on clang-cl; the fork's
`base::ListValue`/`base::DictValue` names; committed the never-tracked
`model_manager.h`; added missing `ggml-backend-dl.cpp`; restored the wrongly-removed
`build_with_tflite_lib` model-service BUILD.gn blocks.

**Next:** Android/iOS (still scaffolding). _(Windows installer + functional
tor/voice/ocr landed in the 2026-06-22 parity rebuild — see milestone above.)_

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
