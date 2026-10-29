# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The engines and models the voice bench measures, and how to run each
through tools/voice_bench/driver/sherpa_bench.c.

Each entry names the artifact (artifacts.json id) it lives in and the
driver flags, with file names relative to that artifact's directory.
"""

# Per-utterance (offline) recognizers.
OFFLINE = {
    "moonshine-tiny": {
        "artifact": "moonshine-tiny-en-int8", "kind": "moonshine",
        "files": {"--preprocessor": "preprocess.onnx", "--encoder": "encode.int8.onnx",
                  "--uncached-decoder": "uncached_decode.int8.onnx",
                  "--cached-decoder": "cached_decode.int8.onnx", "--tokens": "tokens.txt"},
        "label": "Moonshine tiny (v1, int8)", "tier": 1},
    "moonshine-base": {
        "artifact": "moonshine-base-en-int8", "kind": "moonshine",
        "files": {"--preprocessor": "preprocess.onnx", "--encoder": "encode.int8.onnx",
                  "--uncached-decoder": "uncached_decode.int8.onnx",
                  "--cached-decoder": "cached_decode.int8.onnx", "--tokens": "tokens.txt"},
        "label": "Moonshine base (v1, int8)", "tier": 1},
    "moonshine2-tiny": {
        "artifact": "moonshine2-tiny-en", "kind": "moonshine2",
        "files": {"--encoder": "encoder_model.ort",
                  "--merged-decoder": "decoder_model_merged.ort", "--tokens": "tokens.txt"},
        "label": "Moonshine v2 tiny (quantized)", "tier": 1},
    "moonshine2-base": {
        "artifact": "moonshine2-base-en", "kind": "moonshine2",
        "files": {"--encoder": "encoder_model.ort",
                  "--merged-decoder": "decoder_model_merged.ort", "--tokens": "tokens.txt"},
        "label": "Moonshine v2 base (quantized)", "tier": 1},
    "parakeet-tdt-0.6b-v2": {
        "artifact": "parakeet-tdt-0.6b-v2-int8", "kind": "nemo_transducer",
        "files": {"--encoder": "encoder.int8.onnx", "--decoder": "decoder.int8.onnx",
                  "--joiner": "joiner.int8.onnx", "--tokens": "tokens.txt"},
        "label": "Parakeet TDT 0.6B v2 (int8)", "tier": 2},
    "whisper-tiny.en": {
        "artifact": "whisper-tiny-en", "kind": "whisper",
        "files": {"--encoder": "tiny.en-encoder.int8.onnx",
                  "--decoder": "tiny.en-decoder.int8.onnx", "--tokens": "tiny.en-tokens.txt"},
        "label": "Whisper tiny.en (ONNX int8)", "tier": 1},
    "whisper-tiny.en-fp32": {
        "artifact": "whisper-tiny-en", "kind": "whisper",
        "files": {"--encoder": "tiny.en-encoder.onnx",
                  "--decoder": "tiny.en-decoder.onnx", "--tokens": "tiny.en-tokens.txt"},
        "label": "Whisper tiny.en (ONNX fp32)", "tier": 3},
    "whisper-base.en": {
        "artifact": "whisper-base-en", "kind": "whisper",
        "files": {"--encoder": "base.en-encoder.int8.onnx",
                  "--decoder": "base.en-decoder.int8.onnx", "--tokens": "base.en-tokens.txt"},
        "label": "Whisper base.en (ONNX int8)", "tier": 2},
    "whisper-small.en": {
        "artifact": "whisper-small-en", "kind": "whisper",
        "files": {"--encoder": "small.en-encoder.int8.onnx",
                  "--decoder": "small.en-decoder.int8.onnx", "--tokens": "small.en-tokens.txt"},
        "label": "Whisper small.en (ONNX int8)", "tier": 3},
}

# Streaming transducers. "case": how biasing phrases must be written for
# the model's vocabulary; "vocab": where the bpe.vocab for hotwords comes
# from ("bpe.model" parsed, or tokens.txt with score = -id).
STREAM = {
    "zipformer-en-20m": {
        "artifact": "stream-zipformer-en-20m",
        "files": {"--encoder": "encoder-epoch-99-avg-1.int8.onnx",
                  "--decoder": "decoder-epoch-99-avg-1.onnx",
                  "--joiner": "joiner-epoch-99-avg-1.int8.onnx", "--tokens": "tokens.txt"},
        "case": "upper", "vocab": "tokens", "label": "Zipformer EN 20M (2023-02-17, int8)",
        "tier": 1},
    "zipformer-en-2023-06-26": {
        "artifact": "stream-zipformer-en-2023-06-26",
        "files": {"--encoder": "encoder-epoch-99-avg-1-chunk-16-left-128.int8.onnx",
                  "--decoder": "decoder-epoch-99-avg-1-chunk-16-left-128.onnx",
                  "--joiner": "joiner-epoch-99-avg-1-chunk-16-left-128.int8.onnx",
                  "--tokens": "tokens.txt"},
        "case": "upper", "vocab": "bpe.model", "label": "Zipformer EN 70M (2023-06-26, int8)",
        "tier": 1},
    "kroko-en": {
        "artifact": "stream-zipformer-en-kroko",
        "files": {"--encoder": "encoder.onnx", "--decoder": "decoder.onnx",
                  "--joiner": "joiner.onnx", "--tokens": "tokens.txt"},
        "case": "lower", "vocab": "tokens", "label": "Kroko EN (zipformer, 2025-08-06)",
        "tier": 1},
    "fastconformer-80ms": {
        "artifact": "stream-fastconformer-en-80ms-int8",
        "files": {"--encoder": "encoder.int8.onnx", "--decoder": "decoder.int8.onnx",
                  "--joiner": "joiner.int8.onnx", "--tokens": "tokens.txt"},
        "case": "lower", "vocab": "tokens",
        "label": "NeMo FastConformer streaming 80 ms (int8)", "tier": 1},
    "nemotron-0.6b-560ms": {
        "artifact": "stream-nemotron-en-0.6b-560ms-int8",
        "files": {"--encoder": "encoder.int8.onnx", "--decoder": "decoder.int8.onnx",
                  "--joiner": "joiner.int8.onnx", "--tokens": "tokens.txt"},
        "case": "keep", "vocab": "tokens",
        "label": "Nemotron Speech Streaming EN 0.6B, 560 ms (int8)", "tier": 2},
}

VAD = {"artifact": "vad-silero", "file": "silero_vad.onnx"}

# Open-vocabulary keyword spotters. "unit": how a phrase becomes tokens.
KWS = {
    "kws-gigaspeech-3.3m": {
        "artifact": "kws-gigaspeech-3.3m",
        "files": {"--encoder": "encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx",
                  "--decoder": "decoder-epoch-12-avg-2-chunk-16-left-64.onnx",
                  "--joiner": "joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx",
                  "--tokens": "tokens.txt"},
        "unit": "bpe", "bpe_model": "bpe.model", "label": "Zipformer KWS 3.3M (GigaSpeech, BPE)"},
    "kws-zh-en-3m": {
        "artifact": "kws-zh-en-3m",
        "files": {"--encoder": "encoder-epoch-13-avg-2-chunk-16-left-64.int8.onnx",
                  "--decoder": "decoder-epoch-13-avg-2-chunk-16-left-64.onnx",
                  "--joiner": "joiner-epoch-13-avg-2-chunk-16-left-64.int8.onnx",
                  "--tokens": "tokens.txt"},
        "unit": "phone", "lexicon": "en.phone",
        "label": "Zipformer KWS 3M (zh+en, 2025-12-20, phones)"},
}

WAKE_PHRASES = ["hey molt", "hello molt", "hey molt browser"]

# Voices. "lexicon": "vits" or "kokoro" (the generated lexicons, see
# gen_audio.prepare_lexicons). "ship_ok": whether the voice's licence chain
# allows shipping it (see artifacts.json); others are used for test audio
# and speed only.
TTS = {
    "kokoro-v1.0-int8": {
        "artifact": "tts-kokoro-v1_0-int8", "kind": "kokoro", "lexicon": "kokoro",
        "files": {"--model": "model.int8.onnx", "--voices": "voices.bin",
                  "--tokens": "tokens.txt"},
        "label": "Kokoro 82M v1.0 (int8)", "ship_ok": True},
    "kokoro-v1.0": {
        "artifact": "tts-kokoro-v1_0", "kind": "kokoro", "lexicon": "kokoro",
        "files": {"--model": "model.onnx", "--voices": "voices.bin", "--tokens": "tokens.txt"},
        "label": "Kokoro 82M v1.0 (fp32)", "ship_ok": True},
    "piper-ljspeech-medium": {
        "artifact": "tts-piper-en_US-ljspeech-medium", "kind": "vits", "lexicon": "vits",
        "files": {"--model": "en_US-ljspeech-medium.onnx", "--tokens": "tokens.txt"},
        "label": "Piper (VITS) en_US ljspeech medium", "ship_ok": True},
    "piper-libritts_r-medium": {
        "artifact": "tts-piper-en_US-libritts_r-medium", "kind": "vits", "lexicon": "vits",
        "files": {"--model": "en_US-libritts_r-medium.onnx", "--tokens": "tokens.txt"},
        "label": "Piper (VITS) en_US libritts_r medium", "ship_ok": False},
    "piper-libritts_r-medium-int8": {
        "artifact": "tts-piper-en_US-libritts_r-medium-int8", "kind": "vits", "lexicon": "vits",
        "files": {"--model": "en_US-libritts_r-medium.onnx", "--tokens": "tokens.txt"},
        "label": "Piper (VITS) en_US libritts_r medium (int8)", "ship_ok": False},
    "piper-vctk-medium": {
        "artifact": "tts-piper-en_GB-vctk-medium", "kind": "vits", "lexicon": "vits",
        "files": {"--model": "en_GB-vctk-medium.onnx", "--tokens": "tokens.txt"},
        "label": "Piper (VITS) en_GB vctk medium", "ship_ok": False},
    "piper-arctic-medium": {
        "artifact": "tts-piper-en_US-arctic-medium", "kind": "vits", "lexicon": "vits",
        "files": {"--model": "en_US-arctic-medium.onnx", "--tokens": "tokens.txt"},
        "label": "Piper (VITS) en_US arctic medium", "ship_ok": False},
    "matcha-ljspeech": {
        "artifact": "tts-matcha-en_US-ljspeech", "kind": "matcha", "lexicon": "vits",
        "files": {"--model": "model-steps-3.onnx", "--tokens": "tokens.txt"},
        "vocoder": ("tts-vocoder-vocos-22khz", "vocos-22khz-univ.onnx"),
        "label": "Matcha (icefall) en_US ljspeech + Vocos", "ship_ok": True},
}

# Test-audio voices: (name, tts engine, speaker id, speed, pitch semitones).
# Speed > 1 is faster. A pitch shift resamples and the TTS speed
# compensates, so the final speed is as given.
VOICES = [
    ("kokoro-af_heart", "kokoro-v1.0", 3, 1.0, 0.0),
    ("kokoro-am_michael", "kokoro-v1.0", 16, 0.9, 0.0),
    ("kokoro-bf_emma", "kokoro-v1.0", 21, 1.05, 0.0),
    ("kokoro-bm_george", "kokoro-v1.0", 26, 1.1, -1.5),
    ("kokoro-af_nicole-slow", "kokoro-v1.0", 6, 0.75, 0.0),
    ("kokoro-am_puck", "kokoro-v1.0", 18, 1.0, 1.0),
    ("piper-libritts-30", "piper-libritts_r-medium", 30, 1.0, 2.0),
    ("piper-libritts-300", "piper-libritts_r-medium", 300, 0.85, 0.0),
    ("piper-libritts-700-slow", "piper-libritts_r-medium", 700, 0.75, -2.0),
    ("piper-vctk-10", "piper-vctk-medium", 10, 1.0, 0.0),
    ("piper-vctk-60", "piper-vctk-medium", 60, 0.9, 1.0),
    ("piper-arctic-3", "piper-arctic-medium", 3, 1.0, 0.0),
    ("piper-ljspeech", "piper-ljspeech-medium", 0, 1.1, 0.0),
    ("matcha-ljspeech", "matcha-ljspeech", 0, 0.9, 0.0),
]

# The browser's own voice in the "echo" conditions.
BROWSER_VOICE = ("kokoro-bf_isabella", "kokoro-v1.0", 22, 1.0, 0.0)
