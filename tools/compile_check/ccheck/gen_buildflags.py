# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Buildflag headers: finds the buildflag_header() target that writes each
missing header, evaluates its flags with the GN reader (args from
configs/linux-x64.gn, defaults from the declare_args() they come from) and
runs the real writer script (build/write_buildflag_header.py, or
PartitionAlloc's own)."""
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

from .sources import log

TEMPLATES = ("buildflag_header", "pa_buildflag_header")

# Flags whose GN value the reader cannot compute (exec_script, ...), with the
# value the Linux official build gets.
OVERRIDES = {}


def _header_path(t):
    hdr = t.get("header")
    if not isinstance(hdr, str):
        return None
    hd = t.get("header_dir")
    if isinstance(hd, str):
        return hd.strip("/") + "/" + hdr
    return t.path[2:] + "/" + hdr


def _candidates(name, include_dirs):
    parts = name.split("/")
    dirs = []
    for inc in include_dirs:
        if inc.startswith("gen:"):
            continue
        for i in range(len(parts) - 1, 0, -1):
            d = "/".join(parts[:i])
            dirs.append((inc + "/" + d) if inc else d)
    return list(dict.fromkeys(dirs))


def generate(tree, interp, gen_dir, names, include_dirs):
    """Writes gen_dir/<name> for every name it can; returns (written,
    {name: reason})."""
    gen_dir = Path(gen_dir)
    found = {}
    for name in names:
        for d in _candidates(name, include_dirs):
            if not tree.exists(d + "/BUILD.gn"):
                continue
            for t in interp.run_file("//" + d + "/BUILD.gn"):
                if t.func not in TEMPLATES:
                    continue
                hp = _header_path(t)
                if hp is None:
                    continue
                for inc in include_dirs:
                    if inc.startswith("gen:"):
                        continue
                    pre = (inc + "/") if inc else ""
                    if hp == pre + name or (not inc and hp == name):
                        found[name] = (t, d)
            if name in found:
                break
    written, failed = [], {}
    for name in names:
        if name not in found:
            failed[name] = "no buildflag_header() target found"
            continue
        t, d = found[name]
        flags = t.get("flags") or []
        if not isinstance(flags, list):
            failed[name] = "flags not computable"
            continue
        vals = []
        unknown = []
        for f in flags:
            if not isinstance(f, str) or "=" not in f:
                unknown.append(repr(f))
                continue
            k, v = f.split("=", 1)
            if k in OVERRIDES:
                v = OVERRIDES[k]
            if "<UNKNOWN>" in v:
                unknown.append(k)
                v = "0"
            vals.append("%s=%s" % (k, v))
        if unknown:
            log("buildflags: %s: unknown value(s) for %s (set to 0)" % (
                name, ", ".join(unknown)))
        script = ("base/allocator/partition_allocator/src/partition_alloc/"
                  "write_buildflag_header.py"
                  if t.func == "pa_buildflag_header"
                  else "build/write_buildflag_header.py")
        tree.materialize([script])
        with tempfile.NamedTemporaryFile("w", suffix=".rsp",
                                         delete=False) as rsp:
            rsp.write(" ".join(shlex.quote(x) for x in ["--flags"] + vals))
        out = gen_dir / name
        out.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run(
            [sys.executable, "-I", str(tree.local_path(script)),
             "--output", name, "--rulename", "//%s:%s" % (d, t.name),
             "--gen-dir", str(gen_dir), "--definitions", rsp.name],
            capture_output=True, text=True)
        Path(rsp.name).unlink()
        if r.returncode != 0:
            failed[name] = "writer failed: " + r.stderr[-300:]
            continue
        if unknown:
            with open(out, "a") as f:
                f.write("// compile_check: unknown GN values set to 0: %s\n"
                        % ", ".join(unknown))
        written.append(name)
    return written, failed
