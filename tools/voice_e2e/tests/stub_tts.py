# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""A stand-in for the voices (render.DriverTts), for tests without the
voice bench's downloads: each text becomes a tone per word at 22.05 kHz,
its pitch from the word and the speaker, its length from the speed."""

import hashlib
import math
import os
import struct
import wave

RATE = 22050


class StubTts(object):
    def __init__(self, work_dir, words=()):
        self.dir = work_dir
        self.words = set(words)
        self.calls = []

    def base_lexicon(self):
        path = os.path.join(self.dir, "stub-lexicon.txt")
        with open(path, "w", encoding="utf-8") as f:
            for w in sorted(self.words):
                f.write("%s || s t ʌ b\n" % w)
        return path

    def inputs(self, voices):
        return {"stub/%s" % v[0]: "0" * 64 for v in voices}

    def render(self, voice, items, lexicon_path, work):
        self.calls.append((voice[0], len(items)))
        for name, text, speed, out in items:
            samples = []
            for word in text.lower().split():
                h = int(hashlib.sha256((word + voice[0]).encode()).hexdigest()[:4], 16)
                f = 120 + h % 300 + voice[3] % 50
                n = int(0.25 * RATE / speed)
                for i in range(n):
                    env = min(1.0, i / 200.0, (n - i) / 200.0)
                    samples.append(int(8000 * env * math.sin(2 * math.pi * f * i / RATE)))
                samples += [0] * int(0.05 * RATE)
            with wave.open(out, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(RATE)
                w.writeframes(struct.pack("<%dh" % len(samples), *samples))
