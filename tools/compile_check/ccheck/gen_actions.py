# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Runs the GN action() that writes a missing generated file.

For a file under gen/, the BUILD.gn files of its directory and the parent
directories are read; a target whose `outputs` lists the file and that has a
`script` and `args` (action, action_foreach with one source,
blink_python_runner, ...) is run the way ninja would: from the build
directory, with its response file written out. Its inputs and the Python
packages generator scripts commonly import are fetched first.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from . import gn
from .sources import log

# Python packages Chromium's generator scripts import.
PY_DEPS = [
    "build/", "third_party/jinja2/", "third_party/markupsafe/",
    "third_party/ply/", "third_party/pyjson5/", "tools/grit/",
    "tools/json_comment_eater/", "tools/json_to_struct/",
    "tools/json_schema_compiler/", "third_party/blink/renderer/build/scripts/",
    "third_party/blink/tools/blinkpy/common/", "tools/metrics/common/",
    "tools/idl_parser/", "components/vector_icons/",
]
_py_deps_done = False


def _ensure_py_deps(tree):
    global _py_deps_done
    if _py_deps_done:
        return
    paths = []
    for p in PY_DEPS:
        paths += [x for x in tree.glob_upstream(p)
                  if x.endswith((".py", ".tmpl", ".jinja", ".json", ".gni",
                                 ".pyl", ".h", ".cc", ".txt", ".json5"))
                  or "/templates/" in x]
    tree.materialize(paths)
    _py_deps_done = True


def find_action(tree, interp, gen_rel):
    """The target writing gen/<gen_rel>, or None."""
    want = gn.OUT + "/gen/" + gen_rel
    parts = gen_rel.split("/")
    for i in range(len(parts) - 1, -1, -1):
        d = "/".join(parts[:i])
        bf = (d + "/BUILD.gn") if d else "BUILD.gn"
        if not tree.exists(bf):
            continue
        targets = interp.run_file("//" + bf, expand=True)
        # Built-in action types first: a template invocation is recorded
        # with its own (unexpanded) parameters before what it expands to.
        targets.sort(key=lambda t: t.func not in ("action", "action_foreach",
                                                  "copy"))
        for t in targets:
            outs = t.get("outputs")
            if not isinstance(outs, list):
                continue
            for o in outs:
                if isinstance(o, str) and \
                        interp.abspath(o, {"dir": t.path}) == want:
                    return t
    return None


def _src_paths(interp, t, key):
    v = t.get(key)
    if not isinstance(v, list):
        return []
    out = []
    for x in v:
        if isinstance(x, str):
            a = interp.abspath(x, {"dir": t.path})
            if not a.startswith(gn.OUT + "/"):
                out.append(a[2:])
    return out


def run_action(tree, interp, t):
    """Returns None on success, else a reason."""
    script = t.get("script")
    args = t.get("args")
    if not isinstance(script, str) or not isinstance(args, list):
        return "%s has no script/args the reader could compute" % t
    if any(not isinstance(a, str) or "<UNKNOWN>" in a for a in args):
        return "%s: args not computable: %r" % (t, [
            a for a in args if not isinstance(a, str) or "<UNKNOWN>" in a][:3])
    _ensure_py_deps(tree)
    script_path = interp.abspath(script, {"dir": t.path})[2:]
    sd = script_path.rsplit("/", 1)[0] + "/"
    need = [script_path] + [p for p in tree.glob_upstream(sd)
                            if p.endswith(".py")]
    need += _src_paths(interp, t, "inputs") + _src_paths(interp, t, "sources")
    tree.materialize(need)
    rsp = None
    rfc = t.get("response_file_contents")
    if isinstance(rfc, list):
        rsp = tempfile.NamedTemporaryFile("w", suffix=".rsp", delete=False)
        rsp.write(" ".join(str(x) for x in rfc))
        rsp.close()
        args = [a.replace("{{response_file_name}}", rsp.name) for a in args]
    if t.func == "action_foreach":
        return "%s: action_foreach is not supported" % t
    tree.out_dir.mkdir(parents=True, exist_ok=True)
    for o in t.get("outputs") or []:
        if isinstance(o, str):
            a = interp.abspath(o, {"dir": t.path})
            (tree.tree_dir / a[2:]).parent.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, "-E", "-s", str(tree.tree_dir / script_path)] + \
        args
    r = subprocess.run(cmd, cwd=str(tree.out_dir), capture_output=True,
                       text=True, env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
    if rsp:
        Path(rsp.name).unlink()
    if r.returncode != 0:
        return "%s failed: %s" % (t, (r.stderr or r.stdout)[-600:])
    return None


def produce(tree, interp, gen_rels):
    """Generates each gen/<rel> it can. Returns (written, {rel: reason})."""
    written, failed = [], {}
    done_targets = set()
    for rel in gen_rels:
        if (tree.gen_dir / rel).exists():
            continue
        t = find_action(tree, interp, rel)
        if t is None:
            failed[rel] = "no action found"
            continue
        key = (t.path, t.name)
        if key not in done_targets:
            done_targets.add(key)
            log("action: running %s:%s for %s" % (t.path, t.name, rel))
            err = run_action(tree, interp, t)
            if err:
                failed[rel] = err
                continue
        if (tree.gen_dir / rel).exists():
            written.append(rel)
        else:
            failed[rel] = "%s ran but did not write it" % (t,)
    return written, failed
