# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Reuses a file's last check result when nothing that decides it changed.

A check result (its diagnostics) is a function of the compile command, the
compiler and the source the preprocessor produces. So before compiling,
the same command is run as `-E -dD` (preprocess only, keeping the macro
definitions, without the plugins), and the key is the hash of:

  * KEY_VERSION and the compiler's build (toolchain/clang/cr_build_revision:
    clang and its built-in plugins);
  * the exact compile command;
  * the preprocessed output and what preprocessing printed (its
    diagnostics, its exit status).

The preprocessed output carries the content of every header the file
includes (with its path and line markers), every macro definition, and the
outcome of every `__has_include` and every header search, so a header
edited, added where it shadows another, or regenerated with different
content changes the key, as does a different command (flags, include dirs,
plugins). This is ccache's "preprocessor mode"; ccache's faster "direct
mode" (hashing the dependency list) is not used because it misses a new
header that shadows an old one and a `__has_include` that turns true.

Preprocessing a Chromium file takes about a tenth of its check (one to two
seconds for the largest), which is all a re-run of an unchanged file costs.
A result is stored per file and platform (check/cache/<name>.json, one
entry each, replaced on every compile), and only when the compile ran to
completion (exit status 0 or 1, no crash): a compile the memory limit or a
signal stopped is never reused. `--no-cache` compiles everything again.
"""
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

from . import toolchain

# Bump when what a stored result holds, or how the key is made, changes.
KEY_VERSION = "1"

CACHE_DIR = "cache"

# A compile that ran to completion: errors (1) or none (0).
_COMPLETE = (0, 1)
_CRASH_MARKS = ("PLEASE submit a bug report", "clang: error: unable to "
                "execute command", "clang-cl: error: unable to execute "
                "command", "Stack dump:")


def preprocess_command(cmd):
    """The compile command made to preprocess to stdout, keeping the macro
    definitions (-dD): -fsyntax-only (/Zs for clang-cl) becomes -E (/E), and
    Chromium's plugins (-Xclang -add-plugin -Xclang X and their
    -plugin-arg-* pairs), which only see the AST, are dropped; they are in
    the key through the compile command itself."""
    out = []
    i = 0
    while i < len(cmd):
        a = cmd[i]
        if a == "-Xclang" and i + 3 < len(cmd) and \
                cmd[i + 2] == "-Xclang" and (
                    cmd[i + 1] == "-add-plugin" or
                    cmd[i + 1].startswith("-plugin-arg-")):
            i += 4
            continue
        out.append(a)
        i += 1
    if "/Zs" in out:
        out = ["/E" if a == "/Zs" else a for a in out]
        out.insert(len(out) - 1, "/clang:-dD")
    else:
        out = ["-E" if a == "-fsyntax-only" else a for a in out]
        out.insert(len(out) - 1, "-dD")
    return out


def compiler_id(work):
    """The compiler's build (clang and the plugins built into it)."""
    f = toolchain.clang_dir(work) / "cr_build_revision"
    try:
        return f.read_text().strip()
    except OSError:
        return "?"


def key(work, cmd, cwd, preexec_fn=None):
    """The cache key of compiling `cmd` in `cwd`, or None if it can't be
    made (preprocessing could not run)."""
    h = hashlib.sha256()
    h.update(("compile_check result %s\0%s\0" % (
        KEY_VERSION, compiler_id(work))).encode())
    h.update(json.dumps(cmd).encode() + b"\0")
    # What preprocessing prints goes to a file (a pipe read after stdout
    # could fill up and stall it).
    with tempfile.TemporaryFile() as errf:
        try:
            with subprocess.Popen(preprocess_command(cmd), cwd=str(cwd),
                                  stdout=subprocess.PIPE, stderr=errf,
                                  preexec_fn=preexec_fn) as p:
                for chunk in iter(lambda: p.stdout.read(1 << 20), b""):
                    h.update(chunk)
                rc = p.wait()
        except OSError:
            return None
        errf.seek(0)
        err = errf.read()
    if rc not in _COMPLETE:
        return None
    h.update(b"\0stderr\0" + err + (b"\0rc %d" % rc))
    return h.hexdigest()


def entry_path(work, name):
    return Path(work) / "check" / CACHE_DIR / (name + ".json")


def lookup(work, name, k):
    """(returncode, log text, peak MB or None) stored for `name` under key
    `k`, or None."""
    if k is None:
        return None
    try:
        e = json.loads(entry_path(work, name).read_text())
    except (OSError, ValueError):
        return None
    if e.get("key") != k or not isinstance(e.get("log"), str) or \
            e.get("rc") not in _COMPLETE:
        return None
    peak = e.get("max_rss_mb")
    return e["rc"], e["log"], peak if isinstance(peak, int) else None


def reusable(rc, text):
    """Whether a compile's outcome may be stored: it ran to completion."""
    return rc in _COMPLETE and not any(m in text for m in _CRASH_MARKS)


def store(work, name, k, rc, text, peak=None):
    """Stores a result as soon as its compile ends, so a run that is
    interrupted (a restart, Ctrl-C) resumes where it stopped."""
    if k is None or not reusable(rc, text):
        drop(work, name)
        return
    p = entry_path(work, name)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(p.name + ".tmp")
    tmp.write_text(json.dumps({"key": k, "rc": rc, "log": text,
                               "max_rss_mb": peak}))
    tmp.replace(p)


def drop(work, name):
    try:
        entry_path(work, name).unlink()
    except OSError:
        pass
