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
#                                   [--fresh]
#
# --fresh removes the overlay's vendored crates from the checkout first, so
# gnrt downloads and patches each of them again: what the overlay commits is
# then gnrt's whole output, not what it left of the copies already there
# (gnrt keeps a crate directory that exists, missing files and all).
#
# --check runs `gn check` and builds //chrome/browser/molt_ai/shield/adblock
# and //chrome/services/molt_ai/document_parser/anydoc afterwards.
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
FRESH=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --chromium-src) CHROMIUM_SRC="$(cd "$2" && pwd)"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --no-copy-back) COPY_BACK=0; shift ;;
    --check) CHECK=1; shift ;;
    --fresh) FRESH=1; shift ;;
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

# gnrt formats every BUILD.gn it writes with the `gn` on PATH and only warns
# when that fails. depot_tools' gn wrapper fails until gclient has
# bootstrapped depot_tools (as on CI), and the unformatted files then differ
# from upstream's in formatting alone: copied back, each one would shadow
# upstream as a patched file. gnrt gets the checkout's own gn binary instead
# (the one DEPS pins), and its output is checked afterwards.
case "$(uname -s)" in
  Darwin) GN_BIN="$CHROMIUM_SRC/buildtools/mac/gn" ;;
  Linux) GN_BIN="$CHROMIUM_SRC/buildtools/linux64/gn" ;;
  *) GN_BIN="" ;;
esac
if [[ -z "$GN_BIN" || ! -x "$GN_BIN" ]]; then
  echo "error: no gn binary at ${GN_BIN:-buildtools/<os>/gn}: run gclient sync (or gclient runhooks) first" >&2
  exit 1
fi
GN_SHIM="$(mktemp -d)"
trap 'rm -rf "${GN_SHIM:?}"' EXIT
ln -s "$GN_BIN" "$GN_SHIM/gn"

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

if [[ "$FRESH" == 1 ]]; then
  echo "==> Fresh: removing the overlay's vendored crates from the checkout"
  for dir in "$OVERLAY_CIO"/vendor/*/; do
    [[ -d "$dir" ]] || continue
    name="$(basename "$dir")"
    target="$CR_CIO/vendor/$name"
    if [[ -n "$name" && -d "$target" ]]; then
      rm -rf "${target:?}"
    fi
  done
fi

echo "==> [2/4] gnrt vendor"
python3 tools/crates/run_gnrt.py vendor

echo "==> [3/4] gnrt gen"
PATH="$GN_SHIM:$PATH" python3 tools/crates/run_gnrt.py gen
# Every BUILD.gn gnrt wrote must be as `gn format` leaves it. (-m lists the
# ones gnrt deleted too: the crates it no longer vendors.)
unformatted=()
while IFS= read -r -d '' f; do
  [[ -f "$f" ]] || continue
  rc=0
  "$GN_BIN" format --dry-run "$f" >/dev/null 2>&1 || rc=$?
  [[ "$rc" == 0 ]] || unformatted+=("$f (gn format --dry-run exited $rc)")
done < <(git ls-files -z -m -o -- ':(glob)third_party/rust/*/*/BUILD.gn')
if [[ ${#unformatted[@]} -gt 0 ]]; then
  echo "error: gnrt left BUILD.gn files gn format would change (exit 2) or can't read (exit 1):" >&2
  printf '  %s\n' "${unformatted[@]}" >&2
  exit 1
fi

if [[ "$COPY_BACK" == 1 ]]; then
  echo "==> [4/4] Copying gnrt output back into the overlay"
  # Everything gnrt added or modified under third_party/rust in the Chromium
  # checkout (which is a git checkout of upstream at the pin).
  # Ignored files too (each one, not just its directory): crates ship
  # build/ scripts and the like, which gnrt's BUILD.gn files may name and
  # an ignore rule would leave behind.
  # NUL-separated, so no name is quoted (git quotes non-ASCII ones).
  git status --porcelain=v1 -z --untracked-files=all --ignored=traditional -- third_party/rust \
    | while IFS= read -r -d '' entry; do
        xy="${entry:0:2}"
        path="${entry:3}"
        # A rename or copy is followed by its source path.
        case "$xy" in
          *R*|*C*) IFS= read -r -d '' _ || true ;;
        esac
        src="$CHROMIUM_SRC/$path"
        dst="$ROOT_DIR/src/$path"
        if [[ -e "$src" ]]; then
          mkdir -p "$(dirname "$dst")"
          cp -p "$src" "$dst"
        elif [[ -e "$dst" ]]; then
          rm -f "$dst"
        fi
      done
  # An overlay copy of an upstream file the checkout now has as upstream is
  # no MoltBrowser change any more: it goes, or it would shadow upstream as
  # a stale patched file (and fail patches/'s check). It goes if it is the
  # same bytes, or, with --fresh, if it is gnrt's own output (a crate's
  # BUILD.gn or README.chromium): only then did gnrt download and patch
  # every overlay crate itself, so what it generated reflects the overlay.
  # (Without --fresh it keeps the checkout's crate directories, which are
  # upstream's if the overlay wasn't copied in.) Any other such file is
  # kept, and every file dropped or kept is named.
  python3 -I - "$CHROMIUM_SRC" "$ROOT_DIR/src" "$FRESH" <<'PY'
import os
import subprocess
import sys

cr, ov = (os.fsencode(a) for a in sys.argv[1:3])
fresh = sys.argv[3] == '1'


def git(*args):
    return subprocess.run(('git',) + args, cwd=cr, check=True,
                          stdout=subprocess.PIPE).stdout.split(b'\0')


tracked = set(p for p in git('ls-files', '-z', '--', 'third_party/rust') if p)
dirty = set()
items = git('status', '--porcelain=v1', '-z', '--untracked-files=all',
            '--ignored=traditional', '--', 'third_party/rust')
i = 0
while i < len(items):
    entry = items[i]
    i += 1
    if not entry:
        continue
    dirty.add(entry[3:])
    if b'R' in entry[:2] or b'C' in entry[:2]:
        if i < len(items):
            dirty.add(items[i])
        i += 1

MARK = b'@generated from third_party/rust/chromium_crates_io/BUILD.gn.hbs'


def generated(rel):
    d, name = os.path.split(rel)
    if name not in (b'BUILD.gn', b'README.chromium'):
        return False
    try:
        with open(os.path.join(cr, d, b'BUILD.gn'), 'rb') as f:
            return MARK in f.read(4096)
    except OSError:
        return False


CIO = b'third_party/rust/chromium_crates_io/'
# The crate config this script copies in (step 1) stays, whatever it holds.
INPUTS = (CIO + b'Cargo.toml', CIO + b'gnrt_config.toml')
root = os.path.join(ov, b'third_party', b'rust')
dropped = 0
for dirpath, dirnames, filenames in os.walk(root, topdown=False):
    for name in filenames:
        full = os.path.join(dirpath, name)
        rel = os.path.relpath(full, ov)
        if rel not in tracked or rel in dirty or os.path.islink(full):
            continue
        if rel in INPUTS or rel.startswith(CIO + b'patches/'):
            continue
        with open(full, 'rb') as a, open(os.path.join(cr, rel), 'rb') as b:
            same = a.read() == b.read()
        if same or (fresh and generated(rel)):
            os.remove(full)
            dropped += 1
            print('    dropped %s (%s)' % (
                os.fsdecode(rel),
                'the same as upstream\'s' if same else
                'gnrt now generates upstream\'s'))
        elif generated(rel):
            print('    kept %s: gnrt generated upstream\'s, but without '
                  '--fresh it may not have seen the overlay\'s crates'
                  % os.fsdecode(rel))
        else:
            print('    kept %s: the checkout has upstream\'s copy '
                  '(was the overlay copied in first?)' % os.fsdecode(rel))
    if dirpath != root and not os.listdir(dirpath):
        os.rmdir(dirpath)
print('    dropped %d overlay copies the checkout has as upstream' % dropped)
PY
  # A crate only the overlay vendors is the overlay's whole copy of it: a
  # file gnrt no longer writes goes from the overlay too, and so does a
  # crate gnrt removed. (For a crate upstream vendors too, the overlay keeps
  # only the files MoltBrowser changes.)
  for dir in "$OVERLAY_CIO"/vendor/*/; do
    [[ -d "$dir" ]] || continue
    name="$(basename "$dir")"
    [[ -n "$name" ]] || continue
    if [[ -n "$(git ls-files -- "third_party/rust/chromium_crates_io/vendor/$name" | head -n 1)" ]]; then
      continue
    fi
    if [[ ! -d "$CR_CIO/vendor/$name" ]]; then
      rm -rf "${OVERLAY_CIO:?}/vendor/${name:?}"
      continue
    fi
    (cd "$OVERLAY_CIO/vendor/$name" && find . -type f -print0) |
      while IFS= read -r -d '' f; do
        [[ -e "$CR_CIO/vendor/$name/$f" ]] || rm -f "$OVERLAY_CIO/vendor/$name/$f"
      done
  done
  echo "    done. Review: git -C \"$ROOT_DIR\" status --ignored -- src/third_party/rust"
  echo "    (commit with git add -f: crates' own .gitignore files hide some of their files)"

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
  rsync -a "$ROOT_DIR/src/chrome/services/molt_ai/document_parser/" \
    "$CHROMIUM_SRC/chrome/services/molt_ai/document_parser/"
  gn check "$BUILD_DIR" "//chrome/browser/molt_ai/shield/*"
  autoninja -C "$BUILD_DIR" \
    chrome/browser/molt_ai/shield/adblock:molt_adblock \
    chrome/services/molt_ai/document_parser/anydoc:anydoc_ffi
fi
