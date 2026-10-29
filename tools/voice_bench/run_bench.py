#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Runs the voice bench: builds the driver, then measures every engine in
catalog.py on the sets gen_audio.py rendered. Writes JSON Lines under
<results>/ for report.py.

Steps (--steps, comma-separated; default: all but whisper-cpp):
  build        configure and build driver/ against the fetched runtime
  system       machine facts (CPU model and flags, cores, memory, OS)
  sizes        size on disk of each runtime library and model
  offline      per-utterance recognizers: accuracy on every set
  stream       streaming recognizers with Silero VAD: accuracy, time to
               first partial, end of speech to final result
  bias         streaming transducers, beam search without and with the
               page's names as hotwords (entity accuracy)
  timing       per-utterance decode time and real-time factor at 1, 2 and
               4 threads, load time, peak memory (3 runs each)
  stimes       streaming latency at 1 and 2 threads (3 runs each)
  listen       CPU used while listening in real time (wake phrase + VAD,
               streaming recognizer + VAD), with and without ONNX Runtime's
               spin-waiting
  kws          wake phrases: detections on wake sets and on non-wake
               speech and page audio
  tts          voices: load time, time to first audio, real-time factor,
               peak memory at 1, 2 and 4 threads (3 runs each)
  whisper-cpp  MoltBrowser's shipped engine, whisper.cpp at the pinned tag
               (build_whisper_cpp.sh first): whisper-bench timings, and with
               --ggml-dir also whisper-cli per request as the browser runs it

Every driver run takes the lock file given with --lock (POSIX flock), so
on a shared machine one measurement runs at a time.

Usage:
  python3 -I tools/voice_bench/run_bench.py --dir DATA --build
  python3 -I tools/voice_bench/run_bench.py --dir DATA [--suite quick]
      [--steps offline,stream] [--lock /path/heavy.lock] [--results DIR]
"""

import argparse
import contextlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import catalog  # noqa: E402
import fetch  # noqa: E402
import voice_bench_lib as vbl  # noqa: E402

ALL_STEPS = ["build", "system", "sizes", "offline", "stream", "bias", "timing", "stimes", "listen",
             "kws", "tts"]
ASR_SETS_FULL = ["asr-clean-a", "asr-clean-b", "asr-pink20-a", "asr-pink10-a", "asr-pink5-a",
                 "asr-babble10-a", "asr-music10-a", "asr-echo0-a", "asr-echo0-aec-a",
                 "real-sc", "real-ls"]
# Heavier models run on fewer sets (see docs/VOICE_BENCH.md).
ASR_SETS_TIER = {
    1: ASR_SETS_FULL,
    2: ["asr-clean-a", "asr-clean-b", "asr-pink10-a", "asr-pink5-a", "asr-babble10-a",
        "asr-music10-a", "asr-echo0-a", "real-sc", "real-ls"],
    3: ["asr-clean-a", "asr-pink10-a", "real-sc", "real-ls"],
}
TIMING_CLIPS = 40
THREADS = [1, 2, 4]
STREAM_THREADS = [1, 2]
KWS_THRESHOLDS = [0.15, 0.25, 0.35]
TTS_TEXTS = [
    ("short", "YouTube is open."),
    ("result", "Two: Bohemian Rhapsody, live at Live Aid, Queen, five minutes."),
    ("list", "Twenty results. One: Bohemian Rhapsody, official video, Queen, six minutes. "
             "Two: Bohemian Rhapsody, live at Live Aid, Queen, five minutes. Three: Bohemian "
             "Rhapsody by Pentatonix. Say a number or a name."),
]


# ------------------------------------------------------------------ plumbing


class HeavyLock:
    """The --lock file (POSIX flock). A measurement takes it only when it
    has to run, and keeps it for the next one of the same group (the same
    results directory: one model's sets, one model's timing runs), so a
    model's runs go together; it is let go when the group changes, between
    steps and at the end. Nothing is locked when a result already exists."""

    def __init__(self, path):
        self.path = path if path and os.name != "nt" else ""
        self.f = None
        self.key = None

    def hold(self, key):
        if not self.path:
            return
        if self.f is not None and key != self.key:
            self.release()
        if self.f is None:
            import fcntl
            self.f = open(self.path, "a")
            fcntl.flock(self.f, fcntl.LOCK_EX)
        self.key = key

    def release(self):
        if self.f is not None:
            import fcntl
            fcntl.flock(self.f, fcntl.LOCK_UN)
            self.f.close()
            self.f = None
            self.key = None

    @contextlib.contextmanager
    def held(self, key):
        self.hold(key)
        yield


class Bench:
    def __init__(self, args):
        self.data = os.path.abspath(args.dir)
        self.work = os.path.abspath(args.work or os.path.join(self.data, "work"))
        self.results = os.path.abspath(args.results or os.path.join(self.data, "results"))
        self.lock = HeavyLock(args.lock)
        self.quick = args.suite == "quick"
        self.repeats = args.repeats
        self.threads = args.threads
        self.only_models = set(m for m in args.models.split(",") if m)
        self.args = args
        exe = "sherpa_bench.exe" if os.name == "nt" else "sherpa_bench"
        self.driver = args.driver or next(
            (p for p in (os.path.join(self.data, "build", exe),
                         os.path.join(self.data, "build", "Release", exe))
             if os.path.exists(p)), os.path.join(self.data, "build", exe))

    def art(self, artifact):
        d = os.path.join(self.data, "a", artifact)
        if not os.path.exists(os.path.join(d, ".fetched.json")):
            return None
        return d

    def want(self, model_id):
        return not self.only_models or model_id in self.only_models

    def set_rows(self, name):
        path = os.path.join(self.work, "sets", name + ".tsv")
        if not os.path.exists(path):
            return []
        with open(path, encoding="utf-8") as f:
            return [line.rstrip("\n").split("\t") for line in f if line.strip()]

    def out_path(self, *parts):
        p = os.path.join(self.results, *parts)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        return p

    def run_driver(self, out, mode, list_rows, flags, threads, tag=None, extra_meta=None):
        """Runs the driver once (under the lock); writes its JSON lines to
        out, preceded by a header. Skips if out exists and is complete."""
        if os.path.exists(out):
            with open(out, encoding="utf-8") as f:
                if any('"event": "end"' in line or '"event":"end"' in line for line in f):
                    return out
        lst = out + ".lst"
        with open(lst, "w", encoding="utf-8") as f:
            for r in list_rows:
                f.write("\t".join(r) + "\n")
        cmd = [self.driver, mode, "--list", lst, "--threads", str(threads)] + flags
        t0 = time.time()
        with self.lock.held(os.path.dirname(out)):
            p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                               errors="replace")
        meta = {"event": "run", "mode": mode, "threads": threads, "tag": tag,
                "cmd": [os.path.relpath(c, self.data) if os.path.isabs(c) and
                        c.startswith(self.data) else c for c in cmd],
                "wall_s": round(time.time() - t0, 2), "returncode": p.returncode,
                "stderr_tail": p.stderr[-1500:] if p.returncode else "",
                "hotword_errors": p.stderr.count("Encode hotwords failed"),
                "started": time.strftime("%Y-%m-%dT%H:%M:%S")}
        meta.update(extra_meta or {})
        tmp = out + ".part"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(json.dumps(meta) + "\n")
            f.write(p.stdout)
        os.replace(tmp, out)
        os.remove(lst)
        status = "ok" if p.returncode == 0 else "FAILED %d" % p.returncode
        print("  %-60s %6.1fs %s" % (os.path.relpath(out, self.results), meta["wall_s"],
                                     status), flush=True)
        if p.returncode != 0:
            print(p.stderr[-800:], file=sys.stderr)
        return out

    def model_flags(self, entry):
        d = self.art(entry["artifact"])
        if d is None:
            return None
        flags = []
        if "kind" in entry:
            flags += ["--kind", entry["kind"]]
        for k, v in entry["files"].items():
            flags += [k, os.path.join(d, v)]
        if "tail_pad_ms" in entry:
            flags += ["--tail-pad-ms", str(entry["tail_pad_ms"])]
        return flags

    # -------------------------------------------------------------- steps

    def step_build(self):
        plat = fetch.this_platform()
        rt = self.art("sherpa-onnx-" + plat)
        if rt is None:
            raise SystemExit("fetch the runtime first: fetch.py --set auto")
        build = os.path.join(self.data, "build")
        cfg = ["cmake", "-S", os.path.join(HERE, "driver"), "-B", build,
               "-DSHERPA_ONNX_DIR=" + rt]
        if plat.startswith("win"):
            libs = self.art("sizes-win-x64-shared-lib")
            cfg.append("-DSHERPA_ONNX_LIB_DIR=" + os.path.join(libs, "lib"))
        subprocess.run(cfg, check=True)
        subprocess.run(["cmake", "--build", build, "--config", "Release", "-j", "2"], check=True)
        print("built", self.driver)

    def step_system(self):
        info = {"platform": platform.platform(), "machine": platform.machine(),
                "python": platform.python_version(), "cpu_count": os.cpu_count(),
                "date": time.strftime("%Y-%m-%d")}
        if sys.platform.startswith("linux"):
            info["lscpu"] = subprocess.run(["lscpu"], capture_output=True, text=True).stdout
            try:
                with open("/proc/meminfo") as f:
                    info["meminfo"] = f.readline().strip()
            except OSError:
                pass
        elif sys.platform == "darwin":
            info["sysctl"] = subprocess.run(
                ["sysctl", "machdep.cpu.brand_string", "hw.ncpu", "hw.memsize",
                 "hw.perflevel0.physicalcpu", "hw.perflevel1.physicalcpu"],
                capture_output=True, text=True).stdout
        elif os.name == "nt":
            info["cpu"] = platform.processor()
        p = subprocess.run([self.driver, "info"], capture_output=True, text=True)
        info["driver"] = p.stdout.strip()
        with open(self.out_path("system.json"), "w") as f:
            json.dump(info, f, indent=1)

    def step_sizes(self):
        def du(paths):
            return sum(os.path.getsize(p) for p in paths if os.path.isfile(p))
        out = {"runtimes": {}, "models": {}}
        for a in fetch.load_manifest():
            if a["kind"] in ("runtime", "runtime-size"):
                d = self.art(a["id"])
                if d is None:
                    continue
                libs = {}
                for root, _, files in os.walk(d):
                    for fn in files:
                        if any(fn.endswith(x) for x in (".so", ".dylib", ".dll", ".a", ".lib")) \
                                or ".so." in fn:
                            libs[os.path.relpath(os.path.join(root, fn), d)] = \
                                os.path.getsize(os.path.join(root, fn))
                out["runtimes"][a["id"]] = libs
        for group in ("OFFLINE", "STREAM", "KWS", "TTS"):
            for mid, e in getattr(catalog, group).items():
                d = self.art(e["artifact"])
                if d is None:
                    continue
                files = [os.path.join(d, v) for v in e["files"].values()]
                if "vocoder" in e and self.art(e["vocoder"][0]):
                    files.append(os.path.join(self.art(e["vocoder"][0]), e["vocoder"][1]))
                ops = set()
                ort_format = False
                for fpath in files:
                    if fpath.endswith(".ort"):
                        ort_format = True
                    elif fpath.endswith(".onnx") and os.path.isfile(fpath):
                        ops |= {"%s:%s" % (dm or "ai.onnx", op)
                                for dm, op in vbl.onnx_op_types(fpath)}
                out["models"][mid] = {"group": group, "bytes": du(files),
                                      "files": {os.path.basename(f): os.path.getsize(f)
                                                for f in files if os.path.isfile(f)},
                                      "onnx_ops": sorted(ops), "ort_format": ort_format}
        vad = self.art(catalog.VAD["artifact"])
        if vad:
            vf = os.path.join(vad, catalog.VAD["file"])
            out["models"]["silero-vad"] = {
                "group": "VAD", "bytes": du([vf]),
                "onnx_ops": sorted("%s:%s" % (dm or "ai.onnx", op)
                                   for dm, op in vbl.onnx_op_types(vf))}
        lex = os.path.join(self.work, "lex")
        if os.path.isdir(lex):
            out["lexicons"] = {f: os.path.getsize(os.path.join(lex, f)) for f in os.listdir(lex)}
        with open(self.out_path("sizes.json"), "w") as f:
            json.dump(out, f, indent=1)

    def asr_sets(self, tier):
        sets = ASR_SETS_TIER[tier]
        if self.quick:
            sets = [s for s in sets if s in ("asr-clean-a", "asr-pink10-a", "asr-music10-a",
                                             "asr-echo0-a", "real-sc", "real-ls")]
        return sets

    def step_offline(self):
        for mid, e in catalog.OFFLINE.items():
            flags = self.model_flags(e)
            if flags is None or not self.want(mid):
                continue
            for s in self.asr_sets(e["tier"]):
                rows = [(r[0], r[2]) for r in self.set_rows(s)]
                if rows:
                    # As deployed: Silero VAD finds the speech, the
                    # recognizer gets that span (plus 0.25 s each side).
                    self.run_driver(self.out_path("offline", mid, s + ".jsonl"), "offline",
                                    rows, flags + self.vad_flags(), self.threads)

    def vad_flags(self, silence=0.5):
        vad = self.art(catalog.VAD["artifact"])
        return ["--vad", os.path.join(vad, catalog.VAD["file"]), "--vad-silence", str(silence)]

    def step_stream(self):
        for mid, e in catalog.STREAM.items():
            flags = self.model_flags(e)
            if flags is None or not self.want(mid):
                continue
            for s in self.asr_sets(e["tier"]):
                rows = [(r[0], r[2], r[3], r[4]) for r in self.set_rows(s)]
                if rows:
                    self.run_driver(self.out_path("stream", mid, s + ".jsonl"), "stream", rows,
                                    flags + self.vad_flags(), self.threads)

    def bpe_vocab(self, mid, e):
        """Writes the bpe.vocab sherpa-onnx needs to encode hotwords."""
        d = self.art(e["artifact"])
        out = os.path.join(self.work, "vocab", mid + ".bpe.vocab")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        if e["vocab"] == "bpe.model":
            pieces = vbl.read_sentencepiece(os.path.join(d, "bpe.model"))
        else:
            pieces = vbl.vocab_from_tokens(os.path.join(d, "tokens.txt"))
        vbl.write_bpe_vocab(pieces, out)
        return out

    def step_bias(self):
        pages = vbl.load_pages()
        utts = {u["id"]: u for u in vbl.load_testset(pages=pages)}
        sets = ["asr-clean-a", "asr-pink10-a"] if not self.quick else ["asr-clean-a"]
        for mid, e in catalog.STREAM.items():
            flags = self.model_flags(e)
            if flags is None or not self.want(mid):
                continue
            vocab = self.bpe_vocab(mid, e)
            base = flags + ["--decoding", "modified_beam_search", "--modeling-unit", "bpe",
                            "--bpe-vocab", vocab] + self.vad_flags()
            # The 0.6B model is slow under beam search: clean speech only.
            for s in (sets if e["tier"] == 1 else sets[:1]):
                rows = self.set_rows(s)
                plain = [(r[0], r[2], r[3], r[4]) for r in rows]
                self.run_driver(self.out_path("bias", mid, s + ".beam.jsonl"), "stream", plain,
                                base, self.threads)
                for score in (1.5, 3.0):
                    biased = [(r[0], r[2], r[3], r[4], vbl.hotwords_arg(
                        vbl.page_phrases(pages[utts[r[1]]["page"]]), e["case"]))
                        for r in rows]
                    self.run_driver(
                        self.out_path("bias", mid, "%s.hot%.1f.jsonl" % (s, score)), "stream",
                        biased, base + ["--hotwords-score", str(score)], self.threads,
                        extra_meta={"hotwords_score": score})

    def timing_rows(self):
        rows = self.set_rows("asr-clean-a")
        n = 12 if self.quick else TIMING_CLIPS
        step = max(1, len(rows) // n)
        return rows[::step][:n]

    def step_timing(self):
        rows = self.timing_rows()
        for mid, e in catalog.OFFLINE.items():
            flags = self.model_flags(e)
            if flags is None or not self.want(mid):
                continue
            for t in THREADS:
                for rep in range(self.repeats):
                    self.run_driver(self.out_path("timing", mid, "t%d.r%d.jsonl" % (t, rep)),
                                    "offline", [(r[0], r[2]) for r in rows],
                                    flags + self.vad_flags(), t)

    def step_stimes(self):
        rows = self.timing_rows()
        for mid, e in catalog.STREAM.items():
            flags = self.model_flags(e)
            if flags is None or not self.want(mid):
                continue
            for t in STREAM_THREADS:
                for rep in range(self.repeats):
                    self.run_driver(self.out_path("stimes", mid, "t%d.r%d.jsonl" % (t, rep)),
                                    "stream", [(r[0], r[2], r[3], r[4]) for r in rows],
                                    flags + self.vad_flags(), t)

    def keywords_file(self, kid, e, phrase, threshold):
        d = self.art(e["artifact"])
        if e["unit"] == "bpe":
            pieces = vbl.read_sentencepiece(os.path.join(d, e["bpe_model"]))
            toks = vbl.bpe_encode(phrase.upper(), pieces)
        else:
            lex = {}
            with open(os.path.join(d, e["lexicon"]), encoding="utf-8") as f:
                for line in f:
                    parts = line.split()
                    if parts and parts[0] not in lex:
                        lex[parts[0]] = parts[1:]
            toks = [p for w in phrase.upper().split() for p in lex[w]]
        line = "%s @%s #%.2f\n" % (" ".join(toks), phrase.upper().replace(" ", "_"), threshold)
        out = os.path.join(self.work, "keywords", "%s-%s-%.2f.txt" % (
            kid, phrase.replace(" ", "_"), threshold))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "w", encoding="utf-8") as f:
            f.write(line)
        return out, line.strip()

    def step_kws(self):
        conds = ["clean", "pink10", "babble10", "music10"]
        neg_sets = ["asr-clean-a", "asr-clean-b", "asr-pink10-a", "asr-babble10-a",
                    "asr-music10-a", "asr-echo0-a", "fa-page", "fa-real-sc"]
        thresholds = KWS_THRESHOLDS if not self.quick else [0.25]
        for kid, e in catalog.KWS.items():
            flags = self.model_flags(e)
            if flags is None or not self.want(kid):
                continue
            for phrase in catalog.WAKE_PHRASES:
                tag = phrase.replace(" ", "_")
                # The threshold sweep for the default phrase; the
                # alternatives at the middle setting.
                for th in (thresholds if phrase == catalog.WAKE_PHRASES[0] else [0.25]):
                    kw, line = self.keywords_file(kid, e, phrase, th)
                    kf = flags + ["--keywords", kw]
                    for c in conds:
                        rows = [(r[0], r[2]) for r in self.set_rows("wake-%s-%s" % (tag, c))]
                        if rows:
                            self.run_driver(
                                self.out_path("kws", kid, tag, "%.2f" % th, "wake-%s.jsonl" % c),
                                "kws", rows, kf, 1, extra_meta={"keywords": line})
                    for s in neg_sets:
                        rows = [(r[0], r[2]) for r in self.set_rows(s)]
                        if rows:
                            self.run_driver(
                                self.out_path("kws", kid, tag, "%.2f" % th, s + ".jsonl"), "kws",
                                rows, kf, 1, extra_meta={"keywords": line})

    def step_listen(self):
        """CPU cost of listening all the time, in real time (30 s of page
        audio): the wake-phrase spotter with the VAD, and a streaming
        recognizer with the VAD; 1 and 2 threads, and 2 threads with ONNX
        Runtime's spin-waiting off."""
        import wave
        src = [r[2] for r in self.set_rows("fa-page")]
        if not src:
            return
        clip = os.path.join(self.work, "listen-30s.wav")
        if not os.path.exists(clip):
            with wave.open(src[0], "rb") as w:
                frames = w.readframes(30 * w.getframerate())
                params = w.getparams()
            with wave.open(clip, "wb") as o:
                o.setparams(params)
                o.writeframes(frames)
        nospin = os.path.join(self.work, "ort-nospin.cfg")
        with open(nospin, "w") as f:
            f.write("SessionConfig.session.intra_op.allow_spinning=0\n"
                    "SessionConfig.session.inter_op.allow_spinning=0\n")
        rows = [("listen-30s", clip)]
        variants = [("t1", 1, []), ("t2", 2, []), ("t2-nospin", 2, ["--provider-config", nospin])]
        kid = "kws-gigaspeech-3.3m"
        e = catalog.KWS[kid]
        flags = self.model_flags(e)
        if flags and self.want(kid):
            kw, _ = self.keywords_file(kid, e, catalog.WAKE_PHRASES[0], 0.25)
            for name, t, extra in variants:
                for rep in range(self.repeats):
                    self.run_driver(self.out_path("listen", "kws", "%s.r%d.jsonl" % (name, rep)),
                                    "listen", rows, flags + ["--listen-engine", "kws",
                                                             "--keywords", kw] +
                                    self.vad_flags() + extra, t)
        for mid in ("zipformer-en-2023-06-26", "fastconformer-80ms"):
            e = catalog.STREAM[mid]
            flags = self.model_flags(e)
            if not flags or not self.want(mid):
                continue
            for name, t, extra in variants:
                for rep in range(self.repeats):
                    self.run_driver(self.out_path("listen", mid, "%s.r%d.jsonl" % (name, rep)),
                                    "listen", rows, flags + ["--listen-engine", "stream"] +
                                    self.vad_flags() + extra, t)

    def step_tts(self):
        lex = os.path.join(self.work, "lex")
        for tid, e in catalog.TTS.items():
            d = self.art(e["artifact"])
            if d is None or not self.want(tid):
                continue
            flags = ["--kind", e["kind"]]
            for k, v in e["files"].items():
                flags += [k, os.path.join(d, v)]
            flags += ["--lexicon", os.path.join(lex, e["lexicon"] + ".txt")]
            if "vocoder" in e:
                flags += ["--vocoder", os.path.join(self.art(e["vocoder"][0]), e["vocoder"][1])]
            flags += e.get("extra", [])
            rows = []
            for rep_text in range(1 if self.quick else 3):
                for name, text in TTS_TEXTS:
                    rows.append(("%s-%d" % (name, rep_text), "0", "1.0", text))
            for t in THREADS:
                for rep in range(self.repeats):
                    self.run_driver(self.out_path("tts", tid, "t%d.r%d.jsonl" % (t, rep)), "tts",
                                    rows, flags, t)

    def step_whisper_cpp(self):
        """MoltBrowser's shipped speech path, whisper.cpp at the pinned tag
        (build_whisper_cpp.sh). whisper-bench times the encoder (one 30 s
        window) and the decoder per token, on real ggml models from
        --ggml-dir or else on whisper.cpp's vocabulary-only test models
        (same shapes, F16, no weights: the time is the same, the text is
        not). With real models, whisper-cli also transcribes the clips one
        process per request, as voice_service.cc runs it (-nt -np -ng)."""
        wdir = self.args.whisper_dir or os.path.join(self.data, "whisper.cpp")
        exe = ".exe" if os.name == "nt" else ""
        bench = os.path.join(wdir, "build", "bin", "whisper-bench" + exe)
        cli = self.args.whisper_cli or os.path.join(wdir, "build", "bin", "whisper-cli" + exe)
        ggml = self.args.ggml_dir
        if not os.path.exists(bench):
            print("whisper-cpp: run build_whisper_cpp.sh first")
            return
        for model in ("tiny.en", "base.en", "small.en"):
            real = ggml and os.path.exists(os.path.join(ggml, "ggml-%s.bin" % model))
            mpath = os.path.join(ggml, "ggml-%s.bin" % model) if real else \
                os.path.join(wdir, "models", "for-tests-ggml-%s.bin" % model)
            for t in THREADS:
                for rep in range(self.repeats):
                    out = self.out_path("whispercpp-bench", model, "t%d.r%d.json" % (t, rep))
                    if os.path.exists(out):
                        continue
                    with self.lock.held(os.path.dirname(out)):
                        t0 = time.perf_counter()
                        p = subprocess.run([bench, "-m", mpath, "-t", str(t), "-w", "0", "-ng"],
                                           capture_output=True, text=True)
                        wall = time.perf_counter() - t0
                    timings = {}
                    for line in p.stderr.splitlines():
                        m = re.search(r"(load|mel|encode|decode|batchd|prompt|total) time = +"
                                      r"([0-9.]+) ms(?: / +(\d+) runs \( *([0-9.]+) ms per run\))?",
                                      line)
                        if m:
                            timings[m.group(1)] = {"total_ms": float(m.group(2)),
                                                   "per_run_ms": float(m.group(4) or m.group(2)),
                                                   "runs": int(m.group(3) or 1)}
                    with open(out, "w") as f:
                        json.dump({"model": model, "weights": "real" if real else "test-shapes",
                                   "threads": t, "wall_s": round(wall, 2),
                                   "returncode": p.returncode, "timings": timings,
                                   "system_info": next((x for x in p.stderr.splitlines()
                                                        if "system_info" in x), "")}, f, indent=1)
                    print("  whispercpp-bench/%s t%d r%d: encode %s ms" % (
                        model, t, rep, timings.get("encode", {}).get("per_run_ms")), flush=True)
        if not ggml:
            print("whisper-cpp: no --ggml-dir, so no transcription runs")
            return
        sets = ["asr-clean-a", "asr-pink10-a", "real-sc"] if not self.quick else ["asr-clean-a"]
        for model in ("tiny.en", "base.en", "small.en"):
            mpath = os.path.join(ggml, "ggml-%s.bin" % model)
            if not os.path.exists(mpath):
                continue
            for s in sets:
                out = self.out_path("whispercpp", model, s + ".jsonl")
                if os.path.exists(out):
                    continue
                rows = self.set_rows(s)
                tmpd = os.path.join(self.work, "whispercpp-tmp")
                os.makedirs(tmpd, exist_ok=True)
                lines = [json.dumps({"event": "run", "mode": "whisper-cli", "model": model,
                                     "threads": self.threads})]
                with self.lock.held(os.path.dirname(out)):
                    for r in rows:
                        stem = os.path.join(tmpd, "out")
                        t0 = time.perf_counter()
                        p = subprocess.run([cli, "-m", mpath, "-t", str(self.threads), "-nt",
                                            "-np", "-ng", "-l", "auto", "-otxt", "-of", stem,
                                            "-f", r[2]], capture_output=True, text=True)
                        ms = (time.perf_counter() - t0) * 1000
                        text = ""
                        if p.returncode == 0 and os.path.exists(stem + ".txt"):
                            with open(stem + ".txt", encoding="utf-8", errors="replace") as f:
                                text = f.read().strip()
                        lines.append(json.dumps({"event": "utt", "id": r[0],
                                                 "process_ms": round(ms, 1), "text": text}))
                lines.append(json.dumps({"event": "end", "items": len(rows)}))
                with open(out, "w", encoding="utf-8") as f:
                    f.write("\n".join(lines) + "\n")
                shutil.rmtree(tmpd, ignore_errors=True)
                print("  whispercpp/%s/%s done" % (model, s), flush=True)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--dir", default=fetch.default_dir(), help="fetch.py data directory")
    p.add_argument("--work", default="", help="gen_audio.py output (default <dir>/work)")
    p.add_argument("--results", default="", help="default <dir>/results")
    p.add_argument("--driver", default="")
    p.add_argument("--build", action="store_true", help="only build the driver")
    p.add_argument("--steps", default="", help="comma-separated (default: all)")
    p.add_argument("--models", default="", help="comma-separated catalog ids (default: all)")
    p.add_argument("--suite", choices=["full", "quick"], default="full")
    p.add_argument("--threads", type=int, default=4, help="threads for accuracy runs")
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--lock", default="", help="flock this file around every measurement")
    p.add_argument("--whisper-dir", default="", help="build_whisper_cpp.sh checkout "
                   "(default <dir>/whisper.cpp)")
    p.add_argument("--whisper-cli", default="", help="default: the one in --whisper-dir")
    p.add_argument("--ggml-dir", default="", help="real ggml-{tiny,base,small}.en.bin")
    args = p.parse_args(argv)
    b = Bench(args)
    if args.build:
        b.step_build()
        return 0
    steps = [s for s in args.steps.split(",") if s] or [s for s in ALL_STEPS if s != "build"]
    for s in steps:
        if s not in ALL_STEPS + ["whisper-cpp"]:
            p.error("unknown step %s" % s)
    if "build" not in steps and not os.path.exists(b.driver):
        raise SystemExit("driver missing: run with --build first")
    for s in steps:
        print("== %s" % s, flush=True)
        try:
            getattr(b, "step_" + s.replace("-", "_"))()
        finally:
            b.lock.release()
    return 0


if __name__ == "__main__":
    sys.exit(main())
