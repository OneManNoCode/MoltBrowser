#!/bin/bash
# MoltBrowser Windows Packaging Script
# Copyright 2025 GenEye AI Labs Inc.
#
# Creates Windows distribution packages:
#   1. NSIS installer (.exe)
#   2. Portable zip archive
#
# Prerequisites:
#   - NSIS (Nullsoft Scriptable Install System) for installer
#   - Built MoltBrowser in out/MoltBrowser
#
# Usage:
#   ./scripts/package-win.sh [--version VERSION] [--skip-installer]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_SRC="$ROOT_DIR/chromium/src"
BUILD_DIR="$CHROMIUM_SRC/out/MoltBrowser"
DIST_DIR="$ROOT_DIR/dist"
VERSION="0.1.0-alpha"
SKIP_INSTALLER=false

while [[ $# -gt 0 ]]; do
  case $1 in
    --version) VERSION="$2"; shift 2 ;;
    --skip-installer) SKIP_INSTALLER=true; shift ;;
    --help|-h)
      echo "Usage: $0 [--version VERSION] [--skip-installer]"
      exit 0
      ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

echo "=== MoltBrowser Windows Packaging ==="
echo "Version: $VERSION"

# Verify build exists
CHROME_EXE="$BUILD_DIR/chrome.exe"
if [ ! -f "$CHROME_EXE" ]; then
  echo "ERROR: Build not found at $CHROME_EXE"
  echo "Run ./scripts/build.sh first."
  exit 1
fi

mkdir -p "$DIST_DIR"

# --- Stage 1: Create portable zip ---
echo ""
echo "--- Creating portable zip ---"

STAGE_DIR="$DIST_DIR/MoltBrowser-$VERSION-win-x64"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

# Copy Chrome build output (main binary + required DLLs and resources)
echo "Copying build artifacts..."
cp "$BUILD_DIR/chrome.exe" "$STAGE_DIR/MoltBrowser.exe"
cp "$BUILD_DIR/"*.dll "$STAGE_DIR/" 2>/dev/null || true
cp "$BUILD_DIR/"*.pak "$STAGE_DIR/" 2>/dev/null || true
cp "$BUILD_DIR/icudtl.dat" "$STAGE_DIR/" 2>/dev/null || true
cp "$BUILD_DIR/v8_context_snapshot.bin" "$STAGE_DIR/" 2>/dev/null || true
cp -r "$BUILD_DIR/locales" "$STAGE_DIR/" 2>/dev/null || true
cp -r "$BUILD_DIR/resources" "$STAGE_DIR/" 2>/dev/null || true

# Copy WinSparkle DLL if available
if [ -f "$ROOT_DIR/third_party/winsparkle/WinSparkle.dll" ]; then
  cp "$ROOT_DIR/third_party/winsparkle/WinSparkle.dll" "$STAGE_DIR/"
fi

# Create portable zip
ZIP_NAME="MoltBrowser-$VERSION-win-x64-portable.zip"
echo "Creating $ZIP_NAME..."
cd "$DIST_DIR"
zip -r -9 "$ZIP_NAME" "$(basename "$STAGE_DIR")"
echo "Portable zip: $DIST_DIR/$ZIP_NAME ($(du -sh "$ZIP_NAME" | cut -f1))"

# --- Stage 2: NSIS Installer ---
if [ "$SKIP_INSTALLER" = false ]; then
  echo ""
  echo "--- Creating NSIS installer ---"

  NSIS_SCRIPT="$ROOT_DIR/installer/windows/moltbrowser.nsi"
  if [ ! -f "$NSIS_SCRIPT" ]; then
    echo "Creating NSIS script template..."
    mkdir -p "$ROOT_DIR/installer/windows"
    cat > "$NSIS_SCRIPT" << 'NSIEOF'
; MoltBrowser NSIS Installer Script
; Copyright 2025 GenEye AI Labs Inc.

!include "MUI2.nsh"

Name "MoltBrowser"
OutFile "..\..\dist\MoltBrowser-${VERSION}-win-x64-setup.exe"
InstallDir "$PROGRAMFILES64\MoltBrowser"
RequestExecutionLevel admin

; UI
!define MUI_ICON "..\..\branding\icon.ico"
!define MUI_UNICON "..\..\branding\icon.ico"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath $INSTDIR
  File /r "..\..\dist\MoltBrowser-${VERSION}-win-x64\*.*"

  ; Create shortcuts
  CreateDirectory "$SMPROGRAMS\MoltBrowser"
  CreateShortcut "$SMPROGRAMS\MoltBrowser\MoltBrowser.lnk" "$INSTDIR\MoltBrowser.exe"
  CreateShortcut "$DESKTOP\MoltBrowser.lnk" "$INSTDIR\MoltBrowser.exe"

  ; Uninstaller
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Registry for Add/Remove Programs
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser" \
    "DisplayName" "MoltBrowser"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser" \
    "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser" \
    "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser" \
    "Publisher" "GenEye AI Labs Inc."
SectionEnd

Section "Uninstall"
  RMDir /r "$INSTDIR"
  RMDir /r "$SMPROGRAMS\MoltBrowser"
  Delete "$DESKTOP\MoltBrowser.lnk"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser"
SectionEnd
NSIEOF
    echo "NSIS template created at $NSIS_SCRIPT"
  fi

  # Check for makensis
  if command -v makensis &>/dev/null; then
    makensis -DVERSION="$VERSION" "$NSIS_SCRIPT"
    echo "Installer created!"
  else
    echo "NSIS not installed. Install with: choco install nsis"
    echo "NSIS script ready at: $NSIS_SCRIPT"
  fi
fi

# Clean up staging directory
rm -rf "$STAGE_DIR"

echo ""
echo "=== Windows Packaging Complete ==="
echo "Artifacts in: $DIST_DIR/"
ls -la "$DIST_DIR/"MoltBrowser-*win* 2>/dev/null || true
