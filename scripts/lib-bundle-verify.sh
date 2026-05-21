#!/bin/bash
# Shared verification helpers for the bundle-*.sh scripts.
# Copyright 2026 GenEye AI Labs Inc.
#
# Source this from a bundle script with:
#   source "$(dirname "${BASH_SOURCE[0]}")/lib-bundle-verify.sh"
#
# Adds:
#   sha256_of <file>           -> echoes the SHA256 hex of <file>
#   verify_sha256 <file> <expected>
#                              -> exits 1 if checksum mismatches; logs and
#                                 returns 0 on match; returns 2 if <expected>
#                                 is empty (caller decides whether to warn
#                                 or fail; we default to logging only)
#   verify_or_redownload <file> <expected> <url>
#                              -> verify cached <file>; if mismatch, delete
#                                 + redownload from <url> + verify again;
#                                 fail hard if the fresh download also
#                                 mismatches
#
# Why a shared lib instead of duplicating in each bundle-X.sh: when we
# eventually publish a signed-binaries policy doc we want a single
# place to change the verification posture.

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

verify_sha256() {
  local f="$1"
  local expected="$2"
  if [[ -z "$expected" ]]; then
    echo "[bundle-verify] WARNING: no expected SHA pinned for $f."
    echo "[bundle-verify]          actual = $(sha256_of "$f")"
    echo "[bundle-verify]          paste this hex into the EXPECTED_SHA_*"
    echo "[bundle-verify]          variable in the calling script after"
    echo "[bundle-verify]          you've verified the artifact against a"
    echo "[bundle-verify]          trusted source (signed .sha256sum,"
    echo "[bundle-verify]          GitHub release notes, etc.)."
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
# Used by bundle scripts that cache an artifact in
# ~/.molt-<tool>-cache/ and re-verify it on every run. If the cached
# copy was tampered with locally, we redownload from the canonical URL
# and verify again. If the fresh download also fails verification,
# exit 1 — the upstream is compromised or the expected SHA is wrong.
verify_or_redownload() {
  local cache_file="$1"
  local expected="$2"
  local url="$3"
  if [[ -f "$cache_file" ]]; then
    if verify_sha256 "$cache_file" "$expected"; then
      return 0
    fi
    if [[ -n "$expected" ]]; then
      echo "[bundle-verify] cache file tampered or stale — redownloading"
      rm -f "$cache_file"
    else
      # No expected SHA yet — let the caller proceed with a warning.
      return 0
    fi
  fi
  echo "[bundle-verify] downloading $url"
  if ! curl -fL --retry 2 -o "${cache_file}.tmp" "$url"; then
    rm -f "${cache_file}.tmp"
    echo "[bundle-verify] FATAL: download failed"
    return 1
  fi
  mv "${cache_file}.tmp" "$cache_file"
  if ! verify_sha256 "$cache_file" "$expected"; then
    if [[ -n "$expected" ]]; then
      echo "[bundle-verify] FATAL: fresh download also fails verification."
      echo "[bundle-verify]        either the upstream is compromised or the"
      echo "[bundle-verify]        pinned expected SHA needs updating."
      rm -f "$cache_file"
      return 1
    fi
    # No expected SHA pinned — proceed with warning already logged.
  fi
  return 0
}
