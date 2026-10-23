#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for the compile check's own logic, offline (no Chromium, no
network): the GN reader's set_defaults and `configs -=`, each file's
exception/RTTI flags from its GN target, mojom freshness, the tarball
source, grit header freshness, where subprojects came from, and the
report's stand-in labels.

Run: python3 tools/compile_check/compile_check_test.py -v
"""
import hashlib
import io
import json
import lzma
import os
import shutil
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import compile_check  # noqa: E402
from ccheck import check, gen_actions, gen_misc, gen_mojom, gn  # noqa: E402
from ccheck import gnconfigs, sources, tarball  # noqa: E402

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
                            opener=lambda url: io.BytesIO(blob))
            man = json.loads((dest / tarball.MANIFEST).read_text())
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


if __name__ == "__main__":
    unittest.main()
