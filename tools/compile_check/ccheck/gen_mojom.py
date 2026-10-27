# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Mojom C++ bindings, made by Chromium's own generator.

1. Every mojom() target in every BUILD.gn that mentions a .mojom file is read
   with the GN reader: its sources, enabled_features, and its cpp_typemaps /
   blink_cpp_typemaps (which become one global typemap per variant, the
   union of what GN would hand each target through its deps).
2. The .mojom files the checked code needs, plus their imports, are copied
   into one input root (overlay files win) and parsed with
   mojo/public/tools/mojom/mojom_parser.py.
3. mojo/public/tools/bindings/mojom_bindings_generator.py writes the shared
   headers, the message ids and the chromium (and, when asked for, blink)
   variant, exactly as the GN mojom() template invokes it (minus export
   macros, which are empty in a static build, and message-id scrambling).

Outputs are keyed by content (mojom_stamps.json): the .mojom and every file
it imports, transitively, the typemaps and the target's enabled_features.
refresh() regenerates every output whose key changed (an edited .mojom, an
edited import, a typemap change) and deletes the outputs of a .mojom that
is gone, so stale bindings are never compiled against.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from . import gn
from .sources import log

MOJOM_TEMPLATES = ("mojom", "mojom_component")
SUFFIX_RE = re.compile(r"^(?P<base>.+\.mojom)(?P<suffix>(-[a-z-]+)?)\.h$")
IMPORT_RE = re.compile(r'^\s*import\s+"([^"]+)"\s*;', re.M)
# An import with the attribute list before it ([EnableIf=is_win]).
_ATTR_IMPORT_RE = re.compile(
    r'^\s*(?:\[([^\]]*)\]\s*)?import\s+"([^"]+)"\s*;', re.M)
TYPE_KEYS = ("mojom", "cpp", "copyable_pass_by_value", "force_serialize",
             "hashable", "move_only", "non_const_ref",
             "nullable_is_same_type", "forward_declaration",
             "default_constructible")


def _norm(path, base_dir):
    if path.startswith("//"):
        return path[2:]
    parts = [p for p in base_dir[2:].split("/") if p]
    for c in path.split("/"):
        if c == "..":
            if parts:
                parts.pop()
        elif c not in (".", ""):
            parts.append(c)
    return "/".join(parts)


def _scope_to_dict(s):
    if isinstance(s, gn.Scope):
        return {k: _scope_to_dict(v) for k, v in s.vars.items()}
    if isinstance(s, list):
        return [_scope_to_dict(x) for x in s]
    return s


class Index:
    """mojom path -> target info, and the typemaps."""

    def __init__(self, data):
        self.data = data

    @property
    def mojoms(self):
        return self.data["mojoms"]

    @property
    def typemaps(self):
        return self.data["typemaps"]


def build_index(tree, interp, cache_path):
    """Reads every BUILD.gn mentioning .mojom; cached by the content hash of
    those files."""
    files = sorted(set(tree.chromium_files(
        lambda p: p.endswith("BUILD.gn")) + [
            p for p in tree.overlay_set() if p.endswith("BUILD.gn")]))
    h = hashlib.sha1()
    relevant = []
    for p in files:
        data = tree.read(p)
        if data is None or (b".mojom" not in data and
                            b"cpp_typemaps" not in data):
            continue
        relevant.append(p)
        h.update(p.encode() + b"\0" + hashlib.sha1(data).digest())
    key = h.hexdigest()
    if cache_path.exists():
        cached = json.loads(cache_path.read_text())
        if cached.get("key") == key:
            return Index(cached)
    log("mojom: reading %d BUILD.gn files" % len(relevant))
    mojoms = {}
    typemaps = {"chromium": {}, "blink": {}}
    conflicts = []
    for p in relevant:
        try:
            targets = interp.run_file("//" + p)
        except Exception as e:  # noqa: BLE001 - keep going
            log("mojom: %s: %s" % (p, e))
            continue
        for t in targets:
            srcs = t.get("sources")
            tm_c = t.get("cpp_typemaps")
            tm_b = t.get("blink_cpp_typemaps")
            if t.func not in MOJOM_TEMPLATES and not (
                    isinstance(srcs, list) and any(
                        isinstance(s, str) and s.endswith(".mojom")
                        for s in srcs)):
                continue
            feats = t.get("enabled_features")
            info = {
                "target": "%s:%s" % (t.path, t.name),
                "enabled_features": [f for f in feats if isinstance(f, str)]
                if isinstance(feats, list) else [],
                "import_dirs": [_norm(d, t.path) for d in
                                (t.get("import_dirs") or [])
                                if isinstance(d, str)]
                if isinstance(t.get("import_dirs"), list) else [],
            }
            if isinstance(srcs, list):
                for s in srcs:
                    if isinstance(s, str) and s.endswith(".mojom"):
                        n = _norm(s, t.path)
                        if n.startswith(gn.OUT[2:] + "/gen/"):
                            n = n[len(gn.OUT) - 2 + 5:]
                        mojoms.setdefault(n, info)
            for variant, tms in (("chromium", tm_c), ("blink", tm_b)):
                if not isinstance(tms, list):
                    continue
                for cfg in tms:
                    cfg = _scope_to_dict(cfg)
                    if not isinstance(cfg, dict):
                        continue
                    pub = [_norm(x, t.path) for x in
                           cfg.get("traits_headers", []) or []
                           if isinstance(x, str)]
                    priv = [_norm(x, t.path) for x in
                            cfg.get("traits_private_headers", []) or []
                            if isinstance(x, str)]
                    for ty in cfg.get("types", []) or []:
                        if not isinstance(ty, dict) or \
                                not isinstance(ty.get("mojom"), str) or \
                                not isinstance(ty.get("cpp"), str):
                            continue
                        entry = {
                            "typename": ty["cpp"],
                            "forward_declaration":
                                ty.get("forward_declaration"),
                            "public_headers": pub,
                            "traits_headers": priv,
                            "copyable_pass_by_value":
                                ty.get("copyable_pass_by_value", False),
                            "default_constructible":
                                ty.get("default_constructible", True),
                            "force_serialize":
                                ty.get("force_serialize", False),
                            "hashable": ty.get("hashable", False),
                            "move_only": ty.get("move_only", False),
                            "non_const_ref": ty.get("non_const_ref", False),
                            "nullable_is_same_type":
                                ty.get("nullable_is_same_type", False),
                            "non_copyable_non_movable": False,
                        }
                        prev = typemaps[variant].get(ty["mojom"])
                        if prev and prev["typename"] != entry["typename"]:
                            conflicts.append("%s %s: %s vs %s" % (
                                variant, ty["mojom"], prev["typename"],
                                entry["typename"]))
                            continue
                        typemaps[variant][ty["mojom"]] = entry
    data = {"key": key, "mojoms": mojoms, "typemaps": typemaps,
            "conflicts": conflicts}
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    cache_path.write_text(json.dumps(data, indent=1, sort_keys=True))
    for c in conflicts[:20]:
        log("mojom: typemap conflict (first wins): " + c)
    return Index(data)


def wanted_mojoms(names):
    """Missing header names -> {mojom path: set(variants)}."""
    out = {}
    for n in names:
        m = SUFFIX_RE.match(n)
        if not m:
            continue
        suffix = m.group("suffix")
        variant = "blink" if suffix.startswith("-blink") else "chromium"
        out.setdefault(m.group("base"), set()).add(variant)
    return out


def imports_of(text, features=None):
    """The files a .mojom imports. With `features` (the set its parse is
    given), only the imports the parser keeps: an import marked
    [EnableIf=f] needs f, one marked [EnableIfNot=f] needs f to be off,
    as mojom's conditional_features.py filters them."""
    out = []
    for m in _ATTR_IMPORT_RE.finditer(text):
        keep = True
        for attr in (m.group(1) or "").split(","):
            k, _, v = (x.strip() for x in attr.partition("="))
            if features is None or not v:
                continue
            if k == "EnableIf" and v not in features:
                keep = False
            elif k == "EnableIfNot" and v in features:
                keep = False
        if keep:
            out.append(m.group(2))
    return out


def import_closure(tree, gen_dir, roots, produce=None, feats_of=None):
    """{path: content or None} for `roots` and what they import (with
    feats_of(path), only the imports its parse keeps)."""
    seen = {}
    todo = list(roots)
    while todo:
        p = todo.pop()
        if p in seen:
            continue
        data = tree.read(p)
        if data is None and not (Path(gen_dir) / p).exists() and produce:
            produce([p])
        if data is None and (Path(gen_dir) / p).exists():
            data = (Path(gen_dir) / p).read_bytes()
        seen[p] = data
        if data is None:
            continue
        todo += imports_of(data.decode("utf-8", "replace"),
                           feats_of(p) if feats_of else None)
    return seen


def _read_mojom(tree, gen_dir, p):
    data = tree.read(p)
    if data is None and (Path(gen_dir) / p).exists():
        data = (Path(gen_dir) / p).read_bytes()
    return data


def closure_hash(tree, gen_dir, p, memo):
    """sha1 over the content of `p` and, transitively, of every .mojom it
    imports (None if `p` itself can't be read). An import that can't be
    read counts as such, so its appearing later changes the hash."""
    if p in memo:
        return memo[p]
    memo[p] = "cycle"  # a (malformed) import cycle ends here
    data = _read_mojom(tree, gen_dir, p)
    if data is None:
        memo[p] = None
        return None
    h = hashlib.sha1(hashlib.sha1(data).digest())
    for imp in sorted(set(IMPORT_RE.findall(data.decode("utf-8",
                                                        "replace")))):
        h.update(b"\0" + imp.encode() + b"=" + str(
            closure_hash(tree, gen_dir, imp, memo)).encode())
    memo[p] = h.hexdigest()
    return memo[p]


def typemap_hash(index):
    return hashlib.sha1(json.dumps(index.typemaps, sort_keys=True)
                        .encode()).hexdigest()


def stamp_key(index, p, closure, tm_hash):
    """What a .mojom's outputs depend on: its import closure's content, the
    typemaps, its target's enabled_features."""
    feats = sorted(index.mojoms.get(p, {}).get("enabled_features", []))
    return "%s:%s:%s" % (closure, tm_hash, hashlib.sha1(
        "\0".join(feats).encode()).hexdigest()[:12])


def outputs_of(gen_dir, p):
    """The generated files of one .mojom (foo.mojom.h, foo.mojom-*.h, ...,
    foo.mojom-module)."""
    d = Path(gen_dir) / p
    base = d.name
    if not d.parent.is_dir():
        return []
    return sorted(f for f in d.parent.iterdir()
                  if f.name.startswith((base + ".", base + "-")))


def stale(tree, index, work, gen_dir):
    """({mojom: variants} whose outputs are out of date, [mojoms whose
    source is gone])."""
    stamp_path = Path(work) / "mojom_stamps.json"
    stamps = json.loads(stamp_path.read_text()) if stamp_path.exists() \
        else {}
    tm_hash = typemap_hash(index)
    memo = {}
    out, gone = {}, []
    for p, have in sorted(stamps.items()):
        c = closure_hash(tree, gen_dir, p, memo)
        if c is None:
            gone.append(p)
            continue
        if have.get("key") != stamp_key(index, p, c, tm_hash):
            out[p] = set(have.get("v", ["chromium"]))
    return out, gone


def refresh(tree, index, work, gen_dir, produce=None, features=None):
    """Regenerates every stamped .mojom whose key changed and removes the
    outputs of one whose source is gone. Returns (regenerated, removed,
    {mojom: reason})."""
    want, gone = stale(tree, index, work, gen_dir)
    stamp_path = Path(work) / "mojom_stamps.json"
    if gone:
        stamps = json.loads(stamp_path.read_text())
        for p in gone:
            for f in outputs_of(gen_dir, p):
                f.unlink()
            stamps.pop(p, None)
            log("mojom: %s is gone; removed its outputs" % p)
        stamp_path.write_text(json.dumps(stamps, indent=1, sort_keys=True))
    if not want:
        return [], gone, {}
    log("mojom: %d stale output set(s), regenerating" % len(want))
    ok, failed = generate(tree, index, work, gen_dir, want, produce=produce,
                          features=features)
    return ok, gone, failed


def _py(script, args, cwd):
    return subprocess.run([sys.executable, "-E", "-s", str(script)] + args,
                          cwd=cwd, capture_output=True, text=True)


# The features mojom.gni enables on Linux (platforms.Platform has each
# platform's).
LINUX_FEATURES = ("is_posix", "is_linux")


def _parse(parser, root, gen_dir, work, files, feats):
    rsp = Path(work) / "mojom_parse.rsp"
    rsp.write_text("\n".join(str(root / p) for p in sorted(files)))
    # One input root: generated .mojom files were copied into it too (the
    # parser prefers the longest root, which would split a module in two).
    args = ["--input-root", str(root),
            "--output-root", str(gen_dir), "--mojom-file-list=" + str(rsp)]
    for f in sorted(feats):
        args += ["--enable-feature", f]
    return _py(parser, args, cwd=str(work))


def _last_line(text):
    lines = text.strip().splitlines()
    return lines[-1][:300] if lines else "?"


def parse_in_order(deps, feats_of, parse):
    """Parses each .mojom the way its own mojom() target does: with that
    target's features (feats_of(path) -> frozenset), after the modules it
    imports (deps: {path: [imported paths among those to parse]}), in
    batches of files with the same features whose imports are all parsed.
    parse(paths, features) -> None, or the error. A batch that fails is
    parsed file by file to find the bad ones; a file importing a bad one
    fails too. Returns (parsed, {path: error})."""
    done, failed = set(), {}
    remaining = set(deps)
    while remaining:
        ready = sorted(p for p in remaining
                       if all(d in done or d in failed for d in deps[p]))
        if not ready:
            # An import cycle (malformed): parse the rest together.
            ready = sorted(remaining)
        remaining -= set(ready)
        groups = {}
        for p in ready:
            bad = [d for d in deps[p] if d in failed]
            if bad:
                failed[p] = "imports %s, which failed" % bad[0]
            else:
                groups.setdefault(feats_of(p), []).append(p)
        for feats, group in sorted(groups.items(), key=lambda kv: sorted(
                kv[0])):
            err = parse(group, feats)
            if err is None:
                done.update(group)
                continue
            for p in group if len(group) > 1 else []:
                e1 = parse([p], feats)
                if e1 is None:
                    done.add(p)
                else:
                    failed[p] = e1
            if len(group) == 1:
                failed[group[0]] = err
    return done, failed


def generate(tree, index, work, gen_dir, wanted, produce=None,
             features=None):
    """wanted: {mojom path: variants}. Returns (generated mojoms,
    {mojom: reason}). produce(gen_rel_paths) makes generated .mojom files.
    features: the platform's mojom.gni features (LINUX_FEATURES by
    default)."""
    work = Path(work)
    gen_dir = Path(gen_dir)
    failed = {}
    base = frozenset(LINUX_FEATURES if features is None else features)

    def feats_of(p):
        return base | frozenset(index.mojoms.get(p, {}).get(
            "enabled_features", []))

    files = import_closure(tree, gen_dir, sorted(wanted), produce, feats_of)
    for p, data in files.items():
        if data is None:
            failed[p] = "mojom source not found"
    files = {p: d for p, d in files.items() if d is not None}
    root = work / "mojom_src"
    for p, data in files.items():
        dst = root / p
        if not dst.exists() or dst.read_bytes() != data:
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)
    variants = set()
    for v in wanted.values():
        variants |= v
    # What is already generated from the same input?
    stamp_path = work / "mojom_stamps.json"
    stamps = json.loads(stamp_path.read_text()) if stamp_path.exists() \
        else {}
    tm_hash = typemap_hash(index)
    memo = {}
    todo = []
    for p, data in sorted(files.items()):
        want_v = sorted(wanted.get(p, {"chromium"}) | {"chromium"})
        key = stamp_key(index, p, closure_hash(tree, gen_dir, p, memo),
                        tm_hash)
        have = stamps.get(p, {})
        if have.get("key") == key and set(want_v) <= set(have.get("v", [])) \
                and outputs_of(gen_dir, p):
            continue
        # Keep variants generated before (a blink header someone
        # includes stays fresh too).
        want_v = sorted(set(want_v) | set(have.get("v", [])))
        todo.append((p, want_v, key))
    if not todo:
        return [], failed
    tools = tree.tree_dir / "mojo" / "public" / "tools"
    parser = tools / "mojom" / "mojom_parser.py"
    generator = tools / "bindings" / "mojom_bindings_generator.py"
    if not parser.exists() or not generator.exists():
        # Not a mojom's fault: nothing can be generated (the stale outputs
        # go, so nothing compiles against them).
        for p, _, _ in todo:
            _drop(gen_dir, stamps, p)
        stamp_path.write_text(json.dumps(stamps, indent=1, sort_keys=True))
        raise RuntimeError("mojom generator not found: %s" % parser)
    # Each file with its own target's features (mojom.gni parses every
    # target's sources with its enabled_features plus the platform's), in
    # import order: one file the parser rejects (a ChromeOS-only mojom the
    # scan reached through a branch it could not rule out) fails only
    # itself and its importers.
    deps = {p: sorted(set(i for i in imports_of(
        d.decode("utf-8", "replace"), feats_of(p)) if i in files))
        for p, d in files.items()}

    def parse(group, feats):
        r = _parse(parser, root, gen_dir, work, {p: files[p] for p in group},
                   feats)
        return None if r.returncode == 0 else _last_line(r.stderr)

    log("mojom: parsing %d file(s)" % len(files))
    parsed, bad = parse_in_order(deps, feats_of, parse)
    for p, err in sorted(bad.items()):
        if p in wanted or not failed.get(p):
            failed[p] = "mojom parser: " + err
    if bad:
        log("mojom: %d file(s) the parser rejects (gen_report.json)" %
            len(bad))
        for p, _, _ in todo:
            if p in bad:
                _drop(gen_dir, stamps, p)
        todo = [x for x in todo if x[0] not in bad]
        stamp_path.write_text(json.dumps(stamps, indent=1, sort_keys=True))
        if not todo:
            return [], failed
    bytecode = gen_dir / "mojo" / "public" / "tools" / "bindings"
    if not (bytecode / "cpp_templates.zip").exists():
        bytecode.mkdir(parents=True, exist_ok=True)
        r = _py(generator, ["--use_bundled_pylibs", "-o", str(bytecode),
                            "precompile", "-g", "c++"], cwd=str(work))
        if r.returncode != 0:
            log(r.stderr[-3000:])
            raise RuntimeError("mojom template precompile failed")
    tmdir = work / "typemaps"
    tmdir.mkdir(exist_ok=True)
    for v in ("chromium", "blink"):
        (tmdir / (v + ".json")).write_text(json.dumps({
            "c++": index.typemaps.get(v, {}), "typescript": {},
            "_metadata": {"module_typemaps": {"c++": [], "typescript": []}}},
            indent=1))
    gen_list = [p for p, _, _ in todo]
    common = ["--use_bundled_pylibs", "-o", str(gen_dir), "generate", "-d",
              str(root), "-I", str(root), "--bytecode_path", str(bytecode)]
    runs = [
        ("message ids", ["--generate_non_variant_code",
                         "--generate_message_ids", "-g", "c++"], gen_list),
        ("shared", ["--generate_non_variant_code", "-g", "c++"], gen_list),
        ("chromium", ["-g", "c++", "--typemap",
                      str(tmdir / "chromium.json")], gen_list),
    ]
    blink = [p for p, v, _ in todo if "blink" in v]
    if blink:
        runs.append(("blink", ["-g", "c++", "--variant", "blink",
                               "--typemap", str(tmdir / "blink.json"),
                               "--for_blink"], blink))
    ok = set(gen_list)
    for name, extra, lst in runs:
        # One invocation per file keeps one bad module from failing them all.
        log("mojom: generating %s for %d file(s)" % (name, len(lst)))
        batch = work / "mojom_gen.rsp"
        batch.write_text("\n".join(str(root / p) for p in lst))
        r = _py(generator, common + ["--filelist", str(batch)] + extra,
                cwd=str(work))
        if r.returncode != 0 and "TemplateNotFound" in r.stderr:
            log(r.stderr[-2000:])
            raise RuntimeError("mojom generator: templates missing")
        if r.returncode != 0:
            # Retry one by one to find the bad ones.
            for p in lst:
                batch.write_text(str(root / p))
                r1 = _py(generator, common + ["--filelist", str(batch)] +
                         extra, cwd=str(work))
                if r1.returncode != 0:
                    failed[p] = "%s: %s" % (name, r1.stderr.strip()
                                            .splitlines()[-1][:300]
                                            if r1.stderr.strip() else "?")
                    ok.discard(p)
    for p, v, key in todo:
        if p in ok:
            stamps[p] = {"key": key, "v": v}
        else:
            _drop(gen_dir, stamps, p)
    stamp_path.write_text(json.dumps(stamps, indent=1, sort_keys=True))
    return sorted(ok), failed


def _drop(gen_dir, stamps, p):
    """Outputs that could not be regenerated go: a check then reports the
    header as missing instead of compiling against old bindings."""
    for f in outputs_of(gen_dir, p):
        f.unlink()
    stamps.pop(p, None)
