# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The report: the figures, the verdict against Phase 1's exit, and the
Markdown."""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from e2ekit import report  # noqa: E402


def step(status="passed", action_ms=800, heard=("open youtube",), errors=(0, 2), **kw):
    s = {"index": 0, "action": "say", "say": "open YouTube", "status": status,
         "heard": list(heard), "intents": ["open_site"],
         "latency": {"action_ms": action_ms, "heard_ms": 600, "said_ms": 900},
         "word_errors": list(errors), "expectations": []}
    s.update(kw)
    return s


def scenario(sid, task="go", status="passed", counts=True, steps=None, mode="wake"):
    return {"id": sid, "title": sid, "task": task, "mode": mode, "counts": counts,
            "voice": "ljspeech", "speed": 1.0, "status": status,
            "reason": "" if status == "passed" else "step 1: it didn't",
            "steps": steps if steps is not None else [step()]}


def run(label, scs):
    return {"label": label, "audio": label.split("/")[0], "condition": "clean",
            "typed": False, "scenarios": scs}


class ReportTest(unittest.TestCase):
    def test_a_run_that_meets_the_exit(self):
        scs = [scenario("owner-song", task="owner")] + \
              [scenario("go-%d" % i) for i in range(18)] + \
              [scenario("go-bad", status="failed",
                        steps=[step(status="failed", errors=(1, 2),
                                    heard=["open you too"], why="misheard")])] + \
              [scenario("sec-video", task="security")] + \
              [scenario("prompt-word", task="prompts", status="skipped", steps=[])]
        rep = report.build([run("pulse/clean", scs)], {"browser": "chrome"})
        s = rep["runs"][0]["summary"]
        self.assertEqual(s["counted"], 21)
        self.assertEqual(s["passed"], 20)
        self.assertEqual(s["skipped"], 1)
        self.assertAlmostEqual(s["success_rate"], 20 / 21, places=3)
        self.assertTrue(rep["ok"], rep["reasons"])
        self.assertEqual(s["latency_ms"]["action_median"], 800)
        self.assertEqual(s["misheard"][0]["heard"], "open you too")
        self.assertEqual(s["recognition"]["steps"], 21)
        md = report.markdown(rep)
        self.assertIn("Phase 1's exit is met.", md)
        self.assertIn("| pulse/clean | 20/21 (95%)", md)
        self.assertIn("go-bad", md)
        self.assertIn("### Misheard", md)

    def test_what_fails_the_exit(self):
        scs = [scenario("owner-song", task="owner", status="failed",
                        steps=[step(status="failed")]),
               scenario("sec-video", task="security", status="failed",
                        steps=[step(status="failed")]),
               scenario("browser-bookmarks", task="browser", status="not in grammar",
                        steps=[step(status="not in grammar",
                                    missing_commands=["bookmark_page"])]),
               scenario("go-1"), scenario("go-2", status="error", steps=[])]
        rep = report.build([run("file/clean", scs)], {})
        self.assertFalse(rep["ok"])
        text = " ".join(rep["reasons"])
        self.assertIn("1 of 5 tasks passed", text)
        self.assertIn("owner's scenario", text)
        self.assertIn("security scenarios acted: sec-video", text)
        self.assertIn("couldn't run", text)
        s = rep["runs"][0]["summary"]
        self.assertEqual(s["not_in_grammar"], 1)
        self.assertEqual(s["missing_commands"], ["bookmark_page"])
        self.assertAlmostEqual(s["success_rate_known_commands"], 0.25)
        md = report.markdown(rep)
        self.assertIn("Not in the grammar yet: `bookmark_page`.", md)
        self.assertIn("Not met:", md)

    def test_latency_gates_only_when_asked(self):
        scs = [scenario("go-%d" % i, steps=[step(action_ms=2400)]) for i in range(10)]
        self.assertTrue(report.build([run("pulse/clean", scs)], {})["ok"])
        rep = report.build([run("pulse/clean", scs)], {}, gate_latency=True)
        self.assertFalse(rep["ok"])
        self.assertIn("2400 ms", rep["reasons"][0])

    def test_write(self):
        rep = report.build([run("typed", [scenario("go-1")])], {})
        with tempfile.TemporaryDirectory() as d:
            report.write(d, rep)
            self.assertTrue(os.path.exists(os.path.join(d, "report.json")))
            with open(os.path.join(d, "report.md")) as f:
                self.assertIn("# Voice mode end to end", f.read())


if __name__ == "__main__":
    unittest.main()
