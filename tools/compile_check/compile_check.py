#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Syntax-and-type check of MoltBrowser's C++ against Chromium's real headers,
without a Chromium checkout. See tools/compile_check/README.md.

  compile_check.py setup                 # Chromium's clang
  compile_check.py get PATH...           # materialize upstream files
  compile_check.py fetch [TARGETS...]    # include closure of the targets
  compile_check.py gen [TARGETS...]      # buildflags, mojom, grit, ...
  compile_check.py check [TARGETS...]    # clang -fsyntax-only
  compile_check.py all [TARGETS...]      # fetch + gen + check, repeated

TARGETS are paths relative to src/ (files or directories); the default is
the set in targets.py. The work dir (default .molt-compile-check-cache/,
gitignored) holds everything downloaded or generated; re-runs reuse it.
"""
import argparse
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import json  # noqa: E402

from ccheck import config, scan, sources, targets, toolchain  # noqa: E402
from ccheck import check, gen_actions, gen_buildflags, gen_mojom, gn  # noqa: E402,E501

REPO_ROOT = HERE.parents[1]


def make_tree(args):
    llvm = toolchain.llvm_commit(args.work)
    tree = sources.SourceTree(args.work, args.chromium_cache,
                              clang_llvm_commit=llvm)
    tree.sync_overlay()
    return tree


def cmd_setup(args):
    tree = make_tree(args)
    d = toolchain.ensure_clang(args.work, tree)
    print(d)


def ensure_libcxx(tree):
    """libc++ and libc++abi headers are taken whole (the scanner does not
    follow <vector>-style includes)."""
    paths = []
    for d in config.SYSTEM_DIRS:
        paths += tree.glob_upstream(d + "/")
    tree.materialize(paths)


def ensure_build_files(tree):
    """Every BUILD.gn, .gni and .mojom file at the pin (small; the GN reader
    and the mojom generator need them)."""
    tree.materialize(tree.chromium_files(
        lambda p: p.endswith(("/BUILD.gn", ".gni", ".mojom", "/.gn")) or
        p in ("BUILD.gn", ".gn")))


def cmd_fetch(args):
    tree = make_tree(args)
    ensure_libcxx(tree)
    ensure_build_files(tree)
    tgts = targets.expand(args.targets)
    sc = scan.Scanner(tree, config.INCLUDE_DIRS, tree.gen_dir)
    seen = sc.run(tgts)
    missing = {}
    for name, inc in sorted(sc.missing.items()):
        missing.setdefault(scan.classify_missing(name), {})[name] = \
            sorted(inc)[:5]
    out = {"targets": len(tgts), "reached": len(seen), "missing": missing}
    (args.work / "scan.json").write_text(json.dumps(out, indent=1))
    for k, v in sorted(missing.items()):
        print("%-12s %d" % (k, len(v)))
    print("reached %d files from %d targets" % (len(seen), len(tgts)))


def make_interp(tree):
    args_text = (REPO_ROOT / "configs" / "linux-x64.gn").read_text()
    return gn.make_interp(tree, args_text)


# Generator scripts and the Python packages they import.
GEN_TOOLS = ["mojo/public/tools/", "third_party/jinja2/",
             "third_party/markupsafe/", "third_party/ply/",
             "build/gn_helpers.py", "build/write_buildflag_header.py"]


def _produce(tree, interp, report):
    def f(rels):
        w, failed = gen_actions.produce(tree, interp, rels)
        report.setdefault("actions", {"written": [], "failed": {}})
        report["actions"]["written"] += w
        report["actions"]["failed"].update(failed)
        for k, v in failed.items():
            print("action failed: %s: %s" % (k, v))
        return w
    return f


def cmd_gen(args):
    tree = make_tree(args)
    paths = []
    for p in GEN_TOOLS:
        paths += tree.glob_upstream(p) if p.endswith("/") else [p]
    tree.materialize(paths)
    scanned = json.loads((args.work / "scan.json").read_text())
    missing = scanned["missing"]
    gen_dir = tree.gen_dir
    interp = make_interp(tree)
    report = {}
    names = sorted(missing.get("buildflags", {}))
    if names:
        w, f = gen_buildflags.generate(tree, interp, gen_dir, names,
                                       config.INCLUDE_DIRS)
        report["buildflags"] = {"written": w, "failed": f}
        print("buildflags: %d written, %d failed" % (len(w), len(f)))
        for k, v in sorted(f.items()):
            print("  %s: %s" % (k, v))
    names = sorted(missing.get("mojom", {}))
    if names:
        index = gen_mojom.build_index(tree, interp,
                                      args.work / "mojom_index.json")
        wanted = gen_mojom.wanted_mojoms(names)
        ok, f = gen_mojom.generate(
            tree, index, args.work, gen_dir, wanted,
            produce=_produce(tree, interp, report))
        report["mojom"] = {"generated": ok, "failed": f}
        print("mojom: %d generated, %d failed" % (len(ok), len(f)))
        for k, v in sorted(f.items()):
            print("  %s: %s" % (k, v))
    for w in interp.warnings[:20]:
        print("gn warning: " + w)
    (args.work / "gen_report.json").write_text(json.dumps(report, indent=1))


def cmd_check(args):
    tree = make_tree(args)
    tgts = targets.expand(args.targets)
    res = check.run(tree, args.work, tgts, jobs=args.jobs,
                    plugins=not args.no_plugins)
    summary = args.work / "check.json"
    old = {}
    if summary.exists() and args.targets:
        old = {r["target"]: r for r in json.loads(summary.read_text())}
    for r in res:
        old[r["target"]] = r
    summary.write_text(json.dumps(sorted(old.values(),
                                         key=lambda r: r["target"]),
                                  indent=1))
    n = sum(1 for r in res if any("error" in d["kind"] for d in r["diags"]))
    print("%d of %d file(s) with errors; details in %s" % (n, len(res),
                                                          summary))


def cmd_get(args):
    """PATH ending in / = everything under that directory."""
    tree = make_tree(args)
    paths = []
    for p in args.paths:
        paths += tree.glob_upstream(p) if p.endswith("/") else [p]
    tree.materialize(paths)
    for p in args.paths:
        print(tree.local_path(p))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--work", type=Path,
                    default=Path(os.environ.get(
                        "MOLT_COMPILE_CHECK_DIR",
                        REPO_ROOT / ".molt-compile-check-cache")))
    ap.add_argument("--chromium-cache", type=Path,
                    default=Path(os.environ.get(
                        "MOLT_CHROMIUM_CACHE",
                        REPO_ROOT / ".molt-chromium-cache")))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("setup")
    g = sub.add_parser("get")
    g.add_argument("paths", nargs="+")
    f = sub.add_parser("fetch")
    f.add_argument("targets", nargs="*")
    sub.add_parser("gen")
    c = sub.add_parser("check")
    c.add_argument("targets", nargs="*")
    c.add_argument("--jobs", type=int, default=2)
    c.add_argument("--no-plugins", action="store_true")
    args = ap.parse_args()
    args.work = args.work.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    {"setup": cmd_setup, "get": cmd_get, "fetch": cmd_fetch,
     "gen": cmd_gen, "check": cmd_check}[args.cmd](args)


if __name__ == "__main__":
    main()
