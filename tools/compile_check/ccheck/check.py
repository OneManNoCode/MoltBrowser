# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Runs clang -fsyntax-only on each target and summarizes the diagnostics."""
import concurrent.futures
import re
import resource
import subprocess
from pathlib import Path

from . import config, toolchain

DIAG_RE = re.compile(r"^(?P<file>[^:\n]+):(?P<line>\d+):(?P<col>\d+): "
                     r"(?P<kind>error|warning|fatal error): (?P<msg>.*)$",
                     re.M)

# Per-file extra flags (the GN target's own defines/include dirs).
PER_FILE = {}


def compile_command(tree, work, target, plugins=True, dev=False):
    root = tree.tree_dir
    clang = toolchain.clang_dir(work) / "bin" / "clang++"
    cmd = [str(clang)] + config.CFLAGS
    if plugins:
        cmd += config.PLUGIN_FLAGS
        if dev:
            cmd += config.DEV_PLUGIN_FLAGS
    for d in config.DEFINES:
        cmd.append("-D" + d)
    for prefix, extra in PER_FILE.items():
        if target.startswith(prefix):
            cmd += extra
    for inc in config.INCLUDE_DIRS:
        if inc.startswith("gen:"):
            p = tree.gen_dir / inc[4:] if inc[4:] else tree.gen_dir
        else:
            p = root / inc if inc else root
        cmd.append("-I" + str(p))
    for inc in config.SYSTEM_DIRS:
        cmd.append("-isystem" + str(root / inc))
    cmd.append(str(root / target))
    return cmd


def _limit():
    # Keep one compile from taking the machine down (no swap here).
    resource.setrlimit(resource.RLIMIT_AS, (6 << 30, 6 << 30))


def run_one(tree, work, target, out_dir, plugins=True, dev=False):
    cmd = compile_command(tree, work, target, plugins, dev)
    r = subprocess.run(cmd, cwd=str(tree.out_dir), capture_output=True,
                       text=True, preexec_fn=_limit)
    text = r.stderr
    root = str(tree.tree_dir) + "/"
    text = text.replace(root, "")
    log = out_dir / (target.replace("/", "__") + ".log")
    log.write_text(text)
    diags = []
    for m in DIAG_RE.finditer(text):
        f = m.group("file")
        if f.startswith("../../"):
            f = f[6:]
        diags.append({"file": f, "line": int(m.group("line")),
                      "kind": m.group("kind"), "msg": m.group("msg")})
    return {"target": target, "rc": r.returncode, "diags": diags,
            "log": str(log)}


def run(tree, work, targets, jobs=2, plugins=True, dev=False):
    out_dir = Path(work) / "check"
    out_dir.mkdir(parents=True, exist_ok=True)
    tree.out_dir.mkdir(parents=True, exist_ok=True)
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(run_one, tree, work, t, out_dir, plugins, dev): t
                for t in targets}
        for i, f in enumerate(concurrent.futures.as_completed(futs)):
            res = f.result()
            results.append(res)
            n_err = sum(1 for d in res["diags"] if "error" in d["kind"])
            print("[%d/%d] %s: %d error(s)" % (i + 1, len(targets),
                                              res["target"], n_err),
                  flush=True)
    results.sort(key=lambda r: r["target"])
    return results
