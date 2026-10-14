#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Apply MoltBrowser to a Chromium checkout the patch-based way.

The alternative to `cp -R src/. chromium/src/` (still the default, see
scripts/setup.sh --patches): instead of overwriting upstream files with whole
copies, it

  1. checks chromium/src is at the pin in chromium_version.txt,
  2. applies patches/series with `git apply -p1` (all-or-nothing),
  3. copies the first-party files (src/ files that are not upstream) and the
     DEPS whole files (patches/whole_files.txt).

Upstream files listed in patches/identical.txt are left alone, so no
upstream change is silently reverted. Network-free: it only reads src/,
patches/ and the checkout.

Usage:
  tools/chromium/apply_patches.py [--chromium-src DIR] [--reset] [--check]
    --reset  first restore every patched file to upstream@pin (for a warm
             checkout that already has the overlay or the patches applied)
    --check  only report whether the series applies; change nothing
"""
import argparse
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import upstream as up  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--chromium-src",
                    default=str(up.REPO_ROOT / "chromium" / "src"))
    ap.add_argument("--reset", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    pin = up.read_pin()
    root = Path(args.chromium_src).resolve()
    head = up.run_git(["-C", str(root), "rev-parse", "HEAD"]).decode().strip()
    if head != pin.revision:
        up.die("%s is at %s, but chromium_version.txt pins %s.\n"
               "Run scripts/setup.sh --resync first." % (root, head, pin))

    patched = up.series_paths()
    patch_files = [up.PATCHES_DIR / (p + up.PATCH_SUFFIX) for p in patched]
    identical = set(up.read_list(up.IDENTICAL))
    whole = up.read_list(up.WHOLE_FILES)

    if args.reset and not args.check:
        up.log("Restoring %d patched files to upstream@pin ..." % len(patched))
        up.run_git(["-C", str(root), "checkout", pin.revision, "--"] + patched)

    try:
        up.apply_patches(root, patch_files, check_only=True)
    except RuntimeError as e:
        up.die("patches/series does not apply cleanly to %s (already "
               "applied? try --reset):\n%s" % (root, e))
    if args.check:
        print("OK: all %d patches apply to %s" % (len(patch_files), root))
        return 0
    up.apply_patches(root, patch_files)

    skip = set(patched) | identical
    copied = 0
    for path in up.overlay_files():
        if path in skip:
            continue
        dst = root / path
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(up.OVERLAY_DIR / path, dst)
        copied += 1

    print("Applied %d patches and copied %d files (%d first-party + %d DEPS "
          "whole files) into %s" % (len(patch_files), copied,
                                    copied - len(whole), len(whole), root))
    print("Left %d upstream files untouched (patches/identical.txt)."
          % len(identical))
    return 0


if __name__ == "__main__":
    sys.exit(main())
