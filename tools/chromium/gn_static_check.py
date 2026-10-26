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

Usage:
  tools/chromium/gn_static_check.py fetch      # stream the pin's GN files once
  tools/chromium/gn_static_check.py check [--platform CONFIGS] [--tree DIR]
                                    [--warnings] [--json OUT] [FILE...]
  tools/chromium/gn_static_check.py checkdeps [--json OUT]
  tools/chromium/gn_static_check.py blink-audit [--json OUT]
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
                 "data_deps")

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
               "third_party/llama_cpp/vendor/", "third_party/skia/", "v8/")
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


def fetch(version, dest, overlay_files, log=log, opener=None):
    """Streams chromium-<version>-lite.tar.xz once and keeps every BUILD.gn,
    .gn, .gni and DEPS file, //chrome/VERSION and build/util (gn runs
    version.py), checkdeps, and the upstream copy of every overlay path;
    writes paths.txt (every file in the tarball)."""
    dest = Path(dest)
    tmp = dest.with_name(dest.name + ".partial")
    shutil.rmtree(tmp, ignore_errors=True)
    (tmp / "tree").mkdir(parents=True)
    url = BUCKET + "chromium-%s-lite.tar.xz" % version
    top = "chromium-%s/" % version
    keep_prefix = ("buildtools/checkdeps/", "build/util/")
    want = set(overlay_files)
    t0 = time.time()
    log("fetch: streaming %s (about 5 minutes)" % url)
    n = kept = 0
    opener = opener or urllib.request.urlopen
    with open(tmp / "paths.txt", "w") as paths, opener(url) as r, \
            tarfile.open(fileobj=r, mode="r|xz") as tf:
        for m in tf:
            n += 1
            if not m.name.startswith(top):
                continue
            rel = m.name[len(top):]
            if not rel or ".." in rel.split("/"):
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


def run_checkdeps(tree_root, overlay, work, log=log):
    """Runs the tree's buildtools/checkdeps/checkdeps.py over a copy of the
    tree's DEPS files with the overlay's on top and the overlay's C++ files
    (checkdeps reads only those). Returns (findings, number of files)."""
    tree_root = Path(tree_root)
    script = tree_root / "buildtools" / "checkdeps" / "checkdeps.py"
    if not script.is_file():
        raise RuntimeError("no %s (run fetch)" % script)
    work = Path(work)
    root = work / "src"
    shutil.rmtree(work, ignore_errors=True)
    root.mkdir(parents=True)
    for p in tree_root.rglob("DEPS"):
        if p.is_file():
            dst = root / p.relative_to(tree_root)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(p, dst)
    plan = checkdeps_plan(overlay)
    for rel in list(overlay.files):
        if rel.rsplit("/", 1)[-1] == "DEPS" or rel in plan:
            dst = root / rel
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
    findings = Findings()
    for res in json.loads(out.read_text()):
        rel = os.path.relpath(res["dependee_path"], str(root))
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
    return findings, len(plan)


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
    for x in (c, d, b):
        x.add_argument("--tree", help="a fetched tree or a Chromium "
                       "checkout (default: the fetched tree of the pin)")
        x.add_argument("--chromium-cache", default=str(up.DEFAULT_CACHE),
                       help="tools/chromium/upstream.py's cache, for "
                       "upstream copies the tree lacks")
        x.add_argument("--json", help="write the findings here")
    c.add_argument("--platform", default=DEFAULT_CONFIGS,
                   help="configs to evaluate: linux, mac or win from "
                   "configs/, each optionally +vulkan (molt_llama_vulkan) "
                   "and +component (is_component_build) (default "
                   "%(default)s)")
    c.add_argument("--warnings", action="store_true",
                   help="print warnings too")
    c.add_argument("files", nargs="*", help="limit the include check to "
                   "these files (paths under src/)")
    a = ap.parse_args(argv)
    version = getattr(a, "version", None) or up.read_pin().version
    if a.cmd == "fetch":
        fetch(version, default_tree_dir(a.cache, version), up.overlay_files())
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
