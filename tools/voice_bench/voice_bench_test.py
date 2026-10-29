#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Offline tests for the voice bench kit (no downloads, no engines).

  python3 -I tools/voice_bench/voice_bench_test.py -v
"""

import io
import json
import os
import struct
import sys
import tarfile
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import catalog  # noqa: E402
import fetch  # noqa: E402
import voice_bench_lib as vbl  # noqa: E402


def _varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def _field(num, wt, payload):
    key = _varint((num << 3) | wt)
    if wt == 2:
        return key + _varint(len(payload)) + payload
    return key + payload


class NormalizeTest(unittest.TestCase):
    def test_numbers_and_punctuation(self):
        n = vbl.normalize
        self.assertEqual(n("Play the 4th one."), "play the fourth one")
        self.assertEqual(n("Number 12!"), "number twelve")
        self.assertEqual(n("number twelve"), "number twelve")
        self.assertEqual(n("Set the volume to 50%."), "set the volume to fifty percent")
        self.assertEqual(n("Jump to 2:30"), "jump to two thirty")
        self.assertEqual(n("OK, see you later"), "okay see you later")
        self.assertEqual(n("go to Google.com"), "go to google dot com")
        self.assertEqual(n("What's on the screen?"), "whats on the screen")
        self.assertEqual(n("Anti-Hero"), "anti hero")
        self.assertEqual(n("Simon & Garfunkel"), "simon and garfunkel")
        self.assertEqual(n("the 20th one"), "the twentieth one")
        self.assertEqual(n("the 22nd"), "the twenty second")
        self.assertEqual(n("1,000 results"), "one thousand results")
        self.assertEqual(n("  "), "")

    def test_number_words(self):
        self.assertEqual(vbl.number_words(0), "zero")
        self.assertEqual(vbl.number_words(19), "nineteen")
        self.assertEqual(vbl.number_words(40), "forty")
        self.assertEqual(vbl.number_words(101), "one hundred one")
        self.assertEqual(vbl.number_words(2025), "two thousand twenty five")
        self.assertEqual(vbl.ordinal_words(1), "first")
        self.assertEqual(vbl.ordinal_words(12), "twelfth")
        self.assertEqual(vbl.ordinal_words(30), "thirtieth")
        self.assertEqual(vbl.spoken_number("1985"), "nineteen eighty five")
        self.assertEqual(vbl.spoken_number("1901"), "nineteen oh one")
        self.assertEqual(vbl.spoken_number("2005"), "two thousand five")
        self.assertEqual(vbl.spoken_number("Queen"), "Queen")


class ScoreTest(unittest.TestCase):
    def setUp(self):
        self.pages = vbl.load_pages()
        self.utts = {u["id"]: u for u in vbl.load_testset(pages=self.pages)}

    def test_edit_distance(self):
        self.assertEqual(vbl.edit_distance("a b c".split(), "a b c".split()), 0)
        self.assertEqual(vbl.edit_distance("a b c".split(), "a c".split()), 1)
        self.assertEqual(vbl.edit_distance("a b".split(), "x a b y".split()), 2)
        self.assertEqual(vbl.edit_distance([], "a".split()), 1)

    def test_choice_by_name(self):
        u = self.utts["choice-03"]
        s = vbl.score_utterance(u, "Play the live one by Queen.")
        self.assertTrue(s["command_ok"])
        self.assertEqual((s["errors"], s["entities_ok"]), (0, 1))
        s = vbl.score_utterance(u, "play the life one by queen")
        self.assertFalse(s["command_ok"])
        self.assertEqual((s["errors"], s["entities_ok"]), (1, 1))
        s = vbl.score_utterance(u, "play the live one by green")
        self.assertEqual(s["entities_ok"], 0)

    def test_alternatives(self):
        self.assertTrue(vbl.score_utterance(self.utts["media-14"], "Fullscreen.")["command_ok"])
        self.assertTrue(vbl.score_utterance(self.utts["choice-02"], "Number 12.")["command_ok"])
        s = vbl.score_utterance(self.utts["search-05"], "Find Blinding Lights by The Weekend.")
        self.assertTrue(s["command_ok"])
        self.assertEqual(s["entities_ok"], 2)

    def test_aggregate(self):
        u = self.utts["nav-17"]
        a = vbl.aggregate([vbl.score_utterance(u, "go back"),
                           vbl.score_utterance(u, "go bag")])
        self.assertEqual(a["n"], 2)
        self.assertAlmostEqual(a["wer"], 0.25)
        self.assertAlmostEqual(a["command_acc"], 0.5)
        self.assertIsNone(a["entity_acc"])


class TestSetTest(unittest.TestCase):
    def test_shape(self):
        pages = vbl.load_pages()
        utts = vbl.load_testset(pages=pages)
        self.assertGreaterEqual(len(utts), 290)
        cats = {c: sum(1 for u in utts if u["category"] == c) for c in vbl.CATEGORIES}
        self.assertTrue(all(v >= 20 for v in cats.values()), cats)
        # Every referenced name on a choice line is on its page.
        for u in utts:
            if u["category"] != "choice":
                continue
            text = vbl.normalize(" ".join(t for it in pages[u["page"]]["items"] for t in it))
            for name, forms in u["entities"]:
                self.assertTrue(vbl.contains_words(text, forms), (u["id"], name))

    def test_bad_lines_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "u.tsv")
            for bad in ("x\tnav\tgo back\t\t\n",                       # 5 fields
                        "x\tnope\tgo back\t\t\tnews_home\n",          # category
                        "x\tnav\tgo back\t\t\tno_page\n",             # page
                        "x\tnav\tgo back\t\tQueen\tnews_home\n"):      # entity not in text
                with open(p, "w") as f:
                    f.write(bad)
                with self.assertRaises(vbl.TestSetError):
                    vbl.load_testset(p)

    def test_hotwords(self):
        pages = vbl.load_pages()
        ph = vbl.page_phrases(pages["yt_bohemian"])
        self.assertIn("Bohemian Rhapsody", ph)
        self.assertIn("Live Aid nineteen eighty five", ph)
        self.assertEqual(len(ph), len({p.lower() for p in ph}))
        arg = vbl.hotwords_arg(ph, "upper")
        self.assertNotIn("1985", arg)
        for phrase in arg.split("/"):
            for w in phrase.split():
                self.assertRegex(w, r"^[A-Z']+$")
        shop = vbl.hotwords_arg(vbl.page_phrases(pages["shop_headphones"]), "upper")
        self.assertNotIn("1000XM5", shop)
        self.assertNotIn(":", vbl.hotwords_arg(ph, "keep"))


class ProtoTest(unittest.TestCase):
    def test_sentencepiece_and_bpe(self):
        pieces = [("<unk>", 0.0, 2), ("▁H", -1.0, 1), ("E", -2.0, 1), ("Y", -3.0, 1),
                  ("▁HE", -4.0, 1), ("▁HEY", -5.0, 1), ("M", -6.0, 1),
                  ("O", -7.0, 1), ("L", -8.0, 1), ("T", -9.0, 1), ("▁", -10.0, 1),
                  ("LT", -11.0, 1), ("▁M", -12.0, 1)]
        blob = b""
        for p, s, t in pieces:
            inner = _field(1, 2, p.encode()) + _field(2, 5, struct.pack("<f", s)) + \
                _field(3, 0, _varint(t))
            blob += _field(1, 2, inner)
        blob += _field(2, 2, b"\x08\x01")  # trainer spec, ignored
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "bpe.model")
            with open(path, "wb") as f:
                f.write(blob)
            got = vbl.read_sentencepiece(path)
        self.assertEqual([g[0] for g in got], [p for p, _, _ in pieces])
        self.assertAlmostEqual(got[5][1], -5.0)
        self.assertEqual(vbl.bpe_encode("HEY MOLT", got), ["▁HEY", "▁M", "O", "LT"])

    def test_vocab_from_tokens(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "tokens.txt")
            with open(path, "w", encoding="utf-8") as f:
                f.write("<blk> 0\n▁THE 1\nS 2\n")
            v = vbl.vocab_from_tokens(path)
            out = os.path.join(d, "bpe.vocab")
            vbl.write_bpe_vocab(v, out)
            with open(out, encoding="utf-8") as f:
                self.assertEqual(f.read().splitlines()[1], "▁THE\t-1.0")

    def test_onnx_metadata(self):
        entry = _field(1, 2, b"model_type") + _field(2, 2, b"zipformer")
        blob = _field(1, 0, _varint(9)) + _field(7, 2, b"\x00" * 300) + _field(14, 2, entry) + \
            _field(14, 2, _field(1, 2, b"n") + _field(2, 2, b"4"))
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "m.onnx")
            with open(path, "wb") as f:
                f.write(blob)
            self.assertEqual(vbl.onnx_metadata(path), {"model_type": "zipformer", "n": "4"})

    def test_onnx_op_types(self):
        def node(op, domain=b""):
            return _field(1, 2, _field(4, 2, op) + (_field(7, 2, domain) if domain else b""))
        # Nodes around a big initializer (field 5), which is skipped unread.
        graph = node(b"Conv") + _field(5, 2, b"\x01" * 200000) + \
            node(b"Relu", b"com.microsoft") + node(b"Conv") + _field(2, 2, b"main")
        blob = _field(1, 0, _varint(9)) + _field(7, 2, graph) + \
            _field(14, 2, _field(1, 2, b"k") + _field(2, 2, b"v"))
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "m.onnx")
            with open(path, "wb") as f:
                f.write(blob)
            self.assertEqual(vbl.onnx_op_types(path),
                             {("", "Conv"): 2, ("com.microsoft", "Relu"): 1})
            with open(path, "wb") as f:
                f.write(blob[:-10])
            with self.assertRaises(ValueError):
                vbl.onnx_op_types(path)
        self.assertIsNone(vbl.onnx_op_types("m.ort"))


class FetchTest(unittest.TestCase):
    def test_manifest(self):
        arts = fetch.load_manifest()
        ids = {a["id"] for a in arts}
        for a in arts:
            self.assertRegex(a["sha256"], r"^[0-9a-f]{64}$")
            self.assertTrue(a["url"].startswith("https://"))
            self.assertNotIn("huggingface.co", a["url"])
            self.assertGreater(a["size"], 0)
            self.assertNotEqual(a["license"], "TBD", a["id"])
        for group in (catalog.OFFLINE, catalog.STREAM, catalog.KWS, catalog.TTS):
            for e in group.values():
                self.assertIn(e["artifact"], ids)
        self.assertIn(catalog.VAD["artifact"], ids)
        for v in catalog.VOICES + [catalog.BROWSER_VOICE]:
            self.assertIn(v[1], catalog.TTS)

    def _tar(self, members):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as t:
            for name, kind in members:
                info = tarfile.TarInfo(name)
                if kind == "file":
                    data = b"x"
                    info.size = 1
                    t.addfile(info, io.BytesIO(data))
                elif kind == "symlink":
                    info.type = tarfile.SYMTYPE
                    info.linkname = "/etc/passwd"
                    t.addfile(info)
        return buf.getvalue()

    def test_extract_refuses_escapes(self):
        with tempfile.TemporaryDirectory() as d:
            arc = os.path.join(d, "a.tar")
            with open(arc, "wb") as f:
                f.write(self._tar([("top/ok.txt", "file"), ("top/link", "symlink")]))
            out = os.path.join(d, "out")
            os.makedirs(out)
            n = fetch.extract(arc, out, {"id": "t"})
            self.assertEqual(n, 1)
            self.assertTrue(os.path.exists(os.path.join(out, "ok.txt")))
            self.assertFalse(os.path.lexists(os.path.join(out, "link")))
            with open(arc, "wb") as f:
                f.write(self._tar([("top/../../evil.txt", "file")]))
            out2 = os.path.join(d, "out2")
            os.makedirs(out2)
            with self.assertRaises(RuntimeError):
                fetch.extract(arc, out2, {"id": "t"})
            self.assertFalse(os.path.exists(os.path.join(d, "evil.txt")))

    def test_members_filter(self):
        with tempfile.TemporaryDirectory() as d:
            arc = os.path.join(d, "a.tar")
            with open(arc, "wb") as f:
                f.write(self._tar([("top/a.int8.onnx", "file"), ("top/a.onnx", "file"),
                                   ("top/tokens.txt", "file")]))
            out = os.path.join(d, "out")
            os.makedirs(out)
            fetch.extract(arc, out, {"id": "t", "members": ["*int8*", "*tokens*"]})
            self.assertEqual(sorted(os.listdir(out)), ["a.int8.onnx", "tokens.txt"])

    def test_bad_download_deleted(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "src.bin")
            with open(src, "wb") as f:
                f.write(b"hello")
            a = {"id": "t", "url": "file://" + src, "size": 5, "sha256": "0" * 64}
            dest = os.path.join(d, "dest.bin")
            with self.assertRaises(RuntimeError):
                fetch.download(a, dest)
            self.assertFalse(os.path.exists(dest))
            self.assertFalse(os.path.exists(dest + ".part"))


class LexiconTest(unittest.TestCase):
    def test_espeak_to_misaki(self):
        self.assertEqual(vbl.espeak_to_misaki("vˈ ɪ d ɪ o ʊ z"), "v ˈ ɪ d ɪ O z")
        self.assertEqual(vbl.espeak_to_misaki("d ʒ ˈ iː m e ɪ l"), "ʤ ˈ i m A l")
        self.assertEqual(vbl.espeak_to_misaki("bˈ ɜː d"), "b ˈ ɜ ɹ d")
        self.assertEqual(vbl.espeak_to_misaki("bˈ ʌ ɾ ɚ"), "b ˈ ʌ ɾ ə ɹ")

    def test_extra_lexicon_format(self):
        path = os.path.join(HERE, "testset", "lexicon-extra.txt")
        with open(path, encoding="utf-8") as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                word, sep, phones = line.partition("||")
                self.assertEqual(sep, "||", line)
                self.assertEqual(word.strip(), word.strip().lower())
                self.assertTrue(phones.strip())


class ReportTest(unittest.TestCase):
    def test_json_lines(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "r.jsonl")
            with open(p, "w") as f:
                f.write(json.dumps({"event": "run"}) + "\nnoise\n" +
                        json.dumps({"event": "end"}) + "\n")
            self.assertEqual([r["event"] for r in vbl.read_jsonl(p)], ["run", "end"])
        self.assertEqual(vbl.median([3, 1, 2]), 2)
        self.assertEqual(vbl.median([4, 1, 2, 3]), 2.5)
        self.assertIsNone(vbl.median([None]))
        self.assertAlmostEqual(vbl.percentile([1, 2, 3, 4, 5], 90), 4.6)


if __name__ == "__main__":
    unittest.main()
