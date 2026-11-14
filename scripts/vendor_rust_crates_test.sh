#!/bin/bash
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# Tests scripts/vendor-rust-crates.sh's copy-back and its gn format check on
# a stand-in Chromium checkout (a stand-in gnrt and gn), no Chromium needed.
# Runs under macOS's /bin/bash 3.2 too (mac-release-preflight.yml), where
# the owner would run the script:
#
#   bash scripts/vendor_rust_crates_test.sh
#
# What it checks, as on CI, where depot_tools' gn wrapper fails before
# gclient has bootstrapped depot_tools:
#   * gnrt formats with the checkout's own gn, not depot_tools' wrapper;
#   * a BUILD.gn gnrt left unformatted fails the script;
#   * an overlay copy of an upstream file the checkout has as upstream goes
#     (gnrt's own output, or the same bytes), and its empty directories;
#   * a file gnrt changed is copied back; a hand-edited upstream file the
#     checkout has as upstream is kept and named;
#   * a crate only the overlay vendors loses the files gnrt no longer writes;
#   * an upstream crate gnrt deletes passes the format check and its overlay
#     copy goes;
#   * the crate config and patches the script copies in stay;
#   * without --fresh, gnrt's output that differs from the overlay's stays.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "${T:?}"' EXIT
FAILS=0

fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
ok() { echo "ok: $*"; }
has() { [[ -f "$1" ]] && [[ "$(cat "$1")" == "$2" ]]; }

HEADER='# @generated from third_party/rust/chromium_crates_io/BUILD.gn.hbs by
# tools/crates/gnrt.'

put() {  # put FILE CONTENT
  mkdir -p "$(dirname "$1")"
  printf '%s\n' "$2" > "$1"
}

setup() {
  local case_dir="$T/$1"
  REPO="$case_dir/repo"
  CR="$case_dir/cr"
  mkdir -p "$REPO/scripts"
  cp "$ROOT/scripts/vendor-rust-crates.sh" "$REPO/scripts/"

  # depot_tools' gn wrapper as on CI: it fails.
  put "$REPO/depot_tools/gn" '#!/bin/bash
echo "python3_bin_reldir.txt not found. need to initialize depot_tools" >&2
exit 1'
  chmod +x "$REPO/depot_tools/gn"

  # The checkout's gn: "formats" by removing UNFORMATTED lines.
  local gn='#!/bin/bash
[ "$1" = format ] || exit 1
if [ "$2" = --dry-run ]; then
  [ -f "$3" ] || { echo "cannot read $3 for formatting" >&2; exit 1; }
  if grep -q UNFORMATTED "$3"; then exit 2; fi
  exit 0
fi
grep -v UNFORMATTED "$2" > "$2.tmp" || true
mv "$2.tmp" "$2"'
  put "$CR/buildtools/linux64/gn" "$gn"
  put "$CR/buildtools/mac/gn" "$gn"
  chmod +x "$CR/buildtools/linux64/gn" "$CR/buildtools/mac/gn"

  # A stand-in gnrt: writes what gnrt would, formatting with the gn on PATH
  # and ignoring its failure, as gnrt does.
  put "$CR/tools/crates/run_gnrt.py" 'import os, subprocess, sys
HEADER = """'"$HEADER"'
"""
def write(rel, body, fmt=True):
    os.makedirs(os.path.dirname(rel), exist_ok=True)
    with open(rel, "w") as f:
        f.write(body + "UNFORMATTED\n" if fmt else body)
    if fmt and not os.environ.get("FAKE_GNRT_SKIP_FORMAT"):
        subprocess.run(["gn", "format", rel])
v = "third_party/rust/chromium_crates_io/vendor/"
if sys.argv[1] == "vendor":
    write(v + "up-v1/src/lib.rs", "up\n", fmt=False)
    write(v + "ours-v1/src/lib.rs", "ours\n", fmt=False)
else:
    import shutil
    shutil.rmtree("third_party/rust/dropped/v1")
    write("third_party/rust/same/v1/BUILD.gn", HEADER + "same\n")
    write("third_party/rust/same/v1/README.chromium", "Name: same\n", fmt=False)
    write("third_party/rust/changed/v1/BUILD.gn", HEADER + "new\n")
    write("third_party/rust/ours/v1/BUILD.gn", HEADER + "ours\n")'

  # Upstream at the pin.
  put "$CR/third_party/rust/chromium_crates_io/Cargo.toml" 'upstream toml'
  put "$CR/third_party/rust/chromium_crates_io/gnrt_config.toml" 'upstream config'
  put "$CR/third_party/rust/chromium_crates_io/Cargo.lock" 'upstream lock'
  put "$CR/third_party/rust/chromium_crates_io/patches/up-v1/0001-x.patch" 'patch'
  put "$CR/third_party/rust/chromium_crates_io/vendor/up-v1/src/lib.rs" 'up'
  put "$CR/third_party/rust/same/v1/BUILD.gn" "$HEADER
same"
  put "$CR/third_party/rust/same/v1/README.chromium" 'Name: same'
  put "$CR/third_party/rust/changed/v1/BUILD.gn" "$HEADER
old"
  put "$CR/third_party/rust/wrapper/v1/lib.rs" 'upstream'
  put "$CR/third_party/rust/handedit/v1/lib.rs" 'upstream'
  put "$CR/third_party/rust/dropped/v1/BUILD.gn" "$HEADER
dropped"
  put "$CR/third_party/rust/dropped/v1/README.chromium" 'Name: dropped'
  git -C "$CR" init -q
  git -C "$CR" add -A
  git -C "$CR" -c user.name=t -c user.email=t@example.invalid \
    -c commit.gpgsign=false commit -q -m upstream

  # The overlay, before vendoring.
  local o="$REPO/src/third_party/rust"
  put "$o/chromium_crates_io/Cargo.toml" 'our toml'
  # The same as upstream's: the script's input stays all the same.
  put "$o/chromium_crates_io/gnrt_config.toml" 'upstream config'
  put "$o/chromium_crates_io/patches/up-v1/0001-x.patch" 'patch'
  put "$o/chromium_crates_io/Cargo.lock" 'our lock'
  put "$o/chromium_crates_io/vendor/up-v1/src/lib.rs" 'up'
  put "$o/chromium_crates_io/vendor/ours-v1/src/lib.rs" 'ours'
  put "$o/chromium_crates_io/vendor/ours-v1/src/old.rs" 'gone'
  put "$o/same/v1/BUILD.gn" "$HEADER
same
UNFORMATTED"
  put "$o/same/v1/README.chromium" 'Name: same (stale)'
  put "$o/changed/v1/BUILD.gn" "$HEADER
new
UNFORMATTED"
  put "$o/wrapper/v1/lib.rs" 'ours'
  put "$o/handedit/v1/lib.rs" 'ours'
  put "$o/dropped/v1/BUILD.gn" "$HEADER
dropped
UNFORMATTED"
  # CI copies the overlay in first: here only wrapper/, so handedit/ is a
  # hand-edited file the checkout has as upstream.
  cp "$o/wrapper/v1/lib.rs" "$CR/third_party/rust/wrapper/v1/lib.rs"
}

# The script copies the overlay's patches/ in with `rsync -a SRC/ DST/`.
BIN="$T/bin"
mkdir -p "$BIN"
if ! command -v rsync >/dev/null; then
  put "$BIN/rsync" '#!/bin/bash
[ "$1" = -a ] || exit 1
mkdir -p "$3" && cp -pR "$2." "$3"'
  chmod +x "$BIN/rsync"
fi

# Under this test's own bash (macOS's 3.2 in the Mac release preflight), not
# the first bash on PATH.
echo "bash $BASH_VERSION"

run() {  # run LOG [--no-fresh] [ENV=VALUE...]
  local log="$1" fresh=--fresh
  shift
  if [[ "${1:-}" == --no-fresh ]]; then
    fresh=""
    shift
  fi
  (cd "$REPO" && env PATH="$BIN:$PATH" "$@" "$BASH" scripts/vendor-rust-crates.sh \
     --chromium-src "$CR" ${fresh:+"$fresh"}) > "$log" 2>&1
}

# 1. The copy-back.
setup copyback
o="$REPO/src/third_party/rust"
if run "$T/copyback.log"; then
  ok "the script succeeds"
else
  fail "the script failed:"; cat "$T/copyback.log"
fi
if has "$CR/third_party/rust/same/v1/BUILD.gn" "$HEADER
same"; then
  ok "gnrt formatted with the checkout's gn, not depot_tools' wrapper"
else
  fail "gnrt's BUILD.gn was not formatted"
fi
[[ ! -e "$o/same" ]] && ok "upstream's copies (gnrt's output) dropped, directories too" \
  || fail "same/ is still in the overlay: $(find "$o/same" -type f)"
[[ ! -e "$o/chromium_crates_io/vendor/up-v1" ]] && ok "a byte-identical upstream file dropped" \
  || fail "vendor/up-v1 is still in the overlay"
has "$o/changed/v1/BUILD.gn" "$HEADER
new" && ok "a file gnrt changed copied back, formatted" \
  || fail "changed/v1/BUILD.gn: $(cat "$o/changed/v1/BUILD.gn")"
has "$o/ours/v1/BUILD.gn" "$HEADER
ours" && ok "an overlay-only crate's BUILD.gn copied back" \
  || fail "ours/v1/BUILD.gn missing"
has "$o/wrapper/v1/lib.rs" ours && ok "a hand-edited upstream file kept" \
  || fail "wrapper/v1/lib.rs changed"
if has "$o/handedit/v1/lib.rs" ours &&
   grep -q 'kept third_party/rust/handedit/v1/lib.rs' "$T/copyback.log"; then
  ok "a hand-edited file the checkout has as upstream kept and named"
else
  fail "handedit/v1/lib.rs not kept and named"
fi
has "$o/chromium_crates_io/vendor/ours-v1/src/lib.rs" ours &&
  [[ ! -e "$o/chromium_crates_io/vendor/ours-v1/src/old.rs" ]] &&
  ok "an overlay-only crate loses what gnrt no longer writes" ||
  fail "vendor/ours-v1 not mirrored"
[[ ! -e "$o/dropped" ]] &&
  ok "a crate gnrt deleted: no error from the format check, its copy dropped" ||
  fail "dropped/ is still in the overlay"
has "$o/chromium_crates_io/Cargo.toml" 'our toml' &&
  has "$o/chromium_crates_io/gnrt_config.toml" 'upstream config' &&
  has "$o/chromium_crates_io/patches/up-v1/0001-x.patch" patch &&
  ok "the crate config and patches kept, even when the same as upstream's" ||
  fail "the crate config or patches changed"

# 2. A BUILD.gn gnrt left unformatted fails the script.
setup unformatted
if run "$T/unformatted.log" FAKE_GNRT_SKIP_FORMAT=1; then
  fail "the script succeeded with unformatted BUILD.gn files"
elif grep -q 'gn format would change' "$T/unformatted.log" &&
     grep -q 'third_party/rust/changed/v1/BUILD.gn' "$T/unformatted.log"; then
  ok "unformatted BUILD.gn files fail the script, named"
else
  fail "unexpected failure:"; cat "$T/unformatted.log"
fi

# 3. Without --fresh, gnrt keeps the checkout's crate directories (upstream's
# if the overlay wasn't copied in): its output may not reflect the overlay,
# so only the same bytes go.
setup nofresh
o="$REPO/src/third_party/rust"
if run "$T/nofresh.log" --no-fresh; then
  ok "without --fresh: the script succeeds"
else
  fail "without --fresh: the script failed:"; cat "$T/nofresh.log"
fi
if [[ -f "$o/same/v1/BUILD.gn" ]] &&
   grep -q 'kept third_party/rust/same/v1/BUILD.gn: gnrt generated' "$T/nofresh.log"; then
  ok "without --fresh: gnrt's output differing from the overlay's kept and named"
else
  fail "without --fresh: same/v1/BUILD.gn not kept and named"
fi
[[ ! -e "$o/chromium_crates_io/vendor/up-v1" ]] &&
  grep -q 'dropped third_party/rust/chromium_crates_io/vendor/up-v1/src/lib.rs (the same' "$T/nofresh.log" &&
  ok "without --fresh: the same bytes dropped and named" ||
  fail "without --fresh: vendor/up-v1 not dropped and named"

# 4. No gn binary in the checkout.
setup nogn
rm -f "$CR/buildtools/linux64/gn" "$CR/buildtools/mac/gn"
if run "$T/nogn.log"; then
  fail "the script succeeded without a gn binary"
elif grep -q 'no gn binary' "$T/nogn.log"; then
  ok "no gn binary: an error before gnrt runs"
else
  fail "unexpected failure:"; cat "$T/nogn.log"
fi

if [[ "$FAILS" -gt 0 ]]; then
  echo "$FAILS failed"
  exit 1
fi
echo "all passed"
