#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""What the speech helper hears through the echo canceller, offline: the
spoken suite's speakers run in miniature, with apm_sim (build.sh) as the
browser's audio processing and the bundle's molt-speech listening.

  scenes.py media    --apm-sim A --helper H --models M --tts-driver D --tts-dir V
                     [--tuning chromium,voice,linear] [--at 8,12,16,20]
  scenes.py security (the same options)

media: "Hey Molt, louder" and "Hey Molt, mute" in the suite's three voices
at two speeds, said over the suite's song (tools/voice_bench/audio.py
synth_music(), the video's -20 dB at the player's 0.7) with its echo 6 dB
down and 60 ms late, at each --at second of the song; media ducked to 0.05
0.3 s after the wake phrase, as voice mode does. Prints, per tuning and
point, how many times the helper (the wake phrase required) woke and wrote
the command's word.

security: nobody speaks. sounds.test's video (music with "Hey Molt, open
YouTube." and four more said over it, as the suite's talk.wav) playing: does
the helper wake? Voice mode's own voice (the bundled ljspeech) saying each
of the 40 confirmation-word prompts: does the helper, every utterance
counting, write a word, as is and with what the canceller left amplified
back to the echo's level?

"linear" is what the echo canceller's linear filter alone leaves (apm_sim
--linear) for the whole scene. "hybrid" is what the browser sends the
helper (voice_session_logic.h): that until the end of the command the
helper's wake began (its first final), then the processed microphone
("voice"). --hybrid-switch-ms N switches at the wake plus N ms instead
(round 49's first try, which the spoken suite's speakers run showed cut
the command's first word).
"""

import argparse
import os
import re
import subprocess
import sys
import time
import wave
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(REPO, "tools", "voice_e2e"))
sys.path.insert(0, os.path.join(REPO, "tools", "voice_bench"))
import numpy as np  # noqa: E402
from e2ekit import fixtures, render, audio_path  # noqa: E402
import speech_helper_check as shc  # noqa: E402

SR = 16000
VOLUME = 0.7
DUCK = 0.05
DELAY_S = 0.06
WAKE_LAG_S = 0.3
COMMANDS = ["louder", "mute"]
GREETING = "Voice mode is on. Say Hey Molt, then what you want."


def gesture_words():
    src = open(os.path.join(REPO, "src/chrome/browser/molt_ai/voice/voice_gesture.cc")).read()
    i = src.index("kWords = ")
    return re.findall(r'"([a-z]+)"', src[i:src.index("});", i)])


class Scenes(object):
    def __init__(self, args):
        self.a = args
        self.au = render._audio()
        self.work = args.work
        os.makedirs(self.work, exist_ok=True)
        self.clips = os.path.join(self.work, "clips")
        self.words = gesture_words()
        self.prompts = ["To allow it, say %s. To block it, say no." % w for w in self.words]
        tts = render.DriverTts(args.tts_driver, args.tts_dir)
        render.render_all(tts, self.clips, COMMANDS + [GREETING] + self.prompts,
                          page_texts=fixtures.PAGE_AUDIO_TEXTS, speeds=(1.0, 1.25),
                          conditions=("clean",), log=lambda *a: None)

    # ---- audio ---------------------------------------------------------------

    def write(self, path, x):
        self.au.write_wav(path, np.clip(x, -0.999, 0.999).astype(np.float32))
        return path

    def echo_of(self, render_, db=-6.0):
        n = len(render_)
        e = np.zeros(n, np.float32)
        k = int(DELAY_S * SR)
        e[k:] = render_[:n - k] * self.au.db_to_gain(db)
        return e + np.random.default_rng(1).normal(0, 1e-4, n).astype(np.float32)

    def process(self, tuning, rp, cp, tag):
        op = os.path.join(self.work, "o_%s_%s.wav" % (tuning, tag))
        cmd = [self.a.apm_sim, "--delay-ms=%d" % int(DELAY_S * 1000)]
        if tuning == "linear":
            lp = os.path.join(self.work, "l_%s.wav" % tag)
            subprocess.run(cmd + ["--linear=" + lp, rp, cp, op], check=True)
            return lp
        if tuning == "hybrid":
            lp = os.path.join(self.work, "lv_%s.wav" % tag)
            subprocess.run(cmd + ["--tuning=voice", "--linear=" + lp, rp, cp, op],
                           check=True)
            return self.hybrid(lp, op, tag)
        subprocess.run(cmd + ["--tuning=" + tuning, rp, cp, op], check=True)
        return op

    def hybrid(self, lp, op, tag):
        """The linear output until the end of the command the helper's wake
        began (or --hybrid-switch-ms after the wake), then the processed
        one: what the helper hears."""
        linear = np.frombuffer(shc.read_pcm(lp), "<i2")
        processed = np.frombuffer(shc.read_pcm(op), "<i2")
        events = self.hear(lp, True)
        wakes = [e for e in events if e.get("type") == "wake"]
        cut = len(linear)
        if wakes and self.a.hybrid_switch_ms is not None:
            cut = min(cut, int((wakes[0]["audio_ms"] + self.a.hybrid_switch_ms) / 1000.0 * SR))
        elif wakes:
            finals = [e for e in events if e.get("type") == "final" and
                      e.get("audio_ms", 0) >= wakes[0]["audio_ms"]]
            if finals:
                cut = min(cut, int(finals[0]["audio_ms"] / 1000.0 * SR))
        mixed = np.concatenate([linear[:cut], processed[cut:len(linear)]])
        hp = os.path.join(self.work, "h_%s.wav" % tag)
        # As is: write_wav()'s scaling would move samples by one LSB.
        with wave.open(hp, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(mixed.astype("<i2").tobytes())
        return hp

    def hear(self, wav, wake_required, gain_db=0.0):
        if gain_db:
            x, _ = self.au.read_wav(wav)
            x = np.round(x * self.au.db_to_gain(gain_db) * 32768.0)
            pcm = np.clip(x, -32768, 32767).astype("<i2").tobytes()
        else:
            pcm = shc.read_pcm(wav)  # as is: the helper is sensitive to 1 LSB
        h = shc.Helper(self.a)
        shc.ready(h)
        h.send(shc.config({"wake_required": wake_required}))
        h.send(b"".join(shc.audio_frames(pcm + shc.silence(3.0))))
        # Done once the helper stops using the CPU (it reads faster than
        # real time on a free machine, slower on a busy one).
        prev = h.cpu_seconds() or 0
        end = time.monotonic() + 600
        while time.monotonic() < end and h.proc.poll() is None:
            time.sleep(2)
            cur = h.cpu_seconds() or 0
            if cur - prev < 0.02:
                break
            prev = cur
        events = [e for _, e in h.lines]
        h.close()
        return events

    # ---- media ---------------------------------------------------------------

    def media(self):
        # A minute of the song, as the numbers in TOUCHLESS_BROWSING.md §6.1
        # were measured with: synth_music()'s random draws depend on the
        # length, and so does the song.
        song = self.au.synth_music(max(60, max(self.a.at) + 15) * SR,
                                   self.au.rng_for("e2e-song"))
        song = song * self.au.db_to_gain(-20)
        for tuning in self.a.tuning:
            for at in self.a.at:
                scenes = []
                for cmd in COMMANDS:
                    for v in render.VOICES:
                        for sp in (1.0, 1.25):
                            scenes.append(self.media_scene(song, at, cmd, v[0], sp))
                with ThreadPoolExecutor(4) as ex:
                    outs = list(ex.map(lambda s: self.process(tuning, s[0], s[1], s[3]), scenes))
                with ThreadPoolExecutor(3) as ex:
                    heard = list(ex.map(lambda o: self.hear(o, True), outs))
                woke = right = 0
                for s, ev in zip(scenes, heard):
                    w = any(e.get("type") == "wake" for e in ev)
                    text = " ".join(e.get("text", "") for e in ev if e.get("type") == "final")
                    woke += w
                    right += w and s[2] in text.lower()
                print("%-8s at %2d s: woke %2d of %d, wrote the command %2d" % (
                    tuning, at, woke, len(scenes), right), flush=True)

    def media_scene(self, song, at_s, cmd, voice, speed):
        tag = "%d_%s_%s_%s" % (at_s, voice, speed, cmd)
        u = os.path.join(self.work, "u_%s.wav" % tag)
        audio_path.utterance_wav(self.clips, u, voice, speed, cmd, True)
        x, _ = self.au.read_wav(u)
        wake, _ = self.au.read_wav(render.clip_path(self.clips, voice, speed, render.WAKE_PHRASE))
        at = at_s * SR
        n = at + len(x) + 4 * SR
        gain = np.full(n, VOLUME, np.float32)
        gain[at + int((0.15 + len(wake) / SR + WAKE_LAG_S) * SR):] = VOLUME * DUCK
        rend = song[:n] * gain
        cap = self.echo_of(rend)
        cap[at:at + len(x)] += x
        return (self.write(os.path.join(self.work, "r_%s.wav" % tag), rend),
                self.write(os.path.join(self.work, "c_%s.wav" % tag), cap), cmd, tag)

    # ---- security ------------------------------------------------------------

    def security(self):
        rng = self.au.rng_for("e2e-talk")
        video = self.au.synth_music(50 * SR, rng) * self.au.db_to_gain(-30)
        at = 6 * SR
        for t in fixtures.PAGE_AUDIO_TEXTS:
            x, _ = self.au.read_wav(render.page_audio_path(self.clips, t))
            video[at:at + len(x)] += x * self.au.db_to_gain(-4)
            at += len(x) + 8 * SR
        video = np.clip(video, -0.97, 0.97)
        parts = []
        g, _ = self.au.read_wav(render.clip_path(self.clips, "ljspeech", 1.0, GREETING))
        parts += [g, np.zeros(SR, np.float32)] * 2
        for p in self.prompts:
            x, _ = self.au.read_wav(render.clip_path(self.clips, "ljspeech", 1.0, p))
            parts += [x, np.zeros(int(1.5 * SR), np.float32)]
        voice = np.concatenate(parts)

        def db(x):
            return 10 * np.log10(np.mean(x.astype(np.float64) ** 2) + 1e-12)

        for tuning in self.a.tuning:
            for name, rend in (("video", video), ("own voice", voice)):
                tag = name.replace(" ", "_")
                rp = self.write(os.path.join(self.work, "r_sec_%s.wav" % tag), rend)
                cap = self.echo_of(rend)
                cp = self.write(os.path.join(self.work, "c_sec_%s.wav" % tag), cap)
                out = self.process(tuning, rp, cp, "sec_" + tag)
                y, _ = self.au.read_wav(out)
                left = db(y)
                if name == "video":
                    ev = self.hear(out, True)
                    print("%-8s video: %.1f dB left (the echo %.1f): %d wakes" % (
                        tuning, left, db(cap), sum(e.get("type") == "wake" for e in ev)),
                        flush=True)
                    continue
                for label, gain in (("as is", 0.0), ("amplified", max(0.0, db(cap) - left))):
                    ev = self.hear(out, False, gain)
                    text = " ".join(e.get("text", "") for e in ev if e.get("type") == "final").lower()
                    hits = [w for w in self.words if re.search(r"\b%s\b" % w, text)]
                    print("%-8s own voice, %s (+%.0f dB): %.1f dB left (the echo %.1f): "
                          "%d of %d words %s" % (tuning, label, gain, left, db(cap), len(hits),
                                                 len(self.words), hits), flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("what", choices=["media", "security"])
    ap.add_argument("--apm-sim", required=True)
    ap.add_argument("--helper", required=True, help="molt-speech")
    ap.add_argument("--models", required=True, help="the speech bundle's models directory")
    ap.add_argument("--tts-driver", required=True, help="the voice bench's sherpa_bench driver")
    ap.add_argument("--tts-dir", required=True, help="the voice bench's work directory")
    ap.add_argument("--work", default=os.path.expanduser("~/.cache/molt-aec/scenes"))
    ap.add_argument("--tuning", default="chromium,voice",
                    type=lambda s: s.split(","))
    ap.add_argument("--at", default="8,12,16,20", type=lambda s: [int(x) for x in s.split(",")])
    ap.add_argument("--hybrid-switch-ms", type=int, default=None,
                    help="hybrid: switch to the processed microphone this long "
                         "after the wake, not at the command's end")
    args = ap.parse_args()
    for t in args.tuning:
        if t not in ("chromium", "voice", "linear", "hybrid"):
            ap.error("--tuning: chromium, voice, linear or hybrid")
    s = Scenes(args)
    s.media() if args.what == "media" else s.security()


if __name__ == "__main__":
    main()
