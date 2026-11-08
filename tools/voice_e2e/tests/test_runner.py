# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The runner end to end against a stand-in browser (fake_browser.py) over
the real DevTools pipe: typed, and spoken through a player that hands the
stand-in what it plays. Then the report."""

import json
import os
import re
import stat
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from e2ekit import report, runner, scenarios as scen  # noqa: E402

try:
    import numpy  # noqa: F401
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False

TESTDATA = os.path.join(HERE, "testdata", "scenarios")


class FakeServer(object):
    port = 4443


def fake_browser(d):
    """An executable that starts the stand-in."""
    path = os.path.join(d, "fake-chrome")
    with open(path, "w") as f:
        f.write("#!/bin/sh\nexec %s -I %s \"$@\"\n" % (
            sys.executable, os.path.join(HERE, "fake_browser.py")))
    os.chmod(path, stat.S_IRWXU)
    return path


class Player(object):
    """Plays a step by telling the stand-in what it says."""

    def __init__(self, scs):
        self.texts = {s.id: [st.value for st in s.steps] for s in scs}
        self.played = []
        self.played_at = {}

    def play(self, wav):
        run_dir = os.path.dirname(os.path.dirname(wav))
        sid = os.path.basename(run_dir)
        i = int(re.search(r"step(\d+)\.wav$", wav).group(1)) - 1
        text = self.texts[sid][i]
        self.played.append(text)
        self.played_at.setdefault(sid, []).append(time.time())
        with open(os.path.join(run_dir, "fake-mic.txt"), "a") as f:
            f.write(text + "\n")


def by_id(run):
    return {s["id"]: s for s in run["scenarios"]}


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.scs = scen.load_dir(TESTDATA)
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name
        self.exe = fake_browser(self.d)

    def tearDown(self):
        self.tmp.cleanup()

    def check(self, run, typed):
        res = by_id(run)
        owner = res["t-owner"]
        self.assertEqual(owner["status"], "passed", owner)
        self.assertEqual([s["status"] for s in owner["steps"]], ["passed"] * 4)
        self.assertIn("open_site", owner["kinds"])
        lat = owner["steps"][2]["latency"]
        self.assertIn("action_ms", lat)
        self.assertIn("heard_ms", lat)
        self.assertGreaterEqual(lat["action_ms"], -300)
        self.assertEqual(owner["steps"][0]["intents"], ["open_site"])
        if not typed:
            self.assertEqual(owner["steps"][0]["word_errors"], [0, 2])
        self.assertEqual(res["t-cart"]["status"], "passed", res["t-cart"])
        missing = res["t-missing"]
        self.assertEqual(missing["status"], "not in grammar")
        self.assertEqual(missing["steps"][0]["missing_commands"], ["bookmark_page"])
        fails = res["t-fails"]
        self.assertEqual(fails["status"], "failed")
        self.assertIn("volume is 0.7", fails["reason"])
        self.assertEqual(fails["steps"][1]["status"], "not run")
        self.assertTrue(fails["steps"][0].get("screenshot"))
        self.assertTrue(os.path.exists(os.path.join(self.d, "report",
                                                    fails["steps"][0]["screenshot"])))
        self.assertEqual(res["t-quiet"]["status"], "passed", res["t-quiet"])
        self.assertEqual(res["t-word"]["status"], "skipped")

    def test_microphone_levels(self):
        import struct
        pcm = struct.pack("<16000h", *([0] * 16000)) + \
            struct.pack("<16000h", *([16384] + [0] * 15999))
        self.assertEqual(runner.mic_levels(pcm), [-120, -6])

    def test_levels_around_where_speech_starts(self):
        import struct
        # 2 s of silence, then speech at -6 dBFS whose first 0.3 s is held
        # back to -40 dBFS (an echo canceller's onset), from 2.0 s.
        quiet = [0] * 32000
        held = [328] * 4800
        loud = [16384] * 16000
        pcm = struct.pack("<%dh" % (len(quiet) + len(held) + len(loud)),
                          *(quiet + held + loud))
        at, levels = runner.onset_levels(pcm, 30000)
        # The onset is where it gets to -30 dBFS: after the held-back start.
        self.assertAlmostEqual(at, 2.3, places=2)
        # From 0.5 s before it: silence, the held-back start, the speech.
        self.assertEqual(levels[:6], [-120, -120, -40, -40, -40, -6])
        # Up to 2 s after it, or the dump's end (1 s after, here).
        self.assertEqual(len(levels), 15)
        # Nothing that loud near it.
        self.assertEqual(runner.onset_levels(struct.pack("<16000h", *([0] * 16000)), 8000),
                         (None, []))

    def test_a_run_without_the_loopback_is_labelled(self):
        self.assertEqual(runner.RunConfig(audio="pulse").label, "pulse/clean")
        self.assertEqual(runner.RunConfig(audio="pulse", loopback=False).label,
                         "pulse/clean-noloop")
        self.assertEqual(runner.RunConfig(audio="file", loopback=False).label,
                         "file/clean")
        self.assertEqual(
            runner.RunConfig(audio="pulse", echo_cancellation=False).label,
            "pulse/clean-noaec")

    def test_a_run_without_echo_cancellation_disables_it(self):
        cfg = runner.RunConfig(audio="pulse", echo_cancellation=False)
        r = runner.Runner(self.exe, os.path.join(self.d, "speech"),
                          os.path.join(self.d, "report"), FakeServer(), "spki", cfg,
                          log=lambda *a: None)
        self.assertIn("--disable-features=ChromeWideEchoCancellation", r.flags("log"))

    def test_the_microphone_dump_in_file_and_pulse_runs(self):
        def flags(audio, wav=None):
            r = runner.Runner(self.exe, os.path.join(self.d, "speech"),
                              os.path.join(self.d, "report"), FakeServer(), "spki",
                              runner.RunConfig(audio=audio), log=lambda *a: None)
            return r.flags(os.path.join(self.d, "run", "voice.jsonl"), wav)
        dump = "--molt-voice-test-audio=" + os.path.join(self.d, "run", runner.MIC_DUMP)
        self.assertIn(dump, flags("pulse"))
        f = flags("file", wav="/x/scenario.wav")
        self.assertIn(dump, f)
        self.assertIn("--use-file-for-fake-audio-capture=/x/scenario.wav%noloop", f)
        self.assertNotIn(dump, flags("typed"))

    def test_shields_off_disables_molt_shield(self):
        cfg = runner.RunConfig(audio="typed", settle=0.2)
        r = runner.Runner(self.exe, os.path.join(self.d, "speech"),
                          os.path.join(self.d, "report"), FakeServer(), "spki", cfg,
                          log=lambda *a: None)
        self.assertNotIn("--disable-molt-shield", r.flags("log"))
        self.assertIn("--disable-molt-shield", r.flags("log", shields=False))

    def test_typed(self):
        cfg = runner.RunConfig(audio="typed", settle=0.2)
        r = runner.Runner(self.exe, os.path.join(self.d, "speech"),
                          os.path.join(self.d, "report"), FakeServer(), "spki", cfg,
                          log=lambda *a: None)
        run = r.run(self.scs)
        self.check(run, typed=True)
        # The command went through Agent mode's page, with the wake phrase.
        log = os.path.join(self.d, "report", by_id(run)["t-owner"]["log"])
        with open(log) as f:
            heard = [json.loads(line) for line in f if '"heard"' in line]
        self.assertEqual(heard[0]["text"], "Hey Molt, open YouTube")
        self.assertEqual(heard[0]["source"], "typed")
        # And the report.
        rep = report.build([run], {"browser": self.exe})
        s = rep["runs"][0]["summary"]
        self.assertEqual((s["counted"], s["passed"], s["not_in_grammar"]), (5, 3, 1))
        self.assertFalse(rep["ok"])
        report.write(os.path.join(self.d, "report"), rep)
        with open(os.path.join(self.d, "report", "report.md")) as f:
            md = f.read()
        self.assertIn("t-fails", md)
        self.assertIn("`bookmark_page`", md)

    @unittest.skipUnless(HAVE_NUMPY, "needs numpy (the utterances' WAVs)")
    def test_spoken(self):
        from e2ekit import render
        from stub_tts import StubTts
        import voice_bench_lib as vbl
        texts = scen.utterances(self.scs)
        words = {w for t in texts + [render.WAKE_PHRASE, render.REPEAT] + render.gesture_words()
                 for w in vbl.tts_words(render.tts_text(t))}
        speech = os.path.join(self.d, "speech")
        render.render_all(StubTts(speech, words), speech, texts, voices=render.VOICES[:1],
                          speeds=(1.0,), conditions=("clean",), log=lambda *a: None)
        player = Player(self.scs)
        cfg = runner.RunConfig(audio="pulse", settle=0.5, voices=["ljspeech"], speeds=[1.0])
        r = runner.Runner(self.exe, speech, os.path.join(self.d, "report"), FakeServer(),
                          "spki", cfg, player=player, log=lambda *a: None)
        run = r.run(self.scs, modes=["wake"])
        self.check(run, typed=False)
        self.assertIn("open YouTube", player.played)
        # Each step with the wake phrase was said once voice mode waited for
        # it again (the follow-up window closed), not as a follow-up.
        with open(os.path.join(self.d, "report", by_id(run)["t-owner"]["log"])) as f:
            lines = [json.loads(line) for line in f]
        heard = [e for e in lines if e["e"] == "heard"]
        self.assertEqual(len(heard), 4)
        self.assertTrue(all(h["waiting"] for h in heard), heard)
        # And not at once: the settle time after it started waiting (its tone).
        waits = [e["t"] / 1000.0 for e in lines
                 if e["e"] == "activity" and e.get("waiting")]
        for at in player.played_at["t-owner"]:
            since = at - max(w for w in waits if w <= at)
            self.assertGreaterEqual(since, 0.45, (at, waits))
        owner = by_id(run)["t-owner"]
        self.assertEqual(owner["voice"], "ljspeech")
        # The WAV the player got starts with the wake phrase.
        wav = os.path.join(self.d, "report", "runs", "pulse-clean", "t-owner", "audio",
                           "step01.wav")
        import audio
        x, _ = audio.read_wav(wav)
        y, _ = audio.read_wav(render.clip_path(speech, "ljspeech", 1.0, "open YouTube"))
        self.assertGreater(len(x), len(y))


if __name__ == "__main__":
    unittest.main()
