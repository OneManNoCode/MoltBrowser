#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Checks MoltBrowser's per-OS font allow-lists against a fresh OS install.

Outside MoltNet, MoltBrowser lets protected pages name only the font families
an OS installs by default (src/third_party/blink/renderer/core/
molt_fingerprinting/font_allowlists.cc; docs/FINGERPRINTING.md, "Fonts").
The lists were written from the vendors' documentation; this tool checks
them against real installs. Run it on a freshly installed VM of each
supported OS version (Windows 10 22H2 and 11, macOS 13, 14 and 15, the
common Linux distributions) before installing anything else, and send the
two files it writes to the maintainers:

  python3 molt_font_inventory.py         collect and compare; writes
    molt-fonts-<os>-<version>.json         the comparison plus every font
                                           file: its faces, names, versions
                                           and SHA-256
    molt-fonts-<os>-<version>.md           the comparison, readable

Standard library only (Python 3.8+). Copy the whole tools/fonts/ folder to
the VM: the tool needs sfnt.py, manifest.py and font_allowlists.json next to
it. It installs and changes nothing, and its files hold no user or host
name (the home folder is written as "~").

A fresh Windows or macOS has no Python. There, run the collector that uses
what the OS ships, copy its output to any machine with Python and import it:

  Windows:  powershell -NoProfile -ExecutionPolicy Bypass -File collect_windows.ps1
  macOS:    /usr/bin/perl collect_fonts.pl          (works on Linux too)
  then:     python3 molt_font_inventory.py import molt-fonts-raw.json

which writes the same two files. Other commands:

  collect --out FILE            the inventory alone
  compare INVENTORY.json        compare an inventory (or a report's)
  merge REPORT.json...          several installs of one OS: listed families
                                any of them lacks, unlisted families all of
                                them have, listed families whose files
                                differ (Markdown; --out FILE)

Exit status: 0 done (missing families are findings, not failures); 2 bad
usage or unreadable input.
"""

import argparse
import base64
import datetime
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sfnt  # noqa: E402
from manifest import normalize  # noqa: E402

TOOL_VERSION = "1"
INVENTORY_FORMAT = "molt-font-inventory-1"
RAW_FORMAT = "molt-font-raw-1"
REPORT_FORMAT = "molt-font-report-1"
ALLOWLISTS = HERE / "font_allowlists.json"
COMMANDS = ("run", "collect", "import", "compare", "merge")

OPENTYPE_EXTENSIONS = (".ttf", ".otf", ".ttc", ".otc")
OTHER_FONT_EXTENSIONS = (".dfont", ".fon", ".fnt", ".pfb", ".pfm", ".pfa",
                         ".woff", ".woff2", ".pcf", ".pcf.gz", ".bdf", ".t1")
# A face found in one of these counts as installed by default. "local" is
# macOS's /Library/Fonts (for every user; a fresh macOS puts a few there).
DEFAULT_LOCATIONS = ("system", "local")
LOCATIONS = DEFAULT_LOCATIONS + ("on-demand", "user")
# The tables the raw collectors send per face; all read_face() needs.
RAW_TABLES = ("name", "OS/2", "head", "fvar")
MAX_FONT_FILE = 256 << 20
MAX_FILES = 20000
# "Arial Bold (TrueType)", "MS Sans Serif 8,10,12,14,18,24",
# "Cambria & Cambria Math (TrueType)": Windows' registry names of a file.
_REGISTRY_SUFFIX = re.compile(r"\s*(\([^()]*\))?\s*$")
_REGISTRY_SIZES = re.compile(r"\s+\d+(,\d+)*\s*$")


class InventoryError(Exception):
    pass


# --- Collecting --------------------------------------------------------------

def _redact(path, home=None):
    home = str(Path.home()) if home is None else home
    p = str(path)
    if home and len(home) > 1:
        for sep in ("/", "\\"):
            if p == home or p.startswith(home.rstrip(sep) + sep):
                return "~" + p[len(home.rstrip(sep)):]
    return p


def detect_platform():
    system = platform.system()
    if system == "Windows":
        return "windows"
    if system == "Darwin":
        return "mac"
    if system == "Linux":
        return "linux"
    raise InventoryError("unsupported system %r" % system)


def _run(argv, stderr_too=False):
    """The output of a system tool given by absolute path, or ""."""
    if not os.path.isabs(argv[0]) or not os.path.exists(argv[0]):
        return ""
    try:
        return subprocess.run(argv, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT if stderr_too else
                              subprocess.DEVNULL, timeout=600,
                              check=False).stdout.decode("utf-8", "replace")
    except (OSError, subprocess.SubprocessError):
        return ""


def _windows_version_info():
    info = {}
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r"SOFTWARE\Microsoft\Windows NT\CurrentVersion") as k:
            for name in ("ProductName", "DisplayVersion", "ReleaseId",
                         "CurrentBuild", "UBR", "EditionID",
                         "InstallationType"):
                try:
                    info[name] = str(winreg.QueryValueEx(k, name)[0])
                except OSError:
                    pass
    except (ImportError, OSError):
        pass
    return info


def windows_label(info):
    build = info.get("CurrentBuild", "")
    # Windows 11 still calls itself "Windows 10" in ProductName.
    major = "11" if build.isdigit() and int(build) >= 22000 else "10"
    return "windows%s-%s" % (major, info.get("DisplayVersion") or
                             info.get("ReleaseId") or build or "unknown")


def os_info(plat):
    info = {"platform": plat, "machine": platform.machine()}
    if plat == "linux":
        try:
            for line in Path("/etc/os-release").read_text().splitlines():
                key, _, value = line.partition("=")
                if key in ("ID", "VERSION_ID", "PRETTY_NAME"):
                    info[key.lower()] = value.strip().strip('"')
        except OSError:
            pass
        info["kernel"] = platform.release()
        info["desktop"] = os.environ.get("XDG_CURRENT_DESKTOP", "")
        info["fontconfig"] = _run(["/usr/bin/fc-list", "--version"],
                                  stderr_too=True).strip()
        info["label"] = "%s-%s" % (info.get("id", "linux"),
                                   info.get("version_id", "unknown"))
    elif plat == "mac":
        info["version"] = platform.mac_ver()[0]
        info["build"] = _run(["/usr/bin/sw_vers", "-buildVersion"]).strip()
        info["label"] = "macos-%s" % info["version"]
    else:
        info.update(_windows_version_info())
        info["label"] = windows_label(info)
    return info


def _is_font_name(name):
    return name.lower().endswith(OPENTYPE_EXTENSIONS + OTHER_FONT_EXTENSIONS)


def _walk(root, location, out):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            if _is_font_name(name):
                out.setdefault(os.path.join(dirpath, name), location)
            if len(out) > MAX_FILES:
                raise InventoryError("more than %d font files" % MAX_FILES)


def default_roots(plat, home=None):
    """[(directory, location)] where `plat` keeps fonts."""
    home = str(Path.home()) if home is None else home
    if plat == "linux":
        return [("/usr/share/fonts", "system"),
                ("/usr/local/share/fonts", "local"),
                (os.path.join(home, ".local/share/fonts"), "user"),
                (os.path.join(home, ".fonts"), "user")]
    if plat == "mac":
        roots = [("/System/Library/Fonts", "system"),
                 ("/Library/Fonts", "local"),
                 (os.path.join(home, "Library/Fonts"), "user")]
        # Fonts macOS downloads on demand (Font Book, or an app asking).
        assets = "/System/Library/AssetsV2"
        if os.path.isdir(assets):
            roots += [(os.path.join(assets, e), "on-demand")
                      for e in sorted(os.listdir(assets))
                      if e.startswith("com_apple_MobileAsset_Font")]
        return roots
    windir = os.environ.get("WINDIR", r"C:\Windows")
    roots = [(os.path.join(windir, "Fonts"), "system")]
    local = os.environ.get("LOCALAPPDATA", "")
    if local:
        roots.append((os.path.join(local, "Microsoft", "Windows", "Fonts"),
                      "user"))
    return roots


def font_files(plat, roots=None):
    """({path: location}, {path: [Windows registry names]})."""
    found, registry = {}, {}
    home = str(Path.home())
    if plat == "linux" and roots is None:
        # fontconfig may know directories besides the usual ones.
        for line in _run(["/usr/bin/fc-list",
                          "--format=%{file}\n"]).splitlines():
            line = line.strip()
            if line and _is_font_name(line):
                found.setdefault(line, "user" if line.startswith(
                    home + os.sep) else "system")
    for root, location in (roots or default_roots(plat)):
        if os.path.isdir(root):
            _walk(root, location, found)
    if plat == "windows" and roots is None:
        windir = os.environ.get("WINDIR", r"C:\Windows")
        for path, location, name in _windows_registry_fonts(windir):
            found.setdefault(path, location)
            registry.setdefault(path, []).append(name)
    return found, registry


def _windows_registry_fonts(windir):
    try:
        import winreg
    except ImportError:
        return []
    out = []
    for hive, location in ((winreg.HKEY_LOCAL_MACHINE, "system"),
                           (winreg.HKEY_CURRENT_USER, "user")):
        try:
            key = winreg.OpenKey(hive, r"SOFTWARE\Microsoft\Windows NT"
                                       r"\CurrentVersion\Fonts")
        except OSError:
            continue
        with key:
            for i in range(MAX_FILES):
                try:
                    name, value, _ = winreg.EnumValue(key, i)
                except OSError:
                    break
                value = str(value)
                path = value if os.path.isabs(value) else os.path.join(
                    windir, "Fonts", value)
                if os.path.isfile(path):
                    out.append((path, location, str(name)))
    return out


def registry_family_names(name):
    """The family names in a Windows registry font value name."""
    name = _REGISTRY_SUFFIX.sub("", name)
    name = _REGISTRY_SIZES.sub("", name)
    return [n.strip() for n in name.split(" & ") if n.strip()]


def describe_face(face):
    """The inventory's record of a face (one of sfnt.read_faces())."""
    names = list(face.get("family_names") or [])
    if face.get("skia_family") and face["skia_family"] not in names:
        names.append(face["skia_family"])
    return {
        "index": face["index"],
        "family": face.get("family"),
        "typographic_family": face.get("typographic_family"),
        "family_names": names,
        "legacy_names": sfnt.legacy_family_names(face),
        "postscript_name": face.get("postscript_name"),
        "full_name": face.get("full_name"),
        "subfamily": face.get("typographic_subfamily") or face.get("subfamily"),
        "weight": face.get("weight"),
        "italic": face.get("italic"),
        "version": face.get("version"),
        "font_revision": face.get("font_revision"),
        "axes": [a["tag"] for a in face.get("axes", ())],
    }


def describe_file(path, location, hash_files=True, home=None):
    entry = {"path": _redact(path, home), "location": location}
    try:
        size = os.path.getsize(path)
        entry["size"] = size
        if size > MAX_FONT_FILE:
            entry["error"] = "larger than %d bytes" % MAX_FONT_FILE
            return entry
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        entry["error"] = "unreadable: %s" % (e.strerror or e)
        return entry
    if hash_files:
        entry["sha256"] = hashlib.sha256(data).hexdigest()
    if not path.lower().endswith(OPENTYPE_EXTENSIONS):
        entry["error"] = "not an OpenType font (not compared)"
        return entry
    try:
        entry["faces"] = [describe_face(f) for f in sfnt.read_faces(data)]
    except sfnt.SfntError as e:
        entry["error"] = "unreadable font: %s" % e
    return entry


def collect(hash_files=True, plat=None, roots=None):
    plat = plat or detect_platform()
    files, registry = font_files(plat, roots)
    fonts = []
    for path in sorted(files):
        entry = describe_file(path, files[path], hash_files)
        if path in registry:
            entry["registry_names"] = registry[path]
        fonts.append(entry)
    return {
        "format": INVENTORY_FORMAT,
        "tool": "molt_font_inventory.py %s" % TOOL_VERSION,
        "collected": datetime.datetime.now(datetime.timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ"),
        "os": os_info(plat),
        "fonts": fonts,
    }


# --- Raw collections (collect_fonts.pl, collect_windows.ps1) -----------------

def _text(value, limit=4096):
    return str(value)[:limit]


def import_raw(raw):
    """An inventory made from a no-install collector's output. The file came
    off a VM, so it is checked like any other input: bad entries become
    errors in the inventory, never exceptions."""
    if not isinstance(raw, dict) or raw.get("format") != RAW_FORMAT:
        raise InventoryError("not a %s file" % RAW_FORMAT)
    os_section = raw.get("os")
    if not isinstance(os_section, dict) or os_section.get("platform") not in (
            "windows", "mac", "linux"):
        raise InventoryError("raw file without a known os.platform")
    os_section = {_text(k, 64): _text(v, 256) for k, v in os_section.items()
                  if isinstance(v, (str, int, float))}
    if os_section["platform"] == "windows" and "label" not in os_section:
        os_section["label"] = windows_label(os_section)
    os_section.setdefault("label", os_section["platform"])
    entries = raw.get("fonts")
    if not isinstance(entries, list) or len(entries) > MAX_FILES:
        raise InventoryError("raw file without a fonts list")
    fonts = []
    for item in entries:
        if not isinstance(item, dict):
            continue
        location = _text(item.get("location", "system"), 32)
        entry = {"path": _redact(_text(item.get("path", "?"))),
                 "location": location if location in LOCATIONS else "system"}
        if isinstance(item.get("size"), int):
            entry["size"] = item["size"]
        if isinstance(item.get("sha256"), str) and re.fullmatch(
                r"[0-9a-f]{64}", item["sha256"]):
            entry["sha256"] = item["sha256"]
        if isinstance(item.get("registry_names"), list):
            entry["registry_names"] = [_text(n, 256)
                                       for n in item["registry_names"][:64]]
        if "error" in item:
            entry["error"] = _text(item["error"], 256)
        faces = item.get("faces")
        if "error" not in entry and isinstance(faces, list):
            try:
                entry["faces"] = [_raw_face(face, i)
                                  for i, face in enumerate(
                                      faces[:sfnt.MAX_COLLECTION_FACES])]
            except (sfnt.SfntError, ValueError, TypeError, KeyError,
                    AttributeError) as e:
                entry["error"] = "unreadable font: %s" % e
        elif "error" not in entry:
            entry["error"] = "not an OpenType font (not compared)"
        fonts.append(entry)
    return {
        "format": INVENTORY_FORMAT,
        "tool": "molt_font_inventory.py %s from %s" % (
            TOOL_VERSION, _text(raw.get("collector", "a raw collector"), 64)),
        "collected": _text(raw.get("collected", ""), 64),
        "os": os_section,
        "fonts": fonts,
    }


def _raw_face(face, index):
    tag = bytes.fromhex(face["tag"])
    tables = {}
    for name, blob in face["tables"].items():
        if name in RAW_TABLES:
            tables[name] = base64.b64decode(blob, validate=True)
    record = sfnt.read_face(sfnt.assemble(tag, tables), 0, index)
    return describe_face(record)


# --- Comparing ---------------------------------------------------------------

def load_allowlists(path=ALLOWLISTS):
    try:
        lists = json.loads(Path(path).read_text(encoding="utf-8"))["lists"]
    except (OSError, ValueError, KeyError, TypeError) as e:
        raise InventoryError("can't read %s: %s" % (path, e))
    return lists


def _face_names(face):
    names = list(face.get("family_names") or [])
    names += face.get("legacy_names") or []
    for key in ("postscript_name", "full_name"):
        if face.get(key):
            names.append(face[key])
    return names


def _file_name(path):
    return path.replace("\\", "/").rsplit("/", 1)[-1]


def compare(inventory, allowlists):
    if not isinstance(inventory, dict) or \
            inventory.get("format") != INVENTORY_FORMAT:
        raise InventoryError("not a %s file" % INVENTORY_FORMAT)
    plat = inventory["os"]["platform"]
    listed = allowlists.get(plat)
    if listed is None:
        raise InventoryError("no allow-list for %r" % plat)
    listed_norm = {normalize(i["name"]) for i in listed}
    where = {}       # normalized name -> {location: [(entry, face or None)]}
    unlisted = {}    # family -> [entry]
    unreadable = []

    def add(name, entry, face):
        where.setdefault(normalize(name), {}).setdefault(
            entry["location"], []).append((entry, face))

    for entry in inventory["fonts"]:
        # Windows' registry names count for any file, also the bitmap .fon
        # fonts sfnt.py doesn't read (Chromium can't use those).
        for reg in entry.get("registry_names", ()):
            for name in registry_family_names(reg):
                add(name, entry, None)
        if "error" in entry:
            unreadable.append({"path": entry["path"], "error": entry["error"]})
            continue
        for face in entry.get("faces", ()):
            names = _face_names(face)
            for name in names:
                add(name, entry, face)
            if entry["location"] in DEFAULT_LOCATIONS and not any(
                    normalize(n) in listed_norm for n in names):
                primary = (face.get("typographic_family") or
                           face.get("family") or _file_name(entry["path"]))
                unlisted.setdefault(primary, []).append(entry)

    present, missing, on_demand_only, user_only = [], [], [], []
    versions = {}
    for item in listed:
        if item.get("alias_of"):
            continue
        name = item["name"]
        locs = where.get(normalize(name), {})
        default = [x for loc in DEFAULT_LOCATIONS for x in locs.get(loc, ())]
        if default:
            present.append(name)
            seen = {}
            for e, f in default:
                v = {"file": _file_name(e["path"]),
                     "version": (f or {}).get("version"),
                     "sha256": e.get("sha256")}
                seen[json.dumps(v, sort_keys=True)] = v
            versions[name] = [seen[k] for k in sorted(seen)]
        elif locs.get("on-demand"):
            on_demand_only.append(name)
        elif locs.get("user"):
            user_only.append(name)
        else:
            missing.append(name)
    user_fonts = sorted({e["path"] for e in inventory["fonts"]
                         if e["location"] == "user"})
    warnings = []
    if user_fonts:
        warnings.append("%d font file(s) are installed for the user: not a "
                        "fresh install, or something added fonts" %
                        len(user_fonts))
    return {
        "format": REPORT_FORMAT,
        "tool": "molt_font_inventory.py %s" % TOOL_VERSION,
        "os": inventory["os"],
        "collected": inventory.get("collected"),
        "allowlist_entries": len(listed),
        "aliases": [i["name"] for i in listed if i.get("alias_of")],
        "listed_present": present,
        "listed_missing": missing,
        "listed_on_demand_only": on_demand_only,
        "listed_user_only": user_only,
        "listed_versions": versions,
        "unlisted_families": [
            {"family": family,
             "files": sorted({_file_name(e["path"]) for e in entries})}
            for family, entries in sorted(unlisted.items())],
        "unreadable_files": unreadable,
        "user_font_files": user_fonts,
        "warnings": warnings,
    }


def _md(text):
    return str(text).replace("|", "\\|").replace("\n", " ")


def report_markdown(report):
    os_section = report["os"]
    lines = ["# Fonts on %s" % _md(os_section.get("label", "?")), ""]
    lines.append("Collected %s by %s. OS: %s." % (
        _md(report.get("collected") or "?"), _md(report["tool"]), ", ".join(
            "%s=%s" % (_md(k), _md(v)) for k, v in sorted(os_section.items())
            if k != "label")))
    lines.append("")
    for w in report["warnings"]:
        lines.append("**Warning:** %s." % w)
    if report["warnings"]:
        lines.append("")
    lines += [
        "| | |", "|---|---|",
        "| Allow-list entries (of them aliases) | %d (%d) |" % (
            report["allowlist_entries"], len(report["aliases"])),
        "| Listed and installed | %d |" % len(report["listed_present"]),
        "| **Listed, not installed** | **%d** |" % len(
            report["listed_missing"]),
        "| Listed, only on demand or for the user | %d |" % (
            len(report["listed_on_demand_only"]) +
            len(report["listed_user_only"])),
        "| Installed, not listed | %d |" % len(report["unlisted_families"]),
        "| Font files not compared | %d |" % len(report["unreadable_files"]),
        ""]
    if report["listed_missing"]:
        lines += ["## Listed, not installed (remove from the list?)", ""]
        lines += ["- %s" % _md(n) for n in report["listed_missing"]] + [""]
    if report["listed_on_demand_only"] or report["listed_user_only"]:
        lines += ["## Listed, not installed by default", ""]
        lines += ["- %s (on demand)" % _md(n)
                  for n in report["listed_on_demand_only"]]
        lines += ["- %s (user)" % _md(n) for n in report["listed_user_only"]]
        lines.append("")
    if report["unlisted_families"]:
        lines += ["## Installed by default, not listed", "",
                  "| Family | Files |", "|---|---|"]
        for u in report["unlisted_families"]:
            lines.append("| %s | %s |" % (_md(u["family"]),
                                          _md(", ".join(u["files"]))))
        lines.append("")
    lines += ["## Versions of the listed families", "",
              "| Family | File | Version | SHA-256 (start) |",
              "|---|---|---|---|"]
    for name in sorted(report["listed_versions"]):
        for v in report["listed_versions"][name]:
            lines.append("| %s | %s | %s | %s |" % (
                _md(name), _md(v["file"]), _md(v.get("version") or ""),
                (v.get("sha256") or "")[:16]))
    lines.append("")
    if report["unreadable_files"]:
        lines += ["## Not compared", ""]
        lines += ["- %s: %s" % (_md(u["path"]), _md(u["error"]))
                  for u in report["unreadable_files"]] + [""]
    return "\n".join(lines)


def merge(reports):
    """Markdown over several reports of one platform."""
    plats = {r["os"]["platform"] for r in reports}
    if len(plats) != 1:
        raise InventoryError("merge reports of one platform at a time (%s)" %
                             ", ".join(sorted(plats)))
    labels = [r["os"].get("label", "?") for r in reports]
    lines = ["# %s: %d installs (%s)" % (plats.pop(), len(reports),
                                          ", ".join(map(_md, labels))), ""]
    missing = {}
    for r, label in zip(reports, labels):
        for name in (r["listed_missing"] + r["listed_on_demand_only"] +
                     r["listed_user_only"]):
            missing.setdefault(name, []).append(label)
    lines += ["## Listed families an install lacks (remove them)", ""]
    lines += ["- %s: not on %s" % (_md(n), ", ".join(map(_md, missing[n])))
              for n in sorted(missing)] or ["(none)"]
    lines.append("")
    everywhere = None
    for r in reports:
        fams = {u["family"] for u in r["unlisted_families"]}
        everywhere = fams if everywhere is None else everywhere & fams
    lines += ["## Unlisted families every install has (candidates)", ""]
    lines += ["- %s" % _md(f) for f in sorted(everywhere or ())] or ["(none)"]
    lines.append("")
    lines += ["## Listed families whose files differ between installs", ""]
    differ = []
    names = set()
    for r in reports:
        names |= set(r["listed_versions"])
    for name in sorted(names):
        seen = {}
        for r, label in zip(reports, labels):
            key = "; ".join(sorted("%s %s %s" % (
                v["file"], v.get("version") or "?",
                (v.get("sha256") or "")[:12])
                for v in r["listed_versions"].get(name, ()))) or "missing"
            seen.setdefault(key, []).append(label)
        if len(seen) > 1:
            differ.append("- %s: %s" % (_md(name), " | ".join(
                "%s on %s" % (_md(k), ", ".join(map(_md, v)))
                for k, v in sorted(seen.items()))))
    lines += differ or ["(none)"]
    lines.append("")
    return "\n".join(lines)


# --- Command line ------------------------------------------------------------

def write_report(report, inventory, out_dir):
    label = re.sub(r"[^A-Za-z0-9._-]+", "_",
                   report["os"].get("label", "unknown"))
    os.makedirs(out_dir, exist_ok=True)
    base = os.path.join(out_dir, "molt-fonts-%s" % label)
    doc = dict(report)
    doc["inventory"] = inventory
    with open(base + ".json", "w", encoding="utf-8") as f:
        f.write(json.dumps(doc, indent=1, ensure_ascii=False) + "\n")
    with open(base + ".md", "w", encoding="utf-8") as f:
        f.write(report_markdown(report) + "\n")
    return base + ".json", base + ".md"


def _read_json(path):
    try:
        # utf-8-sig: Windows PowerShell 5.1 may write a BOM.
        return json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except (OSError, ValueError) as e:
        raise InventoryError("can't read %s: %s" % (path, e))


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or (argv[0] not in COMMANDS and
                    argv[0] not in ("-h", "--help")):
        argv.insert(0, "run")
    parser = argparse.ArgumentParser(
        prog="molt_font_inventory.py", description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    sub = parser.add_subparsers(dest="command")
    run = sub.add_parser("run", help="collect and compare (the default)")
    col = sub.add_parser("collect", help="the inventory alone")
    col.add_argument("--out", required=True)
    for p in (run, col):
        p.add_argument("--no-hash", action="store_true",
                       help="skip the SHA-256 of every file")
    imp = sub.add_parser("import", help="a raw collector's output")
    imp.add_argument("raw")
    cmp_ = sub.add_parser("compare", help="an inventory")
    cmp_.add_argument("inventory")
    for p in (run, imp, cmp_):
        p.add_argument("--out-dir", default=".")
        p.add_argument("--allowlists", default=str(ALLOWLISTS))
    mrg = sub.add_parser("merge", help="several reports of one OS")
    mrg.add_argument("reports", nargs="+")
    mrg.add_argument("--out")
    args = parser.parse_args(argv)
    try:
        if args.command == "collect":
            inventory = collect(not args.no_hash)
            with open(args.out, "w", encoding="utf-8") as f:
                f.write(json.dumps(inventory, indent=1, ensure_ascii=False))
            print("wrote %s (%d font files)" % (args.out,
                                                len(inventory["fonts"])))
            return 0
        if args.command == "merge":
            reports = [_read_json(p) for p in args.reports]
            for r in reports:
                if not isinstance(r, dict) or r.get("format") != REPORT_FORMAT:
                    raise InventoryError("not a %s file" % REPORT_FORMAT)
            text = merge(reports)
            if args.out:
                with open(args.out, "w", encoding="utf-8") as f:
                    f.write(text + "\n")
            else:
                print(text)
            return 0
        allowlists = load_allowlists(args.allowlists)
        if args.command == "run":
            inventory = collect(not args.no_hash)
        elif args.command == "import":
            inventory = import_raw(_read_json(args.raw))
        else:
            inventory = _read_json(args.inventory)
            if isinstance(inventory, dict) and "inventory" in inventory:
                inventory = inventory["inventory"]
        report = compare(inventory, allowlists)
        json_path, md_path = write_report(report, inventory, args.out_dir)
        print("wrote %s and %s: %d listed families not installed, %d "
              "unlisted installed" % (json_path, md_path,
                                      len(report["listed_missing"]),
                                      len(report["unlisted_families"])))
        return 0
    except (InventoryError, OSError, KeyError, TypeError) as e:
        print("molt_font_inventory: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
