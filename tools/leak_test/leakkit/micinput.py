# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The fixed microphone input of the mic_processing phase.

Chromium's fake capture device (--use-fake-device-for-media-stream) plays
the WAV given with --use-file-for-fake-audio-capture, read from its start
when a stream starts. The device runs at 44.1 kHz stereo
(media::AudioParameters::UnavailableDeviceParams()), so the file has that
format and reaches the audio processing without conversion; the processing
resamples it to its own rates and delivers 48 kHz, as for a real 44.1 kHz
microphone.

The signal: 2 s of digital silence (the stream may start before the page
listens), a 30 ms burst that marks where the sound starts, 3.5 s of two
gliding "voices" of triangle harmonics in syllables with a little hiss, and
1 s of silence. Integer arithmetic only, so the bytes are the same wherever
the kit runs (tests/test_analysis.py checks their digest).
"""

import struct

RATE = 44100
CHANNELS = 2
LEAD_S = 2.0
BURST_S = 0.03
VOICE_S = 3.5
TAIL_S = 1.0


def _tri(phase):
    """A triangle wave in [-32768, 32767] from a 32-bit phase."""
    p = (phase >> 8) & 0xFFFFFF
    v = p if p < (1 << 23) else (1 << 24) - p   # 0 .. 2^23
    return (v - (1 << 22)) >> 7                  # about -32768 .. 32767


def samples():
    """The interleaved 16-bit samples (left, right, left, ...)."""
    out = []
    lead = int(LEAD_S * RATE)
    burst = int(BURST_S * RATE)
    voice = int(VOICE_S * RATE)
    tail = int(TAIL_S * RATE)
    out.extend([0] * (2 * lead))
    phase = 0
    for _ in range(burst):
        phase = (phase + 97391548) & 0xFFFFFFFF   # about 1 kHz
        v = _tri(phase) // 2
        out.extend([v, v])
    ph = [[0, 0, 0], [0, 0, 0]]
    noise = 12345
    for t in range(voice):
        frame = []
        for talker, (base, syll, gap) in enumerate(
                ((9000000, 9000, 5000), (13000000, 11000, 7000))):
            pos = t % (syll + gap)
            env = 0
            if pos < syll:
                up = syll // 8
                env = (pos * 256) // up if pos < up else \
                    ((syll - pos) * 256) // (syll - up)
            glide = base + ((t // 64) % 400) * 3000
            s = 0
            for h in range(3):
                ph[talker][h] = (ph[talker][h] + glide * (h + 1)) & 0xFFFFFFFF
                s += _tri(ph[talker][h]) // (h + 1)
            frame.append((s * env) >> 10)
        noise = (noise * 1103515245 + 12345) & 0x7FFFFFFF
        hiss = (noise >> 16) % 257 - 128
        left = frame[0] + frame[1] // 3 + hiss
        right = frame[0] // 2 + frame[1] + hiss
        out.extend([max(-32768, min(32767, left)),
                    max(-32768, min(32767, right))])
    out.extend([0] * (2 * tail))
    return out


def wav_bytes():
    data = struct.pack('<%dh' % len(samples_cache()), *samples_cache())
    header = b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE' + \
        b'fmt ' + struct.pack('<IHHIIHH', 16, 1, CHANNELS, RATE,
                              RATE * CHANNELS * 2, CHANNELS * 2, 16) + \
        b'data' + struct.pack('<I', len(data))
    return header + data


_CACHE = []


def samples_cache():
    if not _CACHE:
        _CACHE.append(samples())
    return _CACHE[0]
