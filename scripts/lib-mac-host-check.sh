#!/bin/bash
# MoltBrowser: can this Mac build, and run, the pinned Chromium?
# Copyright 2026 GenEye AI Labs Inc. Licensed under GPLv3.
#
# Sourced by configure.sh, which refuses before `gn gen`, and setup.sh,
# which warns once the checkout is synced. Without it a Mac whose Xcode is
# too old fails in `gn gen` with a find_sdk.py traceback, and build.sh then
# with "build.ninja not found".
#
# It reads the checkout itself, so it follows the pin:
#   * mac_sdk_min (build/config/mac/mac_sdk_overrides.gni, else
#     mac_sdk.gni): the oldest macOS SDK that builds it. Like
#     //build/mac/find_sdk.py, the SDKs are the MacOSX<major>.<minor>.sdk
#     entries in `xcode-select -print-path`/Platforms/MacOSX.platform/
#     Developer/SDKs (the Command Line Tools alone have none there);
#   * mac_min_system_version (mac_sdk.gni): the oldest macOS the built
#     browser runs on;
#   * mac_sdk_official_version (mac_sdk.gni): the SDK Chromium's own builds
#     use, the one sure to work.
#
# macOS's own /bin/bash is 3.2: scripts/mac_bash32_test.sh runs this there,
# with stand-in sw_vers, xcode-select and uname.

# molt_version_ge A B: whether the dotted version A is at least B, compared
# number by number ("15" is "15.0"; anything but digits is ignored).
molt_version_ge() {
  local a="$1" b="$2" x y
  while [ -n "$a" ] || [ -n "$b" ]; do
    x="${a%%.*}"
    y="${b%%.*}"
    x="${x//[!0-9]/}"
    y="${y//[!0-9]/}"
    x="${x:-0}"
    y="${y:-0}"
    if [ "$x" -gt "$y" ]; then
      return 0
    fi
    if [ "$x" -lt "$y" ]; then
      return 1
    fi
    if [ "$a" = "${a#*.}" ]; then a=""; else a="${a#*.}"; fi
    if [ "$b" = "${b#*.}" ]; then b=""; else b="${b#*.}"; fi
  done
  return 0
}

# molt_gni_value FILE NAME: the quoted value of `NAME = "value"` in FILE.
molt_gni_value() {
  sed -n 's/^[[:space:]]*'"$2"'[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
    "$1" 2>/dev/null | head -n 1
}

# molt_mac_host_check CHROMIUM_SRC MODE: on a Mac, checks this Mac against
# the checkout at CHROMIUM_SRC. A Mac with no SDK new enough gets an
# explanation: MODE "refuse" then returns 1, MODE "warn" returns 0. A macOS
# too old to run the result gets a note either way. Not a Mac, or no
# checkout yet: nothing.
molt_mac_host_check() {
  local src="$1" mode="$2"
  local gni="$src/build/config/mac/mac_sdk.gni"
  local overrides="$src/build/config/mac/mac_sdk_overrides.gni"
  local sdk_min="" run_min="" official="" host="" dev="" sdk_dir=""
  local found="" have="" entry v level

  [ "$(uname -s)" = Darwin ] || return 0
  [ -f "$gni" ] || return 0

  if [ -f "$overrides" ]; then
    sdk_min="$(molt_gni_value "$overrides" mac_sdk_min)"
  fi
  if [ -z "$sdk_min" ]; then
    sdk_min="$(molt_gni_value "$gni" mac_sdk_min)"
  fi
  run_min="$(molt_gni_value "$gni" mac_min_system_version)"
  official="$(molt_gni_value "$gni" mac_sdk_official_version)"
  host="$(sw_vers -productVersion 2>/dev/null || true)"

  if [ -n "$run_min" ] && [ -n "$host" ] &&
     ! molt_version_ge "$host" "$run_min"; then
    echo "NOTE: this Mac runs macOS $host. MoltBrowser built from this"
    echo "  checkout needs macOS $run_min or later, so it won't open here."
  fi

  [ -n "$sdk_min" ] || return 0

  dev="$(xcode-select -print-path 2>/dev/null || true)"
  if [ -n "$dev" ]; then
    sdk_dir="$dev/Platforms/MacOSX.platform/Developer/SDKs"
  fi
  if [ -n "$sdk_dir" ] && [ -d "$sdk_dir" ]; then
    for entry in "$sdk_dir"/MacOSX*.sdk; do
      v="${entry##*/}"
      v="${v#MacOSX}"
      v="${v%.sdk}"
      # find_sdk.py counts MacOSX<major>.<minor>.sdk only.
      case "$v" in
        *[!0-9.]* | "" | .* | *. | *.*.*) continue ;;
        *.*) ;;
        *) continue ;;
      esac
      have="$have $v"
      if molt_version_ge "$v" "$sdk_min"; then
        found="$v"
      fi
    done
  fi
  [ -z "$found" ] || return 0

  if [ "$mode" = refuse ]; then level=ERROR; else level=WARNING; fi
  echo "$level: building this checkout needs the macOS $sdk_min SDK or newer."
  if [ -z "$sdk_dir" ] || [ ! -d "$sdk_dir" ]; then
    echo "  No Xcode is selected (xcode-select -print-path: ${dev:-none});"
    echo "  the Command Line Tools alone can't build Chromium."
  else
    echo "  The selected Xcode ($dev) has:${have:- no versioned SDK}."
  fi
  echo "  This Mac runs macOS ${host:-unknown}. Install the newest Xcode it"
  echo "  can take (the App Store, or developer.apple.com/download/all), then:"
  echo "    sudo xcode-select -s /Applications/Xcode.app"
  echo "    sudo xcodebuild -license accept"
  echo "  Xcode 16 brings the macOS 15 SDK and needs macOS 14.5 or later;"
  echo "  Xcode 26 (Chromium's own builds use the ${official:-newest} SDK)"
  echo "  needs macOS 15.6 or later. A Mac that can't run a new enough"
  echo "  Xcode can't build MoltBrowser."
  if [ "$mode" = refuse ]; then
    return 1
  fi
  return 0
}
