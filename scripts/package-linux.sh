#!/bin/bash
# MoltBrowser Linux Packaging Script
# Copyright 2025 GenEye AI Labs Inc.
#
# Creates Linux distribution packages:
#   1. .tar.gz archive (universal)
#   2. .deb package (Debian/Ubuntu)
#   3. .rpm package (Fedora/RHEL)
#   4. AppImage (portable)
#   5. Flatpak (sandboxed)
#
# Prerequisites:
#   - Built MoltBrowser in out/MoltBrowser
#   - dpkg-deb for .deb (apt install dpkg)
#   - rpmbuild for .rpm (dnf install rpm-build)
#   - appimagetool for AppImage
#   - flatpak-builder for Flatpak
#
# Usage:
#   ./scripts/package-linux.sh [--version VERSION] [--format FORMAT]
#   FORMAT: all, tar, deb, rpm, appimage, flatpak

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_SRC="$ROOT_DIR/chromium/src"
BUILD_DIR="$CHROMIUM_SRC/out/MoltBrowser"
DIST_DIR="$ROOT_DIR/dist"
VERSION="0.1.0-alpha"
FORMAT="all"

while [[ $# -gt 0 ]]; do
  case $1 in
    --version) VERSION="$2"; shift 2 ;;
    --format) FORMAT="$2"; shift 2 ;;
    --help|-h)
      echo "Usage: $0 [--version VERSION] [--format FORMAT]"
      echo "Formats: all, tar, deb, rpm, appimage, flatpak"
      exit 0
      ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

# Debian AND RPM versions must start with a digit. Callers often pass the git
# tag verbatim (e.g. "v0.2.5"); dpkg-deb then rejects `Version: v0.2.5` with
# "control ... near line 2 ... bad syntax". Strip a single leading "v".
VERSION="${VERSION#v}"

echo "=== MoltBrowser Linux Packaging ==="
echo "Version: $VERSION"
echo "Format: $FORMAT"

# Verify build exists
CHROME_BIN="$BUILD_DIR/chrome"
if [ ! -f "$CHROME_BIN" ]; then
  echo "ERROR: Build not found at $CHROME_BIN"
  echo "Run ./scripts/build.sh first."
  exit 1
fi

mkdir -p "$DIST_DIR"

# On a warm/reused workspace (self-hosted CI), Linux artifacts from prior runs
# linger in dist/ — e.g. an old "MoltBrowser-v0.2.5-linux-x64.tar.gz" from before
# the version was normalized. The workflow's rename glob then matches >1 file and
# `mv` fails, so nothing uploads. Wipe our own Linux outputs first (versioned +
# stable-named). Non-Linux artifacts (e.g. the macOS .dmg) are left untouched.
rm -f "$DIST_DIR"/MoltBrowser-*linux-x64.* \
      "$DIST_DIR"/MoltBrowser-Linux-x64.* \
      "$DIST_DIR"/moltbrowser*.deb \
      "$DIST_DIR"/moltbrowser*.rpm 2>/dev/null || true

# --- Common: Stage files ---
STAGE_DIR="$DIST_DIR/.stage-linux"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/opt/moltbrowser"

echo "Staging build artifacts..."
cp "$BUILD_DIR/chrome" "$STAGE_DIR/opt/moltbrowser/moltbrowser"
cp "$BUILD_DIR/"*.so "$STAGE_DIR/opt/moltbrowser/" 2>/dev/null || true
cp "$BUILD_DIR/"*.pak "$STAGE_DIR/opt/moltbrowser/" 2>/dev/null || true
cp "$BUILD_DIR/icudtl.dat" "$STAGE_DIR/opt/moltbrowser/" 2>/dev/null || true
cp "$BUILD_DIR/v8_context_snapshot.bin" "$STAGE_DIR/opt/moltbrowser/" 2>/dev/null || true
cp -r "$BUILD_DIR/locales" "$STAGE_DIR/opt/moltbrowser/" 2>/dev/null || true
cp -r "$BUILD_DIR/resources" "$STAGE_DIR/opt/moltbrowser/" 2>/dev/null || true

# Desktop entry
mkdir -p "$STAGE_DIR/usr/share/applications"
cat > "$STAGE_DIR/usr/share/applications/moltbrowser.desktop" << EOF
[Desktop Entry]
Name=MoltBrowser
Comment=AI-Native Privacy Browser
Exec=/opt/moltbrowser/moltbrowser %U
Icon=moltbrowser
Type=Application
Categories=Network;WebBrowser;
MimeType=text/html;text/xml;application/xhtml+xml;x-scheme-handler/http;x-scheme-handler/https;
StartupNotify=true
StartupWMClass=MoltBrowser
EOF

# Icon
mkdir -p "$STAGE_DIR/usr/share/icons/hicolor/256x256/apps"
if [ -f "$ROOT_DIR/branding/MoltBrowser.iconset/icon_256x256.png" ]; then
  cp "$ROOT_DIR/branding/MoltBrowser.iconset/icon_256x256.png" \
    "$STAGE_DIR/usr/share/icons/hicolor/256x256/apps/moltbrowser.png"
fi

# Symlink for PATH
mkdir -p "$STAGE_DIR/usr/bin"
ln -sf /opt/moltbrowser/moltbrowser "$STAGE_DIR/usr/bin/moltbrowser"

# --- Format: tar.gz ---
build_tar() {
  echo ""
  echo "--- Creating tar.gz ---"
  TAR_NAME="MoltBrowser-$VERSION-linux-x64.tar.gz"
  cd "$STAGE_DIR"
  tar czf "$DIST_DIR/$TAR_NAME" opt/ usr/
  echo "Tarball: $DIST_DIR/$TAR_NAME ($(du -sh "$DIST_DIR/$TAR_NAME" | cut -f1))"
}

# --- Format: .deb ---
build_deb() {
  echo ""
  echo "--- Creating .deb package ---"

  if ! command -v dpkg-deb &>/dev/null; then
    echo "SKIP: dpkg-deb not found (install dpkg)"
    return
  fi

  DEB_DIR="$DIST_DIR/.deb-build"
  rm -rf "$DEB_DIR"
  mkdir -p "$DEB_DIR/DEBIAN"

  # Copy staged files
  cp -r "$STAGE_DIR/"* "$DEB_DIR/"

  # Control file
  cat > "$DEB_DIR/DEBIAN/control" << EOF
Package: moltbrowser
Version: $VERSION
Section: web
Priority: optional
Architecture: amd64
Depends: libnss3 (>= 3.26), libatk1.0-0 (>= 1.12.4), libatk-bridge2.0-0 (>= 2.5.3), libcups2 (>= 1.6.0), libdrm2 (>= 2.4.38), libgtk-3-0 (>= 3.9.10), libxkbcommon0 (>= 0.4.1), libxcomposite1 (>= 1:0.3-1), libxdamage1 (>= 1:1.1), libxrandr2 (>= 2:1.2.99.3), libgbm1 (>= 8.1~0), libpango-1.0-0 (>= 1.14.0), libasound2 (>= 1.0.17)
Maintainer: GenEye AI Labs Inc. <support@geneye.ai>
Description: MoltBrowser — AI-Native Privacy Browser
 MoltBrowser is a privacy-first browser with built-in AI capabilities:
 on-device LLM inference, MoltShield ad/tracker blocking, an AI chat
 sidebar, MoltNet privacy routing, and support for standard web extensions.
Homepage: https://moltbrowser.com
EOF

  # Post-install: update icon cache + desktop database
  cat > "$DEB_DIR/DEBIAN/postinst" << 'EOF'
#!/bin/sh
set -e
update-desktop-database /usr/share/applications 2>/dev/null || true
gtk-update-icon-cache /usr/share/icons/hicolor 2>/dev/null || true
EOF
  chmod 755 "$DEB_DIR/DEBIAN/postinst"

  DEB_NAME="moltbrowser_${VERSION}_amd64.deb"
  dpkg-deb --build "$DEB_DIR" "$DIST_DIR/$DEB_NAME"
  echo "Deb: $DIST_DIR/$DEB_NAME ($(du -sh "$DIST_DIR/$DEB_NAME" | cut -f1))"
  rm -rf "$DEB_DIR"
}

# --- Format: .rpm ---
build_rpm() {
  echo ""
  echo "--- Creating .rpm package ---"

  if ! command -v rpmbuild &>/dev/null; then
    echo "SKIP: rpmbuild not found (install rpm-build)"
    return
  fi

  RPM_ROOT="$DIST_DIR/.rpm-build"
  rm -rf "$RPM_ROOT"
  mkdir -p "$RPM_ROOT"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

  # Create tarball for rpmbuild
  TAR_SRC="$RPM_ROOT/SOURCES/moltbrowser-$VERSION.tar.gz"
  cd "$STAGE_DIR"
  tar czf "$TAR_SRC" opt/ usr/

  cat > "$RPM_ROOT/SPECS/moltbrowser.spec" << EOF
Name:           moltbrowser
Version:        $(echo "$VERSION" | tr '-' '.')
Release:        1%{?dist}
Summary:        AI-Native Privacy Browser
License:        GPLv3
URL:            https://moltbrowser.com
Source0:        moltbrowser-$VERSION.tar.gz

Requires:       nss >= 3.26, atk >= 1.12.4, at-spi2-atk >= 2.5.3, cups-libs >= 1.6.0, libdrm >= 2.4.38, gtk3 >= 3.9.10, libxkbcommon >= 0.4.1, alsa-lib >= 1.0.17

%description
MoltBrowser is a privacy-first browser with built-in AI capabilities,
on-device LLM inference, and privacy-first design.

%install
cd %{_sourcedir}
tar xzf moltbrowser-$VERSION.tar.gz -C %{buildroot}

%files
/opt/moltbrowser/
/usr/bin/moltbrowser
/usr/share/applications/moltbrowser.desktop
/usr/share/icons/hicolor/256x256/apps/moltbrowser.png
EOF

  rpmbuild --define "_topdir $RPM_ROOT" -bb "$RPM_ROOT/SPECS/moltbrowser.spec"
  cp "$RPM_ROOT/RPMS/"*/*.rpm "$DIST_DIR/" 2>/dev/null || true
  echo "RPM built!"
  rm -rf "$RPM_ROOT"
}

# --- Format: AppImage ---
build_appimage() {
  echo ""
  echo "--- Creating AppImage ---"

  if ! command -v appimagetool &>/dev/null; then
    echo "SKIP: appimagetool not found"
    echo "  Download: https://github.com/AppImage/AppImageKit/releases"
    return
  fi

  APPDIR="$DIST_DIR/.AppDir"
  rm -rf "$APPDIR"
  mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons"

  cp -r "$STAGE_DIR/opt/moltbrowser/"* "$APPDIR/usr/bin/"
  cp "$STAGE_DIR/usr/share/applications/moltbrowser.desktop" "$APPDIR/"
  if [ -f "$STAGE_DIR/usr/share/icons/hicolor/256x256/apps/moltbrowser.png" ]; then
    cp "$STAGE_DIR/usr/share/icons/hicolor/256x256/apps/moltbrowser.png" "$APPDIR/"
  fi

  # AppRun script
  cat > "$APPDIR/AppRun" << 'EOF'
#!/bin/bash
SELF=$(readlink -f "$0")
HERE=${SELF%/*}
exec "$HERE/usr/bin/moltbrowser" "$@"
EOF
  chmod +x "$APPDIR/AppRun"

  APPIMAGE_NAME="MoltBrowser-$VERSION-linux-x64.AppImage"
  appimagetool "$APPDIR" "$DIST_DIR/$APPIMAGE_NAME"
  echo "AppImage: $DIST_DIR/$APPIMAGE_NAME"
  rm -rf "$APPDIR"
}

# --- Format: Flatpak manifest ---
build_flatpak() {
  echo ""
  echo "--- Creating Flatpak manifest ---"

  FLATPAK_DIR="$ROOT_DIR/installer/linux/flatpak"
  mkdir -p "$FLATPAK_DIR"

  cat > "$FLATPAK_DIR/com.geneye.MoltBrowser.yml" << EOF
app-id: com.geneye.MoltBrowser
runtime: org.freedesktop.Platform
runtime-version: '23.08'
sdk: org.freedesktop.Sdk
command: moltbrowser
finish-args:
  - --share=ipc
  - --share=network
  - --socket=x11
  - --socket=wayland
  - --socket=pulseaudio
  - --device=dri
  - --filesystem=home
  - --talk-name=org.freedesktop.Notifications
modules:
  - name: moltbrowser
    buildsystem: simple
    build-commands:
      - install -Dm755 moltbrowser /app/bin/moltbrowser
      - cp -r *.pak locales resources /app/bin/ 2>/dev/null || true
      - cp icudtl.dat v8_context_snapshot.bin /app/bin/ 2>/dev/null || true
      - install -Dm644 moltbrowser.desktop /app/share/applications/com.geneye.MoltBrowser.desktop
      - install -Dm644 moltbrowser.png /app/share/icons/hicolor/256x256/apps/com.geneye.MoltBrowser.png
    sources:
      - type: archive
        url: https://github.com/OneManNoCode/MoltBrowser/releases/download/v${VERSION}/MoltBrowser-${VERSION}-linux-x64.tar.gz
        sha256: FIXME_REPLACE_WITH_ACTUAL_SHA256
EOF

  echo "Flatpak manifest: $FLATPAK_DIR/com.geneye.MoltBrowser.yml"
  echo "Build with: flatpak-builder --force-clean build-dir $FLATPAK_DIR/com.geneye.MoltBrowser.yml"
}

# Execute selected formats
case "$FORMAT" in
  all)
    build_tar
    build_deb
    build_rpm
    build_appimage
    build_flatpak
    ;;
  tar) build_tar ;;
  deb) build_deb ;;
  rpm) build_rpm ;;
  appimage) build_appimage ;;
  flatpak) build_flatpak ;;
  *)
    echo "Unknown format: $FORMAT"
    exit 1
    ;;
esac

# Clean up
rm -rf "$STAGE_DIR"

echo ""
echo "=== Linux Packaging Complete ==="
echo "Artifacts in: $DIST_DIR/"
ls -la "$DIST_DIR/"*linux* "$DIST_DIR/"moltbrowser_* 2>/dev/null || true
