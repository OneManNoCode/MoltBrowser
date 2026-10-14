#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Prove patches/ reproduces the src/ overlay byte-for-byte.

1. Fetches upstream@pin (chromium_version.txt) for every file in
   patches/series and patches/identical.txt into a scratch tree.
2. Applies the whole series there with `git apply -p1` (the same command
   the patch-based build uses).
3. Compares every patched file, and every identical.txt file, with src/.
4. Checks the series is complete: every src/ file that exists upstream at
   the pin is either patched, listed as identical, or a DEPS whole file.

Exits non-zero on any mismatch; run tools/chromium/overlay_to_patches.py to
regenerate after changing an upstream file under src/.

Usage:
  tools/chromium/verify_patches.py [--cache DIR] [--remote URL] [--offline]
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import upstream as up  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cache", default=str(up.DEFAULT_CACHE))
    ap.add_argument("--remote", default=up.DEFAULT_REMOTE)
    ap.add_argument("--offline", action="store_true")
    ap.add_argument("--keep", action="store_true",
                    help="keep the scratch tree and print its path")
    args = ap.parse_args()

    pin = up.read_pin()
    chromium = up.Upstream(args.cache, args.remote, args.offline)
    commit = chromium.resolve(pin.revision)
    tree = chromium.tree(commit)

    patched = up.series_paths()
    identical = up.read_list(up.IDENTICAL)
    whole = up.read_list(up.WHOLE_FILES)
    problems = []

    # Every patch must say it was made against the current pin.
    marker = ("Upstream base: chromium/src@%s" % pin.revision).encode()
    for path in patched:
        pf = up.PATCHES_DIR / (path + up.PATCH_SUFFIX)
        if not pf.exists():
            problems.append("missing patch file %s" % pf)
        elif marker not in pf.read_bytes()[:1024]:
            problems.append("%s was not generated against the pin" % pf)
    missing_upstream = [p for p in patched + identical
                        if p not in tree or tree[p].type != "blob"]
    for p in missing_upstream:
        problems.append("%s is not a file in upstream@pin" % p)
    if problems:
        for p in problems:
            up.log("FAIL: " + p)
        return 1

    bases = chromium.blobs(tree[p].oid for p in patched + identical)
    scratch = Path(up.scratch_dir("molt-verify-"))
    try:
        for path in patched:
            dst = scratch / path
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(bases[tree[path].oid])
        try:
            up.apply_patches(scratch, [up.PATCHES_DIR / (p + up.PATCH_SUFFIX)
                                       for p in patched])
        except RuntimeError as e:
            up.log("FAIL: the series does not apply to upstream@pin:\n" +
                   str(e))
            return 1

        reproduced = 0
        for path in patched:
            src = up.OVERLAY_DIR / path
            if not src.exists():
                problems.append("src/%s is gone but still patched" % path)
            elif (scratch / path).read_bytes() != src.read_bytes():
                problems.append("patched %s differs from src/%s"
                                % (path, path))
            else:
                reproduced += 1
        same = 0
        for path in identical:
            src = up.OVERLAY_DIR / path
            if src.exists() and bases[tree[path].oid] == src.read_bytes():
                same += 1
            else:
                problems.append("src/%s is no longer identical to upstream"
                                % path)

        # Completeness: nothing upstream-owned may be missing from the lists.
        listed = set(patched) | set(identical) | set(whole)
        for path, kind in sorted(up.classify(chromium, commit).items()):
            if kind != up.FIRST_PARTY and path not in listed:
                problems.append("src/%s is an upstream file (%s) but is not "
                                "in patches/ - regenerate" % (path, kind))
    finally:
        if args.keep:
            up.log("Scratch tree kept at %s" % scratch)
        else:
            up.rmtree(scratch)

    print("Upstream base:  %s" % pin)
    print("Patches:        %d applied, %d reproduce src/ byte-for-byte"
          % (len(patched), reproduced))
    print("Identical:      %d of %d still identical to upstream"
          % (same, len(identical)))
    print("DEPS whole:     %d (not diffed)" % len(whole))
    if problems:
        for p in problems:
            print("FAIL: " + p)
        print("Run tools/chromium/overlay_to_patches.py to regenerate.")
        return 1
    print("OK: patches/ reproduces the overlay exactly.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
