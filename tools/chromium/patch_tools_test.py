#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Offline tests for the patch tools: overlay_to_patches.py,
verify_patches.py and rebase.py, with the DEPS-subproject whole files
(subprojects.py).

Each test builds a fake Chromium with two release tags, a DEPS subproject
with a git mirror ("v8") and one only in the release tarballs
("third_party/webrtc"), the two releases' tarballs, and a fake MoltBrowser
checkout, then runs the tools' main() against them. Needs git; no network.

  python3 tools/chromium/patch_tools_test.py -v
"""
import contextlib
import hashlib
import io
import json
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import upstream as up  # noqa: E402
import subprojects as sp  # noqa: E402
import overlay_to_patches  # noqa: E402
import verify_patches  # noqa: E402
import rebase  # noqa: E402
from ccheck import tarball as cctarball  # noqa: E402  (path set by sp)

GIT_ENV = dict(up.clean_git_env(),
               GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid",
               GIT_AUTHOR_DATE="2026-10-28T00:00:00Z",
               GIT_COMMITTER_DATE="2026-10-28T00:00:00Z")

OLD, NEW = "156.0.8078.48", "156.0.8078.74"


def git(cwd, *args):
    return up.run_git(["-C", str(cwd)] + list(args),
                      env=GIT_ENV).decode().strip()


def lines(n, **changed):
    """'line 1\\n' ... 'line n\\n', with l<k>="text" replacing line k."""
    out = ["line %d" % i for i in range(1, n + 1)]
    for k, v in changed.items():
        out[int(k[1:]) - 1] = v
    return ("\n".join(out) + "\n").encode()


def version_file(patch):
    return ("MAJOR=156\nMINOR=0\nBUILD=8078\nPATCH=%s\n" % patch).encode()


def make_repo(path):
    path.mkdir(parents=True)
    git(path, "init", "-q")
    # What a mirror allows: partial clones and fetching commits by id.
    git(path, "config", "uploadpack.allowFilter", "true")
    git(path, "config", "uploadpack.allowAnySHA1InWant", "true")
    return path


def commit(repo, files, gitlinks=None, tag=None):
    for rel, data in files.items():
        if data is None:
            git(repo, "rm", "-q", rel)
            continue
        p = repo / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        git(repo, "add", rel)
    for rel, sha in (gitlinks or {}).items():
        git(repo, "update-index", "--add", "--cacheinfo",
            "160000,%s,%s" % (sha, rel))
    git(repo, "commit", "-q", "-m", "c")
    if tag:
        git(repo, "tag", tag)
    return git(repo, "rev-parse", "HEAD")


def make_tarball(path, top, files):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(path, "w:xz") as tf:
        for rel, data in sorted(files.items()):
            info = tarfile.TarInfo("%s/%s" % (top, rel))
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))
    return hashlib.sha256(path.read_bytes()).hexdigest()


class World:
    """The fake upstream and the fake MoltBrowser checkout."""

    def __init__(self, root):
        self.root = root
        # "v8": a subproject whose mirror has both commits.
        self.v8 = make_repo(root / "v8")
        self.s1 = commit(self.v8, {"src/a.cc": lines(20), "src/b.h": lines(5),
                                   "src/c.cc": lines(5),
                                   "src/gone.cc": lines(5)})
        self.s2 = commit(self.v8, {
            "src/a.cc": lines(20, l18="line 18 upstream fix"),
            "src/c.cc": lines(5, l3="line 3 upstream"),
            "src/gone.cc": None, "src/added.cc": lines(3)})
        # "third_party/webrtc": no mirror has its release-branch commits;
        # the release tarballs do.
        self.t1, self.t2 = "1" * 40, "2" * 40
        webrtc = {
            OLD: {"x.cc": lines(20), "y.cc": lines(5)},
            NEW: {"x.cc": lines(20, l19="line 19 upstream fix"),
                  "y.cc": lines(5)}}
        self.chromium = make_repo(root / "chromium")
        self.c1 = commit(self.chromium, {
            "chrome/VERSION": version_file(48), "base/p.cc": lines(20),
            "base/same.txt": b"same\n"},
            {"v8": self.s1, "third_party/webrtc": self.t1}, tag=OLD)
        self.c2 = commit(self.chromium, {
            "chrome/VERSION": version_file(74),
            "base/p.cc": lines(20, l20="line 20 upstream fix")},
            {"v8": self.s2, "third_party/webrtc": self.t2}, tag=NEW)
        self.bucket = root / "bucket"
        self.pins = {}
        for v, files in webrtc.items():
            name = "chromium-%s-lite.tar.xz" % v
            self.pins[name] = make_tarball(
                self.bucket / name, "chromium-" + v,
                {"third_party/webrtc/" + k: d for k, d in files.items()})
        # The MoltBrowser checkout.
        self.repo = root / "molt"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        (self.repo / "chromium_version.txt").write_text(
            "# pin\nCHROMIUM_VERSION=%s\nCHROMIUM_REVISION=%s\n"
            % (OLD, self.c1))
        overlay = {
            "base/p.cc": lines(20, l2="line 2 ours"),
            "base/same.txt": b"same\n",
            "v8/src/a.cc": lines(20, l2="line 2 ours"),
            "v8/src/b.h": lines(5, l1="line 1 ours"),
            "v8/src/c.cc": lines(5, l3="line 3 ours"),
            "v8/src/gone.cc": lines(5, l1="line 1 ours"),
            "v8/src/molt_own.cc": b"ours\n",
            "third_party/webrtc/x.cc": lines(20, l2="line 2 ours"),
            "third_party/webrtc/y.cc": lines(5, l1="line 1 ours"),
            "chrome/browser/molt_ai/thing.cc": b"first party\n",
        }
        for rel, data in overlay.items():
            p = self.repo / "src" / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        (self.repo / "patches").mkdir()
        git(self.repo, "add", "-A")
        self.cache = root / "cache"

    def src(self, rel):
        return (self.repo / "src" / rel).read_bytes()

    def drop(self, rel):
        git(self.repo, "rm", "-q", "-f", "src/" + rel)

    def patches(self, mirrors=None, pins=None):
        """The tools pointed at this world."""
        r = self.repo
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch.multiple(
            up, REPO_ROOT=r, OVERLAY_DIR=r / "src", PATCHES_DIR=r / "patches",
            PIN_FILE=r / "chromium_version.txt",
            SERIES=r / "patches" / "series",
            IDENTICAL=r / "patches" / "identical.txt",
            WHOLE_FILES=r / "patches" / "whole_files.txt",
            FETCH_RETRY_DELAYS=()))
        stack.enter_context(mock.patch.object(
            sp, "MIRRORS", {"v8": ("file://%s" % self.v8,)}
            if mirrors is None else mirrors))
        stack.enter_context(mock.patch.object(
            cctarball, "BUCKET", "file://%s/" % self.bucket))
        stack.enter_context(mock.patch.object(
            cctarball, "PINNED_SHA256", self.pins if pins is None else pins))
        return stack

    def run(self, module, *argv):
        """(exit code, stdout) of module.main() with these arguments."""
        argv = [module.__name__] + list(argv) + [
            "--cache", str(self.cache),
            "--remote", "file://%s" % self.chromium]
        out = io.StringIO()
        with mock.patch.object(sys, "argv", argv), \
                contextlib.redirect_stdout(out), \
                contextlib.redirect_stderr(io.StringIO()):
            try:
                rc = module.main()
            except SystemExit as e:
                rc = e.code
        return rc, out.getvalue()

    def report(self, tag=NEW):
        data = json.loads((self.cache / "rebase" / tag / "report.json")
                          .read_text())
        return {p: i["status"] for p, i in data["files"].items()}, data


class WholeFilesFormatTest(unittest.TestCase):
    def test_round_trip_and_older_formats(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d, "whole_files.txt")
            entries = {"v8/src/a.cc": sp.WholeFile("v8", "a" * 40, OLD),
                       "third_party/x/y.json": sp.WholeFile(
                           "third_party/x", "b" * 40, None)}
            sp.write_whole_files(up.Pin(OLD, "c" * 40), entries, f)
            self.assertEqual(sp.read_whole_files(f), entries)
            text = f.read_text()
            self.assertIn("v8/src/a.cc\t# v8@%s (%s)\n" % ("a" * 40, OLD),
                          text)
            # read_list (apply_patches.py) still sees just the paths.
            self.assertEqual(sorted(up.read_list(f)), sorted(entries))
            # Before this format: no release; or no commit at all.
            f.write_text("# x\nv8/a\t# v8@%s\nv8/b\n" % ("d" * 40))
            self.assertEqual(sp.read_whole_files(f), {
                "v8/a": sp.WholeFile("v8", "d" * 40, None),
                "v8/b": sp.WholeFile(None, None, None)})

    def test_kept_entry(self):
        link = ("v8", "e" * 40)
        self.assertEqual(sp.kept_entry(None, link, NEW),
                         sp.WholeFile("v8", "e" * 40, NEW))
        old = sp.WholeFile("v8", "a" * 40, None)
        self.assertEqual(sp.kept_entry(old, link, NEW), old)
        self.assertEqual(sp.kept_entry(sp.WholeFile("v8", "e" * 40, None),
                                       link, NEW),
                         sp.WholeFile("v8", "e" * 40, NEW))
        # A different subproject path: the file moved; take the pin's.
        self.assertEqual(sp.kept_entry(sp.WholeFile("x", "a" * 40, OLD),
                                       link, NEW),
                         sp.WholeFile("v8", "e" * 40, NEW))

    def test_mirror_cache_name(self):
        self.assertEqual(sp.mirror_cache_name("https://github.com/v8/v8"),
                         "v8_v8")
        self.assertEqual(sp.mirror_cache_name(
            "https://chromium.googlesource.com/external/x.git"), "external_x")


class TarballTest(unittest.TestCase):
    def test_extract_only_wanted_and_verify(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            tb = d / "chromium-1.2.3.4-lite.tar.xz"
            sha = make_tarball(tb, "chromium-1.2.3.4", {
                "a/b.cc": b"b\n", "a/c.cc": b"c\n",
                "../evil": b"x\n"})
            url = "file://%s" % tb
            dest = sp.extract_paths(url, "1.2.3.4",
                                    {"a/b.cc", "a/missing.cc", "../evil"},
                                    d / "out", log=lambda m: None,
                                    pins={tb.name: sha})
            self.assertEqual((dest / "a/b.cc").read_bytes(), b"b\n")
            self.assertFalse((dest / "a/c.cc").exists())
            m = json.loads((dest / "manifest.json").read_text())
            self.assertEqual(m["found"], {"a/b.cc": up.blob_id(b"b\n")})
            self.assertEqual(m["sha256"], sha)
            # Other bytes than the pin: nothing is kept.
            with self.assertRaises(cctarball.TarballNotVerified):
                sp.extract_paths(url, "1.2.3.4", {"a/b.cc"}, d / "out2",
                                 log=lambda m: None, pins={tb.name: "0" * 64})
            self.assertFalse((d / "out2").exists())
            self.assertFalse((d / "out2.partial").exists())
            # No pin at all: refused before streaming.
            with self.assertRaises(cctarball.TarballNotVerified):
                sp.extract_paths(url, "1.2.3.4", {"a/b.cc"}, d / "out3",
                                 log=lambda m: None, pins={})

    def test_published_hashes_hint(self):
        name = "chromium-1.2.3.4-lite.tar.xz"
        url = "https://bucket.invalid/" + name
        sha, md5 = "b" * 64, "a" * 32
        state = {"etag": md5, "hashes": "md5  %s  %s\nsha1  %s  %s\n"
                 "sha256  %s  %s\n" % (md5, name, "c" * 40, name, sha, name)}

        class Response(io.BytesIO):
            def __init__(self, body=b"", headers=None):
                super().__init__(body)
                self.headers = headers or {}

        def opener(req):
            if isinstance(req, str):
                if state["hashes"] is None:
                    raise OSError("404")
                self.assertEqual(req, url + ".hashes")
                return Response(state["hashes"].encode())
            self.assertEqual((req.full_url, req.get_method()), (url, "HEAD"))
            return Response(headers={"ETag": '"%s"' % state["etag"]})

        self.assertEqual(sp.published_hashes(url, opener), (sha, True))
        state["etag"] = "d" * 32    # .hashes and the bucket disagree
        self.assertEqual(sp.published_hashes(url, opener), (sha, False))
        state["hashes"] = "md5  %s  %s\n" % (md5, name)   # no SHA-256
        self.assertIsNone(sp.published_hashes(url, opener))
        state["hashes"] = None      # unreachable
        self.assertIsNone(sp.published_hashes(url, opener))


class RebaseTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.w = World(Path(self._tmp.name))

    def tearDown(self):
        self._tmp.cleanup()

    def test_respin_merges_subproject_whole_files(self):
        w = self.w
        with w.patches():
            rc, out = w.run(overlay_to_patches)
            self.assertEqual(rc, 0, out)
            whole = sp.read_whole_files(w.repo / "patches/whole_files.txt")
            self.assertEqual(whole["v8/src/a.cc"],
                             sp.WholeFile("v8", w.s1, OLD))
            self.assertEqual(whole["third_party/webrtc/x.cc"],
                             sp.WholeFile("third_party/webrtc", w.t1, OLD))
            self.assertEqual(len(whole), 7)
            self.assertEqual(up.series_paths(), ["base/p.cc"])
            self.assertEqual(up.read_list(up.IDENTICAL), ["base/same.txt"])
            rc, out = w.run(verify_patches)
            self.assertEqual(rc, 0, out)
            self.assertIn("DEPS whole:     7 of 7 merged", out)

            # Dry run.
            rc, out = w.run(rebase, NEW)
            self.assertEqual(rc, 0, out)
            status, data = w.report()
            self.assertEqual(status, {
                "base/p.cc": "clean",
                "base/same.txt": "identical-unchanged",
                "v8/src/a.cc": "deps-clean",
                "v8/src/b.h": "deps-kept",
                "v8/src/c.cc": "deps-conflict",
                "v8/src/gone.cc": "deps-deleted",
                "v8/src/molt_own.cc": "deps-own",
                "third_party/webrtc/x.cc": "deps-clean",
                "third_party/webrtc/y.cc": "deps-kept"})
            self.assertEqual(data["files"]["v8/src/a.cc"]["sources"],
                             ["mirror file://%s" % w.v8] * 2)
            self.assertEqual(data["files"]["third_party/webrtc/x.cc"]
                             ["sources"], ["tarball chromium-" + OLD,
                                           "tarball chromium-" + NEW])
            report = (w.cache / "rebase" / NEW / "report.md").read_text()
            self.assertIn("| `v8` | `%s` | `%s` |" % (w.s1[:12], w.s2[:12]),
                          report)
            self.assertEqual(w.src("v8/src/a.cc"),
                             lines(20, l2="line 2 ours"))   # dry run

            # A conflict and a deleted file block --apply ...
            rc, out = w.run(rebase, NEW, "--apply")
            self.assertEqual(rc, 1)
            self.assertIn("CHROMIUM_REVISION=%s" % w.c1,
                          up.PIN_FILE.read_text())
            # ... unless allowed: conflicts get markers, the deleted file
            # stays behind the pin.
            rc, out = w.run(rebase, NEW, "--apply", "--allow-conflicts")
            self.assertEqual(rc, 0, out)
            self.assertIn("1 DEPS whole file(s) are still behind", out)
            self.assertEqual(up.read_pin().revision, w.c2)
            self.assertEqual(w.src("base/p.cc"), lines(
                20, l2="line 2 ours", l20="line 20 upstream fix"))
            self.assertEqual(w.src("v8/src/a.cc"), lines(
                20, l2="line 2 ours", l18="line 18 upstream fix"))
            self.assertEqual(w.src("third_party/webrtc/x.cc"), lines(
                20, l2="line 2 ours", l19="line 19 upstream fix"))
            self.assertEqual(w.src("v8/src/b.h"), lines(5, l1="line 1 ours"))
            conflict = w.src("v8/src/c.cc").decode()
            self.assertIn("||||||| v8@%s\n" % w.s1[:12], conflict)
            self.assertIn(">>>>>>> v8@%s\n" % w.s2[:12], conflict)
            whole = sp.read_whole_files(up.WHOLE_FILES)
            self.assertEqual(whole["v8/src/a.cc"],
                             sp.WholeFile("v8", w.s2, NEW))
            self.assertEqual(whole["v8/src/molt_own.cc"],
                             sp.WholeFile("v8", w.s2, NEW))
            self.assertEqual(whole["third_party/webrtc/y.cc"],
                             sp.WholeFile("third_party/webrtc", w.t2, NEW))
            self.assertEqual(whole["v8/src/gone.cc"],
                             sp.WholeFile("v8", w.s1, OLD))

            # The generator keeps the recorded commits; verify names the
            # file left behind.
            rc, out = w.run(overlay_to_patches)
            self.assertEqual(rc, 0, out)
            self.assertIn("src/v8/src/gone.cc: v8@%s" % w.s1[:12], out)
            self.assertEqual(sp.read_whole_files(up.WHOLE_FILES), whole)
            rc, out = w.run(verify_patches)
            self.assertEqual(rc, 1)
            self.assertIn("src/v8/src/gone.cc was merged with v8@%s, but the "
                          "pin has v8@%s" % (w.s1[:12], w.s2[:12]), out)

            # Resolved: the conflict by hand, the deleted file dropped.
            (w.repo / "src/v8/src/c.cc").write_bytes(
                lines(5, l3="line 3 upstream and ours"))
            w.drop("v8/src/gone.cc")
            rc, out = w.run(overlay_to_patches)
            self.assertEqual(rc, 0, out)
            rc, out = w.run(verify_patches)
            self.assertEqual(rc, 0, out)
            self.assertIn("DEPS whole:     6 of 6 merged", out)

    def test_unpinned_tarball_leaves_files_behind_until_merged(self):
        w = self.w
        w.drop("v8/src/gone.cc")
        (w.repo / "src/v8/src/c.cc").write_bytes(lines(5))
        only_old = {k: v for k, v in w.pins.items() if OLD in k}
        with w.patches(pins=only_old):
            self.assertEqual(w.run(overlay_to_patches)[0], 0)
            rc, out = w.run(rebase, NEW)
            status, data = w.report()
            self.assertEqual(status["third_party/webrtc/x.cc"],
                             "deps-unavailable")
            self.assertIn("has no pinned SHA-256",
                          data["files"]["third_party/webrtc/x.cc"]["error"])
            self.assertEqual(status["v8/src/a.cc"], "deps-clean")
            rc, out = w.run(rebase, NEW, "--apply")
            self.assertEqual(rc, 1)        # unavailable blocks
            rc, out = w.run(rebase, NEW, "--apply", "--allow-conflicts")
            self.assertEqual(rc, 0, out)
            self.assertIn("2 DEPS whole file(s) are still behind", out)
            self.assertEqual(w.src("third_party/webrtc/x.cc"),
                             lines(20, l2="line 2 ours"))
            self.assertEqual(w.run(overlay_to_patches)[0], 0)
            rc, out = w.run(verify_patches)
            self.assertEqual(rc, 1)
            self.assertIn("src/third_party/webrtc/x.cc was merged with "
                          "third_party/webrtc@%s" % w.t1[:12], out)
        # With the new release's tarball pinned, rebasing onto the pin's
        # own tag catches the files up: the base comes from the release
        # whole_files.txt recorded (the previous pin's tarball).
        with w.patches():
            rc, out = w.run(rebase, NEW, "--apply")
            self.assertEqual(rc, 0, out)
            self.assertEqual(w.src("third_party/webrtc/x.cc"), lines(
                20, l2="line 2 ours", l19="line 19 upstream fix"))
            self.assertEqual(w.run(overlay_to_patches)[0], 0)
            rc, out = w.run(verify_patches)
            self.assertEqual(rc, 0, out)

    def test_mirror_without_the_commit_falls_back_to_the_tarball(self):
        w = self.w
        w.drop("v8/src/gone.cc")
        (w.repo / "src/v8/src/c.cc").write_bytes(lines(5))
        # A "webrtc mirror" that has none of Chromium's webrtc commits.
        mirrors = {"v8": ("file://%s" % w.v8,),
                   "third_party/webrtc": ("file://%s" % w.v8,)}
        with w.patches(mirrors=mirrors):
            self.assertEqual(w.run(overlay_to_patches)[0], 0)
            rc, out = w.run(rebase, NEW)
            self.assertEqual(rc, 0, out)
            status, data = w.report()
            self.assertEqual(status["third_party/webrtc/x.cc"], "deps-clean")
            self.assertEqual(data["files"]["third_party/webrtc/x.cc"]
                             ["sources"][1], "tarball chromium-" + NEW)
            # The tarball was streamed once per release and is cached: a
            # second run needs no bucket.
            for f in w.bucket.iterdir():
                f.unlink()
            rc, out = w.run(rebase, NEW, "--apply")
            self.assertEqual(rc, 0, out)
            self.assertEqual(w.src("third_party/webrtc/x.cc"), lines(
                20, l2="line 2 ours", l19="line 19 upstream fix"))


if __name__ == "__main__":
    unittest.main()
