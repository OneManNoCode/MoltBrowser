#!/bin/bash
# Generate MoltBrowser app icon (ICNS) from SVG
# Creates a modern gradient icon with AI brain motif
#
# Usage: ./scripts/generate-icon.sh
# Requires: Python 3 (for base64/icon generation)
#
# Copyright 2025 GenEye AI Labs Inc. Licensed under GPLv3.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
ICON_DIR="$REPO_DIR/branding"
ICONSET_DIR="$ICON_DIR/MoltBrowser.iconset"

mkdir -p "$ICON_DIR"
mkdir -p "$ICONSET_DIR"

echo "=== MoltBrowser Icon Generator ==="

# Create SVG icon
cat > "$ICON_DIR/icon.svg" << 'SVGEOF'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">
  <defs>
    <linearGradient id="bg" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" style="stop-color:#4338ca"/>
      <stop offset="50%" style="stop-color:#6366f1"/>
      <stop offset="100%" style="stop-color:#a855f7"/>
    </linearGradient>
    <linearGradient id="glow" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" style="stop-color:#818cf8;stop-opacity:0.3"/>
      <stop offset="100%" style="stop-color:#c084fc;stop-opacity:0.1"/>
    </linearGradient>
  </defs>
  <!-- Rounded square background -->
  <rect width="1024" height="1024" rx="220" fill="url(#bg)"/>
  <!-- Inner glow -->
  <rect x="40" y="40" width="944" height="944" rx="190" fill="url(#glow)"/>
  <!-- Brain/Circuit motif - stylized M -->
  <g transform="translate(512,480)" fill="none" stroke="white" stroke-width="48" stroke-linecap="round" stroke-linejoin="round">
    <!-- M shape -->
    <path d="M-240,160 L-240,-160 L-80,80 L80,-160 L80,80" opacity="0.95"/>
    <!-- Neural connections -->
    <circle cx="-240" cy="-160" r="20" fill="white" stroke="none" opacity="0.9"/>
    <circle cx="-80" cy="80" r="20" fill="white" stroke="none" opacity="0.9"/>
    <circle cx="80" cy="-160" r="20" fill="white" stroke="none" opacity="0.9"/>
    <circle cx="80" cy="80" r="20" fill="white" stroke="none" opacity="0.9"/>
    <!-- AI sparkle dots -->
    <circle cx="200" cy="-120" r="14" fill="white" opacity="0.6"/>
    <circle cx="240" cy="0" r="10" fill="white" opacity="0.4"/>
    <circle cx="200" cy="120" r="12" fill="white" opacity="0.5"/>
    <circle cx="-280" cy="40" r="10" fill="white" opacity="0.3"/>
  </g>
  <!-- "AI" text badge -->
  <g transform="translate(512,720)">
    <rect x="-60" y="-28" width="120" height="56" rx="28" fill="rgba(255,255,255,0.2)"/>
    <text x="0" y="10" font-family="system-ui,-apple-system,sans-serif" font-size="36" font-weight="700" fill="white" text-anchor="middle">AI</text>
  </g>
</svg>
SVGEOF

echo "SVG created: $ICON_DIR/icon.svg"

# Convert SVG to PNG at various sizes using sips (macOS built-in)
# First create a high-res PNG from SVG using Python
python3 << 'PYEOF'
import subprocess
import os

icon_dir = os.environ.get('ICON_DIR', 'branding')
iconset_dir = os.environ.get('ICONSET_DIR', 'branding/MoltBrowser.iconset')
svg_path = os.path.join(icon_dir, 'icon.svg')

# Use qlmanage (macOS Quick Look) to convert SVG to PNG
# First create a large PNG
try:
    subprocess.run([
        'qlmanage', '-t', '-s', '1024', '-o', icon_dir, svg_path
    ], capture_output=True, check=True)

    # qlmanage outputs as icon.svg.png
    src_png = os.path.join(icon_dir, 'icon.svg.png')
    if os.path.exists(src_png):
        # Generate all required sizes for iconset
        sizes = [16, 32, 64, 128, 256, 512, 1024]
        for size in sizes:
            out = os.path.join(iconset_dir, f'icon_{size}x{size}.png')
            subprocess.run([
                'sips', '-z', str(size), str(size), src_png,
                '--out', out
            ], capture_output=True)

            # Also create @2x versions
            if size <= 512:
                out2x = os.path.join(iconset_dir, f'icon_{size}x{size}@2x.png')
                s2 = size * 2
                subprocess.run([
                    'sips', '-z', str(s2), str(s2), src_png,
                    '--out', out2x
                ], capture_output=True)

        print(f"Generated PNG icons in {iconset_dir}")
    else:
        print("Warning: qlmanage didn't produce expected output")
        print("Creating placeholder PNGs with sips...")
        # Fallback: try using rsvg-convert or just note the limitation

except Exception as e:
    print(f"Note: SVG conversion requires qlmanage: {e}")
    print("You can manually convert the SVG to PNG and run iconutil")
PYEOF

# Convert iconset to icns
if [ -d "$ICONSET_DIR" ] && [ "$(ls -A $ICONSET_DIR 2>/dev/null)" ]; then
  iconutil -c icns "$ICONSET_DIR" -o "$ICON_DIR/MoltBrowser.icns" 2>/dev/null && \
    echo "ICNS created: $ICON_DIR/MoltBrowser.icns" || \
    echo "Note: iconutil failed — check PNG files in $ICONSET_DIR"
else
  echo "Note: No PNGs generated. Convert $ICON_DIR/icon.svg manually."
fi

echo ""
echo "To apply the icon to the build:"
echo "  cp $ICON_DIR/MoltBrowser.icns chromium/src/chrome/app/theme/chromium/mac/app.icns"
echo ""
echo "Done!"
