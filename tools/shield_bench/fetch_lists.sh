#!/bin/bash
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# Downloads the real filter lists MoltShield ships by default, plus
# adblock-rust's real-world request corpus, into tools/shield_bench/lists/
# (gitignored) for `cargo test` / `cargo run` in this directory.
#
# Uses GitHub mirrors of the official lists (easylist.to / ublockorigin.github.io
# publish the same files) so it works where only raw.githubusercontent.com is
# reachable.

set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lists"
mkdir -p "$DIR"

fetch() {
  local name="$1" url="$2"
  echo "fetching $name"
  curl -fsSL --retry 3 -o "$DIR/$name.tmp" "$url"
  mv "$DIR/$name.tmp" "$DIR/$name"
}

fetch easylist.txt        https://raw.githubusercontent.com/easylist/easylist/gh-pages/easylist.txt
fetch easyprivacy.txt     https://raw.githubusercontent.com/easylist/easylist/gh-pages/easyprivacy.txt
fetch ubo-filters.txt     https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/filters.txt
fetch ubo-privacy.txt     https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/privacy.txt
fetch ubo-badware.txt     https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/badware.txt
fetch ubo-quick-fixes.txt https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/quick-fixes.txt
# Opt-in in the catalog (ublock-cookies); tested, not benchmarked.
fetch ubo-cookies.txt     https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/annoyances-cookies.txt
# ~240k requests recorded on top sites, from adblock-rust's own benchmarks.
fetch requests.json       https://raw.githubusercontent.com/brave/adblock-rust/v0.13.3/data/requests.json

ls -la "$DIR"
