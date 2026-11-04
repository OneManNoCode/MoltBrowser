# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The kit's own ears: the bundle's molt-speech, run by the kit (not by the
browser) over its protocol (tools/voice_bench/speech_helper_check.py), to
hear what the browser said aloud. A prompt's confirmation word is never in
the test log; the kit learns it the way the user does, by hearing the
browser say it (and so checks that it was said aloud at all).
"""

import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))),
                     "tools", "voice_bench")
if BENCH not in sys.path:
    sys.path.insert(0, BENCH)

import speech_helper_check as shc  # noqa: E402
import voice_bench_lib as vbl  # noqa: E402


class _Args(object):
    def __init__(self, helper, models):
        self.helper = helper
        self.models = models


class Ears(object):
    def __init__(self, helper, models):
        self.args = _Args(helper, models)
        self.h = None
        self.words = vbl.gesture_words()

    def start(self):
        self.h = shc.Helper(self.args)
        shc.ready(self.h)
        self.h.send(shc.config({"wake_required": False}))
        return self

    def transcribe(self, pcm, settle=0.8, wait=20.0):
        """What the recognizer makes of |pcm| (16 kHz mono 16-bit)."""
        if not self.h:
            self.start()
        n = len(self.h.lines)
        self.h.send(b"".join(shc.audio_frames(pcm + shc.silence(1.0))))
        sent = time.monotonic()
        while time.monotonic() - sent < wait:
            pending, last = False, sent
            for t, e in self.h.lines[n:]:
                last = max(last, t)
                if e.get("type") in ("wake", "speech_start"):
                    pending = True
                elif e.get("type") == "final":
                    pending = False
            if not pending and time.monotonic() - last >= settle:
                break
            time.sleep(0.05)
        self.h.send(shc.frame(shc.RESET))
        return " ".join(e.get("text", "") for _, e in self.h.lines[n:]
                        if e.get("type") == "final").strip()

    def find_word(self, text):
        """The confirmation word |text| gives ("... say harbor"), if any:
        the last word of the list in it."""
        found = None
        for w in vbl.normalize(text).split():
            if w in self.words:
                found = w
            elif w.endswith("s") and w[:-1] in self.words:
                found = w[:-1]
        return found

    def stop(self):
        if self.h:
            self.h.close()
            self.h = None
