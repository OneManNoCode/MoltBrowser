#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for macho_bundle_deps.py.

Builds a small Homebrew-like tree of arm64 Mach-O files with clang and
ld64.lld (no macOS SDK: a stand-in libSystem), bundles it with LLVM's otool
and install_name_tool, and checks the result. Runs on Linux, which is how CI
runs it (lint.yml). The tests that need those tools are skipped when they're
missing, unless MOLT_REQUIRE_MACHO_TOOLS=1.

  python3 scripts/macho_bundle_deps_test.py
"""

import glob
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import macho_bundle_deps as mbd  # noqa: E402


def llvm_version(bin_dir):
    # /usr/lib/llvm-18/bin -> 18
    version = os.path.basename(os.path.dirname(bin_dir))[len("llvm-"):]
    return int(version) if version.isdigit() else 0


def find_tool(name):
    dirs = os.environ.get("PATH", "").split(os.pathsep)
    dirs += sorted(glob.glob("/usr/lib/llvm-*/bin"), key=llvm_version,
                   reverse=True)
    for d in dirs:
        path = os.path.join(d, name)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return None


CLANG = find_tool("clang")
LD64 = find_tool("ld64.lld")
OTOOL = find_tool("llvm-otool")
INSTALL_NAME_TOOL = find_tool("llvm-install-name-tool")
HAVE_TOOLS = all((CLANG, LD64, OTOOL, INSTALL_NAME_TOOL))
if not HAVE_TOOLS and os.environ.get("MOLT_REQUIRE_MACHO_TOOLS") == "1":
    sys.exit("missing clang / ld64.lld / llvm-otool / llvm-install-name-tool")
needs_tools = unittest.skipUnless(
    HAVE_TOOLS, "needs clang, ld64.lld, llvm-otool, llvm-install-name-tool")

TOOLS = mbd.Tools(OTOOL, INSTALL_NAME_TOOL, "")

# What dyld_stub_binder and each library's function look like to the linker.
SYSTEM_C = """
int sys_fn(void) { return 1; }
void stub_binder(void) __asm__("dyld_stub_binder");
void stub_binder(void) {}
"""


class Tree:
    """Builds C snippets into arm64 Mach-O dylibs and executables."""

    def __init__(self, root):
        self.root = root

    def path(self, rel):
        return os.path.join(self.root, rel)

    def build(self, out, code, links=(), install_name=None, rpaths=(),
              executable=False):
        out = self.path(out)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        src = out + ".c"
        with open(src, "w") as f:
            f.write(code)
        argv = [CLANG, "-target", "arm64-apple-macos11", "-fuse-ld=lld",
                "-B", os.path.dirname(LD64),
                "-nostdlib", "-o", out, src]
        argv += [self.path(link) for link in links]
        argv += ["-Wl,-headerpad_max_install_names"]
        if executable:
            argv += ["-Wl,-e,_start"]
        else:
            argv += ["-shared", "-Wl,-install_name," + install_name]
        for rpath in rpaths:
            argv += ["-Wl,-rpath," + rpath]
        subprocess.run(argv, check=True)
        os.remove(src)
        return out


def call(fn):
    return "int %s(void);\n" % fn


@needs_tools
class BundleTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        # The only OS library reached through an rpath.
        saved = mbd.system_library_exists
        mbd.system_library_exists = (
            lambda path: path == "/usr/lib/swift/libswiftCore.dylib")
        self.addCleanup(setattr, mbd, "system_library_exists", saved)
        t = self.tree = Tree(self.tmp)
        t.build("sdk/libSystem.B.dylib", SYSTEM_C,
                install_name="/usr/lib/libSystem.B.dylib")
        # A system library reached through an rpath, as Swift's are.
        t.build("sdk/libswiftCore.dylib", "int swift_fn(void) { return 9; }",
                install_name="@rpath/libswiftCore.dylib")
        # Found through foo's @loader_path-relative rpath.
        t.build("brew/Cellar/bar/2.0/lib/libbar.2.dylib",
                call("sys_fn") + "int bar_fn(void) { return sys_fn(); }",
                links=["sdk/libSystem.B.dylib"],
                install_name="@rpath/libbar.2.dylib")
        os.makedirs(t.path("brew/opt"))
        os.symlink("../Cellar/bar/2.0", t.path("brew/opt/bar"))
        # Loaded by absolute path, from both the tool and foo.
        t.build("brew/Cellar/baz/1.0/lib/libbaz.dylib",
                call("sys_fn") + "int baz_fn(void) { return sys_fn(); }",
                links=["sdk/libSystem.B.dylib"],
                install_name=t.path("brew/opt/baz/lib/libbaz.dylib"))
        os.symlink("../Cellar/baz/1.0", t.path("brew/opt/baz"))
        # Next to foo, loaded as @loader_path/libqux.dylib.
        t.build("brew/Cellar/foo/1.0/lib/libqux.dylib",
                call("sys_fn") + "int qux_fn(void) { return sys_fn(); }",
                links=["sdk/libSystem.B.dylib"],
                install_name="@loader_path/libqux.dylib")
        t.build("brew/Cellar/foo/1.0/lib/libfoo.1.dylib",
                call("bar_fn") + call("baz_fn") + call("qux_fn")
                + "int foo_fn(void) { return bar_fn() + baz_fn() + qux_fn(); }",
                links=["brew/Cellar/bar/2.0/lib/libbar.2.dylib",
                       "brew/Cellar/baz/1.0/lib/libbaz.dylib",
                       "brew/Cellar/foo/1.0/lib/libqux.dylib",
                       "sdk/libSystem.B.dylib"],
                install_name=t.path("brew/opt/foo/lib/libfoo.1.dylib"),
                rpaths=["@loader_path/../../../../opt/bar/lib"])
        os.symlink("../Cellar/foo/1.0", t.path("brew/opt/foo"))
        t.build("brew/Cellar/tool/1.0/bin/tool",
                call("foo_fn") + call("baz_fn") + call("swift_fn")
                + "int start(void) { return foo_fn() + baz_fn() + swift_fn(); }",
                links=["brew/Cellar/foo/1.0/lib/libfoo.1.dylib",
                       "brew/Cellar/baz/1.0/lib/libbaz.dylib",
                       "sdk/libswiftCore.dylib",
                       "sdk/libSystem.B.dylib"],
                rpaths=["/opt/homebrew/lib", "/usr/lib/swift"],
                executable=True)
        os.makedirs(t.path("brew/bin"))
        os.symlink("../Cellar/tool/1.0/bin/tool", t.path("brew/bin/tool"))

        self.origin = t.path("brew/bin/tool")
        self.helper = t.path("App/Resources/ocr/tool")
        self.lib_dir = t.path("App/Resources/ocr/lib")
        os.makedirs(os.path.dirname(self.helper))
        shutil.copyfile(self.origin, self.helper)

    def cmds(self, path):
        return mbd.load_commands(TOOLS, path)

    def test_bundles_and_rewrites(self):
        changed = mbd.bundle(TOOLS, self.helper, self.lib_dir, self.origin)
        libs = ["libbar.2.dylib", "libbaz.dylib", "libfoo.1.dylib",
                "libqux.dylib"]
        self.assertEqual(sorted(os.listdir(self.lib_dir)), libs)
        self.assertEqual(
            changed,
            sorted([self.helper] + [os.path.join(self.lib_dir, n)
                                    for n in libs]))

        tool = self.cmds(self.helper)
        self.assertEqual(sorted(tool.deps), [
            "/usr/lib/libSystem.B.dylib",
            "/usr/lib/swift/libswiftCore.dylib",
            "@executable_path/lib/libbaz.dylib",
            "@executable_path/lib/libfoo.1.dylib",
        ])
        self.assertEqual(tool.rpaths, [])

        foo = self.cmds(os.path.join(self.lib_dir, "libfoo.1.dylib"))
        self.assertEqual(foo.id, "@rpath/libfoo.1.dylib")
        self.assertEqual(sorted(foo.deps), [
            "/usr/lib/libSystem.B.dylib",
            "@loader_path/libbar.2.dylib",
            "@loader_path/libbaz.dylib",
            "@loader_path/libqux.dylib",
        ])
        self.assertEqual(foo.rpaths, [])
        for name in libs:
            lib = self.cmds(os.path.join(self.lib_dir, name))
            self.assertEqual(lib.id, "@rpath/" + name)
            self.assertNotIn(self.tmp, "".join(lib.deps))

        self.assertEqual(mbd.check(TOOLS, self.helper, self.lib_dir), [])

    def test_check_reports_the_unbundled_binary(self):
        problems = mbd.check(TOOLS, self.helper, None)
        self.assertIn("LC_RPATH /opt/homebrew/lib", "\n".join(problems))
        self.assertIn("loads " + self.tree.path(
            "brew/opt/foo/lib/libfoo.1.dylib"), "\n".join(problems))
        self.assertIn("loads @rpath/libswiftCore.dylib", "\n".join(problems))

    def test_check_rejects_a_library_outside_the_bundle(self):
        mbd.bundle(TOOLS, self.helper, self.lib_dir, self.origin)
        os.remove(os.path.join(self.lib_dir, "libqux.dylib"))
        problems = mbd.check(TOOLS, self.helper, self.lib_dir)
        self.assertEqual(problems, [
            os.path.join(self.lib_dir, "libfoo.1.dylib")
            + ": loads @loader_path/libqux.dylib"])

    def test_missing_library_fails(self):
        os.remove(self.tree.path("brew/Cellar/bar/2.0/lib/libbar.2.dylib"))
        with self.assertRaisesRegex(mbd.BundleError,
                                    "can't find @rpath/libbar.2.dylib"):
            mbd.bundle(TOOLS, self.helper, self.lib_dir, self.origin)

    def test_name_collision_fails(self):
        t = self.tree
        t.build("other/libbaz.dylib", "int other_fn(void) { return 3; }",
                install_name=t.path("other/libbaz.dylib"))
        t.build("App/Resources/ocr/tool",
                call("baz_fn") + call("other_fn")
                + "int start(void) { return baz_fn() + other_fn(); }",
                links=["brew/Cellar/baz/1.0/lib/libbaz.dylib",
                       "other/libbaz.dylib", "sdk/libSystem.B.dylib"],
                executable=True)
        with self.assertRaisesRegex(mbd.BundleError,
                                    "two different libraries are named"):
            mbd.bundle(TOOLS, self.helper, self.lib_dir, self.helper)

    def run_main(self, *argv):
        saved = sys.argv
        sys.argv = ["macho_bundle_deps.py"] + list(argv) + [
            "--otool", OTOOL, "--install-name-tool", INSTALL_NAME_TOOL,
            "--codesign", ""]
        try:
            return mbd.main()
        finally:
            sys.argv = saved

    def test_main(self):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            self.assertEqual(self.run_main(self.helper, "--check"), 1)
            self.assertEqual(self.run_main(self.helper, "--lib-dir",
                                           self.lib_dir, "--origin",
                                           self.origin), 0)
            self.assertEqual(self.run_main(self.helper, "--check",
                                           "--lib-dir", self.lib_dir), 0)
        self.assertIn("still loads code from outside", err.getvalue())
        self.assertIn("rewrote " + self.helper, out.getvalue())

    def test_provenance(self):
        prov = os.path.join(self.tmp, "provenance.json")
        with redirect_stdout(io.StringIO()):
            self.assertEqual(self.run_main(self.helper, "--lib-dir",
                                           self.lib_dir, "--origin",
                                           self.origin, "--provenance", prov),
                             0)
        with open(prov) as f:
            doc = json.load(f)
        cellar = os.path.realpath(self.tree.path("brew/Cellar"))
        self.assertEqual(doc["binary"], {
            "file": "tool",
            "origin": os.path.join(cellar, "tool/1.0/bin/tool")})
        self.assertEqual(doc["libraries"], [
            {"file": "lib/libbar.2.dylib",
             "origin": os.path.join(cellar, "bar/2.0/lib/libbar.2.dylib")},
            {"file": "lib/libbaz.dylib",
             "origin": os.path.join(cellar, "baz/1.0/lib/libbaz.dylib")},
            {"file": "lib/libfoo.1.dylib",
             "origin": os.path.join(cellar, "foo/1.0/lib/libfoo.1.dylib")},
            {"file": "lib/libqux.dylib",
             "origin": os.path.join(cellar, "foo/1.0/lib/libqux.dylib")},
        ])

    def test_nothing_to_bundle_leaves_no_lib_dir(self):
        t = self.tree
        t.build("App/Resources/ocr/tool",
                call("sys_fn") + "int start(void) { return sys_fn(); }",
                links=["sdk/libSystem.B.dylib"], executable=True)
        with redirect_stdout(io.StringIO()):
            self.assertEqual(
                self.run_main(self.helper, "--lib-dir", self.lib_dir), 0)
        self.assertFalse(os.path.exists(self.lib_dir))


class ProvenanceTest(unittest.TestCase):
    def test_home_is_not_recorded(self):
        self.assertEqual(
            mbd.public_origin("/Users/raj/src/whisper.cpp/build/bin/whisper-cli",
                              "/Users/raj"),
            "~/src/whisper.cpp/build/bin/whisper-cli")
        self.assertEqual(
            mbd.public_origin("/opt/homebrew/Cellar/libomp/20.1.8/lib/libomp.dylib",
                              "/Users/raj"),
            "/opt/homebrew/Cellar/libomp/20.1.8/lib/libomp.dylib")
        self.assertEqual(mbd.public_origin("/Users/rajesh/x", "/Users/raj"),
                         "/Users/rajesh/x")


class ParseTest(unittest.TestCase):
    def test_fat_output(self):
        text = """\
/x/tool (architecture x86_64):
Load command 0
      cmd LC_SEGMENT_64
  cmdsize 72
  segname __PAGEZERO
Load command 1
          cmd LC_LOAD_DYLIB
      cmdsize 64
         name /opt/homebrew/opt/lept onica/lib/liblept.6.dylib (offset 24)
   time stamp 2 Thu Jan  1 00:00:02 1970
Load command 2
          cmd LC_RPATH
      cmdsize 32
         path @loader_path/../lib (offset 12)
Load command 3
          cmd LC_LOAD_DYLINKER
      cmdsize 32
         name /usr/lib/dyld (offset 12)
/x/tool (architecture arm64):
Load command 0
          cmd LC_LOAD_WEAK_DYLIB
      cmdsize 64
         name /opt/homebrew/opt/lept onica/lib/liblept.6.dylib (offset 24)
Load command 1
          cmd LC_LOAD_DYLIB
      cmdsize 56
         name /usr/lib/libSystem.B.dylib (offset 24)
Load command 2
          cmd LC_RPATH
      cmdsize 32
         path @loader_path/../lib (offset 12)
"""
        cmds = mbd.parse_load_commands(text)
        self.assertIsNone(cmds.id)
        self.assertEqual(cmds.deps, [
            "/opt/homebrew/opt/lept onica/lib/liblept.6.dylib",
            "/usr/lib/libSystem.B.dylib"])
        self.assertEqual(cmds.rpaths, ["@loader_path/../lib"])

    def test_system(self):
        self.assertTrue(mbd.is_system("/usr/lib/libz.1.dylib"))
        self.assertTrue(mbd.is_system(
            "/System/Library/Frameworks/Metal.framework/Versions/A/Metal"))
        self.assertFalse(mbd.is_system("/usr/local/lib/libz.1.dylib"))
        self.assertFalse(mbd.is_system("/usr/libexec/x"))
        self.assertFalse(mbd.is_system("@rpath/libz.1.dylib"))


if __name__ == "__main__":
    unittest.main()
