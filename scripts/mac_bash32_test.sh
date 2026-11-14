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

echo "all passed"
