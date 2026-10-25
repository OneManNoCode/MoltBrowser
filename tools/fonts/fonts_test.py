#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for tools/fonts (no network): the font reader, the manifest rules,
the generated tables, fetching and installing, pinning, and the fresh-install
inventory.

  python3 -I tools/fonts/fonts_test.py -v

Fonts are built in the test (synthetic sfnt files). If the real bundled
fonts are in the cache (MOLT_FONTS_CACHE, default build/cache/fonts), the
pinned faces are also checked against them.
"""

import copy
import hashlib
import io
import json
import os
import random
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import fetch_fonts  # noqa: E402
import generate_tables  # noqa: E402
import manifest as mf  # noqa: E402
import pin_fonts  # noqa: E402
import sfnt  # noqa: E402


# --- Synthetic fonts -----------------------------------------------------------

def _name_table(records):
    """records: [(platform, encoding, language, name_id, text)]."""
    storage = b""
    entries = []
    for platform, encoding, language, name_id, text in records:
        raw = text.encode("utf-16-be") if platform in (0, 3) else \
            text.encode("mac_roman")
        entries.append((platform, encoding, language, name_id, len(raw),
                        len(storage)))
        storage += raw
    header = struct.pack(">HHH", 0, len(entries), 6 + 12 * len(entries))
    body = b"".join(struct.pack(">HHHHHH", *e) for e in entries)
    return header + body + storage


def _os2(weight=400, width=5, fs_selection=0x40):
    data = bytearray(96)
    struct.pack_into(">H", data, 0, 4)
    struct.pack_into(">H", data, 4, weight)
    struct.pack_into(">H", data, 6, width)
    struct.pack_into(">H", data, 62, fs_selection)
    return bytes(data)


def _head(revision=1.5, mac_style=0):
    data = bytearray(54)
    struct.pack_into(">i", data, 4, int(revision * 65536))
    struct.pack_into(">H", data, 44, mac_style)
    return bytes(data)


def _fvar(axes, instances):
    """axes: [(tag, min, default, max)]; instances: [(subfamily_id,
    postscript_id, {tag: value})]."""
    axis_size, count = 20, len(axes)
    instance_size = 4 + 4 * count + 2
    out = struct.pack(">HHHHHHHH", 1, 0, 16, 2, count, axis_size,
                      len(instances), instance_size)
    for tag, lo, default, hi in axes:
        out += tag.encode("latin-1") + struct.pack(
            ">iiiHH", int(lo * 65536), int(default * 65536),
            int(hi * 65536), 0, 256)
    for subfamily_id, ps_id, coords in instances:
        out += struct.pack(">HH", subfamily_id, 0)
        for tag, _, _, _ in axes:
            out += struct.pack(">i", int(coords[tag] * 65536))
        out += struct.pack(">H", ps_id)
    return out


def _cmap(codepoints):
    """A format 12 (3,10) cmap mapping each code point to glyph 1, 2, ..."""
    groups = b""
    for i, cp in enumerate(sorted(codepoints)):
        groups += struct.pack(">III", cp, cp, i + 1)
    n = len(codepoints)
    sub = struct.pack(">HHIII", 12, 0, 16 + 12 * n, 0, n) + groups
    return struct.pack(">HH", 0, 1) + struct.pack(">HHI", 3, 10, 12) + sub


def _cmap4(start, end, delta):
    seg = 2
    sub = struct.pack(">HHHHHHH", 4, 0, 0, seg * 2, 4, 1, 0)
    sub += struct.pack(">HH", end, 0xFFFF)  # ends
    sub += struct.pack(">H", 0)  # reservedPad
    sub += struct.pack(">HH", start, 0xFFFF)  # starts
    sub += struct.pack(">hh", delta, 1)  # deltas
    sub += struct.pack(">HH", 0, 0)  # range offsets
    sub = sub[:2] + struct.pack(">H", len(sub)) + sub[4:]
    return struct.pack(">HH", 0, 1) + struct.pack(">HHI", 3, 1, 12) + sub


def _face(family, style="Regular", weight=400, italic=False, extra=None,
          names_extra=(), fvar=None, cmap=None, tag=b"\x00\x01\x00\x00"):
    ps = family.replace(" ", "") + "-" + style.replace(" ", "")
    records = [(3, 1, 0x409, 1, family), (3, 1, 0x409, 2, style),
               (3, 1, 0x409, 4, "%s %s" % (family, style)),
               (3, 1, 0x409, 5, "Version 1.000"), (3, 1, 0x409, 6, ps)]
    records += list(names_extra)
    tables = {
        "name": _name_table(records),
        "OS/2": _os2(weight, fs_selection=0x01 if italic else 0x40),
        "head": _head(),
        "cmap": cmap or _cmap([0x41, 0x42, 0x1F600]),
    }
    if fvar:
        tables["fvar"] = fvar
    tables.update(extra or {})
    return tag, tables


def build_font(faces):
    """An sfnt (one face) or a TTC (several) from _face() results."""
    def directory(tag, tables, base):
        tags = sorted(tables)
        header = tag + struct.pack(">HHHH", len(tags), 0, 0, 0)
        offset = base + 12 + 16 * len(tags)
        records, blobs = b"", b""
        for t in tags:
            blob = tables[t]
            records += t.encode("latin-1") + struct.pack(
                ">III", 0, offset + len(blobs), len(blob))
            blobs += blob + b"\0" * (-len(blob) % 4)
        return header + records + blobs

    if len(faces) == 1:
        return directory(faces[0][0], faces[0][1], 0)
    head_len = 12 + 4 * len(faces)
    out = b""
    offsets = []
    for tag, tables in faces:
        offsets.append(head_len + len(out))
        out += directory(tag, tables, head_len + len(out))
    return (b"ttcf" + struct.pack(">HHI", 1, 0, len(faces)) +
            b"".join(struct.pack(">I", o) for o in offsets) + out)


# --- sfnt ----------------------------------------------------------------------

class SfntTest(unittest.TestCase):
    def test_single_face(self):
        data = build_font([_face("Test Sans", "Bold", 700)])
        faces = sfnt.read_faces(data)
        self.assertEqual(len(faces), 1)
        face = faces[0]
        self.assertEqual(face["family"], "Test Sans")
        self.assertEqual(face["skia_family"], "Test Sans")
        self.assertEqual(face["postscript_name"], "TestSans-Bold")
        self.assertEqual(face["full_name"], "Test Sans Bold")
        self.assertEqual(face["weight"], 700)
        self.assertFalse(face["italic"])
        self.assertEqual(face["font_revision"], 1.5)
        self.assertEqual(face["version"], "Version 1.000")

    def test_typographic_and_wws_family(self):
        data = build_font([_face("Test Sans Light", "Regular", 300, names_extra=[
            (3, 1, 0x409, 16, "Test Sans"), (3, 1, 0x409, 17, "Light"),
            (3, 1, 0x411, 16, "テスト")])])
        face = sfnt.read_faces(data)[0]
        self.assertEqual(face["family"], "Test Sans Light")
        self.assertEqual(face["skia_family"], "Test Sans")
        self.assertEqual(face["typographic_subfamily"], "Light")
        self.assertIn("テスト", face["family_names"])
        # WWS family wins unless OS/2 marks the face WWS-only.
        tag, tables = _face("A", names_extra=[(3, 1, 0x409, 16, "B"),
                                              (3, 1, 0x409, 21, "C")])
        self.assertEqual(sfnt.read_faces(build_font([(tag, tables)]))[0]
                         ["skia_family"], "C")
        tables["OS/2"] = _os2(fs_selection=0x0140)
        self.assertEqual(sfnt.read_faces(build_font([(tag, tables)]))[0]
                         ["skia_family"], "B")

    def test_mac_roman_and_english_preference(self):
        tag, tables = _face("x")
        # English (Mac Roman, language 0) beats another language.
        tables["name"] = _name_table([(1, 0, 0, 1, "Caf\xe9"),
                                      (3, 1, 0x40c, 1, "Fran\xe7ais")])
        face = sfnt.read_faces(build_font([(tag, tables)]))[0]
        self.assertEqual(face["family"], "Caf\xe9")
        self.assertEqual(face["family_names"], ["Caf\xe9", "Fran\xe7ais"])
        # Windows en-US beats everything.
        tables["name"] = _name_table([(1, 0, 0, 1, "Caf\xe9"),
                                      (3, 1, 0x409, 1, "Coffee")])
        face = sfnt.read_faces(build_font([(tag, tables)]))[0]
        self.assertEqual(face["family"], "Coffee")
        # Without English, a Windows name.
        tables["name"] = _name_table([(3, 1, 0x40c, 1, "Fran\xe7ais"),
                                      (1, 0, 2, 1, "Deutsch")])
        face = sfnt.read_faces(build_font([(tag, tables)]))[0]
        self.assertEqual(face["family"], "Fran\xe7ais")

    def test_collection_and_fvar(self):
        fvar = _fvar([("wght", 100, 100, 900), ("wdth", 75, 100, 100)],
                     [(2, 6, {"wght": 400, "wdth": 100})])
        data = build_font([
            _face("Test CJK JP", "Thin", 100, fvar=fvar, tag=b"OTTO"),
            _face("Test CJK KR", "Thin", 100, fvar=fvar, tag=b"OTTO")])
        faces = sfnt.read_faces(data)
        self.assertEqual([f["family"] for f in faces],
                         ["Test CJK JP", "Test CJK KR"])
        self.assertEqual(faces[1]["index"], 1)
        self.assertEqual(faces[0]["outlines"], "cff")
        axes = {a["tag"]: a for a in faces[0]["axes"]}
        self.assertEqual((axes["wght"]["min"], axes["wght"]["default"],
                          axes["wght"]["max"]), (100, 100, 900))
        self.assertEqual(faces[0]["instances"][0]["coordinates"]["wght"], 400)
        self.assertEqual(faces[0]["instances"][0]["postscript"],
                         "TestCJKJP-Thin")

    def test_coverage(self):
        data = build_font([_face("C", cmap=_cmap([0x41, 0x4E00, 0x1F600]))])
        covered = sfnt.coverage(data)
        self.assertTrue(covered(0x41))
        self.assertTrue(covered(0x4E00))
        self.assertTrue(covered(0x1F600))
        self.assertFalse(covered(0x42))
        self.assertFalse(covered(0x10FFFF))
        data = build_font([_face("D", cmap=_cmap4(0x30, 0x39, 5))])
        covered = sfnt.coverage(data)
        self.assertTrue(covered(0x30))
        self.assertTrue(covered(0x39))
        self.assertFalse(covered(0x3A))
        self.assertFalse(covered(0x1F600))

    def test_not_a_font(self):
        for data in (b"", b"abcd", b"wOFF" + b"\0" * 40, b"ttcf" + b"\0" * 8,
                     b"\x00\x01\x00\x00" + b"\xff" * 8):
            with self.assertRaises(sfnt.SfntError):
                sfnt.read_faces(data)

    def test_hostile_input_raises_only_sfnt_error(self):
        rng = random.Random(1)
        seeds = [
            build_font([_face("Seed", fvar=_fvar([("wght", 1, 2, 3)],
                                                 [(2, 6, {"wght": 2})]))]),
            build_font([_face("A"), _face("B", tag=b"OTTO")]),
            build_font([_face("C", cmap=_cmap4(0x20, 0x7e, 0))]),
        ]
        for i in range(3000):
            data = bytearray(rng.choice(seeds))
            for _ in range(rng.randint(1, 12)):
                kind = rng.random()
                pos = rng.randrange(len(data))
                if kind < 0.6:
                    data[pos] = rng.randrange(256)
                elif kind < 0.8:
                    del data[pos:pos + rng.randint(1, 64)]
                else:
                    data[pos:pos] = bytes(rng.randrange(256)
                                          for _ in range(rng.randint(1, 8)))
            try:
                for face_offset in sfnt.face_offsets(bytes(data)):
                    sfnt.read_face(bytes(data), face_offset)
                    sfnt.coverage(bytes(data), face_offset)(0x41)
            except sfnt.SfntError:
                pass

    def test_real_bundled_fonts_match_the_manifest(self):
        cache = Path(os.environ.get("MOLT_FONTS_CACHE",
                                    str(fetch_fonts.DEFAULT_CACHE)))
        data = mf.load()
        checked = 0
        for entry in data["files"]:
            path = cache / entry["sha256"]
            if not path.is_file():
                continue
            self.assertEqual(pin_fonts.describe_faces(path.read_bytes()),
                             entry["faces"], entry["install_name"])
            checked += 1
        if not checked:
            self.skipTest("no bundled font in %s" % cache)


# --- The manifest --------------------------------------------------------------

class ManifestTest(unittest.TestCase):
    def setUp(self):
        self.data = mf.load()

    def broken(self, mutate, message=None):
        data = copy.deepcopy(self.data)
        mutate(data)
        with self.assertRaises(mf.ManifestError) as ctx:
            mf.validate(data)
        if message:
            self.assertIn(message, str(ctx.exception))

    def test_repository_manifest_is_valid_and_pinned(self):
        self.assertFalse([e for _, e in mf.all_entries(self.data)
                          if mf.is_pending(e)])
        total = sum(e["size"] for e in self.data["files"])
        self.assertLess(total, 64 << 20, "the set is meant to stay small")
        for source in self.data["sources"].values():
            self.assertRegex(source["commit"], "^[0-9a-f]{40}$")
        self.assertEqual(mf.dump(self.data), mf.MANIFEST.read_text())

    def test_urls_are_pinned_raw_urls(self):
        entry = self.data["files"][0]
        url = mf.source_url(self.data["sources"][entry["source"]],
                            entry["path"])
        self.assertTrue(url.startswith("https://raw.githubusercontent.com/"))
        self.assertIn(self.data["sources"][entry["source"]]["commit"], url)
        self.assertNotIn("[", url)  # Arimo[wght].ttf is quoted.

    def test_paths_and_names(self):
        def files(i, key, value):
            return lambda d: d["files"][i].__setitem__(key, value)
        self.broken(files(0, "path", "../etc/passwd"), "plain relative path")
        self.broken(files(0, "path", "/abs/x.ttf"), "plain relative path")
        self.broken(files(0, "path", "a//b.ttf"), "plain relative path")
        self.broken(files(0, "path", "a/b\\c.ttf"), "isn't allowed")
        self.broken(files(0, "path", "a/b c.ttf"), "isn't allowed")
        self.broken(files(0, "install_name", "../x.ttf"), "not allowed")
        self.broken(files(0, "install_name", "a/b.ttf"), "not allowed")
        self.broken(files(0, "install_name", ".hidden"), "not allowed")
        self.broken(files(0, "install_name", "LICENSES"), "also used")
        self.broken(files(0, "install_name", "MoltNet_Fonts.json"),
                    "also used")
        self.broken(files(1, "install_name",
                          self.data["files"][0]["install_name"].upper()),
                    "also used")
        self.broken(files(0, "source", "elsewhere"), "unknown source")
        self.broken(files(0, "license", "GPL.txt"), "licences")

    def test_pins(self):
        def files(key, value):
            return lambda d: d["files"][0].__setitem__(key, value)
        self.broken(files("sha256", "0" * 63), "not allowed")
        self.broken(files("sha256", "Z" * 64), "not allowed")
        self.broken(files("size", 0), "integer")
        self.broken(files("size", mf.MAX_FILE_SIZE + 1), "integer")
        self.broken(files("size", True), "integer")
        self.broken(files("faces", []), "lists its faces")
        self.broken(lambda d: d["sources"]["noto"].__setitem__(
            "commit", "main"), "not allowed")
        self.broken(lambda d: d["sources"]["noto"].__setitem__(
            "repo", "evil.com/../x"), "not allowed")

        def pending(d):
            d["files"][0]["sha256"] = mf.PENDING
            d["files"][0]["size"] = 0
            d["files"][0]["faces"] = []
        data = copy.deepcopy(self.data)
        pending(data)
        mf.validate(data)  # A pending pin is valid ...
        data["files"][0]["size"] = 12
        with self.assertRaises(mf.ManifestError):
            mf.validate(data)  # ... with size 0 only.

    def test_faces(self):
        def face(key, value):
            return lambda d: d["files"][0]["faces"][0].__setitem__(key, value)
        self.broken(face("weight", [700, 400, 900]), "min <= default")
        self.broken(face("weight", [400, 400]), "[min, default, max]")
        self.broken(face("italic", "no"), "true or false")
        self.broken(face("family", "Not In Names"), "family_names")
        self.broken(face("family", "a\nb"), "not allowed")
        self.broken(face("index", 99), "integer")

    def test_family_references(self):
        self.broken(lambda d: d["aliases"].__setitem__("Arial", "Nope"),
                    "not a family")
        self.broken(lambda d: d["aliases"].__setitem__("Arimo", "Tinos"),
                    "own name")
        self.broken(lambda d: d["aliases"].__setitem__("Sans-Serif", "Arimo"),
                    "generic")
        self.broken(lambda d: d["generic"].__setitem__("serif", "Nope"),
                    "not a family")
        self.broken(lambda d: d["generic"].pop("math"), "must map exactly")
        self.broken(lambda d: d["cjk"]["regions"].pop("HK"),
                    "must map exactly")
        self.broken(lambda d: d["cjk"].__setitem__("default", "XX"),
                    "must be one of")
        self.broken(lambda d: d["cjk"]["scripts"].__setitem__("Hani", "XX"),
                    "region")
        self.broken(lambda d: d["fallback"].__setitem__("Latn", ["Nope"]),
                    "not a family")
        self.broken(lambda d: d["fallback"].__setitem__("Latn", []),
                    "non-empty")
        self.broken(lambda d: d["fallback"].__setitem__("Latn",
                                                        ["Arimo", "Arimo"]),
                    "twice")
        self.broken(lambda d: d["fallback"].__setitem__("latin", ["Arimo"]),
                    "script code")
        self.broken(lambda d: d["fallback"].pop("emoji"), "'emoji'")
        self.broken(lambda d: d["fallback"].pop("Zyyy"), "'Zyyy'")
        self.broken(lambda d: d.__setitem__("format", 2), "format")

    def test_not_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "m.json")
            path.write_text("{")
            with self.assertRaises(mf.ManifestError):
                mf.load(path)
            path.write_text("[]")
            with self.assertRaises(mf.ManifestError):
                mf.load(path)
            with self.assertRaises(mf.ManifestError):
                mf.load(Path(tmp, "missing.json"))


# --- Generated tables ----------------------------------------------------------

class GenerateTest(unittest.TestCase):
    def test_tables_are_up_to_date(self):
        self.assertEqual(generate_tables.main([]), 0)

    def test_hash_matches_fonts_cc(self):
        # FNV-1a over normalized code points (fonts.cc); the C++ unit test
        # checks the generated hashes against FontName::Hash().
        self.assertEqual(generate_tables.font_name_hash(""),
                         generate_tables.FNV_OFFSET)
        self.assertEqual(generate_tables.font_name_hash("Arial"),
                         generate_tables.font_name_hash(" a R i A l "))
        self.assertNotEqual(generate_tables.font_name_hash("Arial"),
                            generate_tables.font_name_hash("Arimo"))

    def test_model(self):
        m = generate_tables.model(mf.load())
        families = [f["name"] for f in m["families"]]
        self.assertEqual(len(families), len(set(families)))
        # Every face of a family together, and the file order kept.
        for i, fam in enumerate(m["families"]):
            faces = m["faces"][fam["first"]:fam["first"] + fam["count"]]
            self.assertTrue(all(f["family"] == i for f in faces))
        self.assertEqual([f["name"] for f in m["files"]],
                         [e["install_name"] for e in mf.load()["files"]])
        self.assertEqual(m["fallback"],
                         sorted(m["fallback"], key=lambda e: e["key"]))

    def test_pending_manifest_is_refused(self):
        data = mf.load()
        data["files"][0]["sha256"] = mf.PENDING
        data["files"][0]["size"] = 0
        with self.assertRaises(generate_tables.GenerateError):
            generate_tables.model(data)

    def test_cpp_string(self):
        self.assertEqual(generate_tables.cpp_string('a"\\b'), '"a\\"\\\\b"')
        self.assertEqual(generate_tables.cpp_string("宋"), '"\\u5B8B"')
        self.assertEqual(generate_tables.cpp_string("\U0001F600"),
                         '"\\U0001F600"')


# --- Fetching and installing ----------------------------------------------------

def tiny_manifest(blobs):
    """A valid manifest of synthetic files {install_name: bytes}."""
    data = mf.load()
    data = copy.deepcopy(data)
    lic = data["licenses"][0]
    lic_blob = b"SIL Open Font License 1.1\n"
    lic.update(size=len(lic_blob), sha256=hashlib.sha256(lic_blob).hexdigest(),
               path="LICENSE")
    data["licenses"] = [lic]
    files = []
    for name, blob in blobs.items():
        faces = pin_fonts.describe_faces(blob)
        files.append({"install_name": name, "source": lic["source"],
                      "path": "fonts/" + name, "license": lic["install_name"],
                      "size": len(blob),
                      "sha256": hashlib.sha256(blob).hexdigest(),
                      "faces": faces})
    data["files"] = files
    fam = files[0]["faces"][0]["family"]
    data["aliases"] = {}
    data["generic"] = {g: fam for g in mf.GENERICS}
    data["cjk"]["regions"] = {r: fam for r in mf.HAN_REGIONS}
    data["fallback"] = {"Zyyy": [fam], "emoji": [fam], "text-emoji": [fam]}
    mf.validate(data)
    served = {"LICENSE": lic_blob}
    served.update({"fonts/" + n: b for n, b in blobs.items()})
    return data, served


class FakeOpener:
    def __init__(self, data, served):
        self.urls = {}
        for path, blob in served.items():
            source = data["sources"][data["licenses"][0]["source"]]
            self.urls[mf.source_url(source, path)] = blob
        self.calls = []

    def __call__(self, url, timeout=None):
        self.calls.append(url)
        if url not in self.urls:
            raise OSError("404 %s" % url)
        return io.BytesIO(self.urls[url])


class FetchTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.blobs = {"A-Regular.ttf": build_font([_face("Alpha")]),
                      "B.ttc": build_font([_face("Beta One"),
                                           _face("Beta Two")])}
        self.data, self.served = tiny_manifest(self.blobs)
        self.opener = FakeOpener(self.data, self.served)

    def tearDown(self):
        self.tmp.cleanup()

    def install(self, **kw):
        return fetch_fonts.install(self.data, self.root / "molt_fonts",
                                   self.root / "cache", opener=self.opener,
                                   **kw)

    def test_install_verify_and_cache(self):
        self.install()
        dest = self.root / "molt_fonts"
        self.assertEqual(sorted(p.relative_to(dest).as_posix()
                                for p in dest.rglob("*") if p.is_file()),
                         sorted(["A-Regular.ttf", "B.ttc",
                                 "LICENSES/OFL-Arimo.txt",
                                 "moltnet_fonts.json"]))
        self.assertEqual(fetch_fonts.verify(self.data, dest), [])
        self.assertEqual(json.loads((dest / "moltnet_fonts.json").read_text()),
                         self.data)
        calls = len(self.opener.calls)
        self.install()  # From the cache: nothing fetched.
        self.assertEqual(len(self.opener.calls), calls)
        self.install(offline=True)

    def test_verify_finds_changes(self):
        self.install()
        dest = self.root / "molt_fonts"
        (dest / "stale.ttf").write_bytes(b"x")
        (dest / "B.ttc").write_bytes(b"tampered")
        (dest / "A-Regular.ttf").unlink()
        problems = fetch_fonts.verify(self.data, dest)
        self.assertIn("unexpected file stale.ttf", problems)
        self.assertIn("B.ttc doesn't match its pin", problems)
        self.assertIn("missing A-Regular.ttf", problems)
        self.assertTrue(fetch_fonts.verify(self.data, self.root / "nope"))

    def test_install_replaces_a_previous_set(self):
        dest = self.root / "molt_fonts"
        dest.mkdir()
        (dest / "Old.ttf").write_bytes(b"old")
        self.install()
        self.assertFalse((dest / "Old.ttf").exists())

    def test_mismatch_and_errors(self):
        self.opener.urls = {u: b + b"!" for u, b in self.opener.urls.items()}
        with self.assertRaises(fetch_fonts.FetchError):
            self.install()
        self.assertFalse((self.root / "molt_fonts").exists())
        with self.assertRaises(fetch_fonts.FetchError):
            fetch_fonts.install(self.data, self.root / "x", self.root / "empty",
                                offline=True)
        with self.assertRaises(fetch_fonts.FetchError):
            fetch_fonts.download("https://example.com/x.ttf",
                                 self.root / "x", 10, self.opener)
        with self.assertRaises(fetch_fonts.FetchError):
            fetch_fonts.download("http://raw.githubusercontent.com/x",
                                 self.root / "x", 10, self.opener)

    def test_download_limit(self):
        url = next(iter(self.opener.urls))
        with self.assertRaises(fetch_fonts.FetchError):
            fetch_fonts.download(url, self.root / "small", 3, self.opener)
        self.assertFalse((self.root / "small").exists())
        self.assertFalse((self.root / "small.part").exists())

    def test_release_build_refuses_pending(self):
        data = copy.deepcopy(self.data)
        data["files"].append(dict(data["files"][0], install_name="P.ttf",
                                  sha256=mf.PENDING, size=0, faces=[]))
        path = self.root / "pending.json"
        path.write_text(mf.dump(data))
        old = os.environ.get("MOLT_RELEASE_BUILD")
        os.environ["MOLT_RELEASE_BUILD"] = "1"
        try:
            self.assertEqual(fetch_fonts.main(
                ["--dest", str(self.root / "d"), "--manifest", str(path),
                 "--offline", "--cache", str(self.root / "cache")]), 1)
        finally:
            if old is None:
                del os.environ["MOLT_RELEASE_BUILD"]
            else:
                os.environ["MOLT_RELEASE_BUILD"] = old


class PinTest(unittest.TestCase):
    def test_describe_variable_and_collection(self):
        fvar = _fvar([("wght", 100, 100, 900)], [(2, 6, {"wght": 100})])
        faces = pin_fonts.describe_faces(build_font([
            _face("V JP", "Thin", 100, fvar=fvar),
            _face("V KR", "Thin", 100, fvar=fvar, italic=True)]))
        self.assertEqual(faces[0]["weight"], [100, 100, 900])
        self.assertEqual(faces[1]["index"], 1)
        self.assertTrue(faces[1]["italic"])
        self.assertEqual(faces[0]["family_names"], ["V JP"])

    def test_write_fills_pending_entries(self):
        blobs = {"A-Regular.ttf": build_font([_face("Alpha")])}
        data, served = tiny_manifest(blobs)
        opener = FakeOpener(data, served)
        entry = data["files"][0]
        entry.update(sha256=mf.PENDING, size=0, faces=[])
        with tempfile.TemporaryDirectory() as tmp:
            real = fetch_fonts.download
            fetch_fonts.download = lambda url, dest, limit, _=None: real(
                url, dest, limit, opener)
            try:
                problems = pin_fonts.run(data, Path(tmp), write=True,
                                         offline=False)
                self.assertEqual(problems, [])
                self.assertEqual(entry["size"], len(blobs["A-Regular.ttf"]))
                self.assertEqual(entry["faces"][0]["family"], "Alpha")
                # Check mode now agrees, and notices a changed face.
                self.assertEqual(pin_fonts.run(data, Path(tmp), write=False,
                                               offline=True), [])
                entry["faces"][0]["weight"] = [700, 700, 700]
                self.assertTrue(pin_fonts.run(data, Path(tmp), write=False,
                                              offline=True))
            finally:
                fetch_fonts.download = real


if __name__ == "__main__":
    unittest.main()
