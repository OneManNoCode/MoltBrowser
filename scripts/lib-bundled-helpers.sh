#!/bin/bash
# Shared steps for the helper programs bundled in a macOS MoltBrowser.app
# (Contents/Resources/tor/, ocr/, whisper/), for release.sh, sign-app.sh and
# dev-install-mac.sh.
# Copyright 2026 GenEye AI Labs Inc.
#
# Source this with:
#   source "$(dirname "${BASH_SOURCE[0]}")/lib-bundled-helpers.sh"
#
# Why: codesign --deep treats Resources/ as data, so the helpers and the
# libraries bundled with them (ocr/lib/*.dylib, see macho_bundle_deps.py) are
# signed one file at a time. tesseract and whisper-cli run under the helper
# sandbox, which maps code only from their own directory and the OS, and
# under the hardened runtime, whose library validation loads only libraries
# signed by the same team. A helper that loads a Homebrew library, or a lib/
# dylib left ad hoc, fails at launch: v0.2.5 shipped a tesseract that loaded
# leptonica, libarchive and libtesseract from /opt/homebrew.
#
# Adds:
#   check_bundled_helper_libraries <app>
#       -> fails if Resources/ocr/tesseract or Resources/whisper/whisper-cli
#          (or a library in its lib/) loads code from outside its directory
#          or the OS; prints each such load. A helper that isn't bundled is
#          skipped.
#   sign_bundled_helpers <app> <identity>
#       -> signs every Mach-O under Resources/{tor,ocr,whisper} with the
#          hardened runtime and a secure timestamp; fails on the first error
#   verify_bundled_helper_signatures <app>
#       -> fails unless every such Mach-O is validly signed by <app>'s team
#          with the hardened runtime

_MOLT_HELPERS_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Under Contents/Resources/.
MOLT_BUNDLED_HELPER_DIRS=(tor ocr whisper)
# The sandboxed helpers, each with its libraries (if any) in lib/ beside it.
MOLT_SANDBOXED_HELPERS=(ocr/tesseract whisper/whisper-cli)

# Every Mach-O file under the bundled helper directories, one per line.
bundled_helper_machos() {
  local app="$1" dir f
  for dir in "${MOLT_BUNDLED_HELPER_DIRS[@]}"; do
    [[ -d "$app/Contents/Resources/$dir" ]] || continue
    while IFS= read -r -d '' f; do
      if file -b "$f" | grep -q "Mach-O"; then
        printf '%s\n' "$f"
      fi
    done < <(find "$app/Contents/Resources/$dir" -type f -print0)
  done
}

check_bundled_helper_libraries() {
  local app="$1" rel bin failed=0
  for rel in "${MOLT_SANDBOXED_HELPERS[@]}"; do
    bin="$app/Contents/Resources/$rel"
    [[ -f "$bin" ]] || continue
    if ! python3 "$_MOLT_HELPERS_SCRIPT_DIR/macho_bundle_deps.py" --check \
           "$bin" --lib-dir "$(dirname "$bin")/lib"; then
      failed=1
    fi
  done
  return "$failed"
}

sign_bundled_helpers() {
  local app="$1" identity="$2" f out
  while IFS= read -r f; do
    # codesign says "replacing existing signature" for each file; show what
    # it says only when it fails.
    if ! out=$(codesign --force --sign "$identity" --options runtime \
                 --timestamp "$f" 2>&1); then
      echo "$out" >&2
      echo "FATAL: could not sign ${f#"$app/"}" >&2
      return 1
    fi
  done < <(bundled_helper_machos "$app")
}

verify_bundled_helper_signatures() {
  local app="$1" team f info failed=0
  team=$(codesign -dv "$app" 2>&1 | sed -n 's/^TeamIdentifier=//p')
  if [[ -z "$team" || "$team" == "not set" ]]; then
    echo "FATAL: $app has no team signature to check its helpers against" >&2
    return 1
  fi
  while IFS= read -r f; do
    info=$(codesign -dv "$f" 2>&1) || info=""
    if ! codesign --verify --strict "$f" 2>/dev/null ||
       ! grep -qx "TeamIdentifier=$team" <<<"$info" ||
       ! grep -q '^CodeDirectory .*flags=0x[0-9a-f]*([^)]*runtime' <<<"$info"; then
      echo "not signed by team $team with the hardened runtime: ${f#"$app/"}" >&2
      failed=1
    fi
  done < <(bundled_helper_machos "$app")
  return "$failed"
}
