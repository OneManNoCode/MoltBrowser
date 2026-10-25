#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Generates the C++ tables of MoltNet's bundled font set from the manifest.

tools/fonts/moltnet_fonts.json is the one source of the set. This script
writes it out for the two places that need it at run time:

  * Blink (MoltNet renderers): the families, faces, names, aliases, generic
    families, CJK regions and per-script fallback order, and the visibility
    list (FontPlatform::kMoltNetBundled), in
    src/third_party/blink/renderer/core/molt_fingerprinting/
    moltnet_font_set_table.cc (rules: moltnet_font_set.h);
  * the browser: the files to open and send, with their sizes, in
    src/chrome/browser/molt_ai/fingerprinting/molt_net_fonts_table.h.

It also copies the per-OS allow-lists of font_allowlists.cc into
tools/fonts/font_allowlists.json, which the fresh-install inventory
(molt_font_inventory.py) compares installed fonts with.

Both list the files in the same order (the browser sends them in it, Blink
reads them in it); molt_net_fonts_unittest.cc checks the two agree.

Usage:
  python3 tools/fonts/generate_tables.py          # check: exit 1 if stale
  python3 tools/fonts/generate_tables.py --write  # regenerate
Exit status 2: the manifest is invalid or still has TODO(pin) entries.
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import manifest as mf  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
BLINK_TABLE = (REPO_ROOT / "src/third_party/blink/renderer/core/"
               "molt_fingerprinting/moltnet_font_set_table.cc")
BROWSER_TABLE = (REPO_ROOT / "src/chrome/browser/molt_ai/fingerprinting/"
                 "molt_net_fonts_table.h")
ALLOWLISTS_CC = (REPO_ROOT / "src/third_party/blink/renderer/core/"
                 "molt_fingerprinting/font_allowlists.cc")
# The per-OS allow-lists for tools/fonts/molt_font_inventory.py.
ALLOWLISTS_JSON = REPO_ROOT / "tools/fonts/font_allowlists.json"

# ISO 15924 code -> ICU UScriptCode (third_party/icu/source/common/unicode/
# uscript.h; moltnet_fonts.cc static_asserts the ones Blink's glue uses).
ICU_SCRIPTS = {
    "Zyyy": 0, "Zinh": 1, "Arab": 2, "Armn": 3, "Beng": 4, "Bopo": 5,
    "Cher": 6, "Cyrl": 8, "Deva": 10, "Ethi": 11, "Geor": 12, "Grek": 14,
    "Gujr": 15, "Guru": 16, "Hani": 17, "Hang": 18, "Hebr": 19, "Hira": 20,
    "Knda": 21, "Kana": 22, "Khmr": 23, "Laoo": 24, "Latn": 25, "Mlym": 26,
    "Mong": 27, "Mymr": 28, "Orya": 31, "Sinh": 33, "Syrc": 34, "Taml": 35,
    "Telu": 36, "Thaa": 37, "Thai": 38, "Tibt": 39, "Cans": 40, "Yiii": 41,
    "Brai": 46, "Hrkt": 54, "Tfng": 60, "Hans": 73, "Hant": 74, "Java": 78,
    "Nkoo": 87, "Jpan": 105, "Olck": 109, "Kore": 119, "Adlm": 167,
}
GENERIC_ORDER = ["standard", "serif", "sans-serif", "monospace", "cursive",
                 "fantasy", "math", "system-ui"]
REGION_ORDER = ["JP", "KR", "SC", "TC", "HK"]
FALLBACK_EMOJI = 0xffff
FALLBACK_TEXT_EMOJI = 0xfffe
FALLBACK_CJK = 0xff
NO_FAMILY = 0xfe

FNV_OFFSET = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3


class GenerateError(Exception):
    pass


def font_name_hash(name):
    """fonts.cc's FontName::Hash(): 64-bit FNV-1a over the normalized code
    points."""
    h = FNV_OFFSET
    for ch in mf.normalize(name):
        h ^= ord(ch)
        h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def cpp_string(s):
    """A C++ string literal for `s` (UTF-8; non-ASCII as \\u escapes)."""
    out = []
    for ch in s:
        o = ord(ch)
        if ch in '"\\':
            out.append("\\" + ch)
        elif 0x20 <= o < 0x7f:
            out.append(ch)
        elif o <= 0xffff:
            out.append("\\u%04X" % o)
        else:
            out.append("\\U%08X" % o)
    return '"' + "".join(out) + '"'


def model(data):
    """The set as the tables lay it out: families in order of first
    appearance, each family's faces together."""
    if any(mf.is_pending(e) for _, e in mf.all_entries(data)):
        raise GenerateError("the manifest has TODO(pin) entries; run "
                            "tools/fonts/pin_fonts.py --write first")
    family_order = []
    faces_by_family = {}
    for file_index, entry in enumerate(data["files"]):
        for face in entry["faces"]:
            fam = face["family"]
            if fam not in faces_by_family:
                family_order.append(fam)
                faces_by_family[fam] = []
            faces_by_family[fam].append((file_index, face))
    if len(data["files"]) > 250 or len(family_order) > 250:
        raise GenerateError("too many files or families for uint8_t indices")
    family_index = {f: i for i, f in enumerate(family_order)}
    faces = []
    families = []
    for fam in family_order:
        first = len(faces)
        for file_index, face in faces_by_family[fam]:
            lo, default, hi = face["weight"]
            faces.append({
                "file": file_index, "index": face["index"],
                "family": family_index[fam], "italic": face["italic"],
                "variable": lo != hi, "weights": (lo, default, hi),
                "postscript": face["postscript_name"],
                "full": face["full_name"], "style": face["style"],
            })
        families.append({"name": fam, "first": first,
                         "count": len(faces) - first})
    if len(faces) > 250:
        raise GenerateError("too many faces for uint8_t indices")

    # Other names: the families' other names, then the aliases.
    names = []
    taken = {mf.normalize(f): f for f in family_order}
    for fam in family_order:
        for _, face in faces_by_family[fam]:
            for other in face["family_names"]:
                key = mf.normalize(other)
                if key in taken:
                    if taken[key] != fam and key != mf.normalize(fam):
                        raise GenerateError(
                            "%r names both %r and %r" % (other, taken[key],
                                                         fam))
                    continue
                taken[key] = fam
                names.append({"name": other, "family": family_index[fam],
                              "alias": False})
    for alias, target in data["aliases"].items():
        key = mf.normalize(alias)
        if key in taken:
            raise GenerateError("alias %r is already a name of %r" %
                                (alias, taken[key]))
        taken[key] = target
        names.append({"name": alias, "family": family_index[target],
                      "alias": True})

    generic = [family_index[data["generic"][g]] for g in GENERIC_ORDER]
    cjk = data["cjk"]
    regions = [family_index[cjk["regions"][r]] for r in REGION_ORDER]
    cjk_scripts = []
    for script, region in sorted(cjk["scripts"].items(),
                                 key=lambda kv: ICU_SCRIPTS.get(kv[0], 0)):
        if script not in ICU_SCRIPTS:
            raise GenerateError("unknown script %r (add it to ICU_SCRIPTS)" %
                                script)
        cjk_scripts.append({
            "script": ICU_SCRIPTS[script],
            "region": 0 if region == "locale" else REGION_ORDER.index(region),
            "by_locale": region == "locale", "iso": script})
    mono = cjk.get("monospace")
    cjk_monospace = family_index[mono] if mono else NO_FAMILY

    fallback = []
    flat = []
    for key, chain in data["fallback"].items():
        if key == "emoji":
            code = FALLBACK_EMOJI
        elif key == "text-emoji":
            code = FALLBACK_TEXT_EMOJI
        elif key in ICU_SCRIPTS:
            code = ICU_SCRIPTS[key]
        else:
            raise GenerateError("unknown script %r (add it to ICU_SCRIPTS)" %
                                key)
        first = len(flat)
        for fam in chain:
            flat.append(FALLBACK_CJK if fam == mf.CJK_PLACEHOLDER
                        else family_index[fam])
        fallback.append({"key": code, "first": first, "count": len(chain),
                         "iso": key})
    fallback.sort(key=lambda e: e["key"])

    visibility = []
    for fam in family_order:
        visibility.append((fam, ""))
    for n in names:
        visibility.append((n["name"], family_order[n["family"]]
                           if n["alias"] else ""))
    return {
        "version": data["version"],
        "files": [{"name": e["install_name"], "size": e["size"],
                   "sha256": e["sha256"]} for e in data["files"]],
        "faces": faces, "families": families, "names": names,
        "generic": generic, "regions": regions,
        "default_region": REGION_ORDER.index(cjk["default"]),
        "cjk_monospace": cjk_monospace, "cjk_scripts": cjk_scripts,
        "fallback": fallback, "fallback_families": flat,
        "visibility": visibility,
    }


HEADER = """\
// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// GENERATED by tools/fonts/generate_tables.py from
// tools/fonts/moltnet_fonts.json. Do not edit: change the manifest and run
//   python3 tools/fonts/generate_tables.py --write
"""


def blink_table(m):
    out = [HEADER, """
// MoltNet's bundled font set (version %s): see moltnet_font_set.h.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_font_set.h"

namespace blink::molt_fingerprinting::moltnet {

namespace {

constexpr BundledFontFile kFiles[] = {
""" % m["version"]]
    for f in m["files"]:
        out.append("    {%s, %du},\n" % (cpp_string(f["name"]), f["size"]))
    out.append("};\n\nconstexpr BundledFontFace kFaces[] = {\n")
    for face in m["faces"]:
        lo, default, hi = face["weights"]
        out.append(
            "    {%d, %d, %d, %s, %s, %d, %d, %d,\n     %s, %s, %s},\n" %
            (face["file"], face["index"], face["family"],
             "true" if face["italic"] else "false",
             "true" if face["variable"] else "false", lo, default, hi,
             cpp_string(face["postscript"]), cpp_string(face["full"]),
             cpp_string(face["style"])))
    out.append("};\n\nconstexpr BundledFontFamily kFamilies[] = {\n")
    for i, fam in enumerate(m["families"]):
        out.append("    {%s, 0x%016xull, %d, %d},  // %d\n" %
                   (cpp_string(fam["name"]), font_name_hash(fam["name"]),
                    fam["first"], fam["count"], i))
    out.append("};\n\nconstexpr BundledFontName kNames[] = {\n")
    for n in m["names"]:
        out.append("    {%s, 0x%016xull, %d, %s},\n" %
                   (cpp_string(n["name"]), font_name_hash(n["name"]),
                    n["family"], "true" if n["alias"] else "false"))
    out.append("};\n\nconstexpr CjkScript kCjkScripts[] = {\n")
    for c in m["cjk_scripts"]:
        out.append("    {%d, %d, %s},  // %s\n" %
                   (c["script"], c["region"],
                    "true" if c["by_locale"] else "false", c["iso"]))
    out.append("};\n\nconstexpr ScriptFallback kFallback[] = {\n")
    for e in m["fallback"]:
        out.append("    {0x%04x, %d, %d},  // %s\n" %
                   (e["key"], e["first"], e["count"], e["iso"]))
    out.append("};\n\nconstexpr uint8_t kFallbackFamilies[] = {\n")
    flat = m["fallback_families"]
    for i in range(0, len(flat), 12):
        out.append("    " + ", ".join(
            "0x%02x" % v for v in flat[i:i + 12]) + ",\n")
    out.append("};\n\nconstexpr FontListEntry kVisibility[] = {\n")
    for name, alias in m["visibility"]:
        if alias:
            out.append("    {%s, %s},\n" % (cpp_string(name),
                                            cpp_string(alias)))
        else:
            out.append("    {%s},\n" % cpp_string(name))
    out.append("};\n\n")
    out.append("""constexpr BundledFontTable kTable = {
    %s,
    kFiles,
    kFaces,
    kFamilies,
    kNames,
    {%s},
    {%s},
    %d,
    %s,
    kCjkScripts,
    kFallback,
    kFallbackFamilies,
    kVisibility,
};

}  // namespace

const BundledFontTable& GetBundledFontTable() {
  return kTable;
}

}  // namespace blink::molt_fingerprinting::moltnet
""" % (cpp_string(m["version"]),
       ", ".join(str(g) for g in m["generic"]),
       ", ".join(str(r) for r in m["regions"]),
       m["default_region"],
       "0x%02x" % m["cjk_monospace"]))
    return "".join(out)


def browser_table(m):
    total = sum(f["size"] for f in m["files"])
    out = [HEADER, """
// MoltNet's bundled font set (version %s), browser side: the files
// molt_net_fonts.h opens and sends to MoltNet renderers, in this order (the
// order of Blink's table, moltnet_font_set_table.cc). %d files, %d bytes.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_FONTS_TABLE_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_FONTS_TABLE_H_

#include <cstdint>
#include <string_view>

namespace molt_ai::fingerprinting {

struct MoltNetFontFileSpec {
  std::string_view name;
  uint64_t size;
  std::string_view sha256;
};

inline constexpr std::string_view kMoltNetFontSetVersion = %s;

inline constexpr MoltNetFontFileSpec kMoltNetFontFiles[] = {
""" % (m["version"], len(m["files"]), total, cpp_string(m["version"]))]
    for f in m["files"]:
        out.append("    {%s, %du,\n     %s},\n" %
                   (cpp_string(f["name"]), f["size"], cpp_string(f["sha256"])))
    out.append("""};

inline constexpr uint64_t kMoltNetFontSetBytes = %du;

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_FONTS_TABLE_H_
""" % total)
    return "".join(out)


def _cpp_unescape(s):
    """The text of a C++ string literal body (\\uXXXX, \\UXXXXXXXX, \\", \\\\
    escapes)."""
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "u":
                out.append(chr(int(s[i + 2:i + 6], 16)))
                i += 6
                continue
            if n == "U":
                out.append(chr(int(s[i + 2:i + 10], 16)))
                i += 10
                continue
            out.append(n)
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def allowlists_json():
    """font_allowlists.cc's per-OS lists as JSON, for the fresh-install
    inventory (molt_font_inventory.py), which runs without the repository."""
    import json
    import re
    src = ALLOWLISTS_CC.read_text(encoding="utf-8")
    lists = {}
    keys = {"Windows": "windows", "Mac": "mac", "Linux": "linux",
            "Android": "android"}
    for platform, body in re.findall(
            r"constexpr FontListEntry k(\w+)Fonts\[\] = \{(.*?)\n\};", src,
            re.S):
        if platform not in keys:
            continue
        entries = []
        for name, alias in re.findall(
                r'\{"((?:[^"\\]|\\.)*)"(?:,\s*"((?:[^"\\]|\\.)*)")?\}', body):
            entry = {"name": _cpp_unescape(name)}
            if alias:
                entry["alias_of"] = _cpp_unescape(alias)
            entries.append(entry)
        lists[keys[platform]] = entries
    if set(lists) != set(keys.values()):
        raise GenerateError("font_allowlists.cc: expected the lists of %s" %
                            ", ".join(sorted(keys.values())))
    # One entry per line: readable, and diffs show the changed names.
    out = ['{', ' "comment": "GENERATED by tools/fonts/generate_tables.py '
           'from src/third_party/blink/renderer/core/molt_fingerprinting/'
           'font_allowlists.cc. Do not edit.",', ' "lists": {']
    keys_sorted = sorted(lists)
    for i, key in enumerate(keys_sorted):
        out.append('  %s: [' % json.dumps(key))
        rows = [json.dumps(e, ensure_ascii=False) for e in lists[key]]
        out.append(",\n".join("   " + r for r in rows))
        out.append('  ]%s' % ("," if i + 1 < len(keys_sorted) else ""))
    out += [' }', '}', '']
    return "\n".join(out)


def outputs(data):
    m = model(data)
    return {BLINK_TABLE: blink_table(m), BROWSER_TABLE: browser_table(m),
            ALLOWLISTS_JSON: allowlists_json()}


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--manifest", default=str(mf.MANIFEST))
    args = parser.parse_args(argv)
    try:
        data = mf.load(args.manifest)
        files = outputs(data)
    except (mf.ManifestError, GenerateError, KeyError) as e:
        print("generate_tables: %s" % e, file=sys.stderr)
        return 2
    stale = []
    for path, text in files.items():
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current == text:
            continue
        if args.write:
            path.write_text(text, encoding="utf-8")
            print("wrote %s" % path.relative_to(REPO_ROOT))
        else:
            stale.append(path)
    for path in stale:
        print("generate_tables: %s is out of date (run with --write)" %
              path.relative_to(REPO_ROOT), file=sys.stderr)
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
