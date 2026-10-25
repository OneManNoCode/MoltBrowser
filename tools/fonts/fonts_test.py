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

import base64
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
import molt_font_inventory as mi  # noqa: E402
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


# --- Fresh-install inventory -------------------------------------------------

def _variable_face(family, instances):
    """A variable face whose named instances are `instances` (subfamily
    names), as Windows' Bahnschrift."""
    names = [(3, 1, 0x409, 256 + i, s) for i, s in enumerate(instances)]
    fvar = _fvar([("wght", 300, 400, 700)],
                 [(256 + i, 0xFFFF, {"wght": 400}) for i in range(
                     len(instances))])
    return _face(family, names_extra=names, fvar=fvar)


class InventoryTest(unittest.TestCase):
    ALLOWLISTS = {"linux": [
        {"name": "Alpha"}, {"name": "Beta Light SemiCondensed"},
        {"name": "Delta"}, {"name": "Epsilon"}, {"name": "Missing Sans"},
        {"name": "Helvetica", "alias_of": "Alpha"}]}

    def make_tree(self, tmp):
        """system/ (Alpha, Beta with instances, Gamma unlisted, a Type 1 file,
        a broken .ttf), on-demand/ (Delta), user/ (Epsilon)."""
        files = {
            "system/alpha.ttf": build_font([_face("Alpha")]),
            "system/sub/beta.ttf": build_font([_variable_face(
                "Beta", ["Regular", "Bold", "Light SemiCondensed"])]),
            "system/gamma.ttc": build_font([_face("Gamma"),
                                            _face("Gamma", "Bold", 700)]),
            "system/old.pfb": b"%!PS-AdobeFont-1.0",
            "system/broken.ttf": b"\x00\x01\x00\x00\x00\x09",
            "system/readme.txt": b"not a font",
            "ondemand/delta.otf": build_font([_face("Delta", tag=b"OTTO")]),
            "user/epsilon.ttf": build_font([_face("Epsilon")]),
        }
        for rel, data in files.items():
            path = Path(tmp) / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        return [(str(Path(tmp) / "system"), "system"),
                (str(Path(tmp) / "ondemand"), "on-demand"),
                (str(Path(tmp) / "user"), "user")], files

    def test_assemble_and_legacy_names(self):
        tag, tables = _variable_face("Bahnschrift",
                                     ["Regular", "Light SemiCondensed",
                                      "Bold Italic", "SemiBold"])
        whole = sfnt.read_faces(build_font([(tag, tables)]))[0]
        wanted = {t: tables[t] for t in mi.RAW_TABLES if t in tables}
        rebuilt = sfnt.read_face(sfnt.assemble(tag, wanted), 0)
        for key in ("family", "full_name", "version", "weight", "axes",
                    "instances", "font_revision"):
            self.assertEqual(rebuilt[key], whole[key], key)
        self.assertEqual(sfnt.legacy_family_names(whole),
                         ["Bahnschrift Light SemiCondensed",
                          "Bahnschrift SemiBold"])
        with self.assertRaises(sfnt.SfntError):
            sfnt.assemble(b"wOFF", wanted)
        with self.assertRaises(sfnt.SfntError):
            sfnt.assemble(tag, {"toolong": b""})

    def test_registry_names(self):
        self.assertEqual(mi.registry_family_names("Arial Bold (TrueType)"),
                         ["Arial Bold"])
        self.assertEqual(mi.registry_family_names(
            "MS Sans Serif 8,10,12,14,18,24"), ["MS Sans Serif"])
        self.assertEqual(mi.registry_family_names(
            "Cambria & Cambria Math (TrueType)"), ["Cambria", "Cambria Math"])
        self.assertEqual(mi.registry_family_names("Marlett"), ["Marlett"])

    def test_redaction(self):
        self.assertEqual(mi._redact("/home/u/.fonts/a.ttf", "/home/u"),
                         "~/.fonts/a.ttf")
        self.assertEqual(mi._redact("/home/user2/a.ttf", "/home/u"),
                         "/home/user2/a.ttf")
        self.assertEqual(mi._redact(r"C:\Users\U\AppData\a.ttf", r"C:\Users\U"),
                         r"~\AppData\a.ttf")
        self.assertEqual(mi._redact("/usr/share/fonts/a.ttf", "/"),
                         "/usr/share/fonts/a.ttf")

    def test_collect_and_compare(self):
        with tempfile.TemporaryDirectory() as tmp:
            roots, files = self.make_tree(tmp)
            inventory = mi.collect(plat="linux", roots=roots)
            report = mi.compare(inventory, self.ALLOWLISTS)
        self.assertEqual(len(inventory["fonts"]), 7)  # not readme.txt
        self.assertEqual(report["listed_present"],
                         ["Alpha", "Beta Light SemiCondensed"])
        self.assertEqual(report["listed_missing"], ["Missing Sans"])
        self.assertEqual(report["listed_on_demand_only"], ["Delta"])
        self.assertEqual(report["listed_user_only"], ["Epsilon"])
        self.assertEqual(report["aliases"], ["Helvetica"])
        self.assertEqual(report["unlisted_families"],
                         [{"family": "Gamma", "files": ["gamma.ttc"]}])
        self.assertEqual(report["listed_versions"]["Alpha"], [{
            "file": "alpha.ttf", "version": "Version 1.000",
            "sha256": hashlib.sha256(files["system/alpha.ttf"]).hexdigest()}])
        errors = {Path(u["path"]).name: u["error"]
                  for u in report["unreadable_files"]}
        self.assertEqual(set(errors), {"old.pfb", "broken.ttf"})
        self.assertIn("not an OpenType font", errors["old.pfb"])
        self.assertIn("unreadable font", errors["broken.ttf"])
        self.assertEqual(len(report["warnings"]), 1)  # the user's font
        text = mi.report_markdown(report)
        self.assertIn("- Missing Sans", text)
        self.assertIn("| Gamma | gamma.ttc |", text)

    def test_registry_names_count_for_bitmap_fonts(self):
        inventory = {"format": mi.INVENTORY_FORMAT,
                     "os": {"platform": "windows", "label": "windows11-24H2"},
                     "fonts": [{"path": r"C:\Windows\Fonts\sserife.fon",
                                "location": "system",
                                "registry_names": [
                                    "MS Sans Serif 8,10,12,14,18,24"],
                                "error": "not an OpenType font"}]}
        report = mi.compare(inventory, {"windows": [
            {"name": "MS Sans Serif"}, {"name": "Arial"}]})
        self.assertEqual(report["listed_present"], ["MS Sans Serif"])
        self.assertEqual(report["listed_missing"], ["Arial"])
        self.assertEqual(report["unlisted_families"], [])

    def raw_from_tree(self, roots):
        """What the no-install collectors write, made here."""
        fonts = []
        for root, location in roots:
            for path in sorted(Path(root).rglob("*")):
                if not path.is_file() or not mi._is_font_name(path.name):
                    continue
                data = path.read_bytes()
                item = {"path": str(path), "location": location,
                        "size": len(data),
                        "sha256": hashlib.sha256(data).hexdigest()}
                try:
                    faces = []
                    for offset in sfnt.face_offsets(data):
                        tables, tag = sfnt.table_directory(data, offset)
                        faces.append({"tag": tag.hex(), "tables": {
                            t: base64.b64encode(data[o:o + n]).decode()
                            for t, (o, n) in tables.items()
                            if t in mi.RAW_TABLES}})
                    if path.suffix in (".ttf", ".otf", ".ttc"):
                        item["faces"] = faces
                    else:
                        item["error"] = "not an OpenType font (not compared)"
                except sfnt.SfntError as e:
                    item["error"] = "unreadable font: %s" % e
                fonts.append(item)
        return {"format": mi.RAW_FORMAT, "collector": "test",
                "collected": "2026-10-25T00:00:00Z",
                "os": {"platform": "linux", "id": "test", "version_id": "1",
                       "label": "test-1"},
                "fonts": fonts}

    def assert_same_faces(self, a, b):
        fa = {Path(f["path"]).name: f.get("faces") for f in a["fonts"]}
        fb = {Path(f["path"]).name: f.get("faces") for f in b["fonts"]}
        self.assertEqual(fa, fb)

    def test_import_raw_matches_collect(self):
        with tempfile.TemporaryDirectory() as tmp:
            roots, _ = self.make_tree(tmp)
            collected = mi.collect(plat="linux", roots=roots)
            imported = mi.import_raw(self.raw_from_tree(roots))
        self.assertEqual(imported["os"]["label"], "test-1")
        self.assert_same_faces(collected, imported)
        self.assertEqual(mi.compare(imported, self.ALLOWLISTS)["listed_missing"],
                         ["Missing Sans"])

    @unittest.skipUnless(os.path.exists("/usr/bin/perl"), "no /usr/bin/perl")
    def test_perl_collector_matches_collect(self):
        import subprocess
        with tempfile.TemporaryDirectory() as tmp:
            roots, _ = self.make_tree(tmp)
            out = str(Path(tmp) / "raw.json")
            argv = ["/usr/bin/perl", str(HERE / "collect_fonts.pl"),
                    "--out", out]
            for root, location in roots:
                argv += ["--root", "%s=%s" % (root, location)]
            result = subprocess.run(argv, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, check=False)
            if b"Can't locate" in result.stderr:
                self.skipTest("perl without core modules")
            self.assertEqual(result.returncode, 0, result.stderr)
            imported = mi.import_raw(json.loads(Path(out).read_text()))
            collected = mi.collect(plat="linux", roots=roots)
        self.assert_same_faces(collected, imported)
        sha = {Path(f["path"]).name: f.get("sha256") for f in collected["fonts"]}
        self.assertEqual(
            sha, {Path(f["path"]).name: f.get("sha256")
                  for f in imported["fonts"]})

    def test_import_raw_refuses_or_contains_bad_input(self):
        for bad in (None, [], {"format": "x"},
                    {"format": mi.RAW_FORMAT, "os": {"platform": "beos"}},
                    {"format": mi.RAW_FORMAT, "os": {"platform": "mac"},
                     "fonts": "x"}):
            with self.assertRaises(mi.InventoryError):
                mi.import_raw(bad)
        with tempfile.TemporaryDirectory() as tmp:
            roots, _ = self.make_tree(tmp)
            seed = self.raw_from_tree(roots)
        rng = random.Random(7)
        junk = [None, 1, -1, "", "zz", "AAAA", [], {}, {"tag": 1},
                {"tag": "00010000", "tables": {"name": "!!"}},
                {"tag": "00010000", "tables": []}, 2 ** 70, "\u0000"]
        for _ in range(400):
            raw = copy.deepcopy(seed)
            for _ in range(rng.randint(1, 4)):
                item = rng.choice(raw["fonts"])
                key = rng.choice(["path", "location", "size", "sha256",
                                  "faces", "registry_names", "error"])
                faces = item.get("faces")
                if key == "faces" and isinstance(faces, list) and faces and \
                        isinstance(faces[0], dict) and rng.random() < .7:
                    face = rng.choice(faces)
                    if isinstance(face.get("tables"), dict) and \
                            face["tables"] and rng.random() < .5:
                        t = rng.choice(sorted(face["tables"]))
                        blob = bytearray(base64.b64decode(face["tables"][t]))
                        if blob:
                            blob[rng.randrange(len(blob))] = rng.randrange(256)
                        face["tables"][t] = base64.b64encode(
                            bytes(blob[:rng.randrange(len(blob) + 1)])).decode()
                    else:
                        face[rng.choice(["tag", "tables"])] = rng.choice(junk)
                else:
                    item[key] = rng.choice(junk)
            inventory = mi.import_raw(raw)
            mi.report_markdown(mi.compare(inventory, self.ALLOWLISTS))

    def test_merge(self):
        def report(label, missing, unlisted, version):
            return {"format": mi.REPORT_FORMAT,
                    "os": {"platform": "mac", "label": label},
                    "listed_missing": missing, "listed_on_demand_only": [],
                    "listed_user_only": [],
                    "unlisted_families": [{"family": f, "files": []}
                                          for f in unlisted],
                    "listed_versions": {"Helvetica": [
                        {"file": "Helvetica.ttc", "version": version,
                         "sha256": "ab" * 32}]}}
        text = mi.merge([report("macos-13.7", ["Old Font"], ["New", "Odd"],
                                "17.0d1e1"),
                         report("macos-15.1", [], ["New"], "18.0d1e1")])
        self.assertIn("- Old Font: not on macos-13.7", text)
        self.assertIn("- New\n", text)
        self.assertNotIn("- Odd", text)
        self.assertIn("- Helvetica: Helvetica.ttc 17.0d1e1", text)
        with self.assertRaises(mi.InventoryError):
            mi.merge([report("a", [], [], "1"),
                      dict(report("b", [], [], "1"),
                           os={"platform": "windows"})])

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            roots, _ = self.make_tree(tmp)
            raw = Path(tmp) / "raw.json"
            # A BOM, as Windows PowerShell 5.1 may write.
            raw.write_text("\ufeff" + json.dumps(self.raw_from_tree(roots)),
                           encoding="utf-8")
            lists = Path(tmp) / "lists.json"
            lists.write_text(json.dumps({"lists": self.ALLOWLISTS}))
            out = Path(tmp) / "out"
            with contextlib_redirect():
                self.assertEqual(mi.main(["import", str(raw), "--out-dir",
                                          str(out), "--allowlists",
                                          str(lists)]), 0)
                written = out / "molt-fonts-test-1.json"
                self.assertTrue((out / "molt-fonts-test-1.md").exists())
                self.assertEqual(mi.main(["compare", str(written),
                                          "--out-dir", str(out / "again"),
                                          "--allowlists", str(lists)]), 0)
                self.assertEqual(mi.main(["merge", str(written), str(
                    out / "again" / "molt-fonts-test-1.json"), "--out",
                    str(out / "merged.md")]), 0)
                self.assertEqual(mi.main(["import", str(lists)]), 2)
                self.assertEqual(mi.main(["compare", str(Path(tmp) / "no")]),
                                 2)
            doc = json.loads(written.read_text())
            self.assertEqual(doc["listed_missing"], ["Missing Sans"])
            self.assertEqual(doc["inventory"]["format"], mi.INVENTORY_FORMAT)

    def test_repository_allowlists(self):
        lists = mi.load_allowlists()
        self.assertEqual(set(lists), {"windows", "mac", "linux", "android"})
        for entries in lists.values():
            self.assertTrue(entries)
            for e in entries:
                self.assertTrue(e["name"])


class contextlib_redirect:
    """Silences stdout and stderr."""
    def __enter__(self):
        import contextlib
        self._stack = contextlib.ExitStack()
        self._stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        self._stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
        return self

    def __exit__(self, *exc):
        self._stack.close()
        return False


if __name__ == "__main__":
    unittest.main()
