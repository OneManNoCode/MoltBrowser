# CLAUDE.md — MoltBrowser Development Guide

## What is this project?
MoltBrowser is an AI-native privacy browser built on a Chromium fork by GenEye AI Labs Inc. It integrates on-device LLM inference, ad/tracker blocking, privacy routing, and browser automation agents.

## Repository Layout
- `src/` — Git-tracked MoltBrowser source files (copied into `chromium/src/` for build)
- `chromium/` — Full Chromium source tree (gitignored, ~30GB)
- `configs/` — Platform-specific GN build args (macos, linux, windows, android, ios)
- `scripts/` — Build, package, and release automation
- `build/docker/` — Docker build environments for Linux and Windows cross-compilation
- `installer/` — NSIS (Windows), desktop files (Linux)
- `branding/` — App icon assets
- `website/` — Landing page
- `dist/` — Build artifacts (gitignored)

## Build Instructions

### macOS (native)
```bash
./scripts/configure.sh --platform macos --arch arm64
./scripts/build.sh
```

### Linux (via Docker from Mac)
```bash
./build/docker/build-linux.sh --package
```

### Windows (self-hosted GitHub Actions runner — NOT Docker)
> **Status (as of 2026-07-03): this build path is paused.** The self-hosted
> runner was on a work machine that got security-flagged, so Windows builds are
> not currently produced this way — Windows binaries remain frozen at v0.2.1
> while the source stays cross-platform. Re-establish a non-work runner before
> using the flow below.

Windows builds run on a self-hosted runner (the host's Windows PC), via the
`release-windows-selfhosted.yml` workflow. Trigger it:
```bash
gh workflow run release-windows-selfhosted.yml -f tag=v0.2.1 --repo OneManNoCode/MoltBrowser
```
Prereqs on the runner: online ("Listening for Jobs"), VS 2022 Build Tools (C++
workload) + Windows SDK "Debugging Tools". Builds in short root `C:\cr`
(MAX_PATH) and resumes incrementally. The monolithic `chrome.dll` link is
RAM-bound and slow on low-memory machines (hours via swap) but completes — a
long link is not a hang. Full gotcha list: `docs/BUILD_PROGRESS.md`.

### Direct build command
```bash
export PATH="$PWD/depot_tools:/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:$PATH"
autoninja -C chromium/src/out/MoltBrowser chrome
```

### Packaging gotchas (learned in the v0.2.1 2026-06-22 rebuild)
- **macOS:** codesign the bundled `tor`/`ocr`/`whisper` binaries under
  `Contents/Resources/` **explicitly** with the hardened runtime — `codesign
  --deep` skips `Resources/`, so they ship unsigned and fail notarization
  otherwise. That includes tesseract's dylibs in `Resources/ocr/lib/` (copied
  from Homebrew and rewritten by `scripts/macho_bundle_deps.py`): the helper
  sandbox maps code only from the helper's directory and the OS, and library
  validation refuses an ad hoc lib. `release.sh`, `sign-app.sh` and
  `dev-install-mac.sh` refuse a helper that loads from outside the app
  (shared steps: `scripts/lib-bundled-helpers.sh`). On a shared mac+linux
  Chromium checkout, `gclient` `target_os` must list **both** `mac` and `linux`; use depot_tools' bundled `python3` (not
  Homebrew 3.14); set `enable_supervised_users=true` + `safe_browsing_mode=1` to
  match linux/windows; download the Metal toolchain via `xcodebuild
  -downloadComponent MetalToolchain`.
- **Windows:** build the portable ZIP with **7-Zip** so entries use forward
  slashes (.NET `ZipFile` writes backslashes; `Compress-Archive` is too slow).
  Fetch NSIS by extracting its 7-Zip SFX with `7zr` from a direct
  `master.dl.sourceforge.net` mirror.
- Full play-by-play in `docs/BUILD_PROGRESS.md`.

## Key Development Rules

### File Management
- Always work in `repo/src/` for git-tracked files
- Files must be copied to `chromium/src/` before building
- Always `cd /Users/raj/Desktop/MoltBrowser/repo` before copy commands
- Use `git add -f` for files in gitignored directories (build/, src/chrome/browser/molt_ai/models/)

### Chromium API Patterns (CRITICAL)
- `FindInt()`, `FindDouble()`, `FindBool()` → return `std::optional<T>`, use `auto v` NOT `auto* v`
- `FindString()` → returns pointer, use `auto* v`
- `base::SysInfo::AmountOfFreeDiskSpace` → returns `std::optional<int64_t>`, use `.value_or(-1)`
- `base::FilePath`: No `.clear()` → use `= base::FilePath()`. No `.empty()` → use `.value().empty()`
- `base::ReplaceFile` instead of `base::Move`
- `base::GetFileSize` → returns `std::optional<int64_t>` (single argument)
- **`base::Value` in this tree has NO nested `List()`/`Dict()`** → use `base::ListValue` / `base::DictValue` (live throughout `molt_ai/automation/*`)
- **`GURL::host()` returns `std::string_view`** here → wrap as `std::string(g.host())`, not `= g.host()`
- **`CopyFromSurface` is the 4-arg glic-era form**: `(gfx::Rect, gfx::Size, base::TimeDelta, cb)` where `cb` takes `const content::CopyFromSurfaceResult&` (a `base::expected<viz::CopyOutputBitmapWithMetadata, …>`); wrap with `mojo::WrapCallbackWithDefaultInvokeIfNotRun`. Used for per-step + record-time screenshots.
- **llama.cpp runs only in the sandboxed inference service.** `//third_party/llama_cpp` is GN-visible only to `chrome/services/molt_ai/inference` (a utility process, `kOnDeviceModelExecution` sandbox; design: `docs/design/SANDBOXED_INFERENCE.md`). Browser code never includes `llama.h`: it uses `BrowserAIRuntime`, which talks to the service over `molt_ai.mojom.MoltInferenceService` and treats everything it sends as hostile (`runtime/stream_validator.*`). Files go to the service as read-only handles, never paths.
- **Never let an exception escape a `llama_*` call.** The llama.cpp targets build *with* exceptions; the service code calling them doesn't, so an escaping exception kills the on-device AI process (not the browser any more, but it is still a crash, and repeated crashes lock the model out for a while). Still enforced: check every C API you start using (the checked list is at the top of `llama_engine.cc`), and re-audit when updating the vendored llama.cpp (local patches are listed in `src/third_party/llama_cpp/README.chromium`; keep `GGML_MOLTBROWSER_NO_BACKTRACE` and `LLAMA_MOLTBROWSER_SANDBOXED`, which drops `MAP_POPULATE` that the sandbox kills).
- **No `std::regex` on page content** — it's quadratic on hostile input and a regex error aborts under `-fno-exceptions`. Write a linear scanner (see `molt_ai/dom/dom_interpreter.cc`).
- **Helper binaries go through `molt_ai/common/helper_binary.h`** (bundled copy, or a root-owned system copy on Linux) — never Homebrew, `/usr/local` or PATH. Launch system tools by absolute path.

### Build gotchas
- **Editing `src/chrome/common/webui_url_constants.h` fans out a ~2h full recompile** on this machine (every WebUI host/URL constant + `kMoltBrowserVersion` live there). Plan version bumps accordingly.
- After a build, **grep the full output for `error:` / "finished with an error"** — the `autoninja` wrapper can print "finished successfully" and exit 0 even on a failed compile. The real product code is in `Contents/Frameworks/MoltBrowser Framework.framework/…` (the `Contents/MacOS/MoltBrowser` binary is a thin launcher stub — its mtime/`strings` are meaningless).
- **Local macOS build+install loop: `./scripts/dev-install-mac.sh`** — stages the out-dir app, carries the tor/ocr/whisper/molt_models payloads + regenerates the REQUIRED `default.metallib` (wiped on every rebuild; built from llama.cpp's split kernels by `scripts/build-ggml-metallib.sh`), does the inside-out per-helper codesign (allow-jit on Renderer/GPU) with the real Developer ID, and installs to /Applications. This is the day-to-day loop after `autoninja … chrome`.
  It re-bundles a carried-over tesseract that loads libraries from outside the app, and won't install unless tesseract reads a test image under the real helper sandbox (`tools/ocr_smoke/ocr_smoke_mac.sh`; run it alone with `--app /Applications/MoltBrowser.app`).

### WebUI Pattern (strict CSP — see `ui/webui/molt_ai/molt_webui_csp.h`)
- JS → C++: `chrome.send('methodName', [args])`
- C++ → JS: `FireWebUIListener("event-name", value)` or `ResolveJavascriptCallback(callback_id, value)`
- Register handlers: `web_ui->AddMessageHandler(std::make_unique<Handler>())`
- Each page's URLDataSource serves the HTML at `/` and its script at `/app.js`
  (`<script src="app.js">`); both still live as C++ raw strings. CSP is
  `script-src 'self'` + `require-trusted-types-for 'script'` via
  `molt_webui::PageContentSecurityPolicy()` — **no inline `<script>`, no
  `onclick=` attributes, no `'unsafe-inline'` for scripts**.
- Events: `data-on-click="name"` in markup + `moltActions({name: fn})` in JS.
  HTML strings only through `moltHTML()` (the single `molt-html` Trusted Types
  policy); prefer DOM APIs + `textContent`. Escape everything page- or
  model-derived.
- Handlers validate arguments with `HasCallbackArgs()` / `StringArg()`
  (`molt_ai_chat_handler_helpers.h`) — never `CHECK` on renderer input.
- Before committing WebUI changes run: `python3 tools/webui_checks/check_embedded_js.py .`,
  `python3 tools/webui_checks/check_no_inline_handlers.py .`, and
  `cd tools/webui_checks && npm ci && npm test` (markdown-renderer fuzz +
  jsdom smoke test of every page). CI runs the same (`webui-checks.yml`).

### Git Workflow
- `main` is branch-protected: work on a branch and merge through a pull request
- GitHub remote: `https://github.com/OneManNoCode/MoltBrowser.git`
- PAT requires `repo` + `workflow` scopes (for GitHub Actions)

### Chromium pin + patch series
- The Chromium revision lives ONLY in `chromium_version.txt` (read via
  `tools/chromium/pin.sh`). Copied upstream files in `src/` are mirrored as a
  patch series in `patches/`. After editing any upstream file in `src/`, run
  `python3 tools/chromium/overlay_to_patches.py` then
  `python3 tools/chromium/verify_patches.py`. Rebasing onto a new Chromium
  release: `tools/chromium/rebase.py` + `docs/REBASING.md` (target: Extended
  Stable; ship each Chrome security release within 7 days).

### Releases are signed offline
- The updater installs only installers matching `molt-release-manifest.json`
  signed by a key in `src/chrome/browser/molt_ai/update/update_signing_keys.cc`.
  Releases are created as drafts; the owner signs offline with
  `tools/release_signing/molt_release_sign.py` and publishes via
  `scripts/publish-release-signature.sh`. Never publish without the manifest
  + `.sig`. Runbook: `docs/SIGNED_UPDATES.md`. The private key never touches
  CI, a cloud machine or this repo.

### Checks that need no Chromium checkout
- WebUI checks above; `molt_ai_unittests` targets can often be compiled
  standalone against small base/crypto stand-ins for quick verification.
- On-device inference: the engine utils (`chrome/services/molt_ai/inference/engine/`)
  and `runtime/stream_validator.*` are std-only and build with system gtest;
  `llama_engine.cc` is std + `llama.h` and links against the vendored
  llama.cpp objects with a tiny GGUF (no `//base`).
- MoltShield engine: `cd tools/shield_bench && ./fetch_lists.sh && cargo test --release && cargo run --release`.
- Rust crates (adblock for MoltShield) are vendored with gnrt on the build
  machine: `scripts/vendor-rust-crates.sh`.

## Agent mode (automation engine)
The v0.2.5 headline feature lives in `src/chrome/browser/molt_ai/automation/`:
`AutomationRecorder` (captures user actions as steps), `AutomationRunner`
(replays them; label-aware element matching + per-step screenshots),
`AutomationScheduler` (+ service/factory), `AutomationStorage` (one `.molt`
JSON per workflow under `~/.moltbrowser/automations/<id>.molt`),
`automation_recorder_tab_helper`, and `automation_background_browser` (the
throwaway run window). The studio UI is an inline WebUI in
`src/chrome/browser/ui/webui/molt_ai/molt_ai_agent_ui.cc` served at
`chrome://molt-ai-agent/` (user-facing `molt://ai-agent`), hosted in the side
panel via `molt_ai/side_panel/agent_side_panel_web_view.*` — it shares one
`kContent` side panel with AI chat, so opening one swaps out the other.
Every navigation/click/type an agent, chat action or replay performs goes
through `molt_ai/security/agent_safety.*` (http(s)-only, origin scope,
credential-field detection, untrusted-text fencing) and the workflow trust
policy in `automation_security.*` (recorded workflows start Approved on their
recorded sites; imports start Casual; trust changes need a real user click).
Recorded secrets are vault references, never stored values. The
toolbar "AI mode" / "Agent mode" buttons (with the violet active ring) are in
`ui/views/toolbar/toolbar_view.cc`.

## Architecture Reference
See [ARCHITECTURE.md](ARCHITECTURE.md) for full system architecture, directory structure, and build configurations.
