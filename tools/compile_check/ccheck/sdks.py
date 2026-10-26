# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""System headers of the platforms other than Linux (platforms.py).

Windows: the Windows SDK of build/vs_toolchain.py's SDK_VERSION (10.0.28000)
and the MSVC runtime headers (vcruntime*.h, sal.h, ...), from Microsoft's
packages on nuget.org (Microsoft.Windows.SDK.CPP and
VisualCppTools.Community.VS2017Layout), each checked against a pinned
SHA-512. Chromium's own toolchain package (vs_toolchain.py) is Google-only,
and Visual Studio's download servers are not reachable from every machine;
nuget.org is. Only the headers are kept (ucrt, um, shared, winrt from the
SDK; MSVC's include/), with a lower-case alias of every file name, since
Windows finds headers case-insensitively. Known gap: the MSVC headers are
VS 2017's (14.11), the newest on nuget.org, where Chromium builds with VS
2022's; only vcruntime's few headers are reached (libc++ replaces the STL),
and they changed little. The one change libc++ needs is added to
vcruntime_new.h (ALIGNED_NEW): the C++17 aligned allocation functions,
declared there from 14.12 on.

macOS: see ensure_mac.
"""
import base64
import hashlib
import json
import os
import shutil
import urllib.request
import zipfile
from pathlib import Path

from .sources import log

NUGET = "https://api.nuget.org/v3-flatcontainer/%s/%s/%s.%s.nupkg"

# (package id, version, SHA-512 of the .nupkg (base64, as nuget.org's
# catalog lists it), the zip prefix to keep, where it goes).
WIN_SDK_VERSION = "10.0.28000.0"
WIN_PACKAGES = [
    ("microsoft.windows.sdk.cpp", "10.0.28000.1721",
     "XppWgJxwl9/gVxBywY6zYNq32CH09mYwsSuZMA9YFKmIk/mHsqfAY5aiSo4pltv9wX5F"
     "JJ9RDUlaPQu5aa97Fw==",
     "c/Include/%s/" % WIN_SDK_VERSION, "sdk"),
    ("visualcpptools.community.vs2017layout", "14.11.25547",
     "qCvT3Fqskcj5jmu1M5TLS+fm02lry/Qq3C62/pxlsXHB1z1FNVPHuvcHeuqLNNsapN2V"
     "qCtJqOrdObP2PjwwAQ==",
     "lib/native/include/", "msvc"),
]
# The SDK's include subdirectories, in the order setup_toolchain.py puts
# them in INCLUDE (after MSVC's include).
WIN_SDK_DIRS = ["ucrt", "shared", "um", "winrt"]
# Bump when what ensure_windows lays out changes.
WIN_LAYOUT_VERSION = "2"


def winsdk_dir(work):
    return Path(work) / "toolchain" / "winsdk"


def _sha512_b64(path):
    h = hashlib.sha512()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return base64.b64encode(h.digest()).decode()


def _download(url, dest, sha512_b64):
    """dest, checked against the pinned hash (a file already there with
    that hash is kept)."""
    if dest.exists() and _sha512_b64(dest) == sha512_b64:
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    log("downloading %s" % url)
    h = hashlib.sha512()
    tmp = dest.with_name(dest.name + ".partial")
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
            f.write(chunk)
    got = base64.b64encode(h.digest()).decode()
    if got != sha512_b64:
        tmp.unlink()
        raise RuntimeError("%s: SHA-512 mismatch" % url)
    tmp.rename(dest)


def _extract(nupkg, prefix, dest, only=None):
    """Writes the members under `prefix` to dest (no path escapes); with
    `only`, those under one of its subdirectories."""
    n = 0
    with zipfile.ZipFile(nupkg) as z:
        for info in z.infolist():
            name = info.filename.replace("\\", "/")
            if not name.startswith(prefix) or name.endswith("/"):
                continue
            rel = os.path.normpath(name[len(prefix):])
            if rel.startswith("..") or os.path.isabs(rel):
                continue
            if only and rel.split("/", 1)[0] not in only:
                continue
            out = dest / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(out, "wb") as f:
                shutil.copyfileobj(src, f)
            n += 1
    return n


def add_lowercase_aliases(root):
    """A lower-case symlink next to every file and directory whose name has
    capitals (<windows.h> finds Windows.h, <WinSock2.h> finds
    winsock2.h's alias the other way round is not needed: the SDK's own
    names are lower-case or exact). Returns how many were made."""
    n = 0
    for dirpath, dirs, files in os.walk(root):
        for name in dirs + files:
            low = name.lower()
            if low == name:
                continue
            link = Path(dirpath) / low
            if not link.exists() and not link.is_symlink():
                os.symlink(name, link)
                n += 1
    return n


def write_case_insensitive_overlay(dirs, path):
    """A clang VFS overlay listing every file under `dirs`, looked up
    case-insensitively (as on Windows: the SDK's headers include
    <SpecStrings.h>, which is specstrings.h). Passed with -ivfsoverlay."""
    def tree_of(d):
        entries = []
        for child in sorted(Path(d).iterdir()):
            if child.is_symlink():
                continue  # the lower-case aliases
            if child.is_dir():
                entries.append({"type": "directory", "name": child.name,
                                "contents": tree_of(child)})
            elif child.is_file():
                entries.append({"type": "file", "name": child.name,
                                "external-contents": str(child)})
        return entries

    roots = [{"type": "directory", "name": str(d), "contents": tree_of(d)}
             for d in dirs]
    Path(path).write_text(json.dumps({
        "version": 0, "case-sensitive": "false",
        "overlay-relative": False, "roots": roots}))
    return path


def windows_overlay(work):
    return winsdk_dir(work) / "case_insensitive_overlay.yaml"


# What vcruntime_new.h declares from MSVC 14.12 (VS 2017 15.5) on, under
# the same condition: the C++17 aligned allocation functions, which
# libc++'s <new> takes from vcruntime (_LIBCPP_ABI_VCRUNTIME) and its
# temporary buffers (std::stable_sort) call.
ALIGNED_NEW_MARK = "// compile_check: MSVC 14.12's aligned allocation"
ALIGNED_NEW = ALIGNED_NEW_MARK + """ functions.
#ifdef __cpp_aligned_new
namespace std { enum class align_val_t : size_t {}; }
_VCRT_ALLOCATOR void* __CRTDECL operator new(size_t _Size,
                                             std::align_val_t _Al);
_VCRT_ALLOCATOR void* __CRTDECL operator new(size_t _Size,
                                             std::align_val_t _Al,
                                             std::nothrow_t const&) noexcept;
_VCRT_ALLOCATOR void* __CRTDECL operator new[](size_t _Size,
                                               std::align_val_t _Al);
_VCRT_ALLOCATOR void* __CRTDECL operator new[](
    size_t _Size, std::align_val_t _Al, std::nothrow_t const&) noexcept;
void __CRTDECL operator delete(void* _Block, std::align_val_t _Al) noexcept;
void __CRTDECL operator delete(void* _Block, std::align_val_t _Al,
                               std::nothrow_t const&) noexcept;
void __CRTDECL operator delete[](void* _Block, std::align_val_t _Al) noexcept;
void __CRTDECL operator delete[](void* _Block, std::align_val_t _Al,
                                 std::nothrow_t const&) noexcept;
void __CRTDECL operator delete(void* _Block, size_t _Size,
                               std::align_val_t _Al) noexcept;
void __CRTDECL operator delete[](void* _Block, size_t _Size,
                                 std::align_val_t _Al) noexcept;
#endif

"""


def add_aligned_new(vcruntime_new):
    """Adds ALIGNED_NEW to MSVC 14.11's vcruntime_new.h (once), before its
    placement forms' end. Returns whether it changed the file."""
    text = vcruntime_new.read_text()
    if ALIGNED_NEW_MARK in text:
        return False
    anchor = '#pragma pop_macro("new")'
    if anchor not in text:
        raise RuntimeError("%s: no %s" % (vcruntime_new, anchor))
    vcruntime_new.write_text(text.replace(anchor, ALIGNED_NEW + anchor, 1))
    return True


def ensure_windows(work):
    """[include dirs] of the Windows SDK and MSVC's runtime headers,
    downloaded and laid out once (toolchain/winsdk)."""
    root = winsdk_dir(work)
    stamp = root / ".stamp"
    want = json.dumps([WIN_LAYOUT_VERSION] + [p[:3] for p in WIN_PACKAGES])
    dirs = [root / "msvc"] + [root / "sdk" / d for d in WIN_SDK_DIRS]
    if stamp.exists() and stamp.read_text() == want:
        add_aligned_new(root / "msvc" / "vcruntime_new.h")
        overlay = windows_overlay(work)
        # Its paths are absolute: rewritten for a layout copied from
        # another work dir.
        if not overlay.exists() or \
                ('"%s' % root) not in overlay.read_text()[:4096]:
            write_case_insensitive_overlay(dirs, overlay)
        return dirs
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir(parents=True)
    dl = Path(work) / "toolchain" / "downloads"
    for pkg, ver, sha, prefix, sub in WIN_PACKAGES:
        nupkg = dl / ("%s.%s.nupkg" % (pkg, ver))
        _download(NUGET % (pkg, ver, pkg, ver), nupkg, sha)
        # Only the dirs the build searches (not cppwinrt).
        n = _extract(nupkg, prefix, root / sub,
                     only=WIN_SDK_DIRS if sub == "sdk" else None)
        nupkg.unlink()
        log("%s %s: %d header(s)" % (pkg, ver, n))
    add_aligned_new(root / "msvc" / "vcruntime_new.h")
    log("windows headers: %d lower-case alias(es)" %
        add_lowercase_aliases(root))
    write_case_insensitive_overlay(dirs, windows_overlay(work))
    stamp.write_text(want)
    return dirs


# Darwin's libc and libSystem headers (mach/, os/, dispatch/, objc/,
# sys/, malloc/, TargetConditionals.h, ...), as the Zig toolchain ships
# them to cross-compile for macOS: its PyPI wheel, pinned by SHA-256.
ZIG_WHEEL = (
    "https://files.pythonhosted.org/packages/fe/6f/"
    "e1c380fe61db36cb464d5d4d18c557338812500986885373e2550d4ebf6b/"
    "ziglang-0.17.0-py3-none-manylinux_2_12_x86_64.manylinux2010_x86_64."
    "musllinux_1_1_x86_64.whl",
    "97079827064a7492cce5541ceb438ae8003ce1e54e399676da2d62fb7a5dc08b")
ZIG_DARWIN_PREFIX = "ziglang/lib/libc/include/any-darwin-any/"
MAC_LAYOUT_VERSION = "1"
# The Apple frameworks' stand-ins (checked in: one directory per
# framework, laid out as its Headers/).
MAC_STAND_INS = Path(__file__).resolve().parent.parent / "mac_stand_ins"
# Its directory of usr/include stand-ins (not a framework).
MAC_USR_INCLUDE = "usr_include"


def mac_sdk_dir(work):
    return Path(work) / "toolchain" / "macsdk"


def _download_sha256(url, dest, sha256):
    if dest.exists() and hashlib.sha256(dest.read_bytes()).hexdigest() == \
            sha256:
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    log("downloading %s" % url)
    h = hashlib.sha256()
    tmp = dest.with_name(dest.name + ".partial")
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        for chunk in iter(lambda: r.read(1 << 20), b""):
            h.update(chunk)
            f.write(chunk)
    if h.hexdigest() != sha256:
        tmp.unlink()
        raise RuntimeError("%s: SHA-256 mismatch" % url)
    tmp.rename(dest)


def ensure_mac(work, tree=None):
    """A macOS "SDK" for -isysroot: usr/include holds Darwin's real libc
    and libSystem headers (ZIG_WHEEL); System/Library/Frameworks holds the
    stand-ins of MAC_STAND_INS (linked, so edits show up at once).
    Returns the dirs whose headers are the system's."""
    root = mac_sdk_dir(work)
    stamp = root / ".stamp"
    want = json.dumps([MAC_LAYOUT_VERSION, ZIG_WHEEL[1]])
    if not (stamp.exists() and stamp.read_text() == want):
        shutil.rmtree(root, ignore_errors=True)
        root.mkdir(parents=True)
        whl = Path(work) / "toolchain" / "downloads" / \
            ZIG_WHEEL[0].rsplit("/", 1)[1]
        _download_sha256(ZIG_WHEEL[0], whl, ZIG_WHEEL[1])
        n = _extract(whl, ZIG_DARWIN_PREFIX, root / "usr" / "include")
        whl.unlink()
        log("darwin headers: %d" % n)
        stamp.write_text(want)
    fw = root / "System" / "Library" / "Frameworks"
    fw.mkdir(parents=True, exist_ok=True)
    # libSystem headers the Zig toolchain's set lacks (usr_include/...),
    # linked into usr/include where it has no file of that name.
    extra = MAC_STAND_INS / MAC_USR_INCLUDE
    for f in sorted(extra.rglob("*.h")) if extra.is_dir() else []:
        dst = root / "usr" / "include" / f.relative_to(extra)
        if dst.is_symlink() and os.readlink(dst) == str(f):
            continue
        if not dst.exists() or dst.is_symlink():
            dst.parent.mkdir(parents=True, exist_ok=True)
            if dst.is_symlink():
                dst.unlink()
            os.symlink(f, dst)
    have = {d.name for d in MAC_STAND_INS.iterdir()
            if d.is_dir() and d.name != MAC_USR_INCLUDE} \
        if MAC_STAND_INS.is_dir() else set()
    for link in fw.iterdir():
        if link.name[:-len(".framework")] not in have:
            shutil.rmtree(link) if not link.is_symlink() and \
                link.is_dir() else link.unlink()
    for name in sorted(have):
        d = fw / (name + ".framework")
        d.mkdir(exist_ok=True)
        h = d / "Headers"
        if not h.is_symlink() or os.readlink(h) != str(MAC_STAND_INS / name):
            if h.is_symlink() or h.exists():
                h.unlink()
            os.symlink(MAC_STAND_INS / name, h)
    return [root / "usr" / "include", fw]


def stand_in_label(platform, lang=None):
    """How report names a platform's system headers when they are
    stand-ins (None when they are the real ones)."""
    if platform == "mac":
        return "Apple framework stand-ins (mac_stand_ins/)"
    return None
