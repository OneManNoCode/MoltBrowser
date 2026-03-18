#!/bin/bash
# MoltBrowser Windows Release Pipeline
# Copyright 2025 GenEye AI Labs Inc.
#
# Complete Windows release: Build → Package → Sign → Upload
# Can run natively on Windows (Git Bash/MSYS2) or cross-compile via Docker.
#
# Usage:
#   # Native Windows build (Git Bash)
#   ./scripts/release-win.sh --version 0.1.0 --build 1
#
#   # Cross-compile from Linux via Docker
#   ./scripts/release-win.sh --version 0.1.0 --build 1 --docker
#
#   # With code signing (EV certificate)
#   ./scripts/release-win.sh --version 0.1.0 --build 1 --sign --upload

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

VERSION=""
BUILD_NUM=""
DO_UPLOAD=false
DO_SIGN=false
SKIP_BUILD=false
USE_DOCKER=false
SIGN_CERT=""

while [[ $# -gt 0 ]]; do
  case $1 in
    --version) VERSION="$2"; shift 2 ;;
    --build) BUILD_NUM="$2"; shift 2 ;;
    --upload) DO_UPLOAD=true; shift ;;
    --sign) DO_SIGN=true; shift ;;
    --cert) SIGN_CERT="$2"; shift 2 ;;
    --skip-build) SKIP_BUILD=true; shift ;;
    --docker) USE_DOCKER=true; shift ;;
    --help|-h)
      echo "MoltBrowser Windows Release Pipeline"
      echo ""
      echo "Usage: $0 --version VERSION --build BUILD_NUM [options]"
      echo ""
      echo "Options:"
      echo "  --version      Version string"
      echo "  --build        Build number"
      echo "  --upload       Upload to GitHub Releases"
      echo "  --sign         Code sign with Authenticode"
      echo "  --cert PATH    Path to .pfx code signing certificate"
      echo "  --skip-build   Skip the build step"
      echo "  --docker       Cross-compile via Docker"
      exit 0
      ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

if [ -z "$VERSION" ] || [ -z "$BUILD_NUM" ]; then
  echo "ERROR: --version and --build required"
  exit 1
fi

echo "╔══════════════════════════════════════════╗"
echo "║    MoltBrowser Windows Release           ║"
echo "╠══════════════════════════════════════════╣"
echo "║  Version:   $VERSION (build $BUILD_NUM)"
echo "║  Sign:      $DO_SIGN"
echo "║  Docker:    $USE_DOCKER"
echo "║  Upload:    $DO_UPLOAD"
echo "╚══════════════════════════════════════════╝"
echo ""

DIST_DIR="$ROOT_DIR/dist"
BUILD_DIR="$ROOT_DIR/chromium/src/out/MoltBrowser"

if [ "$USE_DOCKER" = true ]; then
  # --- Docker cross-compilation ---
  echo "[1/5] Cross-compiling via Docker..."
  DOCKER_IMG="moltbrowser-wincross"

  if [ "$(docker images -q $DOCKER_IMG 2>/dev/null)" = "" ]; then
    docker build \
      --build-arg BUILD_UID=$(id -u) \
      --build-arg BUILD_GID=$(id -g) \
      -t "$DOCKER_IMG" \
      -f "$ROOT_DIR/build/docker/Dockerfile.windows-cross" \
      "$ROOT_DIR"
  fi

  DOCKER_ARGS=(--rm -v "$ROOT_DIR:/moltbrowser" -w /moltbrowser)

  if [ "$SKIP_BUILD" = false ]; then
    docker run "${DOCKER_ARGS[@]}" "$DOCKER_IMG" bash -c \
      "./scripts/configure.sh --platform windows --arch x64 && ./scripts/build.sh"
  fi

else
  # --- Native Windows build ---
  if [ "$SKIP_BUILD" = false ]; then
    echo "[1/5] Building..."
    "$SCRIPT_DIR/configure.sh" --platform windows --arch x64
    "$SCRIPT_DIR/build.sh"
  else
    echo "[1/5] Skipping build"
  fi
fi

# --- Package ---
echo ""
echo "[2/5] Packaging..."
"$SCRIPT_DIR/package-win.sh" --version "$VERSION"

# --- Code Sign ---
echo ""
if [ "$DO_SIGN" = true ]; then
  echo "[3/5] Code signing..."

  if [ -z "$SIGN_CERT" ]; then
    SIGN_CERT="$ROOT_DIR/signing/moltbrowser.pfx"
  fi

  if [ ! -f "$SIGN_CERT" ]; then
    echo "ERROR: Certificate not found at $SIGN_CERT"
    echo "Get an EV code signing certificate from DigiCert, Sectigo, etc."
    exit 1
  fi

  # Sign the main executable
  EXE="$DIST_DIR/MoltBrowser-$VERSION-win-x64/MoltBrowser.exe"
  if command -v signtool &>/dev/null; then
    signtool sign /fd SHA256 /f "$SIGN_CERT" /tr http://timestamp.digicert.com /td SHA256 "$EXE"
    echo "  Signed: $EXE"
  elif command -v osslsigncode &>/dev/null; then
    # Cross-platform signing with osslsigncode
    osslsigncode sign -pkcs12 "$SIGN_CERT" \
      -n "MoltBrowser" -i "https://moltbrowser.com" \
      -ts "http://timestamp.digicert.com" \
      -h sha256 \
      -in "$EXE" -out "${EXE}.signed"
    mv "${EXE}.signed" "$EXE"
    echo "  Signed with osslsigncode: $EXE"
  else
    echo "  WARNING: No signing tool found (signtool or osslsigncode)"
  fi

  # Sign the installer if it exists
  INSTALLER="$DIST_DIR/MoltBrowser-$VERSION-win-x64-setup.exe"
  if [ -f "$INSTALLER" ]; then
    if command -v signtool &>/dev/null; then
      signtool sign /fd SHA256 /f "$SIGN_CERT" /tr http://timestamp.digicert.com /td SHA256 "$INSTALLER"
    fi
    echo "  Signed: $INSTALLER"
  fi
else
  echo "[3/5] Skipping code signing"
fi

# --- Build NSIS installer ---
echo ""
echo "[4/5] Building NSIS installer..."
NSIS_SCRIPT="$ROOT_DIR/installer/windows/moltbrowser.nsi"
if command -v makensis &>/dev/null; then
  makensis -DVERSION="$VERSION" "$NSIS_SCRIPT"
  echo "  Installer: $DIST_DIR/MoltBrowser-$VERSION-win-x64-setup.exe"
else
  echo "  NSIS not available — portable zip only"
  echo "  Install NSIS: choco install nsis (Windows) or brew install makensis (macOS)"
fi

# --- Upload ---
echo ""
if [ "$DO_UPLOAD" = true ]; then
  echo "[5/5] Uploading to GitHub Releases..."
  cd "$ROOT_DIR"

  ARTIFACTS=()
  for f in "$DIST_DIR/"*win*; do
    [ -f "$f" ] && ARTIFACTS+=("$f")
  done

  if [ ${#ARTIFACTS[@]} -eq 0 ]; then
    echo "ERROR: No Windows artifacts found"
    exit 1
  fi

  TAG="v${VERSION}"
  if gh release view "$TAG" &>/dev/null; then
    gh release upload "$TAG" "${ARTIFACTS[@]}" --clobber
    echo "  Uploaded to existing release $TAG"
  else
    git tag -f "$TAG" HEAD
    gh release create "$TAG" \
      --title "MoltBrowser v${VERSION}" \
      --notes "Windows release for MoltBrowser v${VERSION}" \
      "${ARTIFACTS[@]}"
  fi
else
  echo "[5/5] Skipping upload"
fi

echo ""
echo "=== Windows Release Complete ==="
echo "Artifacts:"
ls -lh "$DIST_DIR/"*win* 2>/dev/null || echo "  (none)"
