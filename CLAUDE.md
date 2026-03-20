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

### Windows (via Docker cross-compile)
```bash
./scripts/release-win.sh --version 0.1.0 --build 1 --docker
```

### Direct build command
```bash
export PATH="$PWD/depot_tools:/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:$PATH"
autoninja -C chromium/src/out/MoltBrowser chrome
```

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

### WebUI Pattern
- JS → C++: `chrome.send('methodName', [args])`
- C++ → JS: `FireWebUIListener("event-name", value)` or `ResolveJavascriptCallback(callback_id, value)`
- Register handlers: `web_ui->AddMessageHandler(std::make_unique<Handler>())`
- All WebUI HTML/CSS/JS is inline in C++ raw strings via URLDataSource

### Git Workflow
- Commit to `main` branch directly
- GitHub remote: `https://github.com/OneManNoCode/MoltBrowser.git`
- PAT requires `repo` + `workflow` scopes (for GitHub Actions)

## Architecture Reference
See [ARCHITECTURE.md](ARCHITECTURE.md) for full system architecture, directory structure, and build configurations.
