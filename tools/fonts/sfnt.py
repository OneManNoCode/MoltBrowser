# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""A small, defensive reader for OpenType / TrueType font files.

Standard library only (Python 3.8+), so that tools/fonts/ runs on a fresh
machine. Used by pin_fonts.py (the faces of MoltNet's bundled fonts) and by
molt_font_inventory.py (the fonts a fresh OS install has).

Reads what the tools need and nothing else: the faces of a font or font
collection (TrueType, CFF, TTC/OTC), their names (name table, all platforms
and languages), weight, width and slope (OS/2, head), version (name ID 5,
head.fontRevision), variation axes and named instances (fvar), and whether
a code point is mapped (cmap formats 4 and 12).

Every offset and length is checked against the data, every loop is bounded
by the data's size or a fixed limit, and malformed data raises SfntError and
nothing else, so a hostile file can't crash or stall the caller.
"""

import struct

# Limits far above any real font, below what would let a crafted header make
# the reader do much work.
MAX_TABLES = 512
MAX_COLLECTION_FACES = 256
MAX_NAME_RECORDS = 8192
MAX_AXES = 64
MAX_INSTANCES = 1024
MAX_CMAP_SUBTABLES = 64
MAX_CMAP_GROUPS = 1 << 20

TAG_TRUETYPE = b"\x00\x01\x00\x00"
TAG_OTTO = b"OTTO"
TAG_TRUE = b"true"
TAG_TTCF = b"ttcf"

# name IDs.
NAME_FAMILY = 1
NAME_SUBFAMILY = 2
NAME_FULL = 4
NAME_VERSION = 5
NAME_POSTSCRIPT = 6
NAME_TYPOGRAPHIC_FAMILY = 16
NAME_TYPOGRAPHIC_SUBFAMILY = 17
NAME_WWS_FAMILY = 21

# Windows language ids of English (the names fonts give as "the" name).
_EN_US = 0x0409


class SfntError(ValueError):
    """The data is not a font this reader understands, or is malformed."""


def _u16(data, offset):
    if offset < 0 or offset + 2 > len(data):
        raise SfntError("read past the end at %d" % offset)
    return struct.unpack_from(">H", data, offset)[0]


def _i16(data, offset):
    if offset < 0 or offset + 2 > len(data):
        raise SfntError("read past the end at %d" % offset)
    return struct.unpack_from(">h", data, offset)[0]


def _u32(data, offset):
    if offset < 0 or offset + 4 > len(data):
        raise SfntError("read past the end at %d" % offset)
    return struct.unpack_from(">I", data, offset)[0]


def _fixed(data, offset):
    """A 16.16 fixed-point number."""
    if offset < 0 or offset + 4 > len(data):
        raise SfntError("read past the end at %d" % offset)
    return struct.unpack_from(">i", data, offset)[0] / 65536.0


def _slice(data, offset, length):
    if offset < 0 or length < 0 or offset + length > len(data):
        raise SfntError("range %d+%d is outside the data (%d bytes)" %
                        (offset, length, len(data)))
    return data[offset:offset + length]


def face_offsets(data):
    """The offsets of the faces' table directories: one for a single font,
    one per face for a collection."""
    tag = bytes(_slice(data, 0, 4))
    if tag == TAG_TTCF:
        count = _u32(data, 8)
        if count == 0 or count > MAX_COLLECTION_FACES:
            raise SfntError("collection with %d faces" % count)
        return [_u32(data, 12 + 4 * i) for i in range(count)]
    if tag in (TAG_TRUETYPE, TAG_OTTO, TAG_TRUE):
        return [0]
    raise SfntError("not an OpenType font (tag %r)" % tag)


def table_directory(data, offset):
    """{tag (str): (offset, length)} of the face whose directory is at
    `offset`."""
    tag = bytes(_slice(data, offset, 4))
    if tag not in (TAG_TRUETYPE, TAG_OTTO, TAG_TRUE):
        raise SfntError("bad face tag %r at %d" % (tag, offset))
    count = _u16(data, offset + 4)
    if count > MAX_TABLES:
        raise SfntError("%d tables" % count)
    tables = {}
    for i in range(count):
        record = offset + 12 + 16 * i
        name = bytes(_slice(data, record, 4)).decode("latin-1")
        table_offset = _u32(data, record + 8)
        length = _u32(data, record + 12)
        _slice(data, table_offset, length)  # In range?
        tables[name] = (table_offset, length)
    return tables, tag


def _decode_name(platform, encoding, raw):
    if platform == 3 or platform == 0:
        if len(raw) % 2:
            raw = raw[:-1]
        return raw.decode("utf-16-be", errors="replace")
    if platform == 1 and encoding == 0:
        return raw.decode("mac_roman", errors="replace")
    return None  # Other legacy encodings: not needed.


def read_names(data, table):
    """[(name_id, platform, language, text)] of a name table."""
    offset, length = table
    name = _slice(data, offset, length)
    count = _u16(name, 2)
    storage = _u16(name, 4)
    if count > MAX_NAME_RECORDS:
        raise SfntError("%d name records" % count)
    records = []
    for i in range(count):
        record = 6 + 12 * i
        platform = _u16(name, record)
        encoding = _u16(name, record + 2)
        language = _u16(name, record + 4)
        name_id = _u16(name, record + 6)
        text_length = _u16(name, record + 8)
        text_offset = _u16(name, record + 10)
        try:
            raw = bytes(_slice(name, storage + text_offset, text_length))
        except SfntError:
            continue  # One bad record doesn't make the font unreadable.
        text = _decode_name(platform, encoding, raw)
        if text is None:
            continue
        records.append((name_id, platform, language, text))
    return records


def best_name(records, name_id):
    """The English name with `name_id`: Windows en-US, else any Windows or
    Unicode name, else Mac Roman; None if there is none."""
    ranked = []
    for rid, platform, language, text in records:
        if rid != name_id or not text:
            continue
        if platform == 3 and language == _EN_US:
            rank = 0
        elif platform == 3:
            rank = 2
        elif platform == 0:
            rank = 1
        elif platform == 1 and language == 0:
            rank = 1
        else:
            rank = 3
        ranked.append((rank, text))
    if not ranked:
        return None
    ranked.sort(key=lambda r: r[0])
    return ranked[0][1]


def all_names(records, name_id):
    """Every distinct name with `name_id` (localized ones included), in a
    stable order."""
    seen = []
    for rid, _, _, text in records:
        if rid == name_id and text and text not in seen:
            seen.append(text)
    return seen


def _read_fvar(data, table, records):
    offset, length = table
    fvar = _slice(data, offset, length)
    axes_offset = _u16(fvar, 4)
    axis_count = _u16(fvar, 8)
    axis_size = _u16(fvar, 10)
    instance_count = _u16(fvar, 12)
    instance_size = _u16(fvar, 14)
    if axis_count > MAX_AXES or instance_count > MAX_INSTANCES:
        raise SfntError("fvar with %d axes, %d instances" %
                        (axis_count, instance_count))
    if axis_size < 20 or instance_size < 4 + 4 * axis_count:
        raise SfntError("fvar record sizes %d/%d" % (axis_size, instance_size))
    axes = []
    for i in range(axis_count):
        a = axes_offset + i * axis_size
        tag = bytes(_slice(fvar, a, 4)).decode("latin-1")
        axes.append({
            "tag": tag,
            "min": _fixed(fvar, a + 4),
            "default": _fixed(fvar, a + 8),
            "max": _fixed(fvar, a + 12),
        })
    instances = []
    base = axes_offset + axis_count * axis_size
    for i in range(instance_count):
        r = base + i * instance_size
        subfamily_id = _u16(fvar, r)
        coords = {axes[j]["tag"]: _fixed(fvar, r + 4 + 4 * j)
                  for j in range(axis_count)}
        postscript = None
        if instance_size >= 6 + 4 * axis_count:
            ps_id = _u16(fvar, r + 4 + 4 * axis_count)
            if ps_id not in (0, 0xFFFF):
                postscript = best_name(records, ps_id)
        instances.append({
            "subfamily": best_name(records, subfamily_id),
            "postscript": postscript,
            "coordinates": coords,
        })
    return axes, instances


def read_face(data, directory_offset, index=0):
    """A dict describing the face whose table directory is at
    `directory_offset` (index = its index in a collection)."""
    tables, tag = table_directory(data, directory_offset)
    if "name" not in tables:
        raise SfntError("no name table")
    records = read_names(data, tables["name"])
    family = best_name(records, NAME_FAMILY)
    typographic_family = best_name(records, NAME_TYPOGRAPHIC_FAMILY)
    fs_selection = None
    if "OS/2" in tables:
        os2_offset, os2_length = tables["OS/2"]
        fs_selection = _u16(_slice(data, os2_offset, os2_length), 62)
    # The family Skia's Fontations typefaces report (getFamilyName): the WWS
    # family unless OS/2 says the face is WWS-only, else the typographic
    # family, else the family (skia/src/ports/fontations/src/names.rs).
    wws_family = None
    if fs_selection is None or not fs_selection & 0x0100:
        wws_family = best_name(records, NAME_WWS_FAMILY)
    face = {
        "index": index,
        "outlines": "cff" if tag == TAG_OTTO else "truetype",
        "family": family,
        "typographic_family": typographic_family,
        "skia_family": wws_family or typographic_family or family,
        "subfamily": best_name(records, NAME_SUBFAMILY),
        "typographic_subfamily": best_name(records,
                                           NAME_TYPOGRAPHIC_SUBFAMILY),
        "full_name": best_name(records, NAME_FULL),
        "postscript_name": best_name(records, NAME_POSTSCRIPT),
        "version": best_name(records, NAME_VERSION),
        "family_names": all_names(records, NAME_FAMILY) + [
            n for n in all_names(records, NAME_TYPOGRAPHIC_FAMILY)
            if n not in all_names(records, NAME_FAMILY)],
        "weight": 400,
        "width": 5,
        "italic": False,
        "font_revision": None,
        "axes": [],
        "instances": [],
        "tables": sorted(tables),
    }
    if "OS/2" in tables:
        os2_offset, os2_length = tables["OS/2"]
        os2 = _slice(data, os2_offset, os2_length)
        face["weight"] = _u16(os2, 4)
        face["width"] = _u16(os2, 6)
        face["italic"] = bool(fs_selection & 0x0001 or fs_selection & 0x0200)
    if "head" in tables:
        head_offset, head_length = tables["head"]
        head = _slice(data, head_offset, head_length)
        face["font_revision"] = round(_fixed(head, 4), 5)
        if "OS/2" not in tables:
            mac_style = _u16(head, 44)
            face["italic"] = bool(mac_style & 0x02)
            face["weight"] = 700 if mac_style & 0x01 else 400
    if "fvar" in tables:
        face["axes"], face["instances"] = _read_fvar(data, tables["fvar"],
                                                     records)
    return face


def read_faces(data):
    """Every face of a font file's data (one, or one per collection face)."""
    return [read_face(data, offset, i)
            for i, offset in enumerate(face_offsets(data))]


def assemble(tag, tables):
    """A one-face sfnt made of `tables` ({tag: bytes}), the face tag `tag`
    (b"OTTO", b"\\x00\\x01\\x00\\x00", b"true"): how the no-install
    collectors' raw tables are read back with read_face()."""
    if tag not in (TAG_TRUETYPE, TAG_OTTO, TAG_TRUE):
        raise SfntError("bad face tag %r" % tag)
    if len(tables) > MAX_TABLES:
        raise SfntError("%d tables" % len(tables))
    tags = sorted(tables)
    out = bytearray(tag + struct.pack(">HHHH", len(tags), 0, 0, 0))
    offset = 12 + 16 * len(tags)
    blobs = bytearray()
    for t in tags:
        name = t.encode("latin-1")
        if len(name) != 4:
            raise SfntError("bad table tag %r" % t)
        blob = tables[t]
        out += name + struct.pack(">III", 0, offset + len(blobs), len(blob))
        blobs += blob + b"\0" * (-len(blob) % 4)
    return bytes(out + blobs)


# Style words a legacy (GDI / DirectWrite "Win32") family name leaves out.
_RIBBI = frozenset(("regular", "bold", "italic", "oblique"))


def legacy_family_names(face):
    """The family names a system may list a face under besides its own: for
    a variable font, one per named instance (the typographic family plus the
    instance's non-RIBBI style words, as DirectWrite's legacy names do:
    "Bahnschrift Light SemiCondensed")."""
    family = face.get("typographic_family") or face.get("family")
    if not family:
        return []
    names = []
    for instance in face.get("instances", ()):
        style = instance.get("subfamily") or ""
        kept = [w for w in style.split() if w.lower() not in _RIBBI]
        if kept:
            name = "%s %s" % (family, " ".join(kept))
            if name not in names:
                names.append(name)
    return names


def _cmap_subtable(data, tables):
    """(format, data, offset) of the best Unicode cmap subtable: a full
    repertoire one (3,10 / 0,4 / 0,6, format 12) before a BMP one (3,1 /
    0,3, format 4)."""
    if "cmap" not in tables:
        raise SfntError("no cmap table")
    offset, length = tables["cmap"]
    cmap = _slice(data, offset, length)
    count = _u16(cmap, 2)
    if count > MAX_CMAP_SUBTABLES:
        raise SfntError("%d cmap subtables" % count)
    best = None
    for i in range(count):
        platform = _u16(cmap, 4 + 8 * i)
        encoding = _u16(cmap, 6 + 8 * i)
        sub = _u32(cmap, 8 + 8 * i)
        fmt = _u16(cmap, sub)
        if fmt == 12 and ((platform, encoding) in ((3, 10), (0, 4), (0, 6))):
            rank = 0
        elif fmt == 4 and ((platform, encoding) in ((3, 1), (0, 3), (0, 4))):
            rank = 1
        else:
            continue
        if best is None or rank < best[0]:
            best = (rank, fmt, sub)
    if best is None:
        raise SfntError("no Unicode cmap subtable")
    return best[1], cmap, best[2]


def coverage(data, directory_offset=0):
    """A function codepoint -> bool: whether the face maps it to a glyph."""
    tables, _ = table_directory(data, directory_offset)
    fmt, cmap, sub = _cmap_subtable(data, tables)
    if fmt == 12:
        groups = _u32(cmap, sub + 12)
        if groups > MAX_CMAP_GROUPS:
            raise SfntError("%d cmap groups" % groups)
        ranges = []
        for g in range(groups):
            base = sub + 16 + 12 * g
            start = _u32(cmap, base)
            end = _u32(cmap, base + 4)
            glyph = _u32(cmap, base + 8)
            if end < start:
                continue
            ranges.append((start, end, glyph))
        ranges.sort()

        def covered12(cp):
            lo, hi = 0, len(ranges)
            while lo < hi:
                mid = (lo + hi) // 2
                if ranges[mid][1] < cp:
                    lo = mid + 1
                else:
                    hi = mid
            if lo < len(ranges) and ranges[lo][0] <= cp <= ranges[lo][1]:
                return ranges[lo][2] + (cp - ranges[lo][0]) != 0
            return False
        return covered12

    seg_count = _u16(cmap, sub + 6) // 2
    ends = sub + 14
    starts = ends + 2 * seg_count + 2
    deltas = starts + 2 * seg_count
    range_offsets = deltas + 2 * seg_count
    segments = []
    for s in range(seg_count):
        segments.append((_u16(cmap, starts + 2 * s), _u16(cmap, ends + 2 * s),
                         _i16(cmap, deltas + 2 * s),
                         _u16(cmap, range_offsets + 2 * s),
                         range_offsets + 2 * s))

    def covered4(cp):
        if cp > 0xFFFF:
            return False
        for start, end, delta, range_offset, where in segments:
            if start <= cp <= end:
                if range_offset == 0:
                    return (cp + delta) & 0xFFFF != 0
                glyph_at = where + range_offset + 2 * (cp - start)
                try:
                    glyph = _u16(cmap, glyph_at)
                except SfntError:
                    return False
                return glyph != 0 and (glyph + delta) & 0xFFFF != 0
        return False
    return covered4
