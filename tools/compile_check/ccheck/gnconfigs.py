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
