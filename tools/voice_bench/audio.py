# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Audio helpers for the voice bench (numpy only): WAV I/O, resampling,
levels, noise, synthesized music, and mixing at a given SNR.

Every random choice takes a numpy Generator seeded from the clip's name,
so the generated test set is the same on every machine.
"""

import hashlib
import wave

import numpy as np

SR = 16000


def rng_for(*parts):
    """A Generator seeded from the given strings (stable across runs and
    machines, unlike hash())."""
    h = hashlib.sha256("\x1f".join(str(p) for p in parts).encode("utf-8")).digest()
    return np.random.default_rng(int.from_bytes(h[:8], "little"))


def read_wav(path):
    """(float32 samples in [-1, 1], sample rate). Mono 16-bit PCM only."""
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError("%s: need 16-bit PCM" % path)
        rate, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
        data = np.frombuffer(w.readframes(n), dtype="<i2").astype(np.float32) / 32768.0
    if ch > 1:
        data = data.reshape(-1, ch).mean(axis=1)
    return data, rate


def write_wav(path, x, rate=SR):
    y = np.clip(np.round(np.asarray(x, dtype=np.float64) * 32767.0), -32768, 32767)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(y.astype("<i2").tobytes())


def resample_to(x, n_out):
    """Band-limited resampling of a whole clip to n_out samples (FFT
    method). The clip is zero-padded first, so the circular wrap of the
    FFT lands in silence."""
    x = np.asarray(x, dtype=np.float64)
    n = len(x)
    if n == 0 or n_out == n:
        return x.astype(np.float32)
    pad = max(64, n // 8)
    xp = np.concatenate([np.zeros(pad), x, np.zeros(pad)])
    m_total = int(round(len(xp) * n_out / n))
    spec = np.fft.rfft(xp)
    keep = min(len(spec), m_total // 2 + 1)
    out_spec = np.zeros(m_total // 2 + 1, dtype=complex)
    out_spec[:keep] = spec[:keep]
    y = np.fft.irfft(out_spec, m_total) * (m_total / len(xp))
    start = int(round(pad * n_out / n))
    return y[start:start + n_out].astype(np.float32)


def trim_silence(x, rate, thresh_db=-45.0, frame_ms=10):
    """Drops leading and trailing frames quieter than thresh_db below the
    loudest frame. Returns the trimmed clip."""
    f = max(1, int(rate * frame_ms / 1000))
    n = len(x) // f
    if n == 0:
        return x
    frames = x[: n * f].reshape(n, f)
    db = 10 * np.log10(np.mean(frames.astype(np.float64) ** 2, axis=1) + 1e-12)
    active = np.where(db > db.max() + thresh_db)[0]
    if len(active) == 0:
        return x
    a = max(0, active[0] - 1) * f
    b = min(len(x), (active[-1] + 2) * f)
    return x[a:b]


def rms(x):
    x = np.asarray(x, dtype=np.float64)
    return float(np.sqrt(np.mean(x * x) + 1e-20))


def db_to_gain(db):
    return 10.0 ** (db / 20.0)


def pink_noise(n, rng):
    """Pink (1/f power) noise with unit RMS."""
    white = rng.standard_normal(n + 1024)
    spec = np.fft.rfft(white)
    f = np.arange(len(spec), dtype=np.float64)
    f[0] = 1.0
    y = np.fft.irfft(spec / np.sqrt(f), len(white))[:n]
    return (y / (rms(y) + 1e-12)).astype(np.float32)


def _env(n, attack, release, rate=SR):
    e = np.ones(n, dtype=np.float32)
    a = min(n, int(attack * rate))
    r = min(n - a, int(release * rate))
    if a:
        e[:a] = np.linspace(0, 1, a, dtype=np.float32)
    if r:
        e[n - r:] = np.linspace(1, 0, r, dtype=np.float32)
    return e


def synth_music(n, rng, rate=SR):
    """A few seconds of synthesized pop-like music with unit RMS: chords,
    bass, a pentatonic melody and drums, random key and tempo."""
    out = np.zeros(n, dtype=np.float32)
    bpm = rng.uniform(85, 135)
    beat = int(rate * 60.0 / bpm)
    key = 220.0 * 2 ** (rng.integers(-5, 6) / 12.0)
    prog = [(0, 4, 7), (7, 11, 14), (9, 12, 16), (5, 9, 12)]  # I V vi IV
    t_bar = np.arange(4 * beat) / rate
    for bar_start in range(0, n, 4 * beat):
        chord = prog[(bar_start // (4 * beat)) % 4]
        m = min(4 * beat, n - bar_start)
        t = t_bar[:m]
        seg = np.zeros(m, dtype=np.float32)
        for semi in chord:
            f = key * 2 ** (semi / 12.0)
            for h, amp in ((1, 1.0), (2, 0.4), (3, 0.2)):
                seg += amp * 0.12 * np.sin(2 * np.pi * f * h * t).astype(np.float32)
        seg *= _env(m, 0.05, 0.3, rate)
        bass_f = key / 2 * 2 ** (chord[0] / 12.0)
        for b in range(4):
            s = b * beat
            if s >= m:
                break
            k = min(beat, m - s)
            tb = np.arange(k) / rate
            seg[s:s + k] += 0.35 * np.sin(2 * np.pi * bass_f * tb).astype(np.float32) * \
                np.exp(-tb * 4).astype(np.float32)
        out[bar_start:bar_start + m] += seg
    penta = [0, 2, 4, 7, 9, 12, 14]
    for s in range(0, n, beat // 2):
        if rng.random() < 0.35:
            continue
        k = min(beat // 2, n - s)
        tb = np.arange(k) / rate
        f = key * 2 * 2 ** (penta[rng.integers(len(penta))] / 12.0)
        out[s:s + k] += 0.25 * np.sin(2 * np.pi * f * tb).astype(np.float32) * \
            np.exp(-tb * 6).astype(np.float32)
    for s in range(0, n, beat):
        k = min(int(0.15 * rate), n - s)
        tb = np.arange(k) / rate
        if (s // beat) % 2 == 0:  # kick
            out[s:s + k] += 0.6 * np.sin(2 * np.pi * (50 + 80 * np.exp(-tb * 30)) * tb).astype(
                np.float32) * np.exp(-tb * 18).astype(np.float32)
        else:  # snare
            out[s:s + k] += 0.3 * rng.standard_normal(k).astype(np.float32) * \
                np.exp(-tb * 25).astype(np.float32)
        h = s + beat // 2
        if h < n:  # hi-hat
            kh = min(int(0.04 * rate), n - h)
            noise = rng.standard_normal(kh + 1).astype(np.float32)
            out[h:h + kh] += 0.12 * np.diff(noise) * np.exp(-np.arange(kh) / rate * 80).astype(
                np.float32)
    return out / (rms(out) + 1e-12)


def fit_length(x, n, rng):
    """x looped (from a random offset) or cut to exactly n samples."""
    if len(x) == 0:
        return np.zeros(n, dtype=np.float32)
    reps = int(np.ceil((n + len(x)) / len(x)))
    tiled = np.tile(x, reps)
    off = int(rng.integers(0, len(x)))
    return tiled[off:off + n]


def mix_at_snr(speech, noise, snr_db, span):
    """speech + noise scaled so that, over span=(a, b) samples (where the
    speech is), speech power / noise power = snr_db."""
    a, b = span
    ps = rms(speech[a:b]) ** 2
    pn = rms(noise[a:b]) ** 2
    g = np.sqrt(ps / (pn * 10 ** (snr_db / 10.0)))
    return speech + g * noise


def set_level(x, span, level_db):
    """x scaled so its RMS over span=(a, b) is level_db dBFS."""
    a, b = span
    return (x * db_to_gain(level_db) / (rms(x[a:b]) + 1e-12)).astype(np.float32)


def finalize(x, rng):
    """A -65 dBFS noise floor (no digital silence) and no clipping."""
    x = x + db_to_gain(-65) * rng.standard_normal(len(x)).astype(np.float32)
    peak = float(np.max(np.abs(x))) if len(x) else 0.0
    if peak > 0.97:
        x = x * (0.97 / peak)
    return x.astype(np.float32)
