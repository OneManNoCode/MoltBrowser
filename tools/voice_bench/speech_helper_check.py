#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Checks a built molt-speech (docs/design/SPEECH_HELPER_PROTOCOL.md) from the
outside, over its stdin and stdout, as the browser drives it.

  speech_helper_check.py --helper speech/molt-speech --models speech/models \\
      [--dir <voice bench data dir>] STEP [STEP ...] [--json results.json]

Steps:
  protocol   malformed, oversize and unknown frames; CONFIG errors; CONFIG
             between audio frames; RESET in an utterance; SHUTDOWN; end of
             input (also in the middle of a frame); a browser that stops
             reading stdout (the helper must exit, status 3).
  accuracy   the bench's clean set a (300 utterances), fed as fast as the
             helper takes it, every utterance counting:
             WER, command and entity accuracy, against the bench's Kroko
             numbers (docs/VOICE_BENCH.md §3.1, §3.7).
  wake       the bench's "hey molt" wake clips (phrase, pause, command):
             how many wake, and how many commands then come out right; the
             chatter of set a and the page audio, for false wake-ups.
  latency    40 clean clips in real time: from the end of speech to final
             (and to speech_end), the first partial, peak memory.
  listen     CPU while waiting for the wake phrase (page audio in real
             time), and with every utterance counting.
  bias       the accuracy set again, each utterance after a CONFIG with the
             names of the screen it is spoken at as hotwords: how many of
             the names on screen come out right (§3.7 of the bench).

The audio comes from tools/voice_bench/gen_audio.py (work/sets/*.tsv under
--dir). Python standard library only (the bench's scoring from
voice_bench_lib); run with python3 -I.
"""

import argparse
import json
import os
import struct
import subprocess
import sys
import threading
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voice_bench_lib as vbl  # noqa: E402

SR = 16000
AUDIO, CONFIG, RESET, SHUTDOWN = 1, 2, 3, 4


def frame(kind, payload=b""):
    return struct.pack("<II", kind, len(payload)) + payload


def config(obj):
    return frame(CONFIG, json.dumps(obj).encode("utf-8"))


def read_pcm(path):
    """16 kHz mono 16-bit PCM bytes of a WAV."""
    with wave.open(path, "rb") as w:
        if w.getframerate() != SR or w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise ValueError("%s: need 16 kHz mono 16-bit" % path)
        return w.readframes(w.getnframes())


def silence(seconds):
    return b"\x00\x00" * int(seconds * SR)


def audio_frames(pcm, ms=100):
    step = SR * ms // 1000 * 2
    return [frame(AUDIO, pcm[i:i + step]) for i in range(0, len(pcm), step)]


class Helper:
    """One molt-speech process: frames in, timestamped JSON lines out."""

    def __init__(self, args, extra=(), read=True, env_threads=None):
        cmd = [args.helper, "--models", args.models]
        if env_threads:
            cmd += ["--asr-threads", str(env_threads)]
        cmd += list(extra)
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
        self.lines = []
        self.bad = []
        self.cond = threading.Condition()
        self.stderr = b""
        threading.Thread(target=self._err, daemon=True).start()
        if read:
            threading.Thread(target=self._out, daemon=True).start()

    def _err(self):
        self.stderr = self.proc.stderr.read()

    def _out(self):
        for raw in self.proc.stdout:
            t = time.monotonic()
            try:
                ev = json.loads(raw)
                if not isinstance(ev, dict) or len(raw) > 4096:
                    raise ValueError("not a bounded object")
            except ValueError:
                self.bad.append(raw)
                continue
            with self.cond:
                self.lines.append((t, ev))
                self.cond.notify_all()

    def send(self, data):
        try:
            self.proc.stdin.write(data)
            self.proc.stdin.flush()
        except BrokenPipeError:
            raise SystemExit("the helper exited (status %s): %s %s" % (
                self.proc.poll(), [ev for _, ev in self.lines[-2:]],
                self.stderr[-300:]))

    def wait_for(self, pred, timeout, start=0):
        """The first event (from index |start|) matching pred, or None."""
        end = time.monotonic() + timeout
        with self.cond:
            while True:
                for i in range(start, len(self.lines)):
                    if pred(self.lines[i][1]):
                        return i
                left = end - time.monotonic()
                if left <= 0 or self.proc.poll() is not None and not self._more():
                    return None
                self.cond.wait(min(left, 0.05))

    def _more(self):
        return False

    def types(self, start=0):
        return [ev.get("type") for _, ev in self.lines[start:]]

    def close(self, timeout=5):
        try:
            self.send(frame(SHUTDOWN))
            self.proc.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return None

    def peak_rss_kb(self):
        try:
            with open("/proc/%d/status" % self.proc.pid) as f:
                for line in f:
                    if line.startswith("VmHWM:"):
                        return int(line.split()[1])
        except OSError:
            return None

    def cpu_seconds(self):
        try:
            with open("/proc/%d/stat" % self.proc.pid) as f:
                fields = f.read().rsplit(")", 1)[1].split()
            ticks = os.sysconf("SC_CLK_TCK")
            return (int(fields[11]) + int(fields[12])) / ticks
        except OSError:
            return None


def ready(h):
    i = h.wait_for(lambda e: e.get("type") == "ready", 60)
    if i is None:
        raise SystemExit("no ready line; stderr: %s" % h.stderr[-500:])
    return h.lines[i][1]


def load_set(data, name):
    path = os.path.join(data, "work", "sets", name + ".tsv")
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            rows.append(line.rstrip("\n").split("\t"))
    return rows


# ------------------------------------------------------------------ protocol


def expect_error(args, data, code, results, name, within=3.0):
    h = Helper(args)
    ready(h)
    t0 = time.monotonic()
    try:
        h.send(data)
    except BrokenPipeError:
        pass
    status = None
    try:
        status = h.proc.wait(within)
    except subprocess.TimeoutExpired:
        h.proc.kill()
    took = time.monotonic() - t0
    time.sleep(0.1)
    errors = [ev for _, ev in h.lines if ev.get("type") == "error"]
    ok = status == 2 and len(errors) == 1 and errors[0].get("code") == code
    results[name] = {"ok": ok, "status": status, "error": errors[-1] if errors else None,
                     "seconds": round(took, 3)}
    print("%-34s %s  status=%s %s" % (name, "ok " if ok else "BAD", status,
                                      errors[-1] if errors else ""))
    return ok


def step_protocol(args):
    r = {}
    data_dir = args.dir
    # A clip with speech, for the stateful cases.
    clip = None
    if data_dir and os.path.exists(os.path.join(data_dir, "work", "sets", "asr-clean-a.tsv")):
        rows = load_set(data_dir, "asr-clean-a")
        clip = read_pcm(rows[0][2])
        clip2 = read_pcm(rows[1][2])
    h = Helper(args)
    ev = ready(h)
    r["ready"] = {"ok": ev.get("protocol") == 1 and isinstance(ev.get("models"), list),
                  "line": ev}
    print("ready", ev)
    h.close()

    expect_error(args, frame(9, b""), "protocol", r, "unknown type")
    expect_error(args, struct.pack("<II", AUDIO, 70000), "protocol", r,
                 "oversize length (header only)")
    expect_error(args, struct.pack("<II", CONFIG, 0xFFFFFFFF), "protocol", r,
                 "length 4 GiB (header only)")
    expect_error(args, frame(AUDIO, b"\x01\x02\x03"), "protocol", r, "odd AUDIO")
    expect_error(args, frame(AUDIO, b""), "protocol", r, "empty AUDIO")
    expect_error(args, frame(RESET, b"x"), "protocol", r, "RESET with payload")
    expect_error(args, frame(CONFIG, b"{'a':1}"), "protocol", r, "CONFIG not JSON")
    expect_error(args, frame(CONFIG, b'{"a":' + b"[" * 20 + b"]" * 20 + b"}"),
                 "protocol", r, "CONFIG too deep")
    expect_error(args, config({"vad_silence_ms": 50}), "config", r, "CONFIG out of range")
    expect_error(args, config({"wake_required": "yes"}), "config", r, "CONFIG wrong type")
    expect_error(args, config({"wake_phrases": ["hey mölt"]}), "config", r,
                 "CONFIG wake phrase not ASCII")
    expect_error(args, config({"wake_phrases": ["hey molt 2"]}), "config", r,
                 "CONFIG wake phrase digit")
    # Garbage after a good frame, split across writes.
    expect_error(args, config({"hotwords": ["Queen"]}) + struct.pack("<II", 7, 0), "protocol",
                 r, "good frame then an unknown one", within=3)

    # Hotwords full of the engine's syntax: accepted and harmless.
    h = Helper(args)
    ready(h)
    h.send(config({"hotwords": ["AC/DC :9.0", "#1 @home", "a/b/c", "x" * 1000,
                                "\u202ebidi\u202c", "Beyonc\u00e9", "o'clock"] * 50,
                   "wake_required": False}))
    if clip:
        for f in audio_frames(clip + silence(1.0)):
            h.send(f)
        got = h.wait_for(lambda e: e.get("type") == "final", 20)
    else:
        got = 0
    status = h.close()
    r["hostile hotwords"] = {"ok": got is not None and status == 0, "status": status}
    print("%-34s %s" % ("hostile hotwords", "ok " if r["hostile hotwords"]["ok"] else "BAD"))

    if clip:
        # CONFIG between audio frames, and a RESET in an utterance.
        h = Helper(args)
        ready(h)
        frames = audio_frames(clip + silence(1.0))
        half = len(frames) // 2
        h.send(config({"wake_required": False}))
        for f in frames[:half]:
            h.send(f)
        h.send(config({"hotwords": ["Queen"], "vad_silence_ms": 700}))
        for f in frames[half:]:
            h.send(f)
        i = h.wait_for(lambda e: e.get("type") == "final", 20)
        r["CONFIG between frames"] = {"ok": i is not None, "events": h.types()}
        print("%-34s %s %s" % ("CONFIG between frames", "ok " if i is not None else "BAD",
                               h.types()))
        n = len(h.lines)
        frames2 = audio_frames(clip2 + silence(1.5))
        # Into the second utterance, then RESET, then silence.
        for f in frames2[:len(frames2) // 2]:
            h.send(f)
        h.wait_for(lambda e: e.get("type") == "speech_start", 10, start=n)
        h.send(frame(RESET))
        h.send(b"".join(audio_frames(silence(3.0))))
        time.sleep(1.5)
        after = h.lines[n:]
        epoch0_final = [ev for _, ev in after if ev.get("type") == "final" and ev.get("epoch") == 0]
        epoch1 = [ev for _, ev in after if ev.get("epoch") == 1]
        status = h.close()
        ok = not epoch0_final and status == 0
        r["RESET in an utterance"] = {"ok": ok, "events": [ev for _, ev in after],
                                      "epoch1": len(epoch1)}
        print("%-34s %s %s" % ("RESET in an utterance", "ok " if ok else "BAD",
                               [ev.get("type") for _, ev in after]))

    # SHUTDOWN, end of input, end of input inside a frame.
    for name, tail in (("SHUTDOWN", frame(SHUTDOWN)), ("end of input", b""),
                       ("end of input mid-frame", struct.pack("<II", AUDIO, 3200) + b"\x00" * 100)):
        h = Helper(args)
        ready(h)
        h.send(b"".join(audio_frames(silence(1.0))))
        t0 = time.monotonic()
        h.send(tail)
        h.proc.stdin.close()
        try:
            status = h.proc.wait(5)
        except subprocess.TimeoutExpired:
            h.proc.kill()
            status = None
        took = time.monotonic() - t0
        ok = status == 0 and took < 1.0
        r[name] = {"ok": ok, "status": status, "seconds": round(took, 3)}
        print("%-34s %s status=%s in %.3f s" % (name, "ok " if ok else "BAD", status, took))

    # A browser that stops reading stdout: the helper must give up (status
    # 3), not block forever. The pipe is made small so that it fills fast.
    if clip:
        h = Helper(args, read=False)
        try:
            import fcntl
            fcntl.fcntl(h.proc.stdout.fileno(), 1031, 4096)  # F_SETPIPE_SZ
        except (ImportError, OSError):
            pass
        h.send(config({"wake_required": False}))
        t0 = time.monotonic()
        status = None
        frames = audio_frames(clip + silence(1.0))
        try:
            while time.monotonic() - t0 < 120:
                for f in frames:
                    h.proc.stdin.write(f)
                    h.proc.stdin.flush()
                status = h.proc.poll()
                if status is not None:
                    break
        except BrokenPipeError:
            pass
        try:
            status = h.proc.wait(10)
        except subprocess.TimeoutExpired:
            h.proc.kill()
        took = time.monotonic() - t0
        ok = status == 3
        r["reader stops reading"] = {"ok": ok, "status": status, "seconds": round(took, 1)}
        print("%-34s %s status=%s after %.1f s" % ("reader stops reading", "ok " if ok else "BAD",
                                                  status, took))
    bad = [k for k, v in r.items() if not v.get("ok")]
    print("protocol: %d checks, %d failed %s" % (len(r), len(bad), bad))
    return r


# ------------------------------------------------------------------ accuracy


def run_clips(h, rows, realtime=False, pad=1.0, reset_after=False, wait=20.0,
              before=None):
    """Feeds each clip and collects its events. A clip is done when nothing
    it opened (a wake, a speech_start) waits for its final and the helper
    has been quiet for a moment; with |reset_after|, a RESET ends it after
    at most |wait| seconds."""
    out = []
    for row in rows:
        clip_id, path = row[0], row[2]
        pcm = read_pcm(path) + silence(pad)
        n = len(h.lines)
        if before:
            h.send(before(row))
        t_start = time.monotonic()
        if realtime:
            for k, f in enumerate(audio_frames(pcm)):
                due = t_start + (k + 1) * 0.1
                now = time.monotonic()
                if due > now:
                    time.sleep(due - now)
                h.send(f)
        else:
            h.send(b"".join(audio_frames(pcm)))
        sent = time.monotonic()
        settle = 0.3 if realtime else 0.6 + len(pcm) / 2 / SR * 0.1
        while True:
            now = time.monotonic()
            pending = False
            last = sent
            for t, e in h.lines[n:]:
                last = max(last, t)
                if e.get("type") in ("wake", "speech_start"):
                    pending = True
                elif e.get("type") == "final":
                    pending = False
            if not pending and now - last >= settle:
                break
            if now - sent > wait:
                break
            if h.proc.poll() is not None:
                break
            time.sleep(0.02)
        recs = h.lines[n:]
        if reset_after:
            h.send(frame(RESET))
        out.append({"id": clip_id, "row": row, "t_start": t_start, "events": recs})
    return out


def finals_text(rec):
    return " ".join(ev.get("text", "") for _, ev in rec["events"] if ev.get("type") == "final").strip()


def score_rows(recs, utts):
    scores = []
    per = {}
    for rec in recs:
        uid = rec["row"][1]
        if uid not in utts:
            continue
        s = vbl.score_utterance(utts[uid], finals_text(rec))
        s["id"] = rec["id"]
        s["category"] = utts[uid]["category"]
        scores.append(s)
        per[rec["id"]] = s["hyp"]
    return scores, per


def bench_reference(args, name):
    """The bench's own Kroko transcripts for set |name| (beam search, no
    hotwords), from its results directory, if present."""
    path = os.path.join(args.bench_results or "", "bias", "kroko-en", name + ".beam.jsonl")
    if not os.path.exists(path):
        return None
    out = {}
    for rec in vbl.read_jsonl(path):
        if rec.get("event") == "utt":
            out[rec["id"]] = rec.get("text", "")
    return out


def names_on_screen(recs, utts, pages):
    """Share of the names in the utterances that the screen shows (and
    that it doesn't) that came out right, as the bench counts them
    (docs/VOICE_BENCH.md §3.7)."""
    page_text = {pid: vbl.normalize(" ".join(t for it in p["items"] for t in it))
                 for pid, p in pages.items()}
    on, off = [], []
    for rec in recs:
        u = utts.get(rec["row"][1])
        if not u:
            continue
        hyp = vbl.normalize(finals_text(rec))
        for _, forms in u["entities"]:
            hit = vbl.contains_words(hyp, forms)
            (on if vbl.contains_words(page_text[u["page"]], forms) else off).append(hit)
    return {"on_screen": sum(on) / len(on) if on else None, "n_on": len(on),
            "not_on_screen": sum(off) / len(off) if off else None, "n_off": len(off)}


def step_accuracy(args, fast=False, bias=False):
    pages = vbl.load_pages()
    utts = {u["id"]: u for u in vbl.load_testset(pages=pages)}
    rows = load_set(args.dir, "asr-clean-a")
    h = Helper(args, env_threads=args.asr_threads)
    ready(h)
    h.send(config({"wake_required": False}))
    t0 = time.monotonic()
    before = None
    if bias:
        # The names of the screen the utterance is spoken at, as the
        # browser sends them: titles split into names, and channels.
        def before(row):
            return config({"hotwords": vbl.page_phrases(pages[utts[row[1]]["page"]])})
    recs = run_clips(h, rows, realtime=False, before=before)
    took = time.monotonic() - t0
    rss = h.peak_rss_kb()
    status = h.close()
    scores, per = score_rows(recs, utts)
    agg = vbl.aggregate(scores)
    multi = sum(1 for rec in recs if sum(1 for _, e in rec["events"] if e.get("type") == "final") > 1)
    none = sum(1 for rec in recs if not any(e.get("type") == "final" for _, e in rec["events"]))
    res = {"set": "asr-clean-a", "hotwords": bias, "clips": len(recs), "wer": agg["wer"],
           "names": names_on_screen(recs, utts, pages),
           "command_acc": agg["command_acc"], "entity_acc": agg["entity_acc"],
           "clips_with_several_finals": multi, "clips_without_final": none,
           "seconds": round(took, 1), "peak_rss_kb": rss, "exit": status,
           "by_category": {}}
    for cat in sorted({s["category"] for s in scores}):
        a = vbl.aggregate([s for s in scores if s["category"] == cat])
        res["by_category"][cat] = round(a["command_acc"], 3)
    ref = bench_reference(args, "asr-clean-a")
    if ref:
        ref_scores = [dict(vbl.score_utterance(utts[rec["row"][1]], ref[rec["id"]]), id=rec["id"])
                      for rec in recs if rec["id"] in ref and rec["row"][1] in utts]
        ragg = vbl.aggregate(ref_scores)
        same = sum(1 for s in ref_scores if per.get(s["id"]) == s["hyp"])
        res["bench_same_clips"] = {"wer": ragg["wer"], "command_acc": ragg["command_acc"],
                                   "entity_acc": ragg["entity_acc"], "n": ragg["n"],
                                   "identical_transcripts": same}
    print("accuracy%s: WER %.1f%%, commands %.1f%%, entities %.1f%% over %d clips in %.0f s"
          % (" (with the screen's hotwords)" if bias else "", 100 * agg["wer"],
             100 * agg["command_acc"], 100 * agg["entity_acc"], len(recs), took))
    print("  names on screen right:", res["names"])
    if ref:
        b = res["bench_same_clips"]
        print("  bench (driver, same clips): WER %.1f%%, commands %.1f%%; identical transcripts %d/%d"
              % (100 * b["wer"], 100 * b["command_acc"], b["identical_transcripts"], b["n"]))
    res["transcripts"] = {k: v for k, v in sorted(per.items())}
    return res


WAKE_LIKE = {"hey", "hay", "hi", "a", "hey,", "molt", "mold", "malt", "moult", "melt",
             "bolt", "halt", "mult", "volt", "moat", "mote", "most", "moll"}


def strip_wake(text):
    words = vbl.normalize(text).split()
    i = 0
    while i < len(words) and i < 3 and words[i] in WAKE_LIKE:
        i += 1
    return " ".join(words[i:])


def step_wake(args):
    utts = {u["id"]: u for u in vbl.load_testset()}
    res = {}
    rows = load_set(args.dir, "wake-hey_molt-clean")
    h = Helper(args, env_threads=args.asr_threads)
    ready(h)
    recs = run_clips(h, rows, pad=7.0)
    status = h.close()
    woke = 0
    cmd_ok = 0
    scores = []
    for rec in recs:
        types = [e.get("type") for _, e in rec["events"]]
        if "wake" in types:
            woke += 1
            text = strip_wake(finals_text(rec))
            s = vbl.score_utterance(utts[rec["row"][1]], text)
            scores.append(s)
            cmd_ok += s["command_ok"]
    agg = vbl.aggregate(scores) if scores else {}
    res["wake_clean"] = {"clips": len(recs), "woke": woke, "false_reject": 1 - woke / len(recs),
                         "commands_ok_after_wake": cmd_ok,
                         "command_wer_after_wake": agg.get("wer"), "exit": status}
    print("wake: %d/%d woke (FR %.0f%%); commands right after the wake: %d (WER %.1f%%)"
          % (woke, len(recs), 100 * (1 - woke / len(recs)), cmd_ok, 100 * (agg.get("wer") or 0)))

    # False wake-ups: the chatter of set a, then the whole set a, then page audio.
    rows = load_set(args.dir, "asr-clean-a")
    chatter = [r for r in rows if utts.get(r[1], {}).get("category") == "chatter"]
    for name, sel in (("chatter", chatter), ("asr-clean-a", rows)):
        h = Helper(args)
        ready(h)
        recs = run_clips(h, sel, pad=0.5, reset_after=True, wait=3.0)
        status = h.close()
        wakes = [(rec["id"], rec["row"][1]) for rec in recs
                 if any(e.get("type") == "wake" for _, e in rec["events"])]
        hours = sum(len(read_pcm(r[2])) / 2 / SR for r in sel) / 3600
        res["false_wake_" + name] = {"clips": len(sel), "hours": round(hours, 3),
                                     "wakes": len(wakes), "which": wakes, "exit": status}
        print("false wakes on %s: %d in %.2f h %s" % (name, len(wakes), hours,
                                                     [utts[u]["text"] for _, u in wakes][:5]))
    rows = load_set(args.dir, "fa-page")
    h = Helper(args)
    ready(h)
    recs = run_clips(h, rows, pad=0.5, reset_after=True, wait=3.0)
    status = h.close()
    n = sum(1 for rec in recs for _, e in rec["events"] if e.get("type") == "wake")
    hours = sum(len(read_pcm(r[2])) / 2 / SR for r in rows) / 3600
    res["false_wake_page_audio"] = {"clips": len(rows), "hours": round(hours, 3), "wakes": n,
                                    "exit": status}
    print("false wakes on page audio: %d in %.2f h" % (n, hours))
    return res


# ------------------------------------------------------------------ timing


def step_latency(args):
    rows = load_set(args.dir, "asr-clean-a")[:40]
    out = {}
    for threads in (1, 2):
        h = Helper(args, env_threads=threads)
        ready(h)
        h.send(config({"wake_required": False}))
        recs = run_clips(h, rows, realtime=True, pad=1.5)
        rss = h.peak_rss_kb()
        status = h.close()
        to_final, to_end, first_partial = [], [], []
        for rec in recs:
            speech_end = float(rec["row"][4])
            speech_start = float(rec["row"][3])
            t0 = rec["t_start"]
            fin = [t for t, e in rec["events"] if e.get("type") == "final"]
            end = [t for t, e in rec["events"] if e.get("type") == "speech_end"]
            part = [t for t, e in rec["events"] if e.get("type") == "partial"]
            if fin:
                to_final.append(1000 * (fin[-1] - t0 - speech_end))
            if end:
                to_end.append(1000 * (end[-1] - t0 - speech_end))
            if part:
                first_partial.append(1000 * (part[0] - t0 - speech_start))
        out["threads_%d" % threads] = {
            "clips": len(recs),
            "end_of_speech_to_final_ms_median": round(vbl.median(to_final), 1),
            "end_of_speech_to_final_ms_p90": round(vbl.percentile(to_final, 90), 1),
            "end_of_speech_to_speech_end_ms_median": round(vbl.median(to_end), 1),
            "speech_end_to_final_ms_median": round(vbl.median(
                [a - b for a, b in zip(to_final, to_end)]), 1),
            "first_partial_ms_median": round(vbl.median(first_partial), 1),
            "peak_rss_kb": rss, "exit": status}
        print("latency, %d thread(s):" % threads, out["threads_%d" % threads])
    return out


def listen(args, rows, cfg, seconds, threads=1):
    h = Helper(args, env_threads=threads)
    ready(h)
    if cfg:
        h.send(config(cfg))
    pcm = b"".join(read_pcm(r[2]) for r in rows)
    pcm = pcm[:int(seconds * SR) * 2]
    frames = audio_frames(pcm)
    time.sleep(0.5)
    cpu0 = h.cpu_seconds()
    t0 = time.monotonic()
    late = 0.0
    for k, f in enumerate(frames):
        due = t0 + (k + 1) * 0.1
        now = time.monotonic()
        if due > now:
            time.sleep(due - now)
        late = max(late, time.monotonic() - due)
        h.send(f)
    wall = time.monotonic() - t0
    cpu = h.cpu_seconds() - cpu0
    rss = h.peak_rss_kb()
    events = len(h.lines)
    status = h.close()
    return {"seconds": round(wall, 1), "cpu_pct_of_one_core": round(100 * cpu / wall, 2),
            "peak_rss_kb": rss, "events": events, "exit": status}


def step_listen(args):
    rows = load_set(args.dir, "fa-page")
    res = {}
    res["waiting_for_wake"] = listen(args, rows, None, args.listen_seconds)
    print("listening for the wake phrase:", res["waiting_for_wake"])
    res["every_utterance_counts"] = listen(args, rows, {"wake_required": False},
                                           args.listen_seconds)
    print("every utterance counting:", res["every_utterance_counts"])
    # Pure silence: what waiting costs with nothing to hear.
    path = os.path.join(args.dir, "work", "silence-60s.wav")
    if not os.path.exists(path):
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(silence(60))
    res["waiting_in_silence"] = listen(args, [("s", "-", path)], None, 60)
    print("waiting in silence:", res["waiting_in_silence"])
    return res


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--helper", required=True)
    p.add_argument("--models", required=True)
    p.add_argument("--dir", default="", help="voice bench data dir (gen_audio.py output)")
    p.add_argument("--bench-results", default="",
                   help="the bench's results dir, to compare transcripts")
    p.add_argument("--asr-threads", type=int, default=1)
    p.add_argument("--listen-seconds", type=float, default=120)
    p.add_argument("--json", default="")
    p.add_argument("steps", nargs="+",
                   choices=["protocol", "accuracy", "bias", "wake", "latency", "listen"])
    args = p.parse_args(argv)
    results = {}
    for step in args.steps:
        if step == "protocol":
            results["protocol"] = step_protocol(args)
        elif step == "accuracy":
            results["accuracy"] = step_accuracy(args)
        elif step == "bias":
            results["bias"] = step_accuracy(args, bias=True)
        elif step == "wake":
            results["wake"] = step_wake(args)
        elif step == "latency":
            results["latency"] = step_latency(args)
        elif step == "listen":
            results["listen"] = step_listen(args)
    if args.json:
        old = {}
        if os.path.exists(args.json):
            with open(args.json) as f:
                old = json.load(f)
        old.update(results)
        with open(args.json, "w") as f:
            json.dump(old, f, indent=1, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main())
