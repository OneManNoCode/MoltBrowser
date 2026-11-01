#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for gn_static_check.py, offline: label and pattern matching, gn's
include scanner, and a small GN tree with known-good and known-bad cases
for the include rule (direct and public dependency chains, private headers,
friend, allow_circular_includes_from, check_includes, no_check_targets,
nogncheck, unlisted headers, files a configuration never loads, platform
conditions, mojom() headers), gn gen's dependency rules (undefined labels,
visibility, testonly, the circular-include allowlist), cycles, missing
sources, the overlay's "only the includes MoltBrowser added" rule, the
plumbing of the checkdeps and Blink-audit runs (with stand-in scripts),
and the `tests` and `flags` checks (orphan tests and fuzzers, test data,
the source scanners, chrome://flags entries and their metadata).

Run: python3 tools/chromium/gn_static_check_test.py -v
"""
import hashlib
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import gn_static_check as g  # noqa: E402

DOT_GN = """
buildconfig = "//build/config/BUILDCONFIG.gn"
no_check_targets = [ "//skipped:*" ]
allow_circular_includes_from_allowlist = [ "//app:app" ]
"""

BUILDCONFIG = """
is_linux = current_os == "linux"
is_mac = current_os == "mac"
is_win = current_os == "win"
is_chromeos = false
is_android = false
is_ios = false
declare_args() {
  is_component_build = false
  molt_test_switch = false
}
template("component") {
  if (is_component_build) {
    _type = "shared_library"
  } else {
    _type = "static_library"
  }
  target(_type, target_name) {
    forward_variables_from(invoker, "*")
  }
}
"""

ROOT_BUILD = """
group("all") {
  deps = [
    "//app",
    "//base",
    "//lib",
    "//mojo_user",
    "//skipped",
    "//unloaded_user",
  ]
  if (is_win) {
    deps += [ "//winonly" ]
  }
}
"""

FILES = {
    ".gn": DOT_GN,
    "build/config/BUILDCONFIG.gn": BUILDCONFIG,
    "BUILD.gn": ROOT_BUILD,
    "base/BUILD.gn": """
component("base") {
  sources = [ "base.h", "base.cc" ]
}
""",
    "base/base.h": "",
    "base/base.cc": "",
    # lib: a public header, a private header, a friend and a public chain.
    "lib/BUILD.gn": """
source_set("lib") {
  public = [ "lib.h" ]
  sources = [ "lib.cc", "lib_internal.h" ]
  friend = [ "//app:friendly" ]
  public_deps = [ ":exported" ]
  deps = [ ":hidden" ]
}
source_set("exported") {
  sources = [ "exported.h" ]
}
source_set("hidden") {
  sources = [ "hidden.h" ]
}
source_set("testing_only") {
  testonly = true
  sources = [ "testing_only.h" ]
}
source_set("restricted") {
  visibility = [ "//lib:*" ]
  sources = [ "restricted.h" ]
}
""",
    "lib/lib.h": "",
    "lib/lib.cc": "",
    "lib/lib_internal.h": "",
    "lib/exported.h": "",
    "lib/hidden.h": "",
    "lib/testing_only.h": "",
    "lib/restricted.h": "",
    "app/BUILD.gn": """
source_set("app") {
  sources = [ "app.cc", "circular.h" ]
  deps = [ "//lib", ":circular_user" ]
  allow_circular_includes_from = [ ":circular_user" ]
}
source_set("friendly") {
  sources = [ "friendly.cc" ]
  deps = [ "//lib" ]
}
source_set("circular_user") {
  sources = [ "circular_user.cc" ]
}
source_set("unchecked") {
  check_includes = false
  sources = [ "unchecked.cc" ]
}
""",
    "app/app.cc": "",
    "app/circular.h": "",
    "app/friendly.cc": "",
    "app/circular_user.cc": "",
    "app/unchecked.cc": "",
    "skipped/BUILD.gn": """
source_set("skipped") {
  sources = [ "skipped.cc" ]
}
""",
    "skipped/skipped.cc": "",
    # A directory this configuration never loads (as a ChromeOS-only
    # target on Linux): its headers are unknown to gn check.
    "unloaded/BUILD.gn": """
assert(is_chromeos)
source_set("unloaded") {
  sources = [ "unloaded.h" ]
}
""",
    "unloaded/unloaded.h": "",
    "unloaded_user/BUILD.gn": """
source_set("unloaded_user") {
  sources = [ "user.cc" ]
}
""",
    "unloaded_user/user.cc": "",
    "winonly/BUILD.gn": """
source_set("winonly") {
  sources = [ "winonly.h" ]
}
""",
    "winonly/winonly.h": "",
    "mojo_user/BUILD.gn": """
mojom("mojom") {
  sources = [ "thing.mojom" ]
}
source_set("mojo_user") {
  sources = [ "good.cc" ]
  deps = [ ":mojom" ]
}
source_set("mojo_blink_user") {
  sources = [ "bad.cc" ]
  deps = [ ":mojom" ]
}
""",
    "mojo_user/good.cc": "",
    "mojo_user/bad.cc": "",
}


def write_tree(root, files):
    for rel, text in files.items():
        p = Path(root) / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="gn-static-check-test-"))
        self.root = self.tmp / "tree"
        self.overlay = self.tmp / "src"
        write_tree(self.root, FILES)
        self.overlay.mkdir()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def graph(self, platform="linux", overlay=None):
        tree = g.Tree(self.root, overlay)
        return g.Graph(tree, platform), g.dotfile_settings(tree)

    def includes(self, rel, text, platform="linux", only=None):
        gr, dot = self.graph(platform)
        f = g.Findings()
        listed = g.check_file_includes(gr, rel, text, only, dot, f)
        return listed, [x.message for x in f.errors()]


class LabelTest(unittest.TestCase):
    def test_norm_label(self):
        self.assertEqual(g.norm_label(":a", "//x/y"), "//x/y:a")
        self.assertEqual(g.norm_label("//x/y", "//z"), "//x/y:y")
        self.assertEqual(g.norm_label("//x/y:", "//z"), "//x/y:y")
        self.assertEqual(g.norm_label("../w:v", "//x/y"), "//x/w:v")
        self.assertEqual(g.norm_label("sub", "//x"), "//x/sub:sub")
        self.assertEqual(g.norm_label("//x:a(//build/toolchain:host)", "//"),
                         "//x:a")
        self.assertIsNone(g.norm_label("<UNKNOWN>", "//x"))

    def test_patterns(self):
        m = g.label_matches
        self.assertTrue(m("*", "//a:b"))
        self.assertTrue(m(g.norm_pattern("//a/*", "//"), "//a/b/c:d"))
        self.assertTrue(m(g.norm_pattern("//a/*", "//"), "//a:x"))
        self.assertFalse(m(g.norm_pattern("//a/*", "//"), "//ab:x"))
        self.assertTrue(m(g.norm_pattern(":*", "//a"), "//a:z"))
        self.assertFalse(m(g.norm_pattern(":*", "//a"), "//a/b:z"))
        self.assertTrue(m(g.norm_pattern(":y", "//a"), "//a:y"))
        self.assertFalse(m(g.norm_pattern(":y", "//a"), "//a:z"))
        self.assertTrue(m(g.norm_pattern("//a", "//"), "//a:a"))

    def test_config_specs(self):
        self.assertEqual(g.parse_config("linux"), ("linux", {}))
        self.assertEqual(g.parse_config("win+vulkan"),
                         ("win", {"molt_llama_vulkan": True}))
        p, extra = g.parse_config("linux+component")
        self.assertTrue(extra["is_component_build"])
        self.assertFalse(extra["is_official_build"])
        with self.assertRaises(ValueError):
            g.parse_config("plan9")
        with self.assertRaises(ValueError):
            g.parse_config("linux+turbo")


class ScanTest(unittest.TestCase):
    def test_quoted_system_import_nogncheck(self):
        text = "\n".join([
            "// Copyright",
            "#include \"a/b.h\"",
            "#include <vector>",
            "#import \"c/d.h\"",
            "#include \"e/f.h\"  // nogncheck",
            "  #  include \"g/h.h\"",
        ])
        got = [(inc, sys_) for _, inc, sys_ in g.scan_includes(text)]
        self.assertEqual(got, [("a/b.h", False), ("vector", True),
                               ("c/d.h", False), ("g/h.h", False)])

    def test_stops_after_ten_code_lines(self):
        code = ["int x%d;" % i for i in range(11)]
        text = "\n".join(["#include \"first.h\""] + code +
                         ["#include \"late.h\""])
        self.assertEqual([i for _, i, _ in g.scan_includes(text)],
                         ["first.h"])
        # Comments, blank lines and directives don't count.
        text = "\n".join(["#include \"first.h\""] + code[:10] +
                         ["", "// note", "#if 1", "#include \"in_time.h\""])
        self.assertEqual([i for _, i, _ in g.scan_includes(text)],
                         ["first.h", "in_time.h"])

    def test_all_includes_has_no_limit(self):
        code = ["int x%d;" % i for i in range(30)]
        text = "\n".join(["#include \"first.h\""] + code +
                         ["#include \"late.h\"", "#include <set>"])
        self.assertEqual(g.all_includes(text), ["first.h", "late.h"])


class IncludeRuleTest(Base):
    def test_direct_and_public_chain(self):
        listed, errs = self.includes("app/app.cc", "\n".join([
            '#include "lib/lib.h"',          # direct dep, public header
            '#include "lib/exported.h"',     # lib's public_deps
            '#include "base/base.h"',        # in no chain: error
        ]))
        self.assertTrue(listed)
        self.assertEqual(len(errs), 1)
        self.assertIn('"base/base.h"', errs[0])
        self.assertIn("without a dependency path", errs[0])

    def test_private_dep_two_levels_down(self):
        _, errs = self.includes("app/app.cc", '#include "lib/hidden.h"')
        self.assertEqual(len(errs), 1)
        self.assertIn("lib/hidden.h", errs[0])

    def test_private_header_and_friend(self):
        _, errs = self.includes("app/app.cc",
                                '#include "lib/lib_internal.h"')
        self.assertEqual(len(errs), 1)
        self.assertIn("private to //lib:lib", errs[0])
        _, errs = self.includes("app/friendly.cc",
                                '#include "lib/lib_internal.h"')
        self.assertEqual(errs, [])

    def test_allow_circular_includes_from(self):
        _, errs = self.includes("app/circular_user.cc",
                                '#include "app/circular.h"')
        self.assertEqual(errs, [])

    def test_skips(self):
        # check_includes = false, //.gn's no_check_targets, nogncheck, a
        # header no target lists, a file in no target.
        self.assertEqual(self.includes(
            "app/unchecked.cc", '#include "base/base.h"')[1], [])
        self.assertEqual(self.includes(
            "skipped/skipped.cc", '#include "base/base.h"')[1], [])
        self.assertEqual(self.includes(
            "app/app.cc", '#include "base/base.h"  // nogncheck')[1], [])
        self.assertEqual(self.includes(
            "app/app.cc", '#include "lib/not_listed.h"')[1], [])
        listed, errs = self.includes("app/orphan.cc", '#include "x.h"')
        self.assertFalse(listed)

    def test_file_relative_include(self):
        _, errs = self.includes("lib/lib.cc", '#include "hidden.h"')
        self.assertEqual(errs, [])        # lib depends on :hidden

    def test_unloaded_file_is_unknown(self):
        _, errs = self.includes("unloaded_user/user.cc",
                                '#include "unloaded/unloaded.h"')
        self.assertEqual(errs, [])
        gr, _ = self.graph()
        gr.load_build()
        self.assertNotIn("//unloaded/BUILD.gn", gr.loaded)
        # Read anyway, its false assert keeps it out of the build.
        self.assertEqual(gr.load_dir("//unloaded"), {})
        self.assertIn("//unloaded/BUILD.gn", gr.asserts)

    def test_platform_condition(self):
        text = '#include "winonly/winonly.h"'
        self.assertEqual(self.includes("app/app.cc", text, "linux")[1], [])
        errs = self.includes("app/app.cc", text, "win")[1]
        self.assertEqual(len(errs), 1)

    def test_only_added_includes(self):
        text = '#include "base/base.h"\n#include "lib/lib.h"'
        _, errs = self.includes("app/app.cc", text, only={"lib/lib.h"})
        self.assertEqual(errs, [])

    def test_mojom_headers(self):
        _, errs = self.includes(
            "mojo_user/good.cc",
            '#include "mojo_user/thing.mojom.h"\n'
            '#include "mojo_user/thing.mojom-shared.h"\n'
            '#include "mojo_user/thing.mojom-forward.h"')
        self.assertEqual(errs, [])
        _, errs = self.includes("mojo_user/bad.cc",
                                '#include "mojo_user/thing.mojom-blink.h"')
        self.assertEqual(len(errs), 1)
        self.assertIn("mojom_blink", errs[0])


class DepsRuleTest(Base):
    def check(self, build_gn, label):
        write_tree(self.root, {"extra/BUILD.gn": build_gn})
        gr, dot = self.graph()
        t = gr.target(label)
        f = g.Findings()
        g.check_target_deps(gr, t, t.all_deps(), t.allow_circular, dot, f)
        return [x.message for x in f.errors()]

    def test_good(self):
        self.assertEqual(self.check("""
source_set("extra") {
  deps = [ "//lib", "//lib:exported" ]
}
""", "//extra:extra"), [])

    def test_undefined_visibility_testonly(self):
        errs = self.check("""
source_set("extra") {
  deps = [ "//lib:nope", "//lib:restricted", "//lib:testing_only" ]
}
""", "//extra:extra")
        self.assertEqual(len(errs), 3, errs)
        self.assertIn("not defined", errs[0])
        self.assertIn("visibility", errs[1])
        self.assertIn("testonly", errs[2])

    def test_testonly_may_depend_on_testonly(self):
        self.assertEqual(self.check("""
source_set("extra") {
  testonly = true
  deps = [ "//lib:testing_only" ]
}
""", "//extra:extra"), [])

    def test_file_level_visibility_and_testonly(self):
        errs = self.check("""
source_set("open") {
}
visibility = [ ":*" ]
testonly = true
source_set("closed") {
}
source_set("extra") {
  testonly = false
  deps = [ ":open", ":closed" ]
}
""", "//extra:extra")
        # :closed is testonly (file level, after the assignment); both are
        # visible to //extra:extra (same file).
        self.assertEqual(len(errs), 1, errs)
        self.assertIn("//extra:closed", errs[0])
        gr, _ = self.graph()
        self.assertIsNone(gr.target("//extra:open").visibility)
        self.assertEqual(gr.target("//extra:closed").visibility,
                         ["//extra:*"])

    def test_circular_allowlist(self):
        errs = self.check("""
source_set("extra") {
  deps = [ "//lib" ]
  allow_circular_includes_from = [ "//lib", "//base" ]
}
""", "//extra:extra")
        self.assertEqual(len(errs), 2, errs)
        self.assertIn("//base:base, which is not one of its deps", errs[0])
        self.assertIn("allowlist", errs[1])


class GraphTest(Base):
    def test_cycles(self):
        write_tree(self.root, {"cyc/BUILD.gn": """
source_set("a") { deps = [ ":b" ] }
source_set("b") { public_deps = [ ":c" ] }
source_set("c") { deps = [ ":a" ] }
source_set("d") { deps = [ ":a" ] }
""", "BUILD.gn": ROOT_BUILD.replace('"//app",', '"//app", "//cyc:d",')})
        gr, _ = self.graph()
        cycles = gr.cycles()
        self.assertEqual(cycles, [["//cyc:a", "//cyc:b", "//cyc:c"]])
        self.assertEqual(g.cycle_path(gr, cycles[0]),
                         ["//cyc:a", "//cyc:b", "//cyc:c", "//cyc:a"])

    def test_variant_args(self):
        write_tree(self.root, {"var/BUILD.gn": """
if (is_component_build) {
  source_set("component_only") {}
}
if (molt_test_switch) {
  source_set("switch_only") {}
}
"""})
        gr, _ = self.graph()
        self.assertIsNone(gr.target("//var:component_only"))
        gr = g.Graph(g.Tree(self.root), "linux+component")
        self.assertIsNotNone(gr.target("//var:component_only"))
        gr = g.Graph(g.Tree(self.root), "linux",
                     extra_args={"molt_test_switch": True})
        self.assertIsNotNone(gr.target("//var:switch_only"))
        self.assertEqual(gr.target("//base:base").type, "component")

    def test_filter_labels(self):
        # WebRTC's rtc_library() sorts its deps with these: a target's deps
        # must stay known through them.
        write_tree(self.root, {"lab/BUILD.gn": """
d = [ ":x", "//third_party/abseil-cpp/absl/base:core", "../base" ]
a = filter_labels_include(d, [ "//third_party/abseil-cpp/*" ])
source_set("x") {}
source_set("y") {
  deps = filter_labels_exclude(d, [ "//third_party/abseil-cpp/*" ])
  if (a != []) {
    deps += [ "//third_party/abseil-cpp:absl" ]
  }
}
"""})
        gr, _ = self.graph()
        self.assertEqual(gr.target("//lab:y").deps,
                         ["//lab:x", "//base:base",
                          "//third_party/abseil-cpp:absl"])


class RunTest(Base):
    """run() over an overlay: a first-party file, a changed upstream file
    and an overlay BUILD.gn."""

    def test_run(self):
        # Upstream copy of app.cc includes base.h (allowed upstream by
        # construction here: gn check would flag it, but it is not
        # MoltBrowser's line).
        (self.root / "app" / "app.cc").write_text(
            '#include "base/base.h"\n')
        ov = {
            "app/app.cc": '#include "base/base.h"\n#include "lib/lib.h"\n'
                          '#include "lib/hidden.h"\n',
            "molt/BUILD.gn": """
source_set("molt") {
  sources = [ "molt.cc", "molt.h", "missing.cc" ]
  deps = [ "//lib:restricted" ]
}
""",
            "molt/molt.cc": '#include "molt/molt.h"\n#include "base/base.h"\n',
            "molt/molt.h": "",
            "molt/unlisted.h": "",
        }
        write_tree(self.overlay, ov)
        write_tree(self.root, {"BUILD.gn": ROOT_BUILD.replace(
            '"//app",', '"//app", "//molt",')})
        tree = g.Tree(self.root, self.overlay)
        overlay = g.Overlay(self.overlay, sorted(ov), tree)
        findings, stats = g.run(tree, overlay, ["linux", "mac", "win"],
                                upstream_tree=g.Tree(self.root),
                                progress=None)
        errs = sorted((f.kind, f.where, f.message) for f in
                      findings.errors())
        kinds = [e[0] for e in errs]
        # app.cc: only its added line that breaks the rule (hidden.h).
        self.assertIn(("includes", "app/app.cc:3"),
                      [(e[0], e[1]) for e in errs])
        self.assertNotIn(("includes", "app/app.cc:1"),
                         [(e[0], e[1]) for e in errs])
        # molt.cc: base.h without a dep.
        self.assertIn(("includes", "molt/molt.cc:2"),
                      [(e[0], e[1]) for e in errs])
        # molt/BUILD.gn: a source that doesn't exist, a dep it can't see.
        self.assertIn("sources", kinds)
        self.assertIn("deps", kinds)
        warns = [f for f in findings.all() if f.severity == "warning"]
        self.assertEqual([(w.kind, w.where) for w in warns],
                         [("unlisted", "molt/unlisted.h")])
        # app.cc (its added includes), molt.cc, molt.h, unlisted.h.
        self.assertEqual(stats["include_files"], 4)


FAKE_CHECKDEPS = r'''
import json, os, re, sys
root = sys.argv[sys.argv.index("--root") + 1]
out = sys.argv[sys.argv.index("--json") + 1]
assert os.path.isdir(os.path.join(root, ".svn"))
# Allowed: the root DEPS's "+dir" rules and the file's own directory (as
# checkdeps allows it), both relative to --root.
top = os.path.join(root, "DEPS")
allowed = re.findall(r'"\+([^"]+)"', open(top).read()) if os.path.isfile(
    top) else []
res = []
for dirpath, dirnames, files in sorted(os.walk(root)):
    dirnames[:] = [d for d in dirnames if d != ".svn"]
    own = os.path.relpath(dirpath, root).replace(os.sep, "/")
    for name in sorted(files):
        if not name.endswith(".cc"):
            continue
        path = os.path.join(dirpath, name)
        incs = [l.split('"')[1] for l in open(path)
                if l.startswith("#include")]
        res.append({"dependee_path": path, "violations": [
            {"include_path": i, "violated_rule": ["-", "x", "y"]}
            for i in incs if not i.startswith(own + "/") and
            not any(i.startswith(a + "/") for a in allowed)]})
json.dump(res, open(out, "w"))
'''

FAKE_AUDIT = r'''
import collections
_Result = collections.namedtuple("_Result", "line identifier advice warning")
def check(path, changed):
    return [_Result(n, "std::regex", ["no"], False)
            for n, l in changed if "std::regex" in l]
'''


class PresubmitTest(Base):
    """checkdeps and the Blink audit see what MoltBrowser adds."""

    def overlay_with(self, ov):
        write_tree(self.overlay, ov)
        tree = g.Tree(self.root, self.overlay)
        return g.Overlay(self.overlay, sorted(ov), tree)

    def test_checkdeps_plan_and_run(self):
        (self.root / "app" / "app.cc").write_text(
            '#include "base/base.h"\nint a;\n')
        write_tree(self.root, {"buildtools/checkdeps/checkdeps.py":
                               FAKE_CHECKDEPS, "DEPS": "include_rules=[]\n"})
        ov = self.overlay_with({
            "app/app.cc": '#include "base/base.h"\n#include "lib/lib.h"\n'
                          'int a;\n',
            "app/DEPS": 'include_rules = [ "+lib" ]\n',
            "molt/molt.cc": '#include "base/base.h"\n',
            "v8/molt.cc": '#include "base/base.h"\n',
            "molt/molt.txt": "",
        })
        self.assertEqual(g.checkdeps_plan(ov),
                         {"app/app.cc": {"lib/lib.h"}, "molt/molt.cc": None})
        findings, n = g.run_checkdeps(self.root, ov, self.tmp / "work")
        self.assertEqual(n, 2)
        self.assertEqual(sorted((f.where, f.message.split('"')[1])
                                for f in findings.errors()),
                         [("app/app.cc", "lib/lib.h"),
                          ("molt/molt.cc", "base/base.h")])
        work = self.tmp / "work" / "src"
        self.assertTrue((work / "app" / "DEPS").is_file())
        self.assertTrue((work / "DEPS").is_file())

    def test_checkdeps_subproject_root(self):
        # WebRTC's files include from WebRTC's root ("modules/...",
        # "rtc_base/..."): they are checked in a run rooted there.
        webrtc = "third_party/webrtc/"
        write_tree(self.root, {"buildtools/checkdeps/checkdeps.py":
                               FAKE_CHECKDEPS, "DEPS": "include_rules=[]\n",
                               webrtc + "DEPS":
                               'include_rules = [ "+rtc_base" ]\n'})
        ov = self.overlay_with({
            webrtc + "modules/ap/a.cc": '#include "modules/ap/a.h"\n'
                                        '#include "rtc_base/r.h"\n'
                                        '#include "third_party/x/x.h"\n',
            "molt/molt.cc": '#include "base/base.h"\n',
        })
        findings, n = g.run_checkdeps(self.root, ov, self.tmp / "work")
        self.assertEqual(n, 2)
        self.assertEqual(sorted((f.where, f.message.split('"')[1])
                                for f in findings.errors()),
                         [("molt/molt.cc", "base/base.h"),
                          (webrtc + "modules/ap/a.cc", "third_party/x/x.h")])
        work = self.tmp / "work"
        self.assertIn("+rtc_base", (work / "third_party_webrtc" / "src" /
                                    "DEPS").read_text())
        self.assertTrue((work / "third_party_webrtc" / "src" / "modules" /
                         "ap" / "a.cc").is_file())
        self.assertFalse((work / "src" / "third_party" / "webrtc").exists())

    def test_added_lines_and_blink_audit(self):
        blink = "third_party/blink/renderer/core/"
        (self.root / blink).mkdir(parents=True)
        (self.root / blink / "a.cc").write_text("int a;\nint b;\n")
        ov = self.overlay_with({
            blink + "a.cc": "int a;\nstd::regex r;\nint b;\n",
            blink + "new.h": "std::regex s;\n",
            g.BLINK_AUDIT: FAKE_AUDIT,
        })
        self.assertEqual(g.added_lines(ov, blink + "a.cc"),
                         [(2, "std::regex r;")])
        findings, n = g.run_blink_audit(ov)
        self.assertEqual(n, 2)
        self.assertEqual(sorted(f.where for f in findings.errors()),
                         [blink + "a.cc:2", blink + "new.h:1"])


TESTS_ROOT_BUILD = """
group("all") {
  testonly = true
  deps = [
    "//chrome/browser/molt_ai/test:molt_ai_unittests",
    "//chrome/test:browser_tests",
    "//chrome/browser/molt_ai/fuzzers:molt_fuzzers",
    "//other:other_unittests",
    "//molt",
  ]
}
"""

# A small tree with the test binaries `tests` knows, for the orphan and
# test-data checks.
TESTS_TREE = {
    ".gn": DOT_GN,
    "build/config/BUILDCONFIG.gn": BUILDCONFIG,
    "BUILD.gn": TESTS_ROOT_BUILD,
    "chrome/browser/molt_ai/test/BUILD.gn": """
test("molt_ai_unittests") {
  deps = [ "//molt:unit_tests" ]
}
""",
    "chrome/test/BUILD.gn": """
test("browser_tests") {
  deps = [ "//molt:browser_tests" ]
  data = [ "data/" ]
}
""",
    "chrome/browser/molt_ai/fuzzers/BUILD.gn": """
group("molt_fuzzers") {
  testonly = true
  deps = [ "//molt:reached_fuzzer" ]
}
""",
    "other/BUILD.gn": """
test("other_unittests") {
  deps = [ "//molt:elsewhere_tests" ]
}
""",
    "chrome/test/data/media/clip.webm": "",
}

TESTS_OVERLAY = {
    "molt/BUILD.gn": """
source_set("molt") {
  sources = [ "molt.cc" ]
}
source_set("unit_tests") {
  testonly = true
  sources = [ "listed_unittest.cc", "data_unittest.cc" ]
  data = [ "//chrome/test/data/molt_x/" ]
  data_deps = [ ":fake_helper" ]
  deps = [ ":static_tests" ]
}
static_library("static_tests") {
  testonly = true
  sources = [ "static_unittest.cc" ]
}
source_set("unreached_tests") {
  testonly = true
  sources = [ "unreached_unittest.cc" ]
}
source_set("elsewhere_tests") {
  testonly = true
  sources = [ "elsewhere_unittest.cc" ]
}
source_set("browser_tests") {
  testonly = true
  sources = [ "page_browsertest.cc" ]
}
executable("fake_helper") {
  sources = [ "fake_helper.cc" ]
}
fuzzer_test("reached_fuzzer") {
  sources = [ "reached_fuzzer.cc" ]
}
fuzzer_test("lost_fuzzer") {
  sources = [ "lost_fuzzer.cc" ]
}
""",
    "molt/molt.cc": "",
    "molt/fake_helper.cc": "int main() {}\n",
    "molt/listed_unittest.cc": "TEST(A, B) {}\n",
    "molt/orphan_unittest.cc": "TEST(A, C) {}\n",
    "molt/unreached_unittest.cc": "TEST(A, D) {}\n",
    "molt/elsewhere_unittest.cc": "TEST(A, E) {}\n",
    "molt/static_unittest.cc": "TEST(A, F) {}\n",
    "molt/reached_fuzzer.cc": "int LLVMFuzzerTestOneInput() {}\n",
    "molt/lost_fuzzer.cc": "int LLVMFuzzerTestOneInput() {}\n",
    "molt/data_unittest.cc": "\n".join([
        "TEST(D, Files) {",
        "  base::FilePath root, exe;",
        "  base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root);",
        "  base::PathService::Get(base::DIR_EXE, &exe);",
        '  Read(root.AppendASCII("chrome").AppendASCII("test")',
        '           .AppendASCII("data").AppendASCII("molt_x")',
        '           .AppendASCII(name));',
        '  Load("page.html");',            # in molt_x/: fine
        '  Load("gone.html");',            # line 9: in no directory
        '  Run(exe.AppendASCII("fake_helper"));',
        '  Run(exe.AppendASCII("absent_helper"));',     # line 11
        '#if BUILDFLAG(IS_WIN)',
        '  Run(exe.AppendASCII("absent_helper.exe"));',  # Windows only
        '#endif',
        '  Read(root.AppendASCII("chrome/test/data/media/clip.webm"));',
        '  Read(root.AppendASCII("chrome/test/data/other/x.txt"));',  # 16
        "}",
    ]),
    "molt/page_browsertest.cc": "\n".join([
        "IN_PROC_BROWSER_TEST_F(P, Open) {",
        '  Navigate(GetURL("a.test", "/molt_x/page.html?q=1"));',
        '  Navigate(GetURL("a.test", "/molt_x/missing.html"));',   # 3
        '  late_ = std::make_unique<ControllableHttpResponse>(',
        '      embedded_test_server(), "/molt_x/late.html");',     # handled
        '  EXPECT_EQ(path, "/molt_x/late.html");',
        '  Write("/tmp/scratch.bin");',                           # not served
        "}",
    ]),
    "chrome/test/data/molt_x/page.html":
        '<video src="/media/clip.webm"></video>\n'
        '<img src="/media/missing.png">\n'                        # line 2
        '<script>const u = new URL(location.href);</script>\n',
}


class TestsCheckTest(unittest.TestCase):
    """`tests`: orphans, binaries, fuzzers, and the data tests open."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="gn-static-check-tests-"))
        self.root = self.tmp / "tree"
        self.src = self.tmp / "src"
        write_tree(self.root, TESTS_TREE)
        write_tree(self.src, TESTS_OVERLAY)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_check(self, platforms=("linux",), pinned=None):
        tree = g.Tree(self.root, self.src)
        ov = g.Overlay(self.src, sorted(TESTS_OVERLAY), tree)
        if pinned is None:
            pinned = {p for p in TESTS_TREE if not p.endswith(".gn")}
        listing = g.SourceListing(TESTS_OVERLAY, pinned)
        findings, stats = g.run_tests(tree, ov, list(platforms), listing,
                                      progress=None)
        return findings, stats

    def by_where(self, findings, kind, severity="error"):
        return {f.where: f.message for f in findings.all()
                if f.kind == kind and f.severity == severity}

    def test_kinds(self):
        self.assertEqual(g.test_kind("a/x_unittest.cc", ""), "unit")
        self.assertEqual(g.test_kind("a/x_test.cc", "TEST_F(A, B) {}"),
                         "unit")
        self.assertEqual(g.test_kind("a/x_test.cc", "int x;"), None)
        self.assertEqual(g.test_kind("a/x_browsertest.cc", ""), "browser")
        self.assertEqual(g.test_kind("a/x.cc", "IN_PROC_BROWSER_TEST_F(A, B)"),
                         "browser")
        self.assertEqual(g.test_kind("a/x_interactive_uitest.cc", ""),
                         "interactive")
        self.assertEqual(g.test_kind("a/x_fuzzer.cc", ""), "fuzzer")

    def test_orphans(self):
        findings, stats = self.run_check()
        errs = self.by_where(findings, "tests")
        self.assertIn("no target", errs["molt/orphan_unittest.cc"])
        self.assertIn("//molt:unreached_tests (linux), which no test binary "
                      "reaches", errs["molt/unreached_unittest.cc"])
        self.assertIn("which only //other:other_unittests builds",
                      errs["molt/elsewhere_unittest.cc"])
        self.assertIn("molt_fuzzers doesn't reach", errs["//molt:lost_fuzzer"])
        # Listed and reached: no finding. A fuzzer source is reported
        # through its target only.
        for ok in ("molt/listed_unittest.cc", "molt/page_browsertest.cc",
                   "molt/reached_fuzzer.cc", "molt/lost_fuzzer.cc",
                   "//molt:reached_fuzzer"):
            self.assertNotIn(ok, errs)
        warns = self.by_where(findings, "tests", "warning")
        self.assertIn("static library //molt:static_tests",
                      warns["molt/static_unittest.cc"])
        self.assertEqual(stats["kinds"], {"browser": 1, "fuzzer": 2,
                                          "unit": 6})

    def test_data(self):
        findings, _ = self.run_check(("linux", "win"))
        errs = self.by_where(findings, "testdata")
        self.assertIn("names gone.html, which is in none",
                      errs["molt/data_unittest.cc:9"])
        self.assertIn("runs absent_helper", errs["molt/data_unittest.cc:11"])
        self.assertNotIn("molt/data_unittest.cc:10", errs)   # data_deps
        # The Windows-only line is read on Windows only.
        win_only = [f for f in findings.all()
                    if f.where == "molt/data_unittest.cc:13"]
        self.assertEqual([f.platforms for f in win_only], [["win"]])
        # In the tree, but not in molt_ai_unittests' runtime data.
        self.assertIn("isn't in the runtime data of //chrome/browser/"
                      "molt_ai/test:molt_ai_unittests",
                      errs["molt/data_unittest.cc:15"])
        self.assertIn("neither in src/ nor in Chromium",
                      errs["molt/data_unittest.cc:16"])
        # Served by browser_tests (data/ = //chrome/test/data/).
        self.assertNotIn("molt/page_browsertest.cc:2", errs)
        self.assertIn("molt_x/missing.html",
                      errs["molt/page_browsertest.cc:3"])
        for handled in ("molt/page_browsertest.cc:5",
                        "molt/page_browsertest.cc:6",
                        "molt/page_browsertest.cc:7"):
            self.assertNotIn(handled, errs)
        # What a fixture page loads.
        self.assertNotIn("chrome/test/data/molt_x/page.html:1", errs)
        self.assertIn("/media/missing.png",
                      errs["chrome/test/data/molt_x/page.html:2"])
        self.assertNotIn("chrome/test/data/molt_x/page.html:3", errs)

    def test_unknown_listing_warns(self):
        # Without the pinned tree's list, a file outside src/ is unknown.
        tree = g.Tree(self.root, self.src)
        ov = g.Overlay(self.src, sorted(TESTS_OVERLAY), tree)
        findings, _ = g.run_tests(tree, ov, ["linux"],
                                  g.SourceListing(TESTS_OVERLAY, None),
                                  progress=None)
        warns = self.by_where(findings, "testdata", "warning")
        self.assertIn("unknown", warns["molt/data_unittest.cc:16"])
        self.assertNotIn("molt/data_unittest.cc:16",
                         self.by_where(findings, "testdata"))


class SourceScanTest(unittest.TestCase):
    def test_cxx_literals(self):
        code, lits = g.cxx_literals(
            '// "in a comment"\n'
            'a("one", "t\\"wo"); /* "c" */ b(R"x(raw "q")x", \'"\');\n'
            'c("three");\n')
        self.assertEqual([(ln, v) for ln, _, _, v in lits],
                         [(2, "one"), (2, 't"wo'), (3, "three")])
        self.assertNotIn("in a comment", code)
        self.assertNotIn("raw", code)
        self.assertEqual(code.count("\n"), 3)

    def test_conditions(self):
        text = "\n".join([
            "a",                                   # 1
            "#if BUILDFLAG(IS_WIN)",
            "b",                                   # 3
            "#elif BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)",
            "c",                                   # 5
            "#else",
            "d",                                   # 7
            "#endif",
            "#if !BUILDFLAG(IS_POSIX) && defined(FOO)",
            "e",                                   # 10
            "#endif",
        ])
        conds = g.preprocessor_conditions(text)

        def on(line):
            return [p for p in ("linux", "mac", "win")
                    if g.line_compiled(conds[line], p)]
        self.assertEqual(on(1), ["linux", "mac", "win"])
        self.assertEqual(on(3), ["win"])
        self.assertEqual(on(5), ["linux", "mac"])
        self.assertEqual(on(7), [])
        self.assertEqual(on(10), ["win"])       # defined(FOO) unknown
        self.assertIsNone(g.eval_condition("BUILDFLAG(ENABLE_X)", "linux"))
        self.assertTrue(g.eval_condition("BUILDFLAG(IS_APPLE)", "mac"))

    def test_page_refs(self):
        refs = g.page_refs("chrome/test/data/d/p.html", "\n".join([
            '<img src="/a/b.png"><script src="s.js"></script>',
            '<a href="/not/a/load.html">x</a>',
            '<link rel="stylesheet" href="../c.css">',
            '<div style="background: url(\'/i/bg.png\')"></div>',
            '<video src="https://other.test/v.webm" poster="{{x}}.png">',
            '<script>new URL(location.href); f.src = x;</script>',
        ]))
        self.assertEqual(refs, [(1, "chrome/test/data/a/b.png"),
                                (1, "chrome/test/data/d/s.js"),
                                (3, "chrome/test/data/c.css"),
                                (4, "chrome/test/data/i/bg.png")])

    def test_listing(self):
        ls = g.SourceListing(["a/b.txt"], {"c/d/e.txt"}, ["v8"])
        self.assertTrue(ls.exists("a/b.txt"))
        self.assertTrue(ls.exists("c/d/"))
        self.assertTrue(ls.exists("c/d/e.txt"))
        self.assertFalse(ls.exists("c/x.txt"))
        self.assertIsNone(ls.exists("v8/x.txt"))
        self.assertIsNone(g.SourceListing(["a/b.txt"]).exists("c/x.txt"))


FLAG_HEADER = r"""
#include "x/molt_x_features.h"
#define MOLT_X_FEATURE_ENTRIES                                       \
    {molt_x::flags::kGood, "MoltBrowser: good", "It does good.",     \
     kOsAll, FEATURE_VALUE_TYPE(molt_x::kMoltGood)},                 \
    {molt_x::flags::kQuiet, "MoltBrowser: quiet", "",                \
     kOsAll, FEATURE_VALUE_TYPE(molt_x::kMoltQuiet)}
"""

FLAG_OVERLAY = {
    "chrome/browser/about_flags.cc": "\n".join([
        '#include "x/molt_x_about_flags.h"  // MoltBrowser',
        "const FeatureEntry kFeatureEntries[] = {",
        '    {"upstream-flag", "U", "u", kOsAll, SINGLE_VALUE_TYPE("u")},',
        "    // MoltBrowser.",
        "    MOLT_X_FEATURE_ENTRIES,",
        "};",
    ]),
    "x/molt_x_about_flags.h": FLAG_HEADER,
    "x/molt_x_features.h": "\n".join([
        'inline constexpr char kGood[] = "molt-good";',
        'inline constexpr char kQuiet[] = "molt-quiet";',
    ]),
    "x/molt_x_features.cc": "\n".join([
        "BASE_FEATURE(kMoltGood, base::FEATURE_ENABLED_BY_DEFAULT);",
        "BASE_FEATURE(kMoltQuiet, base::FEATURE_ENABLED_BY_DEFAULT);",
        "BASE_FEATURE(kMoltForgotten, base::FEATURE_DISABLED_BY_DEFAULT);",
    ]),
    "chrome/browser/flag-metadata.json": """[
  // A comment, and an owner path with slashes.
  { "name": "molt-good", "owners": [ "//x/OWNERS" ],
    "expiry_milestone": -1 },
  { "name": "molt-quiet", "owners": [ "moltbrowser" ],
    "expiry_milestone": -1 },
  { "name": "molt-gone", "owners": [ "a@b.c" ], "expiry_milestone": 160 },
  { "name": "aaa-upstream", "owners": [ "a@b.c" ], "expiry_milestone": 160 }
]
""",
    "chrome/browser/flag-never-expire-list.json":
        '[ "molt-good", "molt-unknown" ]\n',
    "x/OWNERS": "",
    "x/README.md": "Turn on chrome://flags/#molt-good or #molt-typo, "
                   "#molt-* and #molt-\nquiet\n",
}


class FlagsCheckTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="gn-static-check-flags-"))
        write_tree(self.tmp / "tree", {"BUILD.gn": ""})
        write_tree(self.tmp / "src", FLAG_OVERLAY)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_flags(self, enums=None):
        tree = g.Tree(self.tmp / "tree", self.tmp / "src")
        ov = g.Overlay(self.tmp / "src", sorted(FLAG_OVERLAY), tree)
        return g.run_flags(ov, (lambda rel: enums) if enums is not None
                           else None)

    def test_uma_id(self):
        # Values in Chromium 156's enums.xml.
        self.assertEqual(g.uma_id("OmniboxMaxURLMatches:enabled"),
                         -2146979021)
        self.assertEqual(g.uma_id("GlassToolbar:enabled"), 763904037)
        self.assertEqual(g.uma_id("GlassToolbar:disabled"), -138951938)

    def test_owner_and_order_rules(self):
        self.assertTrue(g.valid_looking_owner("a@b.com"))
        self.assertTrue(g.valid_looking_owner("//chrome/browser/OWNERS"))
        self.assertFalse(g.valid_looking_owner("moltbrowser"))
        self.assertFalse(g.valid_looking_owner("a@b"))
        self.assertFalse(g.valid_looking_owner("a@b.com,c@d.com"))
        self.assertEqual(g.misordered(["a", "b_c", "b-d", "c"]), [])
        self.assertEqual(g.misordered(["b", "a"]),
                         [("b", "a"), ("a", "(start of file)")])
        self.assertEqual(g.strip_json_comments(
            '[ // c\n "//x/OWNERS", /* d */ 1 ]'), '[ \n "//x/OWNERS",  1 ]')

    def test_flags(self):
        line = '<int value="%d" label="MoltGood:%%s"/>'
        enums = ("\n".join([line % g.uma_id("MoltGood:disabled") % "disabled",
                            line % g.uma_id("MoltGood:enabled") % "enabled"])
                 ).encode()
        findings, stats = self.run_flags(enums)
        msgs = [(f.where, f.message) for f in findings.errors()]
        text = "\n".join("%s: %s" % m for m in msgs)
        self.assertEqual(stats["entries"], 2)
        self.assertIn("x/molt_x_about_flags.h (#molt-quiet): has no "
                      "description", text)
        self.assertIn("kMoltForgotten is not a chrome://flags entry", text)
        self.assertIn("(#molt-quiet): has the owner 'moltbrowser'", text)
        self.assertIn("'aaa-upstream' should be right after "
                      "'(start of file)'", text)
        self.assertIn("lists molt-unknown, which has no metadata", text)
        self.assertIn("never expires but isn't in", text)   # molt-quiet
        self.assertIn("names #molt-typo, which is not", text)
        self.assertNotIn("#molt-good,", text)
        self.assertNotIn("#molt-,", text)   # #molt-*, a name cut at the end
        self.assertNotIn("(#molt-good)", text)
        warns = [f.message for f in findings.all()
                 if f.severity == "warning"]
        self.assertTrue(any("molt-gone" in w or "not a registered" in w
                            for w in warns))
        enum_warn = [w for w in warns if "LoginCustomFlags" in w]
        self.assertEqual(len(enum_warn), 1)
        self.assertIn("MoltQuiet:enabled", enum_warn[0])
        self.assertNotIn("MoltGood:", enum_warn[0])

    def test_unregistered_macro_header(self):
        bad = dict(FLAG_OVERLAY)
        bad["chrome/browser/about_flags.cc"] = bad[
            "chrome/browser/about_flags.cc"].replace(
            '#include "x/molt_x_about_flags.h"  // MoltBrowser', "")
        write_tree(self.tmp / "src", bad)
        findings, _ = self.run_flags(b"")
        self.assertTrue(any("doesn't include x/molt_x_about_flags.h"
                            in f.message for f in findings.errors()))


class FetchTest(unittest.TestCase):
    def test_fetch_keeps_gn_files_and_overlay_copies(self):
        import io
        import tarfile
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:xz") as tf:
            for name, data in (("BUILD.gn", b"group(\"all\") {}\n"),
                               ("a/b.gni", b"x = 1\n"),
                               ("a/DEPS", b"include_rules = []\n"),
                               ("a/c.cc", b"int c;\n"),
                               ("a/d.cc", b"int d;\n"),
                               ("chrome/VERSION", b"MAJOR=156\n"),
                               ("buildtools/checkdeps/rules.py", b"#\n")):
                info = tarfile.TarInfo("chromium-1.2.3.4/" + name)
                info.size = len(data)
                tf.addfile(info, io.BytesIO(data))
        name = "chromium-1.2.3.4-lite.tar.xz"
        pins = {name: hashlib.sha256(buf.getvalue()).hexdigest()}
        tmp = Path(tempfile.mkdtemp(prefix="gn-static-check-fetch-"))
        try:
            dest = tmp / "1.2.3.4"
            g.fetch("1.2.3.4", dest, ["a/c.cc"], log=lambda m: None,
                    opener=lambda url: io.BytesIO(buf.getvalue()), pins=pins)
            kept = sorted(str(p.relative_to(dest / "tree"))
                          for p in (dest / "tree").rglob("*") if p.is_file())
            self.assertEqual(kept, ["BUILD.gn", "a/DEPS", "a/b.gni", "a/c.cc",
                                    "buildtools/checkdeps/rules.py",
                                    "chrome/VERSION"])
            self.assertEqual(len(g.load_paths(dest / "paths.txt")), 7)
            self.assertFalse((tmp / "1.2.3.4.partial").exists())

            # Nothing is kept from a tarball that isn't the pinned one, or
            # that has no pin.
            other = tmp / "other"
            bad = {name: "0" * 64}
            for kwargs in ({"pins": bad}, {"pins": {}}):
                with self.assertRaises(g.cctarball.TarballNotVerified):
                    g.fetch("1.2.3.4", other, ["a/c.cc"], log=lambda m: None,
                            opener=lambda url: io.BytesIO(buf.getvalue()),
                            **kwargs)
                self.assertFalse(other.exists())
                self.assertFalse((tmp / "other.partial").exists())
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_fetch_keeps_nothing_outside_the_tree(self):
        import io
        import tarfile
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:xz") as tf:
            for name in ("chromium-1.2.3.4/a/../../escape.gn",
                         "chromium-1.2.3.4/./a/dot.gn",
                         "chromium-1.2.3.4/a/ok.gn"):
                info = tarfile.TarInfo(name)
                info.size = 2
                tf.addfile(info, io.BytesIO(b"#\n"))
        tmp = Path(tempfile.mkdtemp(prefix="gn-static-check-fetch-"))
        try:
            dest = tmp / "d" / "1.2.3.4"
            g.fetch("1.2.3.4", dest, [], log=lambda m: None,
                    opener=lambda url: io.BytesIO(buf.getvalue()),
                    pins={"chromium-1.2.3.4-lite.tar.xz":
                          hashlib.sha256(buf.getvalue()).hexdigest()})
            written = sorted(str(p.relative_to(tmp))
                             for p in tmp.rglob("*") if p.is_file())
            self.assertEqual(written, ["d/1.2.3.4/paths.txt",
                                       "d/1.2.3.4/tree/a/ok.gn",
                                       "d/1.2.3.4/version.txt"])
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
