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

    def test_utterances_are_stable_and_unique(self):
        u = scen.utterances(self.scs)
        self.assertEqual(len(u), len(set(u)))
        self.assertEqual(u, scen.utterances(scen.load_dir(SHIPPED)))
        self.assertIn("open YouTube", u)

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

    def test_good_one(self):
        sc = scen.parse_scenario(one(), "x.json")
        self.assertEqual(sc.steps[0].text, "open YouTube")
        self.assertEqual(sc.modes, ["wake"])
        self.assertTrue(sc.counts)

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
        self.bad(one(steps=[{"say": "x", "expect": [{"media": {"volume": [0.9, 0.1]}}]}]),
                 "low above high")
        self.bad(one(steps=[{"say": "x", "expect": [{"prompt": "maybe"}]}]), "prompt takes")
        self.bad(one(steps=[{"say": "x", "expect": [{"tabs": -1}]}]), "count")
        self.bad(one(steps=[{"say": "x", "timeout": 999}]), "timeout")
        self.bad(one(requires=["telepathy"]), "requires takes")

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
