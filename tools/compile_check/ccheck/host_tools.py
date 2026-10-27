# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Host tools Chromium builds from source to generate code, built here.

perfetto's protoc plugins (protozero_plugin for .pbzero.h, cppgen_plugin for
.gen.h) are compiled from the pinned perfetto sources against the system's
libprotoc (Ubuntu's libprotoc-dev, unpacked from apt into the work dir if it
is not installed). Their output does not depend on the libprotoc version.

The proto_extras plugin (components/proto_extras: the .equal.h, .to_value.h
and .ostream.h headers of proto_extras() targets) uses libprotoc's C++
generator internals, so it is built against the pinned protobuf itself:
third_party/protobuf's libprotobuf and libprotoc sources (its GN lists,
proto_sources.gni) and third_party/abseil-cpp, compiled here with the
host's clang++ once (ensure_protobuf_host). Its //base calls come from
host_shims/proto_extras/ (a stand-in for //base, in a host tool only).

V8's generators (torque, for torque-generated/; the bytecode builtins list
generator, for builtins-generated/) are built from the pinned V8 with the
sources, defines and include dirs of their GN targets and of the V8
targets they depend on (v8/BUILD.gn, read with the GN reader), with the
host's clang++ (ensure_v8_host_tool).
"""
import glob
import hashlib
import json
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


HERE = Path(__file__).resolve().parent
SHIMS = HERE.parent / "host_shims"

# Abseil files that are not library code (tests, benchmarks, tools with a
# main(), mocks), or need flags of their own the plugin doesn't.
_ABSL_SKIP = ("_test.cc", "_benchmark.cc", "_testing.cc", "test_util.cc",
              "_test_common.cc", "_test_helpers.cc", "mock", "/testing/",
              "gentables.cc", "print_hash_of.cc", "randen_hwaes.cc",
              "/time/internal/cctz/src/cctz_benchmark.cc",
              "absl/log/internal/test_", "absl/log/scoped_mock_log.cc",
              "absl/flags/", "absl/random/")


def _compile_all(cxx, srcs, objdir, flags, jobs=2, c_flags=None):
    """Compiles each source to objdir (skipping up-to-date objects).
    Returns (objects, {source: error})."""
    objdir.mkdir(parents=True, exist_ok=True)
    objs, failed, procs = [], {}, []

    def reap(p):
        src, o, proc = p
        err = proc.stderr.read().decode(errors="replace")
        if proc.wait() != 0:
            failed[src] = err[-1500:]
            if o.exists():
                o.unlink()
        else:
            objs.append(o)

    for src in srcs:
        o = objdir / (src.replace("/", "_") + ".o")
        if o.exists():
            objs.append(o)
            continue
        f = (c_flags or flags) if src.endswith(".c") else flags
        cmd = [cxx if not src.endswith(".c") else
               (shutil.which("clang") or shutil.which("gcc"))] + f + \
            ["-c", src, "-o", str(o)]
        procs.append((src, o, subprocess.Popen(cmd,
                                               stderr=subprocess.PIPE)))
        if len(procs) >= jobs:
            reap(procs.pop(0))
    for p in procs:
        reap(p)
    return objs, failed


def _gni_list(interp, path, names):
    """The lists `names` of a .gni (evaluated by the GN reader)."""
    from . import gn
    s = gn.Scope(interp.globals)
    interp.import_file(path, s, {"dir": path.rsplit("/", 1)[0],
                                 "targets": None})
    out = []
    for n in names:
        v = s.vars.get(n)
        if isinstance(v, list):
            out += [x for x in v if isinstance(x, str)]
    return out


def ensure_protobuf_host(work, tree, interp):
    """A static library of the pinned protobuf (libprotobuf + libprotoc +
    its C++ generator) and abseil, built for the host. Returns (library,
    [include dirs])."""
    out = Path(work) / "toolchain" / "protobuf_host"
    lib = out / "libprotobuf_host.a"
    pb = "third_party/protobuf/"
    key = "%s %s" % (tree.chromium.rev, "1")
    stamp = out / "stamp"
    incs = [tree.tree_dir / pb / "src", tree.tree_dir / "third_party" /
            "abseil-cpp", tree.tree_dir / pb / "third_party" / "utf8_range"]
    if lib.exists() and stamp.exists() and stamp.read_text() == key:
        return lib, incs
    srcs = _gni_list(interp, "//" + pb + "proto_sources.gni",
                     ["protobuf_lite_sources", "protobuf_sources",
                      "protoc_sources", "protoc_cpp_sources"])
    srcs = [interp.abspath(x, {"dir": "//" + pb.rstrip("/")})[2:]
            for x in srcs if x.endswith(".cc")]
    utf8 = []
    for t in interp.run_file("//" + pb + "BUILD.gn"):
        if t.name == "utf8_range":
            utf8 = [interp.abspath(x, {"dir": "//" + pb.rstrip("/")})[2:]
                    for x in t.get("sources") or []
                    if isinstance(x, str) and x.endswith((".c", ".cc"))]
    absl = [p for p in tree.glob_upstream("third_party/abseil-cpp/absl/")
            if p.endswith(".cc") and not any(x in p for x in _ABSL_SKIP)]
    if not srcs:
        raise RuntimeError("no protobuf sources in proto_sources.gni")
    tree.materialize(srcs + utf8 + absl +
                     tree.glob_upstream(pb + "src/") +
                     tree.glob_upstream(pb + "third_party/utf8_range/") +
                     tree.glob_upstream("third_party/abseil-cpp/absl/"))
    cxx = shutil.which("clang++") or shutil.which("g++")
    flags = ["-std=c++20", "-O1", "-w", "-fPIC", "-DHAVE_PTHREAD",
             "-DHAVE_ZLIB=0"] + ["-I" + str(i) for i in incs]
    c_flags = ["-std=c11", "-O1", "-w", "-fPIC"] + \
        ["-I" + str(i) for i in incs]
    root = tree.tree_dir
    log("building the host protobuf (%d protobuf, %d abseil files; once)" %
        (len(srcs) + len(utf8), len(absl)))
    objs, failed = _compile_all(
        cxx, [str(root / x) for x in srcs + utf8 + absl], out / "obj",
        flags, c_flags=c_flags)
    bad = [k for k in failed if "/abseil-cpp/" not in k]
    if bad:
        raise RuntimeError("host protobuf: %s: %s" % (
            bad[0], failed[bad[0]][-800:]))
    for k in failed:
        log("host protobuf: skipped %s (%s)" % (
            k, failed[k].strip().splitlines()[-1][:120]
            if failed[k].strip() else "?"))
    if lib.exists():
        lib.unlink()
    ar = shutil.which("ar") or shutil.which("llvm-ar")
    r = subprocess.run([ar, "rcs", str(lib)] + [str(o) for o in objs],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("ar: " + r.stderr[-500:])
    stamp.write_text(key)
    return lib, incs


def ensure_proto_extras_plugin(work, tree, interp):
    """components/proto_extras:proto_extras_plugin, built for the host
    against the pinned protobuf (ensure_protobuf_host) and the //base
    stand-in of host_shims/proto_extras."""
    from . import toolchain
    out = Path(work) / "toolchain" / "proto_extras"
    exe = out / "proto_extras_plugin"
    pe = "components/proto_extras/"
    srcs = [pe + "proto_extras_plugin.cc", pe + "proto_extras_options.proto"]
    tree.materialize(srcs)
    h = hashlib.sha1(b"1")
    for x in srcs:
        h.update(tree.read(x) or b"-")
    for f in sorted((SHIMS / "proto_extras").rglob("*.h")):
        h.update(f.read_bytes())
    key = "%s %s" % (tree.chromium.rev, h.hexdigest())
    stamp = out / "stamp"
    if exe.exists() and stamp.exists() and stamp.read_text() == key:
        return exe
    lib, incs = ensure_protobuf_host(work, tree, interp)
    gen = out / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    protoc = toolchain.ensure_protoc(work, tree)
    r = subprocess.run([str(protoc), "--cpp_out=" + str(gen),
                        "-I" + str(tree.tree_dir),
                        str(tree.tree_dir / pe / "proto_extras_options.proto")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("protoc proto_extras_options: " + r.stderr[-800:])
    # The plugin includes "third_party/protobuf/src/google/protobuf/...".
    cxx = shutil.which("clang++") or shutil.which("g++")
    # Chromium builds it as C++23 (std::string::contains).
    cmd = [cxx, "-std=c++23", "-O1", "-w", "-DHAVE_PTHREAD",
           "-I" + str(SHIMS / "proto_extras"), "-I" + str(gen),
           "-I" + str(tree.tree_dir)] + ["-I" + str(i) for i in incs] + [
        str(tree.tree_dir / pe / "proto_extras_plugin.cc"),
        str(gen / pe / "proto_extras_options.pb.cc"),
        str(lib), "-lpthread", "-o", str(exe)]
    log("building proto_extras_plugin")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("proto_extras_plugin: " + r.stderr[-2500:])
    stamp.write_text(key)
    return exe


# V8's host executables the build runs through tools/run.py.
V8_HOST_TOOLS = ("torque", "bytecode_builtins_list_generator")
_BUILTIN = ("source_set", "static_library", "component", "executable",
            "shared_library")


def _v8_targets(interp):
    ts = {}
    for t in interp.run_file("//v8/BUILD.gn", expand=True):
        if t.func in _BUILTIN and t.path == "//v8":
            # The expanded target (configs, sources) wins over the
            # template invocation of the same name.
            ts[t.name] = t
    return ts


def _v8_closure(ts, name):
    """The V8 targets `name` links: itself and its deps within
    v8/BUILD.gn, transitively."""
    out, todo = [], [name]
    while todo:
        n = todo.pop()
        if n in out or n not in ts:
            continue
        out.append(n)
        for key in ("deps", "public_deps"):
            for d in ts[n].get(key) or []:
                if isinstance(d, str) and d.startswith(":"):
                    todo.append(d[1:].split("(")[0])
                elif isinstance(d, str) and d.startswith("//v8:"):
                    todo.append(d[5:].split("(")[0])
    return out


def ensure_vulkan_shaders_gen(work, tree, interp):
    """llama.cpp's shader generator (//third_party/llama_cpp:
    vulkan_shaders_gen, built with molt_llama_vulkan): its
    ggml-vulkan-shaders.hpp step, the one the check needs, compiles no
    shader. Returns its path."""
    from . import gnconfigs
    bf = "//third_party/llama_cpp/BUILD.gn"
    t = next((x for x in interp.run_file(bf)
              if x.name == "vulkan_shaders_gen"), None)
    if t is None:
        raise RuntimeError("no vulkan_shaders_gen in %s (molt_llama_vulkan "
                           "off?)" % bf)
    srcs = [interp.abspath(x, {"dir": t.path})[2:]
            for x in t.get("sources") or [] if isinstance(x, str)]
    tree.materialize(srcs)
    flags = ["-std=c++17", "-O1", "-w", "-pthread", "-fexceptions"] + [
        f for f in gnconfigs.own_flags(t, interp, tree, {}, "c++")
        if f.startswith("-D")]
    out = Path(work) / "toolchain" / "llama_tools"
    exe = out / "vulkan_shaders_gen"
    h = hashlib.sha256(json.dumps(flags).encode())
    for s in srcs:
        h.update((tree.tree_dir / s).read_bytes())
    stamp = out / "vulkan_shaders_gen.stamp"
    if exe.exists() and stamp.exists() and stamp.read_text() == \
            h.hexdigest():
        return exe
    out.mkdir(parents=True, exist_ok=True)
    cxx = shutil.which("clang++") or shutil.which("g++")
    log("building llama.cpp's vulkan-shaders-gen")
    r = subprocess.run([cxx] + flags + [str(tree.tree_dir / s)
                                        for s in srcs] + ["-o", str(exe)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("vulkan_shaders_gen: " + r.stderr[-1500:])
    stamp.write_text(h.hexdigest())
    return exe


def ensure_v8_host_tool(work, tree, interp, name):
    """Builds V8's host tool `name` (V8_HOST_TOOLS); returns its path."""
    from . import gnconfigs
    out = Path(work) / "toolchain" / "v8_tools"
    exe = out / name
    sub = tree.sub("v8")[0]
    key = "%s %s 1" % (sub.commit if sub else "?", name)
    stamp = out / (name + ".stamp")
    if exe.exists() and stamp.exists() and stamp.read_text() == key:
        return exe
    ts = _v8_targets(interp)
    if name not in ts:
        raise RuntimeError("no target %s in v8/BUILD.gn" % name)
    # Abseil comes from the host protobuf's library (the same pinned
    # sources).
    absl_lib, _ = ensure_protobuf_host(work, tree, interp)
    cache = {}
    cxx = shutil.which("clang++") or shutil.which("g++")
    objs = []
    units = []
    # V8's math (ieee754.cc) calls MoltBrowser's portable llvm-libc build
    # (//third_party/llvm-libc:molt_portable_math, through :headers).
    ll = "third_party/llvm-libc/"
    units.append(("molt_portable_math", [ll + "molt_portable_math.cc"],
                  ["-std=c++20", "-O1", "-w", "-fPIC", "-fno-exceptions",
                   "-fno-rtti", "-I" + str(tree.tree_dir / ll / "src"),
                   "-I" + str(tree.tree_dir)]))
    llvm_libc_inc = "-I" + str(tree.tree_dir / ll / "src")
    for tn in _v8_closure(ts, name):
        t = ts[tn]
        srcs = [interp.abspath(x, {"dir": "//v8"})[2:]
                for x in (t.get("sources") or [])
                if isinstance(x, str) and x.endswith(".cc")]
        if not srcs:
            continue
        lang = gnconfigs.language_flags(t, interp, tree, cache)
        flags = ["-std=c++20", "-O1", "-w", "-fPIC", "-pthread"]
        flags += ["-fexceptions" if lang["exceptions"] else
                  "-fno-exceptions",
                  "-frtti" if lang["rtti"] else "-fno-rtti"]
        flags += [f for f in gnconfigs.own_flags(t, interp, tree, cache,
                                                  "c++")
                  if f.startswith(("-D", "-I"))]
        flags += ["-I" + str(tree.tree_dir / "v8"),
                  "-I" + str(tree.tree_dir / "v8" / "include"),
                  "-I" + str(tree.gen_dir / "v8"),
                  "-I" + str(tree.gen_dir / "v8" / "include"),
                  "-I" + str(tree.tree_dir),
                  "-I" + str(tree.tree_dir / "third_party" / "abseil-cpp"),
                  "-I" + str(tree.gen_dir), llvm_libc_inc,
                  "-DLIBC_NAMESPACE=__llvm_libc_cr"]
        units.append((tn, srcs, flags))
    tree.materialize(tree.glob_upstream("v8/src/") +
                     tree.glob_upstream("v8/include/") +
                     tree.glob_upstream("v8/third_party/") +
                     tree.glob_upstream(ll) +
                     tree.glob_upstream(ll + "src/") +
                     tree.glob_upstream("third_party/abseil-cpp/absl/"))
    log("building V8's %s (%d files; once)" % (
        name, sum(len(u[1]) for u in units)))
    for tn, srcs, flags in units:
        o, failed = _compile_all(cxx, [str(tree.tree_dir / x) for x in srcs],
                                 out / "obj" / tn, flags)
        if failed:
            k = sorted(failed)[0]
            raise RuntimeError("%s: %s: %s" % (name, k, failed[k][-1500:]))
        objs += o
    r = subprocess.run([cxx] + [str(o) for o in objs] +
                       [str(absl_lib), "-pthread", "-ldl", "-o", str(exe)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("linking %s: %s" % (name, r.stderr[-2500:]))
    stamp.write_text(key)
    return exe
