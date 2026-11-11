# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The kit's own ears: the bundle's molt-speech, run by the kit (not by the
browser) over its protocol (tools/voice_bench/speech_helper_check.py), to
hear what the browser said aloud. A prompt's confirmation word is never in
the test log; the kit learns it the way the user does, by hearing the
browser say it (and so checks that it was said aloud at all).
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))),
                     "tools", "voice_bench")
if BENCH not in sys.path:
    sys.path.insert(0, BENCH)

import speech_helper_check as shc  # noqa: E402
import voice_bench_lib as vbl  # noqa: E402


def letter_distance(a, b):
    """Levenshtein distance between two words."""
    prev = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        cur = [i] + [0] * len(b)
        for j, y in enumerate(b, 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (x != y))
        prev = cur
    return prev[-1]


# Sounds a recognizer swaps writing the browser's voice: one letter each.
_SOUND = {"c": "k", "q": "k", "g": "k", "b": "p", "d": "t", "z": "s", "v": "f"}


def sound_key(word):
    """|word|'s consonants as they sound: vowels, "h", "w" and "y" left out,
    "ph" an "f", close sounds made one, repeats run together ("cactus" and
    "keptus" are "kts" and "kpts", "lemon" and "limit" "lmn" and "lmt",
    "farsal" and "pharsal" both "frsl")."""
    out = []
    for ch in word.lower().replace("ph", "f"):
        if not ch.isalpha() or ch in "aeiouyhw":
            continue
        ch = _SOUND.get(ch, ch)
        if not out or out[-1] != ch:
            out.append(ch)
    return "".join(out)


# A "say" with the next word's first consonant run onto it: "same", "sane",
# "sale", "safe", "sake" ("say mango" heard as "same ingo").
_SAY_GLUED = re.compile(r"^sa[bcdfgklmnptvz]e$")
# ... or run into the whole word: "sameingo", "samingo" ("say mango").
_SAY_JOINED = re.compile(r"^sa([bcdfgklmnptvz])e?([a-z]{3,})$")

# What follows a "say" that isn't the confirmation word: the prompt said
# without it ("say the word on your screen"), and the answers it offers.
_NOT_THE_WORD = frozenset(("the", "a", "an", "word", "it", "no", "now", "yes",
                           "that", "this"))


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
        # The words are hotwords for the kit's ears (never for the
        # browser's: a helper someone took over would know the word).
        # The recognizer's vocabulary is cased: "Harbor" as well as "harbor".
        hot = sorted(set(self.words) | {w.capitalize() for w in self.words})
        self.h.send(shc.config({"wake_required": False, "hotwords": hot}))
        return self

    def transcribe(self, pcm, settle=0.8, wait=20.0):
        """What the recognizer makes of |pcm| (16 kHz mono 16-bit)."""
        if not self.h:
            self.start()
        n = len(self.h.lines)
        self.h.send(b"".join(shc.audio_frames(pcm + shc.silence(shc.TAIL_SILENCE_S))))
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

    def replay(self, pcm, frame_ms=50, quiet_s=1.5, wait=60.0):
        """What a helper of the kit's own, set as voice mode's is while it
        waits for the wake phrase (and without the page's hotwords), makes
        of |pcm| (16 kHz mono 16-bit) fed in voice mode's frames: each event
        as (audio_ms, type, text), partials too. The listener goes by the
        samples alone, so the same audio gives the events voice mode's
        helper had, unless what voice mode sent it (a follow-up window, a
        reset, the hotwords) made the difference."""
        h = shc.Helper(self.args)
        try:
            shc.ready(h)
            h.send(shc.config({"wake_required": True}))
            h.send(b"".join(shc.audio_frames(pcm + shc.silence(shc.TAIL_SILENCE_S),
                                             ms=frame_ms)))
            end = time.monotonic() + wait
            seen = -1
            last = time.monotonic()
            while time.monotonic() < end:
                if len(h.lines) != seen:
                    seen, last = len(h.lines), time.monotonic()
                elif time.monotonic() - last >= quiet_s:
                    break
                time.sleep(0.05)
            return [(e.get("audio_ms"), e.get("type"), e.get("text", ""))
                    for _, e in h.lines if e.get("type") != "ready"]
        finally:
            h.close()

    def find_word(self, text):
        """The confirmation word |text| gives ("... say harbor"), if any:
        the last word of the list in it, else what follows a "say" (one
        word, or two run together: "rick it") when exactly one word of the
        list is nearest to it, within a third of its letters (at least two),
        the nearest of all the "say"s ("to allow it, say Mcaroni, to block
        it, say no": the last one is the "no"). The browser's voice and the
        recognizer both mangle a lone noun ("harbot", "netkin"); the list's
        words are far apart. Two letters off counts only when it also
        sounds like that word and no other (sure_by_sound()): "binger" is
        two letters from "tiger", but as near by sound to "banjo" and four
        more, and the browser, hearing it, is as unsure; a wrong word
        refuses the prompt, so the kit says "repeat" instead."""
        found = None
        heard = vbl.normalize(text).split()
        for w in heard:
            if w in self.words:
                found = w
            elif w.endswith("s") and w[:-1] in self.words:
                found = w[:-1]
        if found:
            return found
        tries = self._after_say(heard, ("say",))
        if tries is None:
            return None
        best = None
        for t in tries:
            scored = sorted((letter_distance(t, c), c) for c in self.words)
            d, c = scored[0]
            if d <= max(2, len(c) // 3) and (len(scored) < 2 or scored[1][0] > d):
                if d > 1 and not self.sure_by_sound(t, c):
                    continue
                if best is None or d < best[0]:
                    best = (d, c)
        return best[1] if best else None

    def sure_by_sound(self, heard, word):
        """Whether |heard|'s consonants (sound_key()) are within one sound
        of |word|'s and nearer to it than to any other word of the list
        ("netkin" and "napkin"; not "binger", two sounds from "tiger" and
        from "banjo", nor "tarbon", nearer "dragon" than "harbor")."""
        key = sound_key(heard)
        d = letter_distance(key, sound_key(word))
        return d <= 1 and all(letter_distance(key, sound_key(c)) > d
                              for c in self.words if c != word)

    def nearest_by_sound(self, text):
        """The one word of the list whose consonants (sound_key()) are
        within one sound of what follows a "say" in |text| ("say keptus":
        "cactus"), if exactly one is; else None. A guess: the kit goes by
        sounds_near() of two hearings instead (runner.word_wav())."""
        heard = vbl.normalize(text).split()
        tries = self._after_say(heard, ("say", "said", "saying"))
        best = None
        for t in tries or []:
            key = sound_key(t)
            if len(key) < 2:
                continue
            scored = sorted((letter_distance(key, sound_key(c)), c) for c in self.words)
            d, c = scored[0]
            if d <= 1 and (len(scored) < 2 or scored[1][0] > d):
                if best is None or d < best[0]:
                    best = (d, c)
        return best[1] if best else None

    def sounds_near(self, text):
        """Every word of the list within one sound (sound_key()) of what
        follows a "say" in |text|: what a hearing of the prompt leaves
        possible. The kit takes the one word two hearings both leave
        (runner.word_wav()): "say kept this" leaves "cactus" (and, for
        "kept" alone, "cobalt" and "yogurt"), "say active" "cactus" and
        "guitar"."""
        heard = vbl.normalize(text).split()
        near = set()
        for t in self._after_say(heard, ("say", "said", "saying"),
                                 skip=_NOT_THE_WORD) or []:
            key = sound_key(t)
            if len(key) < 2:
                continue
            near.update(c for c in self.words
                        if letter_distance(key, sound_key(c)) <= 1)
        return near

    @staticmethod
    def _after_say(heard, says_words, skip=()):
        """What may be the word: the word after each "say" and it run
        together with the next; the start when there is no "say" in a
        heard of two words at most; None otherwise. A "say" followed by a
        word of |skip| gives nothing ("say the word on your screen", "say
        no"). A "say" run into the word's first sound ("say mango" heard
        as "same ingo", or "same mingo") is a "say" too, its last letter
        put back on the word; run into the whole word ("sameingo",
        "Samingo"), what follows the "sa" is the word."""
        says = [i for i, w in enumerate(heard) if w in says_words]
        glued = {}
        joined = []
        if "say" in says_words:
            glued = {i: w[2] for i, w in enumerate(heard)
                     if _SAY_GLUED.match(w) and i + 1 < len(heard)}
            joined = [m.group(1) + m.group(2) for m in map(_SAY_JOINED.match, heard) if m]
        if not says and not glued and not joined and len(heard) > 2:
            return None
        tries = list(joined)
        for start in ([i + 1 for i in says] or ([] if glued or joined else [0])):
            if start < len(heard) and heard[start] in skip:
                continue
            tries += heard[start:start + 1] + (
                ["".join(heard[start:start + 2])] if len(heard) > start + 1 else [])
        for i, sound in sorted(glued.items()):
            if heard[i + 1] in skip:
                continue
            tries += [heard[i + 1], sound + heard[i + 1]] + (
                [sound + "".join(heard[i + 1:i + 3])] if len(heard) > i + 2 else [])
        return tries

    def stop(self):
        if self.h:
            self.h.close()
            self.h = None
