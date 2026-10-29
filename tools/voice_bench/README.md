# Voice bench

Measures the open-source, on-device speech engines that touchless browsing
could use (docs/design/TOUCHLESS_BROWSING.md, §9 and Phase 0): wake phrase,
end of speech, streaming and per-utterance recognition, recognition biased
toward the page, and voices. The results and the recommendation are in
[docs/VOICE_BENCH.md](../../docs/VOICE_BENCH.md). Nothing here goes into the
browser.

Everything runs locally against pinned downloads. Large files live in a data
directory outside the repository (default `~/.cache/moltbrowser-voice-bench`,
`%LOCALAPPDATA%\moltbrowser-voice-bench` on Windows; override with `--dir` or
`VOICE_BENCH_DIR`). Python is the standard library plus numpy (only
`gen_audio.py` and `audio.py` need numpy). Run Python isolated (`-I`), from
the repository, never from inside the data directory.

## Pieces

| File | What it does |
|---|---|
| `artifacts.json` | Every download: URL (GitHub releases, or Google's TensorFlow datasets bucket), size, SHA-256, licence, and whether sherpa-onnx's own `checksum.txt` agrees. |
| `fetch.py` | Downloads and checks them; unpacks each archive into its own new directory, refusing links and paths that leave it. |
| `driver/sherpa_bench.c` | The measuring tool: sherpa-onnx through its C API (what a MoltBrowser speech service would link), printing JSON lines. Modes: `offline`, `stream` (with Silero VAD, simulated real time), `kws`, `tts`, `info`. |
| `testset/utterances.tsv` | 300 spoken commands in seven categories, with accepted alternatives and the names that must come out right. |
| `testset/pages.json` | 12 simulated screens of 20 results each (titles and channels), for biasing and choosing. |
| `testset/lexicon-extra.txt` | Pronunciations the TTS lexicons lack for the test set's words. |
| `testset/page_prose.txt` | 400 sentences of MoltBrowser's own docs, read over music as "page audio" for the false wake-up test. |
| `gen_audio.py`, `audio.py` | Renders the test audio: 14 voices (speeds 0.75–1.1, pitch shifts), noise, babble, music and the browser's own speech; wake-phrase clips; page audio; real speech sets. |
| `catalog.py` | The engines and models measured, and the test voices. |
| `run_bench.py` | Builds the driver and runs every measurement, one at a time under an optional lock file, three runs for timings. |
| `report.py` | Markdown tables (and a JSON summary) from the results. |
| `build_whisper_cpp.sh` | whisper.cpp at the commit `scripts/bundle-whisper.sh` pins, for the `whisper-cpp` step. |
| `voice_bench_lib.py` | Test set loading and checks, normalization, WER, command and entity accuracy, hotwords, SentencePiece and ONNX readers. |
| `voice_bench_test.py` | Offline tests of the above. |

## Running it

Linux and macOS (arm64):

```bash
python3 -I tools/voice_bench/fetch.py --set auto          # ~4.5 GB
python3 -I tools/voice_bench/fetch.py --set sizes         # other OSes' libraries, sizes only
python3 -I tools/voice_bench/run_bench.py --build          # cmake + C compiler
python3 -m pip install --user --require-hashes \
    -r tools/voice_bench/requirements.txt                  # numpy, if missing
python3 -I tools/voice_bench/gen_audio.py                  # ~30 min, ~0.5 GB of audio
python3 -I tools/voice_bench/run_bench.py                  # several hours; resumable
tools/voice_bench/build_whisper_cpp.sh ~/.cache/moltbrowser-voice-bench
python3 -I tools/voice_bench/run_bench.py --steps whisper-cpp \
    --ggml-dir .molt-whisper-cache                         # real ggml models, if present
python3 -I tools/voice_bench/report.py --out report.md --json summary.json
```

Windows (x64, from a "x64 Native Tools" prompt with CMake and Python 3):

```bat
py -3 -I tools\voice_bench\fetch.py --set auto
py -3 -I tools\voice_bench\run_bench.py --build
py -3 -m pip install --user --require-hashes -r tools\voice_bench\requirements.txt
py -3 -I tools\voice_bench\gen_audio.py
py -3 -I tools\voice_bench\run_bench.py
py -3 -I tools\voice_bench\report.py --out report.md
```

`build_whisper_cpp.sh` needs a POSIX shell (Git Bash works) or the two
`cmake` commands in it run by hand.

Useful options: `--suite quick` (a fifth of the utterances, fewer conditions,
one text per voice), `--steps offline,stream`, `--models moonshine-tiny,...`,
`--lock /path/file` (on a shared machine: each timed run takes this flock, one
run at a time; accuracy runs don't),
`--threads N` for accuracy runs (timings always run 1, 2 and 4 threads).
Every step skips work whose results already exist, so an interrupted run
continues where it stopped; delete a results file to redo it.

Tests (no downloads): `python3 -I tools/voice_bench/voice_bench_test.py`.

## What the numbers mean

- **WER**: word error rate after normalization (case, punctuation, digits
  as words), against the closest accepted transcript.
- **Command accuracy**: the whole normalized transcript is one of the
  accepted ones. This is what a strict command grammar would see.
- **Entity accuracy**: the names in the utterance (songs, artists, sites)
  appear exactly.
- **Streaming latency** is simulated: the audio is fed in 100 ms chunks as a
  microphone would deliver them, and a result counts as available when the
  compute for its chunk would have finished on this machine. *First partial*
  is measured from the start of speech; *final* from the end of speech, with
  Silero VAD closing the utterance after 0.5 s of silence.
- **Per-utterance latency** is decode time; with a VAD in front, the user
  waits for the VAD's silence (0.5 s) plus that.
- **Wake phrase**: false rejects are wake clips with no detection; false
  accepts are detections per hour of audio that does not contain the phrase.

Synthetic voices flatter recognizers: they are clean and consistent, and the
same engines' voices may share training data with the recognizers. The real
speech sets (Speech Commands words, a few LibriSpeech and JFK sentences) are
the check. Timings depend on the machine; re-run the kit on the target
hardware.
