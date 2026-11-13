# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The test log reader: the lines voice_test_log.cc writes (the same as
voice_test_log_lines_unittest.cc checks), read as they are appended, and
hostile lines skipped."""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from e2ekit import testlog  # noqa: E402

# Lines as voice_test_log_lines_unittest.cc has TestLogLines make them.
LINES = [
    '{"t":7000,"w":2,"e":"start","kinds":["unknown","open_site"]}',
    '{"t":8000,"w":2,"e":"heard","text":"open \\"YouTube\\"","source":"mic","clean":true,'
    '"intent":"open_site"}',
    '{"t":9000,"w":2,"e":"heard","text":"[word]","source":"typed","clean":false,'
    '"intent":"unknown"}',
    '{"t":10000,"w":2,"e":"said","text":"To allow it, say the word on your screen."}',
    '{"t":11000,"w":1,"e":"activity","activity":"listening","mic":true,"waiting":true}',
    '{"t":12000,"w":1,"e":"prompt","op":"open","id":4,"kind":"permission"}',
    '{"t":13000,"w":1,"e":"confirm","op":"open","what":"press Buy now"}',
    '{"t":14000,"w":1,"e":"planner","step":2,"action":"CLICK","target":"button '
    '\\"Delete\\"","ok":false,"note":"Element not found"}',
]


class TestLogTest(unittest.TestCase):
    def test_reads_appended_lines(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "voice.jsonl")
            log = testlog.TestLog(path)
            self.assertEqual(log.events(), [])
            with open(path, "w") as f:
                f.write("\n".join(LINES[:2]) + "\n" + LINES[2][:20])
            evs = log.events()
            self.assertEqual([e["e"] for e in evs], ["start", "heard"])
            self.assertEqual(evs[1]["text"], 'open "YouTube"')
            self.assertEqual(evs[1]["ts"], 8.0)
            with open(path, "a") as f:
                f.write(LINES[2][20:] + "\n" + "\n".join(LINES[3:]) + "\n")
            evs = log.events()
            self.assertEqual(len(evs), len(LINES))
            self.assertEqual(log.kinds(), ["unknown", "open_site"])
            self.assertEqual(log.activity()["activity"], "listening")
            self.assertEqual([e["e"] for e in log.events(since=11.5)],
                             ["prompt", "confirm", "planner"])
            self.assertEqual([e["e"] for e in log.events(window=1)],
                             ["activity", "prompt", "confirm", "planner"])
            self.assertEqual(log.last("planner")["target"], 'button "Delete"')
            self.assertEqual(log.bad_lines, 0)

    def test_an_event_in_the_same_millisecond_counts(self):
        # The log's times are whole milliseconds; a step's start time isn't.
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "voice.jsonl")
            with open(path, "w") as f:
                f.write('{"t":8000,"w":2,"e":"heard","text":"pause","source":"typed",'
                        '"clean":true,"intent":"pause"}\n')
            log = testlog.TestLog(path)
            self.assertEqual(len(log.events(since=8.0004)), 1)
            self.assertEqual(log.events(since=8.001), [])

    def test_skips_what_isnt_an_event(self):
        bad = [b"not json", b'["a list"]', b'{"e":"heard"}', b'{"t":"soon","e":"said"}',
               b'{"t":1,"e":"launch"}', b'{"t":true,"e":"said"}', b"\xff\xfe",
               b"{" + b"x" * (testlog.MAX_LINE + 1) + b"}"]
        for raw in bad:
            self.assertIsNone(testlog.parse_line(raw), raw[:40])
        ev = testlog.parse_line(b'{"t":1,"e":"said","text":5}')
        self.assertEqual(ev["text"], "5")
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "voice.jsonl")
            with open(path, "wb") as f:
                f.write(b"\n".join(bad) + b"\n" + LINES[3].encode() + b"\n")
            log = testlog.TestLog(path)
            self.assertEqual([e["e"] for e in log.events()], ["said"])
            self.assertEqual(log.bad_lines, len(bad))

    def test_fresh(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "voice.jsonl")
            with open(path, "w") as f:
                f.write(LINES[0] + "\n")
            testlog.fresh(path)
            self.assertFalse(os.path.exists(path))
            testlog.fresh(path)


if __name__ == "__main__":
    unittest.main()
