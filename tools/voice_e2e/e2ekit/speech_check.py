# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The scenarios' speech through the bundle's own recognizer: every
rendered utterance, by every voice and speed, clean and mixed (render.mix),
fed to molt-speech over its protocol with every utterance counting (as in
always-listening mode); the confirmation words alone; and the wake phrase
before each utterance, clean.

Measured per voice, speed and condition: the share of utterances whose
transcript is exactly the utterance (after the voice bench's
normalization: case, punctuation, digits as words), the word error rate,
and for the words how many came out as the word. An utterance no voice
gets right in the clean condition is listed as consistently misheard: a
grammar or vocabulary matter, not noise.
"""

import json
import os
import time

from . import ears as earslib
from . import render

shc = earslib.shc
vbl = earslib.vbl


# The pause after "Hey Molt" in the set that checks the helper waits it out
# (SPEECH_HELPER_PROTOCOL.md §3.5): a command cut there is two utterances.
PAUSE_AFTER_WAKE_S = 0.7


def _feed(h, pcm, wait=20.0, pieces=False):
    n = len(h.lines)
    h.send(b"".join(shc.audio_frames(pcm + shc.silence(shc.TAIL_SILENCE_S))))
    sent = time.monotonic()
    settle = 0.6 + len(pcm) / 2 / render.SR * 0.1
    while time.monotonic() - sent < wait:
        pending, last = False, sent
        for t, e in h.lines[n:]:
            last = max(last, t)
            if e.get("type") in ("wake", "speech_start"):
                pending = True
            elif e.get("type") == "final":
                pending = False
        if not pending and time.monotonic() - last >= settle:
            break
        if h.proc.poll() is not None:
            break
        time.sleep(0.02)
    h.send(shc.frame(shc.RESET))
    events = [e for _, e in h.lines[n:]]
    finals = [e.get("text", "") for e in events if e.get("type") == "final"]
    text = " ".join(finals).strip()
    woke = any(e.get("type") == "wake" for e in events)
    if pieces:
        return text, woke, len([t for t in finals if t.strip()])
    return text, woke


def _helper(bundle, wake_required):
    args = earslib._Args(os.path.join(bundle, "speech", "molt-speech"),
                         os.path.join(bundle, "speech", "models"))
    h = shc.Helper(args)
    shc.ready(h)
    h.send(shc.config({"wake_required": wake_required}))
    return h


def run(speech_dir, bundle, utterances, conditions=("clean", "pink10", "music10"),
        log=print):
    with open(os.path.join(speech_dir, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    words = vbl.gesture_words()
    keys = [render.voice_key(v["name"], sp) for v in manifest["voices"]
            for sp in manifest["speeds"]]
    out = {"helper": os.path.join(bundle, "speech", "molt-speech"), "sets": {},
           "words": {}, "wake": {}, "pause": {}}
    for key in keys:
        voice, _, speed = key.partition("@")
        for cond in conditions:
            h = _helper(bundle, False)
            n = exact = errs = total = 0
            hyps = {}
            for text in utterances:
                clip = render.clip_path(speech_dir, voice, float(speed), text)
                pcm, _ = render.mix(clip, cond, "%s/%s" % (key, render.clip_id(text)))
                hyp, _ = _feed(h, pcm)
                hyps[text] = hyp
                e = vbl.edit_distance(vbl.normalize(text).split(), vbl.normalize(hyp).split())
                n += 1
                exact += e == 0
                errs += e
                total += len(vbl.normalize(text).split())
            ok = 0
            word_hyps = {}
            for w in words:
                clip = render.clip_path(speech_dir, voice, float(speed), w)
                pcm, _ = render.mix(clip, cond, "%s/%s" % (key, render.clip_id(w)))
                hyp, _ = _feed(h, pcm)
                word_hyps[w] = hyp
                ok += vbl.normalize(hyp) in (w, w + "s")
            h.close()
            name = "%s/%s" % (key, cond)
            out["sets"][name] = {"n": n, "exact": round(exact / n, 4) if n else None,
                                 "wer": round(errs / total, 4) if total else None,
                                 "transcripts": hyps}
            out["words"][name] = {"n": len(words), "right": ok, "transcripts": word_hyps}
            log("%-22s utterances %3d/%3d exact, WER %.1f%%; words %d/%d"
                % (name, exact, n, 100.0 * errs / max(1, total), ok, len(words)))
        # The wake phrase before each utterance, clean.
        if "clean" not in conditions:
            continue
        h = _helper(bundle, True)
        woke = right = 0
        import numpy as np
        audio = render._audio()
        w, _ = audio.read_wav(render.clip_path(speech_dir, voice, float(speed),
                                               render.WAKE_PHRASE))
        for text in utterances:
            x, _ = audio.read_wav(render.clip_path(speech_dir, voice, float(speed), text))
            y = np.concatenate([np.zeros(int(0.4 * render.SR), np.float32), w,
                                np.zeros(int(0.35 * render.SR), np.float32), x])
            hyp, did_wake = _feed(h, render.pcm16(y))
            woke += did_wake
            right += did_wake and vbl.normalize(_strip_wake(hyp)) == vbl.normalize(text)
        h.close()
        out["wake"][key] = {"n": len(utterances), "woke": woke, "right_after_wake": right}
        log("%-22s wake %d/%d, then right %d" % (key + "/clean", woke, len(utterances), right))
        # "Hey Molt", a pause, then the utterance, every utterance counting:
        # one utterance, not two.
        h = _helper(bundle, False)
        whole = 0
        for text in utterances:
            x, _ = audio.read_wav(render.clip_path(speech_dir, voice, float(speed), text))
            y = np.concatenate([np.zeros(int(0.4 * render.SR), np.float32), w,
                                np.zeros(int(PAUSE_AFTER_WAKE_S * render.SR), np.float32), x])
            _, _, n_finals = _feed(h, render.pcm16(y), pieces=True)
            whole += n_finals == 1
        h.close()
        out["pause"][key] = {"n": len(utterances), "whole": whole,
                             "pause_s": PAUSE_AFTER_WAKE_S}
        log("%-22s after a %.1f s pause: %d/%d in one piece"
            % (key + "/clean", PAUSE_AFTER_WAKE_S, whole, len(utterances)))
    clean = [s for name, s in out["sets"].items() if name.endswith("/clean")]
    out["consistently_misheard"] = sorted(
        t for t in utterances
        if clean and all(vbl.normalize(s["transcripts"].get(t, "")) != vbl.normalize(t)
                         for s in clean))
    out["examples"] = {t: [s["transcripts"].get(t, "") for s in clean]
                       for t in out["consistently_misheard"]}
    return out


def _strip_wake(text):
    from .runner import WAKE_WORDS
    words = vbl.normalize(text).split()
    i = 0
    while i < len(words) and i < 3 and words[i] in WAKE_WORDS:
        i += 1
    return " ".join(words[i:])


def markdown(res):
    lines = ["| Voice, speed | Condition | Utterances exact | WER | Words right |",
             "|---|---|---|---|---|"]
    for name in sorted(res["sets"]):
        s = res["sets"][name]
        w = res["words"][name]
        key, _, cond = name.rpartition("/")
        lines.append("| %s | %s | %.0f%% (%d) | %.1f%% | %d/%d |" % (
            key, cond, 100 * (s["exact"] or 0), s["n"], 100 * (s["wer"] or 0),
            w["right"], w["n"]))
    if res.get("wake"):
        lines += ["", "| Voice, speed | Woke | Right after the wake phrase |", "|---|---|---|"]
        for key in sorted(res["wake"]):
            w = res["wake"][key]
            lines.append("| %s | %d/%d | %d |" % (key, w["woke"], w["n"], w["right_after_wake"]))
    if res.get("pause"):
        lines += ["", "| Voice, speed | \"Hey Molt\", a pause, the command: in one piece |",
                  "|---|---|"]
        for key in sorted(res["pause"]):
            p = res["pause"][key]
            lines.append("| %s | %d/%d (%.1f s) |" % (key, p["whole"], p["n"], p["pause_s"]))
    if res.get("consistently_misheard"):
        lines += ["", "Misheard by every voice, clean:"]
        for t in res["consistently_misheard"]:
            lines.append("- %r: %s" % (t, "; ".join(repr(x) for x in res["examples"][t])))
    return "\n".join(lines)
