#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""A static `gn gen --check` for MoltBrowser's changes, without a checkout.

It reads Chromium's BUILD.gn and .gni files at the pin (chromium_version.txt)
with the src/ overlay applied, evaluates them with the GN interpreter of
tools/compile_check (ccheck/gn.py) for the desktop configs in configs/
(linux-x64, macos-arm64, windows-x64; also with molt_llama_vulkan on, and
as a component build), loads what `gn gen` would load (//BUILD.gn and every
BUILD.gn a loaded target names, transitively), and applies GN's own rules to
every file and every target MoltBrowser adds or changes:

  includes   `gn check`'s include rule. A quoted #include of a file that some
             loaded target lists (sources, public, or an action's outputs)
             must come from a target that depends on it directly, or through
             a chain of public_deps after the first step; the header must be
             public in it (no `public` list, or listed there, or `friend`
             names the includer); `allow_circular_includes_from` on the
             included target permits the reverse direction;
             `check_includes = false`, //.gn's `no_check_targets` and
             `// nogncheck` skip, as in gn. Files are scanned as gn scans
             them (quoted includes only, until more than ten lines with
             neither an include, a comment, a directive nor a blank).
  deps       `gn gen`'s dependency rules for every dependency a MoltBrowser
             target (or a MoltBrowser line in an upstream BUILD.gn) adds: the
             target exists, its `visibility` lets the dependent in, a target
             that isn't testonly depends on no testonly one, an
             `allow_circular_includes_from` entry is a dependency and the
             target using it is on //.gn's allowlist.
  cycles     dependency cycles (deps, public_deps, data_deps) in the loaded
             graph that upstream's graph doesn't have.
  sources    every file a MoltBrowser target lists exists (in the tree, the
             overlay, or as an output of an action).
  unlisted   MoltBrowser's own C++ files that no target lists in any
             configuration (gn check skips includes of such a file; list it
             where it belongs so later edits don't trip).

Of an upstream file MoltBrowser changed, only the includes MoltBrowser added
are checked (the rest passed upstream's own gn check with the same targets).

What is approximated (the build machine's `gn gen --check` stays the
authority, docs/BUILD_MACHINE_CHECKLIST.md "First build"):
  * GN conditions whose value comes from outside (exec_script, read_file,
    toolchain variables of other toolchains) are UNKNOWN and take their
    "then" branch, as in compile_check; a false assert() keeps a file out of
    the build, unless it reads an UNKNOWN value. Only the default toolchain
    is modelled: a dependency's "(toolchain)" suffix is dropped (data_deps
    with one are left out), and a target's dependency on its own label in
    another toolchain is not an edge.
  * An include resolves against the including file's directory, `//` and
    the generated-files root, as with Chromium's default include_dirs;
    other include_dirs (a third-party library's own) are not followed, so
    such includes are not checked.
  * Templates run (ccheck/gn.py `expand`) except the ones it leaves out;
    for those this tool models what matters: mojom() (the chromium, blink
    and shared variants, their _headers, _shared_cpp_sources and proxy
    targets, typemap and overridden deps), test() and fuzzer_test()
    (testonly), rust_static_library() cxx bindings, and the invocation's
    own sources and deps otherwise. A label such a template defines next
    to its own (<name>_js, ...) is reported as a warning, not an error.
  * The include rule was checked against upstream: every include of the
    upstream copies of the files MoltBrowser changes, and of 15,000
    upstream files in loaded targets, passes on every config (they pass
    upstream's gn check).

Two presubmit checks run over the same overlay:
  checkdeps    Chromium's own buildtools/checkdeps/checkdeps.py (from the
               pin) over a copy of the tree's DEPS files with MoltBrowser's
               on top, for every C++ file MoltBrowser adds and the includes
               it adds to upstream files (Skia and V8 are their own
               repositories with their own rules, and are left out).
  blink-audit  Blink's audit_non_blink_usage.py check() (the overlay's copy,
               with MoltBrowser's entries) over the lines MoltBrowser adds
               under third_party/blink, as Blink's presubmit runs it.

Two checks of what the first real build's tests would trip on:
  tests        Every MoltBrowser test source (a *_unittest, *_browsertest,
               *_interactive_uitest or *_fuzzer file, or one with gtest's
               TEST macros) is listed in a target that a binary meant to run
               it reaches through deps in some configuration (TEST_BINARIES:
               unit tests in molt_ai_unittests, unit_tests, blink_unittests
               or media_unittests; browser tests in browser_tests or
               interactive_ui_tests), and every fuzzer_test() an overlay
               BUILD.gn adds is reached from molt_fuzzers. Orphans are named
               with the targets that list them, or "no target"; a test in a
               static library is a warning (the linker may drop it).
               Then the test data each test opens, as its string literals
               show it: PathService roots (DIR_SRC_TEST_DATA_ROOT,
               DIR_GEN_TEST_DATA_ROOT, DIR_EXE) followed by AppendASCII()
               of literals, a browser test's served paths ("/molt_voice/
               x.html", from chrome/test/data), file names joined to one of
               those directories elsewhere in the file, and what the
               fixture pages under chrome/test/data load ("/media/
               bigbuck.webm"). Each must exist (in src/, or in the pinned
               commit's tree: its file list comes from
               tools/chromium/upstream.py's cache, --chromium-cache, since
               the release tarball leaves test data out), a generated file
               must be some target's output, an executable next to the
               binary must be in its runtime deps, and a file must be in the
               runtime data (`data`, through deps and data_deps) of every
               binary that runs the test. A line under #if BUILDFLAG(IS_*)
               counts only on its platform.
  flags        Every BASE_FEATURE MoltBrowser defines is a chrome://flags
               entry (in about_flags.cc's kFeatureEntries, through a
               MOLT_*_FEATURE_ENTRIES macro of a header it includes), with
               a molt- name, a visible name and a description, or is in
               NOT_FLAGS with the reason; and AboutFlagsTest's rules on
               flag-metadata.json and flag-never-expire-list.json hold
               (an entry per flag, valid-looking owners, never-expiring
               flags listed, both files in order). LoginCustomFlags in
               enums.xml (AboutFlagsHistogramTest) is a warning with the
               missing lines, an owner's decision (BUILD_MACHINE_CHECKLIST.md
               §11). Every #molt-... in src/ and the docs names a flag.

Usage:
  tools/chromium/gn_static_check.py fetch      # stream the pin's GN files once
                                    # (kept if the tarball matches its
                                    # pinned SHA-256: ccheck/tarball.py)
  tools/chromium/gn_static_check.py check [--platform CONFIGS] [--tree DIR]
                                    [--warnings] [--json OUT] [FILE...]
  tools/chromium/gn_static_check.py checkdeps [--json OUT]
  tools/chromium/gn_static_check.py blink-audit [--json OUT]
  tools/chromium/gn_static_check.py tests [--platform CONFIGS]
                                    [--chromium-cache DIR] [--warnings]
  tools/chromium/gn_static_check.py flags [--chromium-cache DIR] [--warnings]
  python3 tools/chromium/gn_static_check_test.py     # the checker's tests

Each command exits 1 when it finds an error. FILE... (paths under src/) limits the
include check to those files; by default every C++ file the overlay adds or
changes is checked, and every target in an overlay BUILD.gn. CONFIGS is a
comma-separated list of linux, mac or win, each optionally +vulkan and
+component (default: DEFAULT_CONFIGS below).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO_ROOT / "tools" / "compile_check"))

import upstream as up  # noqa: E402
from ccheck import gn as gnlib  # noqa: E402
from ccheck import tarball as cctarball  # noqa: E402

DEFAULT_CACHE = REPO_ROOT / ".molt-gn-check-cache"
BUCKET = ("https://commondatastorage.googleapis.com/"
          "chromium-browser-official/")

OUT = gnlib.OUT                       # "//out/cc"
GEN = OUT + "/gen/"

# The desktop configs MoltBrowser ships (configs/*.gn) and the globals gn
# would set for each before BUILDCONFIG.gn runs.
PLATFORMS = {
    "linux": {"config": "configs/linux-x64.gn", "os": "linux", "cpu": "x64",
              "toolchain": "//build/toolchain/linux:clang_x64"},
    "mac": {"config": "configs/macos-arm64.gn", "os": "mac", "cpu": "arm64",
            "toolchain": "//build/toolchain/mac:clang_arm64"},
    "win": {"config": "configs/windows-x64.gn", "os": "win", "cpu": "x64",
            "toolchain": "//build/toolchain/win:win_clang_x64"},
}

# Variants of a desktop config (`--platform linux+vulkan`): the GN args
# MoltBrowser's own switches and Chromium's component build change.
VARIANTS = {
    "vulkan": {"molt_llama_vulkan": True},
    # A developer's component build (an official build can't be one).
    "component": {"is_component_build": True, "is_official_build": False},
}
# What `check` evaluates by default: the three shipped configs, the Vulkan
# backend where it is allowed, and a component build.
DEFAULT_CONFIGS = "linux,mac,win,linux+vulkan,win+vulkan,linux+component"


def parse_config(spec):
    """"linux+vulkan" -> ("linux", {"molt_llama_vulkan": True})."""
    parts = spec.split("+")
    if parts[0] not in PLATFORMS:
        raise ValueError("unknown platform %s" % parts[0])
    extra = {}
    for v in parts[1:]:
        if v not in VARIANTS:
            raise ValueError("unknown variant %s" % v)
        extra.update(VARIANTS[v])
    return parts[0], extra


# Target types gn defines itself (BUILDCONFIG.gn wraps some of them in
# templates of the same name, whose bodies call target(type, name)).
BUILTIN_TYPES = {
    "source_set", "static_library", "shared_library", "executable",
    "loadable_module", "group", "action", "action_foreach", "copy",
    "bundle_data", "create_bundle", "generated_file", "rust_library",
    "rust_proc_macro",
}
GENERATING_TYPES = {"action", "action_foreach", "copy", "generated_file"}
# Templates the interpreter doesn't run whose targets are always testonly.
TESTONLY_TEMPLATES = {"test", "fuzzer_test"}

# What gn check scans (gn's SourceFile types C, CPP, H, M, MM, RC).
CHECKED_EXT = (".c", ".cc", ".cpp", ".cxx", ".c++", ".h", ".hpp", ".hxx",
               ".hh", ".inc", ".ipp", ".inl", ".m", ".mm", ".rc")
CXX_EXT = (".c", ".cc", ".cpp", ".h", ".hpp", ".hh", ".inc", ".m", ".mm")

# mojom() targets: the header suffixes of each generated variant (from
# mojo/public/tools/bindings/mojom.gni). `shared` holds the types both C++
# variants use; the chromium and blink variants depend on it publicly.
MOJOM_VARIANTS = {
    "": ["-forward.h", "-import-headers.h", "-test-utils.h", ".cc", ".h"],
    "_blink": ["-blink-forward.h", "-blink-import-headers.h",
               "-blink-test-utils.h", "-blink.cc", "-blink.h"],
    "_shared": ["-data-view.h", "-features.h", "-params-data.h",
                "-send-validation.h", "-shared-internal.h", "-shared.cc",
                "-shared.h"],
}
MOJOM_TEMPLATES = {"mojom", "mojom_component"}


def log(msg):
    print(msg, file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# The tree: 156's GN files with the overlay on top


class Tree:
    """Chromium's GN files (a `fetch`ed tree or a checkout) with an overlay
    directory on top. `paths` is the set of every file in the tree when it
    is known (a fetched tree records it); None means "ask the disk"."""

    def __init__(self, root, overlay=None, paths=None):
        self.root = Path(root)
        self.tree_dir = self.root        # for gn.py's exec_script
        self.overlay = Path(overlay) if overlay else None
        self.paths = paths

    def _overlay_file(self, rel):
        if self.overlay is None:
            return None
        p = self.overlay / rel
        return p if p.is_file() else None

    def read(self, rel):
        """Bytes of //rel, or None."""
        p = self._overlay_file(rel)
        if p is None:
            p = self.root / rel
            if not p.is_file():
                return None
        return p.read_bytes()

    def exists(self, rel):
        if self._overlay_file(rel) is not None:
            return True
        if self.paths is not None:
            return rel in self.paths
        return (self.root / rel).exists()

    def in_overlay(self, rel):
        return self._overlay_file(rel) is not None


def load_paths(path):
    p = Path(path)
    if not p.is_file():
        return None
    return set(p.read_text().split("\n")) - {""}


# ---------------------------------------------------------------------------
# GN evaluation


class AssertFailed(Exception):
    pass


def _unknownish(v):
    if v is gnlib.UNKNOWN:
        return True
    if isinstance(v, str):
        return "<UNKNOWN>" in v
    if isinstance(v, list):
        return any(_unknownish(x) for x in v)
    return False


class Interp(gnlib.Interp):
    """ccheck's interpreter, plus target(type, name) {} recorded under the
    target's own name and type, and templates that capture the variables of
    their definition by value, as gn's closures do (BUILDCONFIG.gn defines
    the source_set and executable templates in a foreach loop)."""

    def _uncertain(self, e, scope, ctx):
        """Whether an expression reads a value the interpreter doesn't
        know (an exec_script result, a string built from one, ...)."""
        k = e[0]
        if k in ("id", "member", "str", "index"):
            return _unknownish(self.eval(e, scope, ctx))
        if k == "call":
            if e[1] == "defined":
                return False
            return _unknownish(self.eval(e, scope, ctx)) or any(
                self._uncertain(a, scope, ctx) for a in e[2])
        if k == "not":
            return self._uncertain(e[1], scope, ctx)
        if k == "bin":
            return self._uncertain(e[2], scope, ctx) or \
                self._uncertain(e[3], scope, ctx)
        if k == "list":
            return any(self._uncertain(x, scope, ctx) for x in e[1])
        return False

    def _rebase(self, v, new_base, cur, ctx):
        # rebase_path(x, "") is a system-absolute path, which gn turns back
        # into a source-absolute one in `sources` (Blink's build.gni lists
        # go through it): keep it source-absolute.
        if new_base == "" and isinstance(v, str) and \
                isinstance(cur, str) and not (v.startswith("/") and
                                              not v.startswith("//")):
            c = self.abspath(cur, ctx) if cur != "." else ctx["dir"]
            return self.abspath(v, {"dir": c})
        return super()._rebase(v, new_base, cur, ctx)

    def call_expr(self, name, args, scope, ctx):
        if name in ("filter_include", "filter_exclude") and args:
            lst = self.eval(args[0], scope, ctx)
            if isinstance(lst, list) and any(x is gnlib.UNKNOWN
                                             for x in lst):
                return gnlib.UNKNOWN
        if name in ("filter_labels_include", "filter_labels_exclude") and \
                len(args) == 2:
            # WebRTC's rtc_library() moves Abseil deps with these.
            lst = self.eval(args[0], scope, ctx)
            pats = self.eval(args[1], scope, ctx)
            if not isinstance(lst, list) or not isinstance(pats, list) or \
                    _unknownish(lst) or _unknownish(pats):
                return gnlib.UNKNOWN
            d = ctx.get("dir") or "//"
            pats = [p for p in (norm_pattern(x, d) for x in pats) if p]
            keep = name == "filter_labels_include"
            return [x for x in lst if any(
                label_matches(p, norm_label(x, d) or "") for p in pats)
                == keep]
        if name == "get_target_outputs" and args and \
                ctx.get("targets") is not None:
            # Also an action_foreach's outputs, one set per source.
            label = self.eval(args[0], scope, ctx)
            if isinstance(label, str) and label.startswith(":"):
                for t in reversed(ctx["targets"]):
                    if t.name != label[1:] or t.func not in (
                            "action_foreach", "copy"):
                        continue
                    outs = _strs(t.get("outputs"))
                    srcs = _strs(t.get("sources"))
                    if outs and srcs:
                        return [abspath(_expand_output(o, s, t.path),
                                        t.path) for s in srcs for o in outs]
                    break
        return super().call_expr(name, args, scope, ctx)

    def call_stmt(self, name, args, block, scope, ctx):
        if name == "assert" and args and ctx.get("targets") is not None:
            # A false assert makes gn gen fail, so a file whose assert is
            # false here is one this configuration never loads.
            c = self.eval(args[0], scope, ctx)
            if c is False and not self._uncertain(args[0], scope, ctx):
                msg = self.eval(args[1], scope, ctx) if len(args) > 1 else ""
                raise AssertFailed(msg if isinstance(msg, str) else "")
            return
        if name == "template" and block is not None and args:
            tn = self.eval(args[0], scope, ctx)
            if isinstance(tn, str):
                # gn's Scope::MakeClosure(): the mutable scopes are copied;
                # the const global scope (BUILDCONFIG.gn's, once it has run)
                # is referenced. While BUILDCONFIG.gn itself runs, its scope
                # is the mutable root and is copied too.
                in_buildconfig = scope is self.globals
                closure = gnlib.Scope(None if in_buildconfig
                                      else self.globals)
                chain = []
                s = scope
                while s is not None and (in_buildconfig or
                                         s is not self.globals):
                    chain.append(s)
                    s = s.parent
                for s in reversed(chain):
                    closure.vars.update(s.vars)
                scope.set("tmpl::" + tn, (block, closure))
            return
        if name == "target" and block is not None and len(args) >= 2:
            ttype = self.eval(args[0], scope, ctx)
            tname = self.eval(args[1], scope, ctx)
            s = gnlib.Scope(scope)
            if isinstance(ttype, str):
                for k, v in self.target_defaults.get(ttype, {}).items():
                    s.set(k, list(v) if isinstance(v, list) else v)
            s.set("target_name", tname)
            self.exec_block(block, s, ctx)
            if ctx.get("targets") is not None and isinstance(ttype, str):
                r = gnlib.Target(ttype, tname, s, ctx["dir"])
                _snapshot(r)
                ctx["targets"].append(r)
            return
        targets = ctx.get("targets")
        n0 = len(targets) if targets is not None else 0
        super().call_stmt(name, args, block, scope, ctx)
        if targets is not None:
            for r in targets[n0:]:
                if not hasattr(r, "snap"):
                    _snapshot(r)


# Read through enclosing scopes, as gn does, when the target is defined: a
# BUILD.gn's `visibility = [ ":*" ]` covers only the targets after it.
SNAPSHOT_KEYS = ("visibility", "testonly", "check_includes")


def _snapshot(r):
    snap = {}
    for k in SNAPSHOT_KEYS:
        try:
            snap[k] = r.scope.get(k)
        except KeyError:
            pass
    r.snap = snap


def make_interp(tree, platform, extra_args=None):
    """An Interp over `tree` with BUILDCONFIG.gn run for a platform."""
    spec = PLATFORMS[platform]

    def read(path):
        if not path.startswith("//"):
            return None
        data = tree.read(path[2:])
        if data is None and path in gnlib.VIRTUAL_FILES:
            return gnlib.VIRTUAL_FILES[path]
        return data.decode("utf-8", "replace") if data is not None else None

    args = {}
    dot = read("//.gn")
    if dot:
        it0 = Interp(read, {})
        s = gnlib.Scope()
        try:
            it0.exec_block(gnlib.Parser(dot).parse_file(), s,
                           {"dir": "//", "targets": None})
        except SyntaxError as e:
            log("gn: .gn: %s" % e)
        da = s.vars.get("default_args")
        if isinstance(da, gnlib.Scope):
            args.update(da.vars)
    cfg = REPO_ROOT / spec["config"]
    args.update(gnlib.parse_args_file(cfg.read_text()))
    args.update(extra_args or {})
    g = gnlib.Scope()
    host_os = spec["os"]
    host_cpu = spec["cpu"]
    for k, v in {"host_os": host_os, "host_cpu": host_cpu,
                 "target_os": spec["os"], "target_cpu": spec["cpu"],
                 "current_os": spec["os"], "current_cpu": spec["cpu"],
                 "root_build_dir": OUT, "root_gen_dir": OUT + "/gen",
                 "root_out_dir": OUT, "python_path": "python3",
                 "gn_version": 2300,
                 "current_toolchain": spec["toolchain"],
                 "default_toolchain": spec["toolchain"],
                 "host_toolchain": spec["toolchain"]}.items():
        g.set(k, v)
    it = Interp(read, args, g, root=str(tree.tree_dir))
    it.run_buildconfig()
    return it


def dotfile_settings(tree):
    """//.gn's no_check_targets and allow_circular_includes_from_allowlist
    (label patterns)."""

    def read(path):
        data = tree.read(path[2:]) if path.startswith("//") else None
        return data.decode("utf-8", "replace") if data is not None else None

    text = read("//.gn") or ""
    it = Interp(read, {})
    s = gnlib.Scope()
    try:
        it.exec_block(gnlib.Parser(text).parse_file(), s,
                      {"dir": "//", "targets": None})
    except SyntaxError as e:
        log("gn: .gn: %s" % e)

    def pats(v):
        if not isinstance(v, list):
            return None
        return [p for p in (norm_pattern(x, "//") for x in v) if p]

    return {"no_check_targets": pats(s.vars.get("no_check_targets")) or [],
            "circular_allowlist":
                pats(s.vars.get("allow_circular_includes_from_allowlist"))}


# ---------------------------------------------------------------------------
# Labels and patterns


def abspath(p, base_dir):
    """A GN path relative to base_dir ("//a/b") -> "//...". Absolute system
    paths are returned unchanged."""
    if p.startswith("/") and not p.startswith("//"):
        return p
    if p.startswith("//"):
        parts = []
        rest = p[2:]
    else:
        parts = [x for x in base_dir[2:].split("/") if x]
        rest = p
    for c in rest.split("/"):
        if c == "..":
            if parts:
                parts.pop()
        elif c not in (".", ""):
            parts.append(c)
    return "//" + "/".join(parts)


def norm_label(lab, base_dir):
    """A dependency as written -> "//dir:name" (the toolchain dropped);
    None if it can't be read."""
    if not isinstance(lab, str) or "<UNKNOWN>" in lab or not lab:
        return None
    lab = lab.split("(", 1)[0].strip()
    if lab.startswith(":"):
        return base_dir + lab
    if ":" in lab:
        d, n = lab.split(":", 1)
    else:
        d, n = lab, None
    d = abspath(d, base_dir) if d else base_dir
    if not n:
        # "//a/b" and "//a/b:" both name //a/b:b.
        n = d.rstrip("/").rsplit("/", 1)[-1]
    return d + ":" + n


def norm_pattern(pat, base_dir):
    """A label pattern ("*", "//a/*", "//a:*", ":b", "../c:*") -> absolute
    form, None if unreadable."""
    if not isinstance(pat, str) or "<UNKNOWN>" in pat:
        return None
    pat = pat.split("(", 1)[0].strip()
    if pat == "*":
        return "*"
    if pat.startswith(":"):
        return base_dir + pat
    if pat.endswith("/*"):
        d = pat[:-2]
        return (abspath(d, base_dir) if d else base_dir).rstrip("/") + "/*"
    return norm_label(pat, base_dir)


def label_matches(pattern, label):
    """gn's label pattern match (gn help label_pattern)."""
    if pattern == "*":
        return True
    d, _, n = label.partition(":")
    if pattern.endswith("/*"):
        base = pattern[:-2]
        return d == base or d.startswith(base + "/") or base == "/"
    pd, _, pn = pattern.partition(":")
    if pd != d:
        return False
    return pn == "*" or pn == n


# ---------------------------------------------------------------------------
# Targets


class GnTarget:
    __slots__ = ("label", "type", "sources", "public", "deps", "public_deps",
                 "visibility", "testonly", "check_includes",
                 "allow_circular", "friend", "outputs", "path", "template",
                 "data_deps", "data", "output_name")

    def __init__(self, label, ttype, path):
        self.label = label
        self.type = ttype
        self.path = path              # the BUILD.gn file's dir
        self.template = None          # the template it came from, if any
        self.sources = set()
        self.public = None
        self.deps = []
        self.public_deps = []
        self.visibility = None
        self.testonly = False
        self.check_includes = True
        self.allow_circular = []
        self.friend = []
        self.outputs = []
        self.data_deps = []
        self.data = []                # runtime files; a directory ends in /
        self.output_name = None

    def files(self):
        """[(file, is_public, is_generated)] as gn check's file map."""
        out = {}
        default_public = self.public is None
        for f in self.sources:
            out[f] = (default_public, False)
        for f in self.public or ():
            out[f] = (True, False)
        for f in self.outputs:
            out[f] = (True, True)
        return [(f, p, g) for f, (p, g) in out.items()]

    def all_deps(self):
        return self.public_deps + [d for d in self.deps
                                   if d not in self.public_deps]

    def __repr__(self):
        return "<%s %s>" % (self.type, self.label)


def _strs(v):
    if not isinstance(v, list):
        return []
    return [x for x in v if isinstance(x, str) and "<UNKNOWN>" not in x]


def _bool(v, default):
    return v if isinstance(v, bool) else default


def _fill(t, scope_get, path, nested_get=None):
    nested_get = nested_get or scope_get
    t.sources = {abspath(s, path) for s in _strs(scope_get("sources"))}
    pub = scope_get("public")
    if isinstance(pub, list):
        t.public = {abspath(s, path) for s in _strs(pub)}
    # A dependency on the target's own label names it in another toolchain
    # (a host tool's wrapper group): not an edge in this one.
    t.deps = [x for x in (norm_label(d, path)
                          for d in _strs(scope_get("deps")))
              if x and x != t.label]
    t.public_deps = [x for x in (norm_label(d, path)
                                 for d in _strs(scope_get("public_deps")))
                     if x and x != t.label]
    # gn reads these through enclosing scopes too: a BUILD.gn's top-level
    # `testonly = true` or `visibility = [...]` covers all its targets.
    vis = nested_get("visibility")
    if isinstance(vis, list) and _unknownish(vis):
        t.visibility = None      # partly unknown: don't judge it
    elif isinstance(vis, list):
        t.visibility = [x for x in (norm_pattern(v, path) for v in vis) if x]
    elif isinstance(vis, str):
        t.visibility = [x for x in [norm_pattern(vis, path)] if x]
    t.testonly = _bool(nested_get("testonly"), False)
    t.check_includes = _bool(nested_get("check_includes"), True)
    t.allow_circular = [x for x in (norm_label(d, path) for d in
                                    _strs(scope_get("allow_circular_includes_from")))
                        if x]
    # data_deps naming another toolchain (a host tool) can't close a cycle
    # in this one.
    t.data_deps = [x for x in (norm_label(d, path)
                               for d in _strs(scope_get("data_deps"))
                               if "(" not in d) if x and x != t.label]
    t.friend = [x for x in (norm_pattern(v, path)
                            for v in _strs(scope_get("friend"))) if x]
    # Runtime data: a directory keeps its trailing "/", as gn reads it.
    t.data = [abspath(x, path) + ("/" if x.endswith("/") else "")
              for x in _strs(scope_get("data"))]
    name = scope_get("output_name")
    t.output_name = name if isinstance(name, str) and \
        "<UNKNOWN>" not in name else None


def _expand_output(o, src, path):
    a = abspath(src, path)
    d, f = a.rsplit("/", 1)
    vals = {"source": a, "source_file_part": f,
            "source_name_part": f.rsplit(".", 1)[0] if "." in f else f,
            "source_dir": d, "source_root_relative_dir": d[2:],
            "source_gen_dir": OUT + "/gen/" + d[2:],
            "source_out_dir": OUT + "/obj/" + d[2:],
            "source_target_relative": f}
    for k, v in vals.items():
        o = o.replace("{{%s}}" % k, v)
    return o


REF_KEYS = ("deps", "public_deps", "data_deps", "configs", "public_configs",
            "all_dependent_configs")


def referenced_dirs(raw, toolchains):
    """The directories whose BUILD.gn gn loads because of these records (in
    the default toolchain): every label in REF_KEYS, a label with a toolchain
    other than `toolchains` left out."""
    out = set()
    for r in raw:
        for k in REF_KEYS:
            for lab in _strs(r.scope.vars.get(k)):
                if "(" in lab:
                    tc = lab.split("(", 1)[1].rstrip(")")
                    if norm_label(tc, r.path) not in toolchains:
                        continue
                n = norm_label(lab, r.path)
                if n:
                    out.add(n.split(":", 1)[0])
    return out


def targets_of_file(raw, path):
    """ccheck Target records of one BUILD.gn -> {label: GnTarget}."""
    by_label = {}
    for r in raw:
        if r.func == "config" or not isinstance(r.name, str):
            continue
        if "<UNKNOWN>" in r.name:
            continue
        by_label.setdefault(r.path + ":" + r.name, []).append(r)
    out = {}
    for label, recs in by_label.items():
        # Records of one label are nested expansions of one target (a
        # template invocation is recorded before its body runs): the last
        # one is the real target, or the innermost template that was not
        # run (component, test, mojom, ...), whose invocation is kept.
        r = recs[-1]
        t = GnTarget(label, r.func, r.path)
        if recs[0].func != r.func or r.func not in BUILTIN_TYPES:
            t.template = recs[0].func

        def get(k, r=r):
            return r.scope.vars.get(k)

        def nested(k, recs=recs):
            # Innermost record first; each looks through its enclosing
            # scopes (the invocation's reach the BUILD.gn's own).
            for rr in reversed(recs):
                snap = getattr(rr, "snap", None)
                if snap is not None:
                    if k in snap:
                        return snap[k]
                    continue
                try:
                    return rr.scope.get(k)
                except KeyError:
                    continue
            return None

        _fill(t, get, r.path, nested)
        if r.func in TESTONLY_TEMPLATES:
            t.testonly = True
        if r.func in GENERATING_TYPES:
            outs = _strs(get("outputs"))
            if r.func in ("action_foreach", "copy") and \
                    any("{{" in o for o in outs):
                srcs = _strs(get("sources"))
                t.outputs = [abspath(_expand_output(o, s, r.path), r.path)
                             for o in outs for s in srcs]
            else:
                t.outputs = [abspath(o, r.path) for o in outs
                             if "{{" not in o]
            # An action's sources are its inputs, not compiled files.
            t.sources = set()
            t.public = None
        out[label] = t
        if r.func in MOJOM_TEMPLATES:
            _mojom_variants(t, r, out)
        if r.func == "rust_static_library":
            cxx = _strs(r.scope.vars.get("cxx_bindings"))
            t.outputs = [OUT + "/gen/" + abspath(c, r.path)[2:] + ".h"
                         for c in cxx]
    return out


def _typemap_deps(r, key):
    """(traits_public_deps, traits_deps) of a mojom's cpp_typemaps or
    blink_cpp_typemaps."""
    pub, priv = [], []
    tms = r.scope.vars.get(key)
    for tm in tms if isinstance(tms, list) else []:
        if not isinstance(tm, gnlib.Scope):
            continue
        pub += [x for x in (norm_label(d, r.path) for d in
                            _strs(tm.vars.get("traits_public_deps"))) if x]
        priv += [x for x in (norm_label(d, r.path) for d in
                             _strs(tm.vars.get("traits_deps"))) if x]
    return pub, priv


def _labels(r, key):
    return [x for x in (norm_label(d, r.path)
                        for d in _strs(r.scope.vars.get(key))) if x]


def _mojom_variants(t, r, out):
    """The C++ targets mojom() defines (mojo/public/tools/bindings/
    mojom.gni), with the headers each owns: <name> (chromium variant),
    <name>_blink, <name>_shared and the <variant>_headers groups."""
    get = r.scope.vars.get
    srcs = [abspath(s, r.path) for s in _strs(get("sources"))]
    deps = t.deps + t.public_deps           # mojom_cpp_deps
    all_deps = deps + _labels(r, "mojom_source_deps") + \
        _labels(r, "mojom_blink_source_deps")
    base = t.label
    vis = t.visibility
    full = not (get("disallow_native_types") is True and
                get("disallow_interfaces") is True)
    bindings = "//mojo/public/cpp/bindings:" + (
        "bindings" if full else "bindings_base")
    t.sources = set()
    t.public = None
    t.deps = []
    shared_hdrs = [OUT + "/gen/" + s[2:] + e for s in srcs
                   for e in MOJOM_VARIANTS["_shared"] if e.endswith(".h")]
    for suffix, exts in MOJOM_VARIANTS.items():
        lab = base + suffix
        v = t if suffix == "" else GnTarget(lab, "mojom" + suffix, t.path)
        v.template = "mojom"
        v.testonly = t.testonly
        v.outputs = [OUT + "/gen/" + s[2:] + e for s in srcs for e in exts]
        if suffix == "_shared":
            # <name>_shared (a group or component) forwards to the source
            # set <name>_shared_cpp_sources, which lists the headers.
            v.visibility = None
            src = GnTarget(lab + "_cpp_sources", "mojom_shared", t.path)
            src.template = "mojom"
            src.testonly = t.testonly
            src.outputs = v.outputs
            src.public_deps = [d + "_shared" for d in all_deps] + [bindings]
            out.setdefault(src.label, src)
            v.outputs = []
            v.public_deps = [src.label]
            v.deps = []
        else:
            blink = suffix == "_blink"
            if blink:
                vb = get("visibility_blink")
                v.visibility = [x for x in (norm_pattern(p, r.path)
                                            for p in _strs(vb)) if x] \
                    if isinstance(vb, list) else None
                pub, priv = _typemap_deps(r, "blink_cpp_typemaps")
                pub = pub + ["//mojo/public/cpp/bindings:wtf_support"]
                over, comp = "overridden_deps_blink", "component_deps_blink"
            else:
                v.visibility = vis
                pub, priv = _typemap_deps(r, "cpp_typemaps")
                over, comp = "overridden_deps", "component_deps"
            mdeps = [d + suffix for d in deps]
            if get(over) is not None:
                gone = {d + suffix for d in _labels(r, over)}
                mdeps = [d for d in mdeps if d not in gone] + _labels(r, comp)
            v.public_deps = mdeps + [base + "_shared", "//base:base",
                                     bindings] + pub
            v.deps = priv
            proxy = get("blink_cpp_proxy_target" if blink
                        else "cpp_proxy_target")
            proxy = norm_label(proxy, r.path) if isinstance(proxy, str) \
                else None
            if proxy:
                # The sources move to <variant>_cpp_sources; <variant> is a
                # group that forwards to the proxy (a component).
                src = GnTarget(lab + "_cpp_sources", v.type, t.path)
                for k in ("template", "testonly", "outputs", "public_deps",
                          "deps"):
                    setattr(src, k, getattr(v, k))
                src.visibility = None if v.visibility is None else \
                    v.visibility + [lab]
                out.setdefault(src.label, src)
                v.outputs = []
                v.public_deps = [proxy]
                v.deps = []
            # <variant>_headers: the generators' headers without the
            # sources, and the same group of each mojom dependency.
            hdr = GnTarget(lab + "_headers", "mojom_headers", t.path)
            hdr.template = "mojom"
            hdr.testonly = t.testonly
            hdr.outputs = [OUT + "/gen/" + s[2:] + e for s in srcs
                           for e in exts if e.endswith(".h")] + shared_hdrs
            hdr.public_deps = [d + suffix + "_headers" for d in all_deps]
            if blink:
                hdr.public_deps.append(
                    "//mojo/public/cpp/bindings:wtf_support")
            out.setdefault(hdr.label, hdr)
        if suffix != "" and lab not in out:
            out[lab] = v


# ---------------------------------------------------------------------------
# One platform's build graph


def build_file(d):
    """"//a/b" -> "//a/b/BUILD.gn"; "//" -> "//BUILD.gn"."""
    return "//BUILD.gn" if d in ("//", "/", "") else d.rstrip("/") + \
        "/BUILD.gn"


class Graph:
    """One platform's build graph. load_build() reads what `gn gen` loads:
    //BUILD.gn and, transitively, the BUILD.gn of every label a loaded file
    names in REF_KEYS. gn check's file map comes from those files only (a
    target of a file this configuration never loads is unknown to it, as a
    ChromeOS-only target is on Linux)."""

    def __init__(self, tree, config, extra_args=None):
        platform, variant_args = parse_config(config)
        args = dict(variant_args)
        args.update(extra_args or {})
        self.tree = tree
        self.platform = config          # how findings name it
        self.interp = make_interp(tree, platform, args)
        tc = PLATFORMS[platform]["toolchain"]
        self.toolchains = {tc}
        self.files = {}          # "//dir/BUILD.gn" -> {label: GnTarget}
        self.refs = {}           # "//dir/BUILD.gn" -> {dirs it references}
        self.targets = {}        # label -> GnTarget
        self.owners = {}         # "//file" -> [(label, public, generated)]
        self.errors = {}         # "//dir/BUILD.gn" -> message
        self.asserts = {}        # "//dir/BUILD.gn" -> failed assert message
        self.loaded = None       # BUILD.gn files gn gen loads
        self._permitted = {}
        self.eval_seconds = 0.0

    def load_dir(self, d):
        """Evaluates //d/BUILD.gn once; returns its targets."""
        d = d.rstrip("/")
        if d in ("", "/"):
            d = "//"
        bf = build_file(d)
        if bf in self.files:
            return self.files[bf]
        self.files[bf] = {}
        self.refs[bf] = set()
        if not self.tree.exists(bf[2:]):
            return {}
        t0 = time.time()
        try:
            raw = self.interp.run_file(bf, expand=True)
        except AssertFailed as e:
            self.asserts[bf] = str(e)
            raw = []
        except (RecursionError, TypeError, AttributeError, KeyError,
                ValueError, IndexError, SyntaxError) as e:
            self.errors[bf] = "%s: %s" % (type(e).__name__, e)
            raw = []
        self.eval_seconds += time.time() - t0
        ts = targets_of_file(raw, d)
        self.files[bf] = ts
        refs = referenced_dirs(raw, self.toolchains)
        for lab, t in ts.items():
            self.targets[lab] = t
            if t.template in MOJOM_TEMPLATES:
                refs.update(x.split(":", 1)[0] for x in t.all_deps())
        self.refs[bf] = refs
        return ts

    def load_build(self):
        """The BUILD.gn files gn gen loads, and the file map they make."""
        if self.loaded is not None:
            return self.loaded
        loaded = set()
        queue = ["//"]
        seen = {"//"}
        while queue:
            nxt = []
            for d in queue:
                self.load_dir(d)
                bf = build_file(d)
                if not self.tree.exists(bf[2:]) or bf in self.asserts:
                    continue
                loaded.add(bf)
                for r in self.refs[bf]:
                    if r not in seen:
                        seen.add(r)
                        nxt.append(r)
            queue = nxt
        self.loaded = loaded
        for bf in loaded:
            for lab, t in self.files[bf].items():
                for f, pub, gen in t.files():
                    self.owners.setdefault(f, []).append((lab, pub, gen))
        return loaded

    def cycles(self):
        """Dependency cycles gn gen would refuse: strongly connected
        components of the loaded targets over deps, public_deps and
        data_deps, with more than one target (or a target depending on
        itself). Each is a sorted list of labels."""
        self.load_build()
        nodes = [lab for bf in sorted(self.loaded) for lab in self.files[bf]]
        known = set(nodes)

        def succ(lab):
            t = self.targets[lab]
            return [d for d in t.public_deps + t.deps + t.data_deps
                    if d in known]

        index, low, on, stack, out = {}, {}, set(), [], []
        counter = [0]
        for root in nodes:
            if root in index:
                continue
            work = [(root, iter(succ(root)))]
            index[root] = low[root] = counter[0]
            counter[0] += 1
            stack.append(root)
            on.add(root)
            while work:
                v, it = work[-1]
                advanced = False
                for w in it:
                    if w not in index:
                        index[w] = low[w] = counter[0]
                        counter[0] += 1
                        stack.append(w)
                        on.add(w)
                        work.append((w, iter(succ(w))))
                        advanced = True
                        break
                    if w in on:
                        low[v] = min(low[v], index[w])
                if advanced:
                    continue
                work.pop()
                if work:
                    u = work[-1][0]
                    low[u] = min(low[u], low[v])
                if low[v] == index[v]:
                    comp = []
                    while True:
                        w = stack.pop()
                        on.discard(w)
                        comp.append(w)
                        if w == v:
                            break
                    if len(comp) > 1 or v in succ(v):
                        out.append(sorted(comp))
        return out

    def target(self, label):
        d = label.split(":", 1)[0]
        self.load_dir(d)
        return self.targets.get(label)

    def in_build(self, label):
        return build_file(label.split(":", 1)[0]) in self.load_build()

    def owners_of(self, f):
        """gn check's file map entry for //f: [(label, public, generated)]."""
        self.load_build()
        return self.owners.get(f, [])

    def permitted(self, label):
        """Targets `label` may include from: its direct deps, then their
        public_deps, transitively (gn's IsDependencyOf with
        require_permitted)."""
        if label in self._permitted:
            return self._permitted[label]
        t = self.target(label)
        seen = set()
        queue = list(t.all_deps()) if t else []
        seen.update(queue)
        while queue:
            nxt = []
            for lab in queue:
                dt = self.target(lab)
                if dt is None:
                    continue
                for p in dt.public_deps:
                    if p not in seen:
                        seen.add(p)
                        nxt.append(p)
            queue = nxt
        self._permitted[label] = seen
        return seen


# ---------------------------------------------------------------------------
# Includes, as gn's CIncludeIterator finds them

MAX_NON_INCLUDE_LINES = 10
_WS = " \t"


def _count_toward_limit(line):
    if not line:
        return False
    if line.startswith("//"):
        return False
    if line.startswith("/*") or line.startswith(" *"):
        return False
    if line.startswith("#"):
        return False
    if not line.strip(" \t\n\r\f\v"):
        return False
    return True


def _extract_include(line):
    """(path, system_style) of an #include/#import line, else None."""
    t = line.lstrip(_WS)
    if not t.startswith("#"):
        return None
    t = t[1:].lstrip(_WS)
    if t.startswith("include"):
        c = t[len("include"):].lstrip(_WS)
    elif t.startswith("import"):
        c = t[len("import"):].lstrip(_WS)
    else:
        return None
    if not c or c[0] not in "\"<":
        return None
    term = '"' if c[0] == '"' else ">"
    end = c.find(term, 1)
    if end < 0:
        return None
    return c[1:end], c[0] == "<"


def scan_includes(text):
    """[(line number, include, system_style)] the way `gn check` reads a
    file: it stops after more than ten lines that are neither an include,
    a comment, a directive nor blank, and skips lines with `nogncheck`."""
    out = []
    since = 0
    for n, line in enumerate(text.split("\n"), 1):
        if since > MAX_NON_INCLUDE_LINES:
            break
        line = line.rstrip("\r")
        inc = _extract_include(line)
        if "nogncheck" in line:
            continue
        if inc is not None:
            out.append((n, inc[0], inc[1]))
            since = 0
            continue
        if _count_toward_limit(line):
            since += 1
    return out


def all_includes(text):
    """Every quoted include of a file (no line limit), for telling which
    includes a MoltBrowser edit added."""
    out = []
    for line in text.split("\n"):
        inc = _extract_include(line.rstrip("\r"))
        if inc is not None and not inc[1]:
            out.append(inc[0])
    return out


# ---------------------------------------------------------------------------
# Findings


class Finding:
    def __init__(self, kind, severity, where, message, hint=""):
        self.kind = kind            # includes, deps, sources, unlisted, eval
        self.severity = severity    # error, warning
        self.where = where          # "path:line" or a label
        self.message = message
        self.hint = hint
        self.platforms = []

    def key(self):
        return (self.kind, self.severity, self.where, self.message)

    def to_json(self):
        return {"kind": self.kind, "severity": self.severity,
                "where": self.where, "message": self.message,
                "hint": self.hint, "platforms": self.platforms}

    def __str__(self):
        s = "%s %s [%s] %s: %s" % (self.severity.upper(), self.kind,
                                   ",".join(self.platforms), self.where,
                                   self.message)
        if self.hint:
            s += "\n    fix: " + self.hint
        return s


class Findings:
    def __init__(self):
        self.by_key = {}

    def add(self, f, platform):
        k = f.key()
        if k not in self.by_key:
            self.by_key[k] = f
        if platform not in self.by_key[k].platforms:
            self.by_key[k].platforms.append(platform)

    def all(self):
        return sorted(self.by_key.values(),
                      key=lambda f: (f.severity != "error", f.kind, f.where))

    def errors(self):
        return [f for f in self.all() if f.severity == "error"]


# ---------------------------------------------------------------------------
# The include rule


def check_file_includes(graph, rel, text, only, dot, findings):
    """gn check's rule for every target that lists //rel. `only` limits the
    checked includes to that set of include strings (None: all). Returns
    whether a target lists the file."""
    f = "//" + rel
    from_targets = [lab for lab, _, gen in graph.owners_of(f) if not gen]
    if not from_targets:
        return False
    file_dir = f.rsplit("/", 1)[0]
    incs = [(n, inc) for n, inc, system in scan_includes(text) if not system]
    for lab in sorted(set(from_targets)):
        t = graph.target(lab)
        if t is None or not t.check_includes:
            continue
        if any(label_matches(p, lab) for p in dot["no_check_targets"]):
            continue
        for n, inc in incs:
            if only is not None and inc not in only:
                continue
            _check_one(graph, t, f, file_dir, n, inc, findings)
    return True


def resolve_include(graph, file_dir, inc):
    """The file gn's file map has for a quoted include, or None: relative to
    the including file's directory first, then `//` and the gen root."""
    for cand in (abspath(inc, file_dir), "//" + inc.lstrip("/"),
                 GEN + inc.lstrip("/")):
        if cand.startswith("//") and graph.owners_of(cand):
            return cand
    return None


def _check_one(graph, t, f, file_dir, n, inc, findings):
    target_file = resolve_include(graph, file_dir, inc)
    if target_file is None:
        return
    owners = graph.owners_of(target_file)
    labels = [o[0] for o in owners]
    if t.label in labels:
        return
    permitted = graph.permitted(t.label)
    private_in = []
    for lab, public, _gen in owners:
        to = graph.target(lab)
        if lab in permitted:
            if public or (to and any(label_matches(p, t.label)
                                     for p in to.friend)):
                return
            private_in.append(lab)
        elif to is not None and t.label in to.allow_circular:
            return
    where = "%s:%d" % (f[2:], n)
    if private_in:
        findings.add(Finding(
            "includes", "error", where,
            "%s includes \"%s\", which is private to %s" % (
                t.label, inc, ", ".join(sorted(set(private_in)))),
            "list it in that target's `public`, or name %s in its `friend`"
            % t.label), graph.platform)
        return
    findings.add(Finding(
        "includes", "error", where,
        "%s includes \"%s\" (in %s) without a dependency path to it" % (
            t.label, inc, ", ".join(sorted(set(labels)))),
        "add one of them to %s's deps (or reach it through public_deps)"
        % t.label), graph.platform)


# ---------------------------------------------------------------------------
# Dependencies, as gn gen resolves them


def check_target_deps(graph, t, new_edges, new_circular, dot, findings):
    for dep in new_edges:
        d = graph.target(dep)
        where = t.label
        if d is None:
            ddir = dep.split(":", 1)[0]
            if ddir.startswith("//third_party/rust/") and \
                    not graph.tree.exists(build_file(ddir)[2:]):
                # Crates scripts/vendor-rust-crates.sh vendors on the build
                # machine (BUILD_MACHINE_CHECKLIST.md section 1).
                findings.add(Finding(
                    "deps", "warning", where,
                    "depends on %s, a crate vendored on the build machine "
                    "(scripts/vendor-rust-crates.sh)" % dep), graph.platform)
                continue
            name = dep.split(":", 1)[1]
            maker = [x for x in graph.load_dir(ddir).values()
                     if x.template and name.startswith(
                         x.label.split(":", 1)[1] + "_")]
            if maker:
                # <name>_js, <name>__proc_macro, ...: a target a template
                # defines next to its own, which this checker doesn't run.
                findings.add(Finding(
                    "deps", "warning", where,
                    "depends on %s, which is not modelled (%s() %s may "
                    "define it)" % (dep, maker[0].template, maker[0].label)),
                    graph.platform)
                continue
            findings.add(Finding(
                "deps", "error", where,
                "depends on %s, which is not defined" % dep,
                "fix the label (or the condition around the target)"),
                graph.platform)
            continue
        if d.visibility is not None and not any(
                label_matches(p, t.label) for p in d.visibility):
            findings.add(Finding(
                "deps", "error", where,
                "depends on %s, whose visibility does not include it "
                "(visibility = %s)" % (dep, ", ".join(d.visibility)),
                "add %s to %s's visibility, or depend on a visible target"
                % (t.label, dep)), graph.platform)
        if d.testonly and not t.testonly:
            findings.add(Finding(
                "deps", "error", where,
                "is not testonly but depends on the testonly %s" % dep,
                "mark it testonly or drop the dep"), graph.platform)
    deps = set(t.deps) | set(t.public_deps)
    for c in new_circular:
        if c not in deps:
            findings.add(Finding(
                "deps", "error", t.label,
                "allow_circular_includes_from names %s, which is not one "
                "of its deps" % c, "add it to deps"), graph.platform)
    allow = dot["circular_allowlist"]
    if new_circular and allow is not None and not any(
            label_matches(p, t.label) for p in allow):
        findings.add(Finding(
            "deps", "error", t.label,
            "uses allow_circular_includes_from but is not on //.gn's "
            "allow_circular_includes_from_allowlist",
            "move the entries to an allowlisted target"), graph.platform)


# ---------------------------------------------------------------------------
# The overlay: what MoltBrowser adds or changes

# MoltBrowser's own files that should be in a target: not the vendored
# third-party trees (llama.cpp's own sources, the Rust crates), nor files
# of DEPS subprojects (built from their own lists).
OWN_EXCLUDE = ("third_party/rust/", "third_party/llama_cpp/src/",
               "third_party/llama_cpp/ggml/", "third_party/llama_cpp/common/",
               "third_party/llama_cpp/tools/", "third_party/llama_cpp/include/",
               "third_party/llama_cpp/vendor/", "third_party/skia/", "v8/",
               # molt-speech's and molt-voice's engines and main: CMake
               # only (scripts/bundle-speech.sh), they link sherpa-onnx.
               "chrome/browser/molt_ai/voice/speech_helper/standalone/",
               "chrome/browser/molt_ai/voice/voice_helper/standalone/")
# Files no desktop platform builds.
OTHER_PLATFORM = re.compile(
    r"(^|/)(android|ios|chromeos|fuchsia)/|_(android|ios|chromeos|fuchsia)"
    r"(_unittest)?\.(cc|h|mm)$|/java/")


class Overlay:
    """The src/ overlay against the tree it sits on."""

    def __init__(self, src_dir, files, tree, upstream_read=None):
        self.src = Path(src_dir)
        self.files = files                # paths relative to src/
        self.tree = tree
        self._upstream_read = upstream_read
        self._up = {}

    def is_upstream(self, rel):
        """Whether Chromium (or a DEPS subproject) has this file."""
        if self.tree.paths is not None:
            return rel in self.tree.paths
        return (self.tree.root / rel).exists()

    def upstream_bytes(self, rel):
        if rel not in self._up:
            p = self.tree.root / rel
            data = p.read_bytes() if p.is_file() else None
            if data is None and self._upstream_read is not None:
                data = self._upstream_read(rel)
            self._up[rel] = data
        return self._up[rel]

    def text(self, rel):
        return (self.src / rel).read_text(errors="replace")


def include_plan(overlay, only_files=None):
    """[(rel, text, only)] for the include check: each C++ file the overlay
    adds (only=None: every include) or changes (only: the includes it
    added). Files of a DEPS subproject and the vendored trees are left out
    (their includes go through their own include_dirs)."""
    plan = []
    for rel in overlay.files:
        if not rel.endswith(CHECKED_EXT):
            continue
        if only_files is not None and rel not in only_files:
            continue
        if rel.startswith(OWN_EXCLUDE) and not rel.startswith(
                "third_party/llama_cpp/chromium/"):
            if not overlay.is_upstream(rel):
                continue
        text = overlay.text(rel)
        if overlay.is_upstream(rel):
            base = overlay.upstream_bytes(rel)
            if base is None:
                plan.append((rel, text, None))
                continue
            base = base.decode("utf-8", "replace")
            if base == text:
                continue
            added = set(all_includes(text)) - set(all_includes(base))
            if added:
                plan.append((rel, text, added))
        else:
            plan.append((rel, text, None))
    return plan


def own_files(overlay):
    """MoltBrowser's own C++ files that belong in some desktop target."""
    out = []
    for rel in overlay.files:
        if not rel.endswith(CXX_EXT) or overlay.is_upstream(rel):
            continue
        if rel.startswith(OWN_EXCLUDE) and not rel.startswith(
                "third_party/llama_cpp/chromium/"):
            continue
        if OTHER_PLATFORM.search(rel):
            continue
        out.append(rel)
    return out


def overlay_build_files(overlay):
    return [rel for rel in overlay.files
            if rel.rsplit("/", 1)[-1] == "BUILD.gn"
            and not rel.startswith("third_party/rust/")]


# ---------------------------------------------------------------------------
# Running the checks


def run(tree, overlay, platforms, only_files=None, extra_args=None,
        upstream_tree=None, progress=log):
    """All checks; returns (Findings, stats)."""
    dot = dotfile_settings(tree)
    findings = Findings()
    plan = include_plan(overlay, only_files)
    own = set(own_files(overlay)) if only_files is None else set()
    listed = set()
    bfiles = overlay_build_files(overlay) if only_files is None else []
    stats = {"include_files": len(plan), "build_files": len(bfiles),
             "platforms": {}}
    for p in platforms:
        t0 = time.time()
        g = Graph(tree, p, extra_args)
        ug = Graph(upstream_tree, p, extra_args) if upstream_tree else None
        for i, (rel, text, only) in enumerate(plan):
            if check_file_includes(g, rel, text, only, dot, findings):
                listed.add(rel)
            if progress and i and i % 200 == 0:
                progress("  %s: %d/%d files, %d BUILD.gn read" % (
                    p, i, len(plan), len(g.files)))
        for rel in sorted(own - listed):
            if g.owners_of("//" + rel):
                listed.add(rel)
        for bf in bfiles:
            check_build_file(g, ug, overlay, bf, dot, findings)
        if only_files is None:
            # Upstream's graph has none; one it shows too comes from this
            # checker's approximations (a host-toolchain edge), not ours.
            upstream_cycles = {tuple(c) for c in ug.cycles()} if ug else set()
            for comp in g.cycles():
                if tuple(comp) in upstream_cycles:
                    continue
                findings.add(Finding(
                    "cycles", "error", comp[0],
                    "dependency cycle: " + " -> ".join(
                        cycle_path(g, comp)),
                    "drop one of these deps (allow_circular_includes_from "
                    "on an allowlisted target covers includes, not deps)"),
                    p)
        for bf, err in sorted(g.errors.items()):
            if overlay.src.joinpath(bf[2:]).is_file():
                findings.add(Finding("eval", "warning", bf,
                                     "could not be read fully: " + err),
                             p)
        stats["platforms"][p] = {"build_files_read": len(g.files),
                                 "seconds": round(time.time() - t0, 1)}
        if progress:
            progress("%s: %d BUILD.gn files read in %.0f s" % (
                p, len(g.files), time.time() - t0))
    if only_files is None and \
            {parse_config(p)[0] for p in platforms} >= set(PLATFORMS):
        for rel in sorted(own - listed):
            findings.add(Finding(
                "unlisted", "warning", rel,
                "is in no target's sources in any configuration checked "
                "(gn check skips includes of it)",
                "list it in the target that includes or compiles it"),
                "all")
    return findings, stats


def cycle_path(g, comp):
    """One cycle through comp[0] inside the component, as labels."""
    members = set(comp)
    start = comp[0]
    parent = {}
    queue = [start]
    while queue:
        nxt = []
        for lab in queue:
            t = g.targets[lab]
            for d in t.public_deps + t.deps + t.data_deps:
                if d not in members:
                    continue
                if d == start:
                    path = [lab]
                    while path[-1] != start:
                        path.append(parent[path[-1]])
                    return [start] + list(reversed(path))[1:] + [start] \
                        if len(path) > 1 else [start, start]
                if d not in parent:
                    parent[d] = lab
                    nxt.append(d)
        queue = nxt
    return comp


def check_build_file(g, ug, overlay, bf, dot, findings):
    """gn gen's checks for what an overlay BUILD.gn adds, if this
    configuration loads it."""
    d = "//" + bf.rsplit("/", 1)[0]
    if build_file(d) not in g.load_build():
        return
    ts = g.load_dir(d)
    patched = overlay.is_upstream(bf)
    uts = ug.load_dir(d) if (patched and ug is not None) else {}
    if patched and ug is None:
        return
    for lab, t in sorted(ts.items()):
        if t.template == "mojom" and t.type != "mojom":
            continue
        u = uts.get(lab)
        old_edges = set(u.all_deps()) if u else set()
        new_edges = [x for x in t.all_deps() if x not in old_edges]
        old_circ = set(u.allow_circular) if u else set()
        new_circ = [x for x in t.allow_circular if x not in old_circ]
        check_target_deps(g, t, new_edges, new_circ, dot, findings)
        old_src = (u.sources | (u.public or set())) if u else set()
        for s in sorted((t.sources | (t.public or set())) - old_src):
            if s.startswith(OUT + "/") or not s.startswith("//"):
                continue
            if not g.tree.exists(s[2:]):
                findings.add(Finding(
                    "sources", "error", lab,
                    "lists %s, which does not exist" % s,
                    "fix the path or add the file"), g.platform)


# ---------------------------------------------------------------------------
# fetch: Chromium's GN files from the official source tarball


def fetch(version, dest, overlay_files, log=log, opener=None, pins=None):
    """Streams chromium-<version>-lite.tar.xz once and keeps every BUILD.gn,
    .gn, .gni and DEPS file, //chrome/VERSION and build/util (gn runs
    version.py), checkdeps, and the upstream copy of every overlay path;
    writes paths.txt (every file in the tarball). Kept only if the whole
    tarball matches its pinned SHA-256 (ccheck/tarball.py: PINNED_SHA256,
    or `pins`): checkdeps, the DEPS files it reads and version.py run
    here."""
    url = BUCKET + "chromium-%s-lite.tar.xz" % version
    expected = cctarball.pinned_sha256(url, pins)
    dest = Path(dest)
    tmp = dest.with_name(dest.name + ".partial")
    shutil.rmtree(tmp, ignore_errors=True)
    (tmp / "tree").mkdir(parents=True)
    top = "chromium-%s/" % version
    keep_prefix = ("buildtools/checkdeps/", "build/util/")
    want = set(overlay_files)
    t0 = time.time()
    log("fetch: streaming %s (about 5 minutes)" % url)
    n = kept = 0
    opener = opener or urllib.request.urlopen
    with open(tmp / "paths.txt", "w") as paths, opener(url) as r:
        hashed = cctarball.HashingReader(r)
        with tarfile.open(fileobj=hashed, mode="r|xz") as tf:
            for m in tf:
                n += 1
                if not m.name.startswith(top):
                    continue
                rel = m.name[len(top):]
                if cctarball.safe_relpath(rel) is None:
                    continue
                if m.isfile() or m.issym():
                    paths.write(rel + "\n")
                if not m.isfile():
                    continue
                base = rel.rsplit("/", 1)[-1]
                if not (base.endswith((".gn", ".gni")) or base == "DEPS" or
                        rel == "chrome/VERSION" or rel.startswith(keep_prefix)
                        or rel in want):
                    continue
                out = tmp / "tree" / rel
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(tf.extractfile(m).read())
                kept += 1
        got = hashed.drain()
    if got != expected:
        shutil.rmtree(tmp, ignore_errors=True)
        raise cctarball.TarballNotVerified(
            "%s: SHA-256 %s, not the pinned %s" % (url, got, expected))
    (tmp / "version.txt").write_text(version + "\n")
    shutil.rmtree(dest, ignore_errors=True)
    tmp.rename(dest)
    log("fetch: %d files in the tarball, %d kept, %.0f s" % (
        n, kept, time.time() - t0))
    return dest


# ---------------------------------------------------------------------------
# checkdeps: Chromium's own DEPS checker over the overlay

CHECKDEPS_EXT = (".h", ".cc", ".cpp", ".m", ".mm")


def checkdeps_plan(overlay):
    """{rel: None (every include) or the set of includes MoltBrowser added}
    for the C++ files checkdeps should see: what the overlay adds, and the
    upstream files it changes (the vendored trees left out, as for the
    include check)."""
    plan = {}
    for rel in overlay.files:
        if not rel.endswith(CHECKDEPS_EXT):
            continue
        # The vendored trees, and DEPS subprojects (Skia, V8: their own
        # repositories, which Chromium's checkdeps doesn't read).
        if rel.startswith(OWN_EXCLUDE) and not rel.startswith(
                "third_party/llama_cpp/chromium/"):
            continue
        text = overlay.text(rel)
        if overlay.is_upstream(rel):
            base = overlay.upstream_bytes(rel)
            if base is not None:
                base = base.decode("utf-8", "replace")
                if base == text:
                    continue
                added = set(all_includes(text)) - set(all_includes(base))
                if not added:
                    continue
                plan[rel] = added
                continue
        plan[rel] = None
    return plan


# DEPS subprojects whose include paths start at their own root (WebRTC's
# "modules/...", "rtc_base/..."). Their files are checked as their own
# repositories' presubmits check them: in a run of their own, rooted there,
# with their DEPS files only. Chromium's checkdeps doesn't read them.
CHECKDEPS_SUBPROJECTS = ("third_party/webrtc/",)


def checkdeps_root_of(rel):
    """The subproject prefix a file's checkdeps run is rooted at ("" for
    Chromium's root)."""
    for prefix in CHECKDEPS_SUBPROJECTS:
        if rel.startswith(prefix):
            return prefix
    return ""


def run_checkdeps(tree_root, overlay, work, log=log):
    """Runs the tree's buildtools/checkdeps/checkdeps.py over a copy of the
    tree's DEPS files with the overlay's on top and the overlay's C++ files
    (checkdeps reads only those): once at Chromium's root, and once at the
    root of each subproject with files (CHECKDEPS_SUBPROJECTS). Returns
    (findings, number of files)."""
    tree_root = Path(tree_root)
    script = tree_root / "buildtools" / "checkdeps" / "checkdeps.py"
    if not script.is_file():
        raise RuntimeError("no %s (run fetch)" % script)
    work = Path(work)
    shutil.rmtree(work, ignore_errors=True)
    plan = checkdeps_plan(overlay)
    findings = Findings()
    prefixes = sorted({checkdeps_root_of(rel) for rel in plan} | {""})
    for prefix in prefixes:
        group = {rel: only for rel, only in plan.items()
                 if checkdeps_root_of(rel) == prefix}
        sub = work / prefix.strip("/").replace("/", "_") if prefix else work
        _checkdeps_once(script, tree_root, overlay, prefix, group, sub,
                        findings)
    return findings, len(plan)


def _checkdeps_once(script, tree_root, overlay, prefix, plan, work,
                    findings):
    """One checkdeps run rooted at `prefix` ("" or a subproject's
    directory) over the files of `plan` (paths under src/, all under
    `prefix`)."""
    root = work / "src"
    root.mkdir(parents=True)
    base = tree_root / prefix if prefix else tree_root
    for p in base.rglob("DEPS"):
        if not p.is_file():
            continue
        rel = p.relative_to(base).as_posix()
        if not prefix and checkdeps_root_of(rel):
            continue  # read in the subproject's own run
        dst = root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(p, dst)
    for rel in list(overlay.files):
        if checkdeps_root_of(rel) != prefix:
            continue
        if rel.rsplit("/", 1)[-1] == "DEPS" or rel in plan:
            dst = root / rel[len(prefix):]
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(overlay.src / rel, dst)
    # checkdeps wants a repository root, and outside git it walks only the
    # directories that have a .svn entry (instead of asking git ls-files).
    for dirpath, dirnames, _ in os.walk(root):
        if ".svn" not in dirnames:
            os.mkdir(os.path.join(dirpath, ".svn"))
        dirnames[:] = [x for x in dirnames if x != ".svn"]
    out = work / "checkdeps.json"
    r = subprocess.run([sys.executable, "-E", "-s", str(script),
                        "--root", str(root), "--json", str(out), "-s"],
                       cwd=str(work), capture_output=True, text=True)
    if not out.is_file():
        raise RuntimeError("checkdeps failed (%d): %s" % (
            r.returncode, (r.stdout + r.stderr)[-2000:]))
    for res in json.loads(out.read_text()):
        rel = prefix + os.path.relpath(res["dependee_path"], str(root))
        only = plan.get(rel)
        for v in res["violations"]:
            inc = v["include_path"]
            if only is not None and inc not in only:
                continue
            rule = v.get("violated_rule") or ["", "", ""]
            findings.add(Finding(
                "checkdeps", "error", rel,
                "includes \"%s\", which the DEPS rules there don't allow "
                "(%s)" % (inc, " ".join(str(x) for x in rule if x)),
                "add a +rule to the nearest MoltBrowser DEPS (or a "
                "specific_include_rules entry for this file)"), "deps")


# ---------------------------------------------------------------------------
# Blink's audit_non_blink_usage.py over MoltBrowser's Blink lines

BLINK_AUDIT = "third_party/blink/tools/blinkpy/presubmit/audit_non_blink_usage.py"


def added_lines(overlay, rel):
    """[(line number, line)] MoltBrowser adds to a file (every line of a
    file it adds), as a presubmit's ChangedContents() gives them."""
    import difflib
    lines = overlay.text(rel).split("\n")
    if not overlay.is_upstream(rel):
        return [(i + 1, x) for i, x in enumerate(lines)]
    base = overlay.upstream_bytes(rel)
    if base is None:
        return [(i + 1, x) for i, x in enumerate(lines)]
    base = base.decode("utf-8", "replace").split("\n")
    out = []
    for op, _, _, j1, j2 in difflib.SequenceMatcher(
            None, base, lines, autojunk=False).get_opcodes():
        if op in ("replace", "insert"):
            out += [(j + 1, lines[j]) for j in range(j1, j2)]
    return out


def run_blink_audit(overlay, upstream_read=None):
    """Blink's presubmit check (audit_non_blink_usage.check()) over the
    lines MoltBrowser adds in third_party/blink: the overlay's copy of the
    script (with MoltBrowser's entries) if there is one, else upstream's."""
    import importlib.util
    import tempfile
    src = overlay.src / BLINK_AUDIT
    tmp = None
    if not src.is_file():
        data = upstream_read(BLINK_AUDIT) if upstream_read else None
        if data is None:
            raise RuntimeError("no %s" % BLINK_AUDIT)
        tmp = tempfile.NamedTemporaryFile(suffix=".py", delete=False)
        tmp.write(data)
        tmp.close()
        src = Path(tmp.name)
    try:
        spec = importlib.util.spec_from_file_location("molt_blink_audit",
                                                      str(src))
        audit = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(audit)
    finally:
        if tmp is not None:
            os.unlink(tmp.name)
    findings = Findings()
    n = 0
    for rel in overlay.files:
        if not rel.startswith("third_party/blink/") or \
                not rel.endswith((".cc", ".cpp", ".h", ".mm")):
            continue
        lines = added_lines(overlay, rel)
        if not lines:
            continue
        n += 1
        for bad in audit.check(rel, lines) or []:
            findings.add(Finding(
                "blink-audit", "warning" if bad.warning else "error",
                "%s:%d" % (rel, bad.line),
                "uses %s%s" % (bad.identifier,
                               ": " + " ".join(bad.advice)
                               if bad.advice else ""),
                "follow the advice, or add a MoltBrowser entry to %s"
                % BLINK_AUDIT), "blink")
    return findings, n


# ---------------------------------------------------------------------------
# tests: MoltBrowser's test sources no test binary builds, and the data they
# open

# The binaries that run each kind of MoltBrowser test, as
# docs/BUILD_MACHINE_CHECKLIST.md §5 runs them: its own unit tests in
# molt_ai_unittests (or Chrome's unit_tests), the ones that need Blink's or
# media's test environment in that suite (§5.1 runs them by name), its
# browser tests in browser_tests (or interactive_ui_tests), and its fuzzers
# under molt_fuzzers (tools/fuzz/molt_fuzz.py, docs/FUZZING.md). A test in
# another suite is built but not run by the checklist: add the suite here
# and to §5 if that is meant.
TEST_BINARIES = {
    "unit": ("//chrome/browser/molt_ai/test:molt_ai_unittests",
             "//chrome/test:unit_tests",
             "//third_party/blink/renderer/controller:blink_unittests",
             "//media:media_unittests"),
    "browser": ("//chrome/test:browser_tests",),
    "interactive": ("//chrome/test:interactive_ui_tests",),
    "fuzzer": ("//chrome/browser/molt_ai/fuzzers:molt_fuzzers",),
}
FUZZER_TEMPLATES = {"fuzzer_test"}
LINK_KEYS = ("public_deps", "deps")                 # compiled into it
RUNTIME_KEYS = ("public_deps", "deps", "data_deps")  # gn's runtime deps

_TEST_MACRO = re.compile(r"^[ \t]*(?:TEST|TEST_F|TEST_P|TYPED_TEST|"
                         r"TYPED_TEST_P|FUZZ_TEST)\s*\(", re.M)
_BROWSER_TEST_MACRO = re.compile(r"^[ \t]*IN_PROC_BROWSER_TEST(?:_F|_P)?\s*\(",
                                 re.M)


def test_kind(rel, text):
    """"unit", "browser", "interactive", "fuzzer" or None for a C++ file,
    from its name and the test macros it uses."""
    base = rel.rsplit("/", 1)[-1]
    stem = base.rsplit(".", 1)[0]
    if stem.endswith("_fuzzer") or "LLVMFuzzerTestOneInput" in text:
        return "fuzzer"
    if stem.endswith("_interactive_uitest"):
        return "interactive"
    if stem.endswith("_browsertest") or _BROWSER_TEST_MACRO.search(text):
        return "browser"
    if stem.endswith("_unittest") or _TEST_MACRO.search(text):
        return "unit"
    return None


def test_sources(overlay):
    """{rel: kind} for MoltBrowser's own test sources (as own_files: not
    the vendored trees, not another platform's files)."""
    out = {}
    for rel in own_files(overlay):
        if not rel.endswith((".cc", ".mm", ".cpp")):
            continue
        kind = test_kind(rel, overlay.text(rel))
        if kind:
            out[rel] = kind
    return out


def closure(graph, root, keys):
    """Every loaded target reachable from `root` through `keys`, with it."""
    seen = {root}
    queue = [root]
    while queue:
        nxt = []
        for lab in queue:
            t = graph.targets.get(lab)
            if t is None:
                continue
            for k in keys:
                for d in getattr(t, k):
                    if d not in seen:
                        seen.add(d)
                        nxt.append(d)
        queue = nxt
    return seen


class TestPlacement:
    """Where one test source is listed and which binaries build it, per
    configuration."""

    def __init__(self, rel, kind):
        self.rel = rel
        self.kind = kind
        self.owners = {}      # label -> [configs]
        self.binaries = {}    # binary label -> [configs]
        self.static = set()   # labels of static libraries that list it
        self.fuzzer_targets = set()  # labels of fuzzer_test()s that list it

    def run_in(self):
        """[(config, binary)] where an expected binary builds it."""
        want = TEST_BINARIES[self.kind]
        return [(c, b) for b in want for c in self.binaries.get(b, [])]


def _add(d, k, v):
    lst = d.setdefault(k, [])
    if v not in lst:
        lst.append(v)


def place_tests(graph, tests, placements):
    """Fills `placements` ({rel: TestPlacement}) for one configuration."""
    graph.load_build()
    reach = {}
    for kind in sorted({k for k in tests.values()}):
        for b in TEST_BINARIES[kind]:
            if b not in reach and b in graph.targets:
                reach[b] = closure(graph, b, LINK_KEYS)
    # Other test binaries, to say where a misplaced test runs instead.
    others = [lab for lab, t in graph.targets.items()
              if t.template == "test" and lab not in reach]
    for rel, kind in sorted(tests.items()):
        p = placements.setdefault(rel, TestPlacement(rel, kind))
        owners = [lab for lab, _, gen in graph.owners_of("//" + rel)
                  if not gen]
        for lab in owners:
            _add(p.owners, lab, graph.platform)
            t = graph.targets.get(lab)
            if t is not None and t.type == "static_library":
                p.static.add(lab)
            if t is not None and (t.template or t.type) in FUZZER_TEMPLATES:
                p.fuzzer_targets.add(lab)
        for b, members in reach.items():
            if any(o in members for o in owners):
                _add(p.binaries, b, graph.platform)
        if owners and not p.run_in():
            for b in others:
                if any(o in closure_cache(graph, b) for o in owners):
                    _add(p.binaries, b, graph.platform)
    return reach


def closure_cache(graph, root):
    cache = graph.__dict__.setdefault("_link_closures", {})
    if root not in cache:
        cache[root] = closure(graph, root, LINK_KEYS)
    return cache[root]


def _kind_hint(kind):
    return {
        "unit": "list it in the unit_tests source_set of its directory's "
                "BUILD.gn, one that //chrome/browser/molt_ai/test:"
                "molt_ai_unittests depends on (chrome/browser/molt_ai/test/"
                "BUILD.gn)",
        "browser": "list it in a browser_tests source_set that "
                   "//chrome/browser/molt_ai/voice:browser_tests reaches "
                   "(//chrome/browser:core_browser_tests depends on it)",
        "interactive": "list it in a source_set that "
                       "//chrome/test:interactive_ui_tests reaches",
        "fuzzer": "make it a fuzzer_test() and add it to "
                  "//chrome/browser/molt_ai/fuzzers:molt_fuzzers (and to "
                  "tools/fuzz/molt_fuzz.py)",
    }[kind]


def test_findings(placements, configs, findings):
    """The orphans: a test source no expected binary builds in any
    configuration checked."""
    where_all = ",".join(configs)
    for rel, p in sorted(placements.items()):
        for lab in sorted(p.static):
            findings.add(Finding(
                "tests", "warning", rel,
                "is in the static library %s: the linker keeps only the "
                "objects something references, so its tests may not run"
                % lab, "list it in a source_set"), where_all)
        if p.run_in():
            continue
        if p.owners and set(p.owners) <= p.fuzzer_targets:
            continue    # fuzzer_target_findings() names the target
        want = ", ".join(TEST_BINARIES[p.kind])
        if not p.owners:
            msg = "is in no target's sources in any configuration " \
                  "checked (no target), so no test binary builds it"
        else:
            listed = "; ".join("%s (%s)" % (lab, ",".join(cs))
                               for lab, cs in sorted(p.owners.items()))
            if p.binaries:
                msg = "is listed in %s, which only %s builds; MoltBrowser's " \
                      "%s tests run in %s" % (
                          listed, ", ".join(sorted(p.binaries)), p.kind,
                          want)
            else:
                msg = "is listed in %s, which no test binary reaches " \
                      "(expected: %s)" % (listed, want)
        hint = _kind_hint(p.kind)
        if set(configs) != set(DEFAULT_CONFIGS.split(",")):
            hint += " (or it is built only in a configuration not checked " \
                    "here: run without --platform)"
        findings.add(Finding("tests", "error", rel, msg, hint), where_all)


def fuzzer_target_findings(graph, overlay, ugraph, reach, findings):
    """Every fuzzer_test() an overlay BUILD.gn adds is reached from
    molt_fuzzers (one configuration)."""
    root = TEST_BINARIES["fuzzer"][0]
    members = reach.get(root) or closure(graph, root, LINK_KEYS)
    for bf in overlay_build_files(overlay):
        d = "//" + bf.rsplit("/", 1)[0]
        if build_file(d) not in graph.load_build():
            continue
        if overlay.is_upstream(bf) and ugraph is None:
            continue        # can't tell upstream's fuzzers from ours
        old = ugraph.load_dir(d) if overlay.is_upstream(bf) else {}
        for lab, t in sorted(graph.load_dir(d).items()):
            if (t.template or t.type) not in FUZZER_TEMPLATES or \
                    lab in old or lab in members:
                continue
            findings.add(Finding(
                "tests", "error", lab,
                "is a fuzzer_test() that %s doesn't reach, so no fuzzing "
                "build builds it" % root,
                "add it to the deps of %s (or of a group it reaches, such "
                "as :fuzzers next to it)" % root), graph.platform)


# --- Test data -------------------------------------------------------------

# PathService keys a test reads files under, and what each root means here:
# "src" (a path in the source tree), "gen" (under root_gen_dir), "exe" (a
# file next to the test binary: an executable a data_deps builds).
PATH_KEYS = {
    "DIR_SRC_TEST_DATA_ROOT": "src", "DIR_SOURCE_ROOT": "src",
    "DIR_GEN_TEST_DATA_ROOT": "gen", "DIR_EXE": "exe", "DIR_MODULE": "exe",
}
# chrome::DIR_TEST_DATA and browser tests' embedded_test_server() (which
# InProcessBrowserTest points at chrome/test/data).
CHROME_TEST_DATA = "chrome/test/data/"
# File names a test names on their own, joined to a directory elsewhere in
# the file (Fixture("youtube_home.html"), StringsPak("x_en-US.pak")).
FIXTURE_EXT = ("html", "htm", "xhtml", "webm", "mp4", "ogv", "ogg", "wav",
               "mp3", "pak", "pdf", "png", "jpg", "jpeg", "gif", "svg",
               "webp", "txt")
# What a fixture page loads from its own server.
PAGE_EXT = ("html", "htm", "js", "css", "svg")
_FIXTURE_NAME = re.compile(r"^[A-Za-z0-9_.\-]+\.(?:%s)$" %
                           "|".join(FIXTURE_EXT))
_SERVED = re.compile(r"^/[A-Za-z0-9_.\-/%~]*$")


def cxx_literals(text):
    """(code, literals): the source with comments and raw strings blanked
    (offsets and lines kept), and [(line, start, end, value)] of its
    ordinary string literals."""
    out = list(text)
    lits = []
    i, n, line = 0, len(text), 1

    def blank(a, b):
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += text.count("\n", i, j)
            blank(i, j)
            i = j
        elif c == "R" and text.startswith('R"', i) and \
                (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")
                 or text[i - 1] in "uU8L"):
            k = text.find("(", i + 2)
            delim = text[i + 2:k] if k >= 0 else None
            if delim is None or len(delim) > 16 or "\n" in delim:
                i += 1
                continue
            j = text.find(")" + delim + '"', k)
            j = n if j < 0 else j + len(delim) + 2
            line += text.count("\n", i, j)
            blank(i, j)
            i = j
        elif c == '"':
            j = i + 1
            buf = []
            while j < n and text[j] != '"' and text[j] != "\n":
                if text[j] == "\\" and j + 1 < n:
                    buf.append(text[j + 1] if text[j + 1] in "\"'\\?"
                               else text[j:j + 2])
                    j += 2
                    continue
                buf.append(text[j])
                j += 1
            lits.append((line, i, j + 1, "".join(buf)))
            i = j + 1
        elif c == "'" and not (i and (text[i - 1].isalnum() or
                                      text[i - 1] == "_")):
            j = i + 1
            while j < n and text[j] != "'" and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        else:
            i += 1
    return "".join(out), lits


class DataRef:
    """A file or directory a test opens: kind "src" (//path), "gen" (under
    root_gen_dir), "exe" (an executable next to the binary), "served" (a
    path its test server serves from chrome/test/data) or "bare" (a file
    name to find in one of `dirs`)."""

    def __init__(self, kind, line, path, dirs=None):
        self.kind = kind
        self.line = line
        self.path = path            # a directory ends in "/"
        self.dirs = dirs or []      # for "bare": [(kind, dir)]

    def __repr__(self):
        return "<%s %s:%d>" % (self.kind, self.path, self.line)


_PATH_GET = re.compile(r"PathService::Get\s*\(\s*(?:base::|chrome::)?"
                       r"(DIR_\w+)\s*,\s*&\s*([A-Za-z_]\w*)\s*\)")
_PATH_CHECKED = re.compile(r"\b([A-Za-z_]\w*)\s*=\s*(?:base::)?PathService::"
                           r"CheckedGet\s*\(\s*(?:base::|chrome::)?(DIR_\w+)"
                           r"\s*\)")
_APPEND = re.compile(r'\s*\.\s*Append(?:ASCII)?\s*\(\s*(?:FILE_PATH_LITERAL'
                     r'\s*\(\s*)?"((?:[^"\\\n]|\\.)*)"\s*\)?\s*\)')
_APPEND_ANY = re.compile(r"\s*\.\s*Append(?:ASCII)?\s*\(")
_HANDLED = re.compile(r"ControllableHttpResponse>?\s*\(([^;]*?)\)\s*;",
                      re.S)


def _join(*parts):
    return "/".join(p.strip("/") for p in parts if p.strip("/"))


def data_refs(rel, text, kind, served_dirs=None):
    """[DataRef] of the files a test source opens, as far as its string
    literals show them (see `tests` in the module docstring).
    `served_dirs`: the first path segments under chrome/test/data (a
    browser test's "/x/..." literal is a served path when x is one of them,
    or when it names a page)."""
    code, lits = cxx_literals(text)
    roots = {}
    for m in _PATH_GET.finditer(code):
        key, var = m.group(1), m.group(2)
        if key == "DIR_TEST_DATA" and "chrome::" in m.group(0):
            roots[var] = ("src", CHROME_TEST_DATA)
        elif key in PATH_KEYS:
            roots[var] = (PATH_KEYS[key], "")
    for m in _PATH_CHECKED.finditer(code):
        var, key = m.group(1), m.group(2)
        if key in PATH_KEYS:
            roots[var] = (PATH_KEYS[key], "")

    def line_of(pos):
        return code.count("\n", 0, pos) + 1

    refs, dirs = [], []
    for var, (rkind, base) in sorted(roots.items()):
        for m in re.finditer(r"\b%s\b" % re.escape(var), code):
            pos = m.end()
            parts = []
            while True:
                a = _APPEND.match(code, pos)
                if not a:
                    break
                parts.append(a.group(1))
                pos = a.end()
            if not parts:
                continue
            path = _join(base, *parts)
            ln = line_of(m.start())
            if _APPEND_ANY.match(code, pos):
                # A directory, joined to a name the code computes.
                if rkind != "exe":
                    d = path + "/"
                    dirs.append((rkind, d))
                    refs.append(DataRef(rkind, ln, d))
            elif rkind == "exe":
                if "/" not in path:
                    refs.append(DataRef("exe", ln, path))
            else:
                refs.append(DataRef(rkind, ln, path))
    in_browser = kind in ("browser", "interactive")
    handled = set()
    if in_browser:
        for m in _HANDLED.finditer(code):
            handled.update(re.findall(r'"(/[^"]*)"', m.group(1)))
    named = set()
    for ln, _, _, v in lits:
        if not in_browser or v in handled:
            continue
        p = v.split("?", 1)[0].split("#", 1)[0]
        if p == "/" or not _SERVED.match(p):
            continue
        top = p.lstrip("/").split("/", 1)[0]
        last = p.rsplit("/", 1)[-1]
        page = last.rsplit(".", 1)[-1].lower() in ("html", "htm") \
            if "." in last else False
        if not page and (served_dirs is None or top not in served_dirs):
            continue        # "/tmp/x", "/echo": not a file it serves
        served = CHROME_TEST_DATA + p.lstrip("/")
        if p.endswith("/"):
            dirs.append(("src", served))
            refs.append(DataRef("served", ln, served))
        elif "." in last:
            refs.append(DataRef("served", ln, served))
            named.add(last)
    seen_dirs = sorted(set(dirs))
    if seen_dirs:
        for ln, _, _, v in lits:
            if _FIXTURE_NAME.match(v) and v not in named:
                refs.append(DataRef("bare", ln, v, seen_dirs))
    return refs


# src= and poster= attributes, <link href>, and CSS url() with a quoted or
# slashed value (not a script's `new URL(location.href)`).
_PAGE_REF = re.compile(r"""\b(?:[Ss][Rr][Cc]|[Pp][Oo][Ss][Tt][Ee][Rr])"""
                       r"""\s*=\s*["']([^"']*)["']|"""
                       r"""<[Ll][Ii][Nn][Kk]\b[^>]*?\bhref\s*=\s*"""
                       r"""["']([^"']*)["']|"""
                       r"""\burl\(\s*(?:["']([^"')]*)["']|([^"')\s]*/"""
                       r"""[^"')\s]*))\s*\)""")


def page_refs(rel, text):
    """[(line, //path)] a fixture page under chrome/test/data loads from its
    own server: src/poster attributes, <link href> and CSS url(), absolute
    ("/media/x.webm", from chrome/test/data) or relative to the page;
    another host, data: URLs and computed values are left out."""
    out = []
    page_dir = rel.rsplit("/", 1)[0]
    for m in _PAGE_REF.finditer(text):
        v = (m.group(1) or m.group(2) or m.group(3) or m.group(4) or
             "").strip()
        if not v or v.startswith(("//", "#", "data:", "about:", "blob:",
                                  "javascript:", "mailto:")) or \
                "://" in v or any(c in v for c in "{}$`+<>\\ "):
            continue
        v = v.split("?", 1)[0].split("#", 1)[0]
        if "." not in v.rsplit("/", 1)[-1]:
            continue
        p = CHROME_TEST_DATA + v.lstrip("/") if v.startswith("/") \
            else abspath(v, "//" + page_dir)[2:]
        out.append((text.count("\n", 0, m.start()) + 1, p))
    return out


class SourceListing:
    """Which files exist: the overlay's, and the pinned commit's (the
    tarball's paths.txt lacks most test data, so the git tree of
    tools/chromium/upstream.py is used when there is one). Paths inside a
    DEPS subproject (a gitlink) are unknown."""

    def __init__(self, overlay_files, pinned=None, gitlinks=()):
        self.files = set(overlay_files)
        self.pinned = pinned
        self.gitlinks = tuple(g.rstrip("/") + "/" for g in gitlinks)
        self._dirs = None

    def _all_dirs(self):
        if self._dirs is None:
            dirs = set()
            for src in (self.files, self.pinned or ()):
                for f in src:
                    i = f.rfind("/")
                    while i > 0:
                        d = f[:i]
                        if d in dirs:
                            break
                        dirs.add(d)
                        i = f.rfind("/", 0, i)
            self._dirs = dirs
        return self._dirs

    def exists(self, rel):
        """True, False, or None (can't tell)."""
        if rel.endswith("/"):
            if rel.rstrip("/") in self._all_dirs():
                return True
        elif rel in self.files or (self.pinned is not None and
                                   rel in self.pinned):
            return True
        if self.pinned is None or rel.startswith(self.gitlinks):
            return None
        return False


def pinned_listing(chromium_cache, log=log):
    """(paths, gitlinks) of the pinned commit's tree from
    tools/chromium/upstream.py's cache (it fetches the trees, about 20 MB,
    when the cache lacks them), or (None, ()) if that fails."""
    try:
        u = up.Upstream(cache_dir=chromium_cache)
        c = u.resolve(up.read_pin().revision)
        tree = u.tree(c)
    except (RuntimeError, SystemExit, OSError) as e:
        log("the pinned tree's file list is unavailable (%s): test data "
            "that isn't in src/ can't be checked" % e)
        return None, ()
    return ({p for p, e in tree.items() if e.type == "blob"},
            [p for p, e in tree.items() if e.type == "commit"])


def _covered(path, entries):
    """Whether a runtime data list covers //path (a directory ends in /)."""
    for e in entries:
        if e == path or (e.endswith("/") and path.startswith(e)) or \
                (path.endswith("/") and e.startswith(path)):
            return True
    return False


# What a desktop config's preprocessor sees (BUILDFLAG(IS_...) values).
PLATFORM_BUILDFLAGS = {
    "linux": {"IS_LINUX", "IS_POSIX"},
    "mac": {"IS_MAC", "IS_APPLE", "IS_POSIX"},
    "win": {"IS_WIN"},
}
KNOWN_OS_FLAGS = {"IS_LINUX", "IS_POSIX", "IS_MAC", "IS_APPLE", "IS_WIN",
                  "IS_CHROMEOS", "IS_ANDROID", "IS_IOS", "IS_FUCHSIA",
                  "IS_OZONE_X11"}
_DIRECTIVE = re.compile(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b"
                        r"(.*)$")
_COND_TOKEN = re.compile(r"\s*(BUILDFLAG\s*\(\s*\w+\s*\)|defined\s*\(\s*\w+"
                         r"\s*\)|\|\||&&|!|\(|\)|\w+)")


def preprocessor_conditions(text):
    """[conditions] per line (index = line number): the #if branches the
    line sits in, as [(expression, wanted)] (wanted False: a branch not
    taken before an #elif or #else)."""
    stack, out = [], [[]]
    for line in text.split("\n"):
        m = _DIRECTIVE.match(line)
        if m:
            d, rest = m.group(1), m.group(2).split("//", 1)[0].strip()
            if d == "if":
                stack.append([(rest, True)])
            elif d == "ifdef":
                stack.append([("defined(%s)" % rest, True)])
            elif d == "ifndef":
                stack.append([("defined(%s)" % rest, False)])
            elif d == "elif" and stack:
                stack[-1] = [(e, False) for e, _ in stack[-1]] + \
                    [(rest, True)]
            elif d == "else" and stack:
                stack[-1] = [(e, False) for e, _ in stack[-1]]
            elif d == "endif" and stack:
                stack.pop()
        out.append([c for frame in stack for c in frame])
    return out


def eval_condition(expr, platform):
    """True, False or None (unknown) for a #if expression on a desktop
    platform: only BUILDFLAG(IS_<OS>), !, &&, || and parentheses are
    known."""
    toks = []
    pos = 0
    while pos < len(expr):
        m = _COND_TOKEN.match(expr, pos)
        if not m:
            return None
        toks.append(m.group(1))
        pos = m.end()
        if not expr[pos:].strip():
            break
    flags = PLATFORM_BUILDFLAGS.get(platform, set())
    i = [0]

    def peek():
        return toks[i[0]] if i[0] < len(toks) else None

    def atom():
        t = peek()
        i[0] += 1
        if t is None:
            raise ValueError
        if t == "!":
            v = atom()
            return None if v is None else not v
        if t == "(":
            v = disj()
            if peek() != ")":
                raise ValueError
            i[0] += 1
            return v
        m = re.match(r"BUILDFLAG\s*\(\s*(\w+)\s*\)", t)
        if m:
            name = m.group(1)
            return (name in flags) if name in KNOWN_OS_FLAGS else None
        if t in ("0", "1"):
            return t == "1"
        return None

    def conj():
        vals = [atom()]
        while peek() == "&&":
            i[0] += 1
            vals.append(atom())
        if False in vals:
            return False
        return None if None in vals else True

    def disj():
        vals = [conj()]
        while peek() == "||":
            i[0] += 1
            vals.append(conj())
        if True in vals:
            return True
        return None if None in vals else False

    try:
        v = disj()
    except ValueError:
        return None
    return v if peek() is None else None


def line_compiled(conds, platform):
    """Whether a line under `conds` may be compiled on `platform`."""
    for expr, wanted in conds:
        v = eval_condition(expr, platform)
        if v is not None and v != wanted:
            return False
    return True


class RuntimeView:
    """One configuration's runtime deps of a test binary: its data entries
    and the executables it builds, as gn's runtime_deps collects them."""

    def __init__(self, graph, binary):
        members = closure(graph, binary, RUNTIME_KEYS)
        self.data = set()
        self.executables = set()
        for lab in members:
            t = graph.targets.get(lab)
            if t is None:
                continue
            self.data.update(t.data)
            if t.type == "executable" or t.template == "executable":
                self.executables.add(t.output_name or lab.split(":", 1)[1])


class TestData:
    """The data references of the test sources, read once."""

    def __init__(self, overlay, listing):
        self.overlay = overlay
        self.listing = listing
        self.served_dirs = {
            f[len(CHROME_TEST_DATA):].split("/", 1)[0]
            for src in (listing.files, listing.pinned or ())
            for f in src if f.startswith(CHROME_TEST_DATA)}
        self._refs = {}

    def refs(self, rel, kind):
        if rel not in self._refs:
            text = self.overlay.text(rel)
            self._refs[rel] = (data_refs(rel, text, kind, self.served_dirs),
                               preprocessor_conditions(text))
        return self._refs[rel]


def testdata_findings(graph, placements, data, findings):
    """For one configuration: the data each test source opens (on this
    platform) exists and is in the runtime data of each binary that runs
    it here."""
    config = graph.platform
    os_name = parse_config(config)[0]
    views = {}

    def view(binary):
        if binary not in views:
            views[binary] = RuntimeView(graph, binary)
        return views[binary]

    def add(severity, where, msg, hint=""):
        findings.add(Finding("testdata", severity, where, msg, hint), config)

    for rel, p in sorted(placements.items()):
        if p.kind == "fuzzer":
            continue
        bins = [b for b in TEST_BINARIES[p.kind]
                if config in p.binaries.get(b, [])]
        if not bins:
            continue
        refs, conds = data.refs(rel, p.kind)
        for r in refs:
            if not line_compiled(conds[r.line] if r.line < len(conds)
                                 else [], os_name):
                continue
            where = "%s:%d" % (rel, r.line)
            if r.kind == "exe":
                name = r.path[:-4] if r.path.endswith(".exe") else r.path
                if r.path.endswith(".exe") and os_name != "win":
                    continue
                lacking = [b for b in bins if name not in
                           view(b).executables]
                if lacking:
                    add("error", where,
                        "runs %s from the test binary's directory, but no "
                        "executable of that name is in the runtime deps of "
                        "%s" % (name, ", ".join(lacking)),
                        "add the executable target to the test target's "
                        "data_deps")
                continue
            if r.kind == "bare":
                found, unknown = None, False
                for dkind, d in r.dirs:
                    if dkind == "src":
                        e = data.listing.exists(d + r.path)
                        if e:
                            found = (dkind, d + r.path)
                            break
                        unknown = unknown or e is None
                    elif dkind == "gen" and graph.owners_of(GEN + d + r.path):
                        found = (dkind, d + r.path)
                        break
                if found is None:
                    add("warning" if unknown else "error", where,
                        "names %s, which is in none of the directories the "
                        "test reads from (%s)%s" % (
                            r.path, ", ".join(d for _, d in r.dirs),
                            "; the pinned tree's file list is unknown (pass "
                            "--chromium-cache)" if unknown else ""),
                        "" if unknown else "add the file, or fix its name")
                    continue
                r = DataRef(found[0], r.line, found[1])
            if r.kind == "gen":
                path = GEN + r.path
                if not r.path.endswith("/") and not graph.owners_of(path):
                    add("error", where,
                        "reads %s, which no target generates" % path,
                        "fix the path, or depend on the target that "
                        "generates it")
                    continue
            else:
                path = "//" + r.path
                e = data.listing.exists(r.path)
                if e is None:
                    add("warning", where,
                        "reads %s, which isn't in src/; whether the pinned "
                        "tree has it is unknown (pass --chromium-cache)"
                        % path)
                    continue
                if not e:
                    add("error", where,
                        "reads %s, which is neither in src/ nor in Chromium "
                        "at the pin" % path,
                        "add the file under src/, or fix the path")
                    continue
            lacking = [b for b in bins if not _covered(path, view(b).data)]
            if lacking:
                add("error", where,
                    "reads %s, which isn't in the runtime data of %s" % (
                        path, ", ".join(lacking)),
                    "list it (or its directory, ending in /) in the `data` "
                    "of the target that lists the test")


def page_findings(overlay, listing, findings):
    """Fixture pages under chrome/test/data: what they load from their own
    server is there."""
    n = 0
    for rel in overlay.files:
        if not rel.startswith(CHROME_TEST_DATA) or overlay.is_upstream(rel) \
                or rel.rsplit(".", 1)[-1].lower() not in PAGE_EXT:
            continue
        n += 1
        for ln, path in page_refs(rel, overlay.text(rel)):
            e = listing.exists(path)
            if e is False:
                findings.add(Finding(
                    "testdata", "error", "%s:%d" % (rel, ln),
                    "loads /%s, which is neither in src/ nor in Chromium at "
                    "the pin" % path[len(CHROME_TEST_DATA):],
                    "add the file under src/%s, or fix the path"
                    % CHROME_TEST_DATA), "all")
            elif e is None:
                findings.add(Finding(
                    "testdata", "warning", "%s:%d" % (rel, ln),
                    "loads //%s, which isn't in src/; whether the pinned "
                    "tree has it is unknown (pass --chromium-cache)" % path),
                    "all")
    return n


def run_tests(tree, overlay, platforms, listing, upstream_tree=None,
              extra_args=None, progress=log):
    """The `tests` check: (Findings, stats). Each configuration's graph is
    dropped once it has been read (about 600 MB each)."""
    tests = test_sources(overlay)
    findings = Findings()
    placements = {}
    data = TestData(overlay, listing)
    stats = {"test_sources": len(tests), "platforms": {}}
    for p in platforms:
        t0 = time.time()
        g = Graph(tree, p, extra_args)
        ug = Graph(upstream_tree, p, extra_args) if upstream_tree else None
        reach = place_tests(g, tests, placements)
        fuzzer_target_findings(g, overlay, ug, reach, findings)
        testdata_findings(g, placements, data, findings)
        stats["platforms"][p] = {"build_files_read": len(g.files),
                                 "seconds": round(time.time() - t0, 1)}
        del g, ug, reach
        if progress:
            progress("%s: %d test sources placed, %.0f s" % (
                p, len(tests), time.time() - t0))
    test_findings(placements, platforms, findings)
    stats["fixture_pages"] = page_findings(overlay, listing, findings)
    stats["with_data"] = sum(1 for refs, _ in data._refs.values() if refs)
    stats["kinds"] = {k: sum(1 for v in tests.values() if v == k)
                      for k in sorted(set(tests.values()))}
    return findings, stats


# ---------------------------------------------------------------------------
# flags: MoltBrowser's features and chrome://flags entries, as Chromium's
# own flag tests read them

ABOUT_FLAGS = "chrome/browser/about_flags.cc"
FLAG_METADATA = "chrome/browser/flag-metadata.json"
NEVER_EXPIRE = "chrome/browser/flag-never-expire-list.json"
ENUMS_XML = "tools/metrics/histograms/enums.xml"
# MoltBrowser features that are deliberately not chrome://flags entries.
NOT_FLAGS = {
    # Brings back component downloads only Google features read; switched
    # on from the command line only (molt_component_policy.h,
    # docs/NETWORK_TRAFFIC.md).
    "kMoltGoogleOnlyComponents": "command line only",
    # The black-glass toolbar's revertible switch, under the name of
    # upstream's removed experiment (chrome/browser/ui/ui_features.cc):
    # --disable-features=GlassToolbar.
    "kGlassToolbar": "command line only",
}
_BASE_FEATURE = re.compile(r"\bBASE_FEATURE\s*\(\s*(k\w+)\s*,\s*"
                           r'(?:"([^"]*)"\s*,\s*)?')
_FLAG_NAME_CONST = re.compile(r'\bconstexpr\s+char\s+(k\w+)\s*\[\]\s*=\s*'
                              r'"([^"]*)"\s*;')
_FLAG_REF = re.compile(r"#(molt-[a-z0-9-]*[a-z0-9-])")


def uma_id(label):
    """flags_ui::GetSwitchUMAId(): base::HashMetricName() (the first 8
    bytes of the MD5, big-endian) as a signed 32-bit value."""
    import hashlib
    v = int.from_bytes(hashlib.md5(label.encode()).digest()[:8], "big")
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v >= (1 << 31) else v


def strip_json_comments(text):
    """flag-metadata.json allows // comments (Chromium's JSON reader does);
    strings keep theirs ("//chrome/browser/OWNERS")."""
    out, i, n, in_str = [], 0, len(text), False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            out.append(c)
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        else:
            out.append(c)
        i += 1
    return "".join(out)


def valid_looking_owner(owner):
    """components/webui/flags/flags_test_helpers.cc IsValidLookingOwner()."""
    if "," in owner or " " in owner:
        return False
    at = owner.find("@")
    if at >= 0:
        return owner.find(".", at) >= 0
    if owner.startswith("//"):
        return owner.endswith("OWNERS")
    return False


def misordered(names):
    """[(name, should_follow)] as EnsureNamesAreAlphabetical() reports them
    (names compared lowercased, '_' as '-')."""
    norm = [x.lower().replace("_", "-") for x in names]
    back = {nm: orig for nm, orig in zip(norm, names)}
    start = "(start of file)"
    was = {nm: (norm[i - 1] if i else start) for i, nm in enumerate(norm)}
    srt = sorted(norm)
    goes = {nm: (srt[i - 1] if i else start) for i, nm in enumerate(srt)}
    return [(back[nm], back.get(goes[nm], goes[nm])) for nm in norm
            if was[nm] != goes[nm]]


def _split_top(s, sep=","):
    """Splits at `sep` outside brackets and string literals."""
    parts, depth, cur, i, n = [], 0, [], 0, len(s)
    while i < n:
        c = s[i]
        if c == '"':
            j = i + 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == "\\" else 1
            cur.append(s[i:j + 1])
            i = j + 1
            continue
        if c in "({[":
            depth += 1
        elif c in ")}]":
            depth -= 1
        if c == sep and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        parts.append("".join(cur).strip())
    return parts


def _concat_literal(expr):
    """The value of adjacent string literals ("a " "b"), else None."""
    vals = re.findall(r'"((?:[^"\\]|\\.)*)"', expr)
    rest = re.sub(r'"((?:[^"\\]|\\.)*)"', "", expr).strip()
    return "".join(vals) if vals and not rest else None


class FlagEntry:
    def __init__(self, where, internal, name, description, platforms,
                 value):
        self.where = where
        self.internal = internal
        self.name = name
        self.description = description
        self.platforms = platforms
        self.value = value        # ("feature", "kMoltX") or (type, text)


def _macro_body(text, macro):
    """The replacement text of `#define macro` (continuations joined), or
    None."""
    m = re.search(r"^[ \t]*#[ \t]*define[ \t]+%s\b" % re.escape(macro), text,
                  re.M)
    if not m:
        return None
    out = []
    for ln in text[m.end():].split("\n"):
        s = ln.rstrip()
        cont = s.endswith("\\")
        out.append(s[:-1] if cont else s)
        if not cont:
            break
    return " ".join(out)


def _entries_of(body, consts, where):
    out = []
    code = re.sub(r"//[^\n]*", "", body)
    for item in _split_top(code):
        item = item.strip()
        if not (item.startswith("{") and item.endswith("}")):
            continue
        f = _split_top(item[1:-1])
        if len(f) < 5:
            out.append(FlagEntry(where, None, None, None, None,
                                 ("unreadable", item[:60])))
            continue

        def val(x):
            lit = _concat_literal(x)
            if lit is not None:
                return lit
            ident = x.strip().rsplit("::", 1)[-1]
            return consts.get(ident)

        v = f[4].strip()
        m = re.match(r"FEATURE_(?:VALUE|WITH_PARAMS_VALUE)_TYPE\s*\(\s*"
                     r"([\w:]+)", v)
        value = ("feature", m.group(1).rsplit("::", 1)[-1]) if m else \
            ("other", v.split("(", 1)[0])
        out.append(FlagEntry(where, val(f[0]), val(f[1]), val(f[2]),
                             f[3].strip(), value))
    return out


def run_flags(overlay, upstream_read=None):
    """The `flags` check: (Findings, stats)."""
    findings = Findings()
    W = "flags"
    src = overlay.src
    if not (src / ABOUT_FLAGS).is_file():
        findings.add(Finding("flags", "error", ABOUT_FLAGS,
                             "is not in src/, so no MoltBrowser flag is "
                             "registered"), W)
        return findings, {}
    about = overlay.text(ABOUT_FLAGS)
    start = about.find("kFeatureEntries[] = {")
    end = about.find("\n};", start)
    entries_text = about[start:end] if start >= 0 and end > start else ""
    includes = set(all_includes(about))
    # Every "constexpr char kX[] = "..."" in MoltBrowser's headers (flag
    # names), and every BASE_FEATURE MoltBrowser defines.
    consts, features = {}, {}
    for rel in overlay.files:
        if not rel.endswith((".h", ".cc", ".mm")):
            continue
        if rel.startswith(OWN_EXCLUDE):
            continue
        text = overlay.text(rel)
        upstream = overlay.is_upstream(rel)
        up_text = None
        if upstream and "BASE_FEATURE" in text:
            b = overlay.upstream_bytes(rel)
            up_text = b.decode("utf-8", "replace") if b is not None else None
        if rel.endswith(".h"):
            for k, v in _FLAG_NAME_CONST.findall(text):
                if v.startswith("molt-"):
                    consts.setdefault(k, v)
        for m in _BASE_FEATURE.finditer(text):
            ident = m.group(1)
            # In an upstream file, only the features MoltBrowser added
            # (without the upstream copy: the kMolt ones).
            if upstream and (ident in up_text if up_text is not None
                             else not ident.startswith("kMolt")):
                continue
            features[ident] = (rel, text.count("\n", 0, m.start()) + 1,
                               m.group(2) or ident[1:])
    entries = []
    for macro in sorted(set(re.findall(r"\b(MOLT_\w+_FEATURE_ENTRIES)\b",
                                       entries_text))):
        hdr = None
        for rel in sorted(overlay.files):
            if rel.endswith(".h") and \
                    re.search(r"#\s*define\s+%s\b" % macro, overlay.text(rel)):
                hdr = rel
                break
        if hdr is None:
            findings.add(Finding("flags", "error", ABOUT_FLAGS,
                                 "uses %s, which no header in src/ defines"
                                 % macro), W)
            continue
        if hdr not in includes:
            findings.add(Finding("flags", "error", ABOUT_FLAGS,
                                 "uses %s but doesn't include %s" % (
                                     macro, hdr),
                                 "include it (`// MoltBrowser`)"), W)
        entries += _entries_of(_macro_body(overlay.text(hdr), macro), consts,
                               hdr)
    # Entries written into kFeatureEntries itself with a molt- name.
    for m in re.finditer(r'\{\s*"(molt-[^"]*)"', entries_text):
        j = entries_text.find("},", m.start())
        entries += _entries_of(entries_text[m.start():j + 1], consts,
                               ABOUT_FLAGS)
    names = {}
    flagged_features = set()
    for e in entries:
        if e.value[0] == "unreadable" or e.internal is None:
            findings.add(Finding("flags", "error", e.where,
                                 "has an entry this check can't read: %s"
                                 % (e.value[1] if e.value[0] == "unreadable"
                                    else "its internal name"),
                                 "write it as the other MOLT_*_FEATURE_"
                                 "ENTRIES entries are written"), W)
            continue
        names[e.internal] = e
        where = "%s (#%s)" % (e.where, e.internal)
        if not e.internal.startswith("molt-"):
            findings.add(Finding("flags", "error", where,
                                 "a MoltBrowser flag's name starts with "
                                 "molt-"), W)
        if "@" in e.internal:
            findings.add(Finding("flags", "error", where,
                                 "has '@' in its name (AboutFlagsTest."
                                 "NoSeparators)"), W)
        if not e.name or not e.description:
            findings.add(Finding("flags", "error", where,
                                 "has no %s" % ("name" if not e.name
                                                else "description"),
                                 "give the entry a visible name and a "
                                 "description"), W)
        if e.value[0] == "feature":
            flagged_features.add(e.value[1])
            if e.value[1] not in features:
                findings.add(Finding("flags", "error", where,
                                     "turns %s on and off, which no "
                                     "BASE_FEATURE in src/ defines"
                                     % e.value[1]), W)
    for ident, (rel, ln, _) in sorted(features.items()):
        if ident in flagged_features or ident in NOT_FLAGS:
            continue
        findings.add(Finding("flags", "error", "%s:%d" % (rel, ln),
                             "%s is not a chrome://flags entry" % ident,
                             "add it to the MOLT_*_FEATURE_ENTRIES of its "
                             "area (and flag-metadata.json), or to NOT_FLAGS "
                             "in this tool with the reason"), W)
    # flag-metadata.json and flag-never-expire-list.json, as AboutFlagsTest
    # reads them.
    meta, never = None, None
    try:
        meta = json.loads(strip_json_comments(overlay.text(FLAG_METADATA)))
    except (OSError, ValueError) as e:
        findings.add(Finding("flags", "error", FLAG_METADATA,
                             "can't be read: %s" % e), W)
    try:
        never = json.loads(strip_json_comments(overlay.text(NEVER_EXPIRE)))
    except (OSError, ValueError) as e:
        findings.add(Finding("flags", "error", NEVER_EXPIRE,
                             "can't be read: %s" % e), W)
    if meta is not None:
        by_name = {}
        for x in meta:
            if isinstance(x, dict) and isinstance(x.get("name"), str):
                by_name[x["name"]] = x
        for nm in sorted(names):
            x = by_name.get(nm)
            where = "%s (#%s)" % (FLAG_METADATA, nm)
            if x is None:
                findings.add(Finding("flags", "error", where,
                                     "has no entry (AboutFlagsTest."
                                     "EveryFlagHasMetadata)",
                                     "add one, in alphabetical order"), W)
                continue
            owners = x.get("owners") if isinstance(x.get("owners"),
                                                   list) else []
            if not owners:
                findings.add(Finding("flags", "error", where,
                                     "has no owners (AboutFlagsTest."
                                     "EveryFlagHasNonEmptyOwners)"), W)
            for o in owners:
                if not isinstance(o, str) or not valid_looking_owner(o):
                    findings.add(Finding(
                        "flags", "error", where,
                        "has the owner %r, which is neither an email nor "
                        "a //path/to/OWNERS (AboutFlagsTest."
                        "OwnersLookValid)" % (o,),
                        "name an OWNERS file, e.g. "
                        "//chrome/browser/molt_ai/OWNERS"), W)
                elif o.startswith("//") and not overlay.tree.exists(o[2:]):
                    findings.add(Finding(
                        "flags", "warning", where,
                        "names %s, which isn't in src/ or the tree" % o), W)
            exp = x.get("expiry_milestone")
            if not isinstance(exp, int) or isinstance(exp, bool):
                findings.add(Finding("flags", "error", where,
                                     "has no integer expiry_milestone"), W)
            elif exp == -1 and never is not None and nm not in never:
                findings.add(Finding("flags", "error", where,
                                     "never expires but isn't in %s "
                                     "(AboutFlagsTest."
                                     "OnlyPermittedFlagsNeverExpire)"
                                     % NEVER_EXPIRE), W)
        for nm in sorted(by_name):
            if nm.startswith("molt-") and nm not in names:
                findings.add(Finding("flags", "warning",
                                     "%s (#%s)" % (FLAG_METADATA, nm),
                                     "is not a registered flag any more",
                                     "remove the entry (and its "
                                     "never-expire line)"), W)
        for nm, after in misordered([x.get("name", "") for x in meta
                                     if isinstance(x, dict)]):
            findings.add(Finding("flags", "error", FLAG_METADATA,
                                 "'%s' should be right after '%s' "
                                 "(AboutFlagsTest."
                                 "FlagsListedInAlphabeticalOrder)"
                                 % (nm, after)), W)
    if never is not None:
        for nm, after in misordered([x for x in never
                                     if isinstance(x, str)]):
            findings.add(Finding("flags", "error", NEVER_EXPIRE,
                                 "'%s' should be right after '%s' "
                                 "(AboutFlagsTest."
                                 "FlagsListedInAlphabeticalOrder)"
                                 % (nm, after)), W)
        if meta is not None:
            known = {x.get("name") for x in meta if isinstance(x, dict)}
            for nm in never:
                if isinstance(nm, str) and nm.startswith("molt-") and \
                        nm not in known:
                    findings.add(Finding("flags", "error", NEVER_EXPIRE,
                                         "lists %s, which has no metadata "
                                         "(AboutFlagsTest."
                                         "NeverExpireFlagsExist)" % nm), W)
    # enums.xml's LoginCustomFlags (AboutFlagsHistogramTest.CheckHistograms).
    enums = None
    if (src / ENUMS_XML).is_file():
        enums = overlay.text(ENUMS_XML)
    elif upstream_read is not None:
        b = upstream_read(ENUMS_XML)
        enums = b.decode("utf-8", "replace") if b is not None else None
    missing = []
    for nm, e in sorted(names.items()):
        if e.value[0] != "feature" or e.value[1] not in features:
            continue
        fname = features[e.value[1]][2]
        for suffix in ("disabled", "enabled"):
            label = "%s:%s" % (fname, suffix)
            line = '<int value="%d" label="%s"/>' % (uma_id(label), label)
            if enums is None or line not in enums:
                missing.append(line)
    if enums is None:
        findings.add(Finding(
            "flags", "warning", ENUMS_XML,
            "couldn't be read, so LoginCustomFlags wasn't checked (pass "
            "--chromium-cache)"), W)
    elif missing:
        findings.add(Finding(
            "flags", "warning", ENUMS_XML,
            "LoginCustomFlags lacks %d MoltBrowser entries, so "
            "AboutFlagsHistogramTest.CheckHistograms fails (an owner's "
            "decision: BUILD_MACHINE_CHECKLIST.md §5.3, §11). The entries:\n"
            "      %s" % (len(missing), "\n      ".join(missing))), W)
    # #molt-... in the docs and the code names a registered flag.
    refs = 0
    for rel, text in _flag_ref_texts(overlay):
        for m in _FLAG_REF.finditer(text):
            nm = m.group(1)
            refs += 1
            # "#molt-shield-*" and a name cut at a line's end name a prefix.
            ok = nm in names or (nm.endswith("-") and
                                 any(x.startswith(nm) for x in names))
            if not ok:
                findings.add(Finding(
                    "flags", "error",
                    "%s:%d" % (rel, text.count("\n", 0, m.start()) + 1),
                    "names #%s, which is not a chrome://flags entry" % nm,
                    "fix the name, or register the flag"), W)
    return findings, {"entries": len(names), "features": len(features),
                      "references": refs}


def _flag_ref_texts(overlay):
    """(path, text) of what may name a chrome://flags entry: MoltBrowser's
    own files under src/, and the repository's docs and README (not the
    CHANGELOG, whose history may name a flag since removed)."""
    for rel in overlay.files:
        if rel.startswith(OWN_EXCLUDE) or overlay.is_upstream(rel):
            continue
        if rel.endswith((".h", ".cc", ".mm", ".js", ".html", ".grd",
                         ".grdp", ".md", ".json")):
            yield "src/" + rel, overlay.text(rel)
    root = overlay.src.parent
    for p in sorted(list((root / "docs").rglob("*.md")) +
                    [root / "README.md"]):
        if p.is_file():
            yield p.relative_to(root).as_posix(), p.read_text(
                errors="replace")


# ---------------------------------------------------------------------------
# CLI


def default_tree_dir(cache, version):
    return Path(cache) / version


def _upstream_reader(chromium_cache):
    """Reads files of the pinned commit through tools/chromium/upstream.py
    (for overlay files the fetched tree lacks)."""
    state = {}

    def read(rel):
        if "u" not in state:
            try:
                u = up.Upstream(cache_dir=chromium_cache)
                state["u"] = u
                state["c"] = u.resolve(up.read_pin().revision)
            except (RuntimeError, SystemExit) as e:
                log("upstream reader unavailable: %s" % e)
                state["u"] = None
        if state["u"] is None:
            return None
        try:
            return state["u"].read(state["c"], rel)
        except RuntimeError:
            return None

    return read


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="A static gn check of MoltBrowser's overlay.")
    ap.add_argument("--cache", default=str(DEFAULT_CACHE),
                    help="where `fetch` keeps the tree (default %(default)s)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch", help="stream the pinned release's GN files")
    f.add_argument("--version", help="default: chromium_version.txt")
    c = sub.add_parser("check", help="run the GN checks")
    d = sub.add_parser("checkdeps", help="run Chromium's checkdeps over "
                       "the overlay's C++ files and DEPS rules")
    b = sub.add_parser("blink-audit", help="run Blink's "
                       "audit_non_blink_usage check over MoltBrowser's "
                       "Blink lines")
    t = sub.add_parser("tests", help="MoltBrowser's test sources and "
                       "fuzzers no test binary builds, and the test data "
                       "they open")
    fl = sub.add_parser("flags", help="MoltBrowser's features and "
                        "chrome://flags entries, as Chromium's flag tests "
                        "read them")
    for x in (c, d, b, t, fl):
        x.add_argument("--tree", help="a fetched tree or a Chromium "
                       "checkout without the overlay copied in (default: "
                       "the fetched tree of the pin)")
        x.add_argument("--chromium-cache", default=str(up.DEFAULT_CACHE),
                       help="tools/chromium/upstream.py's cache, for "
                       "upstream copies the tree lacks")
        x.add_argument("--json", help="write the findings here")
    for x in (c, t):
        x.add_argument("--platform", default=DEFAULT_CONFIGS,
                       help="configs to evaluate: linux, mac or win from "
                       "configs/, each optionally +vulkan "
                       "(molt_llama_vulkan) and +component "
                       "(is_component_build) (default %(default)s)")
    for x in (c, t, fl):
        x.add_argument("--warnings", action="store_true",
                       help="print warnings too")
    c.add_argument("files", nargs="*", help="limit the include check to "
                   "these files (paths under src/)")
    a = ap.parse_args(argv)
    version = getattr(a, "version", None) or up.read_pin().version
    if a.cmd == "fetch":
        try:
            fetch(version, default_tree_dir(a.cache, version),
                  up.overlay_files())
        except cctarball.TarballNotVerified as e:
            log("fetch: %s" % e)
            return 2
        return 0
    if a.tree:
        root = Path(a.tree)
        paths = None
    else:
        dd = default_tree_dir(a.cache, version)
        if not (dd / "tree").is_dir():
            log("no fetched tree at %s: run `%s fetch` first, or pass "
                "--tree" % (dd, sys.argv[0]))
            return 2
        root = dd / "tree"
        paths = load_paths(dd / "paths.txt")
    files = up.overlay_files()
    tree = Tree(root, up.OVERLAY_DIR, paths)
    upstream_tree = Tree(root, None, paths)
    ov = Overlay(up.OVERLAY_DIR, files, tree,
                 _upstream_reader(a.chromium_cache))
    if a.cmd in ("tests", "flags"):
        if a.cmd == "flags":
            findings, stats = run_flags(ov, _upstream_reader(a.chromium_cache))
            summary = "%d MoltBrowser flags, %d features, %d #molt- " \
                "references" % (stats.get("entries", 0),
                                stats.get("features", 0),
                                stats.get("references", 0))
        else:
            platforms = [p.strip() for p in a.platform.split(",")
                         if p.strip()]
            for p in platforms:
                try:
                    parse_config(p)
                except ValueError as e:
                    ap.error(str(e))
            pinned, links = pinned_listing(a.chromium_cache)
            listing = SourceListing(files, pinned, links)
            findings, stats = run_tests(tree, ov, platforms, listing,
                                        upstream_tree)
            summary = "%d test sources (%s), %d with test data, %s" % (
                stats["test_sources"],
                ", ".join("%d %s" % (v, k)
                          for k, v in stats["kinds"].items()),
                stats["with_data"], ",".join(platforms))
        shown = findings.all() if a.warnings else findings.errors()
        for fd in shown:
            print(fd)
        errs = findings.errors()
        warns = len(findings.all()) - len(errs)
        print("%s: %s: %d errors, %d warnings%s" % (
            a.cmd, summary, len(errs), warns,
            "" if a.warnings or not warns else " (--warnings shows them)"))
        if a.json:
            Path(a.json).write_text(json.dumps(
                {"stats": stats, "findings": [x.to_json()
                                              for x in findings.all()]},
                indent=1))
        return 1 if errs else 0
    if a.cmd in ("checkdeps", "blink-audit"):
        if a.cmd == "checkdeps":
            findings, n = run_checkdeps(root, ov,
                                        Path(a.cache) / "checkdeps")
        else:
            findings, n = run_blink_audit(
                ov, _upstream_reader(a.chromium_cache))
        for fd in findings.all():
            print(fd)
        errs = findings.errors()
        print("%s: %d files, %d errors, %d warnings" % (
            a.cmd, n, len(errs), len(findings.all()) - len(errs)))
        if a.json:
            Path(a.json).write_text(json.dumps(
                [x.to_json() for x in findings.all()], indent=1))
        return 1 if errs else 0
    platforms = [p.strip() for p in a.platform.split(",") if p.strip()]
    for p in platforms:
        try:
            parse_config(p)
        except ValueError as e:
            ap.error(str(e))
    only = None
    if a.files:
        only = set()
        for x in a.files:
            x = x[4:] if x.startswith("src/") else x
            only.add(x)
    findings, stats = run(tree, ov, platforms, only, None, upstream_tree)
    shown = findings.all() if a.warnings else findings.errors()
    for fd in shown:
        print(fd)
    errs = findings.errors()
    warns = len(findings.all()) - len(errs)
    print("gn_static_check: %d include files, %d BUILD.gn files, %s: "
          "%d errors, %d warnings%s" % (
              stats["include_files"], stats["build_files"],
              ",".join(platforms), len(errs), warns,
              "" if a.warnings or not warns else " (--warnings shows them)"))
    if a.json:
        Path(a.json).write_text(json.dumps(
            {"stats": stats, "findings": [x.to_json()
                                          for x in findings.all()]},
            indent=1))
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
