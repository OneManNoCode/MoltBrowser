# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The failures' digest for a CI log: grouped by the browser's first fatal
line, the noise left out, the voice log shown, everything bounded."""

import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from e2ekit import digest  # noqa: E402

CRASH = ("[1:2:1105/024819.211222:ERROR:dbus/bus.cc:405] Failed to connect\n"
         "[123:456:1105/024819.215:FATAL:media/audio/pulse/pulse_util.cc:99] "
         "Check failed: x == y (0x1234 vs. 0x5)\n"
         "#0 0x55 base::debug::CollectStackTrace()\n"
         "#1 0x56 media::Foo()\n")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


class DigestTest(unittest.TestCase):
    def make_report(self, d, scenarios):
        write(os.path.join(d, "report.json"), json.dumps(
            {"runs": [{"label": "pulse/clean", "scenarios": scenarios}]}))

    def test_one_crash_prints_once(self):
        with tempfile.TemporaryDirectory() as d:
            self.make_report(d, [
                {"id": "a", "mode": "wake", "status": "error", "reason": "CdpClosed"},
                {"id": "b", "mode": "always", "status": "error", "reason": "CdpClosed"},
                {"id": "c", "mode": "wake", "status": "passed", "reason": ""}])
            write(os.path.join(d, "runs", "pulse-clean", "a", "browser.log"), CRASH)
            write(os.path.join(d, "runs", "pulse-clean", "b-always", "browser.log"),
                  CRASH.replace("024819.215", "031000.999").replace("0x1234", "0x99"))
            write(os.path.join(d, "runs", "pulse-clean", "a", "voice.jsonl"),
                  '{"t":1000,"e":"start","kinds":[]}\nnot json\n'
                  '{"t":2500,"e":"activity","activity":"listening","mic":true}\n')
            text = digest.build(d)
            self.assertEqual(text.count("### "), 1)
            self.assertIn("the same in 1 more: b-always", text)
            self.assertIn("Check failed: x == y", text)
            self.assertIn("media::Foo()", text)
            self.assertNotIn("dbus", text)
            self.assertIn('+1.5s activity  {"activity": "listening", "mic": true}', text)
            self.assertNotIn("### pulse/clean: c", text)

    def test_no_crash_shows_the_logs_end(self):
        with tempfile.TemporaryDirectory() as d:
            self.make_report(d, [
                {"id": "a", "mode": "wake", "status": "failed", "reason": "said nothing"}])
            write(os.path.join(d, "runs", "pulse-clean", "a", "browser.log"),
                  "[1:2:3/4.5:WARNING:voice_audio.cc:9] [MoltVoice] microphone "
                  "error 0: No audio\n"
                  "[1:2:3/4.6:WARNING:voice_audio.cc:9] [MoltVoice] microphone "
                  "error 0: No audio\n"
                  "[1:2:3/4.5:INFO:x.cc:1] the last line\n")
            text = digest.build(d)
            self.assertEqual(text.count("[MoltVoice] microphone error 0"), 3)
            self.assertIn("MoltBrowser's lines:", text)
            self.assertIn("(nothing fatal; the end of browser.log:)", text)
            self.assertIn("the last line", text)
            self.assertIn("(no voice test log)", text)

    def test_bounded(self):
        with tempfile.TemporaryDirectory() as d:
            scs = [{"id": "s%d" % i, "mode": "wake", "status": "failed",
                    "reason": "reason %d" % i} for i in range(50)]
            self.make_report(d, scs)
            text = digest.build(d, limit=100000, max_groups=5)
            self.assertEqual(text.count("### "), 5)
            self.assertIn("- pulse/clean: s49 (failed): reason 49", text)
            self.assertLessEqual(len(digest.build(d, limit=300)), 400)

    def test_no_report(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(digest.build(d), "(no failed scenarios)")


if __name__ == "__main__":
    unittest.main()
