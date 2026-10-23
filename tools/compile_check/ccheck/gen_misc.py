# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Generated headers no GN action here can produce, written directly.

STAND-INS. Each writer below approximates what the build generates; checks
that include its output rest partly on a stand-in (the report lists them):

  grit       resource-ID headers (grit/*.h): every <message>, <include> and
             <structure> name in the .grd and its <part> files, numbered from
             a per-header base. The real IDs come from resource_ids.spec; the
             names (all that C++ type-checks against) are the same.
  build date base/generated_build_date.h with a fixed date.

Real generators run here: .rs.h (cxx bridge headers) come from cxxbridge
built from Chromium's vendored cxxbridge-cmd.
"""
import hashlib
import re
import subprocess
from pathlib import Path

from . import gn
from .gen_actions import _build_files_mentioning
from .sources import log

GRIT_RE = re.compile(r"(^|/)grit/[^/]+\.h$")
NAME_RE = re.compile(rb'<(message|include|structure)\b[^>]*?\bname="([A-Za-z0-9_]+)"',
                     re.S)
PART_RE = re.compile(rb'<part\s+file="([^"]+)"')


# Lazily built host tools, set by the driver: name -> function returning a
# path.
TOOLS = {}


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


def grit_header(tree, interp, name):
    t = _find_grit_target(tree, interp, name)
    if t is None:
        raise RuntimeError("no grit() target writes %s" % name)
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


def generate(tree, interp, gen_dir, names):
    written, failed = [], {}
    for n in names:
        try:
            if GRIT_RE.search(n):
                text = grit_header(tree, interp, n)
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
