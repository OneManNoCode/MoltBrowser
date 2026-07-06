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
# Write to the appcast that is actually served: website/updates/appcast.xml is
# published to moltbrowser.com/updates/appcast.xml by deploy-website.yml (GitHub
# Pages) on push to main, and is the URL Sparkle fetches (SUFeedURL). The old
# update/appcast.xml target was never deployed.
APPCAST_PATH="$REPO_DIR/website/updates/appcast.xml"

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

# Sign the DMG with the Sparkle EdDSA key. Sparkle 2.6.x `sign_update -p`
# prints just the base64 signature (what the enclosure attribute needs).
# Prefer the bundled sign_update + the exported private-key file (headless,
# no keychain prompt); fall back to the keychain key, then any PATH install.
SPARKLE_VERSION="${SPARKLE_VERSION:-2.6.4}"
SIGN_UPDATE="$REPO_DIR/.sparkle-cache/Sparkle-$SPARKLE_VERSION/bin/sign_update"
KEY_FILE="$REPO_DIR/.sparkle-keys/eddsa_key"

# Only capture stdout on a clean (exit 0) sign — these tools print errors to
# stdout, so a failed sign must not leak its message into the signature field.
ED_SIGNATURE="UNSIGNED"
echo "Signing DMG with Sparkle EdDSA key..."
if [ -x "$SIGN_UPDATE" ]; then
  if [ -f "$KEY_FILE" ]; then
    sig="$("$SIGN_UPDATE" -p --ed-key-file "$KEY_FILE" "$DMG_PATH" 2>/dev/null)" && ED_SIGNATURE="$sig"
  else
    sig="$("$SIGN_UPDATE" -p "$DMG_PATH" 2>/dev/null)" && ED_SIGNATURE="$sig"
  fi
elif command -v sign_update &> /dev/null; then
  sig="$(sign_update -p "$DMG_PATH" 2>/dev/null)" && ED_SIGNATURE="$sig"
fi

if [ -z "$ED_SIGNATURE" ] || [ "$ED_SIGNATURE" = "UNSIGNED" ]; then
  ED_SIGNATURE="UNSIGNED"
  echo "WARNING: DMG not signed — Sparkle EdDSA key unavailable."
  echo "  Run scripts/bundle-sparkle.sh first to create/look up the key."
  echo "  See: https://sparkle-project.org/documentation/eddsa-setup/"
else
  echo "  EdDSA signature: $ED_SIGNATURE"
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

# Insert the new item right after the <language> line. Use awk (portable across
# BSD/macOS + GNU) reading the item from a temp file — BSD sed's `a\` append
# mangles multi-line content ("sed: invalid command code").
if [ -f "$APPCAST_PATH" ]; then
  ITEM_TMP="$(mktemp)"
  printf '%s\n' "$NEW_ITEM" > "$ITEM_TMP"
  awk -v itemfile="$ITEM_TMP" '
    { print }
    /<language>en<\/language>/ {
      while ((getline line < itemfile) > 0) print line
      close(itemfile)
    }
  ' "$APPCAST_PATH" > "$APPCAST_PATH.tmp" && mv "$APPCAST_PATH.tmp" "$APPCAST_PATH"
  rm -f "$ITEM_TMP"
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
