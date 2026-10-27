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
  build date  base/generated_build_date.h with a fixed date, only if
              its GN actions (base:build_date_internal, from the pinned
              commit's time, and the copy base:build_date_header) fail.
  preloads    net/http/transport_security_state_static*.h: the real
              .template the build's generator fills (every declaration
              transport_security_state.cc compiles against), with
              placeholder data where the generator writes the preload
              list's Huffman tree and trie. The generator is a host tool
              of //base (and //base's Rust JSON reader), which can't be
              built here; the data doesn't change what a check sees.

Real generators run here: .rs.h (cxx bridge headers) come from cxxbridge
built from Chromium's vendored cxxbridge-cmd, run the way
third_party/rust/cxx/chromium_integration/run_cxxbridge.py runs it (with
a --cfg for every line `rustc --print=cfg` gives for the platform's Rust
target, so a bridge's #[cfg(target_family = "unix")] items are decided as
in the build). A bridge header is remade when its .rs, the cfgs or
cxxbridge change (cxx_stamps.json).
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
    """Names no GN action here could make (skip looking for one). The
    stand-in writers (WRITERS, PATTERN_WRITERS) are fallbacks: their GN
    action is tried first."""
    return bool(GRIT_RE.search(name)) or name.endswith(".rs.h")


def handles(name):
    return bool(GRIT_RE.search(name)) or name in WRITERS or \
        name.endswith((".rs.h", "_generated.h")) or \
        _pattern_writer(name) is not None


def is_fallback(name):
    """Whether a stand-in writer makes `name` when its GN action can't."""
    return name in WRITERS or _pattern_writer(name) is not None


STANDIN_STAMPS = "standin_stamps.json"
# Bump to retry replacing every fallback stand-in with its real generator
# (else a failure is remembered until the pin changes).
STANDIN_RETRY_VERSION = "1"


def replace_stand_ins(tree, interp, produce):
    """Remakes, with their real GN actions, the fallback stand-ins earlier
    runs wrote (a run before the action could run here, or one where it
    failed). A stand-in whose action fails again stays, and is not retried
    until STANDIN_RETRY_VERSION or the pin changes. Returns the names
    remade."""
    gen_dir = Path(tree.gen_dir)
    if not gen_dir.is_dir():
        return []
    cands = [n for n in WRITERS if (gen_dir / n).is_file()]
    for rx, _ in PATTERN_WRITERS:
        # The pattern writers' names, from what earlier runs wrote.
        d = rx.pattern.lstrip("^").split("/")
        sub = gen_dir.joinpath(*d[:-1]) if len(d) > 1 else gen_dir
        if sub.is_dir():
            for f in sorted(sub.iterdir()):
                rel = str(f.relative_to(gen_dir))
                if rx.match(rel):
                    cands.append(rel)
    try:
        stamps = json.loads((Path(tree.state) / STANDIN_STAMPS).read_text())
    except (OSError, ValueError):
        stamps = {}
    key = "%s %s" % (STANDIN_RETRY_VERSION, tree.chromium.rev)
    remade = []
    for n in cands:
        f = gen_dir / n
        try:
            head = f.read_bytes()[:200]
        except OSError:
            continue
        if b"compile_check STAND-IN" not in head or stamps.get(n) == key:
            continue
        aside = f.with_name(f.name + ".standin")
        f.rename(aside)
        try:
            ok = bool(produce([n])) and f.exists()
        except Exception:  # noqa: BLE001 - keep the stand-in
            ok = False
        if ok:
            aside.unlink()
            stamps.pop(n, None)
            remade.append(n)
        else:
            if f.exists():
                f.unlink()
            aside.rename(f)
            stamps[n] = key
    Path(tree.state).mkdir(parents=True, exist_ok=True)
    (Path(tree.state) / STANDIN_STAMPS).write_text(
        json.dumps(stamps, indent=1, sort_keys=True))
    return remade


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


# The Rust target of each platform (build/config/rust.gni's
# rust_abi_target).
RUST_TARGETS = {"linux": "x86_64-unknown-linux-gnu",
                "linux-vulkan": "x86_64-unknown-linux-gnu",
                "linux-arm64": "aarch64-unknown-linux-gnu",
                "win": "x86_64-pc-windows-msvc",
                "mac": "aarch64-apple-darwin"}

# What `rustc --print=cfg` says for x86_64-unknown-linux-gnu, for when no
# rustc is installed (the cfgs a bridge can test change only with the
# target).
_LINUX_X64_CFG = """debug_assertions
panic="unwind"
target_abi=""
target_arch="x86_64"
target_endian="little"
target_env="gnu"
target_family="unix"
target_feature="fxsr"
target_feature="sse"
target_feature="sse2"
target_has_atomic="16"
target_has_atomic="32"
target_has_atomic="64"
target_has_atomic="8"
target_has_atomic="ptr"
target_os="linux"
target_pointer_width="64"
target_vendor="unknown"
unix
"""

CXX_STAMPS = "cxx_stamps.json"
# Bump when the way bridge headers are made changes.
CXX_DRIVER_VERSION = "2"


def rustc_cfg(tree):
    """The lines of `rustc --print=cfg --target=<the platform's>` (what
    //build/rust/gni_impl:rustc_print_cfg writes), cached in the work
    dir."""
    plat = getattr(tree, "platform", None)
    target = RUST_TARGETS[plat.name if plat else "linux"]
    f = Path(tree.work) / "toolchain" / ("rustc-print-cfg-%s.txt" % target)
    if not f.exists():
        import shutil as _sh
        rustc = _sh.which("rustc") or os.path.expanduser(
            "~/.cargo/bin/rustc")
        text = None
        if os.path.exists(rustc):
            r = subprocess.run([rustc, "--print=cfg", "--target=" + target],
                               capture_output=True, text=True)
            if r.returncode == 0 and r.stdout.strip():
                text = r.stdout
        if text is None:
            if target != RUST_TARGETS["linux"]:
                raise RuntimeError("rustc --print=cfg --target=%s failed "
                                   "(is rustc installed?)" % target)
            text = _LINUX_X64_CFG
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text(text)
    return [x.strip() for x in f.read_text().splitlines() if x.strip()]


def _cxx_key(tree, src, exe, cfg):
    h = hashlib.sha1(CXX_DRIVER_VERSION.encode())
    h.update(tree.read(src) or b"-")
    h.update(b"\0" + str(exe).encode() + b"\0")
    try:
        h.update(Path(exe).parent.parent.joinpath("version").read_bytes())
    except OSError:
        pass
    h.update("\n".join(cfg).encode())
    return h.hexdigest()


def _load_cxx_stamps(tree):
    try:
        return json.loads((tree.state / CXX_STAMPS).read_text())
    except (OSError, ValueError):
        return {}


def _save_cxx_stamps(tree, stamps):
    Path(tree.state).mkdir(parents=True, exist_ok=True)
    (tree.state / CXX_STAMPS).write_text(json.dumps(stamps, indent=1,
                                                    sort_keys=True))


def cxx_bridge_header(tree, interp, name):
    src = name[:-2]
    if not tree.exists(src):
        raise RuntimeError("%s not found" % src)
    tree.materialize([src])
    exe = TOOLS["cxxbridge"]()
    cfg = rustc_cfg(tree)
    # run_cxxbridge.py: the source, then a --cfg per rustc cfg line.
    r = subprocess.run([str(exe), str(tree.tree_dir / src)] +
                       ["--cfg=" + c for c in cfg] + ["--header"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("cxxbridge: " + r.stderr[-1000:])
    stamps = _load_cxx_stamps(tree)
    stamps[name] = _cxx_key(tree, src, exe, cfg)
    _save_cxx_stamps(tree, stamps)
    return r.stdout


def refresh_cxx(tree, interp):
    """Remakes every bridge header (gen/**/*.rs.h) whose .rs, cfgs or
    cxxbridge changed since it was made, or that has no stamp (made before
    stamps); removes the header of a .rs that is gone. Returns (remade,
    removed)."""
    gen_dir = Path(tree.gen_dir)
    if not gen_dir.is_dir():
        return [], []
    headers = sorted(str(f.relative_to(gen_dir))
                     for f in gen_dir.glob("**/*.rs.h"))
    if not headers:
        return [], []
    stamps = _load_cxx_stamps(tree)
    remade, removed = [], []
    try:
        exe = TOOLS["cxxbridge"]()
        cfg = rustc_cfg(tree)
    except (KeyError, RuntimeError) as e:
        log("cxx: cannot check bridge headers: %s" % e)
        return [], []
    def drop(n):
        (gen_dir / n).unlink()
        st = _load_cxx_stamps(tree)
        st.pop(n, None)
        _save_cxx_stamps(tree, st)
        removed.append(n)

    for n in headers:
        src = n[:-2]
        if not tree.exists(src):
            drop(n)
            continue
        if stamps.get(n) == _cxx_key(tree, src, exe, cfg):
            continue
        try:
            text = cxx_bridge_header(tree, interp, n)  # stamps it
            (gen_dir / n).write_text(text)
            remade.append(n)
        except RuntimeError as e:
            log("cxx: %s: %s; removed" % (n, str(e).splitlines()[-1][:200]
                                         if str(e) else e))
            drop(n)
    return remade, removed


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
# Bump when grit_headers.py changes what it writes, or to retry the
# headers grit failed on (the failures are remembered per version: "2"
# retries those of runs before the first-ids file was made reliably, "3"
# those that failed on ninja's asserted output lists).
GRIT_DRIVER_VERSION = "3"


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
        return json.loads((tree.state / GRIT_STAMPS).read_text())
    except (OSError, ValueError):
        return {}


def _save_stamps(tree, stamps):
    Path(tree.state).mkdir(parents=True, exist_ok=True)
    (tree.state / GRIT_STAMPS).write_text(json.dumps(stamps, indent=1,
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
_NINJA_ONLY = ("--assert-file-list", "--assert-output-file-list",
               "--assert-input-file-list", "--depfile", "--depdir",
               "--brotli", "--css-minifier", "--android-output-zip")


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


TEMPLATE_PLACEHOLDER_RE = re.compile(r"\[\[([A-Z0-9_]+)\]\]")

# The pins template's tags, as the generator writes them
# (preloaded_state_generator.cc: ProcessSPKIHashes, ProcessPinsets,
# ProcessPinEntries), with one made-up pinset for a host under .invalid
# (a zero hash), so the declarations and their uses compile as they do.
PINS_FILL = {
    "SPKI_HASHES": "static constexpr SHA256HashValue "
                   "kSPKIHash_CompileCheck = {\n  " + "0x00, " * 31 +
                   "0x00};",
    "ACCEPTABLE_CERTS": "static constexpr SHA256HashValue const * "
                        "kCompileCheckAcceptableCerts[] = {\n"
                        "    &kSPKIHash_CompileCheck,\n};",
    "PINSETS": "static constexpr net::TransportSecurityStatePinsSource::"
               "Pinset kCompileCheckPinset = "
               "{kCompileCheckAcceptableCerts, {}};",
    "HOST_PINS": "base::MakeFixedFlatMap<std::string_view, "
                 "net::TransportSecurityStatePinsSource::HostPin>({\n"
                 "  {\"compile-check.invalid\", {&kCompileCheckPinset, "
                 "false}},\n})",
}


def preload_header(tree, interp, name):
    """net/http/transport_security_state_static*.h from the .template its
    GN action (a compiled_action() running transport_security_state_
    generator) names, with placeholder data (see STAND-INS above)."""
    from .gen_actions import find_action
    act = find_action(tree, interp, name)
    if act is None:
        raise RuntimeError("no action writes %s" % name)
    tmpl = None
    for x in act.get("inputs") or []:
        if isinstance(x, str) and x.endswith(".template"):
            tmpl = interp.abspath(x, {"dir": act.path})[2:]
    if tmpl is None:
        raise RuntimeError("%s: no .template input" % act)
    text = tree.read(tmpl)
    if text is None:
        raise RuntimeError("%s not found" % tmpl)
    text = text.decode()

    def fill(m):
        # The generator's shape for the pins tags; else an array
        # initializer after "[] = ", a number otherwise.
        if m.group(1) in PINS_FILL:
            return PINS_FILL[m.group(1)]
        before = text[:m.start()].rstrip()
        return "{0}" if before.endswith("[] =") else "0"

    body = TEMPLATE_PLACEHOLDER_RE.sub(fill, text)
    return ("// compile_check STAND-IN for %s: the real %s, placeholder "
            "data.\n" % (name, tmpl)) + body


WRITERS = {
    "base/generated_build_date.h": build_date,
}

# Writers for names matching a pattern (fallbacks too).
PATTERN_WRITERS = [
    (re.compile(r"^net/http/transport_security_state_static[a-z0-9_]*\.h$"),
     preload_header),
]


def _pattern_writer(name):
    for rx, fn in PATTERN_WRITERS:
        if rx.match(name):
            return fn
    return None


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
            elif _pattern_writer(n) is not None:
                text = _pattern_writer(n)(tree, interp, n)
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
