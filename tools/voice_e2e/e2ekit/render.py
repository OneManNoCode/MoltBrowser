# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Synthetic speech for the scenarios: every utterance, the wake phrase,
the voice gesture's confirmation words and the page audio of the security
fixtures, rendered to 16 kHz mono 16-bit WAV by offline voices pinned by
SHA-256, at two speeds, then mixed with noise and music at set
signal-to-noise ratios (tools/voice_bench/audio.py).

Voices (tools/voice_bench/artifacts.json pins each archive; the manifest
records each model file's SHA-256 too):
  ljspeech   Piper en_US ljspeech medium (the voice scripts/bundle-speech.sh
             ships as voice mode's own; a woman's voice)
  libritts-m Piper en_US libritts_r medium (int8), speaker 300 (a man's)
  libritts-f Piper en_US libritts_r medium (int8), speaker 30 (a woman's)
at speeds 1.0 and 1.25. The voice bench's driver renders them with
sherpa-onnx 2.0.1 and a vanishing sampling noise (--noise-scale 1e-30),
one thread, so a render is the same bytes every time on a machine; the
mixes take every random choice from a generator seeded with the clip's
name (audio.rng_for()). manifest.json records the inputs and the SHA-256
of every file written; --hashes compares them with a recorded set.

Layout under the output directory:
  lexicon.txt                    the merged lexicon the voices read
  clips/<voice>@<speed>/<id>.wav trimmed, 16 kHz, speech at -26 dBFS
  page/<id>.wav                  the security fixtures' page audio
  manifest.json                  the inputs' and every clip's SHA-256, and
                                 the SHA-256 of each mix's PCM (a clip with
                                 0.4 s before, 1.3 s after, a -65 dBFS
                                 floor and the condition: mix(), made the
                                 same way wherever it is played)
"""

import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(KIT))
BENCH = os.path.join(REPO, "tools", "voice_bench")
if BENCH not in sys.path:
    sys.path.insert(0, BENCH)

import voice_bench_lib as vbl  # noqa: E402

# (name, artifact, model file, speaker id)
VOICES = [
    ("ljspeech", "tts-piper-en_US-ljspeech-medium", "en_US-ljspeech-medium.onnx", 0),
    ("libritts-m", "tts-piper-en_US-libritts_r-medium-int8",
     "en_US-libritts_r-medium.onnx", 300),
    ("libritts-f", "tts-piper-en_US-libritts_r-medium-int8",
     "en_US-libritts_r-medium.onnx", 30),
]
SPEEDS = (1.0, 1.25)
LEXICON_ARTIFACT = "tts-lexicon-en-us"
RUNTIME_ARTIFACT = "sherpa-onnx-linux-x64"
# The page audio's voice: not one the user speaks with.
PAGE_VOICE = ("libritts-p", "tts-piper-en_US-libritts_r-medium-int8",
              "en_US-libritts_r-medium.onnx", 700)
WAKE_PHRASE = "Hey Molt"
# Said when the kit didn't hear a prompt's word: the browser says it again.
REPEAT = "repeat"
NOISE_SCALE = "1e-30"
SPEECH_DB = -26.0
# Conditions: name -> (kind, SNR dB).
CONDITIONS = {"clean": (None, None), "pink10": ("pink", 10.0),
              "music10": ("music", 10.0)}
LEAD_S = 0.4
TRAIL_S = 1.3
SR = 16000

KIT_LEXICON = os.path.join(KIT, "lexicon-extra.txt")
BENCH_LEXICON = os.path.join(BENCH, "testset", "lexicon-extra.txt")


class RenderError(Exception):
    pass


def clip_id(text):
    """A stable file name for |text|."""
    return "u" + hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]


def voice_key(voice, speed):
    return "%s@%s" % (voice, ("%.2f" % speed).rstrip("0").rstrip("."))


def tts_text(text):
    """What the voice reads: hyphens as spaces, digits and marks the
    lexicon has no entry for said as words, a full stop at the end."""
    t = text.replace("-", " ").replace("%", " percent").replace("&", " and ")
    t = re.sub(r"\b(\d+)\.(\d+)\b", lambda m: m.group(1) + " point " +
               " ".join(vbl.number_words(int(d)) for d in m.group(2)), t)
    t = re.sub(r"\b\d+\b", lambda m: vbl.number_words(int(m.group(0))), t)
    t = re.sub(r"\s+", " ", t).strip()
    return t if t[-1:] in ".?!" else t + "."


def texts_for(utterances, page_texts=()):
    """{id: text} for everything a user says, and the page texts apart."""
    user = {}
    for t in list(utterances) + [WAKE_PHRASE, REPEAT] + gesture_words():
        user.setdefault(clip_id(t), t)
    page = {clip_id(t): t for t in page_texts}
    return user, page


def gesture_words():
    return vbl.gesture_words()


def merged_lexicon(base_path, extra_paths):
    """sherpa-onnx's lexicon with the extra entries before it (first entry
    wins), as one text, and the words it has."""
    entries = {}
    lines = []
    for p in extra_paths:
        for w, ph in vbl.read_new_lexicon(p).items():
            if w not in entries:
                entries[w] = ph
                lines.append("%s || %s" % (w, ph))
    base = vbl.read_new_lexicon(base_path)
    for w, ph in base.items():
        entries.setdefault(w, ph)
    with open(base_path, encoding="utf-8") as f:
        text = "\n".join(lines) + "\n" + f.read()
    return text, set(entries)


def missing_words(texts, words):
    out = set()
    for t in texts:
        for w in vbl.tts_words(tts_text(t)):
            if w not in words:
                out.add(w)
    return sorted(out)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class DriverTts(object):
    """The voice bench's sherpa_bench in its tts mode."""

    def __init__(self, driver, data_dir):
        self.driver = driver
        self.data = data_dir

    def artifact_dir(self, artifact):
        d = os.path.join(self.data, "a", artifact)
        if not os.path.exists(os.path.join(d, ".fetched.json")):
            raise RenderError("missing artifact %s: run tools/voice_bench/fetch.py "
                              "--only %s" % (artifact, artifact))
        return d

    def inputs(self, voices):
        """{file: sha256} of every model file the voices read."""
        out = {}
        for _, artifact, model, _ in voices:
            d = self.artifact_dir(artifact)
            for name in (model, "tokens.txt"):
                out["%s/%s" % (artifact, name)] = sha256_file(os.path.join(d, name))
        lex = os.path.join(self.artifact_dir(LEXICON_ARTIFACT), "lexicon-en-us.txt")
        out["%s/lexicon-en-us.txt" % LEXICON_ARTIFACT] = sha256_file(lex)
        for p in (self.driver,):
            out["driver/" + os.path.basename(p)] = sha256_file(p)
        return out

    def base_lexicon(self):
        return os.path.join(self.artifact_dir(LEXICON_ARTIFACT), "lexicon-en-us.txt")

    def render(self, voice, items, lexicon_path, work):
        """items: [(name, text, speed, out_path)] -> writes each out_path."""
        _, artifact, model, sid = voice
        d = self.artifact_dir(artifact)
        lst = os.path.join(work, "%s.lst" % voice[0])
        with open(lst, "w", encoding="utf-8") as f:
            for name, text, speed, out in items:
                f.write("%s\t%d\t%.4f\t%s\t%s\n" % (name, sid, speed, tts_text(text), out))
        env = dict(os.environ)
        lib = os.path.join(self.artifact_dir(RUNTIME_ARTIFACT), "lib")
        env["LD_LIBRARY_PATH"] = lib + (":" + env["LD_LIBRARY_PATH"]
                                        if env.get("LD_LIBRARY_PATH") else "")
        p = subprocess.run(
            [self.driver, "tts", "--list", lst, "--threads", "1", "--kind", "vits",
             "--model", os.path.join(d, model), "--tokens", os.path.join(d, "tokens.txt"),
             "--lexicon", lexicon_path, "--noise-scale", NOISE_SCALE,
             "--noise-scale-w", NOISE_SCALE],
            capture_output=True, text=True, env=env)
        oov = sorted(set(re.findall(r"OOV (?:word skipped: '([^']+)'|(\S+)\. Ignore)",
                                    p.stderr)))
        if p.returncode != 0:
            raise RenderError("tts failed for %s:\n%s" % (voice[0], p.stderr[-2000:]))
        if oov:
            raise RenderError("%s: words missing from the lexicon: %s (add them to "
                              "tools/voice_e2e/lexicon-extra.txt)" % (voice[0], oov))


def _audio():
    import audio  # noqa: E402 (tools/voice_bench/audio.py; needs numpy)
    return audio


def postprocess(raw_path, out_path):
    """The voice's render, trimmed, at 16 kHz, speech at SPEECH_DB."""
    audio = _audio()
    x, rate = audio.read_wav(raw_path)
    x = audio.trim_silence(x, rate)
    n_out = int(round(len(x) * SR / rate))
    y = audio.resample_to(x, n_out)
    y = audio.set_level(y, (0, len(y)), SPEECH_DB)
    audio.write_wav(out_path, y)


def mix(clip_path, cond, seed):
    """|clip_path| placed with silence around it, under |cond|: (16-bit PCM
    bytes, (speech start, speech end) in seconds)."""
    audio = _audio()
    import numpy as np
    x, _ = audio.read_wav(clip_path)
    rng = audio.rng_for("e2e-mix", seed, cond)
    a = int(LEAD_S * SR)
    y = np.concatenate([np.zeros(a, np.float32), x,
                        np.zeros(int(TRAIL_S * SR), np.float32)])
    span = (a, a + len(x))
    kind, snr = CONDITIONS[cond]
    if kind == "pink":
        y = audio.mix_at_snr(y, audio.pink_noise(len(y), rng), snr, span)
    elif kind == "music":
        y = audio.mix_at_snr(y, audio.synth_music(len(y), rng), snr, span)
    y = audio.finalize(y, rng)
    return pcm16(y), (span[0] / SR, span[1] / SR)


def pcm16(x):
    """Float samples as 16-bit little-endian PCM, rounded as
    audio.write_wav() rounds them."""
    import numpy as np
    y = np.clip(np.round(np.asarray(x, dtype=np.float64) * 32767.0), -32768, 32767)
    return y.astype("<i2").tobytes()


def write_pcm_wav(path, pcm, rate=SR, channels=1):
    import wave
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


def music_bed(out_path, seconds, snr_db, seed="bed"):
    """Music to play into the microphone through a whole scenario: its RMS
    |snr_db| under the speech level (SPEECH_DB)."""
    audio = _audio()
    rng = audio.rng_for("e2e-bed", seed)
    y = audio.synth_music(int(seconds * SR), rng)
    y = y * audio.db_to_gain(SPEECH_DB - snr_db)
    audio.write_wav(out_path, y)


def render_all(tts, out_dir, utterances, page_texts=(), voices=None,
               speeds=SPEEDS, conditions=tuple(CONDITIONS), log=print):
    """Renders what is missing, then writes manifest.json; returns it."""
    voices = voices or VOICES
    os.makedirs(out_dir, exist_ok=True)
    user, page = texts_for(utterances, page_texts)
    lex_text, words = merged_lexicon(tts.base_lexicon(), [KIT_LEXICON, BENCH_LEXICON])
    missing = missing_words(list(user.values()) + list(page.values()), words)
    if missing:
        raise RenderError("words missing from the lexicon: %s (add them to "
                          "tools/voice_e2e/lexicon-extra.txt)" % ", ".join(missing))
    lex_path = os.path.join(out_dir, "lexicon.txt")
    with open(lex_path, "w", encoding="utf-8") as f:
        f.write(lex_text)
    work = os.path.join(out_dir, "work")
    os.makedirs(work, exist_ok=True)
    jobs = [(v, sp) for v in voices for sp in speeds]
    for v, sp in jobs:
        key = voice_key(v[0], sp)
        raw_dir = os.path.join(out_dir, "tts", key)
        clip_dir = os.path.join(out_dir, "clips", key)
        os.makedirs(raw_dir, exist_ok=True)
        os.makedirs(clip_dir, exist_ok=True)
        todo = [(cid, t, sp, os.path.join(raw_dir, cid + ".wav"))
                for cid, t in sorted(user.items())
                if not os.path.exists(os.path.join(clip_dir, cid + ".wav"))]
        if todo:
            log("tts %-16s %4d clips" % (key, len(todo)))
            tts.render(v, todo, lex_path, work)
            for cid, _, _, raw in todo:
                postprocess(raw, os.path.join(clip_dir, cid + ".wav"))
                os.remove(raw)
    if page:
        page_dir = os.path.join(out_dir, "page")
        os.makedirs(page_dir, exist_ok=True)
        raw_dir = os.path.join(out_dir, "tts", PAGE_VOICE[0])
        os.makedirs(raw_dir, exist_ok=True)
        todo = [(cid, t, 1.0, os.path.join(raw_dir, cid + ".wav"))
                for cid, t in sorted(page.items())
                if not os.path.exists(os.path.join(page_dir, cid + ".wav"))]
        if todo:
            log("tts %-16s %4d clips" % (PAGE_VOICE[0], len(todo)))
            tts.render(PAGE_VOICE, todo, lex_path, work)
            for cid, _, _, raw in todo:
                postprocess(raw, os.path.join(page_dir, cid + ".wav"))
    # The mixes: made the same way wherever they are needed (the speech
    # check, file-capture runs); here only their hashes are recorded.
    mixes = {}
    for cond in conditions:
        if cond == "clean":
            continue
        for v, sp in jobs:
            key = voice_key(v[0], sp)
            for cid in sorted(user):
                src = os.path.join(out_dir, "clips", key, cid + ".wav")
                pcm, _ = mix(src, cond, "%s/%s" % (key, cid))
                mixes["%s/%s/%s" % (cond, key, cid)] = hashlib.sha256(pcm).hexdigest()
    manifest = {
        "kit": "voice_e2e render", "sample_rate": SR, "speech_dbfs": SPEECH_DB,
        "noise_scale": NOISE_SCALE, "wake_phrase": WAKE_PHRASE,
        "voices": [{"name": v[0], "artifact": v[1], "model": v[2], "speaker": v[3]}
                   for v in voices],
        "page_voice": {"name": PAGE_VOICE[0], "artifact": PAGE_VOICE[1],
                       "speaker": PAGE_VOICE[3]},
        "speeds": list(speeds), "conditions": list(conditions),
        "inputs": tts.inputs(list(voices) + [PAGE_VOICE]),
        "texts": {cid: t for cid, t in sorted(user.items())},
        "page_texts": {cid: t for cid, t in sorted(page.items())},
        "files": file_hashes(out_dir),
        "mix_pcm_sha256": dict(sorted(mixes.items())),
    }
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    return manifest


def file_hashes(out_dir):
    """{relative path: sha256} of the clips and the page audio."""
    out = {}
    for sub in ("clips", "page"):
        base = os.path.join(out_dir, sub)
        for root, _, files in os.walk(base):
            for name in sorted(files):
                if name.endswith(".wav"):
                    p = os.path.join(root, name)
                    out[os.path.relpath(p, out_dir).replace(os.sep, "/")] = sha256_file(p)
    return dict(sorted(out.items()))


def set_digests(manifest):
    """One SHA-256 per set (each voice and speed's clips, each condition's
    mixes of them, the page audio, the texts), over its files' names and
    SHA-256s in order: what tools/voice_e2e/render_hashes.json records."""
    groups = {}
    for name, sha in manifest.get("files", {}).items():
        parts = name.split("/")
        key = "/".join(parts[:2]) if parts[0] == "clips" else parts[0]
        groups.setdefault(key, []).append("%s %s" % (parts[-1], sha))
    for name, sha in manifest.get("mix_pcm_sha256", {}).items():
        group, _, cid = name.rpartition("/")
        groups.setdefault("mix:" + group, []).append("%s %s" % (cid, sha))
    groups["texts"] = ["%s %s" % kv for kv in sorted(manifest.get("texts", {}).items())]
    return {k: hashlib.sha256("\n".join(sorted(v)).encode("utf-8")).hexdigest()
            for k, v in sorted(groups.items())}


def compare_hashes(manifest, recorded):
    """(same, different, missing) counts of |recorded| ({name: sha256}, of
    sets as set_digests() gives them, or of single files and mixes) against
    |manifest|, and the different names (at most 20)."""
    files = dict(manifest.get("files", {}))
    files.update({"mix:" + k: v for k, v in manifest.get("mix_pcm_sha256", {}).items()})
    files.update(set_digests(manifest))
    same = sum(1 for k, v in recorded.items() if files.get(k) == v)
    missing = sum(1 for k in recorded if k not in files)
    different = [k for k, v in recorded.items() if k in files and files[k] != v]
    return same, len(different), missing, different[:20]


def clip_path(out_dir, voice, speed, text):
    """Where |text| is, as |voice| said it at |speed| (the clean clip)."""
    return os.path.join(out_dir, "clips", voice_key(voice, speed),
                        clip_id(text) + ".wav")


def page_audio_path(out_dir, text):
    return os.path.join(out_dir, "page", clip_id(text) + ".wav")
