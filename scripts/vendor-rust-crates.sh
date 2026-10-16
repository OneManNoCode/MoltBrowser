#!/bin/bash
# MoltBrowser — vendor third-party Rust crates with Chromium's gnrt
# Copyright 2026 GenEye AI Labs Inc.
#
# MoltBrowser adds Rust crates (anydoc, adblock for MoltShield, ...) by
# editing the overlay copies of
#   src/third_party/rust/chromium_crates_io/Cargo.toml
#   src/third_party/rust/chromium_crates_io/gnrt_config.toml
#   src/third_party/rust/chromium_crates_io/patches/
# gnrt (Chromium's vendoring tool) needs a full Chromium checkout, so it runs
# on the build machine. This script automates the round trip:
#
#   1. copy the overlay crate config into chromium/src
#   2. tools/crates/run_gnrt.py vendor   (download crates into vendor/,
#                                         apply patches/, update Cargo.lock)
#   3. tools/crates/run_gnrt.py gen      (BUILD.gn + README.chromium per crate)
#   4. copy everything gnrt changed under third_party/rust back into the
#      overlay (src/third_party/rust/...) so it is committed, like the anydoc
#      tree was
#   5. optionally build the first-party targets that use the crates
#
# Usage:
#   ./scripts/vendor-rust-crates.sh [--chromium-src PATH] [--no-copy-back]
#                                   [--build-dir out/MoltBrowser] [--check]
#
# --check runs `gn check` and builds //chrome/browser/molt_ai/shield/adblock
# and //chrome/browser/molt_ai/anydoc afterwards.
#
# On Chromium 152, adding adblock (MoltShield) vendors 15 new crates: adblock,
# arrayvec 0.7 (upstream dropped it at 152), flatbuffers, seahash, url,
# idna(+adapter), form_urlencoded, percent-encoding, thiserror 1 (+impl),
# rustc-hash, precomputed-hash, and the build-only rustc_version + semver
# (itertools 0.14, utf8_iter and regex come from upstream now). It also
# regenerates existing BUILD.gn files whose feature sets grow (serde gains
# `rc`, bitflags gains `serde`), and cargo moves winapi-util from windows-sys
# 0.52 to 0.61 (the version anydoc's anstyle-* crates already use).
# Review `git status src/third_party/rust` before committing.
#
# Crates upstream also vendors are used from upstream (one version per epoch):
# keep overlay files for such a crate only where MoltBrowser must change them
# (e.g. the BUILD.gn of a crate upstream builds test-only but we ship). Step 4
# lists overlay vendor/ dirs that the Chromium checkout also tracks, so such
# overrides get reviewed.
#
# The standalone cargo harness in tools/shield_bench builds the MoltShield
# bridge against the same adblock version without a Chromium checkout.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_SRC="$ROOT_DIR/chromium/src"
BUILD_DIR="out/MoltBrowser"
COPY_BACK=1
CHECK=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --chromium-src) CHROMIUM_SRC="$(cd "$2" && pwd)"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --no-copy-back) COPY_BACK=0; shift ;;
    --check) CHECK=1; shift ;;
    -h|--help) sed -n '2,46p' "$0"; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 1 ;;
  esac
done

if [[ ! -f "$CHROMIUM_SRC/tools/crates/run_gnrt.py" ]]; then
  echo "error: no Chromium checkout at $CHROMIUM_SRC (tools/crates/run_gnrt.py missing)" >&2
  exit 1
fi

if [[ -d /opt/depot_tools ]]; then
  export PATH="/opt/depot_tools:$PATH"
else
  export PATH="$ROOT_DIR/depot_tools:$PATH"
fi

OVERLAY_CIO="$ROOT_DIR/src/third_party/rust/chromium_crates_io"
CR_CIO="$CHROMIUM_SRC/third_party/rust/chromium_crates_io"

echo "==> [1/4] Copying crate config into $CR_CIO"
cp "$OVERLAY_CIO/Cargo.toml" "$CR_CIO/Cargo.toml"
cp "$OVERLAY_CIO/gnrt_config.toml" "$CR_CIO/gnrt_config.toml"
if [[ -d "$OVERLAY_CIO/patches" ]]; then
  # rsync without --delete: upstream Chromium keeps its own patches there.
  rsync -a "$OVERLAY_CIO/patches/" "$CR_CIO/patches/"
fi
# Start from the overlay's lock file (which already records anydoc etc.) so
# gnrt only adds what's new.
if [[ -f "$OVERLAY_CIO/Cargo.lock" ]]; then
  cp "$OVERLAY_CIO/Cargo.lock" "$CR_CIO/Cargo.lock"
fi

cd "$CHROMIUM_SRC"

echo "==> [2/4] gnrt vendor"
python3 tools/crates/run_gnrt.py vendor

echo "==> [3/4] gnrt gen"
python3 tools/crates/run_gnrt.py gen

if [[ "$COPY_BACK" == 1 ]]; then
  echo "==> [4/4] Copying gnrt output back into the overlay"
  # Everything gnrt added or modified under third_party/rust in the Chromium
  # checkout (which is a git checkout of upstream at the pin).
  git status --porcelain --untracked-files=all -- third_party/rust \
    | sed -E 's/^.{3}//' \
    | while IFS= read -r path; do
        # Renames show as "old -> new".
        path="${path##* -> }"
        src="$CHROMIUM_SRC/$path"
        dst="$ROOT_DIR/src/$path"
        if [[ -e "$src" ]]; then
          mkdir -p "$(dirname "$dst")"
          cp -p "$src" "$dst"
        elif [[ -e "$dst" ]]; then
          rm -f "$dst"
        fi
      done
  echo "    done. Review: git -C \"$ROOT_DIR\" status -- src/third_party/rust"

  # Overlay vendor dirs that upstream also vendors replace upstream's copy
  # (Chromium allows one version per epoch). Usually they should be dropped
  # from the overlay so upstream's version is used.
  overrides=()
  for dir in "$OVERLAY_CIO"/vendor/*/; do
    name="$(basename "$dir")"
    if [[ -n "$(git ls-files -- "third_party/rust/chromium_crates_io/vendor/$name" | head -n 1)" ]]; then
      overrides+=("$name")
    fi
  done
  if [[ ${#overrides[@]} -gt 0 ]]; then
    echo "    note: upstream also vendors these overlay crates (overlay copy wins):"
    printf '      %s\n' "${overrides[@]}"
  fi
else
  echo "==> [4/4] Skipped copy-back (--no-copy-back)"
fi

if [[ "$CHECK" == 1 ]]; then
  echo "==> gn check + build of the first-party Rust targets"
  # Make sure the first-party sources are in the tree being built.
  rsync -a "$ROOT_DIR/src/chrome/browser/molt_ai/" "$CHROMIUM_SRC/chrome/browser/molt_ai/"
  gn check "$BUILD_DIR" "//chrome/browser/molt_ai/shield/*"
  autoninja -C "$BUILD_DIR" \
    chrome/browser/molt_ai/shield/adblock:molt_adblock \
    chrome/browser/molt_ai/anydoc:anydoc_ffi
fi
