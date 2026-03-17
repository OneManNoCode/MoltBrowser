#!/bin/bash
# Generate or update the Sparkle appcast.xml for a new release.
#
# Usage: ./scripts/generate-appcast.sh --version 0.1.1 --build 2 --dmg dist/MoltBrowser-*.dmg
#
# Requires: Sparkle's generate_keys / sign_update tools for EdDSA signatures
#   brew install --cask sparkle
#   OR download from https://github.com/sparkle-project/Sparkle/releases
#
# Copyright 2025 GenEye AI Labs Inc. Licensed under GPLv3.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
APPCAST_PATH="$REPO_DIR/update/appcast.xml"

# Defaults
VERSION=""
BUILD_NUM=""
DMG_PATH=""
DOWNLOAD_BASE="https://github.com/OneManNoCode/MoltBrowser/releases/download"
RELEASE_NOTES_BASE="https://moltbrowser.com/updates/release-notes"
MIN_OS="12.0"

while [[ $# -gt 0 ]]; do
  case $1 in
    --version) VERSION="$2"; shift 2 ;;
    --build) BUILD_NUM="$2"; shift 2 ;;
    --dmg) DMG_PATH="$2"; shift 2 ;;
    --download-base) DOWNLOAD_BASE="$2"; shift 2 ;;
    --help|-h)
      echo "Usage: $0 --version VERSION --build BUILD_NUM --dmg DMG_PATH"
      exit 0
      ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

if [ -z "$VERSION" ] || [ -z "$BUILD_NUM" ] || [ -z "$DMG_PATH" ]; then
  echo "ERROR: --version, --build, and --dmg are required"
  echo "Example: $0 --version 0.1.1 --build 2 --dmg dist/MoltBrowser-0.1.1-macOS-arm64.dmg"
  exit 1
fi

if [ ! -f "$DMG_PATH" ]; then
  echo "ERROR: DMG not found: $DMG_PATH"
  exit 1
fi

echo "=== MoltBrowser Appcast Generator ==="
echo "Version: $VERSION (build $BUILD_NUM)"
echo "DMG: $DMG_PATH"

# Get file size
FILE_SIZE=$(stat -f%z "$DMG_PATH")
DMG_FILENAME=$(basename "$DMG_PATH")
DOWNLOAD_URL="$DOWNLOAD_BASE/v${VERSION}/${DMG_FILENAME}"
PUB_DATE=$(date -u +"%a, %d %b %Y %H:%M:%S +0000")

echo "Size: $FILE_SIZE bytes"
echo "URL: $DOWNLOAD_URL"

# Try to sign with Sparkle's EdDSA key
ED_SIGNATURE="UNSIGNED"
if command -v sign_update &> /dev/null; then
  echo "Signing DMG with Sparkle EdDSA key..."
  ED_SIGNATURE=$(sign_update "$DMG_PATH" 2>/dev/null || echo "UNSIGNED")
  if [ "$ED_SIGNATURE" != "UNSIGNED" ]; then
    echo "  EdDSA signature generated"
  fi
elif [ -f "$HOME/Library/Sparkle/ed25519/sign_update" ]; then
  ED_SIGNATURE=$("$HOME/Library/Sparkle/ed25519/sign_update" "$DMG_PATH" 2>/dev/null || echo "UNSIGNED")
fi

if [ "$ED_SIGNATURE" = "UNSIGNED" ]; then
  echo "WARNING: DMG not signed. Run 'generate_keys' first to create EdDSA keys."
  echo "  See: https://sparkle-project.org/documentation/eddsa-setup/"
fi

# Generate the new item XML
NEW_ITEM=$(cat <<XMLEOF

    <item>
      <title>MoltBrowser ${VERSION}</title>
      <sparkle:releaseNotesLink>${RELEASE_NOTES_BASE}/${VERSION}.html</sparkle:releaseNotesLink>
      <pubDate>${PUB_DATE}</pubDate>
      <enclosure
        url="${DOWNLOAD_URL}"
        sparkle:version="${BUILD_NUM}"
        sparkle:shortVersionString="${VERSION}"
        length="${FILE_SIZE}"
        type="application/octet-stream"
        sparkle:edSignature="${ED_SIGNATURE}"
      />
      <sparkle:minimumSystemVersion>${MIN_OS}</sparkle:minimumSystemVersion>
    </item>
XMLEOF
)

# Insert the new item at the top of the channel (after <language>)
if [ -f "$APPCAST_PATH" ]; then
  # Insert after the <language> line
  sed -i.bak "/<language>en<\/language>/a\\
${NEW_ITEM}
" "$APPCAST_PATH"
  rm -f "${APPCAST_PATH}.bak"
  echo "Updated: $APPCAST_PATH"
else
  # Create new appcast
  cat > "$APPCAST_PATH" <<FEEDEOF
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle" xmlns:dc="http://purl.org/dc/elements/1.1/">
  <channel>
    <title>MoltBrowser Updates</title>
    <link>https://moltbrowser.com/updates/appcast.xml</link>
    <description>MoltBrowser — AI-Native Browser with Local LLM</description>
    <language>en</language>
${NEW_ITEM}

  </channel>
</rss>
FEEDEOF
  echo "Created: $APPCAST_PATH"
fi

echo ""
echo "Next steps:"
echo "  1. Upload DMG to: $DOWNLOAD_URL"
echo "  2. Upload appcast.xml to: https://moltbrowser.com/updates/appcast.xml"
echo "  3. Create release notes at: ${RELEASE_NOTES_BASE}/${VERSION}.html"
echo ""
echo "Done!"
