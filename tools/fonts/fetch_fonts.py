#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Fetches MoltNet's bundled fonts and installs them next to the browser.

Every file comes from its pinned commit (tools/fonts/moltnet_fonts.json) and
is used only if its size and SHA-256 match the manifest, like the Tor and
model pins. Downloads are kept in a cache (by SHA-256), so a rebuild fetches
nothing. The installed folder holds:

  <dest>/Arimo-VF.ttf ...        the font files, under their install names
  <dest>/LICENSES/OFL-*.txt      their licences (SIL Open Font License 1.1)
  <dest>/moltnet_fonts.json      the manifest (versions, sources, pins)

and nothing else: files a previous set left there are removed, so the
browser (which checks the folder against its compiled-in table) never sees
a stale set.

Where the packaging scripts put it (scripts/bundle-fonts.sh):
  macOS:   MoltBrowser.app/Contents/Resources/molt_fonts/
  Linux:   /opt/moltbrowser/molt_fonts/   (next to the executable)
  Windows: <install dir>\\molt_fonts\\      (next to MoltBrowser.exe)

Usage:
  fetch_fonts.py --dest DIR [--cache DIR] [--offline]
  fetch_fonts.py --verify DIR      # check an installed folder, fetch nothing

A manifest with TODO(pin) entries is refused when MOLT_RELEASE_BUILD=1, and
otherwise installs only what is pinned (with a warning).
Exit status: 0 ok, 1 a check failed, 2 bad usage or manifest.
"""

import argparse
import hashlib
import os
import shutil
import sys
import tempfile
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import manifest as mf  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CACHE = REPO_ROOT / "build" / "cache" / "fonts"
CHUNK = 1 << 20
TIMEOUT = 120


class FetchError(Exception):
    pass


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(CHUNK)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def download(url, dest, limit, opener=None):
    """Downloads `url` to `dest` (at most `limit` bytes). https only."""
    if not url.startswith("https://%s/" % mf.RAW_HOST):
        raise FetchError("refusing to fetch %s" % url)
    open_url = opener or urllib.request.urlopen
    tmp = Path(str(dest) + ".part")
    total = 0
    try:
        with open_url(url, timeout=TIMEOUT) as response, open(tmp, "wb") as out:
            while True:
                chunk = response.read(CHUNK)
                if not chunk:
                    break
                total += len(chunk)
                if total > limit:
                    raise FetchError("%s is larger than %d bytes" % (url, limit))
                out.write(chunk)
        os.replace(tmp, dest)
    except FetchError:
        _remove(tmp)
        raise
    except (OSError, ValueError) as e:
        _remove(tmp)
        raise FetchError("can't fetch %s: %s" % (url, e))


def _remove(path):
    try:
        path.unlink()
    except OSError:
        pass


def fetch_entry(data, entry, cache, offline=False, opener=None, limit=None):
    """The cached, verified file of a pinned manifest entry (a Path)."""
    if mf.is_pending(entry):
        raise FetchError("%s is not pinned yet" % entry["install_name"])
    cache.mkdir(parents=True, exist_ok=True)
    cached = cache / entry["sha256"]
    if cached.is_file() and cached.stat().st_size == entry["size"] and \
            sha256_of(cached) == entry["sha256"]:
        return cached
    if offline:
        raise FetchError("%s is not in the cache (%s)" %
                         (entry["install_name"], cached))
    url = mf.source_url(data["sources"][entry["source"]], entry["path"])
    download(url, cached, limit or entry["size"], opener)
    size = cached.stat().st_size
    digest = sha256_of(cached)
    if size != entry["size"] or digest != entry["sha256"]:
        cached.unlink()
        raise FetchError(
            "%s from %s doesn't match its pin: %d bytes, sha256 %s "
            "(expected %d, %s)" % (entry["install_name"], url, size, digest,
                                   entry["size"], entry["sha256"]))
    return cached


def expected_layout(data):
    """{relative path in the installed folder: entry or None} (None: the
    manifest itself)."""
    layout = {mf.INSTALLED_MANIFEST: None}
    for kind, entry in mf.all_entries(data):
        if mf.is_pending(entry):
            continue
        rel = entry["install_name"] if kind == "font" else \
            "%s/%s" % (mf.LICENSE_DIR, entry["install_name"])
        layout[rel] = entry
    return layout


def install(data, dest, cache, offline=False, opener=None):
    dest = Path(dest)
    layout = expected_layout(data)
    staging = Path(tempfile.mkdtemp(prefix=".molt_fonts.", dir=str(
        dest.parent if dest.parent.exists() else Path.cwd())))
    try:
        (staging / mf.LICENSE_DIR).mkdir()
        for rel, entry in layout.items():
            if entry is None:
                (staging / rel).write_text(mf.dump(data), encoding="utf-8")
                continue
            source = fetch_entry(data, entry, cache, offline, opener)
            shutil.copyfile(source, staging / rel)
            os.chmod(staging / rel, 0o644)
        if dest.exists():
            shutil.rmtree(dest)
        os.replace(staging, dest)
        os.chmod(dest, 0o755)
        os.chmod(dest / mf.LICENSE_DIR, 0o755)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return layout


def verify(data, folder):
    """[problem] of an installed folder: missing, extra or changed files."""
    folder = Path(folder)
    problems = []
    layout = expected_layout(data)
    present = set()
    if not folder.is_dir():
        return ["%s is not a folder" % folder]
    for root, dirs, files in os.walk(folder):
        for name in files:
            rel = Path(root, name).relative_to(folder).as_posix()
            present.add(rel)
    for rel in sorted(present - set(layout)):
        problems.append("unexpected file %s" % rel)
    for rel, entry in sorted(layout.items()):
        path = folder / rel
        if rel not in present:
            problems.append("missing %s" % rel)
            continue
        if entry is None:
            continue
        if path.stat().st_size != entry["size"] or \
                sha256_of(path) != entry["sha256"]:
            problems.append("%s doesn't match its pin" % rel)
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--dest", help="install the set into this folder")
    group.add_argument("--verify", help="check an installed folder")
    parser.add_argument("--cache", default=os.environ.get(
        "MOLT_FONTS_CACHE", str(DEFAULT_CACHE)))
    parser.add_argument("--offline", action="store_true",
                        help="use the cache only")
    parser.add_argument("--manifest", default=str(mf.MANIFEST))
    args = parser.parse_args(argv)

    try:
        data = mf.load(args.manifest)
    except mf.ManifestError as e:
        print("fetch_fonts: %s" % e, file=sys.stderr)
        return 2
    pending = [e["install_name"] for _, e in mf.all_entries(data)
               if mf.is_pending(e)]
    if pending:
        if os.environ.get("MOLT_RELEASE_BUILD") == "1":
            print("fetch_fonts: FATAL: unpinned entries in the manifest "
                  "(run tools/fonts/pin_fonts.py --write): %s" %
                  ", ".join(pending), file=sys.stderr)
            return 1
        print("fetch_fonts: WARNING: skipping unpinned entries: %s" %
              ", ".join(pending), file=sys.stderr)

    if args.verify:
        problems = verify(data, args.verify)
        for p in problems:
            print("fetch_fonts: %s" % p, file=sys.stderr)
        if not problems:
            print("fetch_fonts: %s matches the manifest (%s)" %
                  (args.verify, data["version"]))
        return 1 if problems else 0

    try:
        layout = install(data, args.dest, Path(args.cache), args.offline)
    except FetchError as e:
        print("fetch_fonts: FATAL: %s" % e, file=sys.stderr)
        return 1
    total = sum(e["size"] for e in layout.values() if e)
    print("fetch_fonts: installed MoltNet font set %s into %s "
          "(%d files, %.1f MiB)" % (data["version"], args.dest,
                                    len(layout), total / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
