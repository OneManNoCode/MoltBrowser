#!/bin/bash
# Shared verification helpers for the bundle-*.sh scripts.
# Copyright 2026 GenEye AI Labs Inc.
#
# Source this from a bundle script with:
#   source "$(dirname "${BASH_SOURCE[0]}")/lib-bundle-verify.sh"
#
# Policy: FAIL CLOSED. Every downloaded artifact must match a pinned SHA-256.
#   * mismatch                      -> always fatal
#   * no pin (empty EXPECTED_*)     -> fatal, unless the developer opts out
#                                      with MOLT_ALLOW_UNPINNED_DOWNLOADS=1
#                                      (logged loudly; dev builds only)
#   * MOLT_RELEASE_BUILD=1          -> the opt-out is ignored. The release
#                                      scripts and workflows set this.
#
# Adds:
#   sha256_of <file>           -> echoes the SHA256 hex of <file>
#   verify_sha256 <file> <expected>
#                              -> 0 on match; 1 on mismatch; 2 if <expected>
#                                 is empty (nothing pinned)
#   unpinned_allowed           -> 0 if an unpinned artifact may be used (the
#                                 dev opt-out above), else 1. Bundle scripts
#                                 use it for their own pins (e.g. commits).
#   verify_or_redownload <file> <expected> <url>
#                              -> verify cached <file>; if mismatch, delete
#                                 + redownload from <url> + verify again;
#                                 fail if the fresh download also mismatches
#                                 or nothing is pinned (see policy)
#
# Why a shared lib instead of duplicating in each bundle-X.sh: one place
# owns the verification posture.

set -e

# Cross-platform shasum: macOS ships `shasum -a 256`, Linux usually has
# `sha256sum`. Pick whichever exists.
_sha256_cmd=""
if command -v sha256sum >/dev/null 2>&1; then
  _sha256_cmd="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
  _sha256_cmd="shasum -a 256"
else
  echo "[bundle-verify] FATAL: no sha256sum / shasum found on PATH"
  exit 1
fi

sha256_of() {
  local f="$1"
  if [[ ! -f "$f" ]]; then
    echo ""
    return 1
  fi
  # Both tools emit "<hex>  <path>"; we want just the hex.
  $_sha256_cmd "$f" | awk '{print $1}'
}

unpinned_allowed() {
  if [[ "${MOLT_RELEASE_BUILD:-0}" == "1" ]]; then
    return 1
  fi
  [[ "${MOLT_ALLOW_UNPINNED_DOWNLOADS:-0}" == "1" ]]
}

_unpinned_verdict() {
  local what="$1"
  if unpinned_allowed; then
    echo "[bundle-verify] WARNING: using UNVERIFIED $what because"
    echo "[bundle-verify]          MOLT_ALLOW_UNPINNED_DOWNLOADS=1. Dev builds only."
    return 0
  fi
  echo "[bundle-verify] FATAL: nothing pinned for $what - refusing to use it."
  echo "[bundle-verify]        Verify the artifact against a trusted source"
  echo "[bundle-verify]        (signed checksums, the publisher's release page)"
  echo "[bundle-verify]        and paste its SHA-256 into the calling script."
  if [[ "${MOLT_RELEASE_BUILD:-0}" == "1" ]]; then
    echo "[bundle-verify]        (MOLT_RELEASE_BUILD=1: no override possible.)"
  else
    echo "[bundle-verify]        Dev-only override: MOLT_ALLOW_UNPINNED_DOWNLOADS=1"
  fi
  return 1
}

verify_sha256() {
  local f="$1"
  local expected="$2"
  if [[ -z "$expected" ]]; then
    echo "[bundle-verify] no expected SHA-256 pinned for $f"
    echo "[bundle-verify]   actual = $(sha256_of "$f")"
    return 2
  fi
  local actual
  actual=$(sha256_of "$f")
  if [[ "$actual" != "$expected" ]]; then
    echo "[bundle-verify] FATAL: SHA256 mismatch for $f"
    echo "[bundle-verify]          expected = $expected"
    echo "[bundle-verify]          actual   = $actual"
    return 1
  fi
  echo "[bundle-verify] OK: $f  $actual"
  return 0
}

# verify_or_redownload <cache_file> <expected_sha> <url>
# Used by bundle scripts that cache an artifact in .molt-<tool>-cache/ and
# re-verify it on every run. A cached copy that fails verification is
# deleted and downloaded again from the canonical URL; if the fresh
# download also fails, exit 1 - the upstream is compromised or the
# expected SHA is wrong.
verify_or_redownload() {
  local cache_file="$1"
  local expected="$2"
  local url="$3"
  local rc
  if [[ -f "$cache_file" ]]; then
    rc=0
    verify_sha256 "$cache_file" "$expected" || rc=$?
    case $rc in
      0) return 0 ;;
      2) _unpinned_verdict "$cache_file" || return 1; return 0 ;;
      *) echo "[bundle-verify] cache file tampered or stale - redownloading"
         rm -f "$cache_file" ;;
    esac
  fi
  echo "[bundle-verify] downloading $url"
  if ! curl -fL --retry 2 -o "${cache_file}.tmp" "$url"; then
    rm -f "${cache_file}.tmp"
    echo "[bundle-verify] FATAL: download failed"
    return 1
  fi
  rc=0
  verify_sha256 "${cache_file}.tmp" "$expected" || rc=$?
  case $rc in
    0) ;;
    2) if ! _unpinned_verdict "$url"; then
         rm -f "${cache_file}.tmp"
         return 1
       fi ;;
    *) echo "[bundle-verify] FATAL: fresh download also fails verification."
       echo "[bundle-verify]        either the upstream is compromised or the"
       echo "[bundle-verify]        pinned expected SHA needs updating."
       rm -f "${cache_file}.tmp"
       return 1 ;;
  esac
  mv "${cache_file}.tmp" "$cache_file"
  return 0
}
