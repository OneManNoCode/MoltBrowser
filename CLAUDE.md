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
  otherwise. On a shared mac+linux Chromium checkout, `gclient` `target_os` must
  list **both** `mac` and `linux`; use depot_tools' bundled `python3` (not
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
- **`llama.cpp .at()` aborts under `-fno-exceptions`** — audit for `.at(` when updating the vendored llama.cpp.

### Build gotchas
- **Editing `src/chrome/common/webui_url_constants.h` fans out a ~2h full recompile** on this machine (every WebUI host/URL constant + `kMoltBrowserVersion` live there). Plan version bumps accordingly.
- After a build, **grep the full output for `error:` / "finished with an error"** — the `autoninja` wrapper can print "finished successfully" and exit 0 even on a failed compile. The real product code is in `Contents/Frameworks/MoltBrowser Framework.framework/…` (the `Contents/MacOS/MoltBrowser` binary is a thin launcher stub — its mtime/`strings` are meaningless).
- **Local macOS build+install loop: `./scripts/dev-install-mac.sh`** — stages the out-dir app, carries the tor/ocr/whisper/molt_models payloads + regenerates the REQUIRED `default.metallib` (wiped on every rebuild), does the inside-out per-helper codesign (allow-jit on Renderer/GPU) with the real Developer ID, and installs to /Applications. This is the day-to-day loop after `autoninja … chrome`.

### WebUI Pattern
- JS → C++: `chrome.send('methodName', [args])`
- C++ → JS: `FireWebUIListener("event-name", value)` or `ResolveJavascriptCallback(callback_id, value)`
- Register handlers: `web_ui->AddMessageHandler(std::make_unique<Handler>())`
- All WebUI HTML/CSS/JS is inline in C++ raw strings via URLDataSource

### Git Workflow
- Commit to `main` branch directly
- GitHub remote: `https://github.com/OneManNoCode/MoltBrowser.git`
- PAT requires `repo` + `workflow` scopes (for GitHub Actions)

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
`kContent` side panel with AI chat, so opening one swaps out the other. The
toolbar "AI mode" / "Agent mode" buttons (with the violet active ring) are in
`ui/views/toolbar/toolbar_view.cc`.

## Architecture Reference
See [ARCHITECTURE.md](ARCHITECTURE.md) for full system architecture, directory structure, and build configurations.
