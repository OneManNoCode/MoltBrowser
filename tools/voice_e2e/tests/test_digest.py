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

    def test_a_crash_at_exit_doesnt_hide_each_failure(self):
        with tempfile.TemporaryDirectory() as d:
            self.make_report(d, [
                {"id": "a", "mode": "wake", "status": "failed", "reason": "not checked"},
                {"id": "b", "mode": "wake", "status": "failed", "reason": "no player"}])
            for name in ("a", "b"):
                write(os.path.join(d, "runs", "pulse-clean", name, "browser.log"), CRASH)
                write(os.path.join(d, "runs", "pulse-clean", name, "voice.jsonl"),
                      '{"t":1,"e":"heard","text":"%s"}\n' % name)
            text = digest.build(d)
            self.assertEqual(text.count("### "), 2)
            self.assertIn("### pulse/clean: a (failed)", text)
            self.assertIn("### pulse/clean: b (failed)", text)
            self.assertIn('"text": "b"', text)
            self.assertNotIn("the same in", text)

    def test_no_crash_shows_the_logs_end(self):
        with tempfile.TemporaryDirectory() as d:
            self.make_report(d, [
                {"id": "a", "mode": "wake", "status": "failed", "reason": "said nothing"}])
            write(os.path.join(d, "runs", "pulse-clean", "a", "browser.log"),
                  "[1:2:3/4.5:WARNING:voice_audio.cc:9] [MoltVoice] microphone "
                  "error 0: No audio\n"
                  "[1:2:3/4.6:WARNING:voice_audio.cc:9] [MoltVoice] microphone "
                  "error 0: No audio\n"
                  "[1:2:3/4.6:WARNING:filter_list_manager.cc:9] [MoltShield] list "
                  "update failed: easylist\n"
                  "[1:2:3/4.5:INFO:x.cc:1] the last line\n")
            text = digest.build(d)
            self.assertEqual(text.count("[MoltVoice] microphone error 0"), 3)
            self.assertIn("MoltBrowser's lines:", text)
            self.assertNotIn("list update failed", text.split("(nothing fatal")[0])
            self.assertIn("(nothing fatal; the end of browser.log:)", text)
            self.assertIn("the last line", text)
            self.assertIn("(no voice test log)", text)

    def test_the_gpu_sandbox_warning_isnt_fatal(self):
        with tempfile.TemporaryDirectory() as d:
            self.make_report(d, [
                {"id": "a", "mode": "wake", "status": "failed", "reason": "no player"}])
            write(os.path.join(d, "runs", "pulse-clean", "a", "browser.log"),
                  "[1:1:1/1.1:WARNING:sandbox/policy/linux/sandbox_linux.cc:406] "
                  "InitializeSandbox() called with multiple threads in process "
                  "gpu-process.\n"
                  + "[1:1:1/1.2:ERROR:dbus/bus.cc:405] Failed to connect\n" * 30
                  + "[1:1:1/1.3:INFO:x.cc:1] the last line\n")
            text = digest.build(d)
            self.assertNotIn("fatal lines", text)
            self.assertNotIn("InitializeSandbox", text)
            self.assertIn("the last line", text)

    def test_bounded(self):
        with tempfile.TemporaryDirectory() as d:
            scs = [{"id": "s%d" % i, "mode": "wake", "status": "failed",
                    "reason": "reason %d" % i} for i in range(50)]
            self.make_report(d, scs)
            text = digest.build(d, limit=100000, max_groups=5)
            self.assertEqual(text.count("### "), 5)
            self.assertIn("- pulse/clean: s49 (failed): reason 49", text)
            self.assertLessEqual(len(digest.build(d, limit=300)), 400)

    def test_the_microphone(self):
        out = digest.microphone_digest({"mic_levels": [-120, -8, -24],
                                        "mic_heard": "hey molt play"})
        self.assertEqual(out[0], "microphone, each second's peak (dBFS): -120 -8 -24")
        self.assertIn('"hey molt play"', out[1])
        self.assertEqual(digest.microphone_digest({"mic_heard": ""})[0],
                         "microphone, as the kit's recognizer hears it: (nothing)")
        self.assertEqual(digest.microphone_digest({}), [])

    def test_audio_lines(self):
        log = ("[1:2:1105/0248.1:WARNING:media/audio/linux/audio_manager_linux.cc:55] "
               "Falling back to ALSA for audio output.\n"
               "[1:3:1105/0248.2:ERROR:media/audio/alsa/alsa_util.cc:204] "
               "PcmOpen: default, No such file or directory\n"
               "[1:3:1105/0248.3:ERROR:media/audio/alsa/alsa_util.cc:204] "
               "PcmOpen: default, No such file or directory\n"
               "[1:4:1105/0248.4:INFO:CONSOLE(1)] \"page\"\n")
        _, out = digest.browser_log_digest(log)
        self.assertEqual(out[0], "audio lines:")
        self.assertEqual(len([l for l in out if "PcmOpen" in l]), 1)
        self.assertTrue(any("Falling back to ALSA" in l for l in out))
        self.assertFalse(any("CONSOLE" in l for l in out[:3]))

    def test_microphone_reports_fold_into_one_line(self):
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "voice.jsonl")
            lines = [{"t": 1000, "w": 1, "e": "heard", "text": "hi"}]
            lines += [{"t": 1000 + 5000 * i, "w": 1, "e": "mic",
                       "peak_db": -120 if i < 3 else -30, "samples": 80000,
                       "echo": "off" if i == 1 else "none"}
                      for i in range(1, 70)]
            lines += [{"t": 400000, "w": 1, "e": "said", "text": "ok"}]
            write(log, "\n".join(json.dumps(l) for l in lines) + "\n")
            out = digest.voice_log_digest(log)
            self.assertEqual(len(out), 3)
            self.assertIn("heard", out[0])
            self.assertIn('"reports": 69', out[1])
            self.assertIn('"loudest_db": -30', out[1])
            self.assertIn('"quietest_db": -120', out[1])
            self.assertIn('"samples": 5520000', out[1])
            self.assertIn('["off", "none"]', out[1])
            self.assertIn("said", out[2])

    def test_no_report(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(digest.build(d), "(no failed scenarios)")


if __name__ == "__main__":
    unittest.main()
