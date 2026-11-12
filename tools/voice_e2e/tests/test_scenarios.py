# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The scenario files: the shipped ones load and cover TOUCHLESS_BROWSING.md
§2's core tasks, and bad ones are refused with where and why."""

import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from e2ekit import scenarios as scen  # noqa: E402

SHIPPED = os.path.join(os.path.dirname(HERE), "scenarios")
CORE = ("go", "read", "choose", "act", "media", "browser", "recover")


def one(**kw):
    sc = {"id": "t-one", "title": "A test", "task": "go",
          "steps": [{"say": "open YouTube", "expect": [{"host": "www.youtube.com"}]}]}
    sc.update(kw)
    return sc


class ShippedTest(unittest.TestCase):
    def setUp(self):
        self.scs = scen.load_dir(SHIPPED)

    def test_at_least_fifty_tasks_over_every_core_task(self):
        counted = [s for s in self.scs if s.counts]
        self.assertGreaterEqual(len(counted), 50)
        tasks = {s.task for s in counted}
        for t in CORE + ("prompts", "security", "owner"):
            self.assertIn(t, tasks)

    def test_the_owners_scenario_runs_with_hey_molt_and_always_listening(self):
        owner = [s for s in self.scs if s.id == "owner-song"]
        self.assertEqual(len(owner), 1)
        self.assertEqual(owner[0].modes, ["wake", "always"])
        said = [st.value for st in owner[0].steps]
        self.assertEqual(said[:4], ["open YouTube", "search for Bohemian Rhapsody",
                                    "play the live one by Queen", "the Live Aid one"])

    def test_the_owners_scenarios_are_spoken_by_one_voice(self):
        # The owner's decision: the voice the recognizer hears right on
        # every step, so that they measure voice mode.
        owner = [s for s in self.scs if s.task == "owner"]
        self.assertEqual({s.id for s in owner}, {"owner-song", "owner-song-fourth"})
        self.assertEqual({s.voice for s in owner}, {"libritts-m"})
        self.assertEqual({s.voice for s in self.scs if s.task != "owner"}, {None})

    def test_the_owners_song_runs_at_normal_speed(self):
        # The owner's decision: at 1.25x the recognizer cut "the Live Aid
        # one" ("The Live A", "The Live AOEN") in always-listening runs.
        # The variant by number keeps turning through the speeds.
        speeds = {s.id: s.speed for s in self.scs if s.task == "owner"}
        self.assertEqual(speeds, {"owner-song": 1.0, "owner-song-fourth": None})
        self.assertEqual({s.speed for s in self.scs if s.task != "owner"}, {None})

    def test_the_owners_song_says_hey_molt_over_the_playing_video(self):
        # The owner's decision: over a playing video, always-listening mode
        # takes a command without "Hey Molt" only when it heard it surely,
        # so that a video's own sound can't give commands.
        owner = [s for s in self.scs if s.id == "owner-song"][0]
        self.assertEqual([st.value for st in owner.steps if st.wake], ["louder", "pause"])
        # Only there: the steps before the video plays, and after it is
        # paused, are said without it in always-listening mode.
        self.assertEqual({s.id for s in self.scs if any(st.wake for st in s.steps)},
                         {"owner-song"})

    def test_speeds_are_the_renderers(self):
        from e2ekit import render
        self.assertEqual(scen.SPEEDS, render.SPEEDS)

    def test_voice_names_are_the_renderers(self):
        from e2ekit import render
        self.assertEqual(set(scen.VOICE_NAMES), {v[0] for v in render.VOICES})

    def test_this_rounds_commands_are_named(self):
        needs = {n for s in self.scs for n in s.needs()}
        for n in ("set_volume", "speed_up", "normal_speed", "captions_on", "undo",
                  "bookmark_page", "list_history", "reopen_tab", "list_downloads",
                  "open_settings", "new_window", "new_window",
                  "new_window", "open_ai_chat", "open_menu", "next_match",
                  "start_over"):
            self.assertIn(n, needs)

    def test_prompts_and_security_scenarios(self):
        ids = {s.id for s in self.scs}
        for i in ("prompt-permission-allow", "prompt-download", "prompt-dialog-confirm",
                  "sec-video-says-commands", "sec-replayed-word",
                  "sec-dialog-imitates-prompt", "sec-trap-history"):
            self.assertIn(i, ids)
        replay = [s for s in self.scs if s.id == "sec-replayed-word"][0]
        self.assertIn("pulse", replay.all_requirements())

    def test_security_scenarios_checks(self):
        # The steps whose failure is "acted" (Step.guards()): the ones that
        # watch for nothing happening. The user's own word is not one.
        guards = {s.id: [st.index for st in s.steps if st.guards()]
                  for s in self.scs if s.task == "security"}
        self.assertEqual(guards["sec-replayed-word"], [3])
        self.assertEqual(guards["sec-dialog-imitates-prompt"], [1, 3])
        self.assertEqual(guards["sec-video-says-commands"], [1])
        # None: every step is its check.
        self.assertEqual(guards["sec-trap-history"], [])

    def test_utterances_are_stable_and_unique(self):
        u = scen.utterances(self.scs)
        self.assertEqual(len(u), len(set(u)))
        self.assertEqual(u, scen.utterances(scen.load_dir(SHIPPED)))
        self.assertIn("open YouTube", u)

    def test_phase2s_thirty_tasks(self):
        # TOUCHLESS_BROWSING.md §10: 30 multi-step tasks (mail, a cart up to
        # the confirmation, a form), every irreversible step asked about.
        p2 = [s for s in self.scs if s.task == "phase2"]
        self.assertEqual(len(p2), 30)
        self.assertTrue(all(len(s.steps) >= 3 for s in p2))
        host = lambda s: s.start["url"].split("/")[2]
        self.assertGreaterEqual(sum(1 for s in p2 if host(s) == "mail.test" and not s.start.get("tabs")), 10)
        self.assertGreaterEqual(sum(1 for s in p2 if host(s) == "shop.test" and not s.start.get("tabs")), 10)
        self.assertGreaterEqual(sum(1 for s in p2 if host(s) == "forms.test" and not s.start.get("tabs")), 7)
        self.assertGreaterEqual(sum(1 for s in p2 if s.start.get("tabs")), 3)

    def test_phase2_asks_before_every_irreversible_step(self):
        # The words that make a click ask first, as the browser has them.
        import re
        src = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))),
                           "src", "chrome", "browser", "molt_ai", "voice",
                           "voice_controller_logic.cc")
        with open(src) as f:
            text = f.read()
        block = text[text.index("kConfirmWords[] = {"):]
        block = block[:block.index("};")]
        words = set(re.findall(r'"([a-z]+)"', block))
        self.assertIn("send", words)
        for s in (s for s in self.scs if s.task == "phase2"):
            for i, st in enumerate(s.steps):
                said = st.value.lower().split() if st.action == "say" else []
                irreversible = said[:1] == ["click"] and bool(words & set(said[1:]))
                asked = any(e.get("confirm") == "open" for e in st.expect)
                self.assertEqual(irreversible, asked, "%s step %d" % (s.id, i + 1))
                if asked:
                    nxt = s.steps[i + 1]
                    self.assertTrue(nxt.answer and nxt.value in ("yes", "no"),
                                    "%s step %d" % (s.id, i + 2))
                    # Nothing has happened yet: the page is where it was.
                    self.assertTrue(any("path" in e for e in st.expect), s.id)

    def test_the_models_tasks(self):
        # Each needs the model; a planner request is no command the grammar
        # knows, and one that orders or deletes is asked about, then a "no"
        # leaves the page where it was.
        model = [s for s in self.scs if s.task == "model"]
        self.assertGreaterEqual(len(model), 4)
        for s in model:
            self.assertIn("model", s.all_requirements(), s.id)
            self.assertTrue(s.id.startswith("model-"), s.id)
            first = s.steps[0]
            if s.id.startswith("model-plan-"):
                self.assertIn({"heard_intent": "unknown"}, first.expect, s.id)
                self.assertFalse(first.needs, s.id)
            if any(e.get("confirm") == "open" for e in first.expect):
                nxt = s.steps[1]
                self.assertTrue(nxt.answer and nxt.value == "no", s.id)
                self.assertTrue(any("path" in e and e.get("quiet") for e in nxt.expect),
                                s.id)
        asks = {s.id for s in model
                if any(e.get("confirm") == "open" for e in s.steps[0].expect)}
        self.assertEqual(asks, {"model-plan-order-asks", "model-plan-delete-asks"})

    def test_every_start_url_is_a_fixture_or_blank(self):
        from e2ekit import fixtures
        import urllib.parse
        for s in self.scs:
            for url in [s.start.get("url")] + s.start.get("tabs", []):
                if url and url != "about:blank":
                    host = urllib.parse.urlsplit(url).hostname
                    self.assertIn(host, fixtures.SITES, s.id)
                    path, _ = fixtures.resolve(host, urllib.parse.urlsplit(url).path)
                    self.assertTrue(path, "%s: %s" % (s.id, url))

    def test_select(self):
        self.assertEqual({s.id for s in scen.select(self.scs, ["go-reload"])}, {"go-reload"})
        media = scen.select(self.scs, ["media-"])
        self.assertTrue(media and all(s.id.startswith("media-") for s in media))
        self.assertTrue(all(s.task == "security" for s in scen.select(self.scs, tasks=["security"])))


class RefusedTest(unittest.TestCase):
    def bad(self, obj, needle):
        with self.assertRaises(scen.ScenarioError) as cm:
            scen.parse_scenario(obj, "x.json")
        self.assertIn(needle, str(cm.exception))

    def test_shields_off(self):
        sc = scen.parse_scenario(
            one(start={"url": "https://news.test/", "shields": False}), "x.json")
        self.assertIs(sc.start["shields"], False)

    def test_query_alternatives(self):
        sc = scen.parse_scenario(
            one(steps=[{"say": "x", "expect": [{"query": {"q": ["a", "b"]}}]}]), "x.json")
        self.assertEqual(sc.steps[0].expect[0]["query"]["q"], ["a", "b"])

    def test_good_one(self):
        sc = scen.parse_scenario(one(), "x.json")
        self.assertEqual(sc.steps[0].text, "open YouTube")
        self.assertEqual(sc.modes, ["wake"])
        self.assertTrue(sc.counts)
        self.assertIsNone(sc.voice)
        self.assertEqual(scen.parse_scenario(one(voice="libritts-f"), "x.json").voice,
                         "libritts-f")

    def test_bad_ones(self):
        self.bad(one(id="Bad Id"), "id must be")
        self.bad(one(task="dance"), "task must be")
        self.bad(one(colour="red"), "unknown keys")
        self.bad(one(modes=["sleep"]), "modes takes")
        self.bad(one(steps=[]), "steps must be")
        self.bad(one(start={"url": "http://news.test/"}), "start.url must be https")
        self.bad(one(steps=[{"say": "x", "wait": 2, "expect": []}]), "does one of")
        self.bad(one(steps=[{"say": ""}]), "non-empty")
        self.bad(one(steps=[{"replay": "nothing"}]), "replay names an earlier step")
        self.bad(one(steps=[{"wait": 3}]), "wait step needs an expectation")
        self.bad(one(steps=[{"say": "x", "needs": ["Open Site"]}]), "command names")
        self.bad(one(steps=[{"say": "x", "expect": [{"host": "a", "path": "/"}]}]),
                 "exactly one")
        self.bad(one(steps=[{"say": "x", "expect": [{"said_re": "("}]}]), "said_re")
        self.bad(one(steps=[{"say": "x", "expect": [{"media": {"loud": True}}]}]),
                 "media takes")
        self.bad(one(steps=[{"say": "x", "expect": [{"media": {"seeked": [-15, -45]}}]}]),
                 "seeked")
        self.bad(one(steps=[{"say": "x", "expect": [{"media": {"seeked": -30}}]}]),
                 "seeked")
        self.bad(one(steps=[{"say": "x", "expect": [{"media": {"volume": [0.9, 0.1]}}]}]),
                 "low above high")
        self.bad(one(steps=[{"say": "x", "expect": [{"prompt": "maybe"}]}]), "prompt takes")
        self.bad(one(steps=[{"say": "x", "expect": [{"query": {"q": 3}}]}]), "query takes")
        self.bad(one(steps=[{"say": "x", "expect": [{"query": {"q": []}}]}]), "query takes")
        self.bad(one(steps=[{"say": "x", "expect": [{"query": {"q": ["a", 1]}}]}]),
                 "query takes")
        self.bad(one(steps=[{"say": "x", "expect": [{"tabs": -1}]}]), "count")
        self.bad(one(steps=[{"say": "x", "timeout": 999}]), "timeout")
        self.bad(one(requires=["telepathy"]), "requires takes")
        self.bad(one(voice="alexa"), "voice must be one of")
        self.bad(one(voice=["libritts-m"]), "voice must be one of")
        for speed in (1.5, "1.0", True, [1.0]):
            self.bad(one(speed=speed), "speed must be one of")
        self.bad(one(start={"url": "https://news.test/", "shields": "off"}),
                 "start.shields takes")
        self.bad(one(steps=[{"say": "x", "file_gap": 6}]), "file_gap is for")
        self.bad(one(steps=[{"say": "x", "wake": "yes"}]), "wake takes true or false")
        self.bad(one(steps=[{"say": "x", "answer": True, "wake": True}]), "wake is for")
        self.bad(one(steps=[{"wait": 2, "wake": True, "expect": [{"no_action": True}]}]),
                 "wake is for")
        self.bad(one(steps=[{"say": "x"}, {"say": "y", "file_gap": 99}]), "file_gap")

    def test_wake(self):
        sc = scen.parse_scenario(one(steps=[{"say": "x"}, {"say": "y", "wake": True}]),
                                 "x.json")
        self.assertEqual([s.wake for s in sc.steps], [False, True])

    def test_file_gap(self):
        sc = scen.parse_scenario(one(steps=[{"say": "x"}, {"say": "y", "file_gap": 6}]),
                                 "x.json")
        self.assertEqual([s.file_gap for s in sc.steps], [None, 6.0])

    def test_word_and_replay_need_live_audio(self):
        sc = scen.parse_scenario(one(steps=[
            {"say": "click share my location"},
            {"say_word": True, "name": "w1"},
            {"replay": "w1", "expect": [{"js": "1", "quiet": 2}]}]), "x.json")
        self.assertIn("pulse", sc.steps[1].requires)
        self.assertIn("microphone", sc.steps[2].requires)
        self.assertEqual(scen.utterances([sc]), ["click share my location"])

    def test_files(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "a.json"), "w") as f:
                json.dump({"scenarios": [one()]}, f)
            with open(os.path.join(d, "b.json"), "w") as f:
                json.dump({"scenarios": [one()]}, f)
            with self.assertRaises(scen.ScenarioError) as cm:
                scen.load_dir(d)
            self.assertIn("also in a.json", str(cm.exception))
            with open(os.path.join(d, "b.json"), "w") as f:
                f.write("{not json")
            with self.assertRaises(scen.ScenarioError):
                scen.load_dir(d)


if __name__ == "__main__":
    unittest.main()
