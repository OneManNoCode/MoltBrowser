#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Syntax-and-type check of MoltBrowser's C, C++ and Objective-C++ against
Chromium's real headers, without a Chromium checkout. See
tools/compile_check/README.md.

  compile_check.py all [TARGETS...]      # setup, then fetch + gen until
                                         # nothing new, then check
  compile_check.py setup                 # Chromium's clang, protoc
  compile_check.py fetch [TARGETS...]    # include closure of the targets
  compile_check.py gen [TARGETS...]      # what the last fetch found missing
  compile_check.py check [TARGETS...]    # clang -fsyntax-only
  compile_check.py report                # summary of the last check
  compile_check.py get PATH...           # materialize upstream files

TARGETS are paths relative to src/ (files or directories), @changed (every
C/C++/Objective-C file MoltBrowser adds or changes; the default),
@first-party, @hooks, @patched or @list:FILE. Each file is checked for its
platform (platforms.py: Linux x64, Linux arm64, Windows x64 or macOS
arm64) in its language. The work dir (default .molt-compile-check-cache/,
gitignored) holds everything downloaded or generated; re-runs reuse it.
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from ccheck import check, config, scan, sources, targets, toolchain  # noqa
from ccheck import gen_actions, gen_buildflags, gen_mojom, gen_misc, gn  # noqa
from ccheck import gnconfigs, host_tools, platforms, sdks  # noqa

REPO_ROOT = HERE.parents[1]

# Generator scripts and the Python packages they import.
GEN_TOOLS = ["mojo/public/tools/", "third_party/jinja2/",
             "third_party/markupsafe/", "third_party/ply/",
             "build/gn_helpers.py", "build/write_buildflag_header.py",
             "tools/protoc_wrapper/"]

# The order platforms are checked in (Linux first: it is what the other
# platforms' shared setup is made by).
PLATFORM_ORDER = ["linux", "linux-arm64", "linux-vulkan", "mac", "win"]


def make_tree(args, platform=None):
    llvm = toolchain.llvm_commit(args.work)
    tree = sources.SourceTree(args.work, args.chromium_cache,
                              clang_llvm_commit=llvm, platform=platform)
    tree.sync_overlay()
    tree.system_dirs = []
    return tree


def make_interp(tree):
    args_text = (REPO_ROOT / tree.platform.args_file).read_text()
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
    for p in GEN_TOOLS + config.MACRO_INCLUDED:
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
    # lastchange.py -s third_party/dawn --revision gpu/webgpu/DAWN_VERSION
    e = tree.chromium.entries.get("third_party/dawn")
    dv = tree.tree_dir / "gpu" / "webgpu" / "DAWN_VERSION"
    text = (e.oid if e is not None else "") + "\n"
    if not dv.exists() or dv.read_text() != text:
        dv.parent.mkdir(parents=True, exist_ok=True)
        dv.write_text(text)


def cmd_setup(args, tree=None):
    tree = tree or make_tree(args)
    print(toolchain.ensure_clang(args.work, tree))
    print(toolchain.ensure_protoc(args.work, tree))
    print(toolchain.ensure_sysroot(args.work, tree))
    ensure_inputs(tree)


def platform_setup(args, tree):
    """What a platform's compiles need besides the checkout: its sysroot or
    SDK. Sets tree.system_dirs (the dirs whose headers are the system's).
    Returns False (with the reason in tree.unavailable_reason) if the
    platform can't be set up here."""
    name = tree.platform.name
    tree.unavailable_reason = None
    try:
        if name in ("linux", "linux-vulkan"):
            toolchain.ensure_sysroot(args.work, tree)
        elif name == "linux-arm64":
            toolchain.ensure_sysroot(args.work, tree, "arm64")
        elif name == "win":
            tree.system_dirs = sdks.ensure_windows(args.work)
        elif name == "mac":
            tree.system_dirs = sdks.ensure_mac(args.work, tree)
    except (RuntimeError, OSError) as e:
        tree.unavailable_reason = str(e).strip().splitlines()[-1][:300] \
            if str(e).strip() else repr(e)
        print("platform %s unavailable: %s" % (name,
                                               tree.unavailable_reason))
        return False
    return True


def do_fetch(args, tree, tgts):
    # A subproject whose source changed drops its old files first.
    tree.validate_mounts()
    plat = tree.platform
    atoms = scan.Atoms(plat.true_atoms, plat.false_atoms,
                       scan.read_buildflags(tree.gen_dir))
    sc = scan.Scanner(tree, config.include_dirs(plat.name), tree.gen_dir,
                      atoms=atoms, system_dirs=tree.system_dirs)
    seen = sc.run(tgts)
    missing = {}
    for name, inc in sorted(sc.missing.items()):
        missing.setdefault(scan.classify_missing(name), {})[name] = \
            sorted(inc)[:5]
    out = {"platform": plat.name, "targets": len(tgts), "reached": len(seen),
           "missing": missing}
    tree.state.mkdir(parents=True, exist_ok=True)
    (tree.state / "scan.json").write_text(json.dumps(out, indent=1))
    # The include graph (over-approximated like the scan), for `report`.
    graph_path = tree.state / "graph.json"
    graph = json.loads(graph_path.read_text()) if graph_path.exists() else {}
    for (kind, path), deps in sc.resolved.items():
        graph[kind + ":" + path] = sorted({k + ":" + p for k, p in deps})
    graph_path.write_text(json.dumps(graph))
    for k, v in sorted(missing.items()):
        print("missing %-12s %d" % (k, len(v)))
    print("reached %d files from %d targets (%s)" % (len(seen), len(tgts),
                                                     plat.name))
    return out


def _gen_candidates(name, plat=None, includers=()):
    """gen-relative paths an include name could be generated at: in a
    generated include root, next to a generated includer, or in a
    generated include root of the includer's library
    (config.SCOPED_INCLUDE_DIRS: V8's torque-generated/...)."""
    out = [name]
    for inc in config.include_dirs(plat or platforms.DEFAULT):
        if inc.startswith("gen:") and inc[4:]:
            out.append(inc[4:] + "/" + name)
    for inc in includers:
        inc = inc[4:] if inc.startswith("gen:") else inc
        cands = [inc.rsplit("/", 1)[0] + "/" + name] if "/" in inc else []
        cands += [d[4:] + "/" + name for d in config.scoped_include_dirs(inc)
                  if d.startswith("gen:")]
        out += [c for c in cands if c not in out]
    return out


def refresh_mojom(args, tree, interp, produce=None):
    """Regenerates the mojom bindings whose inputs changed since they were
    made (gen_mojom.refresh). Returns (regenerated, removed, failed)."""
    if not (tree.state / "mojom_stamps.json").exists():
        return [], [], {}
    index = gen_mojom.build_index(tree, interp, tree.state / "mojom_index.json")
    try:
        ok, gone, failed = gen_mojom.refresh(
            tree, index, tree.state, tree.gen_dir, produce=produce,
            features=tree.platform.mojom_features)
    except RuntimeError as e:
        print("mojom: refresh failed (%s); the stale outputs were removed" %
              e)
        return [], [], {"*": str(e)}
    if ok or gone or failed:
        print("mojom: %d regenerated, %d removed, %d failed (stale inputs)" %
              (len(ok), len(gone), len(failed)))
    return ok, gone, failed


def set_host_tools(args, tree, interp):
    gen_actions.HOST_TOOLS["--protoc"] = toolchain.ensure_protoc(args.work,
                                                                 tree)
    plugins = {}
    try:
        plugins = host_tools.ensure_perfetto_plugins(args.work, tree, interp)
    except RuntimeError as e:
        print("perfetto protoc plugins unavailable: %s" % e)

    def plugin_for(t):
        name = gen_actions.plugin_name(t.get("args") or [])
        if name == "proto_extras_plugin":
            try:
                return host_tools.ensure_proto_extras_plugin(args.work, tree,
                                                             interp)
            except (RuntimeError, OSError) as e:
                print("proto_extras_plugin unavailable: %s" %
                      str(e)[-600:])
                return None
        return plugins.get("cppgen_plugin" if "cpp" in t.name else
                           "protozero_plugin")

    gen_actions.HOST_TOOLS["--plugin"] = plugin_for
    gen_misc.TOOLS["cxxbridge"] = lambda: host_tools.ensure_cxxbridge(
        args.work, tree)
    for name in host_tools.V8_HOST_TOOLS:
        gen_actions.HOST_EXECUTABLES[name] = (
            lambda n=name: host_tools.ensure_v8_host_tool(args.work, tree,
                                                         interp, n))
    gen_misc.TOOLS["flatc"] = lambda: host_tools.ensure_flatc(
        args.work, tree, interp)
    gen_actions.HOST_EXECUTABLES["vulkan_shaders_gen"] = (
        lambda: host_tools.ensure_vulkan_shaders_gen(args.work, tree,
                                                     interp))


def refresh_generated(args, tree, interp, produce):
    """Remakes the generated headers whose inputs changed (mojom bindings,
    grit headers, cxx bridge headers). Returns {kind: report}."""
    report = {}
    ok, gone, f = refresh_mojom(args, tree, interp, produce)
    if ok or gone or f:
        report["mojom_refresh"] = {"regenerated": ok, "removed": gone,
                                   "failed": f}
    ok, gone = gen_misc.refresh_grit(tree, interp)
    if ok or gone:
        report["grit_refresh"] = {"remade": ok, "removed": gone}
        print("grit: %d header(s) remade, %d removed" % (len(ok), len(gone)))
    ok, gone = gen_misc.refresh_cxx(tree, interp)
    if ok or gone:
        report["cxx_refresh"] = {"remade": ok, "removed": gone}
        print("cxx: %d bridge header(s) remade, %d removed" % (len(ok),
                                                               len(gone)))
    return report


def do_gen(args, tree, missing):
    """One generation round. Returns how many files it wrote."""
    gen_dir = tree.gen_dir
    plat = tree.platform.name
    interp = make_interp(tree)
    set_host_tools(args, tree, interp)
    report_path = tree.state / "gen_report.json"
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
                                       config.include_dirs(plat))
        report["buildflags"] = {"written": w, "failed": f}
        written += len(w)
        # Not a buildflag_header() after all (e.g. Blink's generated
        # runtime_enabled_features.h): leave it to the actions below.
        for n in f:
            missing.setdefault("other", {})[n] = \
                missing["buildflags"][n]
        print("buildflags: %d written, %d failed" % (len(w), len(f)))
    ref = refresh_generated(args, tree, interp, produce)
    report.update(ref)
    for r in ref.values():
        written += len(r.get("regenerated", r.get("remade", [])))
    names = sorted(missing.get("mojom", {}))
    if names:
        index = gen_mojom.build_index(tree, interp,
                                      tree.state / "mojom_index.json")
        wanted = gen_mojom.wanted_mojoms(names)
        ok, f = gen_mojom.generate(tree, index, tree.state, gen_dir, wanted,
                                   produce=produce,
                                   features=tree.platform.mojom_features)
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
        # Also relative to the including file's directory, in gen/ (a
        # generated file including a bare name; v8-inspector-protocol.h
        # including "inspector/Debugger.h").
        cands = _gen_candidates(name, plat, includers.get(name, []))
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
        for rel in _gen_candidates(name, plat, includers.get(name, [])):
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


def group_targets(tgts):
    """{platform: [paths]} and {path: reason} for the files no platform here
    builds. tgts: paths (each on its own platform) or (path, platform)
    pairs (targets.platform_targets)."""
    groups, skipped = {}, {}
    for t in tgts:
        t, p = t[:2] if isinstance(t, tuple) else (t, platforms.of(t))
        if p is None:
            skipped[t] = platforms.not_modeled(t)
        elif t not in groups.get(p, []):
            groups.setdefault(p, []).append(t)
    return groups, skipped


def plan(tree, paths):
    """The (path, platform) pairs to check: each file on its own platform
    and on those its changed lines need (targets.platform_targets)."""
    return targets.platform_targets(tree, paths)


def ordered(groups):
    return [p for p in PLATFORM_ORDER if p in groups] + sorted(
        p for p in groups if p not in PLATFORM_ORDER)


def platform_trees(args, groups, base=None):
    """Yields (tree, targets) for each platform in `groups`, set up; a
    platform that can't be set up yields (tree, targets) with
    tree.unavailable_reason set."""
    for name in ordered(groups):
        tree = base if base is not None and \
            base.platform.name == name else make_tree(args, name)
        config.set_platform(tree.platform)
        platform_setup(args, tree)
        yield tree, groups[name]


def fetch_and_gen(args, tree, tgts, rounds):
    for i in range(rounds):
        print("== %s: round %d" % (tree.platform.name, i + 1))
        out = do_fetch(args, tree, tgts)
        if do_gen(args, tree, out["missing"]) == 0:
            break


def cmd_fetch(args):
    base = make_tree(args)
    ensure_inputs(base)
    groups, _ = group_targets(plan(base, targets.expand(args.targets,
                                                        base)))
    for tree, tgts in platform_trees(args, groups, base):
        if tree.unavailable_reason is None:
            do_fetch(args, tree, tgts)


def cmd_gen(args):
    base = make_tree(args)
    ensure_inputs(base)
    groups, _ = group_targets(plan(base, targets.expand(args.targets,
                                                        base)))
    for tree, _ in platform_trees(args, groups, base):
        if tree.unavailable_reason is not None:
            continue
        scanned = json.loads((tree.state / "scan.json").read_text())
        do_gen(args, tree, scanned["missing"])


def do_check(args, tree, tgts):
    """Checks `tgts` (all on tree.platform). Returns the results."""
    # A subproject whose source changed (sources.json) drops its old files:
    # fetch and generate again before compiling.
    if tree.validate_mounts():
        fetch_and_gen(args, tree, tgts, getattr(args, "rounds", 6))
    interp = make_interp(tree)
    set_host_tools(args, tree, interp)
    # Never compile against bindings of a .mojom edited since (or of one
    # whose imports or typemaps changed), grit headers of an edited .grd,
    # or bridge headers of an edited .rs.
    refresh_generated(args, tree, interp,
                      lambda rels: gen_actions.produce(tree, interp,
                                                       rels)[0])
    gn_flags = gnconfigs.resolve(tree, interp)
    # Each file's exception/RTTI setting and own configs, from its own GN
    # target.
    tflags = gnconfigs.target_flags(tree, interp, tgts)
    for t, dirs in scoped_dirs(tree, tgts).items():
        tflags.setdefault(t, {})["scoped"] = dirs
    return check.run(tree, args.work, tgts, jobs=args.jobs,
                     plugins=not args.no_plugins, dev=args.dev_plugins,
                     gn_flags=gn_flags, target_flags=tflags)


def scoped_dirs(tree, tgts):
    """{target: config.SCOPED_INCLUDE_DIRS of the libraries its include
    closure reaches} (as the public configs of its dependencies would add
    them)."""
    p = tree.state / "graph.json"
    graph = json.loads(p.read_text()) if p.exists() else {}
    out = {}
    for t in tgts:
        reach = {n.split(":", 1)[1] for n in _reach(graph, "src:" + t)}
        reach.add(t)
        dirs = []
        for prefix, ds in config.SCOPED_INCLUDE_DIRS.items():
            if any(n.startswith(prefix) for n in reach):
                dirs += [config.expand_dir(d) for d in ds
                         if config.expand_dir(d) not in dirs]
        out[t] = dirs
    return out


def save_results(args, res, partial):
    """Merges results into <work>/check.json (replacing those of the same
    files when `partial`, else everything). With --platforms, only the
    results of those platforms are replaced (all of them on a whole run,
    the same files' otherwise); the others' stay."""
    summary = args.work / "check.json"
    old = {}
    only = getattr(args, "platforms", None)

    def key(r):
        # A result from before platforms (no "platform" key) is Linux's; a
        # file no platform builds has None.
        return (r["target"], r.get("platform", platforms.DEFAULT) or "")

    if summary.exists() and (partial or only):
        old = {key(r): r for r in json.loads(summary.read_text())}
        again = {r["target"] for r in res}

        def replaced(k):
            if only and k[1] not in only:
                return False
            # A file checked again replaces all its earlier results (its
            # platforms may have changed).
            return k[0] in again if partial else True

        old = {k: v for k, v in old.items() if not replaced(k)}
    for r in res:
        old[key(r)] = r
    summary.write_text(json.dumps([old[k] for k in sorted(old)], indent=1))
    n = sum(1 for r in res if any("error" in d["kind"] for d in r["diags"]))
    print("%d of %d file(s) with errors; details in %s" % (n, len(res),
                                                          summary))


def skipped_results(skipped):
    return [{"target": t, "platform": None, "skipped": why, "diags": [],
             "rc": None} for t, why in sorted(skipped.items())]


def unavailable_results(tree, tgts):
    return [{"target": t, "platform": tree.platform.name,
             "skipped": "%s can't be set up here: %s" % (
                 tree.platform.label, tree.unavailable_reason),
             "diags": [], "rc": None} for t in tgts]


def unreached_headers(tree, tgts):
    """The changed headers of tree's platform that no target's include
    closure reaches (checked on their own, so that every changed file is
    compiled somewhere)."""
    p = tree.state / "graph.json"
    graph = json.loads(p.read_text()) if p.exists() else {}
    reach = set()
    for t in tgts:
        reach |= {n.split(":", 1)[1] for n in _reach(graph, "src:" + t)}
    return [h for h in targets.changed_headers(tree)
            if platforms.of(h) == tree.platform.name and h not in reach]


def built_here(tree, tgts, optional):
    """tgts without the `optional` ones (targets.if_built) no GN target of
    tree's platform builds."""
    if not optional:
        return tgts
    interp = make_interp(tree)
    cache = {}
    drop = {t for t in tgts if t in optional and
            gnconfigs.owning_target(tree, interp, t, cache) is None}
    if drop:
        print("%s: %d own source(s) no %s GN target builds (checked on "
              "their own platform only)" % (tree.platform.name, len(drop),
                                            tree.platform.label))
    return [t for t in tgts if t not in drop]


def run_checks(args, base, tgts, rounds=0, headers=False):
    pairs = plan(base, tgts)
    groups, skipped = group_targets(pairs)
    optional = targets.if_built(pairs)
    only = getattr(args, "platforms", None)
    if only:
        groups = {k: v for k, v in groups.items() if k in only}
        if platforms.DEFAULT not in only:
            skipped = {}  # reported with the default platform
    res = skipped_results(skipped)
    for tree, ts in platform_trees(args, groups, base):
        if tree.unavailable_reason is not None:
            res += unavailable_results(
                tree, [t for t in ts if t not in optional.get(
                    tree.platform.name, ())])
            continue
        if tree is not base:
            ensure_inputs(tree)
        ts = built_here(tree, ts, optional.get(tree.platform.name))
        if rounds:
            fetch_and_gen(args, tree, ts, rounds)
        via = {}
        if headers:
            extra = unreached_headers(tree, ts)
            if extra:
                print("%s: %d changed header(s) no target includes; "
                      "checked on their own" % (tree.platform.name,
                                                len(extra)))
                # A textual header (Transform_inl.h) through the source
                # that includes it.
                for h in [h for h in extra
                          if targets.TEXTUAL_HEADER_RE.search(h)]:
                    src = targets.includer_of(tree, h)
                    if src and src not in ts:
                        via[src] = h
                        extra = [src if x == h else x for x in extra]
                ts = ts + extra
                if rounds:
                    fetch_and_gen(args, tree, ts, rounds)
        out = do_check(args, tree, ts)
        for r in out:
            if r["target"] in via:
                r["via"] = r["target"]
                r["target"] = via[r["target"]]
        res += out
    return res


def _whole(args):
    """A run over everything changed (no targets, or @changed): changed
    headers no source includes are checked too."""
    return not args.targets or args.targets == ["@changed"]


def cmd_check(args):
    base = make_tree(args)
    tgts = targets.expand(args.targets, base)
    save_results(args, run_checks(args, base, tgts, headers=_whole(args)),
                 partial=not _whole(args))


def cmd_all(args):
    base = make_tree(args)
    cmd_setup(args, base)
    tgts = targets.expand(args.targets, base)
    save_results(args, run_checks(args, base, tgts, rounds=args.rounds,
                                  headers=_whole(args)),
                 partial=not _whole(args))
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


_STAND_IN_CACHE = {}


def _gen_stand_in(gen_dir, rel):
    """Whether a generated file is one of gen_misc's stand-ins (marked in
    its first line)."""
    key = (str(gen_dir), rel)
    if key not in _STAND_IN_CACHE:
        try:
            with open(Path(gen_dir) / rel, "rb") as f:
                head = f.read(200)
        except OSError:
            head = b""
        _STAND_IN_CACHE[key] = b"compile_check STAND-IN" in head
    return _STAND_IN_CACHE[key]


# An error that names an Apple framework declaration the stand-ins lack
# (mac_stand_ins/ declares what the checked files use; the rest of the SDK
# is Xcode's): the file can't be checked here.
_APPLE_SYMBOL = (r"'((?:NS|CF|CG|CT|Sec|CA|AV|IOSurface|MTL|LS|UT|AX|"
                 r"kCF|kCT|kCG|kSec|kAX)[A-Z_]\w*)")
APPLE_MISSING_RE = re.compile(
    r"^(?:unknown type name |use of undeclared identifier |no visible "
    r"@interface for |cannot find interface declaration for |cannot find "
    r"protocol declaration for |no type or protocol named |unknown "
    r"receiver |no known (?:class|"
    r"instance) method for selector .* on |receiver type |property .* not "
    r"found on object of type |use of undeclared identifier |incomplete "
    r"definition of type )" + _APPLE_SYMBOL)


# Chromium's clang plugins prefix what they report: [chromium-style]
# (find-bad-constructs), [chromium-rawptr], [chromium-raw-ref], ...
PLUGIN_DIAG_RE = re.compile(r"^\[chromium-[a-z-]+\]")


def plugin_diags(r):
    """The plugin diagnostics of a result that are in MoltBrowser's code:
    a file of the src/ overlay (new, or an upstream copy it changes);
    Chromium's own files are its business."""
    out = []
    for d in r["diags"]:
        if not PLUGIN_DIAG_RE.match(d["msg"]):
            continue
        f = d["file"]
        if f.startswith(str(targets.SRC) + "/"):
            f = f[len(str(targets.SRC)) + 1:]
        if not f.startswith("/") and (targets.SRC / f).is_file():
            out.append(dict(d, file=f))
    return out


def apple_sdk_gaps(errs):
    """The Apple framework names the errors say are undeclared (in order),
    or []."""
    out = []
    for d in errs:
        m = APPLE_MISSING_RE.search(d["msg"])
        if m and m.group(1) not in out:
            out.append(m.group(1))
    return out


MOJOM_HEADER_RE = re.compile(r"^(.+\.mojom)(-[a-z-]+)?\.h$")
FRAMEWORK_HEADER_RE = re.compile(r"^([A-Z][A-Za-z0-9]*)/[A-Za-z0-9_+]+\.h$")


def generator_failures(state_dir):
    """{generated name or .mojom: why it couldn't be made}, from the
    platform's gen_report.json."""
    p = Path(state_dir) / "gen_report.json"
    if not p.exists():
        return {}
    rep = json.loads(p.read_text())
    out = {}
    for sec in ("buildflags", "actions", "misc", "mojom"):
        for k, v in ((rep.get(sec) or {}).get("failed") or {}).items():
            if isinstance(v, str):
                out.setdefault(k, v)
    return out


def why_missing(name, platform, failures, gn_target=True):
    """What `report` adds to "header not available here: <name>"."""
    why = failures.get(name)
    m = MOJOM_HEADER_RE.match(name)
    if why is None and m:
        why = failures.get(m.group(1))
    if why == "no action found":
        why = None  # not generated: a system or third-party header
    fw = FRAMEWORK_HEADER_RE.match(name)
    if platform == "mac" and fw and \
            not (sdks.MAC_STAND_INS / fw.group(1)).is_dir():
        why = "an Apple framework of the macOS SDK; mac_stand_ins/ has " \
            "no stand-in for it"
    out = " (%s)" % why[:200] if why else ""
    if not gn_target:
        out += "; no GN target lists the file with %s" % (
            platforms.get(platform).args_file)
    return out


def classify(r, graph, gen_dir, stand_in_mounts=None, sdk_stand_ins=None,
             failures=None):
    """(status, detail, stand-ins) for one check result. The stand-ins are
    the ones the file's include closure (over-approximated, like the scan)
    reaches: grit headers marked STAND-IN, and the subprojects whose tree is
    not the pinned one (`stand_in_mounts`, {mount: label}, from
    sources.stand_in_mounts); sdk_stand_ins names the platform's system
    headers when they are stand-ins (sdks.stand_in_label). The
    toolchain-wide substitutes (README) apply to every file. failures:
    generator_failures of the file's platform, to say why a header is
    missing."""
    if r.get("skipped"):
        return "not checked", r["skipped"], []
    if r.get("via"):
        st, detail, si = classify(dict(r, via=None), graph, gen_dir,
                                  stand_in_mounts, sdk_stand_ins, failures)
        return st, "; ".join(x for x in (
            "through %s" % r["via"], detail) if x), si
    errs = [d for d in r["diags"] if "error" in d["kind"]]
    missing = [d for d in errs if d["kind"] == "fatal error" and
               "file not found" in d["msg"]]
    stand_ins = []
    grit = []
    other = []
    mounts = sorted((stand_in_mounts or {}).items())
    reach = _reach(graph, "src:" + r["target"])
    for n in reach:
        if n.startswith("gen:"):
            if _gen_stand_in(gen_dir, n[4:]):
                (grit if "/grit/" in n else other).append(
                    n.rsplit("/", 1)[-1])
        else:
            for mount, label in mounts:
                if n.startswith("src:" + mount + "/"):
                    stand_ins.append(label)
            for prefix, (_, mark, label) in config.STAND_IN_FLAGS.items():
                if n == "src:" + mark and r["target"].startswith(prefix):
                    stand_ins.append(label)
    if grit:
        stand_ins.append("grit stand-in (%s)" % ", ".join(sorted(set(grit))))
    for name in sorted(set(other)):
        stand_ins.append("%s stand-in" % name)
    if sdk_stand_ins:
        stand_ins.append(sdk_stand_ins)
    stand_ins = sorted(set(stand_ins))
    if missing:
        name = missing[0]["msg"].split("'")[1] if "'" in missing[0]["msg"] \
            else missing[0]["msg"]
        return "not checked", "header not available here: " + name + \
            why_missing(name, r.get("platform") or platforms.DEFAULT,
                        failures or {}, "gn_target" not in r or
                        r["gn_target"] is not None), stand_ins
    gaps = apple_sdk_gaps(errs) if r.get("platform") == "mac" else []
    if gaps:
        return "not checked", "needs macOS SDK declarations mac_stand_ins/ " \
            "lacks: %s%s" % (", ".join(gaps[:4]), ", ..." if len(gaps) > 4
                             else ""), stand_ins
    if errs:
        # With a stand-in repository in reach, an error can be the
        # stand-in's (an API it changed): say so, for a human to judge.
        return "ERRORS", "%d error(s)%s" % (
            len(errs), "; reaches " + ", ".join(stand_ins)
            if stand_ins else ""), stand_ins
    plug = plugin_diags(r)
    if plug:
        # Warnings in these configs (treat_warnings_as_errors = false),
        # errors in a -Werror build such as Chromium's CQ.
        kinds = sorted({PLUGIN_DIAG_RE.match(d["msg"]).group(0)
                        for d in plug})
        return "PLUGIN", "%d %s diagnostic(s) in MoltBrowser code" % (
            len(plug), "/".join(kinds)), stand_ins
    if stand_ins:
        return "partly on stand-ins", ", ".join(stand_ins), stand_ins
    return "checked", "", stand_ins


def cmd_report(args):
    res = json.loads((args.work / "check.json").read_text())
    mounts = sources.stand_in_mounts(args.work)
    graphs = {}

    def graph_of(name):
        if name not in graphs:
            plat = platforms.get(name)
            p = plat.state_dir(args.work) / "graph.json"
            graphs[name] = json.loads(p.read_text()) if p.exists() else {}
        return graphs[name]

    fails = {}

    def failures_of(name):
        if name not in fails:
            fails[name] = generator_failures(
                platforms.get(name).state_dir(args.work))
        return fails[name]

    counts = {}
    rows = []
    for r in res:
        name = r.get("platform", platforms.DEFAULT) or platforms.DEFAULT
        plat = platforms.get(name)
        gen_dir = args.work / "chromium" / "src" / plat.out_rel / "gen"
        status, detail, _ = classify(
            r, graph_of(name) if not r.get("skipped") else {}, gen_dir,
            mounts, sdks.stand_in_label(name, r.get("lang")),
            failures_of(name))
        counts[status] = counts.get(status, 0) + 1
        warns = sum(1 for d in r["diags"] if d["kind"] == "warning")
        rows.append((r["target"], status, detail, warns,
                     r.get("platform") or "-"))
        if args.markdown:
            continue
        print("%-80s %-11s %-20s %s" % (r["target"], r.get("platform") or
                                         "-", status, detail))
        if status == "ERRORS":
            for d in [d for d in r["diags"] if "error" in d["kind"]][
                    :args.max_lines]:
                print("    %s:%d: %s" % (d["file"], d["line"],
                                         d["msg"][:160]))
        if status == "PLUGIN":
            for d in plugin_diags(r)[:args.max_lines]:
                print("    %s:%d: %s" % (d["file"], d["line"],
                                         d["msg"][:160]))
    if args.markdown:
        print("| File | Platform | Result | Detail |")
        print("|---|---|---|---|")
        for t, st, det, _, plat in rows:
            print("| `%s` | %s | %s | %s |" % (t, plat, st, det))
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
    for name in ("fetch", "gen"):
        f = sub.add_parser(name)
        f.add_argument("targets", nargs="*")
    for name in ("check", "all"):
        c = sub.add_parser(name)
        c.add_argument("targets", nargs="*")
        c.add_argument("--jobs", type=int, default=2)
        c.add_argument("--no-plugins", action="store_true",
                       help="skip Chromium's clang plugins")
        c.add_argument("--dev-plugins", action="store_true",
                       help="also the raw_ptr/raw_ref field checks a "
                       "non-official build runs")
        c.add_argument("--rounds", type=int, default=6)
        c.add_argument("--platforms", type=lambda v: v.split(","),
                       help="only these platforms (comma-separated: %s)" %
                       ",".join(platforms.all_names()))
        if name == "all":
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
