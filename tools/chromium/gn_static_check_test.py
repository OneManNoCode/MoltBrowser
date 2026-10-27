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
sources, the overlay's "only the includes MoltBrowser added" rule, and the
plumbing of the checkdeps and Blink-audit runs (with stand-in scripts).

Run: python3 tools/chromium/gn_static_check_test.py -v
"""
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
        tmp = Path(tempfile.mkdtemp(prefix="gn-static-check-fetch-"))
        try:
            dest = tmp / "1.2.3.4"
            g.fetch("1.2.3.4", dest, ["a/c.cc"], log=lambda m: None,
                    opener=lambda url: io.BytesIO(buf.getvalue()))
            kept = sorted(str(p.relative_to(dest / "tree"))
                          for p in (dest / "tree").rglob("*") if p.is_file())
            self.assertEqual(kept, ["BUILD.gn", "a/DEPS", "a/b.gni", "a/c.cc",
                                    "buildtools/checkdeps/rules.py",
                                    "chrome/VERSION"])
            self.assertEqual(len(g.load_paths(dest / "paths.txt")), 7)
            self.assertFalse((tmp / "1.2.3.4.partial").exists())
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
