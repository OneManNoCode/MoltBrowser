# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""DEPS subprojects from Chromium's official source tarball.

Some DEPS repositories have no GitHub mirror of the commit Chromium pins
(webrtc, dawn and pdfium pin commits of their release branches, which the
mirrors don't carry; Chromium's libc++, libc++abi and ICU forks and the
dom_distiller_js package are not on GitHub at all). Chromium publishes a
source tarball of every release with all of its DEPS checked out
(commondatastorage.googleapis.com/chromium-browser-official, reachable where
googlesource.com is not). This module:

  * picks the tarball: the pinned version if it is published, else the
    newest published release of the same branch before it (pick_version);
  * streams it once, without storing the archive, keeping only the files
    under the mounts asked for (extract): about 3 minutes and 600 MB;
  * serves those files like a git tree (DirRepo), with git blob ids.

Whether a mount's tree is the pinned one is decided by comparing gitlinks:
the mount's gitlink in the tarball's release against the pin's (the caller
does that, sources.py). A different commit is the nearest public tree, a
stand-in that `report` names.
"""
import hashlib
import json
import re
import shutil
import tarfile
import time
import urllib.parse
import urllib.request
from pathlib import Path
from types import SimpleNamespace

BUCKET = ("https://commondatastorage.googleapis.com/"
          "chromium-browser-official/")
NAME_RE = re.compile(r"^chromium-(\d+\.\d+\.\d+\.\d+)(-lite)?\.tar\.xz$")

# Inside the mounts, never needed by a syntax check and large: prebuilt
# toolchains, conformance suites, data files.
EXCLUDE = (
    "third_party/openscreen/src/third_party/llvm-build/",
    "third_party/dawn/third_party/webgpu-cts/",
    "third_party/dawn/third_party/directx-shader-compiler/",
    "third_party/dawn/third_party/gn/",
    "third_party/icu/source/data/",
    "third_party/icu/source/test/",
    "third_party/webrtc/data/",
    "third_party/pdfium/testing/resources/",
)
MAX_FILE = 2 << 20

MANIFEST = "manifest.json"


def _vkey(v):
    return tuple(int(x) for x in v.split("."))


def parse_listing(xml):
    """(keys, next marker or None) of one bucket listing page."""
    keys = re.findall(r"<Key>([^<]+)</Key>", xml)
    truncated = re.search(r"<IsTruncated>true</IsTruncated>", xml)
    nxt = re.search(r"<NextMarker>([^<]+)</NextMarker>", xml)
    if not truncated:
        return keys, None
    return keys, (nxt.group(1) if nxt else (keys[-1] if keys else None))


def published(keys):
    """{version: {"lite", "full"}} from bucket keys."""
    out = {}
    for k in keys:
        m = NAME_RE.match(k)
        if m:
            out.setdefault(m.group(1), set()).add(
                "lite" if m.group(2) else "full")
    return out


def list_versions(branch_prefix, urlopen=urllib.request.urlopen):
    """Published tarballs whose version starts with `branch_prefix`
    ("152.0.7977.")."""
    keys = []
    marker = ""
    for _ in range(50):
        q = urllib.parse.urlencode({"prefix": "chromium-" + branch_prefix,
                                    "marker": marker})
        with urlopen(BUCKET + "?" + q) as r:
            page, marker = parse_listing(r.read().decode())
        keys += page
        if not marker:
            break
    return published(keys)


def pick_version(pin_version, versions):
    """The pinned version if published, else the newest published release
    of its branch (same MAJOR.MINOR.BUILD) older than it; None if none."""
    pv = _vkey(pin_version)
    cands = [v for v in versions if _vkey(v)[:3] == pv[:3] and
             _vkey(v) <= pv]
    return max(cands, key=_vkey) if cands else None


def tarball_url(version, kinds):
    # The lite tarball leaves out test data only.
    return BUCKET + "chromium-%s%s.tar.xz" % (
        version, "-lite" if "lite" in kinds else "")


def blob_id(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def member_dest(name, top, mounts):
    """(mount, path inside it) for a tarball member to keep, else None."""
    if not name.startswith(top + "/"):
        return None
    rel = name[len(top) + 1:]
    if rel.startswith(EXCLUDE):
        return None
    for m in mounts:
        if rel.startswith(m + "/"):
            return m, rel[len(m) + 1:]
    return None


def extract(url, version, mounts, dest, log, opener=None):
    """Streams the tarball at `url` and writes every regular file (up to
    MAX_FILE) under the mounts to dest/<mount>/..., then dest/manifest.json:
    {"version", "url", "mounts": {mount: {path: [mode, blob id]}}}.
    Written to a temporary directory first: an interrupted run leaves
    nothing behind."""
    dest = Path(dest)
    tmp = dest.with_name(dest.name + ".partial")
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    top = "chromium-" + version
    files = {m: {} for m in mounts}
    mounts_by_len = sorted(mounts, key=len, reverse=True)
    t0 = time.time()
    log("tarball: streaming %s for %s (a few minutes)" % (
        url, ", ".join(sorted(mounts))))
    opener = opener or urllib.request.urlopen
    n = 0
    with opener(url) as r, tarfile.open(fileobj=r, mode="r|xz") as tf:
        for m in tf:
            n += 1
            if not m.isfile() or m.size > MAX_FILE:
                continue
            hit = member_dest(m.name, top, mounts_by_len)
            if hit is None:
                continue
            mount, rel = hit
            data = tf.extractfile(m).read()
            out = tmp / mount / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(data)
            mode = "100755" if m.mode & 0o111 else "100644"
            files[mount][rel] = [mode, blob_id(data)]
    (tmp / MANIFEST).write_text(json.dumps(
        {"version": version, "url": url, "mounts": files}))
    shutil.rmtree(dest, ignore_errors=True)
    tmp.rename(dest)
    log("tarball: %d members read, %d files kept, %.0f s" % (
        n, sum(len(f) for f in files.values()), time.time() - t0))
    return dest


class DirRepo:
    """A mount's files from an extracted tarball, with the interface of
    sources.Repo (entries, blobs, commit)."""

    def __init__(self, root, files, commit, label):
        from upstream import TreeEntry  # tools/chromium (on sys.path)
        self.root = Path(root)
        self.rev = self.commit = commit
        self.up = SimpleNamespace(remote=label)
        self.entries = {p: TreeEntry(mode, "blob", oid)
                        for p, (mode, oid) in files.items()}
        self._path_of = {}
        for p, (_, oid) in files.items():
            self._path_of.setdefault(oid, p)

    def blobs(self, oids, batch=None):
        for oid in sorted(set(oids)):
            yield oid, (self.root / self._path_of[oid]).read_bytes()
