#!/bin/bash
# MoltBrowser Multi-Platform Release Orchestrator
# Copyright 2025 GenEye AI Labs Inc.
#
# Builds and releases MoltBrowser for all desktop platforms in sequence.
# Manages a single GitHub Release with assets from all platforms.
#
# Usage:
#   # Release all platforms
#   ./scripts/release-all.sh --version 0.1.0 --build 1 --upload
#
#   # macOS + Linux only
#   ./scripts/release-all.sh --version 0.1.0 --build 1 --platforms macos,linux
#
#   # Dry run (build + package, no upload)
#   ./scripts/release-all.sh --version 0.1.0 --build 1

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

VERSION=""
BUILD_NUM=""
DO_UPLOAD=false
PLATFORMS="macos,linux,windows"
MACOS_IDENTITY=""
MACOS_TEAM_ID=""
DO_NOTARIZE=false

while [[ $# -gt 0 ]]; do
  case $1 in
    --version) VERSION="$2"; shift 2 ;;
    --build) BUILD_NUM="$2"; shift 2 ;;
    --platforms) PLATFORMS="$2"; shift 2 ;;
    --upload) DO_UPLOAD=true; shift ;;
    --macos-identity) MACOS_IDENTITY="$2"; shift 2 ;;
    --macos-team-id) MACOS_TEAM_ID="$2"; shift 2 ;;
    --notarize) DO_NOTARIZE=true; shift ;;
    --help|-h)
      echo "MoltBrowser Multi-Platform Release"
      echo ""
      echo "Usage: $0 --version VERSION --build BUILD_NUM [options]"
      echo ""
      echo "Options:"
      echo "  --version          Version string (e.g., 0.1.0)"
      echo "  --build            Build number"
      echo "  --platforms        Comma-separated: macos,linux,windows (default: all)"
      echo "  --upload           Upload all artifacts to GitHub Releases"
      echo "  --macos-identity   Apple code signing identity"
      echo "  --macos-team-id    Apple Developer Team ID"
      echo "  --notarize         Notarize macOS build"
      exit 0
      ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

if [ -z "$VERSION" ] || [ -z "$BUILD_NUM" ]; then
  echo "ERROR: --version and --build required"
  exit 1
fi

echo "╔══════════════════════════════════════════════════╗"
echo "║    MoltBrowser Multi-Platform Release            ║"
echo "╠══════════════════════════════════════════════════╣"
echo "║  Version:     $VERSION (build $BUILD_NUM)"
echo "║  Platforms:   $PLATFORMS"
echo "║  Upload:      $DO_UPLOAD"
echo "╚══════════════════════════════════════════════════╝"
echo ""

DIST_DIR="$ROOT_DIR/dist"
mkdir -p "$DIST_DIR"
RESULTS=()
FAILED=()

# Create GitHub Release first (if uploading)
TAG="v${VERSION}"
if [ "$DO_UPLOAD" = true ]; then
  cd "$ROOT_DIR"
  git tag -f "$TAG" HEAD 2>/dev/null || true

  if ! gh release view "$TAG" &>/dev/null; then
    echo "Creating GitHub Release: $TAG"

    PRERELEASE=""
    [[ "$VERSION" == *"alpha"* || "$VERSION" == *"beta"* || "$VERSION" == *"rc"* ]] && PRERELEASE="--prerelease"

    gh release create "$TAG" \
      --title "MoltBrowser v${VERSION}" \
      $PRERELEASE \
      --notes "$(cat <<EOF
# MoltBrowser v${VERSION}

AI-native privacy browser with on-device LLM inference.

## Downloads
| Platform | Architecture | Format |
|----------|-------------|--------|
| macOS | Universal (arm64 + x64) | DMG |
| Linux | x64 | tar.gz, .deb, AppImage |
| Windows | x64 | Installer (.exe), Portable (.zip) |

## Install
- **macOS**: Download DMG → drag to Applications
- **Linux**: Download .deb/.tar.gz → install
- **Windows**: Download installer → run setup

See [moltbrowser.com](https://moltbrowser.com) for details.
EOF
)"
    echo "Release created."
  fi
fi

# --- macOS ---
if [[ "$PLATFORMS" == *"macos"* ]]; then
  echo ""
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  echo "  Building macOS..."
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

  MACOS_ARGS=(--version "$VERSION" --build "$BUILD_NUM" --universal --skip-build)
  [ -n "$MACOS_IDENTITY" ] && MACOS_ARGS+=(--identity "$MACOS_IDENTITY")
  [ -n "$MACOS_TEAM_ID" ] && MACOS_ARGS+=(--team-id "$MACOS_TEAM_ID")
  [ "$DO_NOTARIZE" = true ] && MACOS_ARGS+=(--notarize)
  [ "$DO_UPLOAD" = true ] && MACOS_ARGS+=(--upload)

  if bash "$SCRIPT_DIR/release.sh" "${MACOS_ARGS[@]}"; then
    RESULTS+=("macOS: ✅")
  else
    RESULTS+=("macOS: ❌")
    FAILED+=(macOS)
  fi
fi

# --- Linux ---
if [[ "$PLATFORMS" == *"linux"* ]]; then
  echo ""
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  echo "  Building Linux..."
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

  LINUX_ARGS=(--version "$VERSION" --build "$BUILD_NUM" --format all)

  # Use Docker if not on Linux
  if [ "$(uname -s)" != "Linux" ]; then
    LINUX_ARGS+=(--docker)
  fi
  [ "$DO_UPLOAD" = true ] && LINUX_ARGS+=(--upload)

  if bash "$SCRIPT_DIR/release-linux.sh" "${LINUX_ARGS[@]}"; then
    RESULTS+=("Linux: ✅")
  else
    RESULTS+=("Linux: ❌")
    FAILED+=(Linux)
  fi
fi

# --- Windows ---
if [[ "$PLATFORMS" == *"windows"* ]]; then
  echo ""
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  echo "  Building Windows..."
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

  WIN_ARGS=(--version "$VERSION" --build "$BUILD_NUM")

  # Use Docker for cross-compilation if not on Windows
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ;; # Native Windows
    *) WIN_ARGS+=(--docker) ;;
  esac
  [ "$DO_UPLOAD" = true ] && WIN_ARGS+=(--upload)

  if bash "$SCRIPT_DIR/release-win.sh" "${WIN_ARGS[@]}"; then
    RESULTS+=("Windows: ✅")
  else
    RESULTS+=("Windows: ❌")
    FAILED+=(Windows)
  fi
fi

# --- Update downloads.json ---
echo ""
echo "Generating downloads.json..."
DOWNLOADS_JSON="$ROOT_DIR/website/downloads.json"
cat > "$DOWNLOADS_JSON" << DLJSON
{
  "version": "$VERSION",
  "build": "$BUILD_NUM",
  "date": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "github_release": "https://github.com/OneManNoCode/MoltBrowser/releases/tag/$TAG",
  "platforms": {
    "macos": {
      "status": "available",
      "dmg": {
        "filename": "MoltBrowser-${VERSION}-macOS-universal.dmg",
        "url": "https://github.com/OneManNoCode/MoltBrowser/releases/download/$TAG/MoltBrowser-${VERSION}-macOS-universal.dmg"
      },
      "min_os": "12.0",
      "architectures": ["arm64", "x86_64"]
    },
    "linux": {
      "status": "available",
      "tar": {
        "filename": "MoltBrowser-${VERSION}-linux-x64.tar.gz",
        "url": "https://github.com/OneManNoCode/MoltBrowser/releases/download/$TAG/MoltBrowser-${VERSION}-linux-x64.tar.gz"
      },
      "deb": {
        "filename": "moltbrowser_${VERSION}_amd64.deb",
        "url": "https://github.com/OneManNoCode/MoltBrowser/releases/download/$TAG/moltbrowser_${VERSION}_amd64.deb"
      },
      "appimage": {
        "filename": "MoltBrowser-${VERSION}-linux-x64.AppImage",
        "url": "https://github.com/OneManNoCode/MoltBrowser/releases/download/$TAG/MoltBrowser-${VERSION}-linux-x64.AppImage"
      },
      "min_os": "Ubuntu 22.04 / Fedora 37+",
      "architectures": ["x86_64"]
    },
    "windows": {
      "status": "available",
      "installer": {
        "filename": "MoltBrowser-${VERSION}-win-x64-setup.exe",
        "url": "https://github.com/OneManNoCode/MoltBrowser/releases/download/$TAG/MoltBrowser-${VERSION}-win-x64-setup.exe"
      },
      "portable": {
        "filename": "MoltBrowser-${VERSION}-win-x64-portable.zip",
        "url": "https://github.com/OneManNoCode/MoltBrowser/releases/download/$TAG/MoltBrowser-${VERSION}-win-x64-portable.zip"
      },
      "min_os": "Windows 10",
      "architectures": ["x86_64"]
    }
  }
}
DLJSON

# --- Summary ---
echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║    Multi-Platform Release Complete               ║"
echo "╠══════════════════════════════════════════════════╣"
for r in "${RESULTS[@]}"; do
  echo "║  $r"
done
echo "╠══════════════════════════════════════════════════╣"
echo "║  Artifacts:"
ls -1 "$DIST_DIR/"MoltBrowser-${VERSION}* "$DIST_DIR/"moltbrowser_${VERSION}* 2>/dev/null | while read f; do
  SIZE=$(du -sh "$f" | cut -f1)
  echo "║    $(basename "$f") ($SIZE)"
done
echo "╚══════════════════════════════════════════════════╝"

if [ ${#FAILED[@]} -gt 0 ]; then
  echo ""
  echo "⚠️  Failed platforms: ${FAILED[*]}"
  echo "Check logs above for details."
  exit 1
fi
