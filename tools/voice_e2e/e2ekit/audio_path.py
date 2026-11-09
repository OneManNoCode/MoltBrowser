# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""How the kit's speech reaches the browser, and how the browser's reaches
the kit.

PulseAudio (preferred): a daemon of the kit's own (its socket in the run
directory, no other client), with
  molt_out      a null sink: the default output, where the browser's
                speech and its pages' sound go; its monitor is recorded
  molt_mic_in   a null sink the kit plays its utterances (and the music of
                the "music" condition) into
  molt_mic      its monitor, remapped: the default input, which the
                browser captures
and, unless turned off, a loopback of molt_out's monitor into molt_mic_in
(at ECHO_DB): the browser hears its own voice and its pages as a
microphone next to a speaker would, for echo cancellation and barge-in.
Each utterance is played when the previous step is done, so a scenario
waits on the browser, not on fixed gaps.

File capture (fallback): one WAV per scenario for Chromium's fake capture
device (--use-fake-device-for-media-stream
--use-file-for-fake-audio-capture=<wav>%noloop), the utterances at fixed
times. No loopback, no recording, and no confirmation word (it isn't known
before the run).
"""

import array
import math
import os
import shutil
import signal
import struct
import subprocess
import tempfile
import time
import wave

from . import render

MAX_SOCKET_PATH = 100
ECHO_DB = -6.0
PCM_RATE = 16000
FILE_RATE = 44100  # the fake capture device's rate (leakkit/micinput.py)
WAKE_GAP_S = 0.35
# The route check's tone, and the least of it the default input must give.
CHECK_TONE_DB = -12.0
CHECK_MIN_DB = -40.0


class AudioError(Exception):
    pass


def pulse_available():
    return all(shutil.which(t) for t in ("pulseaudio", "pactl", "paplay", "parec"))


class Pulse(object):
    """The kit's PulseAudio daemon."""

    def __init__(self, run_dir, loopback=True, log=print):
        self.dir = os.path.join(run_dir, "pulse")
        self.loopback = loopback
        self.log = log
        self.proc = None
        self.recorder = None
        self.bed = None
        self.keepalive = None
        self.socket = os.path.join(self.dir, "runtime", "native")
        if len(self.socket) > MAX_SOCKET_PATH:
            # A Unix socket's path has a small limit (108 bytes on Linux).
            self._short = tempfile.mkdtemp(prefix="molt-pa-")
            self.socket = os.path.join(self._short, "native")
        else:
            self._short = None

    def env(self):
        """The environment a client (the browser, paplay) needs."""
        return {"PULSE_SERVER": "unix:" + self.socket,
                "PULSE_RUNTIME_PATH": os.path.join(self.dir, "runtime"),
                "PULSE_STATE_PATH": os.path.join(self.dir, "state"),
                "PULSE_COOKIE": os.path.join(self.dir, "cookie")}

    def _env(self):
        env = dict(os.environ)
        env.update(self.env())
        env["HOME"] = os.path.join(self.dir, "home")
        env["XDG_RUNTIME_DIR"] = os.path.join(self.dir, "xdg")
        return env

    def start(self):
        if not pulse_available():
            raise AudioError("PulseAudio isn't installed (pulseaudio, pulseaudio-utils)")
        for sub in ("runtime", "state", "home", "xdg"):
            os.makedirs(os.path.join(self.dir, sub), mode=0o700, exist_ok=True)
        if os.path.exists(self.socket):
            os.remove(self.socket)
        script = os.path.join(self.dir, "default.pa")
        lines = [
            "load-module module-native-protocol-unix socket=%s auth-anonymous=1" % self.socket,
            "load-module module-null-sink sink_name=molt_out rate=48000 channels=2 "
            "sink_properties=device.description=MoltOut",
            "load-module module-null-sink sink_name=molt_mic_in rate=48000 channels=1 "
            "sink_properties=device.description=MoltMicIn",
            "load-module module-remap-source source_name=molt_mic master=molt_mic_in.monitor "
            "channels=1 source_properties=device.description=MoltMic",
            "set-default-sink molt_out",
            "set-default-source molt_mic",
        ]
        with open(script, "w") as f:
            f.write("\n".join(lines) + "\n")
        log = open(os.path.join(self.dir, "pulse.log"), "w")
        self.proc = subprocess.Popen(
            ["pulseaudio", "-n", "--daemonize=no", "--exit-idle-time=-1",
             "--disallow-exit", "--use-pid-file=no", "--log-target=stderr",
             "--log-level=notice", "-F", script],
            stdout=log, stderr=log, env=self._env(), start_new_session=True)
        deadline = time.time() + 15
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise AudioError("pulseaudio exited (%s); see %s/pulse.log"
                                 % (self.proc.returncode, self.dir))
            if os.path.exists(self.socket) and self._pactl("info").returncode == 0:
                break
            time.sleep(0.1)
        else:
            self.stop()
            raise AudioError("pulseaudio didn't start; see %s/pulse.log" % self.dir)
        if not self.loopback:
            # No loopback: a silent stream keeps molt_mic_in running, as the
            # loopback does (a null sink with nothing playing gives its
            # monitor no samples, and plays a new stream late).
            self._start_keepalive()
        if self.loopback:
            r = self._pactl("load-module", "module-loopback", "source=molt_out.monitor",
                            "sink=molt_mic_in", "latency_msec=30",
                            "source_dont_move=true", "sink_dont_move=true")
            if r.returncode != 0:
                raise AudioError("no loopback: %s" % r.stderr.strip())
            # The echo's level: the loopback's sink input, ECHO_DB down.
            vol = int(round(65536 * 10 ** (ECHO_DB / 20.0)))
            for line in self._pactl("list", "short", "sink-inputs").stdout.splitlines():
                idx = line.split("\t", 1)[0]
                self._pactl("set-sink-input-volume", idx, str(vol))
        peak = self.check_route()
        self.log("[pulse] a tone played into molt_mic_in reaches the default "
                 "input at %.1f dBFS" % peak)
        if peak < CHECK_MIN_DB:
            self.stop()
            raise AudioError("the default input doesn't hear what is played into "
                             "molt_mic_in (%.1f dBFS); see %s/pulse.log" % (peak, self.dir))
        return self

    def _start_keepalive(self):
        silence = os.path.join(self.dir, "keepalive.wav")
        with wave.open(silence, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(PCM_RATE)
            w.writeframes(b"\0\0" * PCM_RATE * 10)
        self.keepalive = subprocess.Popen(
            ["bash", "-c", 'while :; do paplay --latency-msec=30 '
             '--device=molt_mic_in "$0" || exit 1; done', silence],
            env=self._env(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True)
        time.sleep(0.5)

    def check_route(self, seconds=1.5):
        """Plays a tone into molt_mic_in while recording the default input
        (as the browser opens it: no device named) and returns the
        recording's peak in dBFS (-120 for silence). The recording goes on
        well past the tone: with no low-latency stream on molt_mic_in (no
        loopback), the null sink plays a new stream with a delay, and a
        short tone would come out after a short recording stopped."""
        tone = os.path.join(self.dir, "check-tone.wav")
        n = int(PCM_RATE * seconds)
        amp = 32767 * 10 ** (CHECK_TONE_DB / 20.0)
        with wave.open(tone, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(PCM_RATE)
            w.writeframes(b"".join(
                struct.pack("<h", int(amp * math.sin(2 * math.pi * 440 * i / PCM_RATE)))
                for i in range(n)))
        raw = os.path.join(self.dir, "check-input.raw")
        rec = subprocess.Popen(
            ["parec", "--format=s16le", "--rate=%d" % PCM_RATE, "--channels=1",
             "--raw", raw],
            env=self._env(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True)
        try:
            time.sleep(0.3)
            self.play(tone)
            time.sleep(1.5)
        finally:
            _kill_group(rec)
        return pcm_peak_db(raw)

    def _pactl(self, *args):
        return subprocess.run(["pactl"] + list(args), capture_output=True, text=True,
                              env=self._env(), timeout=15)

    def play(self, wav):
        """Plays |wav| into the microphone and returns when it has been
        played (paplay drains the stream)."""
        r = subprocess.run(["paplay", "--device=molt_mic_in", wav],
                           capture_output=True, text=True, env=self._env(), timeout=120)
        if r.returncode != 0:
            raise AudioError("paplay: %s" % r.stderr.strip())

    def start_bed(self, wav):
        """Plays |wav| into the microphone, again and again, until stop_bed()."""
        self.stop_bed()
        self.bed = subprocess.Popen(
            ["bash", "-c", 'while :; do paplay --device=molt_mic_in "$0" || exit 1; done', wav],
            env=self._env(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True)

    def stop_bed(self):
        if self.bed:
            _kill_group(self.bed)
            self.bed = None

    def start_recording(self, path):
        """Records what the browser plays (molt_out's monitor), 16 kHz mono
        raw PCM, to |path|; returns when the recording started."""
        self.stop_recording()
        self.recorder = subprocess.Popen(
            ["parec", "--device=molt_out.monitor", "--format=s16le",
             "--rate=%d" % PCM_RATE, "--channels=1", "--raw", path],
            env=self._env(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True)
        return time.time()

    def stop_recording(self):
        if self.recorder:
            _kill_group(self.recorder)
            self.recorder = None

    def stop(self):
        self.stop_bed()
        self.stop_recording()
        if self.keepalive:
            _kill_group(self.keepalive)
            self.keepalive = None
        if self.proc:
            _kill_group(self.proc)
            self.proc = None
        if self._short:
            shutil.rmtree(self._short, ignore_errors=True)
            self._short = None


def _kill_group(proc):
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError):
        pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


def pcm_peak_db(path):
    """The peak of a raw s16le file in dBFS (-120 for silence or none)."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return -120.0
    samples = array.array("h")
    samples.frombytes(data[:len(data) // 2 * 2])
    peak = max((abs(x) for x in samples), default=0)
    return 20 * math.log10(peak / 32768.0) if peak else -120.0


def read_pcm_since(path, t0, since, until=None):
    """16 kHz mono PCM of a raw recording started at |t0|, from |since| to
    |until| (seconds since the epoch)."""
    a = max(0, int((since - t0) * PCM_RATE)) * 2
    try:
        with open(path, "rb") as f:
            f.seek(a)
            data = f.read() if until is None else f.read(
                max(0, int((until - since) * PCM_RATE)) * 2)
    except OSError:
        return b""
    return data[:len(data) // 2 * 2]


def utterance_wav(speech_dir, out_path, voice, speed, text, wake):
    """The WAV the kit plays for one step: the clip of |text|, after the
    wake phrase and a short pause when |wake|. Returns its speech length in
    seconds (it has no silence after the speech)."""
    import numpy as np
    audio = render._audio()
    parts = []
    if wake:
        w, _ = audio.read_wav(render.clip_path(speech_dir, voice, speed, render.WAKE_PHRASE))
        parts += [w, np.zeros(int(WAKE_GAP_S * render.SR), np.float32)]
    x, _ = audio.read_wav(render.clip_path(speech_dir, voice, speed, text))
    parts.append(x)
    lead = np.zeros(int(0.15 * render.SR), np.float32)
    y = np.concatenate([lead] + parts)
    audio.write_wav(out_path, y)
    return len(y) / render.SR


def scenario_wav(out_path, pieces, cond, lead_s, gap_s, seed):
    """One WAV for the fake capture device: |pieces| (WAV paths of each
    step's utterance, 16 kHz) at fixed times: the first after |lead_s|, each
    next |gap_s| after the end of the one before (one number, or one per
    piece: the first's is not used); under |cond|; 44.1 kHz stereo. Returns
    each piece's (start, end) in seconds."""
    import numpy as np
    audio = render._audio()
    xs = [audio.read_wav(p)[0] for p in pieces]
    gaps = list(gap_s) if isinstance(gap_s, (list, tuple)) else [gap_s] * len(xs)
    if len(gaps) != len(xs):
        raise ValueError("one gap per piece")
    times, t = [], lead_s
    for k, x in enumerate(xs):
        if k:
            t += gaps[k]
        times.append((t, t + len(x) / render.SR))
        t += len(x) / render.SR
    # The last piece's gap, as before, then the tail.
    t += gaps[-1] if gaps else 0
    n = int((t + 2.0) * render.SR)
    y = np.zeros(n, np.float32)
    for x, (a, _) in zip(xs, times):
        i = int(a * render.SR)
        y[i:i + len(x)] += x
    kind, snr = render.CONDITIONS[cond]
    rng = audio.rng_for("e2e-file", seed, cond)
    if kind == "music":
        y = y + audio.synth_music(n, rng) * audio.db_to_gain(render.SPEECH_DB - snr)
    elif kind == "pink":
        y = y + audio.pink_noise(n, rng) * audio.db_to_gain(render.SPEECH_DB - snr)
    y = audio.finalize(y, rng)
    m = int(round(n * FILE_RATE / render.SR))
    z = audio.resample_to(y, m)
    pcm = np.clip(np.round(z.astype(np.float64) * 32767.0), -32768, 32767).astype("<i2")
    stereo = np.repeat(pcm, 2)
    with wave.open(out_path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(FILE_RATE)
        w.writeframes(stereo.tobytes())
    return times
