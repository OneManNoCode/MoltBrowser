"""Shared helpers for MoltBrowser's Chromium overlay / patch tooling.

MoltBrowser is built by copying src/ over a Chromium checkout. Some files
under src/ are MoltBrowser's own (chrome/browser/molt_ai/, the vendored
llama.cpp, the anydoc Rust crates, ...); the rest are whole copies of
upstream Chromium files with MoltBrowser edits in them. These tools turn the
second group into a reviewable patch series against the pinned Chromium
revision (chromium_version.txt) and 3-way merge it onto newer releases.

Upstream content comes from a bare, shallow, blob-less clone of Chromium
kept under .molt-chromium-cache/ (gitignored). Fetching a revision downloads
only its trees (~20 MB: exact file list, modes, DEPS gitlinks, blob ids);
file contents are then fetched on demand, in one batch, by blob id. No
Chromium checkout is needed. The default remote is the GitHub mirror; on the
build machine you can point --remote (or MOLT_CHROMIUM_REMOTE) at
https://chromium.googlesource.com/chromium/src.git instead.
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
OVERLAY_DIR = REPO_ROOT / "src"
PATCHES_DIR = REPO_ROOT / "patches"
PIN_FILE = REPO_ROOT / "chromium_version.txt"
DEFAULT_CACHE = REPO_ROOT / ".molt-chromium-cache"
DEFAULT_REMOTE = os.environ.get("MOLT_CHROMIUM_REMOTE",
                                "https://github.com/chromium/chromium")

SERIES = PATCHES_DIR / "series"
IDENTICAL = PATCHES_DIR / "identical.txt"
WHOLE_FILES = PATCHES_DIR / "whole_files.txt"
PATCH_SUFFIX = ".patch"

_HEX40 = re.compile(r"^[0-9a-f]{40}$")

# Waits before each retry of a fetch from the mirror (trees or blobs). The
# GitHub mirror sometimes answers with a 5xx, after a long wait for a large
# tree fetch; a second try usually works.
FETCH_RETRY_DELAYS = (15, 60)


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def die(msg):
    log("ERROR: " + msg)
    sys.exit(1)


def clean_git_env():
    """Environment for git calls whose output must not depend on the user's
    config (diff prefixes, context size, autocrlf, external diff, ...)."""
    env = dict(os.environ)
    env["GIT_CONFIG_NOSYSTEM"] = "1"
    env["GIT_CONFIG_GLOBAL"] = os.devnull
    env["LC_ALL"] = "C"
    for k in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE",
              "GIT_EXTERNAL_DIFF"):
        env.pop(k, None)
    return env


def run_git(args, cwd=None, input=None, check=True, env=None):
    """Runs git and returns stdout as bytes."""
    proc = subprocess.run(["git"] + list(args), cwd=cwd, input=input,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          env=env or clean_git_env())
    if check and proc.returncode != 0:
        raise RuntimeError("git %s failed (%d): %s" % (
            " ".join(list(args)[:3]), proc.returncode,
            proc.stderr.decode(errors="replace").strip()))
    return proc.stdout


# ---------------------------------------------------------------------------
# chromium_version.txt


class Pin:
    def __init__(self, version, revision):
        self.version = version
        self.revision = revision

    def __str__(self):
        return "%s (%s)" % (self.version, self.revision)


def read_pin(path=PIN_FILE):
    values = {}
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        values[k.strip()] = v.strip()
    version = values.get("CHROMIUM_VERSION", "")
    revision = values.get("CHROMIUM_REVISION", "")
    if not _HEX40.match(revision):
        die("%s: CHROMIUM_REVISION must be a 40-hex commit id" % path)
    if not re.match(r"^\d+\.\d+\.\d+\.\d+$", version):
        die("%s: CHROMIUM_VERSION must look like 148.0.7729.0" % path)
    return Pin(version, revision)


def write_pin(version, revision, path=PIN_FILE):
    """Rewrites the two values in place, keeping the comment header."""
    text = Path(path).read_text()
    text = re.sub(r"(?m)^CHROMIUM_VERSION=.*$",
                  "CHROMIUM_VERSION=" + version, text)
    text = re.sub(r"(?m)^CHROMIUM_REVISION=.*$",
                  "CHROMIUM_REVISION=" + revision, text)
    Path(path).write_text(text)


# ---------------------------------------------------------------------------
# The src/ overlay


def overlay_files():
    """Files under src/ that git tracks, relative to src/ (working-tree
    content is used, so uncommitted edits are included)."""
    out = run_git(["-C", str(REPO_ROOT), "ls-files", "-z", "--", "src"],
                  env=dict(os.environ))
    files = []
    for raw in out.split(b"\0"):
        if raw:
            p = raw.decode()
            if (REPO_ROOT / p).is_file():
                files.append(p[len("src/"):])
    return sorted(files)


def overlay_bytes(path):
    return (OVERLAY_DIR / path).read_bytes()


def overlay_mode(path):
    return "100755" if os.access(OVERLAY_DIR / path, os.X_OK) else "100644"


def blob_id(data):
    """git's object id for a blob with this content."""
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def is_binary(data):
    return b"\0" in data[:8000]


def read_list(path):
    """Reads series / identical.txt / whole_files.txt: one entry per line;
    '#' starts a comment (a whole line, or after whitespace)."""
    if not Path(path).exists():
        return []
    out = []
    for line in Path(path).read_text().splitlines():
        line = re.split(r"\s#", line, maxsplit=1)[0].strip()
        if line and not line.startswith("#"):
            out.append(line)
    return out


def series_paths():
    """Overlay paths covered by patches/series, in series order."""
    out = []
    for entry in read_list(SERIES):
        if not entry.endswith(PATCH_SUFFIX):
            die("patches/series entry does not end in %s: %s"
                % (PATCH_SUFFIX, entry))
        out.append(entry[:-len(PATCH_SUFFIX)])
    return out


# ---------------------------------------------------------------------------
# Upstream Chromium: a bare, shallow, blob-less cache clone


class TreeEntry:
    __slots__ = ("mode", "type", "oid")

    def __init__(self, mode, type_, oid):
        self.mode = mode
        self.type = type_
        self.oid = oid


class Upstream:
    def __init__(self, cache_dir=DEFAULT_CACHE, remote=DEFAULT_REMOTE,
                 offline=False):
        self.cache_dir = Path(cache_dir)
        self.git_dir = self.cache_dir / "chromium.git"
        self.remote = remote
        self.offline = offline
        self._trees = {}
        self._local = None

    def git(self, args, input=None, check=True):
        return run_git(["--git-dir", str(self.git_dir)] + list(args),
                       input=input, check=check)

    def _ensure_repo(self):
        if (self.git_dir / "HEAD").exists():
            url = self.git(["config", "remote.origin.url"], check=False)
            if url.decode().strip() != self.remote:
                self.git(["config", "remote.origin.url", self.remote])
            return
        log("Creating upstream cache %s (remote %s)"
            % (self.git_dir, self.remote))
        self.git_dir.parent.mkdir(parents=True, exist_ok=True)
        run_git(["init", "-q", "--bare", str(self.git_dir)])
        for k, v in (("core.repositoryformatversion", "1"),
                     ("extensions.partialClone", "origin"),
                     ("remote.origin.url", self.remote),
                     ("remote.origin.promisor", "true"),
                     ("remote.origin.partialclonefilter", "blob:none"),
                     ("remote.origin.tagOpt", "--no-tags"),
                     ("gc.auto", "0")):
            self.git(["config", k, v])

    def _fetch(self, args, input=None):
        """Runs a fetch from the mirror, retrying after FETCH_RETRY_DELAYS."""
        for delay in FETCH_RETRY_DELAYS + (None,):
            try:
                return self.git(args, input=input)
            except RuntimeError as e:
                if delay is None:
                    raise
                log("%s\nRetrying in %d s ..." % (e, delay))
                time.sleep(delay)

    def _fetched_marker(self, rev):
        return self.cache_dir / "fetched" / rev.replace("/", "_")

    def resolve(self, rev):
        """Makes sure `rev` (a 40-hex commit or a tag such as
        152.0.7977.168) and its whole tree are cached; returns the commit."""
        self._ensure_repo()
        is_sha = bool(_HEX40.match(rev))
        marker = self._fetched_marker(rev)
        if not marker.exists():
            if self.offline:
                die("%s is not in the cache and --offline was given" % rev)
            spec = rev if is_sha else "+refs/tags/%s:refs/tags/%s" % (rev, rev)
            log("Fetching trees of %s from %s ..." % (rev, self.remote))
            self._fetch(["-c", "fetch.negotiationAlgorithm=noop", "fetch",
                         "-q", "--no-tags", "--depth=1", "--filter=blob:none",
                         "origin", spec])
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.write_text("")
            self._local = None
        ref = rev if is_sha else "refs/tags/" + rev
        out = self.git(["rev-parse", "--verify", ref + "^{commit}"])
        return out.decode().strip()

    def tree(self, commit):
        """{path: TreeEntry} for every blob and gitlink in the commit."""
        if commit not in self._trees:
            out = self.git(["ls-tree", "-r", "-z", "--full-tree", commit])
            entries = {}
            for rec in out.split(b"\0"):
                if not rec:
                    continue
                meta, path = rec.split(b"\t", 1)
                mode, type_, oid = meta.decode().split()
                entries[path.decode()] = TreeEntry(mode, type_, oid)
            self._trees[commit] = entries
        return self._trees[commit]

    def _local_objects(self):
        if self._local is None:
            out = self.git(["cat-file", "--batch-all-objects",
                            "--batch-check=%(objectname)"])
            self._local = set(out.decode().split())
        return self._local

    def blobs(self, oids):
        """{oid: bytes}. Blobs not yet cached are fetched in one batch."""
        oids = sorted(set(oids))
        if not oids:
            return {}
        missing = [o for o in oids if o not in self._local_objects()]
        if missing:
            if self.offline:
                die("%d upstream files are not cached and --offline was given"
                    % len(missing))
            log("Fetching %d upstream file(s) ..." % len(missing))
            self._fetch(["-c", "fetch.negotiationAlgorithm=noop", "fetch",
                         "-q", "origin", "--no-tags", "--no-write-fetch-head",
                         "--recurse-submodules=no", "--filter=blob:none",
                         "--stdin"],
                        input=("\n".join(missing) + "\n").encode())
            self._local.update(missing)
        out = self.git(["cat-file", "--batch"],
                       input=("\n".join(oids) + "\n").encode())
        result = {}
        pos = 0
        for oid in oids:
            nl = out.index(b"\n", pos)
            header = out[pos:nl].decode().split()
            if len(header) != 3 or header[1] != "blob":
                raise RuntimeError("cat-file: unexpected header %r" % header)
            size = int(header[2])
            result[oid] = out[nl + 1:nl + 1 + size]
            pos = nl + 1 + size + 1
        return result

    def read(self, commit, path):
        entry = self.tree(commit).get(path)
        if entry is None or entry.type != "blob":
            return None
        return self.blobs([entry.oid])[entry.oid]

    def version(self, commit):
        data = self.read(commit, "chrome/VERSION")
        if data is None:
            return "?"
        v = dict(l.split("=", 1) for l in data.decode().split() if "=" in l)
        return "%s.%s.%s.%s" % (v.get("MAJOR"), v.get("MINOR"),
                                v.get("BUILD"), v.get("PATCH"))

    def gitlink_for(self, commit, path):
        """If `path` lives inside a DEPS subproject (a gitlink in the
        tree), returns (gitlink_path, gitlink_commit); else None."""
        tree = self.tree(commit)
        parts = path.split("/")
        for i in range(len(parts) - 1, 0, -1):
            e = tree.get("/".join(parts[:i]))
            if e is not None:
                return ("/".join(parts[:i]), e.oid) if e.type == "commit" \
                    else None
        return None


# ---------------------------------------------------------------------------
# Classifying the overlay against one upstream commit

FIRST_PARTY = "first-party"     # not upstream: stays a plain file
IDENTICAL_KIND = "identical"    # byte-identical upstream copy: droppable
PATCHED = "patched"             # upstream file with MoltBrowser edits
DEPS_FILE = "deps-subproject"   # inside a DEPS repo; kept as a whole file


def classify(upstream, commit, files=None):
    """{overlay path: kind} for the overlay against `commit`."""
    tree = upstream.tree(commit)
    result = {}
    for path in (overlay_files() if files is None else files):
        e = tree.get(path)
        if e is not None and e.type == "blob":
            same = blob_id(overlay_bytes(path)) == e.oid
            result[path] = IDENTICAL_KIND if same else PATCHED
        elif upstream.gitlink_for(commit, path):
            result[path] = DEPS_FILE
        else:
            result[path] = FIRST_PARTY
    return result


# ---------------------------------------------------------------------------
# Patch files


def patch_header(path, pin):
    """Free text before the first 'diff --git' line; git apply skips it."""
    return ("MoltBrowser patch for %s\n"
            "Upstream base: chromium/src@%s (%s)\n"
            "Generated from src/%s by tools/chromium/overlay_to_patches.py.\n"
            "src/ is still the source of truth: edit the file there and\n"
            "re-run the generator (see docs/REBASING.md).\n\n"
            % (path, pin.revision, pin.version, path)).encode()


def make_diff(path, base, ours, mode="100644"):
    """git-format unified diff (binary-safe) turning `base` into `ours`,
    with a/<path> b/<path> headers relative to chromium/src."""
    tmp = scratch_dir("molt-diff-")
    try:
        for side, data in (("a", base), ("b", ours)):
            p = Path(tmp, side, path)
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
            os.chmod(p, 0o755 if mode == "100755" else 0o644)
        proc = subprocess.run(
            ["git", "diff", "--no-index", "--no-prefix", "--binary",
             "--full-index", "--no-color", "--no-ext-diff", "--no-textconv",
             "--no-renames", "--diff-algorithm=myers", "-U3",
             "a/" + path, "b/" + path],
            cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=clean_git_env())
        if proc.returncode not in (0, 1):
            raise RuntimeError("git diff failed for %s: %s" % (
                path, proc.stderr.decode(errors="replace")))
        return proc.stdout
    finally:
        rmtree(tmp)


def apply_patches(root, patch_files, check_only=False):
    """Applies patch files with `git apply` to the tree at `root`. It is
    all-or-nothing: one failing hunk rejects the whole set."""
    if not patch_files:
        return
    args = ["apply", "--whitespace=nowarn", "-p1"]
    if check_only:
        args.append("--check")
    run_git(args + [str(p) for p in patch_files], cwd=str(root))


def scratch_dir(prefix):
    """A temp dir outside any git work tree (inside one, git apply resolves
    paths against that repository's top level instead of the cwd)."""
    return tempfile.mkdtemp(prefix=prefix)


def rmtree(path):
    shutil.rmtree(path, ignore_errors=True)
