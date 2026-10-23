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
import shlex
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
    "tools/gritsettings/", "tools/metrics/", "third_party/inspector_protocol/",
    "tools/protoc_wrapper/", "tools/flags/", "components/policy/tools/",
    "components/policy/resources/", "third_party/mako/",
    "third_party/blink/public/devtools_protocol/", "chrome/VERSION",
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
                                 ".pyl", ".h", ".cc", ".txt", ".json5",
                                 "VERSION", ".pdl", ".spec", ".yaml",
                                 ".template"))
                  or "/templates/" in x]
    tree.materialize(paths)
    _py_deps_done = True


_build_texts = None


def _build_files_mentioning(tree, word, limit=40):
    """BUILD.gn files whose text contains `word` (for outputs written by a
    target outside the output's own directory)."""
    global _build_texts
    if _build_texts is None:
        files = set(tree.chromium_files(lambda p: p.endswith("BUILD.gn")))
        files |= {p for p in tree.overlay_set() if p.endswith("BUILD.gn")}
        _build_texts = {}
        for p in sorted(files):
            d = tree.read(p)
            if d is not None:
                _build_texts[p] = d
    w = word.encode()
    out = [p for p, d in _build_texts.items() if w in d]
    return out if len(out) <= limit else []


# Generated paths whose target lives in a BUILD.gn the searches below do not
# reach (subproject BUILD.gn files are not indexed).
EXTRA_BUILD_FILES = {
    "third_party/perfetto/build_config/": "third_party/perfetto/gn/BUILD.gn",
    "v8/include/inspector/": "v8/src/inspector/BUILD.gn",
    "third_party/dawn/include/dawn/dawn_version": "third_party/dawn/src/dawn/BUILD.gn",
    "third_party/dawn/include/dawn/": "third_party/dawn/include/dawn/BUILD.gn",
    "third_party/dawn/src/dawn/": "third_party/dawn/src/dawn/BUILD.gn",
}


def find_action(tree, interp, gen_rel, hint=None):
    """The target writing gen/<gen_rel>, or None. hint: a BUILD.gn to look
    in first (that of the action needing it as an input)."""
    want = gn.OUT + "/gen/" + gen_rel
    parts = gen_rel.split("/")
    cands = [hint] if hint else []
    cands += [bf for pre, bf in EXTRA_BUILD_FILES.items()
              if gen_rel.startswith(pre) and bf != hint]
    for i in range(len(parts) - 1, -1, -1):
        d = "/".join(parts[:i])
        bf = (d + "/BUILD.gn") if d else "BUILD.gn"
        if bf not in cands:
            cands.append(bf)
    stem = parts[-1].split(".")[0]
    if len(stem) >= 6:
        cands += [p for p in _build_files_mentioning(tree, stem)
                  if p not in cands]
    for bf in cands:
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
            if t.func == "action_foreach":
                for src in _src_paths(interp, t, "sources"):
                    for o in outs:
                        if isinstance(o, str) and interp.abspath(
                                foreach_expand(o, src, False),
                                {"dir": t.path}) == want:
                            t.foreach_source = src
                            return t
                continue
            for o in outs:
                if isinstance(o, str) and \
                        interp.abspath(o, {"dir": t.path}) == want:
                    return t
    return None


def foreach_expand(text, src, for_args):
    """GN's {{source...}} placeholders for one source (src-relative path).
    Outputs use source-absolute paths, args paths relative to the build
    directory."""
    d, f = (src.rsplit("/", 1) if "/" in src else ("", src))
    name = f.rsplit(".", 1)[0] if "." in f else f
    out = gn.OUT[2:]
    up = "/".join(".." for _ in out.split("/"))
    if for_args:
        vals = {"source": up + "/" + src, "source_dir": up + "/" + d,
                "source_gen_dir": "gen/" + d, "source_out_dir": "obj/" + d}
    else:
        vals = {"source": "//" + src, "source_dir": "//" + d,
                "source_gen_dir": gn.OUT + "/gen/" + d,
                "source_out_dir": gn.OUT + "/obj/" + d}
    vals.update({"source_file_part": f, "source_name_part": name,
                 "source_root_relative_dir": d,
                 "source_target_relative": f})
    for k, v in vals.items():
        text = text.replace("{{%s}}" % k, v)
    return text


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


# Host tools a generator action runs, by the flag naming them: our
# substitutes (Chromium builds them from source for the host). A value is a
# path, or a function of the target returning one.
HOST_TOOLS = {}


def _gen_inputs(interp, t):
    """gen-relative paths of the target's inputs that a build generates."""
    out = []
    for key in ("inputs", "sources"):
        v = t.get(key)
        if not isinstance(v, list):
            continue
        for x in v:
            if isinstance(x, str):
                a = interp.abspath(x, {"dir": t.path})
                if a.startswith(gn.OUT + "/gen/"):
                    out.append(a[len(gn.OUT) + 5:])
    return out


def _perfetto_plugin_target(t):
    """perfetto_proto_library() actions: zero_gen (protozero) and cpp_gen
    (cppgen)."""
    return t.name.endswith(("zero_gen", "cpp_gen"))


def run_action(tree, interp, t, depth=0):
    """Returns None on success, else a reason."""
    # Files gn's write_file() would have written while generating.
    for path, text in interp.written_files.items():
        if path.startswith(gn.OUT + "/"):
            f = tree.tree_dir / path[2:]
            if not f.exists() or f.read_text() != text:
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_text(text)
    if depth < 4:
        for rel in _gen_inputs(interp, t):
            if not (tree.gen_dir / rel).exists():
                _, failed = produce(tree, interp, [rel], depth + 1,
                                    hint=t.path[2:].rstrip("/") + "/BUILD.gn")
                if failed:
                    return "%s: input %s: %s" % (t, rel, failed[rel][:300])
    script = t.get("script")
    args = t.get("args")
    if not isinstance(script, str) or not isinstance(args, list):
        return "%s has no script/args the reader could compute" % t
    args = list(args)
    if str(script).endswith("protoc_wrapper.py"):
        # Only the C++ output is needed; drop JS/TS generation (their
        # protoc plugins are built from source by Chromium).
        out = []
        skip = 0
        for a in args:
            if skip:
                skip -= 1
                continue
            if a in ("--js-out-dir", "--protoc-gen-js", "--ts-out-dir",
                     "--protoc-gen-ts"):
                skip = 1
                continue
            if a in ("--plugin", "--plugin-out-dir", "--plugin-options") \
                    and not _perfetto_plugin_target(t):
                # A generator plugin Chromium builds (not perfetto's):
                # the C++ protoc output is all a syntax check needs.
                skip = 1
                continue
            out.append(a)
        args = out
    for i, a in enumerate(args[:-1]):
        tool = HOST_TOOLS.get(a)
        if callable(tool):
            tool = tool(t)
        if tool:
            args[i + 1] = str(tool)
    if any(not isinstance(a, str) or "<UNKNOWN>" in a for a in args):
        return "%s: args not computable: %r" % (t, [
            a for a in args if not isinstance(a, str) or "<UNKNOWN>" in a][:3])
    _ensure_py_deps(tree)
    script_path = interp.abspath(script, {"dir": t.path})[2:]
    sd = script_path.rsplit("/", 1)[0] + "/"
    # The script's package, with the templates it renders (jinja2:
    # inspector_protocol's lib/*.template, ...).
    need = [script_path] + [p for p in tree.glob_upstream(sd)
                            if p.endswith((".py", ".template", ".tmpl",
                                           ".jinja", ".jinja2"))]
    need += _src_paths(interp, t, "inputs") + _src_paths(interp, t, "sources")
    if script_path.endswith("protoc_wrapper.py"):
        # protoc reads the imports of the listed .proto files too.
        for i, a in enumerate(args):
            d = None
            if a.startswith("--import-dir="):
                d = a.split("=", 1)[1]
            elif a == "--proto-in-dir" and i + 1 < len(args):
                d = args[i + 1]
            if d:
                rel = os.path.normpath(os.path.join(gn.OUT[2:], d))
                if not rel.startswith(("..", gn.OUT[2:])):
                    need += tree.glob_upstream(rel.rstrip("/") + "/",
                                               ".proto")
    tree.materialize(need)
    rsp = None
    rfc = t.get("response_file_contents")
    if isinstance(rfc, list):
        rsp = tempfile.NamedTemporaryFile("w", suffix=".rsp", delete=False)
        rsp.write(" ".join(shlex.quote(str(x)) for x in rfc))
        rsp.close()
        args = [a.replace("{{response_file_name}}", rsp.name) for a in args]
    if t.func == "action_foreach":
        src = getattr(t, "foreach_source", None)
        if src is None:
            return "%s: action_foreach without a chosen source" % t
        args = [foreach_expand(a, src, True) for a in args]
    tree.out_dir.mkdir(parents=True, exist_ok=True)
    for o in t.get("outputs") or []:
        if isinstance(o, str):
            if t.func == "action_foreach":
                o = foreach_expand(o, t.foreach_source, False)
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


def produce(tree, interp, gen_rels, depth=0, hint=None):
    """Generates each gen/<rel> it can. Returns (written, {rel: reason})."""
    written, failed = [], {}
    done_targets = set()
    for rel in gen_rels:
        if (tree.gen_dir / rel).exists():
            written.append(rel)
            continue
        t = find_action(tree, interp, rel, hint)
        if t is None:
            failed[rel] = "no action found"
            continue
        key = (t.path, t.name, getattr(t, "foreach_source", None))
        if key not in done_targets:
            done_targets.add(key)
            log("action: running %s:%s for %s" % (t.path, t.name, rel))
            err = run_action(tree, interp, t, depth)
            if err:
                failed[rel] = err
                continue
        if (tree.gen_dir / rel).exists():
            written.append(rel)
        else:
            failed[rel] = "%s ran but did not write it" % (t,)
    return written, failed
