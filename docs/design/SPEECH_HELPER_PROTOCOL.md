# The speech helper protocol (`molt-speech`, version 1)

Status: **version 1**, October 2026 (touchless browsing, round 38). The
browser side (the voice session) and the helper both implement exactly
this page. A change to it bumps the version in `ready` (§3.1) and is
written here first.

`molt-speech` is touchless browsing's ears
([TOUCHLESS_BROWSING.md](TOUCHLESS_BROWSING.md) §5.1, §6.1): a small
resident helper process that listens for the wake phrase, decides when the
user has stopped speaking and recognizes what they said, all on the CPU and
with nothing from the network. The browser starts it once per voice session,
streams the microphone to it (already echo-cancelled) and reads events back.
It is built from pinned sources by `scripts/bundle-speech.sh`, outside GN,
like `whisper-cli` (§7), and runs in MoltBrowser's helper sandbox (§6).

Inside: sherpa-onnx 2.0.1 over ONNX Runtime 1.28.2, the choices of
[VOICE_BENCH.md](../VOICE_BENCH.md) §6:

| Job | Model | Licence |
|---|---|---|
| Wake phrase | sherpa-onnx's open-vocabulary keyword spotter, the GigaSpeech Zipformer KWS 3.3M (2024-01-01), threshold 0.15 | Apache-2.0 (its model card) |
| End of speech | Silero VAD v4 | MIT |
| Recognition | Kroko EN streaming Zipformer (2025-08-06), modified beam search, hotword score 1.5 | CC BY-SA 4.0 (attribution ships with it, §7.3) |

## 1. Starting it

```
molt-speech --models <dir> [--asr-threads 1|2]
```

- `--models` (required): the bundled, read-only model directory (§7.2). The
  helper reads nothing else of its own.
- `--asr-threads` (default 1): ONNX Runtime threads for the recognizer. The
  spotter and the VAD always use one.
- Anything else on the command line is an `args` error (§3.7).

The helper checks that every model file is there, loads the three models
(3.6 s on the 4-core server it was measured on, §8.5; longer on a slow
laptop), then writes `ready`. It reads frames from stdin and writes events
to stdout. stderr carries free-form diagnostics from the helper and its
libraries, for logs only: the browser never parses it and caps what it
keeps.

Audio sent before `ready` waits in the pipe and is processed after the
models are loaded. Until the first CONFIG the helper uses the defaults of
§2.2.

## 2. Browser to helper: frames on stdin

Every frame is an 8-byte header and a payload:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `type`, unsigned, little-endian |
| 4 | 4 | `length` of the payload in bytes, unsigned, little-endian, at most 65,536 |
| 8 | `length` | payload |

| type | name | payload |
|---|---|---|
| 1 | AUDIO | 16 kHz mono signed 16-bit little-endian PCM, already echo-cancelled; an even number of bytes, 2 to 65,536 (at most 2.048 s) |
| 2 | CONFIG | a UTF-8 JSON object (§2.2), 2 to 65,536 bytes |
| 3 | RESET | empty |
| 4 | SHUTDOWN | empty |

Anything else is a protocol error: an unknown type, a length over 65,536, an
AUDIO frame of zero or an odd number of bytes, a RESET or SHUTDOWN with a
payload. The helper checks each header as soon as its 8 bytes are in, so it
refuses a bad frame without waiting for (or buffering) its payload; it
writes an `error` line with the code `protocol` and exits (§3.7).

Frames may be split across reads and pipe writes any way at all; the helper
reassembles them. Send audio as it is captured, 10 to 100 ms per frame
(320 to 3,200 bytes); larger frames only add latency.

### 2.1 AUDIO

The helper processes audio as it arrives, in steps of at most 100 ms, and
never drops it. It doesn't need real-time pacing: faster than real time
works (the tests feed recordings that way, §8). If it falls behind, the
pipe fills and the browser's write waits: the browser writes from a
sequence that may block, and stops the helper if it falls more than a few
seconds behind.

### 2.2 CONFIG

A JSON object. Every field is optional; a field left out keeps its current
value; a field this version doesn't know is skipped (so a newer browser can
add some). The defaults, before the first CONFIG:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `wake_phrases` | array of 1–8 strings | `["hey molt"]` | The phrases that wake the helper. Each: 1–64 bytes of ASCII letters, apostrophes and spaces, 1 to 4 words, each word with a letter and spellable by the spotter's vocabulary (it has no digits). Case and extra spaces don't matter; duplicates count once. |
| `wake_required` | bool | `true` | `true`: an utterance counts only after a wake phrase (§4.1). `false`: every utterance counts (§4.2), for the follow-up window and always-listening mode. |
| `hotwords` | array of 0–1,000 strings | `[]` | Names to bias recognition toward: the page's titles, channels and buttons, the user's own words. The browser sends them already reduced to letters, digits, spaces and apostrophes. |
| `vad_silence_ms` | integer, 100–3,000 | `500` | The silence that ends an utterance. Longer for slow speakers. |

A known field of the wrong type, a value out of range, or a wake phrase
that breaks the rules above is a `config` error. Anything that isn't
strict JSON (RFC 8259: no comments, trailing commas or single quotes; UTF-8
well-formed, escapes and surrogate pairs valid), nesting deeper than 8, a
string over 1,024 bytes (after unescaping), or a field given twice is a
`protocol` error. Either way the helper exits (§3.7).

**Hotwords are cleaned again in the helper**, because they come from page
text: any character other than an ASCII letter, digit or apostrophe
separates words; a word needs a letter or digit, and every character of it
must be in the recognizer's vocabulary; at most 6 words and 64 bytes a
phrase, 200 phrases and 4,096 bytes in all, in the order given, without
duplicates. Nothing that reaches sherpa-onnx's hotword parser can be read
as its syntax (`:` scores, `/` separators, `#`, `@`), and no word can make
it drop the list or fall back to `<unk>`. Case is kept: Kroko's vocabulary
is cased, as the screen is.

**When a CONFIG applies.** A CONFIG never cuts an utterance short:
- `wake_phrases` and `wake_required` apply at once while the helper waits
  for a wake phrase or for speech. An utterance already being recognized
  finishes first, and a `wake` already heard is still answered by its
  `final`. New wake phrases rebuild the spotter, reloading its model
  (0.8 s on the server of §8, during which the helper reads no input); a
  CONFIG whose wake phrases are the ones in use, as every CONFIG the
  browser sends carries them, keeps it (0.1 ms).
- `hotwords` apply from the next utterance; `vad_silence_ms` from the next
  time the helper is waiting (it rebuilds the voice activity detector).

### 2.3 RESET

Drops the utterance in progress, if any: no `speech_end` or `final` is
written for it, nor a `final` for a `wake` not yet answered. The helper then
waits as CONFIG says (for the wake phrase, or for speech). The voice
activity detector and the spotter start afresh, so if the user is still
speaking, a new utterance may start in the middle of their sentence
(`wake_required` false).

Events written before the helper read the RESET may still be in the pipe.
Every event but `ready` and `error` carries `epoch`, the number of RESET
frames the helper had processed when it wrote it (§3): after sending its
n-th RESET, the browser ignores events whose `epoch` is below n.

### 2.4 SHUTDOWN, end of input

SHUTDOWN, or the end of stdin (the browser closed the pipe or went away),
stops the helper at once: it writes what it already queued (for at most
0.5 s), and exits with status 0. An utterance in progress is dropped. End
of input in the middle of a frame is the same.

## 3. Helper to browser: events on stdout

One JSON object per line (`\n`), at most 4,096 bytes with the newline. Text
fields are well-formed UTF-8 of at most 1,000 bytes, with no control
characters (white space is one space). Every object has `"type"`; every
event but `ready` and `error` also has `"epoch"` (§2.3) and `"audio_ms"`:
how much audio the helper had received when it wrote the event (its own
clock: 0 at the first AUDIO sample, in milliseconds of audio).

**The browser treats every line as untrusted.** A hijacked helper can
write anything: the browser checks the line length, that it parses as one
JSON object, the types and ranges of the fields it uses, the UTF-8 and the
length of the text, and that the event fits the sequence of §4, and stops
the helper on any violation. Recognized text is what the user said as far
as the browser can tell, not an instruction from the helper: it goes to the
voice loop's grammar like typed text, with the same checks.

### 3.1 `ready`

```json
{"type":"ready","protocol":1,"engine":"sherpa-onnx 2.0.1, onnxruntime 1.28.2","models":["kws-zipformer-gigaspeech-3.3m-2024-01-01","silero-vad-v4","kroko-en-2025-08-06"],"sample_rate":16000}
```

Written once, after the models loaded. `protocol` is this page's version;
the browser stops a helper whose version it doesn't speak.

### 3.2 `wake`

```json
{"type":"wake","epoch":0,"audio_ms":1940}
```

A wake phrase was heard (`wake_required` true). Exactly one `final` answers
it (§4.1).

### 3.3 `speech_start`

```json
{"type":"speech_start","epoch":0,"audio_ms":1940}
```

The user is saying something that counts (§4).

### 3.4 `partial`

```json
{"type":"partial","epoch":0,"audio_ms":3200,"text":"Hey Molt, open"}
```

The recognizer's current guess for the whole utterance so far (not just
the new words), written when it changes. For live captions only: it can
change completely until the `final`.

### 3.5 `speech_end`

```json
{"type":"speech_end","epoch":0,"audio_ms":3780}
```

The user stopped speaking: `vad_silence_ms` of silence, or the utterance
reached 20 s.

### 3.6 `final`

```json
{"type":"final","epoch":0,"audio_ms":3780,"text":"Hey Molt, open YouTube.","confidence":0.93}
```

What was said. `confidence` (0 to 1, two decimals) is the geometric mean of
the recognizer's probabilities for the words it chose: a hint, not a
calibrated probability. The text is the recognizer's own output: Kroko
writes case and punctuation, and after a wake phrase said in the same
breath, the phrase itself ("Hey Molt, open YouTube."), which the voice
grammar drops as it drops it from typed commands. It may be empty, with
confidence 0, when nothing was recognized, including after a `wake` with no
command (§4.1).

### 3.7 `error`

```json
{"type":"error","code":"protocol","message":"frame length 70000 is over 65536"}
```

Then the helper exits with status 2. Codes:

| code | Meaning |
|---|---|
| `args` | a bad command line |
| `model` | a model file is missing, unreadable or didn't load |
| `protocol` | a malformed frame, or a CONFIG that isn't strict, bounded JSON (§2) |
| `config` | a CONFIG field of the wrong type or out of range, or a wake phrase the spotter can't spell (§2.2) |
| `engine` | the speech engine failed while running |
| `internal` | anything else |

The message is printable ASCII, at most 200 bytes, for logs. It never
echoes the browser's input (it may give a number: a length, a byte offset).

Nothing follows an `error`. The browser logs the first, ends the helper
at once (as a failure: it is restarted, with the backoff of §3.8) and
treats any line after one as a protocol error.

### 3.8 Exit status

| Status | When |
|---|---|
| 0 | SHUTDOWN, or the end of stdin |
| 2 | after an `error` line |
| 3 | stdout broke, a line couldn't be written for 2 s (the browser stopped reading), or more than 1 MiB of lines waited; nothing more is written |
| anything else, or a signal | a crash. sherpa-onnx ends the process with status 255 (`_Exit(-1)`) on some model errors it finds while loading. |

The browser treats every exit but the one its SHUTDOWN asked for as the
helper failing, and restarts it at most a few times before giving up.

## 4. The sequence of events

`ready` comes first, once. After it, each `speech_start` is followed by
`speech_end` and then `final` before anything else starts, and each `wake`
by exactly one `final`; a RESET cancels both (§2.3). `epoch` and
`audio_ms` never go back.

### 4.1 With a wake phrase (`wake_required` true)

```
wake  [speech_start  partial*  speech_end]  final
```

- The spotter and the voice activity detector run; the recognizer waits.
- On a wake phrase: `wake`. The recognizer starts on the audio from just
  before the phrase (at most 2 s back, and not before the speech it is
  in), so "Hey Molt, open YouTube" in one breath works. The text then
  starts with the wake phrase, as recognized; the grammar drops it.
- `speech_start` comes as soon as a word said after the phrase is
  recognized: one that starts later than 0.3 s before the spotter fired (it
  fires 0.2–0.3 s after the phrase ends, [VOICE_BENCH.md](../VOICE_BENCH.md)
  §3.8). The command then ends like any utterance: `speech_end`, then
  `final`.
- If the speech that held the wake phrase ends first, the helper finishes
  recognizing it. Words after the phrase by that rule, or more words than
  the phrase has (a short command in the same breath, when the spotter
  fired late), make it the command: `speech_start`, `speech_end` and
  `final` at once. Otherwise it was the wake phrase alone ("Hey Molt."
  pause "Open YouTube."), and the helper waits for the command: its speech
  starts a new utterance, with `speech_start` when the detector hears it.
- If no command starts within 6 s of the `wake`: `final` with empty text and
  confidence 0, without `speech_start` or `speech_end`, and back to waiting.
- After the `final`, the helper waits for the wake phrase again. A wake
  phrase said during a command is just part of it.

### 4.2 Without (`wake_required` false)

```
speech_start  partial*  speech_end  final
```

for each utterance the voice activity detector finds, with `speech_start`
when it notices the speech (about 0.1 s in). The recognizer runs only
while someone speaks, from 0.6 s before the detector noticed (so the first
word isn't lost) but never before the end of the previous utterance. A
wake phrase said here is just part of the utterance.

### 4.3 Limits

- An utterance ends after 20 s (from its first sample, the pre-roll
  included) even if the user goes on: `speech_end`, `final`. What follows
  is a new utterance, which needs a wake phrase again if `wake_required`
  is true. The voice activity detector itself starts afresh after 20 s
  of speech without a pause, so endless sound (a television, music with
  vocals) can't grow its buffers, and also after each stretch of speech
  and after 10 s without one, so its timing doesn't drift over a long
  session (§8.5).
- `final` and `partial` text is at most 1,000 bytes, cut at a word near the
  end, else at a character. A transducer emits at most one token per
  frame and loops never; 20 s give at most a few hundred tokens.
- The helper keeps 2.5 s of audio of its own (for the pre-rolls), and
  sherpa-onnx's buffers for the utterance in progress. Peak memory is in
  §8.

## 5. Example

A browser session, as hex and as lines (the audio frames are abridged):

```
02 00 00 00 5b 00 00 00  {"wake_phrases":["hey molt"],"wake_required":true,"hotwords":["Bohemian Rhapsody","Queen"]}
01 00 00 00 80 0c 00 00  <100 ms of PCM: 3,200 bytes>
01 00 00 00 80 0c 00 00  ...
04 00 00 00 00 00 00 00  SHUTDOWN
```

```
{"type":"ready","protocol":1,"engine":"sherpa-onnx 2.0.1, onnxruntime 1.28.2","models":["kws-zipformer-gigaspeech-3.3m-2024-01-01","silero-vad-v4","kroko-en-2025-08-06"],"sample_rate":16000}
{"type":"wake","epoch":0,"audio_ms":1940}
{"type":"speech_start","epoch":0,"audio_ms":2600}
{"type":"partial","epoch":0,"audio_ms":2600,"text":"Hey Molt, play"}
{"type":"partial","epoch":0,"audio_ms":3900,"text":"Hey Molt, play Bohemian Rhapsody"}
{"type":"speech_end","epoch":0,"audio_ms":4500}
{"type":"final","epoch":0,"audio_ms":4500,"text":"Hey Molt, play Bohemian Rhapsody.","confidence":0.91}
```

## 6. What the helper needs from the sandbox

The helper is one process. It reads its model directory and what the C
runtime and ONNX Runtime's CPU detection read at start, writes only to
stdout and stderr, opens no socket, creates no file and starts no
process. The voice session starts it through MoltBrowser's helper sandbox
(`molt_ai/common/helper_sandbox.h`) with the confinement the one-shot
helpers (tesseract, whisper-cli) get, launched differently: stdin and
stdout stay open for the whole session, stderr goes to the log apart from
the events, there is no deadline (the browser ends it with SHUTDOWN, and
kills it if it stops answering), and there is no scratch directory, as it
writes nothing. Its memory peaks under 200 MB (§8), far below the sandbox's
4 GiB. The lists below are what that confinement must allow.

### 6.1 Linux (measured)

`strace -f` over a whole session (start, CONFIG, a wake phrase and its
command, CONFIG, an utterance with every utterance counting, RESET in an
utterance, SHUTDOWN), with one and two recognizer threads. The same session
and all 21 protocol checks of §8 also ran under MoltBrowser's current Linux
helper sandbox (`helper_sandbox_linux.cc`: Landlock ABI 7, the seccomp
filter and the resource limits, through a test wrapper; its own folder
readable and executable, an empty scratch directory), with the same events.

**System calls** (27 distinct): `read`, `write` (stdout and stderr only),
`openat` (read-only), `close`, `fstat`, `newfstatat`, `access`,
`getdents64`, `pread64` (the dynamic loader), `fcntl`, `brk`, `mmap`
(private only: anonymous memory, thread stacks and the libraries; no
`MAP_SHARED`, and the models are read, not mapped), `mprotect`, `munmap`,
`futex`, `clock_nanosleep` (the stdout watchdog), `clone3` for threads
(the seccomp filter answers `ENOSYS` and glibc falls back to `clone()` with
thread flags, which the filter allows), `set_robust_list`, `rseq`,
`set_tid_address`, `rt_sigaction`, `rt_sigprocmask`, `prlimit64`,
`getrandom`, `arch_prctl` (`ARCH_SET_FS` at start; on CPUs with AMX,
`ARCH_REQ_XCOMP_PERM` and `ARCH_GET_XCOMP_PERM`, which ONNX Runtime's MLAS
asks for once: refused, it uses its AVX-512 or AVX2 kernels instead),
`execve` (its own start) and `exit_group`. None of `socket`, `connect`,
`bind`, `fork`, `vfork`, `ioctl`, `prctl`, `sched_setaffinity`,
`memfd_create`, SysV IPC, `io_uring_*`, `ptrace`, `unlink`, `rename` or
`mkdir`.

**Threads:** 7 (10 with `--asr-threads 2`): its own three (main, stdout
writer, watchdog) and ONNX Runtime's, blocked between runs (the build has
no spin-waiting).

**Paths:**

| Path | Access | Who | In the sandbox |
|---|---|---|---|
| `<models>/kws/*`, `<models>/vad/*`, `<models>/asr/*` (11 files) | read (opened read-only and read into memory, at start and when the wake phrases change) | the helper and sherpa-onnx | needed: the helper's folder (read) |
| the helper itself | execute | | needed |
| `/etc/ld.so.cache`, `/etc/ld.so.preload` (absent), `/lib/x86_64-linux-gnu/{libc,libm,libmvec}.so.*` | read, map | the dynamic loader | needed (`/etc`, `/lib*`, `/usr`) |
| `/sys/devices/system/cpu/{kernel_max,online,possible,present}` | read | cpuinfo (ONNX Runtime) | needed (allowed today) |
| `/proc/cpuinfo` | read | cpuinfo | allow: refused, it prints "Error in cpuinfo: failed to parse processor information from /proc/cpuinfo" to stderr and carries on; on x86 MLAS detects the CPU with `cpuid` and nothing changes (measured), but on arm64 ONNX Runtime takes the CPU's features (dot product, i8mm) from it, so its fast kernels would be lost |
| `/sys/bus/pci/devices` (listed) and each device's `class`, `/sys/class/drm`, `/sys/class/accel` | read | ONNX Runtime's device discovery (GPUs and NPUs for its execution-provider API) | deny: the CPU path doesn't use it, and refused it is skipped without a fatal error (measured under Landlock) |
| stdout, stderr | write | | the pipes |

Nothing else: no `/home`, `/tmp`, `/dev` (not even `/dev/urandom`:
`getrandom` instead), no other `/proc` entry.

### 6.2 macOS (from the code; not run there yet)

The helper Seatbelt profile (`seatbelt_profile.cc`) already allows what the
helper needs, with no scratch directory:

- **Read:** the helper's own folder (`HELPER_DIR`, `.../Resources/speech/`,
  which holds `models/`), and dyld's system libraries, frameworks and
  shared cache (`/usr/lib`, `/System/Library`, the cryptexes): libc++,
  libSystem and Foundation, which sherpa-onnx links.
- **`sysctl-read`:** ONNX Runtime's CPU detection (cpuinfo and MLAS read
  `hw.ncpu`, `hw.perflevel*`, `hw.cachelinesize`, `hw.optional.arm.*`
  for the dot-product and i8mm kernels).
- **Nothing else:** no file writes (it needs no scratch directory), no
  `process-fork` or other `process-exec`, no network, no Mach services it
  asks for (check on the build machine that Foundation's start doesn't
  look one up under the profile; if it does, the denial is logged and
  harmless unless it ends the process).
- Threads: three of its own plus ONNX Runtime's pool (`--asr-threads 2`).

### 6.3 Windows (from the code; deferred)

As the OCR and voice helpers' AppContainer (`helper_sandbox_win_policy.h`),
without a scratch directory:

- **Read:** the helper's folder through the installer's capability grant
  (`speech\` needs the same grant as `ocr\` and `whisper\`), Windows' system
  DLLs. The static build loads no ONNX Runtime provider DLL.
- **CPU detection:** `GetLogicalProcessorInformationEx` and
  `IsProcessorFeaturePresent`, which an LPAC process may call.
- **The job:** one process, memory well under its limit (§8), no child
  processes, no network capability.
- Model paths are passed as narrow strings: an install path outside the
  ANSI code page (a per-user install under a non-ASCII user name) would
  need wide-character paths in the helper and in sherpa-onnx.

## 7. Building and shipping

### 7.1 The build

`scripts/bundle-speech.sh` builds everything from source tarballs pinned by
SHA-256 (and their commits), with each project's own CMake, outside GN:

1. **ONNX Runtime 1.28.2**, CPU only, static. Its dependencies come from
   the same kind of pinned tarballs: the script rewrites `cmake/deps.txt` to
   point at the local copies, and the configure fails if anything would
   still be fetched from the network. `onnxruntime_CLIENT_PACKAGE_BUILD`
   makes its thread pools stop spin-waiting between runs by default (the
   bench measured that spinning alone costs a third of a core,
   [VOICE_BENCH.md](../VOICE_BENCH.md) §3.6), so no session option is
   needed; the traditional-ML, generation, float8 and float4 operators are
   left out. The static libraries are combined into one `libonnxruntime.a`.
2. **sherpa-onnx 2.0.1**, static, with its C API and without TTS, speaker
   diarization, Python, its programs or other execution providers, against
   that ONNX Runtime. Its own dependencies (kaldi-native-fbank,
   kaldi-decoder, kaldifst, OpenFst, Eigen, simple-sentencepiece, utf8proc,
   nlohmann/json, kissfft) are pinned the same way.
3. **molt-speech** itself (`CMakeLists.txt` in
   `chrome/browser/molt_ai/voice/speech_helper/`): the std-only core GN also
   builds, plus `standalone/` (the engines over sherpa-onnx's C API, stdout
   and `main`), linked statically against both: one executable per OS.
   Linux: glibc from the system, the C++ runtime linked in, `-z relro -z
   now`, stack protector, fortify; macOS: the system's libc++; Windows: the
   static CRT.

The pins were set where github.com's `/archive/` downloads were blocked:
each tarball was reproduced from its pinned commit with
`git archive | gzip -n`, which gives GitHub's bytes (it matched every
upstream pin there is: sherpa-onnx's own pins of its dependencies, the
GitLab Eigen tarballs and ONNX Runtime's pin of KleidiAI's). The script
does the same when a download fails (`--fetch git` forces it), and either
way refuses anything that doesn't match. The build machine confirms the
pins with real downloads once (`docs/BUILD_MACHINE_CHECKLIST.md` §2.7).

The helper's own SHA-256 is printed and recorded in `speech/.build-info`;
it varies with the compiler, so it is recorded, not pinned.

### 7.2 Where it goes

| OS | Folder | Bundled by |
|---|---|---|
| macOS | `MoltBrowser.app/Contents/Resources/speech/` | `scripts/build.sh` |
| Linux | `/opt/moltbrowser/speech/` (next to the executable) | `scripts/package-linux.sh` |
| Windows | `<install dir>\speech\` (next to `MoltBrowser.exe`) | (the Windows workflow, paused) |

Bundling is opt-in until voice listening ships: `MOLT_BUNDLE_SPEECH=1`.

```
speech/molt-speech[.exe]
speech/models/kws/{encoder.int8.onnx, decoder.onnx, joiner.int8.onnx, tokens.txt, pieces.vocab, README.md}
speech/models/vad/silero_vad.onnx
speech/models/asr/{encoder.onnx, decoder.onnx, joiner.onnx, tokens.txt, bpe.vocab, README.md}
speech/LICENSES/...
speech/.build-info
```

The browser finds the helper with `molt_ai::FindHelperBinary()` in that
folder only (no system copy: no distribution packages it) and passes
`--models <folder>/models`. Every model file is pinned
(`MODEL_FILES` in the script; `bundle-speech.sh --verify` checks an
installed copy, and `release.sh` runs it on the Mac when the helper was
bundled). The two `.vocab` files are generated at bundle time from the
pinned `bpe.model` and `tokens.txt` by `make_speech_vocab.py` (word pieces
with scores, for cutting wake phrases and hotwords into the models' pieces)
and are pinned too.

On macOS the helper is signed like `tor`, `tesseract` and `whisper-cli`:
explicitly, with the hardened runtime, before the app is sealed
(`codesign --deep` skips `Contents/Resources/`). It needs no entitlement.

### 7.3 Licences

`speech/LICENSES/` ships `README.md` (from `LICENSES.md` in the helper's
folder: every library and model, with the attribution below), ONNX
Runtime's licence and third-party notices, sherpa-onnx's licence and those
of its dependencies.

- **Kroko EN is CC BY-SA 4.0.** Its attribution ("Kroko ASR" English
  community model by Banafo, https://kroko.ai, licence and source links)
  ships in `LICENSES/README.md`, with the model's own `README.md`; the
  about page's credits should carry the same line when voice listening
  ships. The model files are unmodified; the generated `bpe.vocab` is
  derived from its token list and shared under the same licence.
- **The GigaSpeech keyword spotter:** its model card (the `README.md` in
  the archive, shipped) says Apache-2.0. The terms of GigaSpeech, the data
  it was trained on, sit behind a request form and couldn't be read here.
  If the owner wants that confirmed first, the alternative is to spot the
  wake phrase in Kroko's own streaming transcript (no second model; Kroko
  listens all the time for about 6% of a core, [VOICE_BENCH.md](../VOICE_BENCH.md)
  §3.6).
- Silero VAD is MIT (its licence text is in `LICENSES/README.md`);
  sherpa-onnx Apache-2.0; ONNX Runtime MIT.

### 7.4 A smaller ONNX Runtime

ONNX Runtime is most of the helper. A reduced-operator build
(`--include_ops_by_config` with the 45 operators the three models use, and
the contrib operators its graph fusions turn them into) would drop the
kernels of every other operator: about 7.6 MB of the 14 MB of kernel code
in this build (upsample, scatter, top-k, einsum, attention, pooling and
the rest). A minimal build (`--minimal_build`, ORT-format models) would
also drop the ONNX parser and the graph optimizers (which then run when
the models are converted), and allows `onnxruntime_DISABLE_EXCEPTIONS`.
Both need ONNX Runtime's Python tools to make the operator list and the
`.ort` files, which then have to be pinned too, and a check that the
fusions' kernels are in the list. Not done this round: the full build
works, and the saving is a few MB per platform.

## 8. Measured

October 2026, Linux x64, on the shared 4-vCPU server the voice bench used
(Xeon Sapphire Rapids under KVM, AVX-512 and AMX;
[VOICE_BENCH.md](../VOICE_BENCH.md) §2.4), with the bench's synthetic test
audio (`tools/voice_bench/gen_audio.py`, generated again here).
`tools/voice_bench/speech_helper_check.py` drives the built helper over its
stdin and stdout as the browser will; the build machine runs it again on
the minimum hardware (`docs/BUILD_MACHINE_CHECKLIST.md` §3.4). The timings
ran with no other heavy job on the machine; expect a laptop to be slower.

### 8.1 The build

| | |
|---|---|
| `molt-speech` | 27.3 MB, stripped; links only glibc (`libc`, `libm`, `libmvec`), the C++ runtime is inside |
| Models | 77.8 MB: the spotter 6.0 MB (int8), Silero VAD 0.6 MB, Kroko EN 71.1 MB |
| Build | ONNX Runtime 40 minutes, sherpa-onnx 5 minutes, the helper under a minute (`--jobs 2`) |
| Every source and model | matched its pinned SHA-256; nothing fetched by CMake (checked from its log) |

### 8.2 The protocol (§2, §3)

21 checks, all as specified, also with the helper inside MoltBrowser's
Linux helper sandbox (§6.1):

| Check | The helper |
|---|---|
| Frame type 9; a length of 70,000 and of 4 GiB (refused from the header alone); an AUDIO frame of 3 bytes and of none; RESET with a payload; a good frame then type 7 | `error` `protocol`, exit 2, within 16 ms |
| CONFIG that isn't JSON, or nests too deeply | `error` `protocol`, exit 2 |
| CONFIG out of range, of the wrong type, with a wake phrase that isn't ASCII or has a digit | `error` `config`, exit 2 |
| 350 hotwords full of the engine's syntax (`AC/DC :9.0`, `#1 @home`, `a/b/c`), a 1,000-letter word, bidirectional controls, `Beyoncé`, `o'clock` | accepted, cleaned; the utterance's final comes as usual |
| CONFIG between two AUDIO frames of an utterance | the utterance goes on to its final (the new values apply from the next one) |
| RESET in an utterance | nothing more from that utterance: no `speech_end`, no final |
| SHUTDOWN; end of input; end of input inside a frame | exit 0 within 70 ms |
| A browser that stops reading stdout (a 4 KiB pipe) | exit 3, 7.6 s after it started: filling the pipe, then the 2 s limit |

### 8.3 Recognition

The 300 clean clips of the bench's set a (spoken commands of five kinds,
dictation and chatter, read by fourteen synthetic voices), fed as fast as
the helper takes them, one after another in one session, every utterance
counting (`wake_required` false). The bench driver is sherpa-onnx's own
decoder on the same models (VOICE_BENCH.md §2).

| | WER | Commands right | Entities right | Names on the screen right (56) | Names not on it (112) |
|---|---|---|---|---|---|
| The helper, no hotwords | 11.8% | 68.3% | 68.5% | 69.6% | 67.9% |
| The helper, the screen's names as hotwords | 11.4% | 69.3% | 70.2% | 75.0% | 67.9% |
| The bench driver, no hotwords | 11.0% | 70.3% | 65.5% | | |

- Every clip gave exactly one final; none gave none or two.
- The driver's transcripts are of the bench's own audio. The Kokoro and
  Matcha clips are the same audio here, and 137 of their 153 transcripts
  are identical, 7 more differ in one word: the helper decodes from 0.6 s
  before the speech its VAD found, the driver the whole clip, so the
  recognizer's 1.28 s chunks fall elsewhere. Piper samples a new take of a
  sentence each time it speaks it, so its 147 clips are other takes here
  (85 identical).
- That is also the noise between two runs: a build before the VAD fix of
  §8.5 gave 11.4% and 68.7% (10.9% and 70.0% with hotwords), with 13 of
  the 300 transcripts different, as many better as worse.
- Hotwords lift the names that are on the screen by 5 points (70% to 75%)
  and leave the others as they were, the bias design §5.1 wants. Most of
  what stays wrong is search terms (42% of searches right, 43% with
  hotwords), which no screen lists.

### 8.4 The wake phrase

"hey molt" at threshold 0.15, said by the bench's fourteen voices in 100
clean clips, each followed by a command, one after another in one session
(7 s apart):

| | |
|---|---|
| Woke | 73 of 100 (missed 27%; 23–27% over five runs) |
| The command after it exactly right | 42 of the 73 (WER 22.8% over them) |
| False wake-ups | none: in 0.04 h of chatter written to sound like it ("hey Walt, are you coming with us", "snakes molt their skin", "hello Mo, how was your day"), 0.29 h of the whole set a and 0.72 h of page audio (the browser's own speech) |

The bench measured 12% missed at 0.15. Here the bench driver itself, with
the same keyword line, misses 21% of these clips, and so does the helper
when each clip gets a fresh helper (the same 21 clips): the spotter is
unchanged. The audio isn't: Piper samples a new take each time, and the 11
clips the driver newly misses and the 2 it now catches are all Piper's, 7
of them its slow LibriTTS voice. In one long session the helper misses 2
to 6 more, different borderline clips in each run, as the spotter's
stream starts at another point of its chunks than the clip does (it
starts again after each utterance). Starting it afresh also after every
stretch of speech that held no wake phrase was tried and changed nothing
(25 missed in two runs), so it isn't left over from earlier speech. On
synthetic voices the miss rate is 12% to 27% depending on the take and the
run; the threshold per user the bench asked for (learned from the wake-ups
the user confirms) is still to come.

### 8.5 Speed and memory

Fed in real time (100 ms frames, as the browser will) with 40 clips of
set a, every utterance counting:

| | 1 recognizer thread | 2 threads |
|---|---|---|
| End of speech to `final` (median, p90) | 0.63 s, 0.73 s | 0.63 s, 0.72 s |
| Start of speech to the first `partial` (median) | 1.14 s | 1.15 s |
| Peak memory | 189 MB | 189 MB |
| Transcripts | WER 10.7%, 30 of 40 commands right; 39 of 40 the same as fed fast | the same |

- **The end of speech is the VAD's 0.5 s of silence and Silero's own
  lag.** Where the VAD ended the speech, in the audio (no machine speed in
  it): 0.57 s after the end of speech (median of the 40 clips, at most
  0.74 s, with no drift over the session); on 14 clips the bench timed
  too, within 10 ms of its number wherever the audio is the same
  (Kokoro's). The rest is the 100 ms frames and the helper's own work:
  `speech_end` and `final` go out 56 ms (median) after the frame that
  ended the speech arrived, the 1.2 s of tail padding decoded included.
  The bench measured 0.60 s to the final text (VOICE_BENCH.md §3.5).
- **The VAD starts afresh after each stretch of speech** (§4.3). Before,
  Silero carried its state over the whole session and held on to the end
  of speech longer and longer: 0.90 s median over the same 40 clips, 0.4 s
  late by the fourteenth utterance.
- **The first words show 1.14 s after the user starts** (bench: 1.0 s):
  Kroko decodes in 1.28 s chunks, and the helper's first chunk starts
  0.6 s before the speech its VAD found, not at the start of the clip. A
  second recognizer thread changes nothing here.
- **Ready** 3.6 s after it starts (median of five; loading the three
  models, most of it Kroko's), holding 154 MB.

Listening in real time; CPU over the whole run (100% is one core):

| Listening | CPU | Peak memory |
|---|---|---|
| For the wake phrase, to 120 s of page audio (the browser's own speech) | 4.2% | 165 MB |
| Every utterance counting, the same audio | 5.8% | 191 MB |
| For the wake phrase, in silence (60 s) | 3.8% | 161 MB |

The bench measured 4.4% for the spotter with the VAD and 5.7% for Kroko
with it (VOICE_BENCH.md §3.6). The spotter hears all audio, silence too,
so waiting costs about the same in a quiet room. Threads: 7 (10 with
`--asr-threads 2`), the helper's three and ONNX Runtime's, blocked between
runs.

### 8.6 Robustness

- **Fuzzer** (`molt_speech_protocol_fuzzer`: whole sessions of frames
  through the frame reader, CONFIG parser and listener with engines that
  follow the audio, checking the event sequence of §4): 280,735 runs in
  301 s under ASan and UBSan, no finding; the 20,000-run smoke in 16 s.
- **Unit tests:** 34, under ASan and UBSan.
- **Bounded:** frames ≤ 64 KiB, CONFIG ≤ 1,000 hotwords (200 phrases
  reach the recognizer), event lines ≤ 4 KiB, 1 MiB of unread output, an
  utterance ≤ 20 s, 2.5 s of audio kept for pre-roll.
