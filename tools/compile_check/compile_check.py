#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Syntax-and-type check of MoltBrowser's C++ against Chromium's real headers,
without a Chromium checkout. See tools/compile_check/README.md.

  compile_check.py all [TARGETS...]      # setup, then fetch + gen until
                                         # nothing new, then check
  compile_check.py setup                 # Chromium's clang, protoc
  compile_check.py fetch [TARGETS...]    # include closure of the targets
  compile_check.py gen                   # what the last fetch found missing
  compile_check.py check [TARGETS...]    # clang -fsyntax-only
  compile_check.py report                # summary of the last check
  compile_check.py get PATH...           # materialize upstream files

TARGETS are paths relative to src/ (files or directories), @first-party or
@hooks; the default is both (targets.py). The work dir (default
.molt-compile-check-cache/, gitignored) holds everything downloaded or
generated; re-runs reuse it.
"""
import argparse
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from ccheck import check, config, scan, sources, targets, toolchain  # noqa
from ccheck import gen_actions, gen_buildflags, gen_mojom, gen_misc, gn  # noqa
from ccheck import host_tools  # noqa

REPO_ROOT = HERE.parents[1]

# Generator scripts and the Python packages they import.
GEN_TOOLS = ["mojo/public/tools/", "third_party/jinja2/",
             "third_party/markupsafe/", "third_party/ply/",
             "build/gn_helpers.py", "build/write_buildflag_header.py",
             "tools/protoc_wrapper/"]


def make_tree(args):
    llvm = toolchain.llvm_commit(args.work)
    tree = sources.SourceTree(args.work, args.chromium_cache,
                              clang_llvm_commit=llvm)
    tree.sync_overlay()
    return tree


def make_interp(tree):
    args_text = (REPO_ROOT / "configs" / "linux-x64.gn").read_text()
    return gn.make_interp(tree, args_text)


def ensure_inputs(tree):
    """libc++/libc++abi headers (the scanner does not follow <vector>), and
    every BUILD.gn, .gni, .mojom and .proto file at the pin (small; the GN
    reader and the generators need them), and the generator scripts."""
    paths = []
    for d in config.SYSTEM_DIRS:
        paths += tree.glob_upstream(d + "/")
    paths += tree.chromium_files(
        lambda p: p.endswith(("/BUILD.gn", ".gni", ".mojom", ".proto",
                              "/.gn")) or p in ("BUILD.gn", ".gn"))
    for p in GEN_TOOLS:
        paths += tree.glob_upstream(p) if p.endswith("/") else [p]
    tree.materialize(paths)
    write_hook_outputs(tree)


def write_hook_outputs(tree):
    """Files gclient's hooks write into a checkout."""
    pin = tree.chromium.rev
    util = tree.tree_dir / "build" / "util"
    util.mkdir(parents=True, exist_ok=True)
    lc = "LASTCHANGE=%s-refs/heads/main@{#0}\n" % pin
    if not (util / "LASTCHANGE").exists() or \
            (util / "LASTCHANGE").read_text() != lc:
        (util / "LASTCHANGE").write_text(lc)
        (util / "LASTCHANGE.committime").write_text("1767225600")
    if not (util / "LASTCHANGE.dummy").exists():
        (util / "LASTCHANGE.dummy").write_text(
            "LASTCHANGE=0000000000000000000000000000000000000000-0000000000000000000000000000000000000000\n")


def cmd_setup(args, tree=None):
    tree = tree or make_tree(args)
    print(toolchain.ensure_clang(args.work, tree))
    print(toolchain.ensure_protoc(args.work, tree))
    print(toolchain.ensure_sysroot(args.work, tree))
    ensure_inputs(tree)


def do_fetch(args, tree):
    tgts = targets.expand(args.targets, tree)
    sc = scan.Scanner(tree, config.INCLUDE_DIRS, tree.gen_dir)
    seen = sc.run(tgts)
    missing = {}
    for name, inc in sorted(sc.missing.items()):
        missing.setdefault(scan.classify_missing(name), {})[name] = \
            sorted(inc)[:5]
    out = {"targets": len(tgts), "reached": len(seen), "missing": missing}
    (args.work / "scan.json").write_text(json.dumps(out, indent=1))
    # The include graph (over-approximated like the scan), for `report`.
    graph_path = args.work / "graph.json"
    graph = json.loads(graph_path.read_text()) if graph_path.exists() else {}
    for (kind, path), deps in sc.resolved.items():
        graph[kind + ":" + path] = sorted({k + ":" + p for k, p in deps})
    graph_path.write_text(json.dumps(graph))
    for k, v in sorted(missing.items()):
        print("missing %-12s %d" % (k, len(v)))
    print("reached %d files from %d targets" % (len(seen), len(tgts)))
    return out


def cmd_fetch(args):
    tree = make_tree(args)
    ensure_inputs(tree)
    do_fetch(args, tree)


def _gen_candidates(name):
    """gen-relative paths an include name could be generated at."""
    out = [name]
    for inc in config.INCLUDE_DIRS:
        if inc.startswith("gen:") and inc[4:]:
            out.append(inc[4:] + "/" + name)
    return out


def do_gen(args, tree, missing):
    """One generation round. Returns how many files it wrote."""
    gen_dir = tree.gen_dir
    interp = make_interp(tree)
    gen_actions.HOST_TOOLS["--protoc"] = toolchain.ensure_protoc(args.work,
                                                                 tree)
    try:
        plugins = host_tools.ensure_perfetto_plugins(args.work, tree, interp)
        gen_actions.HOST_TOOLS["--plugin"] = lambda t: plugins[
            "cppgen_plugin" if "cpp" in t.name else "protozero_plugin"]
    except RuntimeError as e:
        print("perfetto protoc plugins unavailable: %s" % e)
    gen_misc.TOOLS["cxxbridge"] = lambda: host_tools.ensure_cxxbridge(
        args.work, tree)
    gen_misc.TOOLS["flatc"] = lambda: host_tools.ensure_flatc(
        args.work, tree, interp)
    report_path = args.work / "gen_report.json"
    report = {}
    written = 0

    def produce(rels):
        w, failed = gen_actions.produce(tree, interp, rels)
        report.setdefault("actions", {"written": [], "failed": {}})
        report["actions"]["written"] += w
        report["actions"]["failed"].update(failed)
        return w

    names = sorted(missing.get("buildflags", {}))
    if names:
        w, f = gen_buildflags.generate(tree, interp, gen_dir, names,
                                       config.INCLUDE_DIRS)
        report["buildflags"] = {"written": w, "failed": f}
        written += len(w)
        # Not a buildflag_header() after all (e.g. Blink's generated
        # runtime_enabled_features.h): leave it to the actions below.
        for n in f:
            missing.setdefault("other", {})[n] = \
                missing["buildflags"][n]
        print("buildflags: %d written, %d failed" % (len(w), len(f)))
    names = sorted(missing.get("mojom", {}))
    if names:
        index = gen_mojom.build_index(tree, interp,
                                      args.work / "mojom_index.json")
        wanted = gen_mojom.wanted_mojoms(names)
        ok, f = gen_mojom.generate(tree, index, args.work, gen_dir, wanted,
                                   produce=produce)
        report["mojom"] = {"generated": ok, "failed": f}
        written += len(ok)
        print("mojom: %d generated, %d failed" % (len(ok), len(f)))
    # Everything else: the GN action that writes it, else a known
    # generator (gen_misc), else it stays missing.
    other = {}
    includers = {}
    for kind in ("proto", "pbzero", "grit", "vector_icons", "other"):
        for name, incs in missing.get(kind, {}).items():
            other[name] = kind
            includers[name] = incs
    done = set()
    for name, kind in sorted(other.items()):
        if gen_misc.handles_exclusively(name):
            continue
        cands = _gen_candidates(name)
        # A generated file including a bare name: next to it.
        for inc in includers.get(name, []):
            if inc.startswith("gen:") and "/" in inc and "/" not in name:
                cands.append(inc[4:].rsplit("/", 1)[0] + "/" + name)
        for rel in cands:
            if produce([rel]):
                done.add(name)
                written += 1
                break
    w, f = [], {}
    for name in [n for n in other if n not in done]:
        if not gen_misc.handles(name):
            continue
        # The include may be relative to a generated include dir.
        for rel in _gen_candidates(name):
            w1, f1 = gen_misc.generate(tree, interp, gen_dir, [rel],
                                       {rel: includers.get(name, [])})
            if w1:
                w.append(name)
                f.pop(name, None)
                break
            f.setdefault(name, f1.get(rel, "?"))
    written += len(w)
    report["misc"] = {"written": w, "failed": f}
    left = sorted(n for n in other if n not in done and n not in w)
    report["still_missing"] = {n: other[n] for n in left}
    report["gn_warnings"] = interp.warnings[:50]
    old = json.loads(report_path.read_text()) if report_path.exists() else {}
    old.update(report)
    report_path.write_text(json.dumps(old, indent=1))
    print("gen: wrote %d file(s); %d name(s) still missing (see %s)" % (
        written, len(left), report_path))
    return written


def cmd_gen(args):
    tree = make_tree(args)
    ensure_inputs(tree)
    scanned = json.loads((args.work / "scan.json").read_text())
    do_gen(args, tree, scanned["missing"])


def do_check(args, tree):
    tgts = targets.expand(args.targets, tree)
    res = check.run(tree, args.work, tgts, jobs=args.jobs,
                    plugins=not args.no_plugins, dev=args.dev_plugins)
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


def cmd_check(args):
    do_check(args, make_tree(args))


def cmd_all(args):
    tree = make_tree(args)
    cmd_setup(args, tree)
    for i in range(args.rounds):
        print("== round %d" % (i + 1))
        out = do_fetch(args, tree)
        if do_gen(args, tree, out["missing"]) == 0:
            break
    do_check(args, tree)
    cmd_report(args)


def _reach(graph, start):
    seen = {start}
    todo = [start]
    while todo:
        for n in graph.get(todo.pop(), ()):
            if n not in seen:
                seen.add(n)
                todo.append(n)
    return seen


def classify(r, graph, gen_dir):
    """(status, detail, stand-ins) for one check result. The stand-ins are
    the ones the file's include closure (over-approximated, like the scan)
    reaches: marked grit headers and pdfium's mirror-HEAD headers. The
    toolchain-wide substitutes (README) apply to every file."""
    errs = [d for d in r["diags"] if "error" in d["kind"]]
    missing = [d for d in errs if d["kind"] == "fatal error" and
               "file not found" in d["msg"]]
    stand_ins = []
    reach = _reach(graph, "src:" + r["target"])
    for n in reach:
        if n.startswith("gen:") and "/grit/" in n:
            try:
                head = (gen_dir / n[4:]).read_text()[:40]
            except OSError:
                head = ""
            if "STAND-IN" in head:
                stand_ins.append("grit stand-in")
        else:
            for mount in ("third_party/pdfium", "third_party/webrtc",
                          "third_party/libyuv", "third_party/openscreen/src",
                          "third_party/dom_distiller_js/dist"):
                if n.startswith("src:" + mount + "/"):
                    stand_ins.append(mount + " (mirror HEAD)")
    stand_ins = sorted(set(stand_ins))
    if missing:
        name = missing[0]["msg"].split("'")[1] if "'" in missing[0]["msg"] \
            else missing[0]["msg"]
        return "not checked", "header not available here: " + name, \
            stand_ins
    if errs:
        return "ERRORS", "%d error(s)" % len(errs), stand_ins
    if stand_ins:
        return "partly on stand-ins", ", ".join(stand_ins), stand_ins
    return "checked", "", stand_ins


def cmd_report(args):
    res = json.loads((args.work / "check.json").read_text())
    graph_path = args.work / "graph.json"
    graph = json.loads(graph_path.read_text()) if graph_path.exists() else {}
    gen_dir = args.work / "chromium" / "src" / sources.OUT_REL / "gen"
    counts = {}
    rows = []
    for r in res:
        status, detail, _ = classify(r, graph, gen_dir)
        counts[status] = counts.get(status, 0) + 1
        warns = sum(1 for d in r["diags"] if d["kind"] == "warning")
        rows.append((r["target"], status, detail, warns))
        if args.markdown:
            continue
        print("%-80s %-20s %s" % (r["target"], status, detail))
        if status == "ERRORS":
            for d in [d for d in r["diags"] if "error" in d["kind"]][
                    :args.max_lines]:
                print("    %s:%d: %s" % (d["file"], d["line"],
                                         d["msg"][:160]))
    if args.markdown:
        print("| File | Result | Detail |")
        print("|---|---|---|")
        for t, st, det, _ in rows:
            print("| `%s` | %s | %s |" % (t, st, det))
    print("\n" + ", ".join("%s: %d" % kv for kv in sorted(counts.items())))


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
    for name in ("check", "all"):
        c = sub.add_parser(name)
        c.add_argument("targets", nargs="*")
        c.add_argument("--jobs", type=int, default=2)
        c.add_argument("--no-plugins", action="store_true",
                       help="skip Chromium's clang plugins")
        c.add_argument("--dev-plugins", action="store_true",
                       help="also the raw_ptr/raw_ref field checks a "
                       "non-official build runs")
        if name == "all":
            c.add_argument("--rounds", type=int, default=6)
            c.add_argument("--max-lines", type=int, default=5)
            c.add_argument("--markdown", action="store_true")
    r = sub.add_parser("report")
    r.add_argument("--max-lines", type=int, default=5)
    r.add_argument("--markdown", action="store_true",
                   help="one table row per file")
    args = ap.parse_args()
    args.work = args.work.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    {"setup": cmd_setup, "get": cmd_get, "fetch": cmd_fetch, "gen": cmd_gen,
     "check": cmd_check, "all": cmd_all, "report": cmd_report}[args.cmd](args)


if __name__ == "__main__":
    main()
