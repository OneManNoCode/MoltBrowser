# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Compiler flags from GN config() targets, read with the GN reader.

A real build gives each file the public configs of everything it depends
on. Here a fixed set stands for that: the configs every chrome/browser file
gets through its dependencies (GLOBAL_CONFIGS), and those of the target a
file belongs to, by directory (DIR_CONFIGS: Blink's inside_blink, ...).
"""
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


def _flags_of(interp, tree, label, seen, out, cache):
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
        for c in t.get("cflags") or []:
            if isinstance(c, str) and not c.startswith("/") \
                    and "<UNKNOWN>" not in c:
                out.append(c)
        for sub in t.get("configs") or []:
            if isinstance(sub, str):
                sub = sub.split("(")[0]
                if sub.startswith(":"):
                    sub = t.path + sub
                _flags_of(interp, tree, sub, seen, out, cache)
        return
    log("gnconfigs: no config %s" % label)


NO_EXCEPTIONS = "//build/config/compiler:no_exceptions"
EXCEPTIONS = "//build/config/compiler:exceptions"
NO_RTTI = "//build/config/compiler:no_rtti"
RTTI = "//build/config/compiler:rtti"


def _label(c, base_dir):
    """A config label as written in a target -> "//dir:name"."""
    c = c.split("(")[0]
    if c.startswith(":"):
        return base_dir + c
    if ":" not in c:
        return c + ":" + c.rstrip("/").rsplit("/", 1)[-1]
    return c


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


def target_flags(tree, interp, paths):
    """{path: {"gn_target": label or None, "exceptions": bool, "rtti":
    bool}} for the files to check (the language flags their own GN target
    compiles them with)."""
    cache = {}
    out = {}
    for p in paths:
        t = owning_target(tree, interp, p, cache)
        if t is None:
            out[p] = {"gn_target": None, "exceptions": False, "rtti": False}
            continue
        info = {"gn_target": "%s:%s" % (t.path, t.name)}
        info.update(language_flags(t, interp, tree, cache))
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
