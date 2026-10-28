# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Runs clang -fsyntax-only on each target and summarizes the diagnostics.

Each target is compiled for its tree's platform (platforms.py) in its
language: C with clang and -std=c11, C++ with clang++, Objective-C++ with
ARC, Windows files with clang-cl against the Windows SDK."""
import concurrent.futures
import json
import os
import re
import resource
import subprocess
import tempfile
import time
from pathlib import Path

from . import config, platforms, result_cache, sdks, toolchain

DIAG_RE = re.compile(r"^(?P<file>[^:\n(]+?)(?::(?P<line>\d+):(?P<col>\d+)|"
                     r"\((?P<wline>\d+),(?P<wcol>\d+)\)): "
                     r"(?P<kind>error|warning|fatal error): (?P<msg>.*)$",
                     re.M)

# Per-file extra flags (the GN target's own defines/include dirs).
PER_FILE = {}

CXX_LANGS = ("c++", "objective-c++")


def language_cflags(lang=None, flags=None, driver="clang"):
    """The platform's flags (config.CFLAGS by default) with the target's own
    exception and RTTI setting (gnconfigs.target_flags: Chromium's default
    is off for both)."""
    out = list(config.CFLAGS if flags is None else flags)
    lang = lang or {}
    if driver == "clang-cl":
        if lang.get("exceptions"):
            out.append("/EHsc")
        if lang.get("rtti"):
            out = ["/GR" if f == "/GR-" else f for f in out]
        return out
    for key, off, on in (("exceptions", "-fno-exceptions", "-fexceptions"),
                         ("rtti", "-fno-rtti", "-frtti")):
        if lang.get(key):
            out = [on if f == off else f for f in out]
    return out


def _dir(tree, inc):
    if inc.startswith("gen:"):
        return tree.gen_dir / inc[4:] if inc[4:] else tree.gen_dir
    return tree.tree_dir / inc if inc else tree.tree_dir


def compile_command(tree, work, target, plugins=True, dev=False,
                    gn_flags=None, lang=None):
    """gn_flags: (global flags, {dir prefix: flags}) from gnconfigs.
    lang: the target's gnconfigs.target_flags entry (exceptions, rtti, its
    own target's flags, the scoped include dirs its closure reaches)."""
    root = tree.tree_dir
    plat = getattr(tree, "platform", None) or platforms.get(
        platforms.DEFAULT)
    language = platforms.language(target)
    driver, flags, defines = config.platform_cflags(plat.name, language)
    bindir = toolchain.clang_dir(work) / "bin"
    if driver == "clang-cl":
        exe = bindir / "clang-cl"
    else:
        exe = bindir / ("clang++" if language in CXX_LANGS else "clang")
    cmd = [str(exe)] + language_cflags(lang, flags, driver)
    if plugins and language in CXX_LANGS:
        cmd += config.plugin_flags(plat.name, root, dev)
    for d in defines:
        cmd.append("-D" + d)
    for prefix, extra in PER_FILE.items():
        if target.startswith(prefix):
            cmd += extra
    for prefix, (extra, _, _) in config.STAND_IN_FLAGS.items():
        if target.startswith(prefix):
            cmd += extra
    for inc in config.include_dirs(plat.name):
        cmd.append("-I" + str(_dir(tree, inc)))
    extra = []
    if gn_flags:
        glob, per_dir = gn_flags
        extra = list(glob)
        for prefix, flags in per_dir.items():
            if target.startswith(prefix):
                extra += flags
    extra += list((lang or {}).get("flags") or [])
    for inc in (lang or {}).get("scoped") or []:
        extra.append("-I" + str(_dir(tree, inc)))
    if not plugins:
        extra = _strip_plugins(extra)
    if language not in CXX_LANGS:
        extra = _strip_plugins(extra)
    cmd += extra
    if driver == "clang-cl":
        # libc++ on Windows comes as an -I in cflags_cc (no libc++abi:
        # the MSVC ABI's runtime is vcruntime's).
        if language in CXX_LANGS:
            cmd.append("-I" + str(root / config.SYSTEM_DIRS[0]))
        for d in getattr(tree, "system_dirs", []):
            cmd.append("/imsvc" + str(d))
        # Found case-insensitively, as on Windows (sdks.py).
        cmd += ["-Xclang", "-ivfsoverlay", "-Xclang",
                str(sdks.windows_overlay(work))]
    else:
        if language in CXX_LANGS:
            for inc in config.SYSTEM_DIRS:
                cmd.append("-isystem" + str(root / inc))
        if plat.name == "mac":
            sdk = sdks.mac_sdk_dir(work)
            cmd += ["-isysroot", str(sdk)]
        else:
            cpu = "arm64" if plat.gn_cpu == "arm64" else "x64"
            sysroot = toolchain.sysroot_dir(work, cpu)
            if (sysroot / ".stamp").exists():
                cmd.append("--sysroot=" + str(sysroot))
                for inc in config.SYSROOT_DIRS:
                    d = inc.replace("x86_64-linux-gnu", "aarch64-linux-gnu") \
                        if cpu == "arm64" else inc
                    cmd.append("-isystem" + str(sysroot / d))
    if target.endswith(".h"):
        # A changed header no source includes, compiled on its own.
        cmd += ["/TP"] if driver == "clang-cl" else [
            "-x", language + "-header"]
    cmd.append(str(root / target))
    return cmd


def _strip_plugins(flags):
    out = []
    i = 0
    while i < len(flags):
        if flags[i] == "-Xclang" and i + 1 < len(flags) and (
                "plugin" in flags[i + 1] or i + 3 < len(flags) and
                flags[i + 2] == "-Xclang" and "plugin" in flags[i + 1]):
            i += 2
            continue
        out.append(flags[i])
        i += 1
    return out


def _limit():
    # Keep one compile from taking the machine down (no swap here).
    resource.setrlimit(resource.RLIMIT_AS, (6 << 30, 6 << 30))


def _vm_hwm_kb(pid="self"):
    """A process's peak resident memory so far (VmHWM), in KB; 0 if
    unknown."""
    try:
        with open("/proc/%s/status" % pid) as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except (OSError, ValueError, IndexError):
        pass
    return 0


def run_measured(cmd, cwd, interval=0.02):
    """(exit status, output, peak resident memory in MB or None) of a
    compile: its stderr and stdout in one stream, and the compiler's own
    peak RSS, which `report` lists to size --jobs.

    wait4's maximum alone is not the compiler's: the child forked from
    this process starts with a copy of its memory (several hundred MB of
    scan state), which Linux counts in the child's maximum. So the
    compiler's VmHWM is sampled while it runs (its own, from exec on), and
    wait4's maximum used where it exceeds anything this process ever held
    (then it can only be the compiler's). A compile that ended before the
    first sample (a few small files) has None: its peak is small, but not
    known."""
    with tempfile.TemporaryFile() as out:
        p = subprocess.Popen(cmd, cwd=cwd, stdout=out,
                             stderr=subprocess.STDOUT, preexec_fn=_limit)
        seen = 0
        while True:
            pid, status, ru = os.wait4(p.pid, os.WNOHANG)
            if pid:
                break
            seen = max(seen, _vm_hwm_kb(p.pid))
            time.sleep(interval)
        p.returncode = os.waitstatus_to_exitcode(status)
        out.seek(0)
        text = out.read().decode("utf-8", "replace")
    own = _vm_hwm_kb()
    kb = ru.ru_maxrss if not own or ru.ru_maxrss > own else seen
    return p.returncode, text, kb // 1024 if kb else None


def parse_diags(text):
    diags = []
    for m in DIAG_RE.finditer(text):
        f = m.group("file")
        if f.startswith("../../"):
            f = f[6:]
        line = m.group("line") or m.group("wline")
        diags.append({"file": f, "line": int(line),
                      "kind": m.group("kind"), "msg": m.group("msg")})
    return diags


def run_one(tree, work, target, out_dir, plugins=True, dev=False,
            gn_flags=None, lang=None, cache=True):
    cmd = compile_command(tree, work, target, plugins, dev, gn_flags, lang)
    plat = getattr(tree, "platform", None)
    name = target.replace("/", "__")
    if plat is not None and plat.name != platforms.of(target):
        name += "@" + plat.name
    # The last result, if nothing that decides it changed (result_cache).
    # The overlay (the working tree) as it was before the key was made.
    stamp = tree.overlay_stamp() if cache else None
    key = result_cache.key(work, cmd, tree.out_dir, _limit) if cache \
        else None
    hit = result_cache.lookup(work, name, key)
    if hit is not None:
        rc, text, peak = hit  # the peak measured when it was compiled
    else:
        rc, text, peak = run_measured(cmd, str(tree.out_dir))
        root = str(tree.tree_dir) + "/"
        text = text.replace(root, "")
        if cache and tree.overlay_stamp() == stamp:
            result_cache.store(work, name, key, rc, text, peak)
        else:
            # --no-cache, or a file edited while this one compiled: what
            # was compiled may not be what the key was made from.
            result_cache.drop(work, name)
    log = out_dir / (name + ".log")
    log.write_text(text)
    (out_dir / "commands").mkdir(parents=True, exist_ok=True)
    (out_dir / "commands" / (name + ".json")).write_text(json.dumps(
        {"directory": str(tree.out_dir), "arguments": cmd}, indent=1))
    res = {"target": target, "rc": rc, "diags": parse_diags(text),
           "log": str(log), "reused": hit is not None,
           "max_rss_mb": peak,
           "platform": plat.name if plat else platforms.DEFAULT,
           "lang": platforms.language(target)}
    if lang:
        res["gn_target"] = lang.get("gn_target")
        res["exceptions"] = bool(lang.get("exceptions"))
        res["rtti"] = bool(lang.get("rtti"))
    return res


def run(tree, work, targets, jobs=2, plugins=True, dev=False, gn_flags=None,
        target_flags=None, cache=True):
    out_dir = Path(work) / "check"
    out_dir.mkdir(parents=True, exist_ok=True)
    tree.out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "commands").mkdir(exist_ok=True)
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(run_one, tree, work, t, out_dir, plugins, dev,
                          gn_flags, (target_flags or {}).get(t), cache): t
                for t in targets}
        for i, f in enumerate(concurrent.futures.as_completed(futs)):
            res = f.result()
            results.append(res)
            n_err = sum(1 for d in res["diags"] if "error" in d["kind"])
            print("[%d/%d] %s (%s): %d error(s)%s" % (
                i + 1, len(targets), res["target"], res["platform"], n_err,
                " (unchanged: last result)" if res.get("reused") else
                ", %d MB" % res["max_rss_mb"] if res.get("max_rss_mb")
                else ""), flush=True)
    n = sum(1 for r in results if r.get("reused"))
    if n:
        print("%s: %d of %d result(s) reused (inputs unchanged)" % (
            getattr(tree, "platform", None) and tree.platform.name or "",
            n, len(results)))
    results.sort(key=lambda r: r["target"])
    return results
