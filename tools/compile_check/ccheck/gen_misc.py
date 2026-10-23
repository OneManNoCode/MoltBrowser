# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Generated headers no GN action here can produce as is, written directly.

grit resource headers (grit/*.h) are made by Chromium's own grit
(grit_headers.py), run with the args of the action the .grd's grit()
target expands to and the first-ids file the build makes
(tools/gritsettings:default_resource_ids). Only the headers are written,
not the .pak files. A header is remade when its .grd, a <part> of it, the
first-ids file or its args change (grit_stamps.json).

STAND-INS. Each writer below approximates what the build generates; checks
that include its output rest partly on a stand-in (the report lists them):

  WebUI grit  chrome/grit/*_resources{,_map}.h of a WebUI whose .grd the
              build generates from its TypeScript build (which can't run
              here): the map declaration, and the IDR_<STEM>_* names the
              includers use.
  grit        only if grit itself fails on a .grd: every <message>,
              <include> and <structure> name in it and its <part> files,
              numbered from a per-header base.
  build date  base/generated_build_date.h with a fixed date.

Real generators run here: .rs.h (cxx bridge headers) come from cxxbridge
built from Chromium's vendored cxxbridge-cmd.
"""
import hashlib
import json
import os
import posixpath
import re
import subprocess
import sys
from pathlib import Path

from . import gn
from .gen_actions import _build_files_mentioning
from .sources import log

GRIT_RE = re.compile(r"(^|/)grit/[^/]+\.h$")
NAME_RE = re.compile(rb'<(message|include|structure)\b[^>]*?\bname="([A-Za-z0-9_]+)"',
                     re.S)
PART_RE = re.compile(rb'<part\s+file="([^"]+)"')


HERE = Path(__file__).resolve().parent

# Lazily built host tools, set by the driver: name -> function returning a
# path.
TOOLS = {}


def handles_exclusively(name):
    """Names no GN action here could make (skip looking for one)."""
    return bool(GRIT_RE.search(name)) or name in WRITERS or \
        name.endswith(".rs.h")


def handles(name):
    return bool(GRIT_RE.search(name)) or name in WRITERS or \
        name.endswith((".rs.h", "_generated.h"))


def flatbuffer_header(tree, interp, name):
    """Runs flatc the way the flatbuffer() template does."""
    stem = name.rsplit("/", 1)[-1][:-len("_generated.h")]
    want = gn.OUT + "/gen/" + name
    for bf in _build_files_mentioning(tree, stem + ".fbs", limit=80):
        for t in interp.run_file("//" + bf):
            if t.func != "flatbuffer":
                continue
            for s in t.get("sources") or []:
                if not isinstance(s, str) or not s.endswith(stem + ".fbs"):
                    continue
                src = interp.abspath(s, {"dir": t.path})
                fod = t.get("flatc_out_dir")
                if isinstance(fod, str):
                    od = gn.OUT + "/gen/" + t.path[2:] + "/" + fod
                else:
                    od = gn.OUT + "/gen/" + src[2:].rsplit("/", 1)[0]
                od = interp.abspath(od, {"dir": "//"})
                if od + "/" + stem + "_generated.h" != want:
                    continue
                tree.materialize([src[2:]] + [
                    p for p in tree.chromium_files(
                        lambda p: p.endswith(".fbs"))])
                args = ["-c", "--keep-prefix", "-o",
                        str(tree.tree_dir / od[2:]), "-I",
                        str(tree.tree_dir)]
                for d in t.get("flatc_include_dirs") or []:
                    if isinstance(d, str):
                        args += ["-I", str(tree.tree_dir / interp.abspath(
                            d, {"dir": t.path})[2:])]
                if t.get("mutable") is True:
                    args.append("--gen-mutable")
                extra = t.get("args")
                if isinstance(extra, list):
                    args += [a for a in extra if isinstance(a, str)]
                exe = TOOLS["flatc"]()
                r = subprocess.run([str(exe)] + args +
                                   [str(tree.tree_dir / src[2:])],
                                   capture_output=True, text=True)
                if r.returncode != 0:
                    raise RuntimeError("flatc: " + r.stderr[-1000:])
                return None  # flatc wrote it
    raise RuntimeError("no flatbuffer() target writes %s" % name)


def cxx_bridge_header(tree, interp, name):
    src = name[:-2]
    if not tree.exists(src):
        raise RuntimeError("%s not found" % src)
    tree.materialize([src])
    exe = TOOLS["cxxbridge"]()
    r = subprocess.run([str(exe), str(tree.tree_dir / src), "--header"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("cxxbridge: " + r.stderr[-1000:])
    return r.stdout


def _find_grit_target(tree, interp, name):
    stem = name.rsplit("/", 1)[-1][:-2]
    want = gn.OUT + "/gen/" + name
    for bf in _build_files_mentioning(tree, "grit/%s.h" % stem, limit=80):
        for t in interp.run_file("//" + bf):
            if t.func not in ("grit", "grit_strings", "chrome_grit"):
                continue
            outs = t.get("outputs")
            od = t.get("output_dir")
            if not isinstance(od, str):
                od = gn.OUT + "/gen/" + t.path[2:]
            od = interp.abspath(od, {"dir": t.path})
            if not isinstance(outs, list):
                continue
            for o in outs:
                if isinstance(o, str) and \
                        interp.abspath(o, {"dir": od}) == want:
                    return t
    return None


def _grd_names(tree, interp, path, seen=None):
    seen = seen if seen is not None else set()
    if path in seen:
        return []
    seen.add(path)
    data = tree.read(path)
    if data is None:
        log("grit stand-in: %s not found" % path)
        return []
    names = [m.group(2).decode() for m in NAME_RE.finditer(data)]
    d = path.rsplit("/", 1)[0]
    for m in PART_RE.finditer(data):
        part = interp.abspath(m.group(1).decode(), {"dir": "//" + d})
        names += _grd_names(tree, interp, part[2:], seen)
    return names


def _camel(stem):
    return "k" + "".join(w[:1].upper() + w[1:] for w in stem.split("_"))


def webui_grit_header(tree, name, includers):
    """A WebUI's resources come from a .grd the build generates from its
    TypeScript build, so no source .grd lists them: the map header
    declares the map (as grit's resource_map_header does) and the ID
    header the IDR_<STEM>_* names its includers use."""
    stem = name.rsplit("/", 1)[-1][:-2]
    head = ["// compile_check STAND-IN for grit's %s (its .grd is generated "
            "by a WebUI build)." % name, "#pragma once", ""]
    if stem.endswith("_map"):
        return "\n".join(head + [
            '#include "ui/base/webui/resource_path.h"', "",
            "extern const webui::ResourcePath %s[1];" % _camel(stem[:-4]),
            ""])
    prefix = "IDR_" + stem[:-len("_resources")].upper() + "_" \
        if stem.endswith("_resources") else "IDR_" + stem.upper() + "_"
    names = set()
    for inc in includers or []:
        if inc.startswith("gen:"):
            continue
        data = tree.read(inc)
        if data:
            names |= set(re.findall(r"\b(%s[A-Z0-9_]+)\b" % re.escape(
                prefix), data.decode("utf-8", "replace")))
    base = 10000 + int(hashlib.sha1(name.encode()).hexdigest()[:4], 16) * 4
    return "\n".join(head + ["#define %s %d" % (n, base + i)
                             for i, n in enumerate(sorted(names))]) + "\n"


GRIT_STAMPS = "grit_stamps.json"
RESOURCE_IDS = "tools/gritsettings/default_resource_ids"
# Bump when grit_headers.py changes what it writes.
GRIT_DRIVER_VERSION = "1"


def _grd_closure(tree, path, seen=None):
    """{src path: bytes} of a .grd and, recursively, its <part> files."""
    seen = seen if seen is not None else {}
    if path in seen:
        return seen
    data = tree.read(path)
    seen[path] = data
    if data is None:
        return seen
    d = path.rsplit("/", 1)[0] if "/" in path else ""
    for m in PART_RE.finditer(data):
        part = m.group(1).decode()
        if "$" in part or part.startswith("<"):
            continue
        rel = posixpath.normpath(posixpath.join(d, part))
        if not rel.startswith(".."):
            _grd_closure(tree, rel, seen)
    return seen


def _grd_of(args):
    """The src-relative .grd a grit action's args name (-i), or None."""
    for i, a in enumerate(args[:-1]):
        if a == "-i":
            return os.path.normpath(os.path.join(gn.OUT[2:], args[i + 1]))
    return None


def _all_grd_files(tree):
    """Every .grd/.grdp (update_resource_ids counts the resources of each
    .grd resource_ids.spec lists, with its parts)."""
    paths = tree.chromium_files(lambda p: p.endswith((".grd", ".grdp")))
    paths += [p for p in tree.overlay_set() if p.endswith((".grd", ".grdp"))]
    return sorted(set(paths))


def _resource_ids_key(tree):
    h = hashlib.sha1()
    for p in ["tools/gritsettings/resource_ids.spec"] + _all_grd_files(tree):
        data = tree.read(p)
        h.update(p.encode() + b"\0" + hashlib.sha1(data or b"").digest())
    return h.hexdigest()


def _grit_key(tree, gen_dir, args):
    h = hashlib.sha1(GRIT_DRIVER_VERSION.encode())
    h.update("\0".join(args).encode())
    grd = _grd_of(args)
    for p, data in sorted(_grd_closure(tree, grd).items()) if grd else []:
        h.update(p.encode() + b"\0" + hashlib.sha1(data or b"").digest())
    ids = Path(gen_dir) / RESOURCE_IDS
    h.update(hashlib.sha1(ids.read_bytes()).digest() if ids.exists()
             else b"-")
    return h.hexdigest()


def _load_stamps(tree):
    try:
        return json.loads((tree.work / GRIT_STAMPS).read_text())
    except (OSError, ValueError):
        return {}


def _save_stamps(tree, stamps):
    (tree.work / GRIT_STAMPS).write_text(json.dumps(stamps, indent=1,
                                                    sort_keys=True))


def ensure_resource_ids(tree, interp, gen_dir):
    """gen/tools/gritsettings/default_resource_ids, made by its GN action
    (grit update_resource_ids over resource_ids.spec and every .grd it
    lists), and remade when any of those changed."""
    from .gen_actions import produce
    stamps = _load_stamps(tree)
    key = _resource_ids_key(tree)
    out = Path(gen_dir) / RESOURCE_IDS
    if out.exists() and stamps.get(RESOURCE_IDS, {}).get("key") == key:
        return out
    tree.materialize(_all_grd_files(tree) +
                     tree.glob_upstream("tools/gritsettings/"))
    if out.exists():
        out.unlink()
    w, failed = produce(tree, interp, [RESOURCE_IDS])
    if failed:
        raise RuntimeError("default_resource_ids: %s" %
                           failed[RESOURCE_IDS][-300:])
    stamps = _load_stamps(tree)
    stamps[RESOURCE_IDS] = {"key": key}
    _save_stamps(tree, stamps)
    return out


# grit build options (with a value) that only matter for outputs other than
# headers: the files ninja checks (expected outputs, depfile) and the tools
# for .pak contents (brotli, the CSS minifier, the Android zip).
_NINJA_ONLY = ("--assert-file-list", "--depfile", "--depdir", "--brotli",
               "--css-minifier", "--android-output-zip")


def _header_args(args):
    out = []
    skip = False
    for a in args:
        if skip:
            skip = False
            continue
        if a in _NINJA_ONLY:
            skip = True
            continue
        if isinstance(a, str) and (a.startswith(tuple(
                o + "=" for o in _NINJA_ONLY)) or a == "--depend-on-stamp"):
            continue
        out.append(a)
    return out


def run_grit(tree, gen_dir, args):
    """Chromium's grit with a grit() action's args, writing its headers
    (grit_headers.py)."""
    args = _header_args(args)
    grd = _grd_of(args)
    if grd is None:
        raise RuntimeError("grit args name no -i input")
    tree.materialize(list(_grd_closure(tree, grd)))
    tree.out_dir.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([sys.executable, "-E", "-s",
                        str(HERE / "grit_headers.py"),
                        str(tree.tree_dir / "tools" / "grit")] + args,
                       cwd=str(tree.out_dir), capture_output=True, text=True,
                       env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
    if r.returncode != 0:
        raise RuntimeError("grit: " + (r.stderr or r.stdout)[-600:])


def real_grit_header(tree, interp, name, t):
    """Writes gen/<name> with Chromium's grit, run with the args of the
    action the grit() target `t` expands to. Records its stamp."""
    from .gen_actions import find_action
    act = find_action(tree, interp, name, hint=t.path[2:] + "/BUILD.gn")
    if act is None or not str(act.get("script") or "").endswith("grit.py"):
        raise RuntimeError("the grit action for %s was not found" % name)
    args = act.get("args")
    if not isinstance(args, list):
        raise RuntimeError("grit args not computable")
    args = _header_args(args)
    bad = [a for a in args if not isinstance(a, str) or "<UNKNOWN>" in a]
    if bad:
        raise RuntimeError("grit args not computable: %r" % bad[:3])
    ensure_resource_ids(tree, interp, tree.gen_dir)
    out = tree.gen_dir / name
    if out.exists():
        out.unlink()
    run_grit(tree, tree.gen_dir, args)
    if not out.exists():
        raise RuntimeError("grit ran but did not write it")
    stamps = _load_stamps(tree)
    stamps[name] = {"args": args, "key": _grit_key(tree, tree.gen_dir,
                                                   args)}
    _save_stamps(tree, stamps)


def refresh_grit(tree, interp):
    """Remakes every grit header whose .grd (or a <part>, or the first-ids
    file, or its args) changed since it was written, and replaces the
    stand-ins of earlier runs where grit can make the header now. A header
    that can't be remade is removed (a check then reports it missing rather
    than compiling against an old one). Returns (remade, removed)."""
    gen_dir = tree.gen_dir
    stamps = _load_stamps(tree)
    remade, removed = [], []
    headers = [n for n in stamps if n != RESOURCE_IDS and
               not n.startswith("_")]
    if headers:
        try:
            ensure_resource_ids(tree, interp, gen_dir)
        except RuntimeError as e:
            log("grit: %s" % e)
    for n in sorted(headers):
        args = stamps[n]["args"]
        out = gen_dir / n
        if out.exists() and stamps[n].get("key") == _grit_key(
                tree, gen_dir, args):
            continue
        try:
            if out.exists():
                out.unlink()
            run_grit(tree, gen_dir, args)
            stamps = _load_stamps(tree)
            stamps[n]["key"] = _grit_key(tree, gen_dir, args)
            _save_stamps(tree, stamps)
            remade.append(n)
        except RuntimeError as e:
            log("grit: %s: %s; removed" % (n, e))
            if out.exists():
                out.unlink()
            stamps = _load_stamps(tree)
            stamps.pop(n, None)
            _save_stamps(tree, stamps)
            removed.append(n)
    # Stand-ins from runs before grit ran here, or from a failure since.
    for f in sorted(gen_dir.glob("**/grit/*.h")):
        rel = str(f.relative_to(gen_dir))
        try:
            text = f.read_text()
        except OSError:
            continue
        if "STAND-IN" not in text[:200] or "WebUI build" in text[:200]:
            continue
        t = _find_grit_target(tree, interp, rel)
        if t is None:
            continue
        # Not again while its .grd is unchanged since grit last failed.
        src = t.get("source")
        fkey = _grit_key(tree, gen_dir, [
            "-i", "../../" + interp.abspath(src, {"dir": t.path})[2:]]) \
            if isinstance(src, str) else "?"
        if _load_stamps(tree).get("_failed", {}).get(rel) == fkey:
            continue
        try:
            real_grit_header(tree, interp, rel, t)
            remade.append(rel)
        except RuntimeError as e:
            log("grit: %s stays a stand-in: %s" % (
                rel, str(e).strip().splitlines()[-1][:200]))
            if not f.exists():
                f.write_text(text)
            stamps = _load_stamps(tree)
            stamps.setdefault("_failed", {})[rel] = fkey
            _save_stamps(tree, stamps)
    return remade, removed


def grit_header(tree, interp, name, includers=None):
    t = _find_grit_target(tree, interp, name)
    if t is None:
        if name.startswith("chrome/grit/") and "_resources" in name:
            return webui_grit_header(tree, name, includers)
        raise RuntimeError("no grit() target writes %s" % name)
    try:
        real_grit_header(tree, interp, name, t)
        return None  # grit wrote it
    except RuntimeError as e:
        log("grit: %s: %s; writing a stand-in" % (
            name, str(e).strip().splitlines()[-1][:200]))
    src = t.get("source")
    if not isinstance(src, str):
        raise RuntimeError("%s: source not computable" % t)
    grd = interp.abspath(src, {"dir": t.path})[2:]
    names = list(dict.fromkeys(_grd_names(tree, interp, grd)))
    base = 10000 + int(hashlib.sha1(name.encode()).hexdigest()[:4], 16) * 4
    lines = ["// compile_check STAND-IN for grit's %s (from %s): resource" %
             (name, grd),
             "// names are real, the numbers are not.",
             "#pragma once", ""]
    for i, n in enumerate(names):
        lines.append("#define %s %d" % (n, base + i))
    return "\n".join(lines) + "\n"


def build_date(tree, interp):
    return ("// compile_check STAND-IN for base/generated_build_date.h\n"
            "#ifndef BASE_GENERATED_BUILD_DATE_H_\n"
            "#define BASE_GENERATED_BUILD_DATE_H_\n"
            "#define BASE_GENERATED_BUILD_DATE_TIMESTAMP 1767225600\n"
            "#endif\n")


WRITERS = {
    "base/generated_build_date.h": build_date,
}


def generate(tree, interp, gen_dir, names, includers=None):
    """includers: {name: [files including it]} (for WebUI ID stand-ins)."""
    written, failed = [], {}
    includers = includers or {}
    for n in names:
        try:
            if GRIT_RE.search(n):
                text = grit_header(tree, interp, n, includers.get(n))
            elif n.endswith(".rs.h"):
                text = cxx_bridge_header(tree, interp, n)
            elif n.endswith("_generated.h"):
                text = flatbuffer_header(tree, interp, n)
            elif n in WRITERS:
                text = WRITERS[n](tree, interp)
            else:
                continue
        except Exception as e:  # noqa: BLE001 - report, keep going
            failed[n] = str(e)
            continue
        out = Path(gen_dir) / n
        if text is not None:
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(text)
        if out.exists():
            written.append(n)
        else:
            failed[n] = "generator ran but did not write it"
    return written, failed
