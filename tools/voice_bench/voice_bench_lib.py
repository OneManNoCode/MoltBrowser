# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Shared, standard-library-only pieces of the voice bench.

- the test set (testset/utterances.tsv, testset/pages.json) and its checks;
- transcript normalization, word error rate, command and entity accuracy;
- recognizer biasing lists built from a page (hotwords);
- a SentencePiece model reader and BPE encoder (to write keywords and the
  bpe.vocab sherpa-onnx needs for hotwords, without the sentencepiece
  package);
- an ONNX metadata reader (model provenance, speaker names);
- WAV reading and writing (16-bit PCM), median and percentiles.
"""

import json
import math
import os
import re
import struct
import unicodedata
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
TESTSET_DIR = os.path.join(HERE, "testset")
CATEGORIES = ("nav", "search", "choice", "media", "read", "dictation", "chatter")

# ------------------------------------------------------------------ test set


class TestSetError(ValueError):
    pass


def load_pages(path=None):
    path = path or os.path.join(TESTSET_DIR, "pages.json")
    with open(path, encoding="utf-8") as f:
        pages = json.load(f)["pages"]
    for pid, page in pages.items():
        items = page.get("items")
        if not isinstance(items, list) or len(items) != 20:
            raise TestSetError("page %s must have 20 items" % pid)
        for it in items:
            if (not isinstance(it, list) or len(it) != 2
                    or not all(isinstance(x, str) and x for x in it)):
                raise TestSetError("page %s: bad item %r" % (pid, it))
    return pages


def parse_entity(spec):
    """'Name=alt1|alt2' -> ('Name', ['name', 'alt1', 'alt2']) normalized."""
    name, _, alts = spec.partition("=")
    name = name.strip()
    if not name:
        raise TestSetError("empty entity in %r" % spec)
    forms = [normalize(name)] + [normalize(a) for a in alts.split("|") if a.strip()]
    return name, [f for i, f in enumerate(forms) if f and f not in forms[:i]]


def load_testset(path=None, pages=None):
    """Returns the utterances as dicts; raises TestSetError on any problem."""
    path = path or os.path.join(TESTSET_DIR, "utterances.tsv")
    pages = pages if pages is not None else load_pages()
    out, seen = [], set()
    with open(path, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) != 6:
                raise TestSetError("line %d: %d fields, want 6" % (lineno, len(fields)))
            uid, cat, text, alts, ents, page = fields
            if uid in seen:
                raise TestSetError("line %d: duplicate id %s" % (lineno, uid))
            seen.add(uid)
            if cat not in CATEGORIES:
                raise TestSetError("line %d: unknown category %s" % (lineno, cat))
            if page not in pages:
                raise TestSetError("line %d: unknown page %s" % (lineno, page))
            if not normalize(text):
                raise TestSetError("line %d: empty text" % lineno)
            accepted = [normalize(text)]
            for a in alts.split("|"):
                n = normalize(a)
                if n and n not in accepted:
                    accepted.append(n)
            entities = [parse_entity(e) for e in ents.split(";") if e.strip()]
            for name, forms in entities:
                if not any(contains_words(acc, forms) for acc in accepted[:1]):
                    raise TestSetError("line %d: entity %r is not in the text" % (lineno, name))
            out.append({"id": uid, "category": cat, "text": text,
                        "accepted": accepted, "entities": entities, "page": page})
    return out


# ------------------------------------------------------------------ normalize

_ONES = ["zero", "one", "two", "three", "four", "five", "six", "seven", "eight",
         "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen",
         "sixteen", "seventeen", "eighteen", "nineteen"]
_TENS = ["", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy",
         "eighty", "ninety"]
_ORD = {"one": "first", "two": "second", "three": "third", "five": "fifth",
        "eight": "eighth", "nine": "ninth", "twelve": "twelfth"}


def number_words(n):
    """Cardinal words for 0 <= n < 10**9 (American style, no 'and')."""
    if n < 20:
        return _ONES[n]
    if n < 100:
        t, o = divmod(n, 10)
        return _TENS[t] + ("" if o == 0 else " " + _ONES[o])
    if n < 1000:
        h, r = divmod(n, 100)
        return _ONES[h] + " hundred" + ("" if r == 0 else " " + number_words(r))
    for div, word in ((10**6, "million"), (1000, "thousand")):
        if n >= div:
            q, r = divmod(n, div)
            return number_words(q) + " " + word + ("" if r == 0 else " " + number_words(r))
    raise ValueError(n)


def ordinal_words(n):
    words = number_words(n).split()
    last = words[-1]
    if last in _ORD:
        words[-1] = _ORD[last]
    elif last.endswith("y"):
        words[-1] = last[:-1] + "ieth"
    else:
        words[-1] = last + "th"
    return " ".join(words)


_REPLACEMENTS = [
    (re.compile(r"(\d+)\s*%"), lambda m: m.group(1) + " percent"),
    (re.compile(r"\b(\d{1,2}):(\d{2})\b"),
     lambda m: number_words(int(m.group(1))) + " " +
     ("" if m.group(2) == "00" else number_words(int(m.group(2))))),
    (re.compile(r"\b(\d+)(st|nd|rd|th)\b"), lambda m: ordinal_words(int(m.group(1)))),
    (re.compile(r"\b\d{1,9}\b"), lambda m: number_words(int(m.group(0)))),
]
_WORD_MAP = {"ok": "okay", "okey": "okay", "o.k.": "okay", "mr": "mister",
             "dr": "doctor", "vs": "versus"}


def normalize(text):
    """Lower case, digits as words, punctuation dropped, apostrophes removed.

    Used for references and hypotheses alike, so "Play the 4th one." and
    "play the fourth one" compare equal. Bounded work: every step is a
    single pass over a short string (test-set and recognizer text only).
    """
    s = unicodedata.normalize("NFKC", text or "").lower()
    s = s.replace("’", "'").replace("&", " and ").replace("+", " plus ")
    # "google.com" -> "google dot com"; other dots are punctuation.
    s = re.sub(r"(?<=[a-z0-9])\.(?=[a-z])", " dot ", s)
    s = re.sub(r"(?<=\d),(?=\d{3}\b)", "", s)
    for pattern, repl in _REPLACEMENTS:
        s = pattern.sub(repl, s)
    s = s.replace("'", "")
    s = re.sub(r"[^\w\s]|_", " ", s)
    words = [_WORD_MAP.get(w, w) for w in s.split()]
    return " ".join(words)


def contains_words(haystack, forms):
    """True if one of the normalized forms is a contiguous word run of
    haystack (both normalized strings)."""
    h = " " + haystack + " "
    return any(f and (" " + f + " ") in h for f in forms)


# ------------------------------------------------------------------ metrics


def edit_distance(ref, hyp):
    """Word-level Levenshtein distance between two word lists."""
    prev = list(range(len(hyp) + 1))
    for i, r in enumerate(ref, 1):
        cur = [i] + [0] * len(hyp)
        for j, h in enumerate(hyp, 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (r != h))
        prev = cur
    return prev[-1]


def score_utterance(utt, hyp_text):
    """Scores one transcript against an utterance from load_testset()."""
    hyp = normalize(hyp_text)
    hw = hyp.split()
    best = None
    for acc in utt["accepted"]:
        ref = acc.split()
        e = edit_distance(ref, hw)
        if best is None or e * best[1] < best[0] * len(ref):
            best = (e, len(ref))
    ents = [contains_words(hyp, forms) for _, forms in utt["entities"]]
    return {"errors": best[0], "ref_words": best[1],
            "command_ok": hyp in utt["accepted"],
            "entities_ok": sum(ents), "entities": len(ents), "hyp": hyp}


def aggregate(scores):
    n = len(scores)
    words = sum(s["ref_words"] for s in scores)
    ents = sum(s["entities"] for s in scores)
    return {
        "n": n,
        "wer": (sum(s["errors"] for s in scores) / words) if words else None,
        "command_acc": (sum(s["command_ok"] for s in scores) / n) if n else None,
        "entity_acc": (sum(s["entities_ok"] for s in scores) / ents) if ents else None,
        "entities": ents,
    }


def median(xs):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    m = len(xs) // 2
    return xs[m] if len(xs) % 2 else (xs[m - 1] + xs[m]) / 2


def percentile(xs, p):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    k = (len(xs) - 1) * p / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


# ------------------------------------------------------------------ biasing


def spoken_number(token):
    """'1985' -> 'nineteen eighty five', '2022' -> 'twenty twenty two',
    '20th' -> 'twentieth', '4' -> 'four'; other tokens unchanged."""
    m = re.fullmatch(r"(\d+)(st|nd|rd|th)", token, re.I)
    if m and len(m.group(1)) <= 6:
        return ordinal_words(int(m.group(1)))
    if not token.isdigit() or len(token) > 9:
        return token
    n = int(token)
    if len(token) == 4 and 1100 <= n <= 2099 and not 2000 <= n <= 2009:
        hi, lo = divmod(n, 100)
        tail = "hundred" if lo == 0 else (("oh " + _ONES[lo]) if lo < 10 else number_words(lo))
        return number_words(hi) + " " + tail
    return number_words(n)


def page_phrases(page):
    """Biasing phrases for a page: each title split at its separators
    ("Queen - Bohemian Rhapsody (Live Aid 1985)" gives "Queen",
    "Bohemian Rhapsody", "Live Aid nineteen eighty five"), and each
    channel. Letters, digits as words, and apostrophes only; de-duplicated
    case-insensitively, in page order."""
    out, seen = [], set()
    for title, channel in page["items"]:
        for text in re.split(r"\s[-–|:]\s|[()\[\]!,?]", title) + [channel]:
            words = re.sub(r"[^A-Za-z0-9' ]+", " ", text).split()
            phrase = " ".join(spoken_number(w) for w in words)
            if phrase and phrase.lower() not in seen:
                seen.add(phrase.lower())
                out.append(phrase)
    return out


def hotwords_arg(phrases, case="upper"):
    """The per-stream hotwords string sherpa-onnx takes: phrases separated
    by '/'. case: 'upper' for upper-case BPE vocabularies (icefall
    LibriSpeech/GigaSpeech models), 'lower' or 'keep' otherwise."""
    fix = {"upper": str.upper, "lower": str.lower}.get(case, lambda s: s)
    out = []
    for p in phrases:
        words = fix(p).split()
        if case == "upper":
            # The vocabulary has no digits or lower case: a word it can't
            # spell would make sherpa-onnx drop the whole list.
            words = [w for w in words if re.fullmatch(r"[A-Z']+", w)]
        if words:
            out.append(" ".join(words))
    return "/".join(out)


# ------------------------------------------------------------------ protobuf


def _varint(buf, pos):
    result = shift = 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not b & 0x80:
            return result, pos
        shift += 7
        if shift > 63:
            raise ValueError("bad varint")


def iter_fields(buf):
    """Yields (field_number, wire_type, value) of one protobuf message.
    Length-delimited values are memoryview slices."""
    pos, end = 0, len(buf)
    while pos < end:
        key, pos = _varint(buf, pos)
        field, wt = key >> 3, key & 7
        if wt == 0:
            val, pos = _varint(buf, pos)
        elif wt == 1:
            val, pos = buf[pos:pos + 8], pos + 8
        elif wt == 2:
            n, pos = _varint(buf, pos)
            if pos + n > end:
                raise ValueError("truncated field %d" % field)
            val, pos = buf[pos:pos + n], pos + n
        elif wt == 5:
            val, pos = buf[pos:pos + 4], pos + 4
        else:
            raise ValueError("unsupported wire type %d" % wt)
        yield field, wt, val


def read_sentencepiece(path):
    """[(piece, score, type)] from a SentencePiece ModelProto (bpe.model)."""
    with open(path, "rb") as f:
        buf = memoryview(f.read())
    pieces = []
    for field, wt, val in iter_fields(buf):
        if field != 1 or wt != 2:
            continue
        piece, score, typ = "", 0.0, 1
        for f2, w2, v2 in iter_fields(val):
            if f2 == 1 and w2 == 2:
                piece = bytes(v2).decode("utf-8")
            elif f2 == 2 and w2 == 5:
                score = struct.unpack("<f", bytes(v2))[0]
            elif f2 == 3 and w2 == 0:
                typ = v2
        pieces.append((piece, score, typ))
    return pieces


def vocab_from_tokens(tokens_path):
    """[(piece, score, type)] from a sherpa-onnx tokens.txt, with
    score = -id. For a SentencePiece BPE model the ids follow the score
    order, so BPE merges come out the same as with the real scores."""
    out = []
    with open(tokens_path, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").rsplit(" ", 1)
            if len(parts) != 2:
                continue
            piece, idx = parts
            out.append((piece, -float(idx), 1))
    return out


def write_bpe_vocab(pieces, path):
    with open(path, "w", encoding="utf-8") as f:
        for piece, score, _ in pieces:
            f.write("%s\t%s\n" % (piece, repr(float(score))))


def bpe_encode_word(word, scores):
    """SentencePiece BPE for one word: start from characters (with the
    leading U+2581), repeatedly merge the adjacent pair whose merge is the
    highest-scoring piece. scores: {piece: score}."""
    syms = list("▁" + word)
    while len(syms) > 1:
        best, best_i = None, -1
        for i in range(len(syms) - 1):
            s = scores.get(syms[i] + syms[i + 1])
            if s is not None and (best is None or s > best):
                best, best_i = s, i
        if best is None:
            break
        syms[best_i:best_i + 2] = [syms[best_i] + syms[best_i + 1]]
    return syms


def bpe_encode(text, pieces):
    scores = {p: s for p, s, t in pieces if t in (1, 4)}  # NORMAL, USER_DEFINED
    out = []
    for w in text.split():
        out.extend(bpe_encode_word(w, scores))
    return out


# ------------------------------------------------------------------ ONNX


def onnx_metadata(path, max_bytes=1 << 31):
    """{key: value} of an ONNX model's metadata_props (ModelProto field 14),
    reading only the top-level fields (the graph is skipped, not parsed)."""
    meta = {}
    with open(path, "rb") as f:
        size = os.fstat(f.fileno()).st_size
        if size > max_bytes:
            return meta
        pos = 0
        while pos < size:
            f.seek(pos)
            head = f.read(20)
            if not head:
                break
            key, p = _varint(head, 0)
            field, wt = key >> 3, key & 7
            if wt == 0:
                _, p2 = _varint(head, p)
                pos += p2
                continue
            if wt != 2:
                skip = {1: 8, 5: 4}.get(wt)
                if skip is None:
                    break
                pos += p + skip
                continue
            n, p2 = _varint(head, p)
            start = pos + p2
            if field == 14:
                f.seek(start)
                entry = memoryview(f.read(n))
                k = v = ""
                for f2, w2, v2 in iter_fields(entry):
                    if f2 == 1 and w2 == 2:
                        k = bytes(v2).decode("utf-8", "replace")
                    elif f2 == 2 and w2 == 2:
                        v = bytes(v2).decode("utf-8", "replace")
                meta[k] = v
            pos = start + n
    return meta


def onnx_op_types(path):
    """{(domain, op_type): count} of an ONNX model's main graph (subgraphs
    of If/Loop are not walked). For ORT-format (.ort) files returns None."""
    if path.endswith(".ort"):
        return None
    with open(path, "rb") as f:
        buf = memoryview(f.read())
    ops = {}
    for field, wt, val in iter_fields(buf):
        if field != 7 or wt != 2:
            continue
        for f2, w2, node in iter_fields(val):
            if f2 != 1 or w2 != 2:
                continue
            op, domain = "", ""
            for f3, w3, v3 in iter_fields(node):
                if f3 == 4 and w3 == 2:
                    op = bytes(v3).decode("utf-8", "replace")
                elif f3 == 7 and w3 == 2:
                    domain = bytes(v3).decode("utf-8", "replace")
            ops[(domain, op)] = ops.get((domain, op), 0) + 1
    return ops


# ------------------------------------------------------------------ WAV


def read_wav_info(path):
    with wave.open(path, "rb") as w:
        return w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()


def write_wav_int16(path, samples_int16_bytes, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(samples_int16_bytes)


def read_jsonl(path):
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("{"):
                out.append(json.loads(line))
    return out
