# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Expectations against a world the test makes up."""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from e2ekit import expect  # noqa: E402


class World(object):
    def __init__(self):
        self._url = "https://www.youtube.com/results?search_query=Bohemian%20Rhapsody"
        self.values = {"kit.state.geo": "granted"}
        self._media = {"paused": False, "muted": False, "volume": 0.8, "rate": 1.5,
                       "time": 61.0, "src": "https://www.youtube.com/_kit/song.wav",
                       "captions": True, "fullscreen": False}
        self.events = []
        self.page = []
        self.tabs = 1
        self.windows = 1
        self.files = set()
        self.targets = ["https://news.test/", "chrome://molt-ai-chat/"]

    def url(self):
        return self._url

    def evaluate(self, expression):
        if expression not in self.values:
            raise ValueError("ReferenceError")
        return self.values[expression]

    def media(self):
        return self._media

    def log_events(self, since):
        return [e for e in self.events if e["ts"] >= since]

    def tab_count(self):
        return self.tabs

    def window_count(self):
        return self.windows

    def downloaded(self, name):
        return name in self.files

    def page_events(self, since):
        return [e for e in self.page if e["t"] / 1000.0 >= since]

    def target_urls(self):
        return self.targets


def ok(e, w, since=0.0):
    return expect.check(e, w, since)[0]


class CheckTest(unittest.TestCase):
    def test_url(self):
        w = World()
        self.assertTrue(ok({"host": "www.youtube.com"}, w))
        self.assertTrue(ok({"path": "/results"}, w))
        self.assertTrue(ok({"query": {"search_query": "Bohemian Rhapsody"}}, w))
        self.assertFalse(ok({"query": {"search_query": "Queen"}}, w))
        # Any of a list.
        self.assertTrue(ok({"query": {"search_query": ["Bohemian rapsody",
                                                       "Bohemian Rhapsody"]}}, w))
        self.assertFalse(ok({"query": {"search_query": ["Queen", "Freddie"]}}, w))
        self.assertTrue(ok({"url_has": "search_query="}, w))
        self.assertFalse(ok({"url": "https://www.youtube.com/"}, w))
        okay, detail = expect.check({"path": "/watch"}, w, 0)
        self.assertFalse(okay)
        self.assertIn("results", detail)

    def test_js(self):
        w = World()
        self.assertTrue(ok({"js": "kit.state.geo", "is": "granted"}, w))
        self.assertFalse(ok({"js": "kit.state.geo", "is": "denied"}, w))
        okay, detail = expect.check({"js": "nope"}, w, 0)
        self.assertFalse(okay)
        self.assertIn("ReferenceError", detail)

    def test_media(self):
        w = World()
        self.assertTrue(ok({"media": {"paused": False, "volume": [0.72, 1.0], "rate": 1.5,
                                      "time_min": 55, "captions": True,
                                      "src_has": "song.wav"}}, w))
        self.assertTrue(ok({"media": {"volume": 0.81}}, w))
        okay, detail = expect.check({"media": {"paused": True, "fullscreen": True}}, w, 0)
        self.assertFalse(okay)
        self.assertIn("paused is false", detail)
        self.assertIn("fullscreen is false", detail)
        w._media = None
        self.assertFalse(ok({"media": {"paused": False}}, w))

    def test_media_seeked(self):
        w = World()
        w._media = {"paused": False, "rate": 1.0, "time": 80.0}
        before = expect.snapshot(w)
        before["t"] -= 6.0  # the step began 6 s ago, at 80 s
        # Back 30 s: 80 + 6 played - 30.
        w._media = {"paused": False, "rate": 1.0, "time": 56.0}
        self.assertTrue(expect.check({"media": {"seeked": [-45, -15]}}, w, 0, before)[0])
        # Played on, not moved: 86.
        w._media = {"paused": False, "rate": 1.0, "time": 86.0}
        okay, detail = expect.check({"media": {"seeked": [-45, -15]}}, w, 0, before)
        self.assertFalse(okay)
        self.assertIn("moved +0 s", detail)
        # Paused when the step began: no time played.
        w._media = {"paused": True, "rate": 1.0, "time": 80.0}
        before = expect.snapshot(w)
        before["t"] -= 6.0
        w._media = {"paused": True, "rate": 1.0, "time": 50.0}
        self.assertTrue(expect.check({"media": {"seeked": [-45, -15]}}, w, 0, before)[0])
        # At twice the speed: 12 s played in 6.
        w._media = {"paused": False, "rate": 2.0, "time": 80.0}
        before = expect.snapshot(w)
        before["t"] -= 6.0
        w._media = {"paused": False, "rate": 2.0, "time": 62.0}
        self.assertTrue(expect.check({"media": {"seeked": [-45, -15]}}, w, 0, before)[0])
        # No player as the step began.
        self.assertFalse(expect.check({"media": {"seeked": [-45, -15]}}, w, 0, None)[0])

    def test_said_and_heard(self):
        w = World()
        w.events = [{"e": "said", "ts": 10.0, "text": "20 results.  1: Queen"},
                    {"e": "said", "ts": 11.0, "text": "Say a number or a name."},
                    {"e": "heard", "ts": 9.0, "text": "search for queen", "intent": "search"}]
        self.assertTrue(ok({"said": "Say a number or a name."}, w))
        self.assertTrue(ok({"said_has": "20 results. 1: Queen"}, w))
        self.assertTrue(ok({"said_re": "^20 results\\."}, w))
        # ^ at any line: a line said before it (the last step's answer).
        w.events.insert(0, {"e": "said", "ts": 9.5, "text": "YouTube is open."})
        self.assertTrue(ok({"said_re": "^20 results\\."}, w))
        self.assertTrue(ok({"said_re": "^Say a number or a name\\.$"}, w))
        self.assertFalse(ok({"said_re": "^1: Queen"}, w))
        self.assertFalse(ok({"said": "Paused."}, w))
        self.assertTrue(ok({"said_none": "Paused."}, w))
        self.assertFalse(ok({"said_none": "number"}, w))
        # Only since the step began.
        self.assertFalse(ok({"said_has": "20 results"}, w, since=10.5))
        self.assertTrue(ok({"heard_intent": "search"}, w))
        self.assertFalse(ok({"heard_intent": "search"}, w, since=9.5))

    def test_prompts(self):
        w = World()
        w.events = [{"e": "prompt", "ts": 1.0, "op": "open", "kind": "permission", "id": 3},
                    {"e": "confirm", "ts": 2.0, "op": "open", "what": "press Send"}]
        self.assertTrue(ok({"prompt": "open", "kind": "permission"}, w))
        self.assertFalse(ok({"prompt": "open", "kind": "alert"}, w))
        self.assertFalse(ok({"prompt": "close"}, w))
        self.assertTrue(ok({"confirm": "open"}, w))

    def test_counts_files_targets(self):
        w = World()
        w.tabs, w.windows = 2, 1
        w.files = {"report.txt"}
        self.assertTrue(ok({"tabs": 2}, w))
        self.assertFalse(ok({"windows": 2}, w))
        self.assertTrue(ok({"file": "report.txt"}, w))
        self.assertFalse(ok({"file": "other.txt"}, w))
        self.assertTrue(ok({"target": "molt-ai-chat"}, w))
        self.assertFalse(ok({"target": "chrome://bookmarks"}, w))

    def test_applies(self):
        self.assertTrue(expect.applies({"host": "a"}, True))
        self.assertTrue(expect.applies({"host": "a"}, False))
        self.assertFalse(expect.applies({"host": "a", "typed": False}, True))
        self.assertFalse(expect.applies({"host": "a", "typed": True}, False))


class QuietTest(unittest.TestCase):
    def test_nothing_happened(self):
        w = World()
        before = expect.snapshot(w)
        w.page = [{"type": "play", "t": 5000, "trusted": True}]
        okay, detail = expect.quiet_check(before, w, 1.0)
        self.assertTrue(okay, detail)

    def test_what_counts_as_acting(self):
        w = World()
        before = expect.snapshot(w)
        w.page = [{"type": "click", "t": 5000, "trusted": True}]
        self.assertFalse(expect.quiet_check(before, w, 1.0)[0])
        w.page = [{"type": "click", "t": 5000, "trusted": False}]
        self.assertTrue(expect.quiet_check(before, w, 1.0)[0])
        w.page = []
        w._url = "https://www.youtube.com/"
        okay, detail = expect.quiet_check(before, w, 1.0)
        self.assertFalse(okay)
        self.assertIn("went to", detail)
        w._url = before["url"]
        w.tabs = 2
        self.assertFalse(expect.quiet_check(before, w, 1.0)[0])
        w.tabs = 1
        w.events = [{"e": "prompt", "ts": 2.0, "op": "close", "id": 1}]
        self.assertFalse(expect.quiet_check(before, w, 1.0)[0])

    def test_heard_is_reported_not_failed(self):
        w = World()
        before = expect.snapshot(w)
        w.events = [{"e": "heard", "ts": 2.0, "text": "hey molt mute", "intent": "mute"}]
        okay, detail = expect.quiet_check(before, w, 1.0)
        self.assertTrue(okay)
        self.assertIn("mute", detail)


if __name__ == "__main__":
    unittest.main()
