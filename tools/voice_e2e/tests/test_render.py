# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The renderer: the same inputs give the same files (and the same mixes),
whose hashes the manifest records; what the voices read; the lexicon."""

import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from e2ekit import render  # noqa: E402
import voice_bench_lib as vbl  # noqa: E402 (render put tools/voice_bench on the path)

try:
    import numpy  # noqa: F401
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False

TEXTS = ["open YouTube", "set the volume to 50 percent", "speed 1.5"]


class PureTest(unittest.TestCase):
    def test_clip_ids_are_stable(self):
        self.assertEqual(render.clip_id("open YouTube"), render.clip_id("open YouTube"))
        self.assertNotEqual(render.clip_id("open YouTube"), render.clip_id("open Youtube"))
        self.assertRegex(render.clip_id("x"), r"^u[0-9a-f]{16}$")

    def test_what_the_voice_reads(self):
        self.assertEqual(render.tts_text("set the volume to 50 percent"),
                         "set the volume to fifty percent.")
        self.assertEqual(render.tts_text("speed 1.5"), "speed one point five.")
        self.assertEqual(render.tts_text("go back 30 seconds"), "go back thirty seconds.")
        self.assertEqual(render.tts_text("Is it?"), "Is it?")
        self.assertEqual(render.tts_text("two-slice"), "two slice.")

    def test_voice_keys(self):
        self.assertEqual(render.voice_key("ljspeech", 1.0), "ljspeech@1")
        self.assertEqual(render.voice_key("ljspeech", 1.25), "ljspeech@1.25")

    def test_lexicon(self):
        with tempfile.TemporaryDirectory() as d:
            base = os.path.join(d, "base.txt")
            extra = os.path.join(d, "extra.txt")
            with open(base, "w") as f:
                f.write("# base\nopen || o p\nyoutube || j u\n")
            with open(extra, "w") as f:
                f.write("# extra\nmolt || m o\nopen || X\n")
            text, words = render.merged_lexicon(base, [extra])
            self.assertEqual(words, {"open", "youtube", "molt"})
            # The extra entry comes first, so it wins.
            self.assertLess(text.index("open || X"), text.index("open || o p"))
            self.assertEqual(render.missing_words(["Hey Molt, open YouTube", "go"], words),
                             ["go", "hey"])

    def test_the_kits_words_are_all_known(self):
        # Every word the scenarios say is in the merged lexicon (with the
        # voice bench's real lexicon only where it was fetched): here, the
        # kit's own and the bench's extra entries hold no duplicates.
        for path in (render.KIT_LEXICON, render.BENCH_LEXICON):
            self.assertTrue(vbl.read_new_lexicon(path), path)

    def test_compare_hashes(self):
        manifest = {"files": {"clips/v@1/a.wav": "1", "clips/v@1/b.wav": "2"},
                    "mix_pcm_sha256": {"music10/v@1/a": "3"}, "texts": {"a": "x"}}
        same, different, missing, names = render.compare_hashes(
            manifest, {"clips/v@1/a.wav": "1", "clips/v@1/b.wav": "9",
                       "clips/v@1/c.wav": "4", "mix:music10/v@1/a": "3"})
        self.assertEqual((same, different, missing), (2, 1, 1))
        self.assertEqual(names, ["clips/v@1/b.wav"])
        digests = render.set_digests(manifest)
        self.assertEqual(sorted(digests), ["clips/v@1", "mix:music10/v@1", "texts"])
        changed = dict(manifest, files={"clips/v@1/a.wav": "1", "clips/v@1/b.wav": "5"})
        self.assertEqual(render.compare_hashes(changed, digests)[:3], (2, 1, 0))


@unittest.skipUnless(HAVE_NUMPY, "needs numpy (tools/voice_bench/requirements.txt)")
class DeterminismTest(unittest.TestCase):
    def render(self, out):
        from stub_tts import StubTts
        words = {w for t in TEXTS + ["Hey Molt, mute.", render.WAKE_PHRASE, render.REPEAT] +
                 render.gesture_words()
                 for w in vbl.tts_words(render.tts_text(t))}
        tts = StubTts(out, words)
        voices = render.VOICES[:2]
        return render.render_all(tts, out, TEXTS, ["Hey Molt, mute."], voices=voices,
                                 speeds=(1.0, 1.25), conditions=("clean", "music10"),
                                 log=lambda *a: None), tts

    def test_same_inputs_same_files(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            ma, tts = self.render(a)
            mb, _ = self.render(b)
            self.assertEqual(ma["files"], mb["files"])
            self.assertEqual(ma["mix_pcm_sha256"], mb["mix_pcm_sha256"])
            digests = render.set_digests(ma)
            self.assertEqual(digests, render.set_digests(mb))
            self.assertEqual(sorted(digests), [
                "clips/libritts-m@1", "clips/libritts-m@1.25", "clips/ljspeech@1",
                "clips/ljspeech@1.25", "mix:music10/libritts-m@1",
                "mix:music10/libritts-m@1.25", "mix:music10/ljspeech@1",
                "mix:music10/ljspeech@1.25", "page", "texts"])
            self.assertEqual(render.compare_hashes(mb, digests), (10, 0, 0, []))
            n = len(TEXTS) + 2 + len(render.gesture_words())
            self.assertEqual(len([k for k in ma["files"] if k.startswith("clips/")]), 4 * n)
            self.assertEqual(len([k for k in ma["files"] if k.startswith("page/")]), 1)
            self.assertEqual(len(ma["mix_pcm_sha256"]), 4 * n)
            # A second render in the same place renders nothing again.
            calls = len(tts.calls)
            again, tts2 = self.render(a)
            self.assertEqual(tts2.calls, [])
            self.assertEqual(again["files"], ma["files"])
            self.assertTrue(calls)
            with open(os.path.join(a, "manifest.json")) as f:
                on_disk = json.load(f)
            self.assertEqual(on_disk["files"], ma["files"])
            self.assertEqual(on_disk["noise_scale"], "1e-30")
            # Each clip is 16 kHz mono 16-bit, speech at -26 dBFS.
            import audio
            x, rate = audio.read_wav(render.clip_path(a, "ljspeech", 1.0, "open YouTube"))
            self.assertEqual(rate, 16000)
            self.assertAlmostEqual(20 * numpy.log10(audio.rms(x)), -26.0, delta=0.2)

    def test_the_file_captures_wav(self):
        from e2ekit import audio_path
        import audio
        import wave
        with tempfile.TemporaryDirectory() as a:
            self.render(a)
            pieces = []
            for i, (text, wake) in enumerate((("open YouTube", True), ("speed 1.5", False))):
                out = os.path.join(a, "step%d.wav" % i)
                speech = audio_path.utterance_wav(a, out, "ljspeech", 1.0, text, wake)
                self.assertGreater(speech, 0.2)
                pieces.append(out)
            first = audio.read_wav(pieces[0])[0]
            plain = audio.read_wav(render.clip_path(a, "ljspeech", 1.0, "open YouTube"))[0]
            self.assertGreater(len(first), len(plain))  # the wake phrase first
            wav = os.path.join(a, "scenario.wav")
            times = audio_path.scenario_wav(wav, pieces, "music10", 6.0, 9.0, "seed")
            self.assertAlmostEqual(times[0][0], 6.0)
            self.assertAlmostEqual(times[1][0], times[0][1] + 9.0, places=3)
            with wave.open(wav) as w:
                self.assertEqual((w.getframerate(), w.getnchannels()), (44100, 2))
                # A gap after the last one too, for its outcome.
                self.assertAlmostEqual(w.getnframes() / 44100.0, times[1][1] + 9.0 + 2.0,
                                       delta=0.01)
            again = os.path.join(a, "again.wav")
            audio_path.scenario_wav(again, pieces, "music10", 6.0, 9.0, "seed")
            self.assertEqual(render.sha256_file(wav), render.sha256_file(again))
            # A gap of its own for a step (file_gap); the first's is not used.
            own = os.path.join(a, "own.wav")
            times = audio_path.scenario_wav(own, pieces, "music10", 6.0, [30.0, 4.0], "seed")
            self.assertAlmostEqual(times[0][0], 6.0)
            self.assertAlmostEqual(times[1][0], times[0][1] + 4.0, places=3)
            with self.assertRaises(ValueError):
                audio_path.scenario_wav(own, pieces, "music10", 6.0, [9.0], "seed")

    def test_mixes(self):
        with tempfile.TemporaryDirectory() as a:
            self.render(a)
            clip = render.clip_path(a, "ljspeech", 1.0, "open YouTube")
            p1, span = render.mix(clip, "music10", "seed")
            p2, _ = render.mix(clip, "music10", "seed")
            p3, _ = render.mix(clip, "music10", "other")
            self.assertEqual(p1, p2)
            self.assertNotEqual(p1, p3)
            self.assertAlmostEqual(span[0], render.LEAD_S, places=3)
            self.assertEqual(len(p1) // 2, int(round((span[1] + render.TRAIL_S) * 16000)))


if __name__ == "__main__":
    unittest.main()
