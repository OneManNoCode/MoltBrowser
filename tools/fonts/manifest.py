# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""MoltNet's bundled font set: the manifest (moltnet_fonts.json) and its rules.

The manifest is the single source of the set: which files, from where
(a repository and commit, fetched from raw.githubusercontent.com), pinned by
size and SHA-256 like the Tor and model pins, under which licence; the faces
each file holds; and what MoltNet's generic families, aliases and per-script
fallback map to. tools/fonts/generate_tables.py turns it into the C++
tables (Blink's and the browser's), tools/fonts/fetch_fonts.py installs the
files next to the browser, and tools/sbom/molt_sbom.py lists them.

Standard library only. load() validates everything; a manifest that fails
is never used, so a bad edit (or a tampered file) can't make the fetch
script write outside its folder or fetch from anywhere but the pinned
commits.
"""

import json
import re
import urllib.parse
from pathlib import Path

MANIFEST = Path(__file__).resolve().with_name("moltnet_fonts.json")

FORMAT = 1
PENDING = "TODO(pin)"
RAW_HOST = "raw.githubusercontent.com"
MAX_FILE_SIZE = 64 << 20
MAX_LICENSE_SIZE = 1 << 20
MAX_FILES = 128
MAX_FACES_PER_FILE = 16
# The file the set's own manifest is installed as (fetch_fonts.py).
INSTALLED_MANIFEST = "moltnet_fonts.json"
LICENSE_DIR = "LICENSES"

GENERICS = ("standard", "serif", "sans-serif", "monospace", "cursive",
            "fantasy", "math", "system-ui")
# Han regions MoltNet's CJK families cover (the "cjk" section).
HAN_REGIONS = ("JP", "KR", "SC", "TC", "HK")
# Fallback lists that are not a script: emoji presentation and text
# presentation emoji (symbols).
FALLBACK_SPECIAL = ("emoji", "text-emoji")
CJK_PLACEHOLDER = "@cjk"

_SOURCE_ID = re.compile(r"^[a-z0-9][a-z0-9-]{0,31}$")
_REPO = re.compile(r"^[A-Za-z0-9_.-]{1,64}/[A-Za-z0-9_.-]{1,100}$")
_COMMIT = re.compile(r"^[0-9a-f]{40}$")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_INSTALL_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$")
_PATH_PART = re.compile(r"^[A-Za-z0-9_.,\[\]()+-]{1,200}$")
# ISO 15924 script codes (as ICU spells them in USCRIPT_*: the generator maps
# them to ICU's codes).
_SCRIPT = re.compile(r"^[A-Z][a-z]{3}$")
_NAME = re.compile(r"^[^\x00-\x1f\x7f]{1,128}$")


class ManifestError(ValueError):
    """The manifest breaks a rule; the message says which."""


def _fail(where, message):
    raise ManifestError("%s: %s" % (where, message))


def _expect(cond, where, message):
    if not cond:
        _fail(where, message)


def _int(value, where, lo, hi):
    _expect(isinstance(value, int) and not isinstance(value, bool) and
            lo <= value <= hi, where, "must be an integer in [%d, %d], not %r"
            % (lo, hi, value))
    return value


def _str(value, where, pattern=None):
    _expect(isinstance(value, str), where, "must be a string")
    if pattern is not None:
        _expect(pattern.match(value) is not None, where,
                "%r is not allowed" % value)
    return value


def _check_path(path, where):
    _str(path, where)
    parts = path.split("/")
    _expect(len(parts) <= 12, where, "too deep")
    for part in parts:
        _expect(part not in ("", ".", ".."), where,
                "%r is not a plain relative path" % path)
        _expect(_PATH_PART.match(part) is not None, where,
                "%r has a character that isn't allowed" % path)
    return path


def source_url(source, path):
    """The pinned URL of `path` in `source` (a validated sources entry)."""
    return "https://%s/%s/%s/%s" % (RAW_HOST, source["repo"], source["commit"],
                                    urllib.parse.quote(path, safe="/"))


def is_pending(entry):
    return entry.get("sha256") == PENDING


def _check_pinned(entry, where, max_size):
    sha = entry.get("sha256")
    if sha == PENDING:
        _expect(entry.get("size") == 0, where, "a pending pin has size 0")
        return
    _str(sha, where + ".sha256", _SHA256)
    _int(entry.get("size"), where + ".size", 1, max_size)


def _check_face(face, where):
    _expect(isinstance(face, dict), where, "must be an object")
    _int(face.get("index"), where + ".index", 0, MAX_FACES_PER_FILE - 1)
    for key in ("family", "postscript_name", "full_name", "style"):
        _str(face.get(key), "%s.%s" % (where, key), _NAME)
    names = face.get("family_names")
    _expect(isinstance(names, list) and names, where + ".family_names",
            "must be a non-empty list")
    for i, name in enumerate(names):
        _str(name, "%s.family_names[%d]" % (where, i), _NAME)
    _expect(face["family"] in names, where,
            "family must be one of family_names")
    weights = face.get("weight")
    _expect(isinstance(weights, list) and len(weights) == 3, where + ".weight",
            "must be [min, default, max]")
    lo = _int(weights[0], where + ".weight[0]", 1, 1000)
    default = _int(weights[1], where + ".weight[1]", 1, 1000)
    hi = _int(weights[2], where + ".weight[2]", 1, 1000)
    _expect(lo <= default <= hi, where + ".weight", "needs min <= default <= max")
    _expect(isinstance(face.get("italic"), bool), where + ".italic",
            "must be true or false")
    _str(face.get("version"), where + ".version", _NAME)


def validate(data):
    """Raises ManifestError unless `data` (the parsed JSON) is a valid
    manifest. Returns `data`."""
    _expect(isinstance(data, dict), "manifest", "must be an object")
    _expect(data.get("format") == FORMAT, "format",
            "must be %d" % FORMAT)
    _str(data.get("version"), "version", re.compile(r"^[0-9]{4}\.[0-9]{1,2}\.[0-9]{1,3}$"))

    sources = data.get("sources")
    _expect(isinstance(sources, dict) and sources, "sources",
            "must be a non-empty object")
    for sid, source in sources.items():
        where = "sources.%s" % sid
        _str(sid, where, _SOURCE_ID)
        _expect(isinstance(source, dict), where, "must be an object")
        _str(source.get("repo"), where + ".repo", _REPO)
        _str(source.get("commit"), where + ".commit", _COMMIT)

    names_seen = {INSTALLED_MANIFEST.lower(): "the installed manifest",
                  LICENSE_DIR.lower(): "the licence folder"}

    def claim(name, where):
        _str(name, where, _INSTALL_NAME)
        key = name.lower()  # Windows and macOS file systems ignore case.
        _expect(key not in names_seen, where,
                "%r is also used by %s" % (name, names_seen.get(key)))
        names_seen[key] = where

    licenses = data.get("licenses")
    _expect(isinstance(licenses, list) and licenses, "licenses",
            "must be a non-empty list")
    license_names = set()
    for i, lic in enumerate(licenses):
        where = "licenses[%d]" % i
        _expect(isinstance(lic, dict), where, "must be an object")
        claim(lic.get("install_name"), where + ".install_name")
        _expect(lic.get("source") in sources, where + ".source",
                "unknown source %r" % lic.get("source"))
        _check_path(lic.get("path"), where + ".path")
        _str(lic.get("spdx"), where + ".spdx", re.compile(r"^[A-Za-z0-9.-]{1,32}$"))
        _check_pinned(lic, where, MAX_LICENSE_SIZE)
        license_names.add(lic["install_name"])

    files = data.get("files")
    _expect(isinstance(files, list) and 0 < len(files) <= MAX_FILES, "files",
            "must be a list of 1 to %d files" % MAX_FILES)
    families = {}
    for i, entry in enumerate(files):
        where = "files[%d]" % i
        _expect(isinstance(entry, dict), where, "must be an object")
        claim(entry.get("install_name"), where + ".install_name")
        _expect(entry.get("source") in sources, where + ".source",
                "unknown source %r" % entry.get("source"))
        _check_path(entry.get("path"), where + ".path")
        _expect(entry.get("license") in license_names, where + ".license",
                "must name one of the licences' install_name")
        _check_pinned(entry, where, MAX_FILE_SIZE)
        faces = entry.get("faces")
        _expect(isinstance(faces, list) and len(faces) <= MAX_FACES_PER_FILE,
                where + ".faces", "must be a list")
        _expect(faces or is_pending(entry), where + ".faces",
                "a pinned file lists its faces")
        indices = set()
        for j, face in enumerate(faces):
            fwhere = "%s.faces[%d]" % (where, j)
            _check_face(face, fwhere)
            _expect(face["index"] not in indices, fwhere, "duplicate index")
            indices.add(face["index"])
            families.setdefault(face["family"], []).append((i, face["index"]))

    pending = any(is_pending(e) for e in files + licenses)

    def family_ref(name, where):
        _str(name, where, _NAME)
        _expect(pending or name in families, where,
                "%r is not a family of the set" % name)
        return name

    aliases = data.get("aliases")
    _expect(isinstance(aliases, dict), "aliases", "must be an object")
    normalized = {normalize(f) for f in families}
    for name, target in aliases.items():
        where = "aliases[%r]" % name
        _str(name, where, _NAME)
        _expect(normalize(name) not in normalized, where,
                "an alias can't be a family's own name")
        _expect(normalize(name) not in {normalize(g) for g in GENERICS},
                where, "generic families are mapped in \"generic\"")
        family_ref(target, where)

    generic = data.get("generic")
    _expect(isinstance(generic, dict) and set(generic) == set(GENERICS),
            "generic", "must map exactly %s" % ", ".join(GENERICS))
    for name, target in generic.items():
        family_ref(target, "generic[%r]" % name)

    cjk = data.get("cjk")
    _expect(isinstance(cjk, dict) and set(cjk.get("regions", {})) ==
            set(HAN_REGIONS), "cjk.regions",
            "must map exactly %s" % ", ".join(HAN_REGIONS))
    for region, target in cjk["regions"].items():
        family_ref(target, "cjk.regions[%r]" % region)
    _expect(cjk.get("default") in HAN_REGIONS, "cjk.default",
            "must be one of %s" % ", ".join(HAN_REGIONS))
    by_script = cjk.get("scripts")
    _expect(isinstance(by_script, dict), "cjk.scripts", "must be an object")
    for script, region in by_script.items():
        _str(script, "cjk.scripts", _SCRIPT)
        _expect(region in HAN_REGIONS or region == "locale",
                "cjk.scripts[%r]" % script,
                "must be a region or \"locale\"")
    mono = cjk.get("monospace")
    _expect(mono is None or isinstance(mono, str), "cjk.monospace",
            "must be a family or null")

    fallback = data.get("fallback")
    _expect(isinstance(fallback, dict), "fallback", "must be an object")
    for key, chain in fallback.items():
        where = "fallback[%r]" % key
        _expect(key in FALLBACK_SPECIAL or _SCRIPT.match(key), where,
                "must be a script code or one of %s" %
                ", ".join(FALLBACK_SPECIAL))
        _expect(isinstance(chain, list) and chain, where,
                "must be a non-empty list")
        _expect(len(set(chain)) == len(chain), where, "lists a family twice")
        for j, name in enumerate(chain):
            if name == CJK_PLACEHOLDER:
                continue
            family_ref(name, "%s[%d]" % (where, j))
    for key in FALLBACK_SPECIAL + ("Zyyy",):
        _expect(key in fallback, "fallback", "needs a %r list" % key)
    return data


def load(path=MANIFEST):
    """The validated manifest at `path`."""
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError as e:
        raise ManifestError("can't read %s: %s" % (path, e))
    try:
        data = json.loads(text)
    except ValueError as e:
        raise ManifestError("%s is not JSON: %s" % (path, e))
    return validate(data)


def dump(data):
    """The manifest as the repository stores it (stable, diff-friendly)."""
    return json.dumps(data, indent=2, ensure_ascii=False) + "\n"


def normalize(name):
    """fonts.h's FontName normalization for comparing names: ASCII letters
    lower-cased, ASCII whitespace dropped, everything else as is."""
    return "".join(c.lower() if "A" <= c <= "Z" else c for c in name
                   if c not in " \t\n\r\f\v")


def families(data):
    """{family: [(file index, face index)]} in manifest order."""
    result = {}
    for i, entry in enumerate(data["files"]):
        for face in entry["faces"]:
            result.setdefault(face["family"], []).append((i, face["index"]))
    return result


def all_entries(data):
    """Every pinned download: the licences, then the font files."""
    return [("license", e) for e in data["licenses"]] + \
           [("font", e) for e in data["files"]]
