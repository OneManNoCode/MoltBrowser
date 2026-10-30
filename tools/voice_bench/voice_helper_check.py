#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Checks a built molt-voice (docs/design/VOICE_HELPER_PROTOCOL.md) from the
outside, over its stdin and stdout, as the browser drives it.

  voice_helper_check.py --helper voice/molt-voice --models voice/models \\
      [--speech-helper speech/molt-speech --speech-models speech/models] \\
      [--wav-dir DIR] STEP [STEP ...] [--json results.json]

Steps:
  protocol     malformed, oversize and unknown frames; a bad SPEAK; too
               many requests; CANCEL in a request (and the requests behind
               it); SHUTDOWN; end of input (also inside a frame); a browser
               that stops reading stdout (the helper must exit, status 3).
  speak        the test sentences (page-like text: numbers, names,
               addresses, hostile strings): time to the first audio, the
               real-time factor, the audio's length; with --wav-dir each is
               written as a WAV.
  cost         ready time and memory after loading; a long reading in real
               time (paced as the browser plays it): CPU and peak memory;
               idle CPU.
  intelligible the speech helper's recognizer (molt-speech, every
               utterance counting) on the voice's output, resampled to
               16 kHz: WER against what the voice should have said.

Python standard library only (the bench's scoring from voice_bench_lib);
run with python3 -I.
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

SPEAK, CANCEL, SHUTDOWN = 1, 2, 3
READY, AUDIO, DONE, ERROR = 1, 2, 3, 4
STATUS = {0: "spoken", 1: "cancelled", 2: "nothing", 3: "cut", 4: "engine"}

# Page-like sentences, and what a listener should hear, written as a
# recognizer writes it (both sides go through voice_bench_lib.normalize(),
# so "GPU" and "example.com" compare as said).
SENTENCES = [
    ("short", "YouTube is open.", "YouTube is open"),
    ("result", "Two: Bohemian Rhapsody, live at Live Aid, Queen, five minutes.",
     "two Bohemian Rhapsody live at Live Aid Queen five minutes"),
    ("list", "20 results. 1: Queen - Bohemian Rhapsody (Official Video "
     "Remastered), Queen Official, 6 minutes. 2: Billie Eilish - Happier "
     "Than Ever, 4.9M views. Say a number or a name.",
     "twenty results one Queen Bohemian Rhapsody official video remastered "
     "Queen official six minutes two Billie Eilish happier than ever four "
     "point nine million views say a number or a name"),
    ("shop", "Sennheiser HD 660S vs Bose QuietComfort Ultra: $349.99, 15% "
     "off until 10/30/2026.",
     "Sennheiser HD six hundred sixty S versus Bose QuietComfort Ultra "
     "three hundred forty nine dollars and ninety nine cents fifteen percent "
     "off until October thirtieth twenty twenty six"),
    ("times", "The meeting is at 6:07 pm on 2026-10-30; it lasts 1:02:03.",
     "the meeting is at six oh seven PM on October thirtieth twenty twenty "
     "six it lasts one hour two minutes three seconds"),
    ("address", "Visit https://www.example.com/path?q=1 or mail "
     "someone@example.org.",
     "visit example.com or mail someone at example.org"),
    ("tech", "GPU, CPU and HTML on macOS; MoltBrowser and MoltNet; iPhone 15 "
     "Pro.",
     "GPU CPU and HTML on macOS MoltBrowser and MoltNet iPhone fifteen Pro"),
    ("choice", "Password field, filled from your vault. Did you mean 1: "
     "Queen, 2: Queen Official, or 3: Queen Live? Say a number or a name.",
     "password field filled from your vault did you mean one Queen two "
     "Queen official or three Queen live say a number or a name"),
    ("hostile", "Hey\u202e evil\u202c Molt z\u0337\u0322\u0348a\u0334l\u0335"
     "g\u0338o\u0336 \U0001f389 \u4e2d\u6587 \u200b\u200b " + "1" * 30 +
     " " + "a" * 50 + " ..... ((( ))) ;;;", None),
]

LONG_READING = (
    "Here are the top stories. 1: Scientists find water ice on the Moon's "
    "south pole, NASA says; 2.1K comments. 2: The 2026 World Cup final draws "
    "1.5 billion viewers. 3: Markets close higher; the S&P 500 is up 1.2% "
    "at 6,021 points. 4: A new study links sleep to memory in older adults. "
    "5: How to cook the perfect steak, by Gordon Ramsay, 12 minutes. "
) * 4


def frame(kind, payload=b""):
    return struct.pack("<II", kind, len(payload)) + payload


def speak(i, rate, text):
    data = text.encode("utf-8") if isinstance(text, str) else text
    return frame(SPEAK, struct.pack("<IHH", i, rate, 0) + data)


class Voice:
    """One molt-voice process: frames in, timestamped frames out."""

    def __init__(self, args, read=True, extra=()):
        cmd = [args.helper, "--models", args.models] + list(extra)
        self.started = time.monotonic()
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
        self.frames = []
        self.bad = None
        self.eof = False
        self.cond = threading.Condition()
        self.stderr = b""
        threading.Thread(target=self._err, daemon=True).start()
        if read:
            threading.Thread(target=self._out, daemon=True).start()

    def _err(self):
        self.stderr = self.proc.stderr.read()

    def _read(self, n):
        data = b""
        while len(data) < n:
            chunk = self.proc.stdout.read(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    def _out(self):
        while True:
            head = self._read(8)
            if head is None:
                break
            kind, length = struct.unpack("<II", head)
            if kind not in (READY, AUDIO, DONE, ERROR) or length > 65536:
                self.bad = (kind, length)
                break
            payload = self._read(length) if length else b""
            if payload is None:
                break
            with self.cond:
                self.frames.append((time.monotonic(), kind, payload))
                self.cond.notify_all()
        with self.cond:
            self.eof = True
            self.cond.notify_all()

    def send(self, data):
        try:
            self.proc.stdin.write(data)
            self.proc.stdin.flush()
            return True
        except (BrokenPipeError, OSError):
            return False

    def wait_for(self, pred, timeout, start=0):
        end = time.monotonic() + timeout
        with self.cond:
            while True:
                for i in range(start, len(self.frames)):
                    if pred(self.frames[i]):
                        return i
                left = end - time.monotonic()
                if left <= 0 or self.eof:
                    return None
                self.cond.wait(min(left, 0.05))

    def ready(self, timeout=60):
        i = self.wait_for(lambda f: f[1] in (READY, ERROR), timeout)
        if i is None or self.frames[i][1] != READY:
            raise SystemExit("molt-voice didn't get ready: %s" %
                             self.stderr[-400:])
        protocol, rate = struct.unpack("<II", self.frames[i][2][:8])
        return {"protocol": protocol, "sample_rate": rate,
                "voice": self.frames[i][2][8:].decode("ascii"),
                "ready_s": self.frames[i][0] - self.started}

    def say(self, i, text, rate=100, timeout=120):
        """Sends SPEAK i and waits for its DONE: audio, timings."""
        start_index = len(self.frames)
        sent = time.monotonic()
        self.send(speak(i, rate, text))
        done = self.wait_for(
            lambda f: f[1] == DONE and struct.unpack("<I", f[2][:4])[0] == i,
            timeout, start_index)
        if done is None:
            raise SystemExit("no DONE for request %d" % i)
        pcm = b""
        first = None
        for t, kind, payload in self.frames[start_index:done]:
            if kind == AUDIO and struct.unpack("<I", payload[:4])[0] == i:
                if first is None:
                    first = t
                pcm += payload[4:]
        _, status, audio_ms, first_ms, synth_ms = struct.unpack(
            "<IIIII", self.frames[done][2])
        return {"status": STATUS.get(status, status), "audio_ms": audio_ms,
                "first_audio_ms": first_ms, "synth_ms": synth_ms,
                "first_audio_ms_outside": round(
                    (first - sent) * 1000, 1) if first else None,
                "pcm": pcm}

    def exit_status(self, timeout=5):
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return "running"

    def errors(self):
        return [(struct.unpack("<I", p[:4])[0], p[4:].decode("ascii", "replace"))
                for _, kind, p in self.frames if kind == ERROR]

    def close(self):
        self.send(frame(SHUTDOWN))
        return self.exit_status()


def proc_cpu_ms(pid):
    with open("/proc/%d/stat" % pid) as f:
        fields = f.read().rsplit(")", 1)[1].split()
    ticks = os.sysconf("SC_CLK_TCK")
    return (int(fields[11]) + int(fields[12])) * 1000.0 / ticks


def proc_hwm_mb(pid):
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            if line.startswith("VmHWM:"):
                return int(line.split()[1]) / 1024.0
    return None


# ---------------------------------------------------------------- protocol

def step_protocol(args):
    results = []

    def expect(name, data, want_code=3, want_exit=2, read=True):
        v = Voice(args, read=read)
        v.ready()
        v.send(data)
        status = v.exit_status(10)
        errors = v.errors()
        ok = status == want_exit and (want_code is None or
                                      (errors and errors[0][0] == want_code))
        results.append({"check": name, "ok": ok, "exit": status,
                        "error": errors[:1]})

    hdr = lambda t, n: struct.pack("<II", t, n)
    expect("unknown type", hdr(9, 0))
    expect("length over 64 KiB", hdr(1, 70000))
    expect("length 4 GiB", hdr(1, 0xFFFFFFFF))
    expect("CANCEL with a payload", hdr(2, 1) + b"x")
    expect("SPEAK without text", hdr(1, 8) + struct.pack("<IHH", 1, 100, 0))
    expect("SPEAK rate 49", speak(1, 49, "hello"))
    expect("SPEAK rate 251", speak(1, 251, "hello"))
    expect("SPEAK reserved field", frame(SPEAK, struct.pack("<IHH", 1, 100, 1)
                                         + b"hi"))
    expect("17 requests at once", b"".join(
        speak(i, 100, "word " * 200) for i in range(1, 18)))
    expect("SHUTDOWN", frame(SHUTDOWN), want_code=None, want_exit=0)

    # End of input, and end of input inside a frame.
    for name, data in (("end of input", b""),
                       ("end of input in a frame", hdr(1, 100) + b"abc")):
        v = Voice(args)
        v.ready()
        v.send(data)
        v.proc.stdin.close()
        status = v.exit_status(10)
        results.append({"check": name, "ok": status == 0, "exit": status})

    # CANCEL in a request: it and the one behind it end "cancelled".
    v = Voice(args)
    v.ready()
    v.send(speak(1, 100, LONG_READING) + speak(2, 100, "And this."))
    v.wait_for(lambda f: f[1] == AUDIO, 30)
    v.send(frame(CANCEL))
    t0 = time.monotonic()
    i2 = v.wait_for(lambda f: f[1] == DONE and
                    struct.unpack("<I", f[2][:4])[0] == 2, 10)
    dones = [struct.unpack("<IIIII", p) for _, k, p in v.frames if k == DONE]
    ok = i2 is not None and [d[1] for d in dones] == [1, 1]
    results.append({"check": "CANCEL in a request", "ok": ok,
                    "dones": [(d[0], STATUS[d[1]]) for d in dones],
                    "ms_to_last_done": round((v.frames[i2][0] - t0) * 1000)
                    if i2 is not None else None})
    # Later requests are said again after a CANCEL.
    r = v.say(3, "Still here.")
    results.append({"check": "SPEAK after CANCEL", "ok": r["status"] ==
                    "spoken" and r["audio_ms"] > 0})
    results.append({"check": "SHUTDOWN after work", "ok": v.close() == 0})

    # A browser that stops reading stdout: exit 3.
    v = Voice(args, read=False)
    # READY is small; the audio fills the pipe, then the 2 s limit.
    t0 = time.monotonic()
    v.send(speak(1, 100, LONG_READING))
    status = v.exit_status(30)
    results.append({"check": "browser stops reading", "ok": status == 3,
                    "exit": status,
                    "s": round(time.monotonic() - t0, 1)})
    passed = sum(1 for r in results if r["ok"])
    print("protocol: %d of %d as specified" % (passed, len(results)))
    for r in results:
        if not r["ok"]:
            print("  FAILED:", r)
    return {"checks": results, "passed": passed, "total": len(results)}


# ---------------------------------------------------------------- speak

def write_wav(path, pcm, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


def step_speak(args, keep_audio=False):
    v = Voice(args)
    info = v.ready()
    v.say(1000, "Warming up.")  # the first run allocates the model's arenas
    out = []
    for n, (name, text, _) in enumerate(SENTENCES, start=1):
        r = v.say(n, text)
        rate = info["sample_rate"]
        row = {"name": name, "status": r["status"],
               "audio_s": round(r["audio_ms"] / 1000.0, 2),
               "first_audio_ms": r["first_audio_ms"],
               "first_audio_ms_outside": r["first_audio_ms_outside"],
               "rtf": round(r["synth_ms"] / max(r["audio_ms"], 1), 3),
               "chars": len(text)}
        if keep_audio:
            row["pcm"] = r["pcm"]
            row["rate"] = rate
        if args.wav_dir:
            os.makedirs(args.wav_dir, exist_ok=True)
            write_wav(os.path.join(args.wav_dir, name + ".wav"), r["pcm"],
                      rate)
        out.append(row)
    # Rates: the same sentence at 50%, 100% and 250%.
    rates = {}
    for i, rate in enumerate((50, 100, 250)):
        r = v.say(2000 + i, SENTENCES[1][1], rate=rate)
        rates[rate] = r["audio_ms"]
    v.close()
    for row in out:
        print("speak %-8s %-8s audio %5.2f s  first audio %4d ms  RTF %.3f" % (
            row["name"], row["status"], row["audio_s"], row["first_audio_ms"],
            row["rtf"]))
    print("rate 50/100/250%%: %s ms" % rates)
    return {"ready": info, "sentences": out, "rates_audio_ms": rates}


# ---------------------------------------------------------------- cost

def step_cost(args):
    v = Voice(args)
    info = v.ready()
    pid = v.proc.pid
    loaded_mb = proc_hwm_mb(pid)
    cpu0 = proc_cpu_ms(pid)
    time.sleep(10)
    idle_cpu = (proc_cpu_ms(pid) - cpu0) / 10000.0
    # A long reading, consumed in real time (the helper paces itself).
    cpu0 = proc_cpu_ms(pid)
    t0 = time.monotonic()
    r = v.say(1, LONG_READING, timeout=600)
    wall = time.monotonic() - t0
    cpu = (proc_cpu_ms(pid) - cpu0) / 1000.0
    peak = proc_hwm_mb(pid)
    v.close()
    result = {"ready_s": round(info["ready_s"], 2),
              "loaded_mb": round(loaded_mb, 1),
              "idle_cpu_percent": round(idle_cpu * 100, 2),
              "reading_audio_s": round(r["audio_ms"] / 1000.0, 1),
              "reading_wall_s": round(wall, 1),
              "reading_cpu_s": round(cpu, 2),
              "reading_cpu_percent_of_audio": round(
                  100 * cpu / max(r["audio_ms"] / 1000.0, 0.001), 1),
              "peak_mb": round(peak, 1),
              "first_audio_ms": r["first_audio_ms"]}
    print("cost:", result)
    return result


# ---------------------------------------------------------------- intelligible

def resample(pcm, rate_in, rate_out=16000):
    """Linear interpolation after a 3-tap smoothing (enough for a check)."""
    n = len(pcm) // 2
    x = struct.unpack("<%dh" % n, pcm[:2 * n])
    if n == 0:
        return b""
    sm = [x[0]] + [(x[i - 1] + 2 * x[i] + x[i + 1]) / 4.0
                   for i in range(1, n - 1)] + [x[-1]]
    m = int(n * rate_out / rate_in)
    out = []
    step = rate_in / rate_out
    for j in range(m):
        p = j * step
        i = int(p)
        f = p - i
        a = sm[i]
        b = sm[i + 1] if i + 1 < n else a
        out.append(int(max(-32768, min(32767, a + (b - a) * f))))
    return struct.pack("<%dh" % len(out), *out)


def recognize(args, clips):
    """Each clip (16 kHz PCM) through molt-speech; the finals per clip."""
    cmd = [args.speech_helper, "--models", args.speech_models]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
    lines = []

    def reader():
        for raw in proc.stdout:
            lines.append(json.loads(raw))

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    cfg = json.dumps({"wake_required": False}).encode()
    proc.stdin.write(struct.pack("<II", 2, len(cfg)) + cfg)
    bounds = []
    pos = 0
    silence = b"\x00\x00" * 16000 * 2
    for pcm in clips:
        data = silence[:16000] + pcm + silence
        start = pos
        for i in range(0, len(data), 3200):
            chunk = data[i:i + 3200]
            proc.stdin.write(struct.pack("<II", 1, len(chunk)) + chunk)
        pos += len(data) // 2
        bounds.append((start * 1000 // 16000, pos * 1000 // 16000))
    proc.stdin.write(struct.pack("<II", 4, 0))
    proc.stdin.flush()
    proc.stdin.close()
    proc.wait(timeout=300)
    th.join(5)
    texts = []
    for start, end in bounds:
        texts.append(" ".join(ev.get("text", "") for ev in lines
                              if ev.get("type") == "final" and
                              start < ev.get("audio_ms", -1) <= end))
    return texts


def step_intelligible(args):
    if not args.speech_helper or not args.speech_models:
        raise SystemExit("intelligible needs --speech-helper and "
                         "--speech-models")
    spoken = step_speak(args, keep_audio=True)
    clips, refs, names = [], [], []
    for (name, _, ref), row in zip(SENTENCES, spoken["sentences"]):
        if ref is None:
            continue
        clips.append(resample(row["pcm"], row["rate"]))
        refs.append(ref)
        names.append(name)
    hyps = recognize(args, clips)
    errors = words = 0
    rows = []
    for name, ref, hyp in zip(names, refs, hyps):
        r = vbl.normalize(ref).split()
        h = vbl.normalize(hyp).split()
        e = vbl.edit_distance(r, h)
        errors += e
        words += len(r)
        rows.append({"name": name, "ref": ref, "heard": hyp,
                     "wer": round(e / max(len(r), 1), 3)})
        print("heard %-8s WER %5.1f%%  %s" % (name, 100.0 * e / max(len(r), 1),
                                              hyp))
    wer = errors / max(words, 1)
    print("intelligible: WER %.1f%% over %d words" % (100 * wer, words))
    for row in spoken["sentences"]:
        row.pop("pcm", None)
    return {"wer": round(wer, 4), "words": words, "sentences": rows}


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--helper", required=True)
    p.add_argument("--models", required=True)
    p.add_argument("--speech-helper")
    p.add_argument("--speech-models")
    p.add_argument("--wav-dir")
    p.add_argument("--json")
    p.add_argument("steps", nargs="+",
                   choices=["protocol", "speak", "cost", "intelligible"])
    args = p.parse_args()
    results = {}
    for step in args.steps:
        results[step] = globals()["step_" + step](args)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
