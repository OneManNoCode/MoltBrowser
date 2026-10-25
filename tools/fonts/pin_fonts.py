#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Pins MoltNet's bundled fonts: sizes, SHA-256 and faces in the manifest.

For every file and licence of tools/fonts/moltnet_fonts.json this fetches the
file from its pinned commit and

  * --check (the default): verifies its size and SHA-256, and that the faces
    the manifest lists are what the file holds (names, weights, slope,
    version), so the generated tables match the files;
  * --write: fills in entries marked TODO(pin) (size 0) and every file's
    faces, and rewrites the manifest. An entry that is already pinned is
    never re-pinned: if the file at the pinned commit no longer matches,
    that is an error, not an update. To move to new fonts, change the
    source's commit (or the path), set the entry's sha256 to "TODO(pin)" and
    size to 0, and run --write; then run generate_tables.py --write and
    review both diffs.

After --write, run: python3 tools/fonts/generate_tables.py --write

Usage: pin_fonts.py [--write] [--cache DIR] [--offline]
Exit status: 0 ok, 1 a file doesn't match, 2 bad manifest or fetch error.
"""

import argparse
import hashlib
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch_fonts  # noqa: E402
import manifest as mf  # noqa: E402
import sfnt  # noqa: E402


def describe_faces(data):
    """The manifest's face records for a font file's bytes."""
    faces = []
    for face in sfnt.read_faces(data):
        family = face["skia_family"]
        if not family:
            raise sfnt.SfntError("face %d has no family name" % face["index"])
        names = [family]
        for name in face["family_names"]:
            if name not in names:
                names.append(name)
        weight = face["weight"]
        weights = [weight, weight, weight]
        for axis in face["axes"]:
            if axis["tag"] == "wght":
                weights = [int(round(axis["min"])), int(round(axis["default"])),
                           int(round(axis["max"]))]
        style = face["typographic_subfamily"] or face["subfamily"] or "Regular"
        faces.append({
            "index": face["index"],
            "family": family,
            "family_names": names,
            "postscript_name": face["postscript_name"] or family,
            "full_name": face["full_name"] or family,
            "style": style,
            "weight": weights,
            "italic": face["italic"],
            "version": face["version"] or (
                "Revision %.3f" % face["font_revision"]
                if face["font_revision"] is not None else "unknown"),
        })
    return faces


def _fetch_unpinned(data, entry, cache, offline):
    """Fetches an entry that has no pin yet; returns (bytes, sha256)."""
    url = mf.source_url(data["sources"][entry["source"]], entry["path"])
    cache.mkdir(parents=True, exist_ok=True)
    tmp = cache / ("unpinned-" + hashlib.sha256(url.encode()).hexdigest())
    if not tmp.is_file():
        if offline:
            raise fetch_fonts.FetchError("%s is not cached" % url)
        fetch_fonts.download(url, tmp, mf.MAX_FILE_SIZE)
    blob = tmp.read_bytes()
    digest = hashlib.sha256(blob).hexdigest()
    final = cache / digest
    if not final.exists():
        os.replace(tmp, final)
    else:
        tmp.unlink()
    return blob, digest


def run(data, cache, write, offline):
    problems = []
    for kind, entry in mf.all_entries(data):
        name = entry["install_name"]
        if mf.is_pending(entry):
            if not write:
                problems.append("%s is not pinned (run with --write)" % name)
                continue
            blob, digest = _fetch_unpinned(data, entry, cache, offline)
            limit = mf.MAX_LICENSE_SIZE if kind == "license" else \
                mf.MAX_FILE_SIZE
            if len(blob) > limit:
                raise fetch_fonts.FetchError("%s is too large" % name)
            entry["sha256"] = digest
            entry["size"] = len(blob)
            print("pinned %-34s %10d %s" % (name, len(blob), digest))
        else:
            blob = fetch_fonts.fetch_entry(data, entry, cache,
                                           offline).read_bytes()
        if kind != "font":
            continue
        faces = describe_faces(blob)
        if write:
            entry["faces"] = faces
        elif faces != entry["faces"]:
            problems.append("%s: the faces in the manifest are not the "
                            "file's (run with --write)" % name)
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--cache", default=os.environ.get(
        "MOLT_FONTS_CACHE", str(fetch_fonts.DEFAULT_CACHE)))
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--manifest", default=str(mf.MANIFEST))
    args = parser.parse_args(argv)
    try:
        data = mf.load(args.manifest)
        problems = run(data, Path(args.cache), args.write, args.offline)
        if args.write:
            mf.validate(data)
            Path(args.manifest).write_text(mf.dump(data), encoding="utf-8")
    except (mf.ManifestError, fetch_fonts.FetchError, sfnt.SfntError) as e:
        print("pin_fonts: %s" % e, file=sys.stderr)
        return 2
    for p in problems:
        print("pin_fonts: %s" % p, file=sys.stderr)
    if not problems:
        print("pin_fonts: %s %s" % (
            "wrote" if args.write else "checked", args.manifest))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
