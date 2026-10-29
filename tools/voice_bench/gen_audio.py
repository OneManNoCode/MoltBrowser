#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Renders the voice bench's audio test set from testset/ (never committed).

Synthetic voices flatter recognizers: they are clean, consistent and
perfectly articulated, and some share training data with the recognizers.
The real-speech sets (Speech Commands, LibriSpeech samples) are the check
on that; see docs/VOICE_BENCH.md.

What it writes under <data>/work/ (the fetch.py data directory):
  lex/            lexicons for the TTS engines (the engines' own, plus
                  testset/lexicon-extra.txt, plus missing words converted
                  between phone sets)
  tts/<voice>/    the raw TTS renders
  clips/<set>/    16 kHz mono clips: silence, speech, silence
  sets/<set>.tsv  one clip per line: clip, utterance, wav, speech start and
                  end (s), voice, condition[, wake phrase, phrase end]

Sets:
  asr-clean-a, asr-clean-b   all 300 utterances, two different voices each
  asr-<cond>-a               the "a" renders under each condition:
                             pink20, pink10, pink5 (pink noise at that SNR),
                             babble10 (three other voices), music10
                             (synthesized music), echo0 (the browser's own
                             speech at 0 dB, no echo cancellation),
                             echo0-aec (the same after 25 dB of echo
                             cancellation)
  wake-<phrase>-<cond>       the wake phrase, a pause, then a command;
                             cond in clean, pink10, babble10, music10
  fa-page                    page audio: the browser reading pages and
                             prose over music, in 60 s clips (false accepts)
  real-sc                    Speech Commands test words (real speech),
                             padded with silence for streaming
  real-ls                    LibriSpeech and JFK samples (real speech)

Usage:
  python3 -I tools/voice_bench/gen_audio.py --dir DATA [--driver BIN]
      [--threads 4] [--quick]
--quick renders 60 utterances and fewer conditions, for a smoke test.
"""

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import numpy as np  # noqa: E402

import audio  # noqa: E402
import catalog  # noqa: E402
import fetch  # noqa: E402
import voice_bench_lib as vbl  # noqa: E402

CONDITIONS = ["pink20", "pink10", "pink5", "babble10", "music10", "echo0", "echo0-aec"]
WAKE_CONDITIONS = ["clean", "pink10", "babble10", "music10"]
LEAD_S = (0.4, 0.8)
WORK = ""  # set by main(): <data>/work or --work
TRAIL_S = 1.3
SC_WORDS = ["yes", "no", "stop", "go", "up", "down", "left", "right", "on", "off"]
SC_PER_WORD = 40
SC_DIGITS = ["zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"]


def art_dir(data, artifact):
    d = os.path.join(data, "a", artifact)
    if not os.path.exists(os.path.join(d, ".fetched.json")):
        raise SystemExit("missing artifact %s: run fetch.py --only %s" % (artifact, artifact))
    return d


def default_driver(data):
    exe = "sherpa_bench.exe" if os.name == "nt" else "sherpa_bench"
    for p in (os.path.join(data, "build", exe), os.path.join(data, "build", "Release", exe)):
        if os.path.exists(p):
            return p
    raise SystemExit("driver not built: run run_bench.py --build first")


# ------------------------------------------------------------------ lexicons

_ESPEAK_TO_MISAKI = [("e ɪ", "A"), ("a ɪ", "I"), ("o ʊ", "O"), ("a ʊ", "W"),
                     ("ɔ ɪ", "Y"), ("d ʒ", "ʤ"), ("t ʃ", "ʧ")]


def espeak_to_misaki(phones):
    """Converts an espeak-style US entry ("vˈ ɪ d ɪ o ʊ z") to Kokoro's
    misaki phone set ("v ˈ ɪ d ɪ O z"). Approximate, for test audio."""
    chars = [c for c in phones.replace(" ", "") if not unicode_combining(c)]
    s = " ".join(chars)
    s = s.replace("ː", "").replace("ɐ", "ə").replace("ᵻ", "ɪ").replace("ɚ", "ə ɹ")
    s = re.sub(r"\s+", " ", s).strip()
    for a, b in _ESPEAK_TO_MISAKI:
        s = re.sub(r"(?<!\S)%s(?!\S)" % re.escape(a), b, s)
        # Stress marks may sit inside a diphthong in espeak's output.
        s = s.replace(a.replace(" ", " ˈ "), "ˈ " + b).replace(a.replace(" ", " ˌ "), "ˌ " + b)
    s = re.sub(r"ɜ(?! ɹ)", "ɜ ɹ", s)
    return re.sub(r"\s+", " ", s).strip()


def unicode_combining(c):
    import unicodedata
    return unicodedata.combining(c) != 0


def read_new_lexicon(path):
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or "||" not in line:
                continue
            w, _, p = line.partition("||")
            w, p = w.strip(), p.strip()
            if w and p and w not in out:
                out[w] = p
    return out


def read_old_lexicon(path):
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split(" ", 1)
            if len(parts) == 2 and parts[0] and parts[0] not in out:
                out[parts[0]] = parts[1].strip()
    return out


def tts_words(text):
    return [w.lower() for w in re.findall(r"[A-Za-z][A-Za-z']*", text)]


def tts_text(text):
    """What the TTS reads: hyphens as spaces, a full stop at the end."""
    t = text.replace("-", " ").strip()
    return t if t[-1:] in ".?!" else t + "."


def prepare_lexicons(data, texts):
    """Writes work/lex/vits.txt (sherpa-onnx's lexicon-en-us.txt plus the
    test set's extra entries) and work/lex/kokoro.txt (Kokoro's own US
    lexicon plus every needed word it lacks, converted from the first).
    Returns per-lexicon lists of words still missing (should be empty)."""
    lex_dir = os.path.join(WORK, "lex")
    os.makedirs(lex_dir, exist_ok=True)
    base_path = os.path.join(art_dir(data, "tts-lexicon-en-us"), "lexicon-en-us.txt")
    base = read_new_lexicon(base_path)
    extra = read_new_lexicon(os.path.join(HERE, "testset", "lexicon-extra.txt"))
    kok = read_old_lexicon(os.path.join(art_dir(data, "tts-kokoro-v1_0-int8"), "lexicon-us-en.txt"))
    needed = sorted({w for t in texts for w in tts_words(t)})
    merged_new = dict(base)
    merged_new.update(extra)
    with open(base_path, encoding="utf-8") as f, \
            open(os.path.join(lex_dir, "vits.txt"), "w", encoding="utf-8") as out:
        out.write(f.read())
        out.write("\n# voice bench: testset/lexicon-extra.txt\n")
        for w, p in sorted(extra.items()):
            out.write("%s || %s\n" % (w, p))
    added = {}
    for w in needed:
        if w not in kok and w in merged_new:
            added[w] = espeak_to_misaki(merged_new[w])
    with open(os.path.join(lex_dir, "kokoro.txt"), "w", encoding="utf-8") as out:
        for w, p in kok.items():
            out.write("%s %s\n" % (w, p))
        for w, p in sorted(added.items()):
            out.write("%s %s\n" % (w, p))
    missing_vits = [w for w in needed if w not in merged_new]
    missing_kok = [w for w in needed if w not in kok and w not in added]
    return {"vits": missing_vits, "kokoro": missing_kok,
            "kokoro_converted": len(added), "needed_words": len(needed)}


# ------------------------------------------------------------------ TTS


def tts_flags(data, tts_id):
    t = catalog.TTS[tts_id]
    d = art_dir(data, t["artifact"])
    flags = ["--kind", t["kind"]]
    for k, v in t["files"].items():
        flags += [k, os.path.join(d, v)]
    flags += ["--lexicon", os.path.join(WORK, "lex", t["lexicon"] + ".txt")]
    if "vocoder" in t:
        flags += ["--vocoder", os.path.join(art_dir(data, t["vocoder"][0]), t["vocoder"][1])]
    flags += t.get("extra", [])
    return flags


def render(data, driver, jobs, threads):
    """jobs: [(voice, name, text, strict)] -> renders missing ones; returns
    {(voice, name): wav path}. A strict job fails on any word the TTS
    lexicon lacks (the reference would not match the audio); page audio
    is not strict (the TTS skips unknown words, as it would on a page)."""
    voices = {v[0]: v for v in catalog.VOICES + [catalog.BROWSER_VOICE]}
    out, todo = {}, {}
    for voice, name, text, strict in jobs:
        d = os.path.join(WORK, "tts", voice)
        path = os.path.join(d, name + ".wav")
        if (voice, name) in out:
            continue
        out[(voice, name)] = path
        if not os.path.exists(path):
            todo.setdefault((voice, strict), []).append((name, text, path))
    for (voice, strict), items in todo.items():
        _, tts_id, sid, speed, pitch = voices[voice]
        os.makedirs(os.path.join(WORK, "tts", voice), exist_ok=True)
        lst = os.path.join(WORK, "tts", "%s%s.lst" % (voice, "" if strict else "-page"))
        r = 2 ** (pitch / 12.0)
        with open(lst, "w", encoding="utf-8") as f:
            for name, text, path in items:
                f.write("%s\t%d\t%.4f\t%s\t%s\n" % (name, sid, speed / r, tts_text(text),
                                                    path + ".raw.wav"))
        print("tts %-26s %4d clips" % (voice, len(items)), flush=True)
        p = subprocess.run([driver, "tts", "--list", lst, "--threads", str(threads)]
                           + tts_flags(data, tts_id), capture_output=True, text=True)
        oov = sorted(set(re.findall(r"OOV (?:word skipped: '([^']+)'|(\S+)\. Ignore)",
                                    p.stderr)))
        if p.returncode != 0:
            raise SystemExit("tts failed for %s:\n%s" % (voice, p.stderr[-2000:]))
        if oov and strict:
            raise SystemExit("%s: words missing from the TTS lexicon: %s (add them to "
                             "testset/lexicon-extra.txt)" % (voice, oov))
        for name, text, path in items:
            x, rate = audio.read_wav(path + ".raw.wav")
            x = audio.trim_silence(x, rate)
            n_out = int(round(len(x) * audio.SR / rate / r))
            audio.write_wav(path, audio.resample_to(x, n_out))
            os.remove(path + ".raw.wav")
    return out


# ------------------------------------------------------------------ clips


def place(speech, rng, lead=None, trail=TRAIL_S):
    lead = rng.uniform(*LEAD_S) if lead is None else lead
    a = int(lead * audio.SR)
    x = np.concatenate([np.zeros(a, np.float32), speech,
                        np.zeros(int(trail * audio.SR), np.float32)])
    return x, (a, a + len(speech))


def condition(x, span, cond, rng, others):
    """Applies a condition to a placed clip. others: dict with 'babble'
    (list of three speech arrays) and 'echo' (browser speech)."""
    n = len(x)
    if cond == "clean":
        return x
    m = re.fullmatch(r"pink(\d+)", cond)
    if m:
        return audio.mix_at_snr(x, audio.pink_noise(n, rng), float(m.group(1)), span)
    if cond.startswith("babble"):
        b = sum(audio.fit_length(o, n, rng) / (audio.rms(o) + 1e-12) for o in others["babble"])
        return audio.mix_at_snr(x, b, float(cond[6:]), span)
    if cond.startswith("music"):
        return audio.mix_at_snr(x, audio.synth_music(n, rng), float(cond[5:]), span)
    if cond in ("echo0", "echo0-aec"):
        e = others["echo"]
        # The browser starts speaking just before the user and talks over
        # them (no barge-in yet).
        start = max(0, span[0] - int(0.3 * audio.SR))
        echo = np.zeros(n, np.float32)
        seg = audio.fit_length(e, n - start, rng)
        echo[start:] = seg
        snr = 0.0 if cond == "echo0" else 25.0
        return audio.mix_at_snr(x, echo, snr, span)
    raise ValueError(cond)


def write_set(data, name, rows):
    d = os.path.join(WORK, "sets")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name + ".tsv"), "w", encoding="utf-8") as f:
        for r in rows:
            f.write("\t".join(str(c) for c in r) + "\n")


def make_clip(data, set_name, clip_id, speech, cond, others, level_db, seed_parts,
              lead=None):
    rng = audio.rng_for(*seed_parts)
    x, span = place(speech, rng, lead)
    x = audio.set_level(x, span, level_db)
    x = condition(x, span, cond, rng, others)
    x = audio.finalize(x, rng)
    d = os.path.join(WORK, "clips", set_name)
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, clip_id + ".wav")
    audio.write_wav(path, x)
    return path, span[0] / audio.SR, span[1] / audio.SR


def page_readout(page, start=0, count=5):
    """What the browser says when it reads a result list."""
    parts = ["%d results." % len(page["items"])]
    for i, (title, channel) in enumerate(page["items"][start:start + count], start + 1):
        title = re.sub(r"[()\[\]|]", " ", title)
        parts.append("%s: %s, %s." % (vbl.number_words(i), title, channel))
    return " ".join(parts)


def docs_sentences(limit):
    """Plain sentences from the repository's own docs, for long page
    audio. Sentences naming a wake phrase are left out."""
    root = os.path.dirname(os.path.dirname(HERE))
    out = []
    for rel in ("docs/design/TOUCHLESS_BROWSING.md", "docs/design/SANDBOXED_INFERENCE.md",
                "docs/design/AGENT_ACTIONS.md", "docs/FINGERPRINTING.md"):
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            text = f.read()
        text = re.sub(r"```.*?```", " ", text, flags=re.S)
        text = re.sub(r"`[^`]*`|\[([^\]]*)\]\([^)]*\)|[#*>|_]", lambda m: m.group(1) or " ", text)
        for s in re.split(r"(?<=[.!?])\s+", text):
            s = " ".join(s.split())
            words = tts_words(s)
            if 6 <= len(words) <= 30 and not re.search(r"\b(hey|hello) molt", s, re.I):
                out.append(s)
    return out[:limit]


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--dir", default=fetch.default_dir())
    p.add_argument("--driver", default="")
    p.add_argument("--threads", type=int, default=4)
    p.add_argument("--quick", action="store_true")
    p.add_argument("--work", default="", help="output directory (default <dir>/work)")
    args = p.parse_args(argv)
    data = os.path.abspath(args.dir)
    global WORK
    WORK = os.path.abspath(args.work or os.path.join(data, "work"))
    driver = args.driver or default_driver(data)
    pages = vbl.load_pages()
    utts = vbl.load_testset(pages=pages)
    if args.quick:
        utts = utts[::5]
    conds = CONDITIONS if not args.quick else ["pink10", "music10", "echo0"]
    ncmd = 100 if not args.quick else 20
    voices = [v[0] for v in catalog.VOICES]
    cmd_utts = [u for u in utts if u["category"] not in ("chatter",)][::2][:ncmd]
    prose = docs_sentences(400 if not args.quick else 40)

    # Every text the TTS will read.
    texts = [u["text"] for u in utts] + [ph for ph in catalog.WAKE_PHRASES] + prose
    texts += [page_readout(pg, s) for pg in pages.values() for s in (0, 5, 10, 15)]
    texts += [t for pg in pages.values() for it in pg["items"] for t in it]
    miss = prepare_lexicons(data, texts)
    print("lexicons: %(needed_words)d words, %(kokoro_converted)d converted for Kokoro" % miss)
    # Page titles and docs prose are only page audio: their unknown words
    # are allowed (the TTS skips them, as it would on a real page); the
    # test utterances and wake phrases must be complete.
    strict = {w for t in [u["text"] for u in utts] + catalog.WAKE_PHRASES for w in tts_words(t)}
    for lex in ("vits", "kokoro"):
        bad = [w for w in miss[lex] if w in strict]
        if bad:
            raise SystemExit("words missing from the %s lexicon: %s" % (lex, bad))
    os.makedirs(WORK, exist_ok=True)
    with open(os.path.join(WORK, "lexicon_report.json"), "w") as f:
        json.dump(miss, f, indent=1)

    # Which voices read which utterance.
    plan = {}
    for i, u in enumerate(utts):
        rng = audio.rng_for("plan", u["id"])
        a = voices[i % len(voices)]
        b = voices[(i + 1 + int(rng.integers(0, len(voices) - 1))) % len(voices)]
        if b == a:
            b = voices[(i + 1) % len(voices)]
        plan[u["id"]] = (a, b)
    jobs = []
    for u in utts:
        for v in plan[u["id"]]:
            jobs.append((v, u["id"], u["text"], True))
    for i, u in enumerate(cmd_utts):
        v = voices[(i * 5 + 2) % len(voices)]
        jobs.append((v, u["id"], u["text"], True))
        for ph in catalog.WAKE_PHRASES:
            jobs.append((v, "wake-" + ph.replace(" ", "_"), ph.title(), True))
    browser = catalog.BROWSER_VOICE[0]
    readouts = []
    for pid, pg in pages.items():
        for s in (0, 5, 10, 15):
            name = "readout-%s-%d" % (pid, s)
            readouts.append(name)
            jobs.append((browser, name, page_readout(pg, s), False))
    page_voices = ["piper-ljspeech", "kokoro-bf_emma", "piper-libritts-300", "matcha-ljspeech"]
    for i, s in enumerate(prose):
        jobs.append((page_voices[i % len(page_voices)], "prose-%04d" % i, s, False))
    wavs = render(data, driver, jobs, args.threads)

    def load(voice, name):
        x, _ = audio.read_wav(wavs[(voice, name)])
        return x

    by_id = {u["id"]: u for u in utts}
    ids = [u["id"] for u in utts]
    echo_pool = [load(browser, n) for n in readouts]

    # ASR sets.
    sets = {}
    for which, k in (("a", 0), ("b", 1)):
        for cond in ["clean"] + (conds if which == "a" else []):
            name = "asr-%s-%s" % (cond, which)
            rows = []
            for i, uid in enumerate(ids):
                voice = plan[uid][k]
                speech = load(voice, uid)
                rng = audio.rng_for("others", uid, cond)
                others = {
                    "babble": [load(plan[o][1], o) for o in
                               (ids[(i + 7 + int(rng.integers(0, 50))) % len(ids)],
                                ids[(i + 61 + int(rng.integers(0, 50))) % len(ids)],
                                ids[(i + 131 + int(rng.integers(0, 50))) % len(ids)])],
                    "echo": echo_pool[int(rng.integers(0, len(echo_pool)))],
                }
                level = -26.0 + float(audio.rng_for("level", uid, which).uniform(-5, 5))
                path, s, e = make_clip(data, name, uid, speech, cond, others, level,
                                       ("clip", uid, which, cond),
                                       lead=float(audio.rng_for("lead", uid, which).uniform(*LEAD_S)))
                rows.append((uid + "@" + which, uid, path, "%.3f" % s, "%.3f" % e, voice, cond))
            write_set(data, name, rows)
            sets[name] = len(rows)
            print("set %-18s %d clips" % (name, len(rows)), flush=True)

    # Wake-phrase sets: phrase, 0.25-0.5 s pause, command.
    for ph in catalog.WAKE_PHRASES:
        tag = ph.replace(" ", "_")
        for cond in WAKE_CONDITIONS if not args.quick else ["clean", "music10"]:
            name = "wake-%s-%s" % (tag, cond)
            rows = []
            for i, u in enumerate(cmd_utts):
                v = voices[(i * 5 + 2) % len(voices)]
                rng = audio.rng_for("wake", ph, u["id"], cond)
                w = load(v, "wake-" + tag)
                c = load(v, u["id"])
                gap = np.zeros(int(rng.uniform(0.25, 0.5) * audio.SR), np.float32)
                c = c * (audio.rms(w) / (audio.rms(c) + 1e-12))
                speech = np.concatenate([w, gap, c])
                others = {"babble": [load(plan[o][1], o) for o in
                                     (ids[(i + 11) % len(ids)], ids[(i + 101) % len(ids)],
                                      ids[(i + 201) % len(ids)])],
                          "echo": echo_pool[i % len(echo_pool)]}
                level = -26.0 + float(rng.uniform(-5, 5))
                path, s, e = make_clip(data, name, u["id"], speech, cond, others, level,
                                       ("wakeclip", ph, u["id"], cond))
                rows.append((u["id"] + "@" + tag, u["id"], path, "%.3f" % s, "%.3f" % e, v,
                             cond, ph, "%.3f" % (s + len(w) / audio.SR)))
            write_set(data, name, rows)
            sets[name] = len(rows)
            print("set %-30s %d clips" % (name, len(rows)), flush=True)

    # Page audio for false accepts: 60 s clips of readouts and prose over
    # synthesized music (music 10 dB under the speech).
    pool = [load(browser, n) for n in readouts] + \
        [load(page_voices[i % len(page_voices)], "prose-%04d" % i) for i in range(len(prose))]
    rows, clip, k, n_clip = [], [], 0, 0
    target = 60 * audio.SR
    d = os.path.join(WORK, "clips", "fa-page")
    os.makedirs(d, exist_ok=True)
    for x in pool:
        clip.append(x)
        clip.append(np.zeros(int(0.4 * audio.SR), np.float32))
        k += len(x) + int(0.4 * audio.SR)
        if k >= target:
            rng = audio.rng_for("fa-page", n_clip)
            y = np.concatenate(clip)
            y = audio.set_level(y, (0, len(y)), -24.0)
            y = audio.mix_at_snr(y, audio.synth_music(len(y), rng), 10.0, (0, len(y)))
            y = audio.finalize(y, rng)
            path = os.path.join(d, "page-%03d.wav" % n_clip)
            audio.write_wav(path, y)
            rows.append(("page-%03d" % n_clip, "-", path, "0", "%.3f" % (len(y) / audio.SR),
                         "page", "music10"))
            clip, k, n_clip = [], 0, n_clip + 1
    write_set(data, "fa-page", rows)
    sets["fa-page"] = len(rows)

    # Real speech: Speech Commands words (padded so a VAD can close), and
    # LibriSpeech/JFK samples that ship with the models.
    sc = os.path.join(data, "a", "real-speech-commands-test")
    if os.path.exists(os.path.join(sc, ".fetched.json")):
        rows = []
        d = os.path.join(WORK, "clips", "real-sc")
        os.makedirs(d, exist_ok=True)
        picks = []
        for word in SC_WORDS:
            files = sorted(os.listdir(os.path.join(sc, word)))[:SC_PER_WORD if not args.quick else 4]
            picks += [(word, os.path.join(sc, word, f)) for f in files]
        for f in sorted(os.listdir(os.path.join(sc, "_unknown_"))):
            word = f.split("_", 1)[0]
            if word in SC_DIGITS:
                picks.append((word, os.path.join(sc, "_unknown_", f)))
        for word, src in picks:
            x, rate = audio.read_wav(src)
            if rate != audio.SR:
                continue
            rng = audio.rng_for("sc", src)
            y, span = place(x, rng, lead=0.3, trail=TRAIL_S)
            clip_id = "sc-%s-%s" % (word, os.path.basename(src).split(".")[0])
            path = os.path.join(d, clip_id + ".wav")
            audio.write_wav(path, audio.finalize(y, rng))
            rows.append((clip_id, "sc:" + word, path, "%.3f" % (span[0] / audio.SR),
                         "%.3f" % (span[1] / audio.SR), "real", "speech-commands"))
        write_set(data, "real-sc", rows)
        sets["real-sc"] = len(rows)
        all_rows = []
        for sub in sorted(os.listdir(sc)):
            sd = os.path.join(sc, sub)
            if os.path.isdir(sd):
                for f in sorted(os.listdir(sd)):
                    all_rows.append(("scall-%s-%s" % (sub, f.split(".")[0]), "-",
                                     os.path.join(sd, f), "0", "1", "real", "speech-commands"))
        write_set(data, "fa-real-sc", all_rows)
        sets["fa-real-sc"] = len(all_rows)
    ls_rows = []
    zf = os.path.join(data, "a", "stream-zipformer-en-2023-06-26", "test_wavs")
    trans = {}
    if os.path.exists(os.path.join(zf, "trans.txt")):
        with open(os.path.join(zf, "trans.txt"), encoding="utf-8") as f:
            for line in f:
                k2, _, v2 = line.strip().partition(" ")
                trans[k2] = v2
        for k2, v2 in sorted(trans.items()):
            src = os.path.join(zf, k2 if k2.endswith(".wav") else k2 + ".wav")
            if os.path.exists(src):
                x, rate = audio.read_wav(src)
                rng = audio.rng_for("ls", k2)
                y, span = place(x, rng, lead=0.3)
                path = os.path.join(WORK, "clips", "real-ls-" + os.path.basename(src))
                audio.write_wav(path, audio.finalize(y, rng))
                ls_rows.append(("ls-" + k2.replace(".wav", ""), "ls:" + v2, path,
                                "%.3f" % (span[0] / audio.SR), "%.3f" % (span[1] / audio.SR),
                                "real", "librispeech"))
    jfk = os.path.join(data, "a", "real-jfk", "jfk.wav")
    if os.path.exists(jfk):
        x, rate = audio.read_wav(jfk)
        rng = audio.rng_for("jfk")
        y, span = place(x, rng, lead=0.3)
        path = os.path.join(WORK, "clips", "real-ls-jfk.wav")
        audio.write_wav(path, audio.finalize(y, rng))
        ls_rows.append(("jfk", "ls:And so my fellow Americans ask not what your country can do "
                        "for you ask what you can do for your country", path,
                        "%.3f" % (span[0] / audio.SR), "%.3f" % (span[1] / audio.SR), "real",
                        "jfk"))
    write_set(data, "real-ls", ls_rows)
    sets["real-ls"] = len(ls_rows)
    with open(os.path.join(WORK, "sets", "index.json"), "w") as f:
        json.dump({"sets": sets, "plan": plan, "quick": args.quick,
                   "lexicons": miss}, f, indent=1)
    print("done: %d sets" % len(sets))
    return 0


if __name__ == "__main__":
    sys.exit(main())
