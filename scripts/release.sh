#!/bin/bash
# MoltBrowser Full Release Pipeline
# Build → Patch → Sign → Package → Notarize → Upload
#
# Usage:
#   # Alpha (unsigned, no notarization)
#   ./scripts/release.sh --version 0.1.0-alpha --build 1
#
#   # Production (signed + notarized)
#   ./scripts/release.sh --version 0.1.0 --build 1 \
#     --identity "Developer ID Application: GenEye AI Labs Inc (TEAM_ID)" \
#     --team-id TEAM_ID \
#     --notarize
#
#   # Full release with GitHub upload
#   ./scripts/release.sh --version 0.1.0 --build 1 \
#     --identity "..." --team-id TEAM_ID --notarize --upload
#
#   # Universal binary (arm64 + x64)
#   ./scripts/release.sh --version 0.1.0 --build 1 --universal \
#     --identity "..." --notarize --upload
#
# Copyright 2025 GenEye AI Labs Inc. Licensed under GPLv3.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

# Defaults
VERSION=""
BUILD_NUM=""
SIGN_IDENTITY=""
TEAM_ID=""
DO_NOTARIZE=false
DO_UPLOAD=false
SKIP_BUILD=false
UNIVERSAL=false
BUNDLE_SPARKLE=true

while [[ $# -gt 0 ]]; do
  case $1 in
    --version) VERSION="$2"; shift 2 ;;
    --build) BUILD_NUM="$2"; shift 2 ;;
    --identity) SIGN_IDENTITY="$2"; shift 2 ;;
    --team-id) TEAM_ID="$2"; shift 2 ;;
    --notarize) DO_NOTARIZE=true; shift ;;
    --upload) DO_UPLOAD=true; shift ;;
    --skip-build) SKIP_BUILD=true; shift ;;
    --universal) UNIVERSAL=true; shift ;;
    --no-sparkle) BUNDLE_SPARKLE=false; shift ;;
    --help|-h)
      echo "MoltBrowser Release Pipeline"
      echo ""
      echo "Usage: $0 --version VERSION --build BUILD_NUM [options]"
      echo ""
      echo "Options:"
      echo "  --version      Version string (e.g., 0.1.0)"
      echo "  --build        Build number (integer)"
      echo "  --identity     Code signing identity"
      echo "  --team-id      Apple Developer Team ID"
      echo "  --notarize     Submit for Apple notarization"
      echo "  --upload       Upload to GitHub Releases"
      echo "  --skip-build   Skip the Chromium build step"
      echo "  --universal    Build universal binary (arm64 + x64)"
      echo "  --no-sparkle   Skip bundling Sparkle.framework"
      exit 0
      ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

if [ -z "$VERSION" ] || [ -z "$BUILD_NUM" ]; then
  echo "ERROR: --version and --build are required"
  echo "Run: $0 --help"
  exit 1
fi

if [ "$UNIVERSAL" = true ]; then
  DMG_NAME="MoltBrowser-${VERSION}-macOS-universal"
  BUILD_DIR="$REPO_DIR/chromium/src/out/MoltBrowser-universal"
else
  DMG_NAME="MoltBrowser-${VERSION}-macOS-arm64"
  BUILD_DIR="$REPO_DIR/chromium/src/out/MoltBrowser"
fi
DMG_PATH="$REPO_DIR/dist/${DMG_NAME}.dmg"
APP_PATH="$BUILD_DIR/MoltBrowser.app"

echo "╔══════════════════════════════════════════╗"
echo "║    MoltBrowser Release Pipeline          ║"
echo "╠══════════════════════════════════════════╣"
echo "║  Version:    $VERSION"
echo "║  Build:      $BUILD_NUM"
echo "║  Universal:  $UNIVERSAL"
echo "║  Sparkle:    $BUNDLE_SPARKLE"
echo "║  Signed:     $([ -n "$SIGN_IDENTITY" ] && echo "Yes" || echo "No")"
echo "║  Notarize:   $DO_NOTARIZE"
echo "║  Upload:     $DO_UPLOAD"
echo "╚══════════════════════════════════════════╝"
echo ""

STEPS_TOTAL=9
STEP=0

# ---- Step 1: Build ----
STEP=$((STEP + 1))
if [ "$SKIP_BUILD" = false ]; then
  export PATH="$REPO_DIR/depot_tools:/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:$PATH"

  if [ "$UNIVERSAL" = true ]; then
    echo "[$STEP/$STEPS_TOTAL] Building Universal Binary (arm64 + x64)..."
    bash "$SCRIPT_DIR/build-universal.sh" --version "$VERSION" --skip-build false \
      ${SIGN_IDENTITY:+--sign "$SIGN_IDENTITY"}
  else
    echo "[$STEP/$STEPS_TOTAL] Building MoltBrowser (arm64)..."

    # Copy source files to Chromium tree
    if [ -d "$REPO_DIR/src/chrome/browser/ui/webui/molt_ai" ]; then
      cp "$REPO_DIR"/src/chrome/browser/ui/webui/molt_ai/*.cc \
         "$REPO_DIR/chromium/src/chrome/browser/ui/webui/molt_ai/" 2>/dev/null || true
      cp "$REPO_DIR"/src/chrome/browser/ui/webui/molt_ai/*.h \
         "$REPO_DIR/chromium/src/chrome/browser/ui/webui/molt_ai/" 2>/dev/null || true
    fi

    # Apply icon
    if [ -f "$REPO_DIR/branding/MoltBrowser.icns" ]; then
      cp "$REPO_DIR/branding/MoltBrowser.icns" \
         "$REPO_DIR/chromium/src/chrome/app/theme/chromium/mac/app.icns"
    fi

    autoninja -C "$BUILD_DIR" chrome
  fi
  echo "  Build complete"
else
  echo "[$STEP/$STEPS_TOTAL] Skipping build (--skip-build)"
fi

# ---- Step 2: Patch Info.plist ----
STEP=$((STEP + 1))
echo ""
echo "[$STEP/$STEPS_TOTAL] Patching Info.plist..."
bash "$SCRIPT_DIR/patch-info-plist.sh" "$VERSION" "$BUILD_NUM"

# ---- Step 3: Bundle Sparkle.framework ----
STEP=$((STEP + 1))
echo ""
if [ "$BUNDLE_SPARKLE" = true ]; then
  echo "[$STEP/$STEPS_TOTAL] Bundling Sparkle.framework..."
  bash "$SCRIPT_DIR/bundle-sparkle.sh" --app "$APP_PATH"
else
  echo "[$STEP/$STEPS_TOTAL] Skipping Sparkle bundling (--no-sparkle)"
fi

# ---- Step 4: Metal shader library (default.metallib) ----
# llama.cpp's Metal backend looks up default.metallib in the framework's
# Resources/ at runtime (ggml-metal-device.m pathForResource). No GN rule
# produces it — a fresh out dir ships without it and Metal falls back to
# JIT-compiling ggml-metal.metal, which fails under the hardened runtime.
# v0.2.2 shipped with model loading broken on macOS because this file was
# missing. Compile it here, BEFORE code signing, so the framework seal
# covers it (dropping it in after signing invalidates the signature).
STEP=$((STEP + 1))
echo ""
FRAMEWORK="$APP_PATH/Contents/Frameworks/MoltBrowser Framework.framework"
# Resources/ is a symlink into the versioned dir — resolve it the same way
# the helper-signing step resolves paths inside the framework, so we write
# into Versions/<ver>/Resources and not through a possibly-dangling symlink.
FRAMEWORK_RES="$FRAMEWORK/Versions/Current/Resources"
if [ ! -d "$FRAMEWORK_RES" ]; then
  FRAMEWORK_RES=$(find "$FRAMEWORK/Versions" -mindepth 2 -maxdepth 2 -type d -name "Resources" 2>/dev/null | head -1 || true)
fi
if [ -f "$FRAMEWORK_RES/default.metallib" ]; then
  echo "[$STEP/$STEPS_TOTAL] Metal shader library already present, skipping compile"
else
  echo "[$STEP/$STEPS_TOTAL] Compiling Metal shader library (default.metallib)..."
  if [ -z "$FRAMEWORK_RES" ] || [ ! -d "$FRAMEWORK_RES" ]; then
    echo ""
    echo "  ❌ FATAL: cannot locate framework Resources dir under:"
    echo "     $FRAMEWORK/Versions"
    echo "     Cannot install default.metallib — DO NOT SHIP THIS BUILD."
    exit 1
  fi
  GGML_SRC="$REPO_DIR/src/third_party/llama_cpp/ggml/src"
  METAL_WORKDIR=$(mktemp -d)
  if (cp "$GGML_SRC/ggml-metal/ggml-metal.metal" \
         "$GGML_SRC/ggml-common.h" \
         "$GGML_SRC/ggml-metal/ggml-metal-impl.h" \
         "$METAL_WORKDIR/" \
      && cd "$METAL_WORKDIR" \
      && xcrun -sdk macosx metal -O2 -fno-fast-math -I . -c ggml-metal.metal -o ggml-metal.air \
      && xcrun -sdk macosx metallib ggml-metal.air -o default.metallib); then
    cp "$METAL_WORKDIR/default.metallib" "$FRAMEWORK_RES/default.metallib"
    rm -rf "$METAL_WORKDIR"
    echo "  default.metallib installed: $FRAMEWORK_RES/default.metallib"
  else
    rm -rf "$METAL_WORKDIR"
    echo ""
    echo "  ╔══════════════════════════════════════════════════════════════════╗"
    echo "  ║  ❌ FATAL: Metal shader compilation FAILED                        ║"
    echo "  ║                                                                  ║"
    echo "  ║  v0.2.2 shipped with model loading broken on macOS because this ║"
    echo "  ║  file was missing — do not ship without it.                     ║"
    echo "  ║                                                                  ║"
    echo "  ║  Ensure the Metal toolchain is installed:                        ║"
    echo "  ║    xcodebuild -downloadComponent MetalToolchain                  ║"
    echo "  ╚══════════════════════════════════════════════════════════════════╝"
    exit 1
  fi
fi

# ---- Step 5: Code Sign ----
STEP=$((STEP + 1))
echo ""
if [ -n "$SIGN_IDENTITY" ]; then
  echo "[$STEP/$STEPS_TOTAL] Code signing..."

  # --- Inside-out code signing with per-process-type entitlements ---
  # CRITICAL: the Renderer/GPU helpers MUST be signed with allow-jit
  # (helper-renderer/gpu-entitlements.plist) or V8 cannot reserve its JIT
  # CodeRange under the hardened runtime -> every renderer is KILLED on launch
  # ("V8 process OOM (Failed to reserve virtual memory for CodeRange)"),
  # surfacing as "Can't open this page / Error code 5" on every URL and a blank
  # AI-chat/WebUI. Two prior bugs caused this: (a) the helper glob below pointed
  # at Contents/Frameworks/ but the helpers live INSIDE the framework
  # (Framework.framework/Versions/X/Helpers/), so it matched nothing; (b) the
  # final `codesign --deep` on the app re-signed the nested helpers with the
  # OUTER (empty) entitlements, stripping allow-jit. Fix: locate the real
  # helpers, sign each with its entitlements, and NEVER --deep the outer app.
  ENT_DIR="$SCRIPT_DIR/../chromium/src/chrome/app"

  # 1. Base pass: --deep sign EVERYTHING (all nested Mach-O — dylibs AND the
  # BARE Helper executables that sit directly in Helpers/ and are NOT .app
  # bundles: chrome_crashpad_handler, app_mode_loader, web_app_shortcut_copier).
  # Signing only *.dylib here missed those bare executables -> notarization
  # Invalid ("not signed / no hardened runtime / no secure timestamp"). The
  # per-helper entitlement re-sign in step 3 overwrites the .app helpers
  # (restoring allow-jit), and the framework + app are resealed WITHOUT --deep in
  # steps 4-5 so those entitlements survive.
  codesign --force --deep --sign "$SIGN_IDENTITY" --options runtime --timestamp "$APP_PATH"

  # 2. Bundled third-party binaries in Resources/ (tor, tesseract/OCR, whisper).
  # codesign --deep treats Resources/ as DATA and skips them; sign each Mach-O
  # explicitly (before the app seal), same Developer ID/team.
  for d in tor ocr whisper; do
    if [ -d "$APP_PATH/Contents/Resources/$d" ]; then
      find "$APP_PATH/Contents/Resources/$d" -type f | while read -r f; do
        if file "$f" | grep -q "Mach-O"; then
          codesign --force --sign "$SIGN_IDENTITY" --options runtime --timestamp "$f" 2>/dev/null || true
        fi
      done
    fi
  done

  # 3. Helper apps (nested in the framework) — each with its process-type
  # entitlements. Renderer/GPU -> allow-jit; Plugin/base/Alerts -> plugin
  # entitlements (allow-unsigned-executable-memory + disable-library-validation).
  REN_HELPER=$(find "$APP_PATH/Contents/Frameworks" -type d -name "*Helper (Renderer).app" 2>/dev/null | head -1)
  if [ -n "$REN_HELPER" ]; then
    HELPERS_DIR=$(dirname "$REN_HELPER")
    for helper in "$HELPERS_DIR/"*.app; do
      [ -d "$helper" ] || continue
      case "$(basename "$helper")" in
        *"(Renderer)"*) ENT="$ENT_DIR/helper-renderer-entitlements.plist" ;;
        *"(GPU)"*)      ENT="$ENT_DIR/helper-gpu-entitlements.plist" ;;
        *)              ENT="$ENT_DIR/helper-plugin-entitlements.plist" ;;
      esac
      codesign --force --sign "$SIGN_IDENTITY" --options runtime --timestamp \
        --entitlements "$ENT" "$helper"
    done
  fi

  # 4. Framework — re-seal (no --deep, so helper entitlements survive).
  FRAMEWORK="$APP_PATH/Contents/Frameworks/MoltBrowser Framework.framework"
  [ -d "$FRAMEWORK" ] && codesign --force --sign "$SIGN_IDENTITY" --options runtime --timestamp "$FRAMEWORK/Versions/Current"

  # 5. Outer app — app entitlements, NO --deep (must not re-sign the helpers).
  codesign --force --sign "$SIGN_IDENTITY" --options runtime --timestamp \
    --entitlements "$ENT_DIR/app-entitlements.plist" "$APP_PATH"
  codesign --verify --deep --strict "$APP_PATH"
  echo "  Code signing complete and verified"
else
  echo "[$STEP/$STEPS_TOTAL] Skipping code signing (no --identity)"
fi

# ---- Step 6: Package DMG ----
STEP=$((STEP + 1))
echo ""
echo "[$STEP/$STEPS_TOTAL] Creating DMG..."
mkdir -p "$REPO_DIR/dist"
rm -f "$DMG_PATH"

if command -v create-dmg &> /dev/null; then
  create-dmg \
    --volname "MoltBrowser" \
    --volicon "$APP_PATH/Contents/Resources/app.icns" \
    --window-pos 200 120 --window-size 600 400 \
    --icon-size 100 --icon "MoltBrowser.app" 175 190 \
    --app-drop-link 425 190 \
    --hide-extension "MoltBrowser.app" \
    --no-internet-enable \
    "$DMG_PATH" "$APP_PATH" || true
else
  STAGING=$(mktemp -d)
  cp -R "$APP_PATH" "$STAGING/"
  ln -s /Applications "$STAGING/Applications"
  hdiutil create -volname "MoltBrowser" -srcfolder "$STAGING" -ov -format UDZO "$DMG_PATH"
  rm -rf "$STAGING"
fi

# Sign DMG if identity provided
[ -n "$SIGN_IDENTITY" ] && codesign --force --sign "$SIGN_IDENTITY" --timestamp "$DMG_PATH"

DMG_SIZE=$(du -sh "$DMG_PATH" | cut -f1)
echo "  DMG created: $DMG_PATH ($DMG_SIZE)"

# ---- Step 7: Notarize ----
STEP=$((STEP + 1))
echo ""
if [ "$DO_NOTARIZE" = true ] && [ -n "$SIGN_IDENTITY" ]; then
  echo "[$STEP/$STEPS_TOTAL] Notarizing..."
  if [ -n "$TEAM_ID" ]; then
    xcrun notarytool submit "$DMG_PATH" --keychain-profile "AC_PASSWORD" --wait
    xcrun stapler staple "$DMG_PATH"
    echo "  Notarized and stapled"
  else
    echo "  WARNING: --team-id required for notarization, skipping"
  fi
else
  echo "[$STEP/$STEPS_TOTAL] Skipping notarization"
fi

# ---- Step 8: Upload to GitHub Releases ----
STEP=$((STEP + 1))
echo ""
if [ "$DO_UPLOAD" = true ]; then
  echo "[$STEP/$STEPS_TOTAL] Uploading to GitHub Releases..."
  if command -v gh &> /dev/null; then
    cd "$REPO_DIR"

    # Create release tag
    git tag -f "v${VERSION}" HEAD

    # Determine if this is a prerelease
    PRERELEASE_FLAG=""
    if [[ "$VERSION" == *"alpha"* ]] || [[ "$VERSION" == *"beta"* ]] || [[ "$VERSION" == *"rc"* ]]; then
      PRERELEASE_FLAG="--prerelease"
    fi

    # Create GitHub release
    gh release create "v${VERSION}" \
      --title "MoltBrowser v${VERSION}" \
      $PRERELEASE_FLAG \
      --notes "$(cat <<RELEASE_EOF
# MoltBrowser v${VERSION}

AI-native privacy browser with on-device LLM inference.

## Highlights
- 🤖 Local AI chat powered by llama.cpp + Metal GPU acceleration
- 🔒 MoltShield ad/tracker blocking built-in
- 📄 Page-aware context (summarize, explain, extract, translate)
- 📦 Multi-model support with one-click download (Llama, Mistral, Phi, Qwen)
- ⌨️ Keyboard shortcuts for all AI actions
- 💬 Chat history, export/import, conversation search
- 🧩 Chrome extension compatibility

## System Requirements
- macOS 12.0 (Monterey) or later
- $([ "$UNIVERSAL" = true ] && echo "Apple Silicon or Intel Mac (universal binary)" || echo "Apple Silicon Mac (arm64)")
- 8 GB RAM minimum (16 GB recommended for larger models)
- 4-8 GB free disk space for model files

## Install
1. Download the DMG below
2. Open it and drag MoltBrowser to Applications
3. Launch MoltBrowser — the first-run wizard will guide you through model setup

## Downloads
- **macOS**: \`${DMG_NAME}.dmg\` ($(du -sh "$DMG_PATH" 2>/dev/null | cut -f1 || echo "N/A"))
RELEASE_EOF
)" \
      "$DMG_PATH"

    echo "  Release: https://github.com/OneManNoCode/MoltBrowser/releases/tag/v${VERSION}"

    # Update appcast
    bash "$SCRIPT_DIR/generate-appcast.sh" --version "$VERSION" --build "$BUILD_NUM" --dmg "$DMG_PATH"
  else
    echo "  ERROR: gh CLI not found. Install with: brew install gh"
  fi
else
  echo "[$STEP/$STEPS_TOTAL] Skipping upload"
fi

# ---- Step 9: Generate landing page assets ----
STEP=$((STEP + 1))
echo ""
echo "[$STEP/$STEPS_TOTAL] Generating landing page download metadata..."
DOWNLOADS_JSON="$REPO_DIR/website/downloads.json"
mkdir -p "$(dirname "$DOWNLOADS_JSON")"
DMG_SIZE_BYTES=$(stat -f%z "$DMG_PATH" 2>/dev/null || echo "0")
cat > "$DOWNLOADS_JSON" << DLJSON
{
  "version": "$VERSION",
  "build": "$BUILD_NUM",
  "date": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "platforms": {
    "macos": {
      "universal": $UNIVERSAL,
      "dmg": {
        "filename": "${DMG_NAME}.dmg",
        "url": "https://github.com/OneManNoCode/MoltBrowser/releases/download/v${VERSION}/${DMG_NAME}.dmg",
        "size_bytes": $DMG_SIZE_BYTES
      },
      "min_os": "12.0",
      "architectures": $([ "$UNIVERSAL" = true ] && echo '["arm64", "x86_64"]' || echo '["arm64"]')
    }
  }
}
DLJSON
echo "  Download metadata: $DOWNLOADS_JSON"

# ---- Summary ----
echo ""
echo "╔══════════════════════════════════════════════╗"
echo "║    Release Complete!                         ║"
echo "╠══════════════════════════════════════════════╣"
echo "║  Version:    $VERSION (build $BUILD_NUM)"
echo "║  DMG:        $DMG_PATH"
echo "║  Size:       $DMG_SIZE"
echo "║  Universal:  $UNIVERSAL"
echo "║  Sparkle:    $BUNDLE_SPARKLE"
echo "║  Signed:     $([ -n "$SIGN_IDENTITY" ] && echo "Yes" || echo "No")"
echo "║  Notarized:  $([ "$DO_NOTARIZE" = true ] && echo "Yes" || echo "No")"
echo "║  Uploaded:   $([ "$DO_UPLOAD" = true ] && echo "Yes" || echo "No")"
echo "╚══════════════════════════════════════════════╝"
echo ""
if [ -z "$SIGN_IDENTITY" ]; then
  echo "⚠️  This release is UNSIGNED. For public GA release, run:"
  echo ""
  echo "  $0 --version $VERSION --build $BUILD_NUM \\"
  echo "    --identity \"Developer ID Application: GenEye AI Labs Inc (TEAM_ID)\" \\"
  echo "    --team-id TEAM_ID --notarize --upload --universal"
  echo ""
fi
