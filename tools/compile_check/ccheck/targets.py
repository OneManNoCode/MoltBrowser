# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Which files the check compiles by default."""
import os
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[2] / "src"

# Priority 1: MoltBrowser's own code.
FIRST_PARTY_DIRS = [
    "chrome/browser/molt_ai",
    "chrome/browser/ui/webui/molt_ai",
    "chrome/services/molt_ai",
    "chrome/common/molt_ai",
    "chrome/renderer/molt_ai",
    "third_party/blink/renderer/core/molt_fingerprinting",
    "third_party/blink/renderer/core/molt_net",
]
FIRST_PARTY_GLOBS = [
    ("components/omnibox/browser", "molt_"),
    ("chrome/browser/ui/views/toolbar", "molt_"),
    ("content/utility/on_device_model", "molt_"),
]

# Priority 2: patched upstream files with MoltBrowser hooks
# (docs/BUILD_MACHINE_CHECKLIST.md section 4).
UPSTREAM_HOOKS = [
    "net/socket/socks_connect_job.cc",
    "net/socket/ssl_client_socket.cc",
    "net/http/http_network_session.cc",
    "services/network/network_context.cc",
    "services/cert_verifier/cert_net_url_loader/cert_net_fetcher_url_loader.cc",
    "services/cert_verifier/cert_verifier_service_factory.cc",
    "chrome/browser/net/profile_network_context_service.cc",
    "chrome/browser/ssl/chrome_security_blocking_page_factory.cc",
    "chrome/browser/profiles/off_the_record_profile_impl.cc",
    "net/dns/host_resolver_manager.cc",
    "net/base/features.cc",
    "components/translate/core/browser/translate_manager.cc",
    "components/translate/core/browser/translate_language_list.cc",
    "chrome/browser/translate/translate_service.cc",
    "chrome/browser/translate/chrome_translate_client.cc",
    "chrome/browser/ui/views/translate/translate_bubble_view.cc",
    "chrome/browser/chrome_content_browser_client.cc",
    "chrome/browser/browsing_data/chrome_browsing_data_remover_delegate.cc",
    "chrome/browser/profile_resetter/profile_resetter.cc",
    "chrome/browser/profiles/chrome_browser_main_extra_parts_profiles.cc",
    "components/omnibox/browser/autocomplete_controller.cc",
    "chrome/browser/ui/omnibox/chrome_omnibox_client.cc",
    "components/omnibox/browser/ai_prompt_provider.cc",
    "chrome/utility/services.cc",
    "content/utility/on_device_model/on_device_model_sandbox_init.cc",
    "chrome/browser/ui/views/frame/contents_web_view.cc",
    "chrome/browser/universal_web_contents_observers.cc",
    "chrome/browser/about_flags.cc",
    "content/browser/network/network_quality_observer_impl.cc",
    "content/browser/network/browser_online_state_observer.cc",
    "content/public/browser/content_browser_client.cc",
]

# Sources the check compiles: C, C++, Objective-C and Objective-C++, each
# for its platform (platforms.py).
SOURCE_EXTS = (".cc", ".cpp", ".c", ".mm", ".m")

# Third-party code vendored under src/ that is not Chromium's and is built
# by its own build elsewhere: llama.cpp's own sources (built and tested by
# its CMake build, see CLAUDE.md; MoltBrowser's glue in chromium/ is
# checked) and the vendored Rust crates' C++ (a Qt test tool of ttf-parser
# that no GN target builds).
VENDORED = ("third_party/llama_cpp/",
            "third_party/rust/chromium_crates_io/vendor/")
VENDORED_CHECKED = ("third_party/llama_cpp/chromium/",)


def is_vendored(path):
    return path.startswith(VENDORED) and not path.startswith(
        VENDORED_CHECKED)


def _cc_under(d):
    out = []
    for dirpath, dirs, files in os.walk(SRC / d):
        for f in files:
            if f.endswith(SOURCE_EXTS):
                out.append(os.path.relpath(os.path.join(dirpath, f), SRC))
    return sorted(out)


def first_party():
    out = []
    for d in FIRST_PARTY_DIRS:
        out += _cc_under(d)
    for d, prefix in FIRST_PARTY_GLOBS:
        out += [p for p in _cc_under(d)
                if os.path.basename(p).startswith(prefix)]
    return out


def upstream_hooks():
    return [p for p in UPSTREAM_HOOKS if (SRC / p).exists()]


def default_targets(tree=None):
    if tree is None:
        return first_party() + upstream_hooks()
    return changed(tree)


def _upstream_oid(tree, p):
    """The blob id of `p` upstream (Chromium or a mounted subproject), or
    None if it is not an upstream file."""
    e = tree.chromium.entries.get(p)
    if e is not None:
        return e.oid if e.type == "blob" else None
    if tree.mount_of(p) is None:
        return None
    ent = tree.upstream_entry(p)
    return ent[1] if ent else None


def _blob_oid(p):
    import hashlib
    data = (SRC / p).read_bytes()
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def patched(tree):
    """Every upstream source file (C, C++, Objective-C++; Chromium's own or
    a subproject's) the overlay carries a modified copy of."""
    out = []
    for p in sorted(tree.overlay_set()):
        if not p.endswith(SOURCE_EXTS) or is_vendored(p):
            continue
        oid = _upstream_oid(tree, p)
        if oid is not None and oid != _blob_oid(p):
            out.append(p)
    return out


def changed_headers(tree):
    """Every header (.h) the overlay adds or changes, vendored code
    excepted."""
    out = []
    for p in sorted(tree.overlay_set()):
        if not p.endswith(".h") or is_vendored(p):
            continue
        oid = _upstream_oid(tree, p)
        if oid is None or oid != _blob_oid(p):
            out.append(p)
    return out


def changed(tree):
    """Every C/C++/Objective-C(++) source MoltBrowser adds or changes: the
    overlay's new files and its modified upstream copies, except vendored
    third-party code (VENDORED)."""
    out = []
    for p in sorted(tree.overlay_set()):
        if not p.endswith(SOURCE_EXTS) or is_vendored(p):
            continue
        oid = _upstream_oid(tree, p)
        if oid is None or oid != _blob_oid(p):
            out.append(p)
    return out


def expand(args, tree=None):
    """Paths relative to src/ (files or directories) -> source files.
    @changed (every source MoltBrowser adds or changes; the default),
    @first-party, @hooks (section 4 of the build checklist) and @patched
    (every modified upstream source) name groups."""
    if not args:
        return default_targets(tree)
    out = []
    for a in args:
        a = a[len("src/"):] if a.startswith("src/") else a
        if a == "@changed":
            out += changed(tree)
        elif a == "@first-party":
            out += first_party()
        elif a == "@hooks":
            out += upstream_hooks()
        elif a == "@patched":
            out += patched(tree)
        elif a.startswith("@list:"):
            with open(a[6:]) as f:
                out += expand([x.strip() for x in f if x.strip() and
                               x.strip().endswith(SOURCE_EXTS + (".h",))],
                              tree)
        elif (SRC / a).is_dir():
            out += _cc_under(a)
        else:
            out.append(a)
    return list(dict.fromkeys(out))


# Platforms a file's other-platform branches are checked on, besides its own
# (platforms.of): MoltBrowser's hooks in a Linux-built file's
# `#if BUILDFLAG(IS_WIN)` block (utility_main.cc's warm-up) or in the
# IS_APPLE branch of a shared file (font_metrics.cc) are compiled there,
# and those in the NEON branch of a file the x64 builds compile without it
# (WebRTC's `#if defined(WEBRTC_HAS_NEON)`) on Linux arm64, as the
# arm-only sources are (the Mac arm64 build compiles them too; Linux arm64
# has the real headers).
EXTRA_PLATFORM_CANDIDATES = ("linux", "linux-arm64", "mac", "win",
                             "linux-vulkan")


# Headers made to be included in a context (several times, after macros
# their includer sets): checked through a source that includes them.
TEXTUAL_HEADER_RE = re.compile(r"[_-]inl\.h$|\.inc$")


def includer_of(tree, header):
    """A source next to `header`, or one directory up, whose text includes
    it (by its name), or None."""
    name = header.rsplit("/", 1)[-1].encode()
    d = header.rsplit("/", 1)[0]
    dirs = [d] + ([d.rsplit("/", 1)[0]] if "/" in d else [])
    for base in dirs:
        for p in sorted(tree.glob_upstream(base + "/")):
            if not p.endswith((".c", ".cc", ".cpp")) or \
                    p.count("/") > base.count("/") + 2:
                continue
            data = tree.read(p) or b""
            for line in data.split(b"\n"):
                if line.lstrip().startswith(b"#include") and name in line:
                    return p
    return None


def changed_lines(new, old):
    """0-based indices of the lines of `new` (bytes) that are not in `old`
    (bytes, or None for a new file: every line)."""
    import difflib
    a = new.split(b"\n")
    if old is None:
        return set(range(len(a)))
    b = old.split(b"\n")
    out = set()
    sm = difflib.SequenceMatcher(None, b, a, autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag in ("replace", "insert"):
            out.update(range(j1, j2))
    return out


def extra_platforms(path, new, old):
    """The platforms besides platforms.of(path) on which some of the file's
    changed lines are compiled while its own platform certainly skips
    them (scan.live_lines with each platform's certain atoms)."""
    from . import platforms, scan
    own = platforms.of(path)
    if own is None or not path.endswith(SOURCE_EXTS):
        return []
    lines = changed_lines(new, old)
    if not lines:
        return []

    def live(name):
        p = platforms.get(name)
        return scan.live_lines(new, scan.Atoms(p.true_atoms, p.false_atoms))

    own_live = live(own)
    # The own platform's lines with NEON's atoms unknown: a line dead only
    # without them is in a NEON branch.
    p = platforms.get(own)
    neon_live = scan.live_lines(new, scan.Atoms(
        p.true_atoms - platforms.NEON_ATOMS,
        p.false_atoms - platforms.NEON_ATOMS))
    out = []
    needs = {}
    for name in EXTRA_PLATFORM_CANDIDATES:
        if name == own or not platforms.may_build(path, name):
            continue
        other = live(name)
        need = {i for i in lines if i < len(own_live) and i < len(other)
                and not own_live[i] and other[i]}
        if name == "linux-arm64":
            need = {i for i in need if i < len(neon_live) and neon_live[i]}
        if not need:
            continue
        # NEON branches are checked on Linux arm64 (above); the Mac only
        # for lines of its own.
        if name == "mac" and need <= needs.get("linux-arm64", set()):
            continue
        needs[name] = need
        out.append(name)
    return out


# MoltBrowser's own sources are compiled for Windows too, when a Windows
# GN target builds them (if_built): the same code compiles differently
# there (base::FilePath holds wide strings, Win32's headers and macros).
OWN_SOURCES_ALSO_ON = ("win",)
OWN_SOURCE_EXTS = (".cc", ".cpp", ".c")


def if_built(pairs):
    """{platform: {paths}} of the (path, platform, "if_built") entries of
    platform_targets: checked there only if a GN target there builds
    them."""
    out = {}
    for x in pairs:
        if len(x) == 3 and x[2] == "if_built":
            out.setdefault(x[1], set()).add(x[0])
    return out


def platform_targets(tree, paths):
    """[(path, platform)] for the files to check: each on its own platform
    and on the extra platforms its changed lines need (extra_platforms;
    upstream copies compared with the pinned file); MoltBrowser's own
    sources also as (path, platform, "if_built") on OWN_SOURCES_ALSO_ON."""
    from . import platforms
    oids = {}
    for p in paths:
        if (SRC / p).is_file() and p.endswith(SOURCE_EXTS):
            ent = tree.upstream_entry(p) if tree is not None else None
            if ent is not None:
                oids[p] = ent
    old = {}
    by_repo = {}
    for p, (repo, oid) in oids.items():
        wanted = by_repo.setdefault(id(repo), (repo, {}))[1]
        wanted.setdefault(oid, []).append(p)
    for repo, wanted in by_repo.values():
        try:
            for oid, data in repo.blobs(list(wanted)):
                for p in wanted[oid]:
                    old[p] = data
        except (RuntimeError, OSError, KeyError):
            pass  # left out: every line counts as changed
    out = []
    for p in paths:
        out.append((p, platforms.of(p)))
        if not (SRC / p).is_file():
            continue
        new = (SRC / p).read_bytes()
        prev = old.get(p) if p in oids else None
        if p in oids and prev is None:
            # The pinned copy can't be read: its MoltBrowser-marked lines.
            lines = new.split(b"\n")
            prev = b"\n".join(x for x in lines if b"MoltBrowser" not in x)
        extras = extra_platforms(p, new, prev)
        for extra in extras:
            out.append((p, extra))
        if p not in oids and p.endswith(OWN_SOURCE_EXTS) and \
                platforms.of(p) == platforms.DEFAULT:
            for name in OWN_SOURCES_ALSO_ON:
                if name not in extras and platforms.may_build(p, name):
                    out.append((p, name, "if_built"))
    return out
