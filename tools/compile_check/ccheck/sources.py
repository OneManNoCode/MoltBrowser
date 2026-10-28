# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The source tree the check compiles against, materialized on demand.

Three layers, highest priority first:
  1. MoltBrowser's src/ overlay (the working tree, so uncommitted edits count);
  2. Chromium at CHROMIUM_REVISION, read from the blob-less cache that
     tools/chromium/upstream.py keeps (only the files a check needs are
     fetched, in batches, and written under <work>/tree/);
  3. DEPS subprojects (perfetto, boringssl, skia, ...) at the gitlink commit
     in Chromium's tree, mounted at their Chromium path
     (third_party/perfetto/...), each from the first of:
       a. a blob-less clone of its GitHub mirror (same commit ids);
       b. Chromium's official source tarball (tarball.py), for the commits
          and repositories no GitHub mirror has: the pinned release's, or
          the newest published release of the same branch (exact when that
          release pins the same commit; else the nearest public tree, a
          stand-in);
       c. a substitute (SUBSTITUTES, or the mirror's HEAD), a stand-in.

googlesource.com is not reachable from every machine, so nothing comes from
it. `<work>/repos/sources.json` records where each mount came from; when
that changes, the mount's materialized and generated files are dropped.
"""
import json
import lzma
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "chromium"))
import upstream as _up  # noqa: E402

# The mirror's large fetches sometimes fail for a while (5xx, rate limits)
# when several checkouts fetch at once: wait longer before giving up than
# the patch tools do (a compile check run is long anyway).
_up.FETCH_RETRY_DELAYS = (15, 60, 180, 300, 600)

from . import tarball  # noqa: E402

LLVM = "https://github.com/llvm/llvm-project"

# mount (Chromium path) -> (git mirror, or a tuple of mirrors tried in
# order, or None if there is none; prefix inside that repo; commit override
# or None to use the gitlink commit in Chromium's tree). A mirror serves the
# exact tree when it has the commit (git checks the ids), whoever runs it.
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
    # llama.cpp's Vulkan backend (molt_llama_vulkan): SPIR-V's headers, and
    # glslang and SPIRV-Tools for its shader compiler (ggml_vk_glslc).
    "third_party/spirv-headers/src": (
        "https://github.com/KhronosGroup/SPIRV-Headers", "", None),
    "third_party/glslang/src": (
        "https://github.com/KhronosGroup/glslang", "", None),
    "third_party/spirv-tools/src": (
        "https://github.com/KhronosGroup/SPIRV-Tools", "", None),
    "third_party/jsoncpp/source": (
        "https://github.com/open-source-parsers/jsoncpp", "", None),
    "net/third_party/quiche/src": ("https://github.com/google/quiche", "",
                                   None),
    "third_party/wuffs/src": ("https://github.com/google/wuffs", "", None),
    "third_party/libwebp/src": ("https://github.com/webmproject/libwebp", "",
                                None),
    # media/muxers/webm_muxer.h (Blink's MediaRecorder, which
    # media_capabilities.cc includes).
    "third_party/libwebm/source": ("https://github.com/webmproject/libwebm",
                                   "", None),
    # media/video/vpx_video_encoder.h (MediaRecorder's software encoder,
    # which video_track_recorder.cc creates).
    "third_party/libvpx/source/libvpx": (
        "https://github.com/webmproject/libvpx", "", None),
    # media/gfx/paint_canvas_video_renderer.cc (half floats for 16-bit
    # frames).
    "third_party/fp16/src": ("https://github.com/Maratyszcza/FP16", "", None),
    # GitHub's mirror stops at older release branches; this GitLab mirror
    # carries chromium/<branch> (the pinned M152 commit among them).
    "third_party/pdfium": (("https://github.com/chromium/pdfium",
                            "https://gitlab.com/kkowalczyk/pdfium"), "",
                           None),
    "third_party/leveldatabase/src": ("https://github.com/google/leveldb",
                                      "", None),
    "third_party/tflite/src": ("https://github.com/tensorflow/tensorflow",
                               "", None),
    "third_party/webrtc": ("https://github.com/webrtc-mirror/webrtc", "",
                           None),
    "third_party/libyuv": ("https://github.com/lemenkov/libyuv", "", None),
    "third_party/openscreen/src": ("https://github.com/chromium/openscreen",
                                   "", None),
    # Chromium's external/libaddressinput is Google's repository (Autofill's
    # address metadata; molt_net_autofill.cc and its hooks reach it).
    "third_party/libaddressinput/src": (
        "https://github.com/google/libaddressinput", "", None),
    # Blink's AVIF decoder (avif_image_decoder.cc) includes CrabbyAvif's
    # C header; Chromium's external/ copy has GitHub's commit ids.
    "third_party/crabbyavif/src": (
        "https://github.com/webmproject/CrabbyAvif", "", None),
    # No GitHub mirror (Chromium's own forks and a CIPD-built package):
    # from the tarball.
    "third_party/dom_distiller_js/dist": (None, "", None),
    "third_party/libc++/src": (None, "", None),
    "third_party/libc++abi/src": (None, "", None),
    "third_party/icu": (None, "", None),
    # media/video/av1_video_encoder.h (MediaRecorder's AV1 encoder, which
    # video_track_recorder.cc creates): aomedia.googlesource.com only.
    "third_party/libaom/source/libaom": (None, "", None),
    # llvm-libc (V8's ieee754.cc and molt_portable_math.cc include its
    # shared math): Chromium pins a libc-only split of llvm-project, whose
    # commit ids no mirror has.
    "third_party/llvm-libc/src": (None, "", None),
    # Chromium's FFmpeg fork (media/base/molt_portable_media.cc includes
    # libavutil/cpu.h): chromium.googlesource.com only.
    "third_party/ffmpeg": (None, "", None),
    # Chromium's SQLite (chromium/deps/sqlite.git, googlesource.com only):
    # third_party/sqlite/sqlite3.h includes the amalgamation's headers
    # (amalgamation/sqlite3.h and rename_exports.h), which Chromium's
    # scripts/generate_amalgamation.py writes and the repository carries.
    # The SQL wrappers' includers (storage_service_impl.cc) reach them.
    "third_party/sqlite/src": (None, "", None),
}

# Stand-ins for when neither a mirror nor a tarball has the pinned tree
# (same layout, nearest public source). "@clang": the llvm-project commit
# Chromium's pinned clang was built from (read from the downloaded
# toolchain); "@head": the repository's HEAD.
SUBSTITUTES = {
    # dom_distiller_js/dist is built from dom-distiller; its proto/ is the
    # repository's.
    "third_party/dom_distiller_js/dist": (
        "https://github.com/chromium/dom-distiller", "", "@head"),
    "third_party/libc++/src": (LLVM, "libcxx/", "@clang"),
    "third_party/libc++abi/src": (LLVM, "libcxxabi/", "@clang"),
    # Chromium's ICU fork is upstream ICU plus a few patches.
    "third_party/icu": ("https://github.com/unicode-org/icu", "icu4c/",
                        "release-78.3"),
}

# Subprojects whose mirror's HEAD is the last resort (public C APIs that
# change slowly only).
HEAD_FALLBACK = {"third_party/pdfium", "third_party/webrtc",
                 "third_party/libyuv", "third_party/openscreen/src",
                 "third_party/dawn"}

SOURCES_STATE = "sources.json"


def cache_name(remote):
    """The clone's directory under <work>/repos: the repository name for a
    GitHub mirror, host-owner-name for any other."""
    parts = remote.rstrip("/").split("/")
    if "github.com" in parts:
        return parts[-1]
    return "-".join(parts[2:]).removesuffix(".git")


def load_sources_state(work):
    """{mount: {"key", "exact", "desc"}}: where each mount's files came
    from (written by SourceTree.sub)."""
    f = Path(work) / "repos" / SOURCES_STATE
    try:
        return json.loads(f.read_text())
    except (OSError, ValueError):
        return {}


def stand_in_mounts(work):
    """{mount: label} of the mounts whose tree is not the pinned one."""
    return {m: v.get("desc") or m for m, v in
            load_sources_state(work).items() if not v.get("exact")}


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


OUT_REL = "out/cc"


class SourceTree:
    """Path lookups across overlay + Chromium + subprojects, and lazy
    materialization of upstream files.

    <work>/chromium/src is laid out like a checkout: upstream files are
    written there as they are needed, every src/ overlay file is a symlink to
    the worktree (so edits show up without a re-sync), and the generated
    files live in out/cc/gen, so generator scripts and include paths work as
    in a real build."""

    def __init__(self, work, chromium_cache, clang_llvm_commit=None,
                 overlay=None, platform=None):
        from . import platforms
        self.work = Path(work)
        self.tree_dir = self.work / "chromium" / "src"
        # The platform whose build directory and generator state this tree
        # uses (platforms.py); the checkout itself is shared.
        self.platform = platforms.get(platform or platforms.DEFAULT)
        self.out_dir = self.tree_dir / self.platform.out_rel
        self.gen_dir = self.out_dir / "gen"
        self.state = self.platform.state_dir(self.work)
        self.overlay = Path(overlay or (REPO_ROOT / "src"))
        pin = _up.read_pin(REPO_ROOT / "chromium_version.txt")
        self.chromium = Repo(chromium_cache, _up.DEFAULT_REMOTE,
                             pin.revision)
        self.clang_llvm_commit = clang_llvm_commit
        self._subs = {}
        self.purged = []
        self._tarball_info = None
        self.unavailable = {}
        self.substituted = {}
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
        """(repo, prefix) for a mounted subproject (repo None if no source
        has it)."""
        if mount not in self._subs:
            self._subs[mount] = self._resolve_sub(mount)
        return self._subs[mount]

    def _resolve_sub(self, mount):
        remote, prefix, rev = SUBPROJECTS[mount]
        e = self.chromium.entries.get(mount)
        gitlink = e.oid if e is not None and e.type == "commit" else None
        if rev is None and gitlink is None:
            raise RuntimeError("%s is not a gitlink at the pin" % mount)
        remotes = list(remote) if isinstance(remote, tuple) else \
            [remote] if remote else []
        for r in remotes:
            repo = self._mirror(mount, r, rev or gitlink)
            if repo is not None:
                self._record(mount, "git %s %s" % (r, repo.commit), True,
                             None)
                return repo, prefix
        if gitlink:
            got = self._from_tarball(mount, gitlink)
            if got is not None:
                repo, exact, key, desc = got
                self._record(mount, key, exact, desc)
                return repo, ""
        if mount in SUBSTITUTES:
            sremote, sprefix, srev = SUBSTITUTES[mount]
            if srev == "@head":
                repo = self._head_fallback(mount, cache_name(sremote),
                                           sremote)
            else:
                if srev == "@clang":
                    if not self.clang_llvm_commit:
                        raise RuntimeError("clang's llvm commit is unknown; "
                                           "run `compile_check.py setup` "
                                           "first")
                    srev = self.clang_llvm_commit
                repo = self._mirror(mount, sremote, srev)
                if repo is not None:
                    self.substituted[mount] = "%s @ %s" % (sremote, srev)
            if repo is not None:
                self._record(
                    mount, "substitute %s %s" % (sremote, repo.commit), False,
                    "%s (%s at %s)" % (mount, sremote.split("github.com/")[-1],
                                       "HEAD" if srev == "@head" else
                                       srev[:12]))
                return repo, sprefix
        if remotes and mount in HEAD_FALLBACK:
            repo = self._head_fallback(mount, cache_name(remotes[0]),
                                       remotes[0])
            if repo is not None:
                self._record(mount, "head %s %s" % (remotes[0],
                                                    repo.commit),
                             False, "%s (mirror HEAD)" % mount)
                return repo, prefix
        return None, prefix

    def _mirror(self, mount, remote, rev):
        """A blob-less clone of `remote` at `rev`, or None if the mirror
        doesn't have it (remembered in repos/unavailable.json)."""
        key = "%s @ %s" % (remote, rev)
        marker = self.work / "repos" / "unavailable.json"
        known_bad = json.loads(marker.read_text()) if marker.exists() \
            else {}
        bad = known_bad.get(mount, [])
        bad = [bad] if isinstance(bad, str) else bad
        if key in bad:
            self.unavailable[mount] = key
            return None
        repo = Repo(self.work / "repos" / cache_name(remote), remote, rev)
        try:
            repo.entries
        except RuntimeError as e:
            log("subproject %s unavailable (%s): %s" % (
                mount, key, str(e).splitlines()[-1][-200:]))
            self.unavailable[mount] = key
            known_bad[mount] = sorted(set(bad) | {key})
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.write_text(json.dumps(known_bad, indent=1))
            return None
        return repo

    # -- the official tarball --------------------------------------------
    def tarball_info(self):
        """{"version", "url", "gitlinks": {mount: commit}} of the tarball
        to use for this pin (cached in repos/tarball.json), or None."""
        if self._tarball_info is not None:
            return self._tarball_info or None
        state_f = self.work / "repos" / "tarball.json"
        pin = _up.read_pin(REPO_ROOT / "chromium_version.txt")
        try:
            state = json.loads(state_f.read_text())
        except (OSError, ValueError):
            state = {}
        # A mount added since the state was written needs its gitlink.
        stale = state.get("version") and any(
            m not in state.get("gitlinks", {}) and
            m not in state.get("no_gitlink", []) for m in SUBPROJECTS)
        if state.get("pin") != pin.version or stale:
            branch = ".".join(pin.version.split(".")[:3]) + "."
            try:
                versions = tarball.list_versions(branch)
            except OSError as e:
                log("tarball: cannot list releases: %s" % e)
                self._tarball_info = {}
                return None
            v = tarball.pick_version(pin.version, versions)
            state = {"pin": pin.version, "version": v}
            if v is not None:
                if v == pin.version:
                    entries = self.chromium.entries
                else:
                    up = _up.Upstream(cache_dir=self.chromium.up.cache_dir,
                                      remote=self.chromium.up.remote)
                    try:
                        entries = up.tree(up.resolve(v))
                    except RuntimeError as e:
                        log("tarball: cannot read %s's tree: %s" % (v, e))
                        self._tarball_info = {}
                        return None
                state["url"] = tarball.tarball_url(v, versions[v])
                state["gitlinks"] = {
                    m: entries[m].oid for m in SUBPROJECTS
                    if m in entries and entries[m].type == "commit"}
                state["no_gitlink"] = sorted(
                    m for m in SUBPROJECTS if m not in state["gitlinks"])
            state_f.parent.mkdir(parents=True, exist_ok=True)
            state_f.write_text(json.dumps(state, indent=1, sort_keys=True))
        self._tarball_info = state if state.get("version") else {}
        return self._tarball_info or None

    def _tarball_mounts(self, mount):
        """The mounts to keep from one pass over the tarball: this one, every
        mount without a mirror, and every mount whose mirror lacks the
        pinned commit (so one pass serves them all)."""
        marker = self.work / "repos" / "unavailable.json"
        bad = json.loads(marker.read_text()) if marker.exists() else {}
        return {mount} | {m for m, (r, _, _) in SUBPROJECTS.items()
                          if not r} | (set(bad) & set(SUBPROJECTS))

    def _from_tarball(self, mount, gitlink):
        """(DirRepo, exact, source key, stand-in label) from the official
        tarball, or None."""
        info = self.tarball_info()
        if info is None:
            return None
        root = self.work / "repos" / ("tarball-" + info["version"])
        man = None
        try:
            man = json.loads((root / tarball.MANIFEST).read_text())
        except (OSError, ValueError):
            pass
        if man is None or mount not in man["mounts"]:
            mounts = set(man["mounts"]) if man else set()
            mounts |= self._tarball_mounts(mount)
            for attempt in (1, 2):
                try:
                    tarball.extract(info["url"], info["version"],
                                    sorted(mounts), root, log)
                    break
                except (OSError, EOFError, tarfile.TarError,
                        lzma.LZMAError) as e:
                    log("tarball: extraction failed (%s)%s" % (
                        e, "; retrying" if attempt == 1 else ""))
            else:
                return None
            man = json.loads((root / tarball.MANIFEST).read_text())
        files = man["mounts"].get(mount) or {}
        if not files:
            return None
        commit = info["gitlinks"].get(mount)
        exact = commit == gitlink
        repo = tarball.DirRepo(root / mount, files, commit or gitlink,
                               "tarball " + info["version"])
        key = "tarball %s %s" % (info["version"], commit)
        desc = None
        if not exact:
            desc = "%s (%s's %s; the pinned %s is not mirrored)" % (
                mount, info["version"], (commit or "?")[:12], gitlink[:12])
            self.substituted[mount] = desc
        return repo, exact, key, desc

    def _record(self, mount, key, exact, desc):
        """Remembers where a mount came from; a different source than last
        time drops the mount's materialized and generated files (they came
        from the old one)."""
        f = self.work / "repos" / SOURCES_STATE
        state = load_sources_state(self.work)
        old = state.get(mount, {}).get("key")
        d = self.tree_dir / mount
        # A work dir from before sources.json: what's there came from a
        # mirror at the pin, or from a stand-in (the mirror's HEAD, listed
        # in head_fallback.json, or a substitute).
        legacy = old is None and d.exists() and (
            not key.startswith("git ") or mount in self._legacy_heads())
        if (old is not None and old != key) or legacy:
            log("subproject %s: source changed (%s -> %s); dropping its "
                "files" % (mount, old or "earlier stand-in", key))
            self.purged.append(mount)
            shutil.rmtree(d, ignore_errors=True)
            # Its generated files, and what actions made from its files
            # elsewhere in gen/ (gen_actions.GEN_INPUTS), on every platform.
            from . import platforms
            for name in platforms.all_names():
                plat = platforms.get(name)
                gen = self.tree_dir / plat.out_rel / "gen"
                shutil.rmtree(gen / mount, ignore_errors=True)
                try:
                    made = json.loads((plat.state_dir(self.work) /
                                       "gen_inputs.json").read_text())
                except (OSError, ValueError):
                    made = {}
                for rel, mounts in made.items():
                    if mount in mounts and (gen / rel).exists():
                        (gen / rel).unlink()
            for p in self.overlay_set():
                if p.startswith(mount + "/"):
                    (self.tree_dir / p).parent.mkdir(parents=True,
                                                     exist_ok=True)
                    os.symlink(self.overlay / p, self.tree_dir / p)
        if state.get(mount) != {"key": key, "exact": exact, "desc": desc}:
            state[mount] = {"key": key, "exact": exact, "desc": desc}
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text(json.dumps(state, indent=1, sort_keys=True))

    def _legacy_heads(self):
        try:
            return set(json.loads((self.work / "repos" /
                                   "head_fallback.json").read_text()))
        except (OSError, ValueError):
            return set()

    def validate_mounts(self):
        """Resolves every mount used before (recorded, or with files here),
        so a changed source is noticed and its old files dropped before
        anything reads them. Returns the mounts whose files were dropped
        (fetch and gen bring the new ones)."""
        before = len(self.purged)
        state = load_sources_state(self.work)
        for m in SUBPROJECTS:
            if m in state or (self.tree_dir / m).exists():
                self.sub(m)
        return self.purged[before:]

    def _head_fallback(self, mount, name, remote):
        """The mirror's current HEAD, for a subproject whose pinned commit
        the mirror lacks. Recorded in self.substituted."""
        state = self.work / "repos" / "head_fallback.json"
        known = json.loads(state.read_text()) if state.exists() else {}
        sha = known.get(mount)
        if not sha:
            r = subprocess.run(["git", "ls-remote", remote, "HEAD"],
                               capture_output=True, text=True)
            if r.returncode != 0 or not r.stdout.strip():
                return None
            sha = r.stdout.split()[0]
            known[mount] = sha
            state.parent.mkdir(parents=True, exist_ok=True)
            state.write_text(json.dumps(known, indent=1))
        repo = Repo(self.work / "repos" / (name + "-head"), remote, sha)
        try:
            repo.entries
        except RuntimeError as e:
            log("subproject %s: HEAD fallback failed: %s" % (mount, e))
            return None
        log("subproject %s: using the mirror's HEAD %s (STAND-IN for the "
            "pinned commit)" % (mount, sha[:12]))
        self.substituted[mount] = "%s @ HEAD %s" % (remote, sha)
        return repo

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

    def sync_overlay(self):
        """Symlinks every overlay file into the merged tree (replacing an
        upstream copy) and drops links to overlay files that are gone."""
        state = self.work / "overlay_links.json"
        old = set(json.loads(state.read_text())) if state.exists() else set()
        cur = self.overlay_set()
        for p in sorted(old - cur):
            d = self.tree_dir / p
            if d.is_symlink():
                d.unlink()
        for p in sorted(cur):
            d = self.tree_dir / p
            target = self.overlay / p
            if d.is_symlink() and os.readlink(d) == str(target):
                continue
            if d.exists() or d.is_symlink():
                d.unlink()
            d.parent.mkdir(parents=True, exist_ok=True)
            os.symlink(target, d)
        state.parent.mkdir(parents=True, exist_ok=True)
        state.write_text(json.dumps(sorted(cur)))

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

    def chromium_files(self, pred):
        """Chromium (not subproject) blob paths matching pred(path)."""
        return [k for k, e in self.chromium.entries.items()
                if e.type == "blob" and pred(k)]

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
