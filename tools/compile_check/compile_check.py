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

REPO_ROOT = HERE.parents[1]


def make_tree(args):
    llvm = toolchain.llvm_commit(args.work)
    return sources.SourceTree(args.work, args.chromium_cache,
                              clang_llvm_commit=llvm)


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


def cmd_fetch(args):
    tree = make_tree(args)
    ensure_libcxx(tree)
    tgts = targets.expand(args.targets)
    sc = scan.Scanner(tree, config.INCLUDE_DIRS, args.work / "gen")
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


def cmd_get(args):
    tree = make_tree(args)
    tree.materialize(args.paths)
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
    args = ap.parse_args()
    args.work = args.work.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    {"setup": cmd_setup, "get": cmd_get, "fetch": cmd_fetch}[args.cmd](args)


if __name__ == "__main__":
    main()
