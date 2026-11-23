#!/bin/bash
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# The release scripts' bash-sensitive paths under the bash running this
# script: macOS's own /bin/bash 3.2 in the Mac release preflight workflow
# (.github/workflows/mac-release-preflight.yml), which runs it as
# `/bin/bash scripts/mac_bash32_test.sh`. The owner's release Mac runs the
# release scripts with that bash: no mapfile, and "${a[@]}" of an empty
# array under `set -u` is an error before bash 4.4.
#
# No build and no network: the tools the scripts call are stand-ins.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "bash: $BASH_VERSION"

# release-sbom.sh reads one SBOM path per artifact from molt_sbom.py
# generate, then runs molt_sbom.py validate. The stand-in python3 prints
# "<artifact>.spdx.json" for each --artifact and accepts validate.
mkdir -p "$tmp/bin"
cat > "$tmp/bin/python3" <<'STUB'
#!/bin/bash
shift
cmd="$1"
shift
if [ "$cmd" = generate ]; then
  while [ $# -gt 0 ]; do
    if [ "$1" = --artifact ]; then
      echo "$2.spdx.json"
      shift
    fi
    shift
  done
fi
exit 0
STUB
chmod +x "$tmp/bin/python3"
touch "$tmp/a.dmg" "$tmp/b.dmg"

out="$(PATH="$tmp/bin:$PATH" "$BASH" "$ROOT/scripts/release-sbom.sh" \
  --platform macos --arch arm64 --version 0.0.1 "$tmp/a.dmg" "$tmp/b.dmg")"
want="$(printf '%s\n%s' "$tmp/a.dmg.spdx.json" "$tmp/b.dmg.spdx.json")"
if [ "$out" != "$want" ]; then
  echo "release-sbom.sh printed:" >&2
  echo "$out" >&2
  exit 1
fi
echo "release-sbom.sh: ok"

# A generate that prints fewer paths than artifacts fails the script.
cat > "$tmp/bin/python3" <<'STUB'
#!/bin/bash
exit 0
STUB
if PATH="$tmp/bin:$PATH" "$BASH" "$ROOT/scripts/release-sbom.sh" \
  --platform macos --arch arm64 --version 0.0.1 "$tmp/a.dmg" 2>/dev/null; then
  echo "release-sbom.sh accepted a generate that wrote no SBOM" >&2
  exit 1
fi
echo "release-sbom.sh refuses a missing SBOM: ok"

# The guard bundle-speech.sh uses for arrays that may be empty
# (target_args, GENERATOR): no words when empty, each element one word.
empty=()
set -- ${empty[@]+"${empty[@]}"}
[ $# -eq 0 ] || { echo "an empty array gave $# words" >&2; exit 1; }
two=(--target "re 2")
set -- ${two[@]+"${two[@]}"}
[ $# -eq 2 ] && [ "$2" = "re 2" ] || { echo "a two-element array gave $# words" >&2; exit 1; }
echo "empty-array guard: ok"

# Every "${NAME[@]}" in bundle-speech.sh whose array starts empty is guarded.
unguarded="$(grep -nE '"\$\{(GENERATOR|target_args)\[@\]\}"' "$ROOT/scripts/bundle-speech.sh" |
  grep -vE '\$\{(GENERATOR|target_args)\[@\]\+' || true)"
if [ -n "$unguarded" ]; then
  echo "bundle-speech.sh expands an array that may be empty without the guard:" >&2
  echo "$unguarded" >&2
  exit 1
fi
echo "bundle-speech.sh guards: ok"

# lib-mac-host-check.sh: configure.sh refuses, and setup.sh warns, on a Mac
# whose Xcode has no SDK new enough for the checkout, as find_sdk.py counts
# them (MacOSX<major>.<minor>.sdk), and notes a macOS too old to run the
# result. Stand-in sw_vers, xcode-select and uname; a stand-in checkout
# pinning the 15 SDK, macOS 13 to run, and the 26.5 SDK for official builds.
mkdir -p "$tmp/hostbin" "$tmp/cr/build/config/mac"
printf '  mac_min_system_version = "13.0"\n  mac_sdk_official_version = "26.5"\n' \
  > "$tmp/cr/build/config/mac/mac_sdk.gni"
printf '  mac_sdk_min = "15"\n' > "$tmp/cr/build/config/mac/mac_sdk_overrides.gni"
cat > "$tmp/hostbin/uname" <<'STUB'
#!/bin/bash
echo "${MOLT_TEST_UNAME:-Darwin}"
STUB
cat > "$tmp/hostbin/sw_vers" <<'STUB'
#!/bin/bash
echo "$MOLT_TEST_MACOS"
STUB
cat > "$tmp/hostbin/xcode-select" <<'STUB'
#!/bin/bash
[ -n "$MOLT_TEST_DEV" ] || { echo "no developer directory" >&2; exit 2; }
echo "$MOLT_TEST_DEV"
STUB
chmod +x "$tmp/hostbin/uname" "$tmp/hostbin/sw_vers" "$tmp/hostbin/xcode-select"
for d in xc13/MacOSX.sdk xc13/MacOSX13.1.sdk xc13/MacOSX13.sdk \
         xc15bare/MacOSX15.sdk xc15bare/MacOSX14.5.sdk \
         xc26/MacOSX.sdk xc26/MacOSX26.5.sdk; do
  mkdir -p "$tmp/${d%%/*}/Platforms/MacOSX.platform/Developer/SDKs/${d#*/}"
done

# host_check MACOS DEV MODE [SRC]: runs the check; prints its output, then
# "rc=<status>".
host_check() {
  local rc=0 out
  out="$(PATH="$tmp/hostbin:$PATH" MOLT_TEST_MACOS="$1" MOLT_TEST_DEV="$2" \
    "$BASH" -c 'set -euo pipefail; source "$1"; molt_mac_host_check "$2" "$3"' \
    _ "$ROOT/scripts/lib-mac-host-check.sh" "${4:-$tmp/cr}" "$3")" || rc=$?
  [ -z "$out" ] || printf '%s\n' "$out"
  printf 'rc=%s\n' "$rc"
}
expect() {  # expect NAME OUTPUT PATTERN...: each grep -E pattern matches
  local name="$1" out="$2" pat
  shift 2
  for pat in "$@"; do
    if ! printf '%s\n' "$out" | grep -qE -- "$pat"; then
      echo "$name: no match for /$pat/ in:" >&2
      printf '%s\n' "$out" >&2
      exit 1
    fi
  done
}

out="$(host_check 12.7.6 "$tmp/xc13" refuse)"
expect "Monterey with Xcode 14" "$out" '^ERROR: .*macOS 15 SDK' 'has: 13\.1\.$' \
  'runs macOS 12\.7\.6' "won't open here" 'Xcode 26 .*26\.5' '^rc=1$'
out="$(host_check 12.7.6 "$tmp/xc13" warn)"
expect "setup.sh's warning" "$out" '^WARNING: ' '^rc=0$'
out="$(host_check 15.6.1 "$tmp/xc15bare" refuse)"
expect "an unversioned MacOSX15.sdk" "$out" 'has: 14\.5\.$' '^rc=1$'
out="$(host_check 15.6.1 "" refuse)"
expect "no Xcode" "$out" 'No Xcode is selected' '^rc=1$'
out="$(host_check 26.1 "$tmp/xc26" refuse)"
[ "$out" = "rc=0" ] || { echo "Xcode 26 on macOS 26 printed: $out" >&2; exit 1; }
out="$(MOLT_TEST_UNAME=Linux host_check 12.7.6 "$tmp/xc13" refuse)"
[ "$out" = "rc=0" ] || { echo "a Linux host printed: $out" >&2; exit 1; }
out="$(host_check 12.7.6 "$tmp/xc13" refuse "$tmp/no-checkout")"
[ "$out" = "rc=0" ] || { echo "no checkout printed: $out" >&2; exit 1; }
(
  # shellcheck source=scripts/lib-mac-host-check.sh
  source "$ROOT/scripts/lib-mac-host-check.sh"
  molt_version_ge 15.0 15 && molt_version_ge 26.5 15 && molt_version_ge 13.0 13.0 &&
    molt_version_ge 15.10 15.9 && ! molt_version_ge 14.5 15 &&
    ! molt_version_ge 12.7.6 13.0 && ! molt_version_ge 13 13.1
) || { echo "molt_version_ge compared wrongly" >&2; exit 1; }
echo "lib-mac-host-check.sh: ok"

# configure.sh --dev: the Mac config, not official (so no ThinLTO), with
# DCHECKs off. release.sh then refuses that out dir, and a --debug one,
# before building anything; an official one gets past the check. A
# stand-in repo root with a stand-in gn (writes its --args to args.gn) and
# a Linux uname (no host check).
dev="$tmp/devroot"
mkdir -p "$dev/scripts" "$dev/configs" "$dev/chromium/src" "$tmp/devbin"
cp "$ROOT/scripts/configure.sh" "$ROOT/scripts/lib-mac-host-check.sh" \
   "$ROOT/scripts/release.sh" "$ROOT/scripts/lib-release-names.sh" "$dev/scripts/"
cp "$ROOT/configs/macos-x64.gn" "$dev/configs/"
cat > "$tmp/devbin/gn" <<'STUB'
#!/bin/bash
mkdir -p "$2"
printf '%s\n' "${3#--args=}" > "$2/args.gn"
STUB
cat > "$tmp/devbin/uname" <<'STUB'
#!/bin/bash
case "$1" in -m) echo x86_64 ;; *) echo Linux ;; esac
STUB
chmod +x "$tmp/devbin/gn" "$tmp/devbin/uname"
args="$dev/chromium/src/out/MoltBrowser/args.gn"
PATH="$tmp/devbin:$PATH" "$BASH" "$dev/scripts/configure.sh" \
  --platform macos --arch x64 --dev > "$tmp/dev.log" 2>&1 ||
  { echo "configure.sh --dev failed:" >&2; cat "$tmp/dev.log" >&2; exit 1; }
grep -q '^is_official_build = false$' "$args" &&
  grep -q '^dcheck_always_on = false$' "$args" &&
  grep -q '^target_cpu = "x64"$' "$args" &&
  ! grep -q 'is_official_build = true' "$args" ||
  { echo "configure.sh --dev wrote:" >&2; cat "$args" >&2; exit 1; }
if PATH="$tmp/devbin:$PATH" "$BASH" "$dev/scripts/configure.sh" \
     --platform macos --arch x64 --dev --debug > /dev/null 2>&1; then
  echo "configure.sh took --dev with --debug" >&2
  exit 1
fi
release_out() {  # release_out: release.sh's output on the stand-in root
  PATH="$tmp/devbin:$PATH" "$BASH" "$dev/scripts/release.sh" \
    --version 0.0.1 --build 1 --skip-build 2>&1 || true
}
expect "release.sh on a --dev out dir" "$(release_out)" "isn't an official release build"
sed 's/is_official_build = false/is_official_build = true/; s/is_debug = false/is_debug = true/' \
  "$args" > "$args.new" && mv "$args.new" "$args"
expect "release.sh on a --debug out dir" "$(release_out)" "isn't an official release build"
sed 's/is_debug = true/is_debug = false/' "$args" > "$args.new" && mv "$args.new" "$args"
out="$(release_out)"
if printf '%s\n' "$out" | grep -q "official release build"; then
  echo "release.sh refused an official out dir:" >&2
  printf '%s\n' "$out" >&2
  exit 1
fi
expect "release.sh on an official out dir" "$out" 'Patching Info.plist'
echo "configure.sh --dev and release.sh's refusal: ok"

echo "all passed"
