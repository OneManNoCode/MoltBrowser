# Voice bench: on-device speech engines for touchless browsing

Status: **Phase 0 measurements**, October 2026. Nothing here is in the
browser. These are the numbers for the owner's first decision in
[TOUCHLESS_BROWSING.md §11](design/TOUCHLESS_BROWSING.md#11-decisions-for-the-owner):
sherpa-onnx as the speech engine (the design's §9 recommendation), or
whisper.cpp only. The owner decides; §6 gives a recommendation.

The kit that produced them is
[`tools/voice_bench/`](../tools/voice_bench/README.md). It downloads every engine and model pinned by SHA-256, renders the audio
test set, runs the measurements and writes the tables below. The build
machine re-runs it for the real numbers (§2.4).

## 1. Summary

Measured on a shared 4-vCPU server VM with synthetic voices plus a
real-speech check; the build machine re-runs the kit for the real numbers.
Section references like "design §9" point into
[TOUCHLESS_BROWSING.md](design/TOUCHLESS_BROWSING.md).

- **Accuracy.** The best streaming model, **Kroko EN**, gets 70% of the
  spoken commands exactly right and 84% of real single words, close to the
  most accurate model measured, Whisper small.en (78%, 86%), and better than
  the Whisper tiny.en MoltBrowser bundles (68%, 71%). The LibriSpeech
  Zipformers the design assumed miss modern names ("you tube") and are not
  good enough.
- **Latency.** With voice activity detection, the text is ready 0.55–0.8 s
  after the user stops speaking for every model but Whisper small.en
  (1.4 s); most of that is the 0.5 s of silence that ends an utterance.
  Streaming adds live captions about 1 s after the user starts.
- **Names** are the weak spot: searches with song and artist names are the
  hardest commands for every model. Biasing the recognizer toward the
  names on screen helps a lot where the kit can cut the names into the
  model's own pieces (names on screen recognized: Zipformer 70M from 43% to
  70%, FastConformer from 54% to 84%), and barely for Kroko (63% to 66%),
  whose archive ships no tokenizer model.
- **The browser's own voice** wrecks recognition without echo
  cancellation (0–3% of commands right for most models); with it they
  recover partly or fully. Echo cancellation and barge-in are requirements.
- **Wake phrase.** The open-vocabulary spotter misses 7–15% of clean wake
  phrases and 15–55% in noise, music or chatter, with up to two false
  wake-ups an hour from sound-alike phrases ("hey mom") and none from page
  audio. Misses cluster on particular voices, so it must adapt per user.
- **Voices.** Piper starts speaking in under 0.15 s; Kokoro, the most
  natural, takes 0.7–1.5 s on this fast CPU, too slow for modest
  hardware. sherpa-onnx 2.x reads English through a lexicon only and
  silently skips unknown words, so a bundled voice needs a text front end.
- **Cost.** About 32 MB of libraries on Linux x64 plus about 80 MB of
  recognition models; 4–6% of a core to listen for the wake phrase or run
  Kroko continuously. Chromium 156 doesn't vendor ONNX Runtime;
  vendoring it is the main integration cost (§5).

§6 recommends keeping sherpa-onnx, as the design's §9 does, with Kroko as
the streaming model and Whisper small.en as an optional second pass.

## 2. Method

### 2.1 Candidates

Everything is open source with a licence that allows commercial use
(§4); what isn't was left out (openWakeWord's models, Moonshine's
non-English models, Piper's `l2arctic` voice) or is used only to make test
audio (Piper voices fine-tuned from `lessac`).

| Need | Measured |
|---|---|
| Wake phrase | sherpa-onnx open-vocabulary keyword spotting: the GigaSpeech Zipformer KWS (3.3M parameters, SentencePiece word pieces) and the newer zh+en model (3M, 2025-12-20, English as CMU phones). Phrases: "hey molt", "hello molt", "hey molt browser". |
| End of speech | Silero VAD (v4, through sherpa-onnx), 0.5 s of silence closes an utterance. |
| Streaming recognition | sherpa-onnx streaming transducers: Zipformer EN 20M (2023-02-17), Zipformer EN 70M (2023-06-26), Kroko EN (2025-08-06), NeMo FastConformer 80 ms (int8), and the newest English one, NVIDIA Nemotron Speech Streaming EN 0.6B (560 ms chunks, int8, 2026-04-25). |
| Per-utterance recognition | Moonshine tiny and base (v1, int8) and v2 tiny and base (2026-02-27), NVIDIA Parakeet TDT 0.6B v2 (int8), Whisper tiny.en, base.en and small.en. |
| Biasing toward the screen | sherpa-onnx hotwords (modified beam search) on every streaming transducer, with the page's titles and channels as the list. |
| Voices | Kokoro 82M v1.0 (fp32 and int8), Piper/VITS (`ljspeech`, `libritts_r`, `vctk`, `arctic`; `libritts_r` int8), Matcha (LJ Speech) with the Vocos vocoder, all through sherpa-onnx. |
| Today's engine | whisper.cpp at the tag `scripts/bundle-whisper.sh` pins (v1.7.5), one process per request. |

Whisper ran from sherpa-onnx's ONNX exports, because whisper.cpp's ggml
weights are only on Hugging Face, which this machine can't reach. The same
OpenAI weights, a different runtime, int8 (and fp32 for tiny.en) instead of
whisper.cpp's f16. whisper.cpp
itself was timed with its own vocabulary-only test models (§3.10).

All engines run through **sherpa-onnx v2.0.1** (ONNX Runtime 1.28.2), the
prebuilt release, driven by a small C program over sherpa-onnx's C API: the
API a MoltBrowser speech service would link.

### 2.2 Test set

`tools/voice_bench/testset/utterances.tsv`: 300 utterances from the plan's
scenarios.

| Category | Lines | Examples |
|---|---|---|
| nav | 45 | "open YouTube", "go to google dot com", "switch to tab three" |
| search | 60 | "search YouTube for Hotel California by the Eagles", "find Blinding Lights by The Weeknd" |
| choice | 60 | "play the fourth one", "number twelve", "play the live one by Queen", "the one below that" |
| media | 30 | "skip ahead one minute", "turn on captions" |
| read | 25 | "what's on the screen", "next heading" |
| dictation | 45 | "type I'll be home at six", "no I said Queen", "yes", "never mind" |
| chatter | 35 | non-commands, several close to the wake phrase: "hey Walt are you coming with us", "snakes molt their skin", "hey the mold is back" |

Each line lists the transcripts that also count as right and the names
that must come out exactly (168 in all). `pages.json` holds 12 simulated
screens of 20 results each (YouTube searches, a music chart, news, a
Wikipedia search, shopping, mail, podcasts); every utterance names the
screen it is spoken at, and every name in a choice is on that screen.

### 2.3 Audio

Rendered by `gen_audio.py` (never committed):

- **Voices:** 14 synthetic speakers: six Kokoro voices (US and UK, female
  and male), Piper `libritts_r` (three speakers), `vctk` (two, British),
  `arctic` (Indian English), `ljspeech`, and Matcha. Speeds 0.75 to 1.1
  (two "slow speaker" voices at 0.75), pitch shifts of −2 to +2 semitones.
  Each utterance is read by two different voices (sets a and b).
- **Conditions** (on set a): clean; pink noise at 20, 10 and 5 dB SNR;
  babble (three other voices) at 10 dB; synthesized music at 10 dB; the
  browser's own voice reading a result list at 0 dB (no echo cancellation,
  "echo0"); and the same after 25 dB of echo cancellation ("echo0-aec", an
  assumed but typical figure for WebRTC's canceller). SNR is measured over
  the speech.
- **Clips:** 0.4–0.8 s of silence, the utterance, 1.3 s of silence, with a
  −65 dBFS noise floor and a random level around −26 dBFS.
- **Wake phrase:** 100 commands, each preceded by the phrase and a short
  pause, per phrase and per condition (clean, pink 10 dB, babble, music).
- **Page audio** (for false wake-ups): 400 sentences of MoltBrowser's own
  documentation and the 12 pages' result lists read by four voices, over
  music, in 60 s clips.
- **Real speech:** Google's Speech Commands test set (CC BY 4.0), one
  word per clip from many speakers: 20 clips each of yes, no, stop, go, up,
  down, left, right, on, off, and the 240 digit words in its "unknown"
  folder; all 4,890 clips for false wake-ups; plus the LibriSpeech and JFK
  samples that ship with the models.

**Synthetic voices flatter recognizers.** They are clean and evenly paced,
and some share training data with recognizers (LibriTTS-R comes from the
same LibriVox audiobooks as LibriSpeech; VCTK and LJ Speech are common
training sets).
Absolute accuracy here is an upper bound; the order of the engines is the
useful part, and the real-speech rows are the check.

### 2.4 Measurement

- **Accuracy** after normalization (case, punctuation, digits as words,
  "ok"/"okay"): word error rate against the closest accepted transcript;
  command accuracy (the whole transcript is an accepted one); entity
  accuracy (each name appears exactly).
- **As deployed:** a per-utterance recognizer decodes what Silero VAD
  finds in the clip (plus 0.25 s each side), so the user waits 0.5 s of
  silence plus the decode. A streaming recognizer gets the clip in 100 ms
  chunks, in simulated real time; Silero VAD's end of speech (0.5 s of
  silence) triggers the final result, after the model gets enough silence
  to complete its last chunk (0.3 s; 1.2 s for Kroko, whose chunks are
  1.28 s long). That compute counts in the latency. A clip is one
  utterance: the stream stops at the first 0.5 s pause, as a deployed
  service would close the utterance there.
- **Listening cost:** the wake-phrase spotter (or a streaming recognizer)
  with the VAD fed in real time, process CPU time over wall time.
- **Timing** ran under the shared heavy-job lock, three times, and the
  tables give the median: decode time per utterance (40 clean clips) at 1,
  2 and 4 threads, load time, peak resident memory (the whole process,
  runtime and driver included).

**The machine is a shared 4-vCPU VM** (Intel Xeon Sapphire Rapids at
2.1 GHz under KVM, AVX-512 and AMX, 15 GB; other jobs ran between
measurements). It is much faster per core than MoltBrowser's minimum
hardware is likely to be and has AMX, which ONNX Runtime's int8 kernels use.
The build machine re-runs the kit (`tools/voice_bench/README.md`) for the
real numbers; treat the timings here as relative.

## 3. Results

The machine (`lscpu`, abridged):

- CPU(s): 4
- Model name: Intel(R) Xeon(R) Processor @ 2.10GHz
- Thread(s) per core: 1
- SIMD flags: fma sse4_1 sse4_2 avx f16c bmi1 avx2 bmi2 avx512f avx512dq avx512ifma avx512cd avx512bw avx512vl avx_vnni avx512_bf16 avx512vbmi avx512_vbmi2 avx512_vnni avx512_bitalg avx512_vpopcntdq amx_bf16 avx512_fp16 amx_tile amx_int8
- Hypervisor vendor: KVM
- Virtualization type: full
- L3 cache: 260 MiB (1 instance)
- Linux-6.18.44-fc-v185-x86_64-with-glibc2.39, Python 3.13.16, 4 logical CPUs, MemTotal: 16477352 kB
- driver: `{"event":"info","version":"2.0.1","git":"af2d8d81","onnxruntime":"1.28.2","rss_kb":7788}`

### 3.1 Recognition accuracy

Clean synthetic speech, 300 utterances read by two voices each (600 clips;
the slowest models read one set, 300 clips):

| model | kind | clips | WER | command acc. | entity acc. | WER, Kokoro voices | command | entity |
|---|---|---|---|---|---|---|---|---|
| Moonshine tiny (v1, int8) | offline | 600 | 16.7% | 63.8% | 66.4% | 8.3% | 78.1% | 81.0% |
| Moonshine base (v1, int8) | offline | 600 | 13.7% | 67.8% | 70.2% | 6.5% | 81.1% | 83.7% |
| Moonshine v2 tiny (quantized) | offline | 600 | 16.9% | 62.5% | 65.8% | 8.3% | 75.1% | 78.9% |
| Moonshine v2 base (quantized) | offline | 600 | 13.6% | 67.5% | 70.8% | 6.5% | 80.8% | 85.0% |
| Parakeet TDT 0.6B v2 (int8) | offline | 600 | 13.3% | 68.8% | 67.9% | 6.0% | 80.8% | 79.6% |
| Whisper tiny.en (ONNX int8) | offline | 600 | 14.2% | 68.3% | 67.9% | 7.9% | 80.0% | 78.2% |
| Whisper tiny.en (ONNX fp32) | offline | 300 | 14.1% | 66.7% | 66.7% | 8.1% | 78.8% | 76.8% |
| Whisper base.en (ONNX int8) | offline | 600 | 18.9% | 69.5% | 72.6% | 15.5% | 80.8% | 78.9% |
| Whisper small.en (ONNX int8) | offline | 300 | 8.9% | 78.3% | 81.5% | 3.9% | 88.6% | 92.7% |
| Zipformer EN 20M (2023-02-17, int8) | stream | 600 | 71.7% | 0.3% | 25.6% | 66.1% | 0.0% | 32.7% |
| Zipformer EN 70M (2023-06-26, int8) | stream | 600 | 27.1% | 44.7% | 42.9% | 19.6% | 55.8% | 53.7% |
| Kroko EN (zipformer, 2025-08-06) | stream | 600 | 11.7% | 70.3% | 66.7% | 5.6% | 82.3% | 77.6% |
| NeMo FastConformer streaming 80 ms (int8) | stream | 600 | 19.1% | 57.5% | 60.1% | 11.6% | 70.6% | 68.7% |
| Nemotron Speech Streaming EN 0.6B, 560 ms (int8) | stream | 600 | 10.9% | 70.2% | 73.5% | 5.0% | 83.8% | 84.4% |

- **Per-utterance:** Whisper small.en is clearly the most accurate (8.9%
  word error rate, 78% of commands exactly right). The rest cluster at 13–17%
  and 63–70%: Parakeet, Moonshine base (v1 and v2) and Whisper tiny.en, the
  model MoltBrowser bundles today (here through ONNX). Whisper base.en's
  word error rate is inflated by transcripts stuck in a loop (§3.2); its
  command accuracy is at the top of the cluster.
- **Streaming:** Kroko (2025, 71 MB) and Nemotron 0.6B (662 MB) are as
  accurate as the best per-utterance models short of small.en: 11–12%
  and 70%. NeMo FastConformer 80 ms is behind (19%, 58%). The Zipformer 70M,
  trained on LibriSpeech audiobooks only, misses modern words ("open you
  tube", "go to wickeypedia", "open jimmi" for Gmail, "net flakes"): 27%
  and 45%. The Zipformer 20M loses the first 0.5–1 s of nearly every
  utterance, in sherpa-onnx v2.0.1 and v1.12.40 alike and on its own test
  files too, so it can't be used as published.
- Most recognizers make about half as many errors on the Kokoro voices as
  on the rest (next tables).

Command accuracy by kind of utterance (clean):

| model | nav | search | choice | media | read | dictation | chatter |
|---|---|---|---|---|---|---|---|
| Moonshine tiny (v1, int8) | 71% | 52% | 72% | 55% | 82% | 60% | 60% |
| Moonshine base (v1, int8) | 70% | 48% | 73% | 72% | 90% | 67% | 71% |
| Moonshine v2 tiny (quantized) | 66% | 48% | 69% | 53% | 80% | 64% | 64% |
| Moonshine v2 base (quantized) | 71% | 48% | 72% | 70% | 90% | 64% | 73% |
| Parakeet TDT 0.6B v2 (int8) | 76% | 46% | 71% | 73% | 92% | 70% | 74% |
| Whisper tiny.en (ONNX int8) | 77% | 47% | 70% | 75% | 86% | 72% | 69% |
| Whisper tiny.en (ONNX fp32) | 78% | 48% | 75% | 73% | 76% | 64% | 60% |
| Whisper base.en (ONNX int8) | 78% | 52% | 70% | 78% | 80% | 71% | 70% |
| Whisper small.en (ONNX int8) | 84% | 60% | 83% | 90% | 88% | 76% | 80% |
| Zipformer EN 20M (2023-02-17, int8) | 0% | 0% | 0% | 0% | 4% | 0% | 0% |
| Zipformer EN 70M (2023-06-26, int8) | 32% | 22% | 49% | 45% | 72% | 61% | 51% |
| Kroko EN (zipformer, 2025-08-06) | 84% | 41% | 75% | 73% | 86% | 76% | 74% |
| NeMo FastConformer streaming 80 ms (int8) | 54% | 36% | 62% | 60% | 86% | 68% | 56% |
| Nemotron Speech Streaming EN 0.6B, 560 ms (int8) | 79% | 52% | 75% | 72% | 86% | 69% | 70% |

Searches, full of song and artist names, are the hardest for every model
(22–60% exactly right, leaving out the broken Zipformer 20M), and choices
("play the fourth one") and reading commands the easiest. Names are where recognition fails, which is what
biasing toward the screen (§3.7) and the spoken-choice matcher's
sound-alike matching are for. Command accuracy is the strict measure: the
intent layer accepts more than an exact transcript.

How much the test voice matters:

| test voice | clips | median WER over per-utterance recognizers | best | worst |
|---|---|---|---|---|
| kokoro-af_heart | 49 | 5.6% | 4.6% | 9.7% |
| kokoro-af_nicole-slow | 36 | 5.2% | 3.9% | 7.1% |
| kokoro-am_michael | 45 | 4.4% | 0.5% | 10.7% |
| kokoro-am_puck | 46 | 7.3% | 3.6% | 16.7% |
| kokoro-bf_emma | 47 | 9.6% | 4.8% | 12.4% |
| kokoro-bm_george | 42 | 10.1% | 4.8% | 66.3% |
| matcha-ljspeech | 43 | 9.3% | 6.0% | 10.9% |
| piper-arctic-3 | 48 | 18.0% | 15.5% | 38.7% |
| piper-libritts-30 | 36 | 17.8% | 13.8% | 24.1% |
| piper-libritts-300 | 41 | 14.2% | 10.5% | 17.9% |
| piper-libritts-700-slow | 45 | 15.6% | 13.4% | 26.7% |
| piper-ljspeech | 43 | 17.6% | 16.0% | 23.9% |
| piper-vctk-10 | 36 | 31.2% | 28.8% | 34.1% |
| piper-vctk-60 | 43 | 31.6% | 27.8% | 39.0% |

82 of 600 clean clips were transcribed exactly by no per-utterance recognizer.

The test voice moves the word error rate more than the recognizer does:
4–10% on the Kokoro and Matcha voices, 14–32% on Piper's, worst on the two
British VCTK voices. Part of that is the voices themselves (Piper's
mispronounce some words, and their lexicon drops words, §3.9), so read the
Piper rows as hard audio, not as typical users.

### 3.2 Noise, music, other voices and the browser's own speech

Word error rate by condition (set a):

| model | clean | pink20 | pink10 | pink5 | babble10 | music10 | echo0 | echo0-aec |
|---|---|---|---|---|---|---|---|---|
| Moonshine tiny (v1, int8) | 15.9% | 16.8% | 22.3% | 33.2% | 68.8% | 15.9% | 197.2% | 39.4% |
| Moonshine base (v1, int8) | 13.4% | 15.7% | 19.3% | 28.5% | 59.7% | 13.9% | 192.3% | 30.2% |
| Moonshine v2 tiny (quantized) | 16.6% | 18.4% | 24.0% | 38.9% | 113.5% | 18.4% | 252.6% | 48.1% |
| Moonshine v2 base (quantized) | 14.1% | 16.2% | 24.7% | 30.1% | 99.4% | 16.9% | 248.1% | 37.7% |
| Parakeet TDT 0.6B v2 (int8) | 14.0% | – | 13.0% | 20.8% | 42.5% | 10.3% | 147.5% | – |
| Whisper tiny.en (ONNX int8) | 14.0% | 16.3% | 26.2% | 35.3% | 56.8% | 16.2% | 168.9% | 17.6% |
| Whisper tiny.en (ONNX fp32) | 14.1% | – | 24.8% | – | – | – | – | – |
| Whisper base.en (ONNX int8) | 21.7% | – | 18.0% | 27.0% | 26.4% | 15.7% | 154.3% | – |
| Whisper small.en (ONNX int8) | 8.9% | – | 12.9% | – | – | – | – | – |
| Zipformer EN 20M (2023-02-17, int8) | 71.8% | 71.6% | 78.5% | 83.5% | 81.5% | 72.9% | 113.5% | 71.2% |
| Zipformer EN 70M (2023-06-26, int8) | 26.7% | 28.9% | 33.0% | 44.6% | 49.2% | 27.2% | 138.4% | 26.7% |
| Kroko EN (zipformer, 2025-08-06) | 11.4% | 11.9% | 15.1% | 20.3% | 22.8% | 11.5% | 70.3% | 11.3% |
| NeMo FastConformer streaming 80 ms (int8) | 18.5% | 20.4% | 24.4% | 33.5% | 53.9% | 18.7% | 176.3% | 32.0% |
| Nemotron Speech Streaming EN 0.6B, 560 ms (int8) | 10.9% | – | 14.3% | 21.8% | 16.1% | 9.8% | 137.8% | – |

Command accuracy by condition:

| model | clean | pink20 | pink10 | pink5 | babble10 | music10 | echo0 | echo0-aec |
|---|---|---|---|---|---|---|---|---|
| Moonshine tiny (v1, int8) | 65% | 64% | 56% | 41% | 22% | 63% | 0% | 43% |
| Moonshine base (v1, int8) | 66% | 65% | 58% | 44% | 33% | 68% | 0% | 52% |
| Moonshine v2 tiny (quantized) | 62% | 64% | 52% | 38% | 19% | 60% | 0% | 41% |
| Moonshine v2 base (quantized) | 65% | 65% | 59% | 46% | 31% | 67% | 0% | 50% |
| Parakeet TDT 0.6B v2 (int8) | 68% | – | 69% | 58% | 41% | 74% | 0% | – |
| Whisper tiny.en (ONNX int8) | 67% | 62% | 51% | 37% | 38% | 61% | 2% | 64% |
| Whisper tiny.en (ONNX fp32) | 67% | – | 52% | – | – | – | – | – |
| Whisper base.en (ONNX int8) | 66% | – | 64% | 48% | 54% | 68% | 3% | – |
| Whisper small.en (ONNX int8) | 78% | – | 69% | – | – | – | – | – |
| Zipformer EN 20M (2023-02-17, int8) | 0% | 0% | 0% | 0% | 1% | 0% | 0% | 1% |
| Zipformer EN 70M (2023-06-26, int8) | 45% | 42% | 36% | 24% | 18% | 43% | 0% | 44% |
| Kroko EN (zipformer, 2025-08-06) | 70% | 70% | 63% | 54% | 53% | 69% | 21% | 70% |
| NeMo FastConformer streaming 80 ms (int8) | 60% | 55% | 52% | 39% | 23% | 58% | 0% | 36% |
| Nemotron Speech Streaming EN 0.6B, 560 ms (int8) | 68% | – | 63% | 50% | 61% | 71% | 0% | – |

- **Noise and music.** Pink noise down to 10 dB costs 1 to 12 points of
  word error rate, 5 dB a lot more. Music at 10 dB barely matters. Other
  voices (babble at 10 dB) hurt most: Nemotron, Whisper base.en and Kroko
  still get 53–61% of commands right, Moonshine v2 falls apart.
- **The browser's own speech.** With the browser reading results at the
  same level and no echo cancellation ("echo0"), every recognizer
  transcribes the browser too: 0–3% of commands right (Kroko 21%). With
  25 dB of echo cancellation, Whisper tiny.en and Kroko are back to their
  clean accuracy; Moonshine and FastConformer still lose 14–24 points. So
  echo cancellation with the browser's output as the reference (design
  §6.1, step 2) is required, and the voice should duck or stop when the
  user starts speaking (barge-in) to cover what cancellation leaves.

Transcripts stuck in a loop (a word repeated four or more times in a row,
over all synthetic clips):

| model | synthetic clips | runaway repeats | share |
|---|---|---|---|
| Moonshine tiny (v1, int8) | 2700 | 2 | 0.07% |
| Moonshine base (v1, int8) | 2700 | 9 | 0.33% |
| Moonshine v2 tiny (quantized) | 2700 | 9 | 0.33% |
| Moonshine v2 base (quantized) | 2700 | 19 | 0.70% |
| Parakeet TDT 0.6B v2 (int8) | 2100 | 0 | 0.00% |
| Whisper tiny.en (ONNX int8) | 2700 | 9 | 0.33% |
| Whisper tiny.en (ONNX fp32) | 600 | 1 | 0.17% |
| Whisper base.en (ONNX int8) | 2100 | 20 | 0.95% |
| Whisper small.en (ONNX int8) | 600 | 0 | 0.00% |
| Zipformer EN 20M (2023-02-17, int8) | 2700 | 0 | 0.00% |
| Zipformer EN 70M (2023-06-26, int8) | 2700 | 0 | 0.00% |
| Kroko EN (zipformer, 2025-08-06) | 2700 | 0 | 0.00% |
| NeMo FastConformer streaming 80 ms (int8) | 2700 | 0 | 0.00% |
| Nemotron Speech Streaming EN 0.6B, 560 ms (int8) | 2100 | 0 | 0.00% |

The autoregressive decoders (Moonshine, Whisper) sometimes repeat a word
until they hit their length limit, 0.1–1% of clips and more in noise; the
transducers never do. A service using one should cap the output by the
audio's length and treat a loop as a failed recognition.

### 3.3 Real speech

| model | Speech Commands words | word correct | LibriSpeech + JFK WER (4 clips) |
|---|---|---|---|
| Moonshine tiny (v1, int8) | 440 | 70.9% | 12.1% |
| Moonshine base (v1, int8) | 440 | 71.4% | 12.1% |
| Moonshine v2 tiny (quantized) | 440 | 73.0% | 15.2% |
| Moonshine v2 base (quantized) | 440 | 71.4% | 10.1% |
| Parakeet TDT 0.6B v2 (int8) | 440 | 78.2% | 10.1% |
| Whisper tiny.en (ONNX int8) | 440 | 70.7% | 14.1% |
| Whisper tiny.en (ONNX fp32) | 440 | 73.2% | 13.1% |
| Whisper base.en (ONNX int8) | 440 | 77.3% | 11.1% |
| Whisper small.en (ONNX int8) | 440 | 85.7% | 9.1% |
| Zipformer EN 20M (2023-02-17, int8) | 440 | 1.1% | 41.4% |
| Zipformer EN 70M (2023-06-26, int8) | 440 | 81.1% | 31.3% |
| Kroko EN (zipformer, 2025-08-06) | 440 | 84.1% | 23.2% |
| NeMo FastConformer streaming 80 ms (int8) | 440 | 83.2% | 29.3% |
| Nemotron Speech Streaming EN 0.6B, 560 ms (int8) | 440 | 42.3% | 30.3% |

The LibriSpeech clips are two 16 kHz test sentences and one 8 kHz (telephone-band) one. Streaming runs treat a clip as one utterance, so they stop at the first 0.5 s pause (the JFK clip has one); per-utterance runs decode every VAD segment.

Real speech (single words from many speakers) keeps the top of the
synthetic ranking: Whisper small.en 86%, Kroko 84%, FastConformer 83%,
Zipformer 70M 81% (no modern names to miss here), Parakeet 78%; Moonshine
and Whisper tiny.en 71–73%. **Nemotron returns nothing for many isolated
words** (42%): more silence after the word helps only partly (16 of 40
words with 0.3 s, 26 with 1.5 s, at 0.4 s more latency). That matters for
"yes", "no" and "stop". The LibriSpeech column has only four clips; the
streaming rows lose the JFK clip after its first pause and do badly on the
8 kHz clip.

### 3.4 Per-utterance latency

Median of three runs over 40 clean clips (1.3 s of speech on average,
0.6–2.6 s); "end of speech → text" is the VAD's 0.5 s of silence plus the
decode at two threads:

| model | decode ms, 1 thr | 2 thr | 4 thr | RTF 1 thr | RTF 4 thr | load ms | peak RSS MB | end of speech → text, ms (2 thr) |
|---|---|---|---|---|---|---|---|---|
| Moonshine tiny (v1, int8) | 39 | 38 | 104 | 0.012 | 0.034 | 696 | 193.1 | 538 |
| Moonshine base (v1, int8) | 77 | 70 | 145 | 0.026 | 0.046 | 1219 | 379.9 | 570 |
| Moonshine v2 tiny (quantized) | 39 | 36 | 47 | 0.013 | 0.014 | 433 | 147.5 | 536 |
| Moonshine v2 base (quantized) | 73 | 60 | 81 | 0.023 | 0.026 | 794 | 350.8 | 560 |
| Parakeet TDT 0.6B v2 (int8) | 320 | 227 | 215 | 0.107 | 0.068 | 2809 | 853.0 | 727 |
| Whisper tiny.en (ONNX int8) | 244 | 171 | 166 | 0.080 | 0.054 | 556 | 259.3 | 671 |
| Whisper tiny.en (ONNX fp32) | 327 | 238 | 208 | 0.107 | 0.067 | 581 | 312.8 | 738 |
| Whisper base.en (ONNX int8) | 416 | 309 | 262 | 0.136 | 0.086 | 785 | 386.9 | 809 |
| Whisper small.en (ONNX int8) | 1195 | 915 | 750 | 0.375 | 0.235 | 1817 | 877.1 | 1415 |

Every per-utterance model except Whisper small.en has the text within
0.75 s of the end of speech, most of it the VAD's 0.5 s of silence:
Moonshine decodes a command in 40–75 ms, Parakeet in about 0.23 s, Whisper
tiny.en and base.en in 0.17–0.31 s (Whisper always encodes a 30 s window,
however short the command). Whisper small.en takes 0.9 s, 1.4 s after the
end of speech. Two threads is enough: four rarely helps on clips this short
and sometimes hurts on a shared machine. Loading takes 0.4–2.8 s, so the
model has to stay loaded in a resident service; one process per recording,
as MoltBrowser runs whisper-cli today, pays that every time (§3.10). Peak
memory is for the whole process.

### 3.5 Streaming latency

Simulated real time, 100 ms chunks, the same 40 clips, median of three
runs. *First partial* counts from the start of speech, *final text* from the
end of speech (the VAD needs 0.5 s of silence; "built-in endpoint" is
sherpa-onnx's own rule, for comparison):

| model | threads | first partial ms | p90 | VAD end ms | final text ms | p90 | built-in endpoint ms | RTF | load ms | peak RSS MB |
|---|---|---|---|---|---|---|---|---|---|---|
| Zipformer EN 20M (2023-02-17, int8) | 1 | 1068 | 1599 | 546 | 558 | 603 | 553 | 0.028 | 550 | 99.9 |
| Zipformer EN 20M (2023-02-17, int8) | 2 | 1069 | 1599 | 546 | 558 | 603 | 553 | 0.029 | 643 | 100.2 |
| Zipformer EN 70M (2023-06-26, int8) | 1 | 807 | 951 | 546 | 583 | 627 | – | 0.071 | 3142 | 153.3 |
| Zipformer EN 70M (2023-06-26, int8) | 2 | 803 | 949 | 546 | 580 | 624 | – | 0.067 | 3274 | 152.5 |
| Kroko EN (zipformer, 2025-08-06) | 1 | 1002 | 1137 | 546 | 599 | 652 | – | 0.040 | 2915 | 169.2 |
| Kroko EN (zipformer, 2025-08-06) | 2 | 998 | 1135 | 546 | 599 | 647 | – | 0.037 | 3241 | 170.6 |
| NeMo FastConformer streaming 80 ms (int8) | 1 | 724 | 883 | 546 | 693 | 744 | – | 0.285 | 1379 | 244.4 |
| NeMo FastConformer streaming 80 ms (int8) | 2 | 710 | 865 | 546 | 654 | 703 | – | 0.208 | 1385 | 244.6 |
| Nemotron Speech Streaming EN 0.6B, 560 ms (int8) | 1 | 1103 | 1367 | 546 | 815 | 903 | – | 0.383 | 3734 | 980.0 |
| Nemotron Speech Streaming EN 0.6B, 560 ms (int8) | 2 | 989 | 1274 | 546 | 722 | 784 | – | 0.266 | 3334 | 984.3 |

With the VAD closing the utterance, the final text comes 0.58–0.82 s after
the end of speech, nearly all of it the VAD's 0.5 s of silence. That is no
sooner than a per-utterance model, which waits for the same silence
(§3.4). What streaming adds is the live caption: the first words show
0.7–1.1 s after the user starts speaking (Kroko's 1.28 s chunks make it the
slowest of the accurate ones to show anything), and the recognizer has
heard everything by the time the user stops. The compute differs a lot:
Kroko needs 4% of one core of this machine, the Zipformer 70M 7%,
FastConformer and Nemotron 21–38%, which matters on slow machines and for
listening all the time (§3.6).

### 3.6 Listening all the time

The CPU an always-on listener costs, fed 30 s of page audio in real time
(median of three runs; 100% is one core):

| listening | threads | CPU, % of one core (median) | worst lag behind real time, ms |
|---|---|---|---|
| wake phrase (Zipformer KWS 3.3M) + VAD | 1 | 4.4 | 34 |
| wake phrase (Zipformer KWS 3.3M) + VAD | 2 | 34.5 | 4 |
| wake phrase (Zipformer KWS 3.3M) + VAD | 2, spinning off | 6.1 | 68 |
| Zipformer EN 70M (2023-06-26, int8) + VAD | 1 | 11.3 | 24 |
| Zipformer EN 70M (2023-06-26, int8) + VAD | 2 | 44.9 | 2 |
| Zipformer EN 70M (2023-06-26, int8) + VAD | 2, spinning off | 15.9 | 22 |
| Kroko EN (zipformer, 2025-08-06) + VAD | 1 | 5.7 | 21 |
| Kroko EN (zipformer, 2025-08-06) + VAD | 2 | 15.4 | 5 |
| Kroko EN (zipformer, 2025-08-06) + VAD | 2, spinning off | 7.4 | 32 |
| NeMo FastConformer streaming 80 ms (int8) + VAD | 1 | 34.7 | 3 |
| NeMo FastConformer streaming 80 ms (int8) + VAD | 2 | 99.1 | 19 |
| NeMo FastConformer streaming 80 ms (int8) + VAD | 2, spinning off | 44.9 | 2 |

- **The wake-phrase spotter with the VAD costs about 4% of one core** at
  one thread.
- **ONNX Runtime's threads spin-wait between runs by default.** With two
  threads that alone burns a third of a core for the spotter (34.5%
  against 6.1% with spinning off) and a whole core for FastConformer. A
  speech service must turn spinning off
  (`session.intra_op.allow_spinning=0`) or use one thread.
- **A streaming recognizer running all the time** costs 6% (Kroko) to
  35% (FastConformer) of a core, with the VAD. Kroko is little more than
  the spotter, so it could listen all the time, as the always-listening
  mode (design §5.1) needs; with the heavier models, start the recognizer
  only after the wake phrase.

### 3.7 Biasing toward the screen

Each utterance biased toward the 20 titles and channels of the screen it
is spoken at (split into names, as `page_phrases` does), with sherpa-onnx's
modified beam search; the baseline is the same beam search without
hotwords:

| model | set | decoding | WER | command acc. | names on screen right (n) | names not on screen right (n) | hotword encode errors |
|---|---|---|---|---|---|---|---|
| Zipformer EN 20M (2023-02-17, int8) | clean | beam | 55.2% | 1.7% | 39.3% (56) | 32.1% (112) | 0 |
| Zipformer EN 20M (2023-02-17, int8) | clean | hot 1.5 | 55.0% | 1.7% | 41.1% (56) | 32.1% (112) | 0 |
| Zipformer EN 20M (2023-02-17, int8) | clean | hot 3.0 | 55.0% | 1.7% | 41.1% (56) | 31.2% (112) | 0 |
| Zipformer EN 20M (2023-02-17, int8) | pink10 | beam | 63.7% | 1.7% | 30.4% (56) | 22.3% (112) | 0 |
| Zipformer EN 20M (2023-02-17, int8) | pink10 | hot 1.5 | 63.6% | 1.7% | 30.4% (56) | 22.3% (112) | 0 |
| Zipformer EN 20M (2023-02-17, int8) | pink10 | hot 3.0 | 63.8% | 1.7% | 28.6% (56) | 22.3% (112) | 0 |
| Zipformer EN 70M (2023-06-26, int8) | clean | beam | 26.5% | 44.3% | 42.9% (56) | 46.4% (112) | 0 |
| Zipformer EN 70M (2023-06-26, int8) | clean | hot 1.5 | 24.5% | 46.3% | 64.3% (56) | 46.4% (112) | 0 |
| Zipformer EN 70M (2023-06-26, int8) | clean | hot 3.0 | 24.2% | 46.7% | 69.6% (56) | 47.3% (112) | 0 |
| Zipformer EN 70M (2023-06-26, int8) | pink10 | beam | 32.9% | 35.3% | 33.9% (56) | 34.8% (112) | 0 |
| Zipformer EN 70M (2023-06-26, int8) | pink10 | hot 1.5 | 31.6% | 37.0% | 50.0% (56) | 34.8% (112) | 0 |
| Zipformer EN 70M (2023-06-26, int8) | pink10 | hot 3.0 | 31.4% | 36.3% | 55.4% (56) | 35.7% (112) | 0 |
| Kroko EN (zipformer, 2025-08-06) | clean | beam | 11.0% | 70.3% | 62.5% (56) | 67.0% (112) | 0 |
| Kroko EN (zipformer, 2025-08-06) | clean | hot 1.5 | 11.1% | 70.3% | 64.3% (56) | 67.0% (112) | 0 |
| Kroko EN (zipformer, 2025-08-06) | clean | hot 3.0 | 10.8% | 71.7% | 66.1% (56) | 67.0% (112) | 0 |
| Kroko EN (zipformer, 2025-08-06) | pink10 | beam | 14.5% | 63.3% | 57.1% (56) | 64.3% (112) | 0 |
| Kroko EN (zipformer, 2025-08-06) | pink10 | hot 1.5 | 14.5% | 63.7% | 57.1% (56) | 63.4% (112) | 0 |
| Kroko EN (zipformer, 2025-08-06) | pink10 | hot 3.0 | 14.9% | 63.7% | 57.1% (56) | 62.5% (112) | 0 |
| NeMo FastConformer streaming 80 ms (int8) | clean | beam | 17.3% | 61.7% | 53.6% (56) | 64.3% (112) | 0 |
| NeMo FastConformer streaming 80 ms (int8) | clean | hot 1.5 | 16.1% | 64.0% | 73.2% (56) | 64.3% (112) | 0 |
| NeMo FastConformer streaming 80 ms (int8) | clean | hot 3.0 | 16.5% | 62.7% | 83.9% (56) | 64.3% (112) | 0 |
| NeMo FastConformer streaming 80 ms (int8) | pink10 | beam | 24.2% | 52.7% | 44.6% (56) | 55.4% (112) | 0 |

"Names on screen" are the names in an utterance that the screen shows
(choices such as "play the live one by Queen"); "names not on screen" are
the rest (new searches). Command accuracy here is with beam search, so it
differs a little from §3.1.

- **Biasing works when the recognizer's own tokenizer is known.** The
  Zipformer 70M ships its SentencePiece model: biasing raises the names on
  screen it gets right from 43% to 64% (score 1.5) and 70% (score 3.0) on
  clean speech, and from 34% to 50–55% in noise, without hurting the other
  names or the rest of the sentence.
- **Kroko** ships only its token list, so the kit has to guess how sherpa-onnx
  should cut a name into Kroko's pieces (`vocab_from_tokens`); with that
  guess biasing gains only 2–4 points on clean speech (62.5% to 66%) and
  nothing in noise. Without help Kroko already gets more names on screen
  right (62.5%) than the Zipformer 70M (43%). Its SentencePiece model,
  from its authors, would show whether biasing can do more for it.
- **FastConformer,** with the same kind of guessed pieces, gains the most:
  names on screen from 54% to 73% (score 1.5) and 84% (3.0) on clean
  speech, with the other names unchanged. So the guess can work, and
  Kroko's own tokenizer is worth getting. Nemotron's biasing wasn't run
  here (its beam search is slow and the machine was shared); the kit runs
  it on the build machine.
- Biasing needs modified beam search, which costs more than greedy
  decoding; the cost and the gain are worth re-measuring on the build
  machine. The spoken-choice matcher (sound-alike matching against the
  screen's names) is the other half of getting names right.

### 3.8 Wake phrase

| model | phrase | threshold | FR clean | FR pink 10 dB | FR babble | FR music | FR all | FA/h non-wake speech | FA/h browser speech (echo0) | FA/h page audio | FA/h real speech | delay ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Zipformer KWS 3.3M (GigaSpeech, word pieces) | hey molt | 0.15 | 12.0% | 23.0% | 45.0% | 15.0% | 23.8% | 2.1 (3 in 1.4 h) | 3.5 (1 in 0.3 h) | 0.0 (0 in 0.7 h) | 0.0 (0 in 1.4 h) | 283 |
| Zipformer KWS 3.3M (GigaSpeech, word pieces) | hey molt | 0.25 | 15.0% | 26.0% | 49.0% | 20.0% | 27.5% | 1.4 (2 in 1.4 h) | 3.5 (1 in 0.3 h) | 0.0 (0 in 0.7 h) | 0.0 (0 in 1.4 h) | 286 |
| Zipformer KWS 3.3M (GigaSpeech, word pieces) | hey molt | 0.35 | 30.0% | 41.0% | 62.0% | 33.0% | 41.5% | 0.7 (1 in 1.4 h) | 3.5 (1 in 0.3 h) | 0.0 (0 in 0.7 h) | 0.0 (0 in 1.4 h) | 286 |
| Zipformer KWS 3.3M (GigaSpeech, word pieces) | hello molt | 0.25 | 7.0% | 32.0% | 55.0% | 9.0% | 25.8% | 0.0 (0 in 1.4 h) | 0.0 (0 in 0.3 h) | 0.0 (0 in 0.7 h) | 0.0 (0 in 1.4 h) | 288 |
| Zipformer KWS 3.3M (GigaSpeech, word pieces) | hey molt browser | 0.25 | 27.0% | 45.0% | 51.0% | 35.0% | 39.5% | 0.0 (0 in 1.4 h) | 0.0 (0 in 0.3 h) | 0.0 (0 in 0.7 h) | 0.0 (0 in 1.4 h) | 212 |
| Zipformer KWS 3M (zh+en, 2025-12-20, phones) | hey molt | 0.15 | 25.0% | 59.0% | 76.0% | 33.0% | 48.2% | 0.0 (0 in 1.4 h) | 0.0 (0 in 0.3 h) | 0.0 (0 in 0.7 h) | 0.0 (0 in 1.4 h) | 254 |
| Zipformer KWS 3M (zh+en, 2025-12-20, phones) | hey molt | 0.25 | 25.0% | 61.0% | 76.0% | 36.0% | 49.5% | 0.0 (0 in 1.4 h) | 0.0 (0 in 0.3 h) | 0.0 (0 in 0.7 h) | 0.0 (0 in 1.4 h) | 252 |
| Zipformer KWS 3M (zh+en, 2025-12-20, phones) | hey molt | 0.35 | 35.0% | 70.0% | 76.0% | 47.0% | 57.0% | 0.0 (0 in 1.4 h) | 0.0 (0 in 0.3 h) | 0.0 (0 in 0.7 h) | 0.0 (0 in 1.4 h) | 256 |
| Zipformer KWS 3M (zh+en, 2025-12-20, phones) | hello molt | 0.25 | 30.0% | 81.0% | 72.0% | 42.0% | 56.2% | 0.0 (0 in 1.4 h) | 0.0 (0 in 0.3 h) | 0.0 (0 in 0.7 h) | 0.0 (0 in 1.4 h) | 209 |
| Zipformer KWS 3M (zh+en, 2025-12-20, phones) | hey molt browser | 0.25 | 63.0% | 93.0% | 82.0% | 62.0% | 75.0% | 0.0 (0 in 1.4 h) | 0.0 (0 in 0.3 h) | 0.0 (0 in 0.7 h) | 0.0 (0 in 1.4 h) | 171 |

FR is the share of the 100 wake clips (the phrase, a pause, a command) in
which the phrase was not spotted; FA/h counts detections per hour of audio
without the phrase: the test set's commands and chatter, the browser's own
voice, page audio (documentation and result lists read over music) and the
4,890 real one-word clips of Speech Commands. Delay is from the end of the
phrase to the detection.

- **The GigaSpeech spotter is the one to use;** the newer zh+en model
  misses twice as often. With "hey molt" at threshold 0.15 it misses 12% of
  clean wake clips, 23% in pink noise and 45% under other voices, and it
  fires about twice an hour on speech, never on page audio or real speech
  here. Every false wake-up came from chatter written to be close to the
  phrase: "hey mom, I'm home" and "hey, the mold is back".
- **"Hello molt"**, at the same threshold (0.25), misses fewer clean clips
  than "hey molt" (7% against 15%), about as many overall, and set off no
  false wake-up; "hey molt browser" misses more. The phrase is the owner's
  decision 2: these numbers lean slightly toward "hello molt".
- **The misses are per speaker, not random:** all twelve clean misses of
  "hey molt" are two of the fourteen test voices, which say it the same way
  every time. A real user whose way of saying the phrase isn't spotted
  would be missed over and over. So the threshold must adapt per user, not
  only the phrase (design §5.1 lets the user choose it), and
  always-listening mode (design §5.1) is the fallback for someone the
  spotter keeps missing.
- Open-vocabulary spotting needs no training, but it is far from a trained
  wake word's accuracy; a phrase-specific model, trained on recordings of
  the chosen phrase, is the upgrade path if the misses matter in use.

### 3.9 Voices

| voice | threads | first audio ms: short | one result | list of three | RTF | load ms | peak RSS MB | model MB |
|---|---|---|---|---|---|---|---|---|
| Kokoro 82M v1.0 (int8) | 1 | 2702 | 3918 | 3872 | 1.491 | 1630 | 503.8 | 142.4 |
| Kokoro 82M v1.0 (int8) | 2 | 2228 | 3276 | 3230 | 1.247 | 1677 | 504.1 | 142.4 |
| Kokoro 82M v1.0 (int8) | 4 | 2010 | 3100 | 2832 | 1.105 | 1801 | 504.5 | 142.4 |
| Kokoro 82M v1.0 (fp32) | 1 | 1501 | 2289 | 2300 | 0.882 | 1868 | 657.6 | 353.8 |
| Kokoro 82M v1.0 (fp32) | 2 | 977 | 1390 | 1409 | 0.557 | 1759 | 657.7 | 353.8 |
| Kokoro 82M v1.0 (fp32) | 4 | 698 | 1047 | 1068 | 0.402 | 1775 | 657.9 | 353.8 |
| Piper (VITS) en_US ljspeech medium | 1 | 130 | 53 | 120 | 0.099 | 2021 | 365.2 | 63.5 |
| Piper (VITS) en_US ljspeech medium | 2 | 80 | 37 | 79 | 0.066 | 2033 | 369.4 | 63.5 |
| Piper (VITS) en_US ljspeech medium | 4 | 75 | 37 | 55 | 0.056 | 2079 | 366.9 | 63.5 |
| Piper (VITS) en_US libritts_r medium | 1 | 101 | 48 | 91 | 0.102 | 1535 | 387.5 | 78.5 |
| Piper (VITS) en_US libritts_r medium | 2 | 61 | 43 | 60 | 0.065 | 1522 | 386.5 | 78.5 |
| Piper (VITS) en_US libritts_r medium | 4 | 51 | 29 | 53 | 0.050 | 1398 | 387.0 | 78.5 |
| Piper (VITS) en_US libritts_r medium (int8) | 1 | 197 | 103 | 203 | 0.210 | 2395 | 324.2 | 22.5 |
| Piper (VITS) en_US libritts_r medium (int8) | 2 | 166 | 85 | 185 | 0.197 | 2373 | 320.6 | 22.5 |
| Piper (VITS) en_US libritts_r medium (int8) | 4 | 235 | 98 | 209 | 0.201 | 2447 | 328.4 | 22.5 |
| Piper (VITS) en_GB vctk medium | 1 | 110 | 54 | 101 | 0.099 | 1678 | 352.8 | 77.0 |
| Piper (VITS) en_GB vctk medium | 2 | 64 | 37 | 65 | 0.066 | 1942 | 359.7 | 77.0 |
| Piper (VITS) en_GB vctk medium | 4 | 66 | 34 | 65 | 0.057 | 1967 | 346.3 | 77.0 |
| Piper (VITS) en_US arctic medium | 1 | 121 | 55 | 125 | 0.108 | 1482 | 387.4 | 76.7 |
| Piper (VITS) en_US arctic medium | 2 | 81 | 42 | 73 | 0.072 | 1407 | 388.3 | 76.7 |
| Piper (VITS) en_US arctic medium | 4 | 65 | 34 | 72 | 0.062 | 1446 | 388.9 | 76.7 |
| Matcha (icefall) en_US ljspeech + Vocos | 1 | 143 | 251 | 236 | 0.085 | 2494 | 326.2 | 128.0 |
| Matcha (icefall) en_US ljspeech + Vocos | 2 | 111 | 194 | 182 | 0.068 | 2437 | 326.6 | 128.0 |
| Matcha (icefall) en_US ljspeech + Vocos | 4 | 135 | 209 | 214 | 0.079 | 2485 | 327.2 | 128.0 |

The three texts are "YouTube is open.", one result ("Two: Bohemian
Rhapsody, live at Live Aid, Queen, five minutes.") and a list of three
results with the prompt. First audio is the time from the text to the first
samples; RTF is synthesis time over audio length.

- **Piper (VITS)** starts speaking 30–130 ms after the text and runs at
  5–10% of real time; **Matcha** with Vocos 110–250 ms. The int8 Piper file
  is a third of the size but twice as slow.
- **Kokoro**, the most natural of them, is in another class: 0.7–1.5 s
  before the first sound of "YouTube is open." and 0.4–0.9 of real time in
  fp32 on this fast CPU. Its int8 file is slower still (2–2.7 s, above real
  time), since int8 doesn't pay off here. On the minimum hardware (design
  §11, decision 4) Kokoro would lag behind the conversation.
- Peak memory is the whole process: 320–390 MB for Piper and Matcha,
  660 MB for Kokoro.

Words the voices skip (sherpa-onnx 2.x looks English words up in a
lexicon and silently drops the rest):

| lexicon | text | words | skipped (not in lexicon) | most frequent skipped words |
|---|---|---|---|---|
| lexicon-en-us.txt (Piper/VITS, Matcha) | test-set commands | 1286 | 1.9% | eilish, lofi, pentatonix, moulting, wayne's, netflix, instagram, weeknd |
| lexicon-en-us.txt (Piper/VITS, Matcha) | result titles and channels | 1524 | 8.2% | eilish, lofi, quietcomfort, sennheiser, chillhop, ramsay's, weeknd, coachella |
| lexicon-en-us.txt (Piper/VITS, Matcha) | docs prose (400 sentences) | 5816 | 7.1% | or, cpp, gb, gpu, per, macos, moltnet, lpac |
| lexicon-us-en.txt (Kokoro v1.0) | test-set commands | 1286 | 6.7% | sheeran, what's, wikipedia, videos, ed, eilish, lofi, that's |
| lexicon-us-en.txt (Kokoro v1.0) | result titles and channels | 1524 | 16.0% | adele, eilish, lofi, sony, bose, gordon, anker, quietcomfort |
| lexicon-us-en.txt (Kokoro v1.0) | docs prose (400 sentences) | 5816 | 8.6% | cpp, gb, gpu, cpu, macos, moltnet, lpac, onnx |

sherpa-onnx 2.x no longer bundles espeak-ng: English text goes through a
lexicon lookup only, and a word that isn't in the lexicon is skipped, with
no sound and no error. The VITS and Matcha lexicon lacks "or" (so "say a
number or a name" comes out as "say a number a name") and 8% of the words in
result titles and channels; Kokoro's lacks plurals and inflections
("videos", "what's") and 16% of the title words. The bench adds 22 entries
(`testset/lexicon-extra.txt`) so its own test audio is complete. A shipped
voice needs MoltBrowser's own text normalization (numbers, abbreviations,
addresses) and letter-to-sound rules for unknown words in front of the
engine, or a G2P model.

### 3.10 whisper.cpp, as MoltBrowser ships it

| engine | weights | threads | load ms | encode ms (30 s window) | decode ms/token | 5-beam step ms | ≈ per request ms (load + encode + 12 steps) |
|---|---|---|---|---|---|---|---|
| whisper.cpp tiny.en | test-shapes | 1 | 78 | 1523 | 3.6 | 2.5 | 1632 |
| whisper.cpp tiny.en | test-shapes | 2 | 32 | 901 | 2.3 | 2.0 | 957 |
| whisper.cpp tiny.en | test-shapes | 4 | 36 | 586 | 2.2 | 1.1 | 636 |
| whisper.cpp base.en | test-shapes | 1 | 47 | 3666 | 6.0 | 4.7 | 3769 |
| whisper.cpp base.en | test-shapes | 2 | 44 | 1800 | 3.8 | 2.4 | 1873 |
| whisper.cpp base.en | test-shapes | 4 | 33 | 1073 | 2.7 | 1.7 | 1127 |
| whisper.cpp small.en | test-shapes | 1 | 47 | 10046 | 17.5 | 10.8 | 10223 |
| whisper.cpp small.en | test-shapes | 2 | 39 | 6063 | 10.6 | 7.0 | 6186 |
| whisper.cpp small.en | test-shapes | 4 | 63 | 4096 | 8.6 | 4.2 | 4209 |

whisper.cpp v1.7.5 (the commit `scripts/bundle-whisper.sh` pins), built
here with its CPU back end (AVX-512, AMX, OpenMP), timed with
`whisper-bench`. Its ggml weights are only on Hugging Face, so the timings
use whisper.cpp's own test models: the real tensor shapes in F16 without
trained values, which compute in the same time (the load times are not
representative, and there is no transcript to score). Median of three runs
(one for small.en at four threads).

- **Whisper encodes a 30 s window however short the command,** and
  whisper.cpp's F16 encoder takes 0.6–1.5 s for tiny.en here, 1.1–3.7 s for
  base.en and 4–10 s for small.en. The ONNX int8 exports of the same models
  decode a whole command in 0.17–0.24 s, 0.26–0.42 s and 0.75–1.2 s
  (§3.4): four to eight times faster. Part of that is int8 against F16
  (whisper.cpp also has quantized models, not measured here); its
  `--audio-ctx` option shortens the window for short commands at some cost
  in accuracy (not measured).
- **MoltBrowser's path today,** one `whisper-cli` process per recording,
  adds a process start and a model load to that: about a second per
  command with tiny.en on this fast machine, several on a laptop. Fine for
  occasional dictation, too slow for a conversation (design §4).

### 3.11 Size and memory

| kind | model | files used (MB) | distinct ONNX ops |
|---|---|---|---|
| kws | kws-gigaspeech-3.3m | 6.0 | 41 |
| kws | kws-zh-en-3m | 5.4 | 41 |
| offline | moonshine-base | 286.9 | 40 |
| offline | moonshine-tiny | 124.0 | 40 |
| offline | moonshine2-base | 141.3 | ORT format |
| offline | moonshine2-tiny | 44.2 | ORT format |
| offline | parakeet-tdt-0.6b-v2 | 661.2 | 37 |
| offline | whisper-base.en | 160.6 | 30 |
| offline | whisper-small.en | 375.5 | 30 |
| offline | whisper-tiny.en | 103.6 | 30 |
| offline | whisper-tiny.en-fp32 | 153.0 | 28 |
| stream | fastconformer-80ms | 136.9 | 40 |
| stream | kroko-en | 71.1 | 41 |
| stream | nemotron-0.6b-560ms | 661.9 | 40 |
| stream | zipformer-en-2023-06-26 | 72.5 | 41 |
| stream | zipformer-en-20m | 45.2 | 36 |
| tts | kokoro-v1.0 | 353.8 | 64 |
| tts | kokoro-v1.0-int8 | 142.4 | 67 |
| tts | matcha-ljspeech | 128.0 | 48 |
| tts | piper-arctic-medium | 76.7 | 50 |
| tts | piper-libritts_r-medium | 78.5 | 50 |
| tts | piper-libritts_r-medium-int8 | 22.5 | 52 |
| tts | piper-ljspeech-medium | 63.5 | 51 |
| tts | piper-vctk-medium | 77.0 | 50 |
| vad | silero-vad | 0.6 | 20 |

Distinct ONNX operators used by kws-gigaspeech-3.3m, silero-vad, zipformer-en-2023-06-26, piper-ljspeech-medium together: 59 (Abs, Add, And, Cast, Ceil, Clip, Concat, Constant, ConstantOfShape, Conv, ConvTranspose, CumSum, Div, DynamicQuantizeLinear, Equal, Erf, Exp, Expand, Gather, GatherElements, GatherND, Gemm, GreaterOrEqual, LSTM, LeakyRelu, Less, LessOrEqual, Log, MatMul, MatMulInteger, Max, Mul, Neg, NonZero, Not, Pad, Pow, RandomNormalLike, Range, ReduceMax, ReduceMean, ReduceSum, Relu, Reshape, ScatterND, Shape, Sigmoid, Slice, Softmax, Softplus, Split, Sqrt, Squeeze, Sub, Tanh, Tile, Transpose, Unsqueeze, Where).

Sizes are the files a speech service would load. A minimal English set,
Silero VAD (0.6 MB), the GigaSpeech spotter (6 MB) and Kroko (71 MB), is
under 80 MB. Whisper small.en adds 376 MB and Parakeet 661 MB. A Piper
voice is 64–79 MB (23 MB in int8), Kokoro 142 MB (int8) or 354 MB. Peak
memory of the whole process is in §3.4, §3.5 and §3.9. The runtime
libraries are in §5.3.

## 4. Licences

Every artifact's licence, and where it was read, is in
`tools/voice_bench/artifacts.json` (and in the last table of the kit's
report). What matters for shipping:

| Part | Licence | For MoltBrowser (GPLv3) |
|---|---|---|
| sherpa-onnx v2.0.1 | Apache-2.0 | Compatible. Its build pulls kaldi-native-fbank, kaldi-decoder, OpenFst, simple-sentencepiece (Apache-2.0), utf8proc, nlohmann/json (MIT), Eigen (MPL-2.0) and kissfft (BSD-3-Clause). |
| ONNX Runtime 1.28.2 | MIT | Compatible; its own third-party notices (abseil, protobuf, onnx, flatbuffers, Eigen, cpuinfo and others) ship with it. |
| whisper.cpp v1.7.5 | MIT | Already bundled. |
| Silero VAD v4 | MIT | Ship. |
| KWS GigaSpeech 3.3M | Apache-2.0 | Ship. |
| KWS zh+en 3M | no licence file | A k2-fsa model like the Apache-2.0 ones; confirm with the authors before shipping. |
| Zipformer EN 20M and 70M | Apache-2.0 | Ship. |
| Kroko EN | CC BY-SA 4.0 | Attribution, and changed weights stay CC BY-SA. |
| NeMo FastConformer 80 ms, Parakeet TDT 0.6B v2 | CC BY 4.0 | Attribution in the about page and notices. |
| Nemotron Speech Streaming 0.6B | NVIDIA Open Model License | Allows commercial use, with conditions of its own (attribution among them); read the current terms before shipping. |
| Moonshine (English, v1 and v2) | MIT | Ship. Its other languages are under the non-commercial Moonshine Community License: left out. |
| Whisper weights | MIT | Already bundled (`ggml-tiny.en`). |
| Kokoro-82M v1.0 | Apache-2.0 | Ship. |
| Piper `ljspeech` voice | public-domain data, trained from scratch | Ship. |
| Piper `libritts_r`, `vctk`, `arctic` | not cleared: fine-tuned from the `lessac` voice, whose dataset licence restricts use | Test audio only. |
| Matcha (LJ Speech) | Apache-2.0, public-domain data | Ship; its Vocos vocoder is MIT code, the weights' card couldn't be read here: confirm. |
| sherpa-onnx's English TTS lexicon | not stated; built from CMUdict (BSD-2-Clause) and espeak-ng's word list (GPL-3.0-or-later) | Usable under GPLv3, so fine for MoltBrowser; not for a non-GPL product. |
| Speech Commands, JFK sample | CC BY 4.0, public domain | Test data only, never shipped. |

Left out as not usable commercially: openWakeWord's pre-trained models
(CC BY-NC-SA 4.0), Moonshine's non-English models, Piper's `l2arctic`
voice.

## 5. Integration facts

### 5.1 Loading models from memory or a handle

The speech service gets read-only handles, never paths (CLAUDE.md).

- **sherpa-onnx C API (v2.0.1): models by path only.** Every model field in
  `c-api.h` is a `const char*` path (`SherpaOnnxOnlineTransducerModelConfig`,
  `SherpaOnnxOfflineModelConfig`, `SherpaOnnxKeywordSpotterConfig`,
  `SherpaOnnxVadModelConfig`, `SherpaOnnxOfflineTtsModelConfig`). Only the
  small text files can come from memory: `tokens_buf`, `hotwords_buf`
  (and per-stream hotwords as a string, `SherpaOnnxCreateOnlineStreamWithHotwords`),
  `keywords_buf`. Lexicons, `bpe.vocab`, voices files and provider configs
  are opened by path (`std::ifstream`).
- **Inside, the C++ layer already loads from memory.** The desktop
  constructors call `Ort::Session(env, path, options)`; the Android
  (`AAssetManager*`) and HarmonyOS (`NativeResourceManager*`) constructors
  read each file into a buffer (`ReadFile(mgr, name)`, `file-utils.cc`) and
  create the sessions with `Ort::Session(env, data, length, options)`
  (e.g. `OnlineZipformer2TransducerModel::InitEncoder(void*, size_t)` in
  `online-zipformer2-transducer-model.cc`). A vendored copy needs one more
  "manager" of the same shape that serves bytes from mapped handles, plus
  buffer fields in the C API or a thin C++ entry point. That is a contained
  patch, not a redesign.
- **ONNX Runtime: yes.** `OrtApi::CreateSessionFromArray(env, data, length,
  options, &session)`. For ORT-format models (`.ort`, as Moonshine v2 ships)
  `session.use_ort_model_bytes_directly=1` and
  `session.use_ort_model_bytes_for_initializers=1` make ORT use the caller's
  bytes without copying (they must outlive the session): map the read-only
  handle in the service and hand ORT the mapping. Models with external
  weights (the fp32 NeMo exports' `encoder.data`) can take them from memory
  with `AddExternalInitializersFromFilesInMemory`. Loaded from a path, ORT
  `mmap`s external data itself (`PROT_READ|PROT_WRITE, MAP_PRIVATE`,
  `onnxruntime/core/platform/posix/env.cc`).
- **whisper.cpp (v1.7.5, what MoltBrowser bundles): yes.**
  `whisper_init_from_buffer_with_params(void*, size_t, params)` and
  `whisper_init_with_params(whisper_model_loader*, params)` (read/eof/close
  callbacks, so a file descriptor works) in `include/whisper.h`. Today
  MoltBrowser runs `whisper-cli -m <path>` as a helper process instead.

### 5.2 ONNX Runtime in Chromium 156

- **Not vendored.** `third_party/` has no `onnxruntime` (nor `onnx`), and
  `DEPS` names neither.
- **But Chromium already speaks its C API on Windows.** WebNN's Windows
  backend (`services/webnn/ort/`) loads `onnxruntime.dll` at run time from
  the OS's Windows App Runtime package (Windows ML) on Windows 11 24H2 and
  later (`platform_functions_ort.cc`: `InitializeWinML`,
  `InitializePackageDependency`), against a vendored copy of the C API
  header: `third_party/windows_app_sdk_headers/src/inc/abi/winml/winml/onnxruntime_c_api.h`
  (`ORT_API_VERSION 24`, MIT, from WindowsAppSDK 2.0.1). That runtime is
  the OS's, varies by machine and exists only on recent Windows, so it is
  no substitute for a bundled one, but the C API boundary is familiar code
  in the tree.
- **What Chromium does vendor that ONNX Runtime also uses:** abseil-cpp,
  protobuf, flatbuffers, eigen3, re2, cpuinfo, pthreadpool, XNNPACK,
  nlohmann_json, sentencepiece (and TFLite/LiteRT, a different runtime).
  Versions differ from what ORT pins in `cmake/deps.txt`.

### 5.3 What vendoring sherpa-onnx and ONNX Runtime would cost

**Binary size** (shared libraries of the v2.0.1 release; ORT 1.28.2 with
every operator):

| sherpa-onnx v2.0.1 release | ONNX Runtime (MB) | sherpa-onnx (MB) | total (MB) |
|---|---|---|---|
| Linux x64, shared libraries | 27.0 | 4.9 | 31.9 |
| Linux x64, shared, without TTS | 27.0 | 4.1 | 31.1 |
| Linux arm64, shared libraries | 34.6 | 4.4 | 39.1 |
| macOS arm64, shared libraries | 28.8 | 4.1 | 32.9 |
| Windows x64, DLLs | 17.1 | 3.9 | 21.1 |
| Linux x64, static archives (before linking) | 112.5 | 26.6 | 139.1 |

A reduced build (ORT's `--minimal_build` with `--include_ops_by_config`,
ORT-format models, only the operators the bundled models use) is ONNX
Runtime's documented way to shrink this; it was not built here. The
models measured use 20 to 67 distinct ONNX operators each, and a wake-phrase
spotter, the VAD, a streaming transducer and a VITS voice together about 60
(§3.11), where a full build carries kernels for the whole ONNX operator set
(about 190 operators) and ORT's own contrib operators.

**Build system.** Both are CMake. sherpa-onnx fetches its dependencies at
configure time (kaldi-native-fbank, kaldi-decoder with OpenFst and Eigen,
simple-sentencepiece, utf8proc, nlohmann/json, and a prebuilt ONNX Runtime);
`sherpa-onnx/csrc` has 304 `.cc` files across many model families, of which
a speech service needs a fraction (transducers, the VAD, the keyword spotter,
one or two TTS families). ONNX Runtime 1.28.2 is the big one: 585 non-test
C/C++ source files in `onnxruntime/core` without the other execution
providers (138 of them MLAS, its kernel library), 89 more for the CPU
contrib operators, 143 hand-written assembly files in MLAS (x86-64
AVX2/AVX-512/AMX and arm64 NEON/SVE kernels, `.S` and `.asm`), about 730
headers, plus abseil, protobuf (or protobuf-lite), onnx, flatbuffers, Eigen,
SafeInt, date, mp11, GSL and cpuinfo (KleidiAI on arm64). For comparison,
MoltBrowser's `src/third_party/llama_cpp/BUILD.gn` lists 327 files. Options:
(a) a GN port of ORT's CPU provider (large, but the
standard for Chromium code); (b) build ORT and sherpa-onnx with their own
CMake as a pinned prebuilt step and link the static libraries into the
service (fast, non-hermetic); (c) a minimal ORT build, which also allows
`onnxruntime_DISABLE_EXCEPTIONS` (only supported together with
`onnxruntime_MINIMAL_BUILD`).

**Threading.** Each ORT session owns an intra-op thread pool sized by
`num_threads`, and a sherpa-onnx recognizer has several sessions (encoder,
decoder, joiner), so threads multiply unless ORT's global thread pools are
used. ORT's pool threads spin-wait between runs by default
(`session.intra_op.allow_spinning=1`): for an always-on listener that is
CPU burned for nothing (§3.6). sherpa-onnx forwards ORT settings from a
provider config ("`cpu:<file>`", keys prefixed `SessionConfig.`), but by
path; a vendored build should set them in code. Thread affinity is only
set when asked (`session.intra_op_thread_affinities`).

**Sandbox (Linux `kOnDeviceModelExecution`, as the inference service):**
- `/proc` and `/sys`: ORT calls `cpuinfo_initialize()` when its `Env` is
  created (`onnxruntime/core/platform/posix/env.cc`). pytorch/cpuinfo reads
  `/sys/devices/system/cpu/*` (and `/proc/cpuinfo` on arm64). If the broker
  denies them, ORT prints "cpuinfo_initialize failed. May cause CPU EP
  performance degradation due to undetected CPU features." and carries on;
  on arm64 its CPU feature flags come from cpuinfo, so kernels are lost.
  Create the ORT environment before the sandbox engages (as the inference
  service's pre-sandbox hook warms up its drivers), or allow those paths.
- MLAS (ORT's kernels) otherwise probes with `cpuid` on x86. On CPUs with
  AMX (Sapphire Rapids and later Xeons, like this machine) its platform
  setup calls `arch_prctl(ARCH_REQ_XCOMP_PERM)` (`mlas/lib/platform.cpp`,
  `MlasInitAMX`), the call
  [SANDBOXED_INFERENCE.md](design/SANDBOXED_INFERENCE.md) lists as risky
  under Chrome's seccomp policy for ggml: initialize MLAS before the
  sandbox or build without the AMX kernels.
- `dlopen`: only for execution-provider libraries and custom operator
  libraries; a CPU-only build loads none. The Windows release ships
  `onnxruntime_providers_shared.dll`, which ORT loads only for shared
  providers (CUDA, TensorRT, OpenVINO).
- `mmap`: none when sessions are created from memory; from a path, external
  data is mapped `MAP_PRIVATE` (no `MAP_POPULATE`, which Chrome's seccomp
  kills).
- sherpa-onnx itself makes no `/proc` or `/sys` reads, no `dlopen` and no
  `mmap` in the code a speech service uses (`sherpa-onnx/csrc`, outside the
  NPU back ends).

**Error handling (the service treats its own engine as fallible):**
- **Exceptions.** sherpa-onnx is built on ORT's C++ API, which reports
  errors as `Ort::Exception`; its C API catches exceptions only around the
  `SherpaOnnxCreate*` calls. Like llama.cpp, it has to build with exceptions
  and every call has to be wrapped so nothing escapes into code built with
  `-fno-exceptions`.
- **Fatal errors.** sherpa-onnx ends the process (`SHERPA_ONNX_EXIT`, which
  calls `_Exit`) on many configuration and model-metadata errors, at about
  1,000 places in `sherpa-onnx/csrc`. A bad model file ends the speech
  process; the browser treats that like a crash, as it does for llama.cpp.
- **Hotwords are page text.** Biasing toward the screen sends page titles
  into sherpa-onnx's hotword parser, which gives `:` (a score), `/` (a
  separator), `#` and `@` special meaning, and drops the whole list when one
  word has a character the model can't spell. Pass only letters, digits and
  apostrophes, and only words the model's vocabulary can spell
  (`voice_bench_lib.page_phrases` and `hotwords_arg` do this for the bench).
- **TTS input is page text too,** and in sherpa-onnx 2.x English text goes
  only through a lexicon lookup (§3.9). A browser voice needs its own text
  normalization (numbers, abbreviations) and letter-to-sound for unknown
  words before the engine.

## 6. Recommendation

The owner decides (design §11, decision 1). On these numbers:

1. **Keep sherpa-onnx as the speech engine, as the design's §9
   recommends.** It ran every model that did well here (the streaming and
   per-utterance recognizers, the wake-phrase spotter, the VAD and the
   voices) through the C API a speech service would link. whisper.cpp alone
   can't stream, spot a wake phrase or speak, and the Whisper tiny.en it
   bundles is mid-table on accuracy (§3.1).
2. **Streaming recognition: Kroko EN**, not the LibriSpeech Zipformers the
   design's §9 assumed. It is as accurate on commands as anything measured
   except Whisper small.en, among the most robust to noise, the best with
   the browser's own voice in the background (§3.2), and it costs 4–6% of a
   core (71 MB, CC BY-SA 4.0). If the share-alike licence is a problem,
   NeMo FastConformer 80 ms
   (CC BY 4.0) is the fallback, less accurate at six times the CPU.
   Nemotron 0.6B matches Kroko's accuracy at six times the memory and seven
   to nine times the CPU, and misses isolated words.
3. **End of speech: Silero VAD** with 0.5 s of silence by default, longer
   per user for slow speakers.
4. **Wake phrase: the GigaSpeech spotter** at a low threshold, adapted per
   user, with always-listening mode as the fallback for someone it keeps
   missing (§3.8).
5. **Names: bias toward the screen** with sherpa-onnx's hotwords (§3.7).
   For Kroko, get its SentencePiece model from its authors and measure
   again; the spoken-choice matcher covers names either way.
6. **Per-utterance recognition** as an optional second pass: Whisper
   small.en where the machine is fast enough (0.9 s per command here), for
   dictation and as an "accurate" setting; Kroko alone elsewhere. Keep
   whisper.cpp for long dictation and other languages (design §9), but in
   the resident service (it loads models from a buffer, §5.1), not as one
   `whisper-cli` process per recording; its encoder is several times slower
   than the ONNX Whisper here (§3.10).
7. **Voices:** platform voices first (Phase 1, as planned). For the
   bundled neural voice, a Piper (VITS) voice, which starts speaking in
   under 0.15 s; Kokoro only on fast machines. Either needs MoltBrowser's
   own text normalization and letter-to-sound in front of sherpa-onnx 2.x
   (§3.9). Of the English Piper voices measured, only `ljspeech` is cleared
   for shipping.
8. **Whatever the engine:** echo cancellation with the browser's output as
   the reference, and barge-in (§3.2); one resident speech process, not one
   per utterance (§3.4); ONNX Runtime's spinning off (§3.6); output caps on
   autoregressive decoders (§3.2); hotwords and spoken text sanitized
   (§5.3).
9. **Cost:** about 32 MB of libraries on Linux x64 (21 MB on Windows,
   33 MB on macOS arm64), plus about 80 MB of recognition models (Kroko,
   the spotter, the VAD) and 64 MB per Piper voice. The real work is
   vendoring ONNX Runtime (§5.3) and a small sherpa-onnx patch to load
   models from memory (§5.1).

## 7. Reproducing

The kit's [README](../tools/voice_bench/README.md) has the commands for
Linux, macOS (arm64) and Windows; only Linux was run here. Here the run took
about ten hours on the shared VM (the always-listening runs are real
time, and the slow models' accuracy runs take most of the rest), with 5 GB
of downloads and 0.7 GB of generated audio.

On the build machine, before the choice is final:
1. Run the kit on an x86-64 and an arm64 machine at the minimum hardware
   (design §11, decision 4). The timings here come from a fast server core with
   AMX; expect several times slower on a laptop.
2. Run the `whisper-cpp` step with whisper.cpp's real ggml models
   (`--ggml-dir`), which this machine couldn't download, for whisper.cpp's
   own accuracy next to the ONNX Whisper rows.
3. From Phase 1, real microphones and real users (design §12): synthetic voices
   flatter recognizers, and the users this is for may speak slowly, softly
   or with atypical speech.
