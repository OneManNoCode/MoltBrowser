#!/bin/bash
# MoltBrowser — a test build of MoltBrowser on GitHub's runners, in stages
# Copyright 2026 GenEye AI Labs Inc.
#
# A Chromium build doesn't fit in one 6-hour job on a 4-core runner, so
# .github/workflows/molt-test-build.yml runs it as a chain of jobs (stages).
# Each stage syncs the pinned Chromium, applies the overlay, restores the
# out dir the previous stage left (or a previous run's), builds until its
# time budget ends and saves the out dir again. A stage whose build is
# complete runs MoltBrowser's tests.
#
# Ninja decides what to rebuild by file times, and a fresh sync gives every
# file a new time; tools/ci/molt_ci_times.py sets the tree to one old time
# and only the files that changed since the restored build to now.
#
# Usage: tools/ci/molt_test_build.sh <command> [args]
#   deps                 build and test packages, depot_tools
#   sync                 Chromium at the pin (shallow), with its hooks
#   overlay              src/ over chromium/src, then the Rust crates (gnrt)
#   restore DIR          unpack the out dir parts in DIR, if there are any
#   times                file times for ninja (see above)
#   gen                  args.gn and gn gen, unless an out dir was restored
#   build MINUTES T...   ninja -k 0 on the targets until done or MINUTES pass
#   save DIR             the out dir as parts in DIR, with the manifest
#   test DIR             MoltBrowser's unit and browser tests; reports in DIR
#   failures             the failed build steps and what they printed
#   recheck DIR          rebuild only the steps the restored build failed
#
# The out dir is chromium/src/out/MoltCI: a component build with DCHECKs on
# (developer and test configuration, not a release build).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CR="$ROOT/chromium"
SRC="$CR/src"
OUT_REL="out/MoltCI"
OUT="$SRC/$OUT_REL"
STATE="$CR/.molt-ci"
DEPOT="${DEPOT_TOOLS:-$HOME/depot_tools}"
export DEPOT_TOOLS_UPDATE=0
export PATH="$DEPOT:$PATH"
mkdir -p "$STATE"

say() { echo "==> $*"; }
output() { [ -n "${GITHUB_OUTPUT:-}" ] && echo "$1=$2" >> "$GITHUB_OUTPUT"; return 0; }
summary() { [ -n "${GITHUB_STEP_SUMMARY:-}" ] && cat >> "$GITHUB_STEP_SUMMARY"; return 0; }

pin_revision() { "$ROOT/tools/chromium/pin.sh" revision; }

ninja_bin() {
  local c
  for c in "$SRC/third_party/ninja/ninja" "$DEPOT/ninja"; do
    [ -x "$c" ] && { echo "$c"; return; }
  done
  command -v ninja
}

cmd_deps() {
  say "Packages"
  sudo apt-get update -q
  sudo apt-get install -y -q --no-install-recommends \
    build-essential clang lld cmake ninja-build python3 python3-pip \
    git curl wget pkg-config gperf bison flex zstd \
    libgtk-3-dev libglib2.0-dev libnss3-dev libatk1.0-dev \
    libatk-bridge2.0-dev libcups2-dev libdrm-dev libxkbcommon-dev \
    libxcomposite-dev libxdamage-dev libxrandr-dev libgbm-dev \
    libpango1.0-dev libasound2-dev libdbus-1-dev libexpat1-dev \
    libfontconfig1-dev libfreetype-dev libnspr4-dev libpulse-dev \
    libx11-xcb-dev libxcb-dri3-dev libxext-dev libxfixes-dev \
    libxi-dev libxrender-dev libxtst-dev libvulkan-dev \
    xvfb xauth dbus-x11 pulseaudio pulseaudio-utils \
    fonts-liberation fonts-noto-color-emoji
  if [ ! -d "$DEPOT" ]; then
    say "depot_tools"
    git clone -q --depth 1 \
      https://chromium.googlesource.com/chromium/tools/depot_tools.git "$DEPOT"
  fi
  ( cd "$DEPOT" && ./gclient --version >/dev/null 2>&1 ) || true
  [ -n "${GITHUB_PATH:-}" ] && echo "$DEPOT" >> "$GITHUB_PATH"
  return 0
}

cmd_sync() {
  local rev
  rev="$(pin_revision)"
  say "Chromium at $rev"
  mkdir -p "$CR"
  cd "$CR"
  cat > .gclient <<'EOF'
solutions = [
  {
    "name": "src",
    "url": "https://chromium.googlesource.com/chromium/src.git",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {},
  },
]
target_os = ['linux']
EOF
  if [ ! -d src/.git ]; then
    mkdir -p src
    git -C src init -q
    git -C src remote add origin https://chromium.googlesource.com/chromium/src.git
    git -C src fetch -q --depth=1 --no-tags origin "$rev"
    git -C src checkout -q --detach FETCH_HEAD
  fi
  [ "$(git -C src rev-parse HEAD)" = "$rev" ] || { echo "not at the pin"; exit 1; }
  gclient sync --revision "src@$rev" --no-history --shallow
  df -h / | tail -1
}

cmd_overlay() {
  say "Overlay"
  cp -R "$ROOT/src/." "$SRC/"
  [ -d "$SRC/chrome/browser/molt_ai" ] || { echo "overlay copy failed"; exit 1; }
  say "Rust crates (gnrt)"
  # The vendored tree is committed in src/ and a Mac build uses it without
  # gnrt, so it must be gnrt's whole output: every crate downloaded and
  # patched afresh (--fresh), then copied back and compared. Ignored files
  # count: crates' own .gitignore files hide some of theirs.
  "$ROOT/scripts/vendor-rust-crates.sh" --chromium-src "$SRC" --fresh
  if [ "${MOLT_VENDOR_DUMP:-0}" = 1 ]; then
    dump_vendored
  elif [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=all --ignored=traditional -- src/third_party/rust)" ]; then
    echo "::error::gnrt's output differs from the vendored tree in src/third_party/rust: commit it with tools/ci/molt_vendored_from_log.py on this log (git add -f)"
    dump_vendored
    exit 1
  fi
}

# What gnrt added, changed or removed under the overlay's src/third_party/rust
# (vendor-rust-crates.sh copied it back), printed into the log as a base64
# tar.gz between markers, with its SHA-256 and the removed paths, so a machine
# without a Chromium checkout can commit the vendored tree:
# tools/ci/molt_vendored_from_log.py <job log>.
dump_vendored() {
  local files="$STATE/vendored-files.txt" removed="$STATE/vendored-removed.txt"
  local archive="$STATE/vendored.tar.gz"
  # NUL-separated, so no file name is quoted or split.
  git -C "$ROOT" status --porcelain=v1 -z --untracked-files=all --ignored=traditional -- src/third_party/rust |
    python3 -I -c '
import sys
items = sys.stdin.buffer.read().split(b"\0")
keep, gone = [], []
i = 0
while i < len(items):
    entry = items[i]
    i += 1
    if not entry:
        continue
    xy, path = entry[:2], entry[3:]
    if b"R" in xy or b"C" in xy:
        i += 1
    (gone if b"D" in xy else keep).append(path)
open(sys.argv[1], "wb").write(b"".join(p + b"\0" for p in sorted(keep)))
open(sys.argv[2], "wb").write(b"".join(p + b"\n" for p in sorted(gone)))
' "$files" "$removed"
  local n_files n_removed
  n_files="$(tr -cd '\000' < "$files" | wc -c | tr -d ' ')"
  n_removed="$(wc -l < "$removed" | tr -d ' ')"
  say "Vendored: $n_files files added or changed, $n_removed removed"
  if [ "$n_files" = 0 ] && [ "$n_removed" = 0 ]; then
    echo "MOLT-VENDORED-NONE"
    return 0
  fi
  tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='2000-01-01 00:00:00Z' \
    --null -czf "$archive" -C "$ROOT" -T "$files"
  echo "MOLT-VENDORED-SHA256 $(sha256sum "$archive" | cut -d' ' -f1)"
  echo "MOLT-VENDORED-BEGIN"
  base64 -w 76 "$archive"
  echo "MOLT-VENDORED-END"
  echo "MOLT-VENDORED-REMOVED-BEGIN"
  cat "$removed"
  echo "MOLT-VENDORED-REMOVED-END"
}

cmd_restore() {
  local dir="$1"
  shopt -s nullglob
  local parts=("$dir"/molt-out.tar.zst.*)
  shopt -u nullglob
  if [ "${#parts[@]}" -eq 0 ]; then
    say "No out dir to restore: a build from scratch"
    rm -f "$STATE/previous-manifest.txt"
    return 0
  fi
  say "Restoring the out dir from ${#parts[@]} parts"
  mkdir -p "$SRC/out"
  cat "${parts[@]}" | zstd -d -q -T0 | tar -x -C "$SRC"
  rm -f "${parts[@]}"
  if [ -f "$dir/manifest.txt.zst" ]; then
    zstd -d -q -f "$dir/manifest.txt.zst" -o "$STATE/previous-manifest.txt"
  fi
  df -h / | tail -1
}

cmd_times() {
  local prev=()
  [ -f "$STATE/previous-manifest.txt" ] && [ -d "$OUT" ] && \
    prev=(--previous "$STATE/previous-manifest.txt")
  python3 "$ROOT/tools/ci/molt_ci_times.py" --tree "$SRC" \
    --write "$STATE/manifest.txt" "${prev[@]}"
}

cmd_gen() {
  if [ -f "$OUT/build.ninja" ]; then
    say "Out dir restored; ninja regenerates it if a BUILD.gn changed"
    return 0
  fi
  say "gn gen $OUT_REL"
  mkdir -p "$OUT"
  cat > "$OUT/args.gn" <<'EOF'
# MoltBrowser CI test build (tools/ci/molt_test_build.sh): a component
# build with DCHECKs on, for MoltBrowser's tests. Not a release build.
target_os = "linux"
target_cpu = "x64"
is_debug = false
is_official_build = false
is_component_build = true
dcheck_always_on = true
use_siso = false
use_remoteexec = false
symbol_level = 0
blink_symbol_level = 0
v8_symbol_level = 0
enable_nacl = false
is_clang = true
use_lld = true
use_sysroot = true
use_ozone = true
ozone_auto_platforms = true
google_api_key = ""
google_default_client_id = ""
google_default_client_secret = ""
safe_browsing_mode = 1
enable_reporting = false
enable_supervised_users = true
is_chrome_branded = false
enable_extensions = true
chrome_pgo_phase = 0
treat_warnings_as_errors = false
EOF
  cd "$SRC"
  buildtools/linux64/gn gen "$OUT_REL" 2>&1 | tee "$STATE/gn-gen.log"
}

cmd_build() {
  local minutes="$1"; shift
  local ninja rc status log="$STATE/ninja.log"
  ninja="$(ninja_bin)"
  cd "$SRC"
  if [ "$minutes" -lt 10 ]; then
    say "No time left to build in this stage"
    status=partial
  else
    say "ninja -k 0 $* for at most $minutes minutes"
    set +e
    timeout --signal=INT --kill-after=180 "${minutes}m" \
      "$ninja" -C "$OUT_REL" -k 0 "$@" > "$log" 2>&1
    rc=$?
    set -e
    tail -5 "$log"
    if [ "$rc" -eq 0 ]; then
      status=complete
    elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ] || grep -q "interrupted by user" "$log"; then
      status=partial
    else
      status=errors
    fi
  fi
  echo "$status" > "$STATE/status"
  grep -E '^FAILED: ' "$log" 2>/dev/null | sort -u > "$STATE/failed-steps.txt" || true
  grep -E 'error:|undefined symbol|duplicate symbol' "$log" 2>/dev/null \
    | sort | uniq -c | sort -rn | head -400 > "$STATE/errors.txt" || true
  # Each failed step with what it printed, up to the next step's line.
  awk '/^FAILED: /{p=1; n=0} p&&/^\[[0-9]+\/[0-9]+\] /{p=0}
       p{print; if (++n > 60) {print "  ..."; p=0}}' "$log" 2>/dev/null \
    | head -1500 > "$STATE/failures.txt" || true
  local progress
  progress="$(grep -oE '^\[[0-9]+/[0-9]+\]' "$log" 2>/dev/null | tail -1 || true)"
  say "Build status: $status ${progress} ($(wc -l < "$STATE/failed-steps.txt") failed steps)"
  output status "$status"
  output progress "$progress"
  {
    echo "### Build: $status ${progress}"
    echo
    echo "Failed steps: $(wc -l < "$STATE/failed-steps.txt")"
    if [ -s "$STATE/failed-steps.txt" ]; then
      echo
      echo '```'
      head -60 "$STATE/failed-steps.txt"
      echo '```'
      echo
      echo "First errors:"
      echo
      echo '```'
      head -80 "$STATE/errors.txt"
      echo '```'
    fi
  } | summary
}

cmd_save() {
  local dir="$1"
  mkdir -p "$dir"
  say "Saving the out dir"
  tar -C "$SRC" -cf - "$OUT_REL" | zstd -q -T0 -3 \
    | split -b 3800M -d -a 2 - "$dir/molt-out.tar.zst."
  zstd -q -f "$STATE/manifest.txt" -o "$dir/manifest.txt.zst"
  cp "$STATE/status" "$dir/status" 2>/dev/null || true
  for f in failed-steps.txt errors.txt failures.txt; do
    cp "$STATE/$f" "$dir/$f" 2>/dev/null || true
  done
  ls -l "$dir"
  df -h / | tail -1
}

# Every fixture of MoltBrowser's browser tests, as a gtest filter.
browser_test_filter() {
  local files
  files="$(git -C "$ROOT" ls-files 'src/*_browsertest.cc' | grep -E '/molt_ai/|molt_')" || true
  [ -n "$files" ] || return 0
  # shellcheck disable=SC2086
  (cd "$ROOT" && grep -ohE 'IN_PROC_BROWSER_TEST_[FP]\(\s*[A-Za-z0-9_]+' $files) \
    | sed -E 's/.*\(\s*//' | sort -u | sed 's/.*/*&.*/' | paste -sd: -
}

# Browser tests that need their window to keep the focus: a menu closes as
# another window takes it, and four tests at once on one display take it
# from each other ("open the menu" read the toolbar in 4 of 11 builds, the
# menu shown again or not). They run after the others, one at a time, as
# Chromium's interactive_ui_tests do.
FOCUS_TESTS="VoiceBrowserActionsBrowserTest.TheAppMenuByVoice"
FOCUS_TESTS+=":VoiceBrowserActionsBrowserTest.AMenuThatClosedByItselfIsShownAgainForThePress"

# A crashed test that printed nothing, run again on its own, in-process, so
# its output reaches the summary (molt_test_summary.py), at most 3.
rerun_silent_crashes() {
  local dir="$1" binary="$2" name
  mkdir -p "$dir/rerun"
  # Under gdb when there is one: a crash that prints nothing (a stack
  # overflow can take the crash handler with it) still gets its stack.
  local gdb=()
  if command -v gdb >/dev/null 2>&1 ||
      sudo apt-get install -y -qq gdb >/dev/null 2>&1; then
    gdb=(gdb -q -batch -ex "set pagination off" -ex run
         -ex "bt 40" -ex "bt -25" -ex "info registers rip rsp" --args)
  fi
  python3 "$ROOT/tools/ci/molt_test_summary.py" --silent-crashes "$dir" "$binary" |
    while read -r name; do
      [ -n "$name" ] || continue
      say "running $name again on its own"
      timeout 300 xvfb-run -a -s "-screen 0 1600x1200x24" "${gdb[@]}" \
        "$OUT/$binary" --single-process-tests --gtest_filter="$name" \
        > "$dir/rerun/$binary.$name.log" 2>&1 || true
    done
}

cmd_test() {
  local dir="$1" rc=0
  mkdir -p "$dir"
  cd "$SRC"
  # Ubuntu's AppArmor policy blocks the unprivileged user namespaces
  # Chromium's sandbox (and MoltBrowser's helper sandbox) use.
  sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0 >/dev/null 2>&1 || true
  sudo sysctl -w kernel.unprivileged_userns_clone=1 >/dev/null 2>&1 || true
  if [ -x "$OUT/molt_ai_unittests" ]; then
    say "molt_ai_unittests"
    set +e
    # One retry: a crash takes the rest of its batch with it (NOTRUN), and
    # the retry runs those on their own. A test that needed it is reported
    # as flaky, not passed (molt_test_summary.py).
    xvfb-run -a -s "-screen 0 1600x1200x24" "$OUT/molt_ai_unittests" \
      --test-launcher-jobs=4 --test-launcher-retry-limit=1 \
      --test-launcher-summary-output="$dir/molt_ai_unittests.json" \
      > "$dir/molt_ai_unittests.log" 2>&1
    rc=$((rc | $?))
    set -e
    tail -5 "$dir/molt_ai_unittests.log"
    rerun_silent_crashes "$dir" molt_ai_unittests
  else
    echo "molt_ai_unittests was not built" | tee "$dir/molt_ai_unittests.log"
    rc=1
  fi
  if [ -x "$OUT/browser_tests" ]; then
    local filter
    filter="$(browser_test_filter)"
    echo "$filter" | tr ':' '\n' > "$dir/browser_tests.filter"
    say "browser_tests: $(wc -l < "$dir/browser_tests.filter") MoltBrowser fixtures"
    set +e
    # A wait that never ends fails after 30 s (Chromium's own action
    # timeout), so its message is printed; the test as a whole may take
    # 150 s (one with many spoken steps on a slow DCHECK build). With the
    # action timeout at the test's, the launcher killed a stuck test before
    # its wait could say what it was waiting for.
    xvfb-run -a -s "-screen 0 1600x1200x24" "$OUT/browser_tests" \
      --gtest_filter="$filter-$FOCUS_TESTS" --test-launcher-jobs=4 \
      --test-launcher-retry-limit=1 --ui-test-action-max-timeout=30000 \
      --test-launcher-timeout=150000 \
      --test-launcher-summary-output="$dir/browser_tests.json" \
      > "$dir/browser_tests.log" 2>&1
    rc=$((rc | $?))
    tail -5 "$dir/browser_tests.log"
    # The tests that need the window's focus, one at a time on a display
    # of their own (molt_test_summary.py reads both result files).
    xvfb-run -a -s "-screen 0 1600x1200x24" "$OUT/browser_tests" \
      --gtest_filter="$FOCUS_TESTS" --test-launcher-jobs=1 \
      --test-launcher-retry-limit=1 --ui-test-action-max-timeout=30000 \
      --test-launcher-timeout=150000 \
      --test-launcher-summary-output="$dir/browser_tests.serial.json" \
      > "$dir/browser_tests.serial.log" 2>&1
    rc=$((rc | $?))
    set -e
    tail -3 "$dir/browser_tests.serial.log"
  else
    echo "browser_tests was not built" | tee "$dir/browser_tests.log"
    rc=1
  fi
  python3 "$ROOT/tools/ci/molt_test_summary.py" "$dir" > "$dir/summary.md" || rc=1
  # ninja -k 0 links what it can: after a failed step, a binary it feeds is
  # the last build's, and so are its tests' results.
  if [ "$(cat "$STATE/status" 2>/dev/null)" = errors ]; then
    {
      echo "> **The build had $(wc -l < "$STATE/failed-steps.txt") failed steps:**" \
        "the binaries they feed were not rebuilt, so these tests may have run" \
        "on an earlier build of this code."
      echo
      cat "$dir/summary.md"
    } > "$dir/summary.md.new"
    mv "$dir/summary.md.new" "$dir/summary.md"
  fi
  cat "$dir/summary.md"
  summary < "$dir/summary.md"
  output tests_rc "$rc"
  return 0
}

# The steps that failed in the restored build, rebuilt on their own: a fix
# is checked in minutes instead of after a whole stage.
cmd_recheck() {
  local dir="$1" ninja rc
  if [ ! -s "$dir/failed-steps.txt" ]; then
    say "The restored build recorded no failed steps"
    output recheck none
    return 0
  fi
  cp "$dir/failed-steps.txt" "$STATE/previous-failed-steps.txt"
  local targets=()
  # "FAILED: [code=1] obj/a.o obj/b.o" -> obj/a.o obj/b.o
  mapfile -t targets < <(sed -E 's/^FAILED: //; s/\[code=[0-9]+\] //' \
    "$dir/failed-steps.txt" | tr ' ' '\n' | sed '/^$/d' | sort -u)
  say "Rebuilding the ${#targets[@]} outputs that failed: ${targets[*]}"
  ninja="$(ninja_bin)"
  cd "$SRC"
  set +e
  "$ninja" -C "$OUT_REL" -k 0 "${targets[@]}" > "$STATE/ninja.log" 2>&1
  rc=$?
  set -e
  tail -5 "$STATE/ninja.log"
  grep -E '^FAILED: ' "$STATE/ninja.log" | sort -u > "$STATE/failed-steps.txt" || true
  awk '/^FAILED: /{p=1; n=0} p&&/^\[[0-9]+\/[0-9]+\] /{p=0}
       p{print; if (++n > 60) {print "  ..."; p=0}}' "$STATE/ninja.log" \
    | head -1500 > "$STATE/failures.txt" || true
  if [ "$rc" -eq 0 ]; then output recheck fixed; else output recheck failing; fi
  say "Recheck: ninja exit $rc, $(wc -l < "$STATE/failed-steps.txt") still failing"
}

cmd_failures() {
  if [ -s "$STATE/failures.txt" ]; then
    echo "==> $(wc -l < "$STATE/failed-steps.txt") failed steps:"
    cat "$STATE/failed-steps.txt"
    echo "==> What they printed:"
    cat "$STATE/failures.txt"
  else
    echo "==> No failed build steps."
  fi
}

main() {
  local c="${1:-}"
  [ -n "$c" ] || { sed -n "16,28p" "$0"; exit 2; }
  shift
  case "$c" in
    deps) cmd_deps ;;
    sync) cmd_sync ;;
    overlay) cmd_overlay ;;
    restore) cmd_restore "$@" ;;
    times) cmd_times ;;
    gen) cmd_gen ;;
    build) cmd_build "$@" ;;
    save) cmd_save "$@" ;;
    test) cmd_test "$@" ;;
    failures) cmd_failures ;;
    recheck) cmd_recheck "$@" ;;
    *) echo "unknown command: $c"; exit 2 ;;
  esac
}

main "$@"
