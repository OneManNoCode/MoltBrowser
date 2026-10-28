# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Compiler flags from GN config() targets, read with the GN reader.

A real build gives each file the public configs of everything it depends
on. Here a fixed set stands for that: the configs every chrome/browser file
gets through its dependencies (GLOBAL_CONFIGS), and those of the target a
file belongs to, by directory (DIR_CONFIGS: Blink's inside_blink, ...).
"""
import posixpath

from .sources import log

GLOBAL_CONFIGS = [
    "//ui/views:flags",
    "//skia:skia_config",
    "//third_party/perfetto/gn:public_config",
    "//third_party/boringssl:external_config",
    "//third_party/webrtc:common_inherited_config",
    "//v8:external_config",
]

DIR_CONFIGS = {
    # GetContentClient() and friends are declared for content's own files.
    "content/": ["//content:content_implementation"],
    "third_party/blink/renderer/": [
        "//third_party/blink/renderer:inside_blink",
        "//third_party/blink/renderer:features",
        "//third_party/blink/renderer:config",
    ],
}


def _keep_cflag(c, interp):
    """A config's cflag the check passes on: not a clang-cl flag on the
    GCC-style platforms, nothing GN could not compute."""
    if not isinstance(c, str) or "<UNKNOWN>" in c:
        return False
    if c.startswith("/") and not _is_win(interp):
        return False
    return True


def _is_win(interp):
    try:
        return interp.globals.get("current_os") == "win"
    except KeyError:
        return False


def _flags_of(interp, tree, label, seen, out, cache, lang=None):
    if label in seen:
        return
    seen.add(label)
    path, _, name = label[2:].partition(":")
    bf = "//" + path + "/BUILD.gn"
    if bf not in cache:
        try:
            cache[bf] = interp.run_file(bf)
        except Exception as e:  # noqa: BLE001 - report, keep going
            log("gnconfigs: %s: %s" % (bf, e))
            cache[bf] = []
    for t in cache[bf]:
        if t.func != "config" or t.name != name:
            continue
        for d in t.get("defines") or []:
            if isinstance(d, str) and "<UNKNOWN>" not in d:
                out.append("-D" + d)
        for d in t.get("include_dirs") or []:
            if not isinstance(d, str) or "<UNKNOWN>" in d:
                continue
            a = interp.abspath(d, {"dir": t.path})
            out.append("-I" + str(tree.tree_dir / a[2:]))
        for key in ["cflags"] + _lang_keys(lang):
            v = t.get(key)
            for c in v if isinstance(v, list) else []:
                if _keep_cflag(c, interp):
                    out.append(c)
        for sub in t.get("configs") or []:
            if isinstance(sub, str):
                sub = sub.split("(")[0]
                if sub.startswith(":"):
                    sub = t.path + sub
                _flags_of(interp, tree, sub, seen, out, cache, lang)
        return
    log("gnconfigs: no config %s" % label)


def _lang_keys(lang):
    """GN's language-specific cflags keys for a file's language."""
    return {"c": ["cflags_c"], "c++": ["cflags_cc"],
            "objective-c": ["cflags_objc"],
            "objective-c++": ["cflags_objcc"]}.get(lang, [])


NO_EXCEPTIONS = "//build/config/compiler:no_exceptions"
EXCEPTIONS = "//build/config/compiler:exceptions"
NO_RTTI = "//build/config/compiler:no_rtti"
RTTI = "//build/config/compiler:rtti"


def _label(c, base_dir):
    """A config label as written in a target -> "//dir:name". A label
    without "//" is relative to the target's directory `base_dir`, as GN
    resolves it (":x", "..:x", "sub:x", "../dir"); a directory alone names
    the config called like its last part."""
    c = c.split("(")[0]
    d, colon, name = c.partition(":")
    if d.startswith("//"):
        d = "//" + posixpath.normpath(d[2:]) if d[2:].strip("/") else "//"
    else:
        d = posixpath.normpath(posixpath.join(base_dir[2:], d))
        d = "//" if d in (".", "") else "//" + d
    if not colon:
        name = d.rstrip("/").rsplit("/", 1)[-1]
    return d + ":" + name


def _config_cflags(interp, tree, label, cache, seen=None):
    """cflags + cflags_cc of a config() and the configs it pulls in."""
    seen = seen if seen is not None else set()
    if label in seen:
        return []
    seen.add(label)
    path, _, name = label[2:].partition(":")
    bf = "//" + path + "/BUILD.gn"
    if bf not in cache:
        try:
            cache[bf] = interp.run_file(bf)
        except Exception as e:  # noqa: BLE001 - report, keep going
            log("gnconfigs: %s: %s" % (bf, e))
            cache[bf] = []
    out = []
    for t in cache[bf]:
        if t.func != "config" or t.name != name:
            continue
        for key in ("cflags", "cflags_cc"):
            out += [c for c in t.get(key) or [] if isinstance(c, str)]
        for sub in t.get("configs") or []:
            if isinstance(sub, str):
                out += _config_cflags(interp, tree, _label(sub, t.path),
                                      cache, seen)
    return out


def language_flags(t, interp, tree, cache):
    """{"exceptions": bool, "rtti": bool} for a GN target, as its configs
    give them: Chromium's defaults turn both off (no_exceptions, no_rtti in
    default_compiler_configs); a target turns one on by removing that
    config, by adding //build/config/compiler:exceptions (:rtti), or by
    adding a config of its own whose cflags say -fexceptions (-frtti)."""
    configs = [_label(c, t.path) for c in t.get("configs") or []
               if isinstance(c, str)]
    removed = [_label(c, t.path) for c in t.scope.removed.get("configs", [])
               if isinstance(c, str)]
    defaults = interp.target_defaults.get(t.func, {}).get("configs")
    defaults = [_label(c, "//build/config") for c in defaults
                if isinstance(c, str)] if isinstance(defaults, list) else []
    own = [c for c in configs if c not in defaults]
    direct = [c for key in ("cflags", "cflags_cc")
              for c in (t.get(key) if isinstance(t.get(key), list) else [])]
    out = {}
    for key, off, on, flag in (("exceptions", NO_EXCEPTIONS, EXCEPTIONS,
                                "-fexceptions"),
                               ("rtti", NO_RTTI, RTTI, "-frtti")):
        val = off in removed or on in configs or flag in direct or (
            off in defaults and off not in configs)
        if not val:
            for c in own:
                if flag in _config_cflags(interp, tree, c, cache):
                    val = True
                    break
        out[key] = val
    return out


def _sources(t, interp):
    srcs = t.get("sources")
    if not isinstance(srcs, list):
        return set()
    root = (interp.root or "").rstrip("/") + "/"
    out = set()
    for s in srcs:
        if not isinstance(s, str):
            continue
        if interp.root and s.startswith(root):
            # rebase_path(x, "") (Blink's build.gni source lists).
            s = "//" + s[len(root):]
        out.add(interp.abspath(s, {"dir": t.path}))
    return out


def owning_target(tree, interp, path, cache):
    """The GN target (gn.Target) whose `sources` list `path` (src-relative),
    from the nearest BUILD.gn up, then any other BUILD.gn naming the file
    (content/test/BUILD.gn lists ../browser/... test files); None if no
    BUILD.gn does."""
    want = "//" + path
    parts = path.split("/")[:-1]
    ancestors = ["//" + "/".join(parts[:i]) + "/BUILD.gn"
                 for i in range(len(parts), 0, -1)]

    def others():
        from .gen_actions import _build_files_mentioning
        return ["//" + p for p in _build_files_mentioning(
            tree, path.rsplit("/", 1)[-1], limit=20)
            if "//" + p not in ancestors]

    for group in (lambda: ancestors, others):
        for bf in group():
            if not tree.exists(bf[2:]):
                continue
            if bf not in cache:
                try:
                    cache[bf] = interp.run_file(bf)
                except Exception as e:  # noqa: BLE001 - report, keep going
                    log("gnconfigs: %s: %s" % (bf, e))
                    cache[bf] = []
            for t in cache[bf]:
                if t.func != "config" and want in _sources(t, interp):
                    return t
    return None


# Built-in target types (what a template expands to).
BUILTIN_TARGETS = ("source_set", "static_library", "shared_library",
                   "executable", "loadable_module", "component",
                   "jumbo_source_set", "jumbo_static_library")

# Configs whose flags the check sets itself (config.py) or that only
# matter past the front end: a target's own configs other than these are
# passed on.
SKIP_OWN_CONFIGS = (
    "//build/config/compiler:", "//build/config/clang:",
    "//build/config/sanitizers:", "//build/config/coverage:",
    "//build/config/c++:", "//build/config/win:", "//build/config/mac:",
    "//build/config/linux:", "//build/config/posix:",
    "//build/config/gcc:", "//build/config:",
)


def expanded_target(tree, interp, t, cache):
    """For a template invocation (v8_source_set("x"), ...), the built-in
    target its expansion defines with the same sources; else t."""
    if t.func in BUILTIN_TARGETS:
        return t
    bf = t.path + "/BUILD.gn"
    key = ("expanded", bf)
    if key not in cache:
        try:
            cache[key] = interp.run_file(bf, expand=True)
        except Exception as e:  # noqa: BLE001 - report, keep going
            log("gnconfigs: %s (expanded): %s" % (bf, e))
            cache[key] = []
    want = _sources(t, interp)
    for x in cache[key]:
        if x.func in BUILTIN_TARGETS and x.path == t.path and \
                want and want <= _sources(x, interp):
            return x
    return t


def own_flags(t, interp, tree, cache, lang=None):
    """The flags a target's own settings give its files: its defines,
    include_dirs and cflags, and those of the configs it adds beyond the
    defaults (SKIP_OWN_CONFIGS excepted)."""
    out = []
    for d in t.get("defines") or []:
        if isinstance(d, str) and "<UNKNOWN>" not in d:
            out.append("-D" + d)
    for d in t.get("include_dirs") or []:
        if isinstance(d, str) and "<UNKNOWN>" not in d:
            a = interp.abspath(d, {"dir": t.path})
            if a.startswith("//"):
                out.append("-I" + str(tree.tree_dir / a[2:]))
    for key in ["cflags"] + _lang_keys(lang):
        v = t.get(key)
        for c in v if isinstance(v, list) else []:
            if _keep_cflag(c, interp) and c not in (
                    "-fexceptions", "-frtti", "-fno-exceptions",
                    "-fno-rtti", "/EHsc", "/GR", "/GR-"):
                out.append(c)
    defaults = interp.target_defaults.get(t.func, {}).get("configs")
    defaults = [_label(c, "//build/config") for c in defaults
                if isinstance(c, str)] if isinstance(defaults, list) else []
    seen = set()
    for c in t.get("configs") or []:
        if not isinstance(c, str):
            continue
        c = _label(c, t.path)
        if c in defaults or c.startswith(SKIP_OWN_CONFIGS):
            continue
        sub = []
        _flags_of(interp, tree, c, seen, sub, cache, lang)
        out += [f for f in sub if f not in (
            "-fexceptions", "-frtti", "-fno-exceptions", "-fno-rtti",
            "/EHsc", "/GR", "/GR-")]
    return _dedupe(out)


def target_flags(tree, interp, paths):
    """{path: {"gn_target": label or None, "exceptions": bool, "rtti":
    bool, "flags": [...]}} for the files to check: the language flags their
    own GN target compiles them with, and the flags of its own settings
    (own_flags)."""
    from . import platforms
    cache = {}
    out = {}
    for p in paths:
        t = owning_target(tree, interp, p, cache)
        if t is None:
            out[p] = {"gn_target": None, "exceptions": False, "rtti": False,
                      "flags": []}
            continue
        info = {"gn_target": "%s:%s" % (t.path, t.name)}
        info.update(language_flags(t, interp, tree, cache))
        x = expanded_target(tree, interp, t, cache)
        if x is not t:
            more = language_flags(x, interp, tree, cache)
            for k in ("exceptions", "rtti"):
                info[k] = info[k] or more[k]
        info["flags"] = own_flags(x, interp, tree, cache,
                                  platforms.language(p))
        out[p] = info
    return out


def resolve(tree, interp):
    """(global flags, {dir prefix: flags})."""
    cache = {}
    glob = []
    for label in GLOBAL_CONFIGS:
        _flags_of(interp, tree, label, set(), glob, cache)
    per_dir = {}
    for prefix, labels in DIR_CONFIGS.items():
        flags = []
        for label in labels:
            _flags_of(interp, tree, label, set(), flags, cache)
        per_dir[prefix] = flags
    return _dedupe(glob), {k: _dedupe(v) for k, v in per_dir.items()}


def _dedupe(flags):
    out = []
    i = 0
    seen = set()
    while i < len(flags):
        f = flags[i]
        # -Xclang pairs stay together.
        if f == "-Xclang" and i + 1 < len(flags):
            out += [f, flags[i + 1]]
            i += 2
            continue
        if f not in seen:
            seen.add(f)
            out.append(f)
        i += 1
    return out
