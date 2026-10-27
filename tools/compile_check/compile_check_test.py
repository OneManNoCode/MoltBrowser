#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for the compile check's own logic, offline (no Chromium, no
network): the GN reader's set_defaults and `configs -=`, each file's
exception/RTTI flags and own configs from its GN target, mojom freshness
and the parser bisection, the tarball source, grit header freshness, cxx
bridge headers' cfgs and freshness, where subprojects came from, the
platforms (which files, their atoms, buildflags in the scan, the compile
command of each platform and language), the Windows SDK's layout, the
report's stand-in labels, and when a check result is reused.

Run: python3 tools/compile_check/compile_check_test.py -v
"""
import base64
import hashlib
import io
import json
import lzma
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import types
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import compile_check  # noqa: E402
from ccheck import check, config, gen_actions, gen_misc, gen_mojom  # noqa
from ccheck import gen_buildflags  # noqa: E402
from ccheck import gn, gnconfigs, platforms, scan, sdks, sources  # noqa
from ccheck import host_tools, result_cache, tarball, targets  # noqa: E402

BUILDCONFIG = """
default_compiler_configs = [
  "//build/config/compiler:compiler",
  "//build/config/compiler:no_exceptions",
  "//build/config/compiler:no_rtti",
]
set_defaults("source_set") {
  configs = default_compiler_configs
}
set_defaults("component") {
  configs = default_compiler_configs
}
"""

COMPILER_BUILD = """
config("compiler") {
  cflags = [ "-fno-strict-aliasing" ]
}
config("no_exceptions") {
  cflags_cc = [ "-fno-exceptions" ]
}
config("exceptions") {
  cflags_cc = [ "-fexceptions" ]
}
config("no_rtti") {
  cflags_cc = [ "-fno-rtti" ]
}
config("rtti") {
  cflags_cc = [ "-frtti" ]
}
"""


class FakeTree:
    """The bits of sources.SourceTree the GN-side code uses."""

    def __init__(self, files, root="/nonexistent/src"):
        self.files = dict(files)
        self.tree_dir = Path(root)

    def read(self, path):
        d = self.files.get(path)
        return d.encode() if isinstance(d, str) else d

    def exists(self, path):
        return path in self.files

    def chromium_files(self, pred):
        return [p for p in self.files if pred(p)]

    def overlay_set(self):
        return set()

    def glob_upstream(self, prefix):
        return [p for p in self.files if p.startswith(prefix)]


def make_interp(files):
    def read(path):
        d = files.get(path[2:]) if path.startswith("//") else None
        return d

    it = gn.Interp(read, {}, root="/nonexistent/src")
    it.globals.set("root_build_dir", gn.OUT)
    it.exec_block(gn.Parser(BUILDCONFIG).parse_file(), it.globals,
                  {"dir": "//build/config", "targets": None})
    return it


class GnReaderTest(unittest.TestCase):
    def test_set_defaults_and_removed_configs(self):
        files = {"a/BUILD.gn": """
source_set("plain") { sources = [ "x.cc" ] }
source_set("exc") {
  sources = [ "y.cc" ]
  configs -= [ "//build/config/compiler:no_exceptions" ]
}
"""}
        it = make_interp(files)
        ts = {t.name: t for t in it.run_file("//a/BUILD.gn")}
        self.assertIn("//build/config/compiler:no_exceptions",
                      ts["plain"].get("configs"))
        self.assertNotIn("//build/config/compiler:no_exceptions",
                         ts["exc"].get("configs"))
        self.assertEqual(ts["exc"].scope.removed["configs"],
                         ["//build/config/compiler:no_exceptions"])
        # The defaults are copied, not shared between targets.
        self.assertIsNot(ts["plain"].get("configs"),
                         ts["exc"].get("configs"))

    def test_target_function(self):
        files = {"a/BUILD.gn": """
template("wrapped") {
  target("source_set", target_name) {
    forward_variables_from(invoker, "*")
    configs += [ ":extra" ]
  }
}
wrapped("w") { sources = [ "w.cc" ] }
"""}
        it = make_interp(files)
        ts = [t for t in it.run_file("//a/BUILD.gn", expand=True)
              if t.func == "source_set"]
        self.assertEqual([t.name for t in ts], ["w"])
        self.assertIn(":extra", ts[0].get("configs"))
        self.assertIn("//build/config/compiler:no_rtti",
                      ts[0].get("configs"))


class LanguageFlagsTest(unittest.TestCase):
    def setUp(self):
        gen_actions._build_texts = None
        self.files = {
            "build/config/compiler/BUILD.gn": COMPILER_BUILD,
            "lib/BUILD.gn": """
config("my_exceptions") {
  cflags_cc = [ "-fexceptions" ]
}
source_set("default") { sources = [ "default.cc" ] }
source_set("removes") {
  sources = [ "removes.cc" ]
  configs -= [ "//build/config/compiler:no_exceptions" ]
  configs += [ ":my_exceptions" ]
}
component("adds") {
  sources = [ "sub/adds.cc" ]
  configs += [ "//build/config/compiler:exceptions" ]
}
source_set("own_config") {
  sources = [ "own_config.cc" ]
  configs += [ ":my_exceptions" ]
}
test("no_defaults") {
  sources = [ "no_defaults.cc" ]
  configs -= [ "//build/config/compiler:no_rtti" ]
}
source_set("blink_style") {
  sources = rebase_path([ "listed.cc" ], "", ".")
}
""",
            "other/BUILD.gn": """
source_set("elsewhere") { sources = [ "../lib/sub/far.cc" ] }
""",
        }
        self.tree = FakeTree(self.files)
        self.interp = make_interp(self.files)

    def flags(self, path):
        return gnconfigs.target_flags(self.tree, self.interp, [path])[path]

    def test_default_target_has_neither(self):
        f = self.flags("lib/default.cc")
        self.assertEqual(f["gn_target"], "//lib:default")
        self.assertFalse(f["exceptions"])
        self.assertFalse(f["rtti"])

    def test_removing_no_exceptions(self):
        self.assertTrue(self.flags("lib/removes.cc")["exceptions"])

    def test_adding_the_exceptions_config(self):
        f = self.flags("lib/sub/adds.cc")
        self.assertEqual(f["gn_target"], "//lib:adds")
        self.assertTrue(f["exceptions"])

    def test_own_config_with_fexceptions(self):
        self.assertTrue(self.flags("lib/own_config.cc")["exceptions"])

    def test_template_without_defaults(self):
        f = self.flags("lib/no_defaults.cc")
        self.assertTrue(f["rtti"])
        self.assertFalse(f["exceptions"])

    def test_rebased_source_list(self):
        self.assertEqual(self.flags("lib/listed.cc")["gn_target"],
                         "//lib:blink_style")

    def test_non_ancestor_build_file(self):
        self.assertEqual(self.flags("lib/sub/far.cc")["gn_target"],
                         "//other:elsewhere")

    def test_unknown_file(self):
        f = self.flags("lib/nowhere.cc")
        self.assertIsNone(f["gn_target"])
        self.assertFalse(f["exceptions"])

    def test_compile_flags(self):
        self.assertIn("-fno-exceptions", check.language_cflags(None))
        on = check.language_cflags({"exceptions": True, "rtti": False})
        self.assertIn("-fexceptions", on)
        self.assertNotIn("-fno-exceptions", on)
        self.assertIn("-fno-rtti", on)
        self.assertIn("-frtti", check.language_cflags({"rtti": True}))


class MojomFreshnessTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.gen = self.tmp / "gen"
        self.files = {
            "a/a.mojom": 'module a;\nimport "b/b.mojom";\nstruct A {};\n',
            "b/b.mojom": 'module b;\nimport "c/c.mojom";\nstruct B {};\n',
            "c/c.mojom": "module c;\nstruct C {};\n",
        }
        self.tree = FakeTree(self.files)
        self.index = gen_mojom.Index({"mojoms": {}, "typemaps": {}})

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def stamp_all(self):
        tm = gen_mojom.typemap_hash(self.index)
        memo = {}
        stamps = {p: {"key": gen_mojom.stamp_key(
            self.index, p, gen_mojom.closure_hash(self.tree, self.gen, p,
                                                  memo), tm),
            "v": ["chromium"]} for p in self.files}
        (self.tmp / "mojom_stamps.json").write_text(json.dumps(stamps))

    def test_transitive_import_change_is_stale(self):
        self.stamp_all()
        self.assertEqual(gen_mojom.stale(self.tree, self.index, self.tmp,
                                         self.gen), ({}, []))
        self.tree.files["c/c.mojom"] += "struct D {};\n"
        want, gone = gen_mojom.stale(self.tree, self.index, self.tmp,
                                     self.gen)
        self.assertEqual(sorted(want), ["a/a.mojom", "b/b.mojom",
                                        "c/c.mojom"])
        self.assertEqual(gone, [])

    def test_typemap_and_features_change_the_key(self):
        self.stamp_all()
        idx = gen_mojom.Index({"mojoms": {}, "typemaps": {"chromium": {
            "a.A": {"typename": "::A"}}}})
        self.assertEqual(len(gen_mojom.stale(self.tree, idx, self.tmp,
                                             self.gen)[0]), 3)
        idx = gen_mojom.Index({"mojoms": {"a/a.mojom": {
            "enabled_features": ["is_linux"]}}, "typemaps": {}})
        self.assertEqual(sorted(gen_mojom.stale(self.tree, idx, self.tmp,
                                                self.gen)[0]),
                         ["a/a.mojom"])

    def test_deleted_mojom_outputs_are_removed(self):
        self.stamp_all()
        d = self.gen / "c"
        d.mkdir(parents=True)
        for n in ("c.mojom.h", "c.mojom-shared.h", "c.mojom-module",
                  "c.mojom_other.h", "cc.mojom.h"):
            (d / n).write_text("x")
        self.assertEqual([f.name for f in gen_mojom.outputs_of(
            self.gen, "c/c.mojom")],
            ["c.mojom-module", "c.mojom-shared.h", "c.mojom.h"])
        del self.tree.files["c/c.mojom"]
        want, gone = gen_mojom.stale(self.tree, self.index, self.tmp,
                                     self.gen)
        self.assertEqual(gone, ["c/c.mojom"])
        self.assertEqual(sorted(want), ["a/a.mojom", "b/b.mojom"])
        # There is no mojom generator here, so regenerating a and b fails:
        # their outputs go too, rather than staying stale.
        (self.gen / "a").mkdir()
        (self.gen / "a" / "a.mojom.h").write_text("old")
        with self.assertRaises(RuntimeError):
            gen_mojom.refresh(self.tree, self.index, self.tmp, self.gen)
        self.assertEqual(sorted(f.name for f in d.iterdir()),
                         ["c.mojom_other.h", "cc.mojom.h"])
        self.assertFalse((self.gen / "a" / "a.mojom.h").exists())
        stamps = json.loads((self.tmp / "mojom_stamps.json").read_text())
        self.assertEqual(stamps, {})


def make_tarball(members, top):
    """A .tar.xz in memory: {path: bytes or None (a directory)}."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w") as tf:
        for path, data in members.items():
            info = tarfile.TarInfo(top + "/" + path)
            if data is None:
                info.type = tarfile.DIRTYPE
                tf.addfile(info)
                continue
            info.size = len(data)
            info.mode = 0o755 if path.endswith(".py") else 0o644
            tf.addfile(info, io.BytesIO(data))
    return lzma.compress(raw.getvalue())


class TarballTest(unittest.TestCase):
    def test_listing(self):
        xml = ("<ListBucketResult><IsTruncated>true</IsTruncated>"
               "<Key>chromium-152.0.7977.12-lite.tar.xz</Key>"
               "<Key>chromium-152.0.7977.12.tar.xz</Key>"
               "<Key>chromium-152.0.7977.12.tar.xz.hashes</Key>"
               "<Key>chromium-152.0.7977.132.tar.xz</Key>"
               "<NextMarker>chromium-152.0.7977.132.tar.xz</NextMarker>"
               "</ListBucketResult>")
        keys, nxt = tarball.parse_listing(xml)
        self.assertEqual(nxt, "chromium-152.0.7977.132.tar.xz")
        self.assertEqual(tarball.published(keys), {
            "152.0.7977.12": {"lite", "full"},
            "152.0.7977.132": {"full"}})
        self.assertIsNone(tarball.parse_listing(
            "<IsTruncated>false</IsTruncated><Key>k</Key>")[1])

    def test_pick_version(self):
        v = {"152.0.7977.12": 1, "152.0.7977.132": 1, "152.0.7977.170": 1,
             "152.0.7978.1": 1, "153.0.8000.1": 1}
        self.assertEqual(tarball.pick_version("152.0.7977.169", v),
                         "152.0.7977.132")
        self.assertEqual(tarball.pick_version("152.0.7977.170", v),
                         "152.0.7977.170")
        self.assertIsNone(tarball.pick_version("152.0.7977.5", v))
        self.assertTrue(tarball.tarball_url("1.2.3.4", {"lite", "full"})
                        .endswith("/chromium-1.2.3.4-lite.tar.xz"))
        self.assertTrue(tarball.tarball_url("1.2.3.4", {"full"})
                        .endswith("/chromium-1.2.3.4.tar.xz"))

    def test_member_dest(self):
        mounts = ["third_party/webrtc", "third_party/icu"]
        top = "chromium-1.2.3.4"
        self.assertEqual(tarball.member_dest(
            top + "/third_party/webrtc/api/x.h", top, mounts),
            ("third_party/webrtc", "api/x.h"))
        self.assertIsNone(tarball.member_dest(
            top + "/third_party/webrtcx/a.h", top, mounts))
        self.assertIsNone(tarball.member_dest(
            top + "/third_party/icu/source/data/x.txt", top, mounts))
        self.assertIsNone(tarball.member_dest("other/third_party/icu/a.h",
                                              top, mounts))
        # Nothing that would be written outside its mount.
        for bad in ("third_party/webrtc/../../../x.h",
                    "third_party/webrtc/a/../../b.h",
                    "third_party/webrtc/./a.h", "third_party/webrtc//a.h",
                    "third_party/webrtc/"):
            self.assertIsNone(tarball.member_dest(top + "/" + bad, top,
                                                  mounts), bad)

    def test_extract_and_dir_repo(self):
        top = "chromium-1.2.3.4"
        blob = make_tarball({
            "third_party/webrtc": None,
            "third_party/webrtc/api/x.h": b"int x;\n",
            "third_party/webrtc/tools/gen.py": b"print(1)\n",
            "third_party/webrtc/data/big.pcm": b"\0" * 10,
            "third_party/webrtc/huge.bin": b"\1" * (tarball.MAX_FILE + 1),
            "base/not_a_mount.h": b"no\n",
        }, top)
        tmp = Path(tempfile.mkdtemp())
        try:
            dest = tmp / "tarball-1.2.3.4"
            tarball.extract("mem://x", "1.2.3.4", ["third_party/webrtc"],
                            dest, lambda m: None,
                            opener=lambda url: io.BytesIO(blob),
                            pins={"x": hashlib.sha256(blob).hexdigest()})
            man = json.loads((dest / tarball.MANIFEST).read_text())
            self.assertEqual(man["sha256"], hashlib.sha256(blob).hexdigest())
            files = man["mounts"]["third_party/webrtc"]
            self.assertEqual(sorted(files), ["api/x.h", "tools/gen.py"])
            self.assertEqual(files["tools/gen.py"][0], "100755")
            self.assertEqual(files["api/x.h"][1], hashlib.sha1(
                b"blob 7\0int x;\n").hexdigest())
            self.assertFalse((tmp / "tarball-1.2.3.4.partial").exists())
            repo = tarball.DirRepo(dest / "third_party/webrtc", files,
                                   "c0ffee", "tarball 1.2.3.4")
            oid = repo.entries["api/x.h"].oid
            self.assertEqual(dict(repo.blobs([oid])), {oid: b"int x;\n"})
            self.assertEqual(repo.commit, "c0ffee")
        finally:
            shutil.rmtree(tmp)


class TarballPinTest(unittest.TestCase):
    """Nothing from a tarball is used unless the whole tarball matches its
    pinned SHA-256."""

    def setUp(self):
        self.top = "chromium-1.2.3.4"
        self.blob = make_tarball({
            "third_party/webrtc/api/x.h": b"int x;\n",
            "third_party/webrtc/tools/gen.py": b"print(1)\n",
        }, self.top)
        self.tmp = Path(tempfile.mkdtemp())
        self.dest = self.tmp / "tarball-1.2.3.4"

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def extract(self, pins, blob=None):
        data = self.blob if blob is None else blob
        return tarball.extract(
            tarball.BUCKET + "chromium-1.2.3.4-lite.tar.xz", "1.2.3.4",
            ["third_party/webrtc"], self.dest, lambda m: None,
            opener=lambda url: io.BytesIO(data), pins=pins)

    def assertNothingKept(self):
        self.assertFalse(self.dest.exists())
        self.assertFalse((self.tmp / "tarball-1.2.3.4.partial").exists())

    def test_pinned(self):
        self.extract({"chromium-1.2.3.4-lite.tar.xz":
                      hashlib.sha256(self.blob).hexdigest()})
        self.assertTrue((self.dest / "third_party/webrtc/api/x.h").exists())

    def test_unpinned_is_refused(self):
        with self.assertRaises(tarball.TarballNotVerified):
            self.extract({})
        self.assertNothingKept()

    def test_another_tarball_is_refused(self):
        # The same files, after the archive's end the hash covers too.
        with self.assertRaises(tarball.TarballNotVerified):
            self.extract({"chromium-1.2.3.4-lite.tar.xz":
                          hashlib.sha256(self.blob).hexdigest()},
                         blob=self.blob + lzma.compress(b"\0" * 1024))
        self.assertNothingKept()

    def test_the_pin_of_the_pinned_release(self):
        pin = sources._up.read_pin(sources.REPO_ROOT / "chromium_version.txt")
        name = "chromium-%s-lite.tar.xz" % pin.version
        self.assertRegex(tarball.pinned_sha256(tarball.BUCKET + name),
                         "^[0-9a-f]{64}$")

    def test_reader_hashes_what_tarfile_leaves(self):
        # tarfile stops at the archive's end; what follows it (here bytes
        # that don't compress) is hashed too.
        raw = io.BytesIO()
        with tarfile.open(fileobj=raw, mode="w") as tf:
            info = tarfile.TarInfo(self.top + "/third_party/webrtc/a.h")
            info.size = 2
            tf.addfile(info, io.BytesIO(b"a\n"))
        blob = lzma.compress(raw.getvalue() + os.urandom(1 << 18))
        f = io.BytesIO(blob)
        r = tarball.HashingReader(f)
        with tarfile.open(fileobj=r, mode="r|xz") as tf:
            names = [m.name for m in tf]
        self.assertEqual(len(names), 1)
        self.assertLess(f.tell(), len(blob))
        self.assertEqual(r.drain(), hashlib.sha256(blob).hexdigest())


class GritFreshnessTest(unittest.TestCase):
    def test_header_args(self):
        args = ["-i", "../../a/x.grd", "build", "-o", "gen/a", "--depdir",
                ".", "--depfile", "gen/a/x.d", "--write-only-new=1",
                "--depend-on-stamp", "-D", "a=1", "--brotli", "<UNKNOWN>",
                "-f", "gen/tools/gritsettings/default_resource_ids",
                "--assert-file-list", "obj/a/x_expected_outputs.txt"]
        self.assertEqual(gen_misc._header_args(args), [
            "-i", "../../a/x.grd", "build", "-o", "gen/a",
            "--write-only-new=1", "-D", "a=1", "-f",
            "gen/tools/gritsettings/default_resource_ids"])
        self.assertEqual(gen_misc._grd_of(args), "a/x.grd")

    def test_part_edit_changes_the_key(self):
        tree = FakeTree({
            "a/x.grd": '<grit><part file="parts/p.grdp" /></grit>',
            "a/parts/p.grdp": '<grit-part><part file="../q.grdp"/>'
                              '</grit-part>',
            "a/q.grdp": "<grit-part/>",
        })
        tmp = Path(tempfile.mkdtemp())
        try:
            args = ["-i", "../../a/x.grd", "build"]
            self.assertEqual(sorted(gen_misc._grd_closure(tree, "a/x.grd")),
                             ["a/parts/p.grdp", "a/q.grdp", "a/x.grd"])
            k1 = gen_misc._grit_key(tree, tmp, args)
            tree.files["a/q.grdp"] = '<grit-part><message name="IDS_X"/>'
            self.assertNotEqual(k1, gen_misc._grit_key(tree, tmp, args))
            k2 = gen_misc._grit_key(tree, tmp, args)
            ids = tmp / gen_misc.RESOURCE_IDS
            ids.parent.mkdir(parents=True)
            ids.write_text("{}")
            self.assertNotEqual(k2, gen_misc._grit_key(tree, tmp, args))
        finally:
            shutil.rmtree(tmp)


class SourcesStateTest(unittest.TestCase):
    """SourceTree._record: a mount whose source changed drops its files."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        (self.tmp / "overlay").mkdir()
        self.tree = sources.SourceTree(self.tmp / "work", self.tmp / "cache",
                                       overlay=self.tmp / "overlay")
        self.mount = "third_party/webrtc"
        self.files = self.tree.tree_dir / self.mount / "api" / "x.h"
        self.gen = self.tree.gen_dir / self.mount / "y.h"

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def populate(self):
        for f in (self.files, self.gen):
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text("x")

    def test_same_source_keeps_files(self):
        self.tree._record(self.mount, "git r c1", True, None)
        self.populate()
        self.tree._record(self.mount, "git r c1", True, None)
        self.assertTrue(self.files.exists())
        self.assertTrue(self.gen.exists())

    def test_changed_source_drops_files(self):
        self.tree._record(self.mount, "head r c0", False, "webrtc (HEAD)")
        self.populate()
        self.tree._record(self.mount, "tarball 1.2.3.4 c1", True, None)
        self.assertFalse(self.files.exists())
        self.assertFalse(self.gen.exists())
        self.assertEqual(sources.stand_in_mounts(self.tree.work), {})

    def test_changed_source_drops_what_was_made_from_it(self):
        self.tree._record(self.mount, "head r c0", False, "webrtc (HEAD)")
        made = self.tree.gen_dir / "elsewhere" / "x.pb.h"
        kept = self.tree.gen_dir / "elsewhere" / "y.pb.h"
        for f in (made, kept):
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text("x")
        (self.tree.work / gen_actions.GEN_INPUTS).write_text(json.dumps({
            "elsewhere/x.pb.h": [self.mount],
            "elsewhere/y.pb.h": ["third_party/icu"]}))
        self.tree._record(self.mount, "tarball 1.2.3.4 c1", True, None)
        self.assertFalse(made.exists())
        self.assertTrue(kept.exists())

    def test_mirror_names_and_known_misses(self):
        self.assertEqual(sources.cache_name(
            "https://github.com/chromium/pdfium"), "pdfium")
        self.assertEqual(sources.cache_name(
            "https://gitlab.com/kkowalczyk/pdfium.git"),
            "gitlab.com-kkowalczyk-pdfium")
        marker = self.tree.work / "repos" / "unavailable.json"
        marker.parent.mkdir(parents=True, exist_ok=True)
        # The format before several mirrors per mount (one string), and
        # the list: a known miss is not fetched again.
        marker.write_text(json.dumps({"a": "https://x/a @ c1",
                                      "b": ["https://x/b @ c2"]}))
        self.assertIsNone(self.tree._mirror("a", "https://x/a", "c1"))
        self.assertIsNone(self.tree._mirror("b", "https://x/b", "c2"))
        self.assertFalse((self.tree.work / "repos" / "a").exists())

    def test_legacy_work_dir(self):
        # Files from before sources.json: kept for a mirror at the pin,
        # dropped for anything else.
        self.populate()
        self.tree._record(self.mount, "git r c1", True, None)
        self.assertTrue(self.files.exists())
        os.remove(self.tree.work / "repos" / sources.SOURCES_STATE)
        self.tree._record(self.mount, "tarball 1.2.3.4 c0", False,
                          "webrtc (near)")
        self.assertFalse(self.files.exists())
        self.assertEqual(sources.stand_in_mounts(self.tree.work),
                         {self.mount: "webrtc (near)"})

    def test_legacy_head_fallback_to_a_mirror(self):
        # Before sources.json the mount came from the mirror's HEAD
        # (head_fallback.json); now a mirror has the pinned commit.
        self.populate()
        repos = self.tree.work / "repos"
        repos.mkdir(parents=True, exist_ok=True)
        (repos / "head_fallback.json").write_text(json.dumps(
            {self.mount: "c0"}))
        self.tree._record(self.mount, "git https://x/webrtc c1", True, None)
        self.assertFalse(self.files.exists())


class ReportTest(unittest.TestCase):
    def test_classify_names_stand_ins(self):
        tmp = Path(tempfile.mkdtemp())
        try:
            gen = tmp / "gen"
            (gen / "chrome/grit").mkdir(parents=True)
            (gen / "chrome/grit/real.h").write_text(
                "// This file is automatically generated by GRIT.\n")
            (gen / "chrome/grit/webui.h").write_text(
                "// compile_check STAND-IN for grit's chrome/grit/webui.h\n")
            graph = {
                "src:a.cc": ["src:third_party/webrtc/api/x.h",
                             "gen:chrome/grit/real.h"],
                "src:b.cc": ["gen:chrome/grit/webui.h",
                             "src:third_party/dawn/y.h"],
                "src:c.cc": ["gen:chrome/grit/real.h"],
            }
            mounts = {"third_party/webrtc": "third_party/webrtc (near)"}
            ok = {"diags": []}
            st = compile_check.classify(dict(ok, target="a.cc"), graph, gen,
                                        mounts)
            self.assertEqual(st[:2], ("partly on stand-ins",
                                      "third_party/webrtc (near)"))
            st = compile_check.classify(dict(ok, target="b.cc"), graph, gen,
                                        mounts)
            self.assertEqual(st[:2], ("partly on stand-ins",
                                      "grit stand-in (webui.h)"))
            st = compile_check.classify(dict(ok, target="c.cc"), graph, gen,
                                        mounts)
            self.assertEqual(st[0], "checked")
            err = {"target": "a.cc", "diags": [
                {"kind": "error", "msg": "no member", "file": "a.cc",
                 "line": 1}]}
            st = compile_check.classify(err, graph, gen, mounts)
            self.assertEqual(st[:2], ("ERRORS", "1 error(s); reaches "
                                      "third_party/webrtc (near)"))
        finally:
            shutil.rmtree(tmp)


class PlatformsTest(unittest.TestCase):
    def test_platform_of_a_file(self):
        of = platforms.of
        self.assertEqual(of("chrome/browser/molt_ai/x.cc"), "linux")
        self.assertEqual(of("sandbox/policy/win/sandbox_win.cc"), "win")
        self.assertEqual(of("chrome/browser/net/x_dialog_win.cc"), "win")
        self.assertEqual(of("chrome/browser/x_win_unittest.cc"), "win")
        self.assertEqual(of("chrome/browser/app_controller_mac.mm"), "mac")
        self.assertEqual(of("third_party/blink/renderer/platform/fonts/mac/"
                            "font_cache_mac.mm"), "mac")
        self.assertEqual(of("components/remote_cocoa/app_shim/x.mm"), "mac")
        self.assertEqual(of("third_party/opus/src/celt/arm/"
                            "celt_neon_intr.c"), "linux-arm64")
        self.assertEqual(of("third_party/opus/src/celt/x86/x86cpu.c"),
                         "linux")
        self.assertEqual(of("third_party/opus/src/celt/celt.c"), "linux")
        self.assertIsNone(of("chrome/browser/molt_ai/ios/View.mm"))
        self.assertIsNone(of("chrome/browser/x_android.cc"))
        self.assertIsNone(of("chrome/browser/ash/x.cc"))
        # "crash/" is not ChromeOS's ash/.
        self.assertEqual(of("components/crash/x.cc"), "linux")
        self.assertIn("ios", platforms.not_modeled(
            "chrome/browser/molt_ai/ios/View.mm"))

    def test_language(self):
        self.assertEqual(platforms.language("a.c"), "c")
        self.assertEqual(platforms.language("a.cc"), "c++")
        self.assertEqual(platforms.language("a.cpp"), "c++")
        self.assertEqual(platforms.language("a.mm"), "objective-c++")
        self.assertEqual(platforms.language("a.m"), "objective-c")

    def test_linux_atoms_are_the_scanners(self):
        lin = platforms.get("linux")
        self.assertEqual(lin.true_atoms, scan.TRUE_ATOMS)
        self.assertEqual(lin.false_atoms, scan.FALSE_ATOMS)
        for name in platforms.all_names():
            p = platforms.get(name)
            self.assertFalse(p.true_atoms & p.false_atoms, name)
        win = platforms.get("win")
        self.assertIn("BUILDFLAG(IS_WIN)", win.true_atoms)
        self.assertIn("BUILDFLAG(IS_POSIX)", win.false_atoms)
        mac = platforms.get("mac")
        self.assertIn("BUILDFLAG(IS_APPLE)", mac.true_atoms)
        self.assertIn("defined(__aarch64__)", mac.true_atoms)
        self.assertEqual(mac.mojom_features, ("is_posix", "is_mac",
                                              "is_apple"))
        self.assertEqual(win.mojom_features, ("is_win",))
        self.assertEqual(platforms.get("linux-arm64").out_rel,
                         "out/cc-linux-arm64")

    def test_state_dirs(self):
        w = Path("/w")
        self.assertEqual(platforms.get("linux").state_dir(w), w)
        self.assertEqual(platforms.get("win").state_dir(w),
                         w / "platform-win")


class ScanAtomsTest(unittest.TestCase):
    SRC = (b"#if BUILDFLAG(USE_VAAPI)\n#include \"a.h\"\n"
           b"#if BUILDFLAG(ENABLE_OOP_VIDEO_DECODER)\n#include \"b.h\"\n"
           b"#endif\n#endif\n#if PA_BUILDFLAG(USE_X)\n#include \"c.h\"\n"
           b"#endif\n#if BUILDFLAG(IS_WIN)\n#include \"w.h\"\n#endif\n")

    def names(self, atoms=None):
        return [n for _, n, _ in scan.extract_includes(self.SRC, atoms)]

    def test_unknown_buildflags_are_followed(self):
        self.assertEqual(self.names(), ["a.h", "b.h", "c.h"])

    def test_generated_buildflags_decide(self):
        tmp = Path(tempfile.mkdtemp())
        try:
            (tmp / "media").mkdir()
            (tmp / "media" / "media_buildflags.h").write_text(
                "#define BUILDFLAG_INTERNAL_USE_VAAPI() (1)\n"
                "#define BUILDFLAG_INTERNAL_ENABLE_OOP_VIDEO_DECODER() (0)\n")
            (tmp / "pa").mkdir()
            (tmp / "pa" / "buildflags.h").write_text(
                "#define PA_BUILDFLAG_INTERNAL_USE_X() (0)\n")
            # Two headers that disagree: unknown.
            (tmp / "x_features.h").write_text(
                "#define BUILDFLAG_INTERNAL_SPLIT() (1)\n")
            (tmp / "y_features.h").write_text(
                "#define BUILDFLAG_INTERNAL_SPLIT() (0)\n")
            flags = scan.read_buildflags(tmp)
            self.assertEqual(flags, {"USE_VAAPI": True,
                                     "ENABLE_OOP_VIDEO_DECODER": False,
                                     "PA_USE_X": False})
            win = platforms.get("win")
            atoms = scan.Atoms(win.true_atoms, win.false_atoms, flags)
            self.assertEqual(self.names(atoms), ["a.h", "w.h"])
        finally:
            shutil.rmtree(tmp)

    def test_system_headers_of_the_platform(self):
        tmp = Path(tempfile.mkdtemp())
        try:
            (tmp / "um").mkdir()
            (tmp / "um" / "Windows.h").write_text("")
            (tmp / "um" / "windows.h").symlink_to("Windows.h")
            fw = tmp / "Frameworks" / "CoreText.framework" / "Headers"
            fw.mkdir(parents=True)
            (fw / "CoreText.h").write_text("")
            sc = scan.Scanner(FakeTree({}), [], tmp / "gen",
                              system_dirs=[tmp / "um", tmp / "Frameworks"])
            self.assertTrue(sc._in_system("windows.h"))
            self.assertTrue(sc._in_system("WINDOWS.H"))
            self.assertTrue(sc._in_system("CoreText/CoreText.h"))
            self.assertFalse(sc._in_system("AppKit/AppKit.h"))
        finally:
            shutil.rmtree(tmp)

    def test_scoped_include_dirs(self):
        self.assertIn("v8", config.scoped_include_dirs("v8/src/init/v8.cc"))
        self.assertIn("third_party/llvm-libc/src", config.scoped_include_dirs(
            "third_party/llvm-libc/src/shared/math.h"))
        self.assertEqual(config.scoped_include_dirs("base/x.h"), [])
        tree = FakeTree({"v8/src/base/a.h": "", "third_party/skia/src/a.h": "",
                         "v8/src/init/v8.cc": ""})
        sc = scan.Scanner(tree, ["", "third_party/skia"], Path("/nonexistent"))
        # "src/base/a.h" from V8's own file: V8's root; from anyone else:
        # nowhere (V8's root is not searched for them).
        self.assertEqual(sc.resolve("src/base/a.h", ("src", "v8/src/x.cc"),
                                    False), ("src", "v8/src/base/a.h"))
        self.assertIsNone(sc.resolve("src/base/a.h", ("src", "cc/x.cc"),
                                     False))
        self.assertEqual(sc.resolve("src/a.h", ("src", "v8/src/x.cc"),
                                    True), ("src", "third_party/skia/src/a.h"))


class MojomParseOrderTest(unittest.TestCase):
    def test_own_features_import_order_and_failures(self):
        deps = {"base.mojom": [], "a.mojom": ["base.mojom"],
                "arc.mojom": [], "uses_arc.mojom": ["arc.mojom"],
                "b.mojom": ["a.mojom", "base.mojom"]}
        feats = {"a.mojom": frozenset({"is_linux", "use_x"})}
        calls = []

        def parse(group, f):
            calls.append((tuple(group), tuple(sorted(f))))
            for p in group:
                # Each file's imports are parsed before it.
                for d in deps[p]:
                    self.assertTrue(any(d in g for g, _ in calls[:-1]), p)
            if "arc.mojom" in group:
                return "Extensible enum must specify a [Default]"
            return None

        done, failed = gen_mojom.parse_in_order(
            deps, lambda p: feats.get(p, frozenset({"is_linux"})), parse)
        self.assertEqual(done, {"base.mojom", "a.mojom", "b.mojom"})
        self.assertEqual(sorted(failed), ["arc.mojom", "uses_arc.mojom"])
        self.assertIn("arc.mojom", failed["uses_arc.mojom"])
        # a.mojom is parsed with its own target's features only.
        self.assertIn((("a.mojom",), ("is_linux", "use_x")), calls)
        self.assertFalse(any("use_x" in f for g, f in calls
                             if "b.mojom" in g))

    def test_one_bad_file_in_a_batch(self):
        deps = {"x.mojom": [], "y.mojom": [], "z.mojom": []}

        def parse(group, f):
            return "bad" if "y.mojom" in group else None

        done, failed = gen_mojom.parse_in_order(
            deps, lambda p: frozenset(), parse)
        self.assertEqual(done, {"x.mojom", "z.mojom"})
        self.assertEqual(failed, {"y.mojom": "bad"})


class CxxBridgeTest(unittest.TestCase):
    """Bridge headers are made with the platform's cfgs and remade when the
    .rs changes."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.files = {"base/files/file_path.rs": "#[cxx::bridge] mod ffi {}"}
        self.tree = FakeTree(self.files, root=str(self.tmp / "src"))
        self.tree.work = self.tmp
        self.tree.state = self.tmp
        self.tree.gen_dir = self.tmp / "gen"
        self.tree.platform = platforms.get("linux")
        self.tree.materialize = lambda paths: None
        (self.tmp / "src" / "base" / "files").mkdir(parents=True)
        (self.tmp / "src" / "base" / "files" / "file_path.rs").write_text(
            self.files["base/files/file_path.rs"])
        # A cxxbridge that writes its arguments as the header.
        exe = self.tmp / "bin" / "cxxbridge"
        exe.parent.mkdir()
        exe.write_text("#!/bin/sh\necho \"// $*\"\n")
        exe.chmod(0o755)
        (self.tmp / "toolchain").mkdir()
        (self.tmp / "toolchain" / "rustc-print-cfg-x86_64-unknown-linux-gnu"
         ".txt").write_text('target_family="unix"\nunix\n')
        self.old_tools = dict(gen_misc.TOOLS)
        gen_misc.TOOLS["cxxbridge"] = lambda: exe

    def tearDown(self):
        gen_misc.TOOLS.clear()
        gen_misc.TOOLS.update(self.old_tools)
        shutil.rmtree(self.tmp)

    def test_cfgs_and_freshness(self):
        name = "base/files/file_path.rs.h"
        text = gen_misc.cxx_bridge_header(self.tree, None, name)
        self.assertIn('--cfg=target_family="unix"', text)
        self.assertIn("--cfg=unix", text)
        self.assertTrue(text.rstrip().endswith("--header"))
        out = self.tree.gen_dir / name
        out.parent.mkdir(parents=True)
        out.write_text(text)
        self.assertEqual(gen_misc.refresh_cxx(self.tree, None), ([], []))
        self.tree.files["base/files/file_path.rs"] += "\n// changed"
        self.assertEqual(gen_misc.refresh_cxx(self.tree, None),
                         ([name], []))
        # A header from before stamps is remade once.
        other = self.tree.gen_dir / "base" / "old.rs.h"
        other.write_text("old")
        self.tree.files["base/old.rs"] = "x"
        self.assertEqual(gen_misc.refresh_cxx(self.tree, None),
                         (["base/old.rs.h"], []))
        # A bridge that is gone takes its header with it.
        del self.tree.files["base/old.rs"]
        self.assertEqual(gen_misc.refresh_cxx(self.tree, None),
                         ([], ["base/old.rs.h"]))
        self.assertFalse(other.exists())

    def test_install_takes_the_locked_dependencies(self):
        # cxxbridge-cmd's dependencies are the versions its Cargo.lock
        # pins, not the newest crates.io has.
        cmd = host_tools.cxxbridge_install_cmd(
            "cargo", Path("/v/cxxbridge-cmd-v1"), Path("/w/cxxbridge"))
        self.assertEqual(cmd[:2], ["cargo", "install"])
        self.assertIn("--locked", cmd)
        self.assertEqual(cmd[cmd.index("--path") + 1], "/v/cxxbridge-cmd-v1")
        self.assertNotIn("--offline", cmd)


class CompileCommandTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.work = self.tmp / "work"

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def tree(self, plat, system_dirs=()):
        t = sources.SourceTree(self.work, self.tmp / "cache",
                               overlay=self.tmp, platform=plat)
        t.system_dirs = list(system_dirs)
        return t

    def cmd(self, plat, target, lang=None, system_dirs=()):
        return check.compile_command(self.tree(plat, system_dirs), self.work,
                                     target, lang=lang)

    def test_linux_cxx_and_c(self):
        c = self.cmd("linux", "chrome/browser/a.cc")
        self.assertTrue(c[0].endswith("/clang++"))
        self.assertIn("-std=c++23", c)
        self.assertIn("-fno-exceptions", c)
        self.assertTrue(any("find-bad-constructs" in x for x in c))
        c = self.cmd("linux", "third_party/opus/src/celt/celt.c")
        self.assertTrue(c[0].endswith("/clang"))
        self.assertIn("-std=c11", c)
        for flag in ("-std=c++23", "-fno-exceptions", "-nostdinc++"):
            self.assertNotIn(flag, c)
        self.assertFalse(any("plugin" in x for x in c))
        self.assertFalse(any("libc++" in x and "src/include" in x
                             for x in c))

    def test_arm64(self):
        c = self.cmd("linux-arm64", "third_party/opus/src/celt/arm/x.c")
        self.assertIn("--target=aarch64-linux-gnu", c)
        self.assertNotIn("-msse3", c)

    def test_windows(self):
        sdk = [self.tmp / "msvc", self.tmp / "um"]
        c = self.cmd("win", "sandbox/policy/win/sandbox_win.cc",
                     lang={"rtti": True}, system_dirs=sdk)
        self.assertTrue(c[0].endswith("/clang-cl"))
        self.assertIn("/Zs", c)
        self.assertIn("/std:c++23preview", c)
        self.assertIn("/GR", c)
        self.assertNotIn("/GR-", c)
        self.assertIn("-DUNICODE", c)
        self.assertIn("-D_WIN32_WINNT=0x0A00", c)
        self.assertIn("/imsvc" + str(self.tmp / "msvc"), c)
        self.assertIn("-ivfsoverlay", c)
        self.assertNotIn("-DUSE_UDEV", c)
        self.assertFalse(any(x.startswith("--sysroot") for x in c))

    def test_mac(self):
        c = self.cmd("mac", "chrome/browser/a_mac.mm")
        self.assertTrue(c[0].endswith("/clang++"))
        self.assertIn("--target=arm64-apple-macos", c)
        self.assertIn("-fobjc-arc", c)
        self.assertIn("-isysroot", c)
        self.assertIn("-DWEBRTC_MAC", c)
        self.assertNotIn("-DWEBRTC_LINUX", c)
        self.assertNotIn("-msse3", c)

    def test_own_and_scoped_flags(self):
        c = self.cmd("linux", "v8/src/base/ieee754.cc",
                     lang={"flags": ["-DOWN=1"],
                           "scoped": ["third_party/llvm-libc/src"]})
        self.assertIn("-DOWN=1", c)
        self.assertTrue(any(x.endswith("third_party/llvm-libc/src")
                            for x in c))

    def test_diagnostics_both_styles(self):
        d = check.parse_diags(
            "a/b.cc:12:3: error: no member\n"
            "x\\y.h(7,9): warning: unused\n"
            "In file included from a.cc:1:\n")
        self.assertEqual([(x["file"], x["line"], x["kind"]) for x in d],
                         [("a/b.cc", 12, "error"), ("x\\y.h", 7, "warning")])


class ExtraPlatformsTest(unittest.TestCase):
    OLD = (b"#include \"a.h\"\n"
           b"void F() {\n"
           b"#if BUILDFLAG(IS_WIN)\n"
           b"  LowerToken();\n"
           b"#endif\n"
           b"#if BUILDFLAG(IS_APPLE)\n"
           b"  Apple();\n"
           b"#else\n"
           b"  Other();\n"
           b"#endif\n"
           b"}\n")

    def test_live_lines_follow_continuations(self):
        src = (b"#if defined(X) && \\\n    BUILDFLAG(IS_WIN)\nw\n#endif\nz")
        lin = platforms.get("linux")
        self.assertEqual(scan.live_lines(src, scan.Atoms(
            lin.true_atoms, lin.false_atoms)),
            [True, True, False, False, True])

    def test_hook_in_the_windows_block(self):
        new = self.OLD.replace(b"  LowerToken();\n",
                               b"  MoltWarmUp();  // MoltBrowser\n"
                               b"  LowerToken();\n")
        self.assertEqual(targets.extra_platforms(
            "content/utility/utility_main.cc", new, self.OLD), ["win"])

    def test_hook_in_the_apple_branch(self):
        new = self.OLD.replace(b"  Apple();\n", b"  Apple(molt);\n")
        self.assertEqual(targets.extra_platforms("a/font_metrics.cc", new,
                                                 self.OLD), ["mac"])

    def test_hook_in_the_non_apple_branch_is_linux_s(self):
        new = self.OLD.replace(b"  Other();\n", b"  Other(molt);\n")
        self.assertEqual(targets.extra_platforms("a/x.cc", new, self.OLD),
                         [])

    def test_new_file_and_mac_file(self):
        self.assertEqual(targets.extra_platforms("a/new.cc", self.OLD, None),
                         ["mac", "win"])
        # Only macOS builds a .mm file; nor does Windows a POSIX one.
        self.assertEqual(targets.extra_platforms("a/x_mac.mm", self.OLD,
                                                 None), [])
        self.assertEqual(targets.extra_platforms("a/x_posix.cc", self.OLD,
                                                 None), ["mac"])
        self.assertEqual(targets.changed_lines(b"a\nb\nc", b"a\nc"), {1})

    def test_vulkan_switch_variant(self):
        old = (b"void F() {\n#if BUILDFLAG(MOLT_LLAMA_VULKAN)\n  G();\n"
               b"#endif\n}\n")
        new = old.replace(b"  G();\n", b"  G(1);\n")
        self.assertEqual(targets.extra_platforms(
            "chrome/browser/molt_ai/runtime/r.cc", new, old),
            ["linux-vulkan"])
        self.assertEqual(platforms.of(
            "third_party/llama_cpp/chromium/ggml-vulkan-moltbrowser.cpp"),
            "linux-vulkan")
        self.assertEqual(platforms.of(
            "third_party/llama_cpp/chromium/ggml_vk_glslc.cc"),
            "linux-vulkan")
        self.assertEqual(platforms.of(
            "third_party/llama_cpp/chromium/llama_guard.cc"), "linux")
        v = platforms.get("linux-vulkan")
        self.assertEqual(v.extra_args, {"molt_llama_vulkan": True})
        self.assertNotEqual(v.out_rel, platforms.get("linux").out_rel)
        self.assertTrue(platforms.may_build("a/x_linux.cc", "linux-vulkan"))

    def test_headers_follow_the_rules(self):
        self.assertEqual(platforms.of("chrome/browser/app_controller_mac.h"),
                         "mac")
        self.assertEqual(platforms.language(
            "chrome/browser/app_controller_mac.h"), "objective-c++")
        self.assertEqual(platforms.of("a/b_win.h"), "win")
        self.assertEqual(platforms.language("a/b.h"), "c++")
        self.assertFalse(platforms.may_build("a/b_posix.h", "win"))
        tree = sources.SourceTree(Path("/w"), Path("/c"), overlay=Path("/o"))
        tree.platform = platforms.get("mac")
        c = check.compile_command(tree, Path("/w"), "a/x_mac.h")
        self.assertEqual(c[-3:-1], ["-x", "objective-c++-header"])
        c = check.compile_command(sources.SourceTree(
            Path("/w"), Path("/c"), overlay=Path("/o")), Path("/w"), "a/x.h")
        self.assertEqual(c[-3:-1], ["-x", "c++-header"])

    def test_textual_header_through_its_includer(self):
        tree = FakeTree({
            "m/skcms/skcms.cc": '#include "src/Transform_inl.h"\n',
            "m/skcms/other.cc": "int x;\n",
            "m/skcms/src/Transform_inl.h": "// no guard\n"})
        self.assertTrue(targets.TEXTUAL_HEADER_RE.search(
            "m/skcms/src/Transform_inl.h"))
        self.assertFalse(targets.TEXTUAL_HEADER_RE.search("m/a.h"))
        self.assertEqual(targets.includer_of(tree,
                                             "m/skcms/src/Transform_inl.h"),
                         "m/skcms/skcms.cc")
        r = {"target": "m/skcms/src/Transform_inl.h",
             "via": "m/skcms/skcms.cc", "platform": "linux", "diags": []}
        self.assertEqual(compile_check.classify(r, {}, Path("/x"))[:2],
                         ("checked", "through m/skcms/skcms.cc"))
        self.assertEqual(platforms.of(
            "components/remote_cocoa/app_shim/x_bridge.h"), "mac")

    def test_platform_dirs_in_scoped_include_dirs(self):
        try:
            config.set_platform(platforms.get("win"))
            self.assertIn(
                "third_party/ffmpeg/chromium/config/Chromium/win/x64",
                config.scoped_include_dirs(
                    "media/base/molt_portable_media.cc"))
        finally:
            config.set_platform(platforms.get("linux"))
        self.assertIn("third_party/ffmpeg/chromium/config/Chromium/linux/x64",
                      config.scoped_include_dirs("third_party/ffmpeg/a.h"))

    def test_may_build(self):
        self.assertTrue(platforms.may_build("a/x.cc", "win"))
        self.assertFalse(platforms.may_build("a/x_posix.cc", "win"))
        self.assertTrue(platforms.may_build("a/x_posix.cc", "mac"))
        self.assertFalse(platforms.may_build("a/linux/x.cc", "mac"))
        self.assertFalse(platforms.may_build("a/x_linux_unittest.cc", "win"))
        self.assertFalse(platforms.may_build("a/x_win.cc", "linux"))
        self.assertFalse(platforms.may_build("a/x_android.cc", "linux"))
        self.assertTrue(platforms.may_build("a/arm/x_neon.c", "mac"))

    def test_own_sources_also_on_windows_if_built(self):
        class Tree:
            def upstream_entry(self, p):
                return None  # every file is MoltBrowser's own
        tmp = Path(tempfile.mkdtemp())
        old_src = targets.SRC
        try:
            for f in ("m/own.cc", "m/own_posix.cc", "m/own_win.cc"):
                (tmp / f).parent.mkdir(parents=True, exist_ok=True)
                (tmp / f).write_bytes(b"int x;\n")
            targets.SRC = tmp
            pairs = targets.platform_targets(
                Tree(), ["m/own.cc", "m/own_posix.cc", "m/own_win.cc"])
        finally:
            targets.SRC = old_src
            shutil.rmtree(tmp)
        self.assertEqual(pairs, [("m/own.cc", "linux"),
                                 ("m/own.cc", "win", "if_built"),
                                 ("m/own_posix.cc", "linux"),
                                 ("m/own_win.cc", "win")])
        self.assertEqual(targets.if_built(pairs), {"win": {"m/own.cc"}})
        groups, _ = compile_check.group_targets(pairs)
        self.assertEqual(groups, {"linux": ["m/own.cc", "m/own_posix.cc"],
                                  "win": ["m/own.cc", "m/own_win.cc"]})


class GenCandidatesTest(unittest.TestCase):
    def test_library_generated_roots(self):
        c = compile_check._gen_candidates(
            "torque-generated/instance-types.h", "linux",
            ["v8/src/objects/instance-types-gen.h"])
        self.assertIn("v8/torque-generated/instance-types.h", c)
        self.assertIn("v8/src/objects/torque-generated/instance-types.h", c)
        c = compile_check._gen_candidates(
            "ggml-vulkan-shaders.hpp", "linux-vulkan",
            ["third_party/llama_cpp/ggml/src/ggml-vulkan/"
             "ggml-vulkan-common.h"])
        self.assertIn("third_party/llama_cpp/ggml-vulkan/"
                      "ggml-vulkan-shaders.hpp", c)

    def test_label_root_out_dir(self):
        files = {"BUILD.gn": """
group("g") {
  data = [
    get_label_info(":p($host_toolchain)", "root_out_dir"),
    get_label_info(":p(//build/toolchain/win:x)", "root_out_dir"),
    get_label_info(":p", "toolchain"),
  ]
}
"""}
        it = make_interp(files)
        tc = "//build/toolchain/linux:clang_x64"
        for k in ("default_toolchain", "current_toolchain", "host_toolchain"):
            it.globals.set(k, tc)
        g = [t for t in it.run_file("//BUILD.gn") if t.name == "g"][0]
        self.assertEqual(g.get("data"), [
            gn.OUT, gn.OUT + "/x", "//build/toolchain/linux:clang_x64"])


class MojomImportsTest(unittest.TestCase):
    TEXT = ('import "a.mojom";\n'
            '[EnableIf=is_chromeos]\nimport "cros.mojom";\n'
            '[EnableIfNot=is_win]\n  import "not_win.mojom";\n'
            '[EnableIf=is_win]\nimport "win.mojom";\n')

    def test_conditional_imports(self):
        self.assertEqual(gen_mojom.imports_of(self.TEXT),
                         ["a.mojom", "cros.mojom", "not_win.mojom",
                          "win.mojom"])
        self.assertEqual(gen_mojom.imports_of(self.TEXT, {"is_win"}),
                         ["a.mojom", "win.mojom"])
        self.assertEqual(gen_mojom.imports_of(self.TEXT,
                                              {"is_linux", "is_posix"}),
                         ["a.mojom", "not_win.mojom"])

    def test_closure_follows_the_kept_imports(self):
        files = {"r.mojom": self.TEXT.encode(), "a.mojom": b"",
                 "win.mojom": b"", "not_win.mojom": b"",
                 "cros.mojom": b'import "missing.mojom";\n'}
        tree = FakeTree({k: v.decode() for k, v in files.items()})
        got = gen_mojom.import_closure(tree, "/nonexistent", ["r.mojom"],
                                       feats_of=lambda p: {"is_win"})
        self.assertEqual(sorted(got), ["a.mojom", "r.mojom", "win.mojom"])


class GritArgsTest(unittest.TestCase):
    def test_ninja_only_options_dropped(self):
        args = ["-i", "x.grd", "build", "--assert-output-file-list",
                "obj/x_expected_outputs.txt", "--depfile", "x.d",
                "--assert-input-file-list=y", "-o", "gen"]
        self.assertEqual(gen_misc._header_args(args),
                         ["-i", "x.grd", "build", "-o", "gen"])


class SaveResultsTest(unittest.TestCase):
    def test_platform_runs_keep_the_others(self):
        import argparse
        tmp = Path(tempfile.mkdtemp())
        try:
            def r(t, p):
                return {"target": t, "platform": p, "diags": []}
            (tmp / "check.json").write_text(json.dumps([
                r("a.cc", "linux"), r("a.cc", "win"), r("b.cc", "win"),
                r("c_android.cc", None)]))
            args = argparse.Namespace(work=tmp, platforms=["win"])
            compile_check.save_results(args, [r("a.cc", "win")], False)
            got = sorted((x["target"], x["platform"] or "") for x in
                         json.loads((tmp / "check.json").read_text()))
            self.assertEqual(got, [("a.cc", "linux"), ("a.cc", "win"),
                                   ("c_android.cc", "")])
            args = argparse.Namespace(work=tmp, platforms=None)
            compile_check.save_results(args, [r("a.cc", "linux")], True)
            got = sorted((x["target"], x["platform"] or "") for x in
                         json.loads((tmp / "check.json").read_text()))
            self.assertEqual(got, [("a.cc", "linux"), ("c_android.cc", "")])
        finally:
            shutil.rmtree(tmp)


class TargetGroupsTest(unittest.TestCase):
    def test_vendored(self):
        self.assertTrue(targets.is_vendored(
            "third_party/llama_cpp/ggml/src/ggml.c"))
        self.assertFalse(targets.is_vendored(
            "third_party/llama_cpp/chromium/llama_guard.cc"))
        self.assertTrue(targets.is_vendored(
            "third_party/rust/chromium_crates_io/vendor/x/main.cpp"))

    def test_grouping(self):
        groups, skipped = compile_check.group_targets([
            "a.cc", "b_win.cc", "c.mm", "d/arm/e.c", "f_android.cc"])
        self.assertEqual(groups, {"linux": ["a.cc"], "win": ["b_win.cc"],
                                  "mac": ["c.mm"],
                                  "linux-arm64": ["d/arm/e.c"]})
        self.assertEqual(list(skipped), ["f_android.cc"])
        self.assertEqual(compile_check.ordered(groups),
                         ["linux", "linux-arm64", "mac", "win"])


class OwnFlagsTest(unittest.TestCase):
    def test_target_configs_and_template_expansion(self):
        gen_actions._build_texts = None
        files = {
            "build/config/compiler/BUILD.gn": COMPILER_BUILD,
            "lib/BUILD.gn": """
config("private") {
  defines = [ "OPUS_BUILD" ]
  include_dirs = [ "src/celt" ]
  cflags_c = [ "-Wno-c-only" ]
  cflags_cc = [ "-Wno-cc-only" ]
}
template("wrapped_source_set") {
  source_set(target_name) {
    forward_variables_from(invoker, "*")
    configs += [ ":private" ]
  }
}
source_set("plain") {
  sources = [ "plain.c" ]
  defines = [ "OWN=1" ]
  configs += [ ":private", "//build/config/compiler:exceptions" ]
}
wrapped_source_set("wrapped") { sources = [ "w.cc" ] }
""",
        }
        tree = FakeTree(files)
        interp = make_interp(files)
        f = gnconfigs.target_flags(tree, interp, ["lib/plain.c",
                                                  "lib/w.cc"])
        plain = f["lib/plain.c"]["flags"]
        self.assertIn("-DOWN=1", plain)
        self.assertIn("-DOPUS_BUILD", plain)
        self.assertIn("-I" + str(Path("/nonexistent/src/lib/src/celt")),
                      plain)
        self.assertIn("-Wno-c-only", plain)
        self.assertNotIn("-Wno-cc-only", plain)
        # The compiler's own configs are config.py's business.
        self.assertNotIn("-fexceptions", plain)
        self.assertTrue(f["lib/plain.c"]["exceptions"])
        w = f["lib/w.cc"]["flags"]
        self.assertIn("-DOPUS_BUILD", w)
        self.assertIn("-Wno-cc-only", w)


class WindowsSdkTest(unittest.TestCase):
    def test_aliases_overlay_and_safe_extract(self):
        import zipfile
        tmp = Path(tempfile.mkdtemp())
        try:
            nupkg = tmp / "p.nupkg"
            with zipfile.ZipFile(nupkg, "w") as z:
                z.writestr("c/Include/v/um/Windows.h", "w")
                z.writestr("c/Include/v/um/sub/X.h", "x")
                z.writestr("c/Include/v/cppwinrt/a.h", "skip")
                z.writestr("c/Include/v/../../escape.h", "no")
                z.writestr("other/b.h", "no")
            out = tmp / "sdk"
            self.assertEqual(sdks._extract(nupkg, "c/Include/v/", out,
                                           only=["um"]), 2)
            self.assertFalse((tmp / "escape.h").exists())
            self.assertFalse((out / "cppwinrt").exists())
            self.assertEqual(sdks.add_lowercase_aliases(out), 2)
            self.assertEqual((out / "um" / "windows.h").read_text(), "w")
            self.assertEqual((out / "um" / "sub" / "x.h").read_text(), "x")
            ov = sdks.write_case_insensitive_overlay([out / "um"],
                                                     tmp / "o.yaml")
            data = json.loads(Path(ov).read_text())
            self.assertEqual(data["case-sensitive"], "false")
            names = [e["name"] for e in data["roots"][0]["contents"]]
            # The aliases are left out (the overlay is case-insensitive).
            self.assertEqual(sorted(names), ["Windows.h", "sub"])
        finally:
            shutil.rmtree(tmp)

    def test_pinned_packages(self):
        for pkg, ver, sha, prefix, sub in sdks.WIN_PACKAGES:
            self.assertEqual(len(base64.b64decode(sha)), 64, pkg)
        self.assertIn(sdks.WIN_SDK_VERSION, sdks.WIN_PACKAGES[0][3])
        self.assertEqual(len(sdks.ZIG_WHEEL[1]), 64)


class WindowsAlignedNewTest(unittest.TestCase):
    def test_added_once_before_the_end(self):
        tmp = Path(tempfile.mkdtemp())
        try:
            f = tmp / "vcruntime_new.h"
            f.write_text('struct nothrow_t {};\n#pragma pop_macro("new")\n')
            self.assertTrue(sdks.add_aligned_new(f))
            self.assertFalse(sdks.add_aligned_new(f))
            text = f.read_text()
            self.assertEqual(text.count("std::align_val_t _Al,\n"
                                        "                                     "
                                        "        std::nothrow_t const&"), 1)
            self.assertLess(text.index("#ifdef __cpp_aligned_new"),
                            text.index('#pragma pop_macro("new")'))
        finally:
            shutil.rmtree(tmp)


class GeneratedStandInsTest(unittest.TestCase):
    def test_preload_header_from_its_template(self):
        tmpl = ("static constexpr uint8_t kHSTSHuffmanTree[] = "
                "[[HUFFMAN_TREE]];\n"
                "static constexpr unsigned kPreloadedHSTSBits = "
                "[[HSTS_TRIE_BITS]];\n")
        files = {"net/http/BUILD.gn": """
action("generate_transport_security_state") {
  script = "run.py"
  inputs = [ "transport_security_state_static.json",
             "transport_security_state_static.template" ]
  outputs = [ "$target_gen_dir/transport_security_state_static.h" ]
  args = []
}
""", "net/http/transport_security_state_static.template": tmpl}
        gen_actions._build_texts = None
        tree = FakeTree(files)
        interp = make_interp(files)
        name = "net/http/transport_security_state_static.h"
        # A fallback: the generator's GN action is tried first.
        self.assertFalse(gen_misc.handles_exclusively(name))
        self.assertTrue(gen_misc.handles(name))
        self.assertTrue(gen_misc.is_fallback(name))
        text = gen_misc.preload_header(tree, interp, name)
        self.assertTrue(text.startswith("// compile_check STAND-IN"))
        self.assertIn("kHSTSHuffmanTree[] = {0};", text)
        self.assertIn("kPreloadedHSTSBits = 0;", text)
        self.assertNotIn("[[", text)

    def test_pins_header_in_the_generators_shape(self):
        tmpl = ("[[SPKI_HASHES]]\n\n[[ACCEPTABLE_CERTS]]\n\n[[PINSETS]]\n"
                "static constexpr auto kHostPins = [[HOST_PINS]];\n")
        files = {"net/http/BUILD.gn": """
action("generate_transport_security_state_pins") {
  script = "run.py"
  inputs = [ "transport_security_state_static_pins.template" ]
  outputs = [ "$target_gen_dir/transport_security_state_static_pins.h" ]
  args = []
}
""", "net/http/transport_security_state_static_pins.template": tmpl}
        gen_actions._build_texts = None
        text = gen_misc.preload_header(
            FakeTree(files), make_interp(files),
            "net/http/transport_security_state_static_pins.h")
        self.assertIn("kSPKIHash_CompileCheck = {", text)
        self.assertIn("&kSPKIHash_CompileCheck,", text)
        self.assertIn("kCompileCheckPinset = {kCompileCheckAcceptableCerts, "
                      "{}};", text)
        self.assertIn("kHostPins = base::MakeFixedFlatMap<std::string_view, "
                      "net::TransportSecurityStatePinsSource::HostPin>({",
                      text)
        self.assertEqual(text.count("0x00"), 32)

    def test_report_names_generated_and_flag_stand_ins(self):
        tmp = Path(tempfile.mkdtemp())
        try:
            gen = tmp / "gen"
            (gen / "net/http").mkdir(parents=True)
            (gen / "net/http/tss.h").write_text(
                "// compile_check STAND-IN for net/http/tss.h\n")
            mark = "src:" + config.STAND_IN_FLAGS["v8/"][1]
            graph = {"src:net/x.cc": ["gen:net/http/tss.h"],
                     "src:v8/src/init/v8.cc": [mark]}
            st = compile_check.classify({"target": "net/x.cc", "diags": []},
                                        graph, gen)
            self.assertEqual(st[:2], ("partly on stand-ins",
                                      "tss.h stand-in"))
            st = compile_check.classify(
                {"target": "v8/src/init/v8.cc", "diags": []}, graph, gen)
            self.assertEqual(st[0], "partly on stand-ins")
            self.assertIn("Torque", st[1])
        finally:
            shutil.rmtree(tmp)
        tree = FakeTree({})
        c = check.compile_command(sources.SourceTree(
            Path("/w"), Path("/c"), overlay=Path("/o")), Path("/w"),
            "v8/src/init/v8.cc")
        self.assertIn("-DV8_USE_METAGEN_INSTANCE_TYPES=0", c)
        self.assertIn("V8_USE_METAGEN_INSTANCE_TYPES", scan.FALSE_ATOMS)


class FallbackStandInsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.tree = types.SimpleNamespace(
            gen_dir=self.tmp / "gen", state=self.tmp / "state",
            chromium=types.SimpleNamespace(rev="pin1"))
        f = self.tree.gen_dir / "base" / "generated_build_date.h"
        f.parent.mkdir(parents=True)
        f.write_text("// compile_check STAND-IN for base/x\n")
        self.f = f

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_stand_in_replaced_when_the_action_runs(self):
        def produce(rels):
            self.assertFalse(self.f.exists())  # moved aside first
            self.f.write_text("#define BASE_GENERATED_BUILD_DATE_TIMESTAMP 1\n")
            return rels
        self.assertEqual(gen_misc.replace_stand_ins(self.tree, None, produce),
                         ["base/generated_build_date.h"])
        self.assertNotIn("STAND-IN", self.f.read_text())
        self.assertEqual(list(self.f.parent.iterdir()), [self.f])

    def test_failure_keeps_it_and_is_not_retried_until_the_pin_moves(self):
        calls = []

        def produce(rels):
            calls.append(rels)
            return []
        for _ in range(2):
            self.assertEqual(gen_misc.replace_stand_ins(self.tree, None,
                                                        produce), [])
        self.assertEqual(len(calls), 1)
        self.assertIn("STAND-IN", self.f.read_text())
        self.tree.chromium.rev = "pin2"
        gen_misc.replace_stand_ins(self.tree, None, produce)
        self.assertEqual(len(calls), 2)


class CopyActionTest(unittest.TestCase):
    def test_copy_target(self):
        tmp = Path(tempfile.mkdtemp())
        try:
            files = {"base/BUILD.gn": """
copy("build_date_header") {
  sources = [ "$root_build_dir/gen/base/internal.h" ]
  outputs = [ "$target_gen_dir/generated.h" ]
}
"""}
            interp = make_interp(files)
            tree = types.SimpleNamespace(tree_dir=tmp, materialize=None)
            (tmp / "out/cc/gen/base").mkdir(parents=True)
            (tmp / "out/cc/gen/base/internal.h").write_text("#define X 1\n")
            t = [x for x in interp.run_file("//base/BUILD.gn")
                 if x.name == "build_date_header"][0]
            self.assertIsNone(gen_actions.run_copy(tree, interp, t))
            self.assertEqual((tmp / "out/cc/gen/base/generated.h")
                             .read_text(), "#define X 1\n")
        finally:
            shutil.rmtree(tmp)


class ActionFreshnessTest(unittest.TestCase):
    """An action's outputs are made again when what it read changed."""

    BUILD = """
action("gen") {
  script = "gen.py"
  inputs = [ "in.txt" ]
  outputs = [ "$target_gen_dir/out.h" ]
  args = [ rebase_path("in.txt", root_build_dir),
           rebase_path("$target_gen_dir/out.h", root_build_dir) ]
}
"""
    SCRIPT = ("import sys\n"
              "open(sys.argv[2], 'w').write('// ' + open(sys.argv[1]).read())\n")

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        src = self.tmp / "src"
        (src / "a").mkdir(parents=True)
        (src / "a/BUILD.gn").write_text(self.BUILD)
        (src / "a/gen.py").write_text(self.SCRIPT)
        (src / "a/in.txt").write_text("one\n")
        files = {"a/BUILD.gn": self.BUILD}

        class Tree:
            tree_dir = src
            out_dir = src / "out/cc"
            gen_dir = src / "out/cc/gen"
            state = self.tmp / "state"
            platform = platforms.get("linux")
            chromium = types.SimpleNamespace(rev="pin")

            def local_path(self, p):
                return src / p

            def read(self, p):
                f = src / p
                return f.read_bytes() if f.exists() else None

            def exists(self, p):
                return (src / p).exists()

            def glob_upstream(self, prefix, suffix=""):
                d = src / prefix
                return sorted(str(f.relative_to(src)) for f in d.rglob("*")
                              if f.is_file() and str(f).endswith(suffix)) \
                    if d.is_dir() else []

            def materialize(self, paths):
                return 0

            def chromium_files(self, pred):
                return [p for p in files if pred(p)]

            def overlay_set(self):
                return set()

            def mount_of(self, p):
                return None

        self.tree = Tree()
        self.interp = gn.Interp(lambda p: files.get(p[2:]), {},
                                root=str(src))
        self.interp.globals.set("root_build_dir", gn.OUT)
        gen_actions._py_deps_done = True
        gen_actions._build_texts = None
        gen_actions._stamps.clear()
        self.out = self.tree.gen_dir / "a/out.h"

    def tearDown(self):
        gen_actions._py_deps_done = False
        gen_actions._stamps.clear()
        shutil.rmtree(self.tmp)

    def test_made_again_only_when_an_input_changed(self):
        w, f = gen_actions.produce(self.tree, self.interp, ["a/out.h"])
        self.assertEqual((w, f), (["a/out.h"], {}))
        self.assertEqual(self.out.read_text(), "// one\n")
        self.assertEqual(gen_actions.refresh(self.tree, self.interp),
                         ([], []))
        (self.tree.tree_dir / "a/in.txt").write_text("two\n")
        self.assertEqual(gen_actions.refresh(self.tree, self.interp),
                         (["a/out.h"], []))
        self.assertEqual(self.out.read_text(), "// two\n")
        # Its script changing counts too; a failing run leaves no output.
        (self.tree.tree_dir / "a/gen.py").write_text("raise SystemExit(1)\n")
        self.assertEqual(gen_actions.refresh(self.tree, self.interp),
                         ([], ["a/out.h"]))
        self.assertFalse(self.out.exists())
        # The stamps outlive the process.
        gen_actions._stamps.clear()
        self.assertEqual(gen_actions.refresh(self.tree, self.interp),
                         ([], []))


class BuildflagFreshnessTest(unittest.TestCase):
    """A buildflag header is written again when its flags changed."""

    WRITER = """import argparse, os, shlex
p = argparse.ArgumentParser()
for a in ("--output", "--rulename", "--gen-dir", "--definitions"):
    p.add_argument(a)
a = p.parse_args()
flags = shlex.split(open(a.definitions).read())[1:]
out = os.path.join(a.gen_dir, a.output)
open(out, "w").write("// Generated by build/write_buildflag_header.py\\n"
                     '// From "%s"\\n' % a.rulename +
                     "".join("#define %s\\n" % f for f in flags))
"""

    def build(self, value):
        return ('buildflag_header("buildflags") {\n'
                '  header = "buildflags.h"\n'
                '  flags = [ "ENABLE_X=%s" ]\n}\n' % value)

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        src = self.tmp / "src"
        (src / "build").mkdir(parents=True)
        (src / "build/write_buildflag_header.py").write_text(self.WRITER)
        (src / "a").mkdir()
        self.src = src
        self.files = {}
        self.set_build("true")
        files = self.files

        class Tree:
            state = self.tmp / "state"
            platform = platforms.get("linux")
            chromium = types.SimpleNamespace(rev="pin")

            def local_path(self, p):
                return src / p

            def read(self, p):
                f = src / p
                return f.read_bytes() if f.exists() else None

            def exists(self, p):
                return (src / p).exists()

            def materialize(self, paths):
                return 0

            def overlay_set(self):
                return {"a/BUILD.gn"}

        self.tree = Tree()
        self.gen = src / "out/cc/gen"
        self.gen.mkdir(parents=True)
        self.files = files

    def set_build(self, value):
        self.files["a/BUILD.gn"] = self.build(value)
        (self.src / "a/BUILD.gn").write_text(self.files["a/BUILD.gn"])

    def interp(self):
        files = dict(self.files)
        it = gn.Interp(lambda p: files.get(p[2:]), {}, root=str(self.src))
        it.exec_block(gn.Parser("template(\"buildflag_header\") {}")
                      .parse_file(), it.globals,
                      {"dir": "//build", "targets": None})
        return it

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_rewritten_when_the_flags_change(self):
        w, f = gen_buildflags.generate(self.tree, self.interp(), self.gen,
                                       ["a/buildflags.h"], [""])
        self.assertEqual((w, f), (["a/buildflags.h"], {}))
        h = self.gen / "a/buildflags.h"
        self.assertIn("ENABLE_X=true", h.read_text())
        self.assertEqual(gen_buildflags.refresh(self.tree, self.interp(),
                                                self.gen), ([], []))
        self.set_build("false")
        self.assertEqual(gen_buildflags.refresh(self.tree, self.interp(),
                                                self.gen),
                         (["a/buildflags.h"], []))
        self.assertIn("ENABLE_X=false", h.read_text())
        # A header made before stamps is found by its label and checked.
        (self.tree.state / gen_buildflags.STAMPS).unlink()
        self.assertEqual(gen_buildflags.refresh(self.tree, self.interp(),
                                                self.gen), ([], []))
        self.assertTrue((self.tree.state / gen_buildflags.STAMPS).exists())


class CleanTest(unittest.TestCase):
    def test_stale_outputs(self):
        tmp = Path(tempfile.mkdtemp())
        try:
            w = tmp / "work"
            for f in ("check/a__x.cc.log", "check/a__gone.cc.log",
                      "check/commands/a__x.cc.json",
                      "check/commands/a__gone.cc.json",
                      "check/cache/a__x.cc@win.json",
                      "check/cache/a__gone.cc.json",
                      "toolchain/protobuf_host/obj/x.o",
                      "toolchain/protobuf_host/libprotobuf_host.a",
                      "toolchain/protobuf_host/stamp",
                      "toolchain/flatc/obj/y.o",
                      "repos/perfetto/HEAD", "repos/oldclone/HEAD",
                      "repos/tarball-1.2.3.4/manifest.json",
                      "repos/tarball-1.2.3.3/manifest.json",
                      "chromium/src/out/cc/gen/a.h",
                      "chromium/src/out/cc/gen/a.pb.cc",
                      "chromium/src/out/cc/gen/m/x.mojom-module",
                      "chromium/src/out/cc/gen/b.h.standin",
                      "chromium/src/out/cc-old/gen/a.h",
                      "platform-old/scan.json", "graph.json"):
                (w / f).parent.mkdir(parents=True, exist_ok=True)
                (w / f).write_text("x")
            (w / "repos/sources.json").write_text(json.dumps({
                "third_party/perfetto": {
                    "key": "git https://github.com/google/perfetto c1"},
                "third_party/webrtc": {"key": "tarball 1.2.3.4 c2"}}))
            res = [{"target": "a/x.cc", "platform": "linux"},
                   {"target": "a/x.cc", "platform": "win"}]
            got = {str(p.relative_to(w)) for p, _ in
                   compile_check.stale_paths(w, res)}
            self.assertEqual(got, {
                "check/a__gone.cc.log", "check/commands/a__gone.cc.json",
                "check/cache/a__gone.cc.json", "toolchain/protobuf_host/obj",
                "repos/oldclone", "repos/tarball-1.2.3.3",
                "chromium/src/out/cc/gen/b.h.standin",
                "chromium/src/out/cc/gen/a.pb.cc",
                "chromium/src/out/cc/gen/m/x.mojom-module",
                "chromium/src/out/cc-old", "platform-old"})
            got = {str(p.relative_to(w)) for p, _ in
                   compile_check.stale_paths(w, res, generated=True)}
            self.assertIn("chromium/src/out/cc", got)
            self.assertIn("graph.json", got)
            self.assertNotIn("check/a__x.cc.log", got)
        finally:
            shutil.rmtree(tmp)


class HostShimTest(unittest.TestCase):
    """The //base stand-in of the HSTS generator behaves as //base does on
    what the generator gives it (host_shims/hsts_generator/shim_test.cc,
    built with the host's clang++)."""

    def test_hsts_generator_shim(self):
        cxx = shutil.which("clang++") or shutil.which("g++")
        if cxx is None:
            self.skipTest("no host C++ compiler")
        d = HERE / "host_shims" / "hsts_generator"
        tmp = Path(tempfile.mkdtemp())
        try:
            exe = tmp / "shim_test"
            r = subprocess.run([cxx, "-std=c++20", "-Wall", "-Werror",
                                "-I" + str(d), str(d / "shim_test.cc"), "-o",
                                str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-2000:])
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-2000:])
        finally:
            shutil.rmtree(tmp)


class LastChangeTest(unittest.TestCase):
    COMMIT = ("tree 1\nparent 2\nauthor a <a> 1792446655 -0700\n"
              "committer c <c> 1792446656 -0700\n\n"
              "Incrementing VERSION to 1.2.3.4\n\n"
              "Change-Id: I123\n"
              "Cr-Commit-Position: refs/branch-heads/8078@{#1314}\n"
              "Cr-Branched-From: abc-refs/heads/main@{#1706539}\n")

    def test_release_commit(self):
        self.assertEqual(compile_check.parse_lastchange("d0c0", self.COMMIT),
                         ("d0c0-refs/branch-heads/8078@{#1314}", 1792446656))

    def test_commit_lastchange_would_skip(self):
        self.assertIsNone(compile_check.parse_lastchange(
            "d0c0", self.COMMIT.replace("Change-Id: I123\n", "")))
        self.assertEqual(compile_check.parse_lastchange(
            "d0c0", self.COMMIT.replace(
                "Cr-Commit-Position: refs/branch-heads/8078@{#1314}\n", "")),
            ("d0c0", 1792446656))


class UnlistedTest(unittest.TestCase):
    def test_files_no_build_file_names(self):
        tree = FakeTree({
            "a/BUILD.gn": 'source_set("a") { sources = [ "ios/x.mm" ] }',
            "b/BUILD.gn": 'group("b") {}'})
        self.assertEqual(compile_check.unlisted(
            tree, ["a/ios/x.mm", "c/ios/Lonely.mm"]), ["c/ios/Lonely.mm"])


class SkippedReportTest(unittest.TestCase):
    def test_skipped_and_sdk_stand_ins(self):
        r = {"target": "x_android.cc", "skipped": "built for android only",
             "diags": []}
        self.assertEqual(compile_check.classify(r, {}, Path("/x")),
                         ("not checked", "built for android only", []))
        ok = {"target": "a.mm", "diags": []}
        st = compile_check.classify(ok, {}, Path("/x"), {},
                                    sdks.stand_in_label("mac"))
        self.assertEqual(st[0], "partly on stand-ins")
        self.assertIn("Apple framework", st[1])
        self.assertIsNone(sdks.stand_in_label("win"))

    def test_apple_declarations_the_stand_ins_lack(self):
        def res(platform, *msgs):
            return {"target": "a/x.mm", "platform": platform,
                    "diags": [{"kind": "error", "file": "a/x.mm", "line": 1,
                               "msg": m} for m in msgs]}
        r = res("mac", "use of undeclared identifier 'NSMenu'",
                "no type or protocol named 'NSTextInputClient'",
                "unknown type name 'NSApplication'",
                "no visible @interface for 'NSWindow' declares the selector "
                "'toggleFullScreen:'", "use of undeclared identifier 'x'")
        st = compile_check.classify(r, {}, Path("/x"))
        self.assertEqual(st[:2], ("not checked", "needs macOS SDK "
                                  "declarations mac_stand_ins/ lacks: "
                                  "NSMenu, NSTextInputClient, NSApplication, "
                                  "NSWindow"))
        # Elsewhere (and for other errors) they stay errors.
        st = compile_check.classify(res("linux", "use of undeclared "
                                        "identifier 'NSMenu'"), {},
                                    Path("/x"))
        self.assertEqual(st[0], "ERRORS")
        st = compile_check.classify(res("mac", "use of undeclared "
                                        "identifier 'kMolt'"), {},
                                    Path("/x"))
        self.assertEqual(st[0], "ERRORS")

    def test_plugin_diagnostics_in_moltbrowser_code(self):
        def res(path):
            return {"target": "a/x.cc", "platform": "linux", "diags": [
                {"kind": "warning", "file": path, "line": 3,
                 "msg": "[chromium-style] Complex constructor has an "
                        "inlined body."}]}
        # A src/ file (MoltBrowser's) ...
        st = compile_check.classify(res("net/socket/molt_identity_end.h"),
                                    {}, Path("/x"))
        self.assertEqual(st[:2], ("PLUGIN", "1 [chromium-style] "
                                  "diagnostic(s) in MoltBrowser code"))
        # ... not Chromium's own.
        st = compile_check.classify(res("base/not_in_the_overlay.h"), {},
                                    Path("/x"))
        self.assertEqual(st[0], "checked")

    def test_plugin_flags_per_platform(self):
        lin = config.plugin_flags("linux", "/r")
        self.assertIn("check-ipc", lin)
        self.assertNotIn("check-raw-ptr-fields", lin)
        self.assertNotIn("check-ipc", config.plugin_flags("mac", "/r"))
        win = config.plugin_flags("win", "/r")
        self.assertIn("check-raw-ptr-fields", win)
        self.assertIn("raw-ptr-exclude-path=/r/third_party/dawn/", win)
        self.assertIn("check-raw-ref-fields",
                      config.plugin_flags("linux", "/r", dev=True))
        self.assertEqual(win.count("-Xclang") * 2, len(win))

    def test_why_a_header_is_missing(self):
        def missing(name, platform, gn_target="//a:a"):
            return {"target": "a/x.cc", "platform": platform,
                    "gn_target": gn_target,
                    "diags": [{"kind": "fatal error", "file": "a/x.cc",
                               "line": 1,
                               "msg": "'%s' file not found" % name}]}
        fails = {"a/b.mojom": "mojom parser: bad enum"}
        st = compile_check.classify(missing("a/b.mojom-shared.h", "win"),
                                    {}, Path("/x"), failures=fails)
        self.assertEqual(st[:2], ("not checked", "header not available "
                                  "here: a/b.mojom-shared.h (mojom parser: "
                                  "bad enum)"))
        st = compile_check.classify(missing("Metal/Metal.h", "mac"), {},
                                    Path("/x"))
        self.assertIn("no stand-in for it", st[1])
        st = compile_check.classify(missing(
            "AuthenticationServices/AuthenticationServices.h", "mac"), {},
                                    Path("/x"))
        self.assertNotIn("no stand-in for it", st[1])
        st = compile_check.classify(missing("CoreText/CoreText.h", "mac"),
                                    {}, Path("/x"))
        self.assertEqual(st[1], "header not available here: "
                         "CoreText/CoreText.h")
        st = compile_check.classify(missing("v.hpp", "linux", None), {},
                                    Path("/x"))
        self.assertIn("no GN target lists the file with configs/linux-x64.gn",
                      st[1])


# A stand-in for clang: with -E it prints the source and the files it
# includes ("#include "x"" lines, from the source's directory); otherwise it
# counts the compile (in compiles.txt next to it) and warns about "WARN".
FAKE_CLANG = r"""#!/usr/bin/env python3
import os, re, sys
src = sys.argv[-1]
text = open(src).read()
if "-E" in sys.argv or "/E" in sys.argv:
    sys.stdout.write(text)
    for inc in re.findall(r'#include "([^"]+)"', text):
        p = os.path.join(os.path.dirname(src), inc)
        if not os.path.exists(p):
            sys.stderr.write("%s:1:10: fatal error: '%s' file not found\n"
                             % (src, inc))
            sys.exit(1)
        sys.stdout.write(open(p).read())
    sys.exit(int(os.environ.get("FAKE_PP_RC", "0")))
with open(os.path.join(os.path.dirname(__file__), "compiles.txt"), "a") as f:
    f.write(src + "\n")
if "CRASH" in text:
    sys.stderr.write("PLEASE submit a bug report\n")
    sys.exit(1)
if "WARN" in text:
    sys.stderr.write("%s:1:1: warning: a warning\n" % src)
"""


class ResultCacheTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.work = self.tmp / "work"
        bindir = self.work / "toolchain" / "clang" / "bin"
        bindir.mkdir(parents=True)
        (bindir.parent / "cr_build_revision").write_text("r1-1\n")
        for exe in ("clang++", "clang", "clang-cl"):
            (bindir / exe).write_text(FAKE_CLANG)
            (bindir / exe).chmod(0o755)
        self.compiles = bindir / "compiles.txt"
        self.tree = sources.SourceTree(self.work, self.tmp / "cache",
                                       overlay=self.tmp / "overlay",
                                       platform="linux")
        self.tree.system_dirs = []
        self.tree.out_dir.mkdir(parents=True)
        (self.work / "check").mkdir(parents=True)
        self.src = self.tree.tree_dir / "a" / "x.cc"
        self.src.parent.mkdir(parents=True)
        self.src.write_text('#include "x.h"\nint x;\n')
        (self.src.parent / "x.h").write_text("int y;\n")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_one(self, cache=True):
        return check.run_one(self.tree, self.work, "a/x.cc",
                             self.work / "check", plugins=True, cache=cache)

    def compiled(self):
        return len(self.compiles.read_text().splitlines()) \
            if self.compiles.exists() else 0

    def test_preprocess_command(self):
        cmd = ["clang++", "-fsyntax-only", "-DA", "-Xclang", "-add-plugin",
               "-Xclang", "find-bad-constructs", "-Xclang",
               "-plugin-arg-find-bad-constructs", "-Xclang", "check-ipc",
               "-Xclang", "-fno-x", "/src/a.cc"]
        self.assertEqual(result_cache.preprocess_command(cmd),
                         ["clang++", "-E", "-DA", "-Xclang", "-fno-x", "-dD",
                          "/src/a.cc"])
        self.assertEqual(result_cache.preprocess_command(
            ["clang-cl", "/Zs", "/W4", "a.cc"]),
            ["clang-cl", "/E", "/W4", "/clang:-dD", "a.cc"])

    def test_unchanged_input_reuses_the_result(self):
        self.src.write_text('#include "x.h"\nint x; // WARN\n')
        r1 = self.run_one()
        self.assertFalse(r1["reused"])
        self.assertEqual([d["kind"] for d in r1["diags"]], ["warning"])
        r2 = self.run_one()
        self.assertTrue(r2["reused"])
        self.assertEqual(r2["diags"], r1["diags"])
        self.assertEqual(self.compiled(), 1)
        # An included header changes: compiled again.
        (self.src.parent / "x.h").write_text("int y, z;\n")
        self.assertFalse(self.run_one()["reused"])
        self.assertEqual(self.compiled(), 2)
        self.assertTrue(self.run_one()["reused"])
        # So does the compiler, or the command (plugins off).
        (self.work / "toolchain/clang/cr_build_revision").write_text("r2-1")
        self.assertFalse(self.run_one()["reused"])
        r = check.run_one(self.tree, self.work, "a/x.cc",
                          self.work / "check", plugins=False)
        self.assertFalse(r["reused"])
        self.assertEqual(self.compiled(), 4)
        # --no-cache compiles, and forgets the entry.
        self.assertFalse(self.run_one(cache=False)["reused"])
        self.assertFalse(self.run_one()["reused"])
        self.assertEqual(self.compiled(), 6)

    def test_new_header_that_preprocessing_finds(self):
        self.src.write_text('#include "y.h"\nint x;\n')
        self.run_one()
        self.run_one()
        self.assertEqual(self.compiled(), 1)
        (self.src.parent / "y.h").write_text("int w;\n")
        self.assertFalse(self.run_one()["reused"])

    def test_crash_and_failed_preprocessing_are_not_stored(self):
        self.src.write_text("int x; // CRASH\n")
        self.run_one()
        self.assertFalse(result_cache.entry_path(self.work,
                                                 "a__x.cc").exists())
        self.assertFalse(self.run_one()["reused"])
        self.src.write_text("int x;\n")
        os.environ["FAKE_PP_RC"] = "2"
        try:
            self.run_one()
            self.assertFalse(self.run_one()["reused"])
        finally:
            del os.environ["FAKE_PP_RC"]
        self.assertEqual(self.compiled(), 4)

    def test_entry_of_another_key_is_not_used(self):
        result_cache.store(self.work, "n", "k1", 0, "log")
        self.assertEqual(result_cache.lookup(self.work, "n", "k1"),
                         (0, "log"))
        self.assertIsNone(result_cache.lookup(self.work, "n", "k2"))
        self.assertIsNone(result_cache.lookup(self.work, "n", None))
        result_cache.store(self.work, "n", "k1", -9, "killed")
        self.assertIsNone(result_cache.lookup(self.work, "n", "k1"))


if __name__ == "__main__":
    unittest.main()
