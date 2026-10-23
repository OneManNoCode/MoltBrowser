# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Host tools Chromium builds from source to generate code, built here.

perfetto's protoc plugins (protozero_plugin for .pbzero.h, cppgen_plugin for
.gen.h) are compiled from the pinned perfetto sources against the system's
libprotoc (Ubuntu's libprotoc-dev, unpacked from apt into the work dir if it
is not installed). Their output does not depend on the libprotoc version.
"""
import glob
import re
import os
import shutil
import subprocess
from pathlib import Path

from . import gen_actions
from .sources import log

DEBS = ["libprotoc-dev", "libprotoc32t64", "libprotobuf-dev",
        "libprotobuf32t64"]
PLUGINS = ["protozero_plugin", "cppgen_plugin"]
# perfetto/src/base files the plugins need (string_utils and what it uses).
# Files a pinned perfetto doesn't have are skipped (progress_reporter.cc,
# which logging.cc calls, is new in the perfetto of Chromium 156).
PERFETTO_BASE = ["src/base/string_utils.cc", "src/base/logging.cc",
                 "src/base/utils.cc", "src/base/string_view.cc",
                 "src/base/file_utils.cc", "src/base/pipe.cc",
                 "src/base/status.cc", "src/base/platform.cc",
                 "src/base/progress_reporter.cc"]


def _libprotoc(work):
    """(include dir, lib dir) of a libprotoc to build against."""
    if Path("/usr/include/google/protobuf/compiler/plugin.h").exists() and \
            glob.glob("/usr/lib/x86_64-linux-gnu/libprotoc.so*"):
        return "/usr/include", "/usr/lib/x86_64-linux-gnu"
    root = Path(work) / "toolchain" / "protobuf-sys"
    lib = root / "usr" / "lib" / "x86_64-linux-gnu"
    if not glob.glob(str(lib / "libprotoc.so*")):
        dl = Path(work) / "toolchain" / "debs"
        dl.mkdir(parents=True, exist_ok=True)
        log("downloading %s with apt-get" % ", ".join(DEBS))
        r = subprocess.run(["apt-get", "download"] + DEBS, cwd=str(dl),
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("apt-get download failed: " + r.stderr[-500:])
        for deb in sorted(dl.glob("*.deb")):
            subprocess.run(["dpkg-deb", "-x", str(deb), str(root)],
                           check=True)
    return str(root / "usr" / "include"), str(lib)


def ensure_perfetto_plugins(work, tree, interp):
    """{name: path} of the built plugins."""
    out = Path(work) / "toolchain" / "perfetto_plugins"
    sub = tree.sub("third_party/perfetto")[0]
    stamp = out / "stamp"
    paths = {p: out / p for p in PLUGINS}
    if stamp.exists() and stamp.read_text() == sub.commit and \
            all(p.exists() for p in paths.values()):
        return paths
    flags_rel = "third_party/perfetto/build_config/perfetto_build_flags.h"
    w, failed = gen_actions.produce(tree, interp, [flags_rel])
    if failed:
        raise RuntimeError("perfetto build flags: %s" % failed[flags_rel])
    pf = "third_party/perfetto/"
    srcs = [pf + "src/protozero/protoc_plugin/%s.cc" % p for p in PLUGINS]
    base = [pf + b for b in PERFETTO_BASE]
    tree.materialize(srcs + base + tree.glob_upstream(pf + "include/") +
                     tree.glob_upstream(pf + "src/base/"))
    inc, lib = _libprotoc(work)
    out.mkdir(parents=True, exist_ok=True)
    cxx = shutil.which("clang++") or shutil.which("g++")
    root = tree.tree_dir
    for p in PLUGINS:
        cmd = [cxx, "-std=c++17", "-O1", "-w", "-ffunction-sections",
               "-fdata-sections", "-Wl,--gc-sections",
               "-I" + str(root / pf / "include"),
               "-I" + str(root / pf),
               "-I" + str(tree.gen_dir / "third_party/perfetto/build_config"),
               "-I" + inc, str(root / pf / "src/protozero/protoc_plugin" /
                               (p + ".cc"))]
        cmd += [str(root / b) for b in base if (root / b).exists()]
        cmd += ["-L" + lib, "-Wl,-rpath," + lib, "-lprotoc", "-lprotobuf",
                "-lpthread", "-o", str(paths[p])]
        log("building %s" % p)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("building %s failed:\n%s" % (
                p, r.stderr[-3000:]))
    stamp.write_text(sub.commit)
    return paths


def ensure_cxxbridge(work, tree):
    """cxxbridge from Chromium's vendored cxxbridge-cmd (its dependencies
    come from crates.io), for the .rs.h headers of cxx bindings."""
    vend = "third_party/rust/chromium_crates_io/vendor/cxxbridge-cmd-v1/"
    tree.materialize(tree.glob_upstream(vend))
    toml = (tree.tree_dir / vend / "Cargo.toml").read_text()
    ver = re.search(r'^version = "([^"]+)"', toml, re.M).group(1)
    root = Path(work) / "toolchain" / "cxxbridge"
    exe = root / "bin" / "cxxbridge"
    stamp = root / "version"
    if exe.exists() and stamp.exists() and stamp.read_text() == ver:
        return exe
    cargo = shutil.which("cargo") or os.path.expanduser("~/.cargo/bin/cargo")
    log("building cxxbridge %s with cargo" % ver)
    # Outside chromium_crates_io's Cargo workspace.
    crate = root / "src"
    shutil.rmtree(crate, ignore_errors=True)
    shutil.copytree(tree.tree_dir / vend, crate)
    r = subprocess.run([cargo, "install", "--path", str(crate),
                        "--root", str(root), "-j", "2", "--force"],
                       capture_output=True, text=True,
                       env=dict(os.environ,
                                CARGO_TARGET_DIR=str(root / "target")))
    if r.returncode != 0:
        raise RuntimeError("cargo install cxxbridge failed: " +
                           r.stderr[-2000:])
    shutil.rmtree(root / "target", ignore_errors=True)
    shutil.rmtree(crate, ignore_errors=True)
    stamp.write_text(ver)
    return exe


def ensure_flatc(work, tree, interp):
    """flatc from the pinned flatbuffers sources (Chromium's own
    //third_party/flatbuffers:flatc target), for *_generated.h headers."""
    fb = "third_party/flatbuffers/"
    sub = tree.sub(fb + "src")[0]
    out = Path(work) / "toolchain" / "flatc"
    exe = out / "flatc"
    stamp = out / "stamp"
    if exe.exists() and stamp.exists() and stamp.read_text() == sub.commit:
        return exe
    srcs = []
    for t in interp.run_file("//" + fb + "BUILD.gn"):
        if t.name in ("flatc", "compiler_files", "flatbuffers"):
            for s in t.get("sources") or []:
                if isinstance(s, str) and s.endswith((".cpp", ".cc")):
                    srcs.append(fb + s)
    tree.materialize(tree.glob_upstream(fb + "src/include/") +
                     tree.glob_upstream(fb + "src/src/") +
                     tree.glob_upstream(fb + "src/grpc/") + srcs)
    out.mkdir(parents=True, exist_ok=True)
    obj = out / "obj"
    obj.mkdir(exist_ok=True)
    cxx = shutil.which("clang++") or shutil.which("g++")
    root = tree.tree_dir
    log("building flatc (%d files)" % len(srcs))
    objs = []
    procs = []
    for s in sorted(set(srcs)):
        o = obj / (s.replace("/", "_") + ".o")
        objs.append(o)
        if o.exists():
            continue
        cmd = [cxx, "-std=c++17", "-O1", "-w", "-c",
               "-I" + str(root / fb / "src/include"),
               "-I" + str(root / fb / "src/grpc"),
               "-I" + str(root / fb / "src"), str(root / s), "-o", str(o)]
        procs.append(subprocess.Popen(cmd, stderr=subprocess.PIPE))
        if len(procs) >= 2:
            p = procs.pop(0)
            if p.wait() != 0:
                raise RuntimeError("flatc: " + p.stderr.read().decode()[-2000:])
    for p in procs:
        if p.wait() != 0:
            raise RuntimeError("flatc: " + p.stderr.read().decode()[-2000:])
    r = subprocess.run([cxx] + [str(o) for o in objs] + ["-o", str(exe)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("linking flatc: " + r.stderr[-2000:])
    shutil.rmtree(obj, ignore_errors=True)
    stamp.write_text(sub.commit)
    return exe
