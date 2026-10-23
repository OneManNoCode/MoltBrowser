#!/bin/bash
# MoltBrowser release asset names
# Copyright 2026 GenEye AI Labs Inc. Licensed under GPLv3.
#
# The names every release path gives the installers it uploads: fixed, with
# no version in them, because
#   * the in-app updater looks for them: UpdateManager::PlatformAssetName()
#     (src/chrome/browser/molt_ai/update/update_manager.cc) builds the
#     download URL from the name when the GitHub API is unreachable, and
#     PlatformAssetMatcher() picks the asset whose name contains ".dmg",
#     "Setup.exe", ".deb", ".rpm" or ".tar.gz";
#   * the signed release manifest then lists them under these names
#     (tools/release_signing/molt_release_sign.py);
#   * README's /releases/latest/download/ links use them.
#
# Sourced by release.sh, release-all.sh, package-linux.sh, release-linux.sh,
# release-win.sh and build/docker/build-linux-fresh.sh; the release
# workflows spell the same names out. scripts/release_asset_names_test.py
# (CI: lint.yml) checks the names against the updater and the release
# tooling, runs package-linux.sh, and fails on a versioned installer name in
# a release script or workflow.

# shellcheck disable=SC2034  # used by the scripts that source this file
MOLT_ASSET_MACOS_ARM64_DMG="MoltBrowser-macOS-arm64.dmg"
MOLT_ASSET_MACOS_UNIVERSAL_DMG="MoltBrowser-macOS-universal.dmg"
MOLT_ASSET_LINUX_DEB="MoltBrowser-Linux-x64.deb"
MOLT_ASSET_LINUX_RPM="MoltBrowser-Linux-x64.rpm"
MOLT_ASSET_LINUX_TAR="MoltBrowser-Linux-x64.tar.gz"
MOLT_ASSET_LINUX_APPIMAGE="MoltBrowser-Linux-x64.AppImage"
MOLT_ASSET_WINDOWS_SETUP="MoltBrowser-Windows-x64-Setup.exe"
MOLT_ASSET_WINDOWS_ZIP="MoltBrowser-Windows-x64.zip"
