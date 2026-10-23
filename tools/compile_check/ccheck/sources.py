# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The source tree the check compiles against, materialized on demand.

Three layers, highest priority first:
  1. MoltBrowser's src/ overlay (the working tree, so uncommitted edits count);
  2. Chromium at CHROMIUM_REVISION, read from the blob-less cache that
     tools/chromium/upstream.py keeps (only the files a check needs are
     fetched, in batches, and written under <work>/tree/);
  3. DEPS subprojects (perfetto, boringssl, skia, ...), each its own blob-less
     clone of the GitHub mirror at the gitlink commit in Chromium's tree,
     mounted at its Chromium path (third_party/perfetto/...).

googlesource.com is not reachable from every machine, so every subproject
comes from a GitHub mirror with the same commit ids. Two have no such mirror
and are substituted (see SUBPROJECTS): libc++/libc++abi come from
llvm-project at the commit of Chromium's pinned clang, ICU from the upstream
release Chromium's fork is based on.
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "chromium"))
import upstream as _up  # noqa: E402

LLVM = "https://github.com/llvm/llvm-project"

# mount (Chromium path) -> (GitHub remote, prefix inside that repo,
# commit override or None to use the gitlink commit in Chromium's tree).
# "@clang" means: the llvm-project commit Chromium's pinned clang was built
# from (read from the downloaded toolchain).
SUBPROJECTS = {
    "third_party/perfetto": ("https://github.com/google/perfetto", "", None),
    "third_party/boringssl/src": ("https://github.com/google/boringssl", "",
                                  None),
    "third_party/googletest/src": ("https://github.com/google/googletest", "",
                                   None),
    "third_party/re2/src": ("https://github.com/google/re2", "", None),
    "third_party/skia": ("https://github.com/google/skia", "", None),
    "v8": ("https://github.com/v8/v8", "", None),
    "third_party/flatbuffers/src": ("https://github.com/google/flatbuffers",
                                    "", None),
    "third_party/crc32c/src": ("https://github.com/google/crc32c", "", None),
    "third_party/snappy/src": ("https://github.com/google/snappy", "", None),
    "third_party/highway/src": ("https://github.com/google/highway", "",
                                None),
    "third_party/fuzztest/src": ("https://github.com/google/fuzztest", "",
                                 None),
    "third_party/harfbuzz/src": ("https://github.com/harfbuzz/harfbuzz", "",
                                 None),
    "third_party/freetype/src": ("https://github.com/freetype/freetype", "",
                                 None),
    "third_party/dawn": ("https://github.com/google/dawn", "", None),
    "third_party/angle": ("https://github.com/google/angle", "", None),
    "third_party/vulkan-headers/src": (
        "https://github.com/KhronosGroup/Vulkan-Headers", "", None),
    "third_party/jsoncpp/source": (
        "https://github.com/open-source-parsers/jsoncpp", "", None),
    "net/third_party/quiche/src": ("https://github.com/google/quiche", "",
                                   None),
    "third_party/wuffs/src": ("https://github.com/google/wuffs", "", None),
    "third_party/libwebp/src": ("https://github.com/webmproject/libwebp", "",
                                None),
    "third_party/pdfium": ("https://github.com/chromium/pdfium", "", None),
    "third_party/leveldatabase/src": ("https://github.com/google/leveldb",
                                      "", None),
    "third_party/tflite/src": ("https://github.com/tensorflow/tensorflow",
                               "", None),
    # Substitutes (no GitHub mirror of Chromium's copy):
    "third_party/libc++/src": (LLVM, "libcxx/", "@clang"),
    "third_party/libc++abi/src": (LLVM, "libcxxabi/", "@clang"),
    # Chromium's ICU fork is upstream ICU plus a few patches; the headers
    # come from the upstream release it tracks.
    "third_party/icu": ("https://github.com/unicode-org/icu", "icu4c/",
                        "release-78.3"),
}


def log(msg):
    print(msg, file=sys.stderr, flush=True)


class Repo:
    """One blob-less clone (Chromium itself or a subproject)."""

    def __init__(self, cache_dir, remote, rev):
        self.up = _up.Upstream(cache_dir=cache_dir, remote=remote)
        self.rev = rev
        self._commit = None
        self._entries = None

    @property
    def commit(self):
        if self._commit is None:
            self._commit = self.up.resolve(self.rev)
        return self._commit

    @property
    def entries(self):
        if self._entries is None:
            self._entries = self.up.tree(self.commit)
        return self._entries

    def blobs(self, oids, batch=1500):
        """Yields (oid, bytes), fetching missing blobs in big batches."""
        oids = sorted(set(oids))
        for i in range(0, len(oids), batch):
            chunk = oids[i:i + batch]
            got = self.up.blobs(chunk)
            for oid in chunk:
                yield oid, got[oid]


class SourceTree:
    """Path lookups across overlay + Chromium + subprojects, and lazy
    materialization of upstream files under <work>/tree."""

    def __init__(self, work, chromium_cache, clang_llvm_commit=None,
                 overlay=None):
        self.work = Path(work)
        self.tree_dir = self.work / "tree"
        self.overlay = Path(overlay or (REPO_ROOT / "src"))
        pin = _up.read_pin(REPO_ROOT / "chromium_version.txt")
        self.chromium = Repo(chromium_cache, _up.DEFAULT_REMOTE,
                             pin.revision)
        self.clang_llvm_commit = clang_llvm_commit
        self._subs = {}
        self.unavailable = {}
        self._overlay_set = None
        self._mounts = sorted(SUBPROJECTS, key=len, reverse=True)

    # -- overlay ---------------------------------------------------------
    def overlay_set(self):
        if self._overlay_set is None:
            s = set()
            for dirpath, dirnames, filenames in os.walk(self.overlay):
                rel = os.path.relpath(dirpath, self.overlay)
                for f in filenames:
                    s.add(f if rel == "." else rel + "/" + f)
            self._overlay_set = s
        return self._overlay_set

    # -- subprojects -----------------------------------------------------
    def mount_of(self, path):
        for m in self._mounts:
            if path.startswith(m + "/"):
                return m
        return None

    def sub(self, mount):
        if mount not in self._subs:
            remote, prefix, rev = SUBPROJECTS[mount]
            if rev is None:
                e = self.chromium.entries.get(mount)
                if e is None or e.type != "commit":
                    raise RuntimeError("%s is not a gitlink at the pin" % mount)
                rev = e.oid
            elif rev == "@clang":
                if not self.clang_llvm_commit:
                    raise RuntimeError("clang's llvm commit is unknown; run "
                                       "`compile_check.py setup` first")
                rev = self.clang_llvm_commit
            name = remote.rstrip("/").split("/")[-1]
            repo = Repo(self.work / "repos" / name, remote, rev)
            try:
                repo.entries
            except RuntimeError as e:
                log("subproject %s unavailable (%s @ %s): %s" % (
                    mount, remote, rev, str(e).splitlines()[-1][-200:]))
                self.unavailable[mount] = "%s @ %s" % (remote, rev)
                repo = None
            self._subs[mount] = (repo, prefix)
        return self._subs[mount]

    def sub_lookup(self, path):
        """(repo, path inside repo) for a path under a mounted subproject."""
        m = self.mount_of(path)
        if m is None:
            return None
        repo, prefix = self.sub(m)
        if repo is None:
            return None
        return repo, prefix + path[len(m) + 1:]

    # -- lookups ---------------------------------------------------------
    def in_overlay(self, path):
        return path in self.overlay_set()

    def upstream_entry(self, path):
        """(repo, oid) for an upstream blob, or None."""
        e = self.chromium.entries.get(path)
        if e is not None:
            return (self.chromium, e.oid) if e.type == "blob" else None
        sl = self.sub_lookup(path)
        if sl is None:
            return None
        repo, rp = sl
        e = repo.entries.get(rp)
        if e is not None and e.type == "blob":
            return repo, e.oid
        return None

    def exists(self, path):
        return self.in_overlay(path) or self.upstream_entry(path) is not None

    def is_dir(self, path):
        """True if `path` is a directory in Chromium or a subproject."""
        e = self.chromium.entries.get(path)
        if e is not None:
            return e.type in ("tree", "commit")
        sl = self.sub_lookup(path)
        if sl:
            repo, rp = sl
            return any(k.startswith(rp + "/") for k in repo.entries)
        return False

    def local_path(self, path):
        """Where the file the compiler would read lives (overlay first)."""
        if self.in_overlay(path):
            return self.overlay / path
        return self.tree_dir / path

    def read(self, path):
        p = self.local_path(path)
        if p.exists():
            return p.read_bytes()
        self.materialize([path])
        return p.read_bytes() if p.exists() else None

    def materialize(self, paths):
        """Writes the upstream files in `paths` under <work>/tree (skipping
        overlay files and files already there). Returns how many were
        written."""
        by_repo = {}
        for path in paths:
            if self.in_overlay(path):
                continue
            dest = self.tree_dir / path
            if dest.exists():
                continue
            ent = self.upstream_entry(path)
            if ent is None:
                continue
            repo, oid = ent
            by_repo.setdefault(id(repo), (repo, {}))[1].setdefault(
                oid, []).append(path)
        n = 0
        for repo, wanted in by_repo.values():
            if len(wanted) > 50:
                log("fetching %d file(s) from %s" % (len(wanted),
                                                     repo.up.remote))
            for oid, data in repo.blobs(list(wanted)):
                for path in wanted[oid]:
                    dest = self.tree_dir / path
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(data)
                    n += 1
        return n

    def list_dir(self, path):
        """Names directly under an upstream directory (no blob fetch)."""
        out = set()
        pre = path.rstrip("/") + "/"
        if self.mount_of(pre + "x"):
            sl = self.sub_lookup(pre + "x")
            if sl is None:
                return []
            repo, rp = sl
            rp = rp[:-1]
            for k in repo.entries:
                if k.startswith(rp):
                    out.add(k[len(rp):].split("/", 1)[0])
        else:
            for k in self.chromium.entries:
                if k.startswith(pre):
                    out.add(k[len(pre):].split("/", 1)[0])
        for k in self.overlay_set():
            if k.startswith(pre):
                out.add(k[len(pre):].split("/", 1)[0])
        return sorted(out)

    def glob_upstream(self, prefix, suffix=""):
        """All upstream+overlay file paths under `prefix` ending in
        `suffix` (no blob fetch)."""
        out = set()
        if self.mount_of(prefix.rstrip("/") + "/x"):
            sl = self.sub_lookup(prefix.rstrip("/") + "/x")
            if sl is None:
                return []
            repo, rp = sl
            rp = rp[:-1]
            mount = self.mount_of(prefix.rstrip("/") + "/x")
            _, pfx = self.sub(mount)
            for k, e in repo.entries.items():
                if e.type == "blob" and k.startswith(rp) and k.endswith(suffix):
                    out.add(mount + "/" + k[len(pfx):])
        else:
            for k, e in self.chromium.entries.items():
                if e.type == "blob" and k.startswith(prefix) and \
                        k.endswith(suffix):
                    out.add(k)
        for k in self.overlay_set():
            if k.startswith(prefix) and k.endswith(suffix):
                out.add(k)
        return sorted(out)
