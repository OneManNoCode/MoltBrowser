#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Writes the word-piece vocabularies molt-speech reads (scripts/bundle-speech.sh).

  make_speech_vocab.py sentencepiece BPE_MODEL TOKENS OUT
      The keyword spotter's pieces: the normal and user-defined pieces of
      its SentencePiece model (a unigram model: icefall's "bpe.model") with
      their scores, keeping only pieces its tokens.txt has. molt-speech cuts
      wake phrases with them (highest-scoring split), as the model's own
      SentencePiece does (docs/VOICE_BENCH.md §3.8).
  make_speech_vocab.py tokens TOKENS OUT
      The recognizer's pieces, for sherpa-onnx's hotword splitting
      (modeling_unit "bpe"): Kroko ships no SentencePiece model, so every
      token with score -ln(id + 2), which reproduces 85-86% of real unigram
      splits (docs/VOICE_BENCH.md §3.7, voice_bench_lib.vocab_from_tokens).

Output: "<piece>\\t<score>\\n" per piece, scores with six decimals, in the
input's order. The output is a pure function of the input files, so its
SHA-256 is pinned in scripts/bundle-speech.sh. Standard library only; run
with python3 -I.
"""

import math
import struct
import sys


def _varint(buf, pos):
    value, shift = 0, 0
    while True:
        if pos >= len(buf):
            raise ValueError("truncated varint")
        b = buf[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        if not b & 0x80:
            return value, pos
        shift += 7
        if shift > 63:
            raise ValueError("varint too long")


def _fields(buf):
    pos = 0
    while pos < len(buf):
        key, pos = _varint(buf, pos)
        field, wire = key >> 3, key & 7
        if wire == 0:
            value, pos = _varint(buf, pos)
        elif wire == 1:
            value, pos = buf[pos:pos + 8], pos + 8
        elif wire == 2:
            n, pos = _varint(buf, pos)
            if pos + n > len(buf):
                raise ValueError("truncated field")
            value, pos = buf[pos:pos + n], pos + n
        elif wire == 5:
            value, pos = buf[pos:pos + 4], pos + 4
        else:
            raise ValueError("unsupported wire type %d" % wire)
        yield field, wire, value


def sentencepiece_pieces(path):
    """[(piece, score, type)] of a SentencePiece ModelProto."""
    with open(path, "rb") as f:
        buf = f.read()
    out = []
    for field, wire, value in _fields(buf):
        if field != 1 or wire != 2:
            continue
        piece, score, kind = "", 0.0, 1
        for f2, w2, v2 in _fields(value):
            if f2 == 1 and w2 == 2:
                piece = bytes(v2).decode("utf-8")
            elif f2 == 2 and w2 == 5:
                score = struct.unpack("<f", bytes(v2))[0]
            elif f2 == 3 and w2 == 0:
                kind = v2
        out.append((piece, score, kind))
    return out


def read_tokens(path):
    """[(token, id)] of a sherpa-onnx tokens.txt."""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            token, _, idx = line.rpartition(" ")
            if not token or not idx.isdigit():
                raise ValueError("malformed tokens line: %r" % line)
            out.append((token, int(idx)))
    return out


def write_vocab(pieces, path):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for piece, score in pieces:
            if not piece or any(c.isspace() for c in piece):
                raise ValueError("piece with white space: %r" % piece)
            f.write("%s\t%.6f\n" % (piece, score))


def main(argv):
    if len(argv) == 5 and argv[1] == "sentencepiece":
        tokens = {t for t, _ in read_tokens(argv[3])}
        pieces = [(p, s) for p, s, kind in sentencepiece_pieces(argv[2])
                  if kind in (1, 4) and p in tokens]  # NORMAL, USER_DEFINED
        write_vocab(pieces, argv[4])
        return 0
    if len(argv) == 4 and argv[1] == "tokens":
        pieces = [(t, -math.log(i + 2)) for t, i in read_tokens(argv[2])]
        write_vocab(pieces, argv[3])
        return 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
