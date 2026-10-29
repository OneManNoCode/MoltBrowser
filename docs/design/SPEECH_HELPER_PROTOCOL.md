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

Inside: sherpa-onnx 2.0.1 over ONNX Runtime 1.28.2 (`docs/VOICE_BENCH.md`
§6), with three models:

| Job | Model | Licence |
|---|---|---|
| Wake phrase | sherpa-onnx's open-vocabulary keyword spotter, the GigaSpeech Zipformer KWS 3.3M (2024-01-01) | Apache-2.0 (its model card) |
| End of speech | Silero VAD v4 | MIT |
| Recognition | Kroko EN streaming Zipformer (2025-08-06) | CC BY-SA 4.0 (attribution ships with it, §7.3) |

## 1. Starting it

```
molt-speech --models <dir>
```

- `--models` (required): the absolute path of the bundled, read-only model
  directory (§7.2). The helper reads nothing else of its own.
- `--asr-threads <1|2>` (optional, default 1): ONNX Runtime threads for the
  recognizer. The spotter and the VAD always use one.
- Anything else on the command line is an `args` error (§3.7).

The helper loads the three models (about 0.5 s here; seconds on a slow
laptop), then writes `ready`. It reads frames from stdin and writes events to
stdout. stderr carries free-form diagnostics from the helper and its
libraries, for logs only: the browser never parses it and caps what it keeps.

Audio sent before `ready` waits in the pipe; it is processed after the
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
AUDIO frame of zero or an odd number of bytes, a non-empty RESET or SHUTDOWN,
a CONFIG that isn't valid JSON or breaks §2.2. The helper writes an `error`
line with the code `protocol` (or `config`) and exits (§3.7).

Frames may be split across reads and pipe writes any way at all; the helper
reassembles them. Send audio as it is captured, 10 to 100 ms per frame
(320 to 3,200 bytes); larger frames only add latency.

### 2.1 AUDIO

The helper processes audio as it arrives and never drops it. It doesn't
need real-time pacing: faster than real time works (the tests feed
recordings that way). It needs about 4–6% of one core of a 2021-era desktop
core while waiting for the wake phrase (§5). If it falls behind, the pipe
fills and the browser's write waits; the browser must write from a sequence
that may block, and may stop the helper if it falls more than a few seconds
behind.

### 2.2 CONFIG

A JSON object. Every field is optional; a field left out keeps its current
value; fields this version doesn't know are ignored (so a newer browser can
add some). Defaults before the first CONFIG are shown.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `wake_phrases` | array of 1–8 strings | `["hey molt"]` | The phrases that wake the helper. Each: 1–64 bytes of ASCII letters, apostrophes and single spaces, 1 to 4 words, every word spellable by the spotter's vocabulary (it has no digits). Case doesn't matter. |
| `wake_required` | bool | `true` | `true`: an utterance counts only after a wake phrase (§4.1). `false`: every utterance counts (§4.2), for the follow-up window and always-listening mode. |
| `hotwords` | array of 0–1,000 strings | `[]` | Names to bias recognition toward (the page's titles, channels, buttons; the user's own words). The browser sends them already reduced to letters, digits, spaces and apostrophes. |
| `vad_silence_ms` | integer, 100–3,000 | `500` | Silence that ends an utterance. Longer for slow speakers. |

A field of the wrong type, a value out of range, or a wake phrase that breaks
the rules above is a `config` error (the helper exits). JSON nesting deeper
than 8, a string over 1,024 bytes, or a duplicate known field is a
`protocol` error.

**Hotwords are cleaned again in the helper** (they come from page text):
any character other than an ASCII letter, digit, apostrophe or space
separates words; words the recognizer's vocabulary can't spell are dropped, as are
phrases left empty and duplicates; at most 200 phrases of at most 6 words
and 64 bytes each, 4,096 bytes in all, are kept, in the order given. Nothing
that reaches sherpa-onnx's hotword parser can be read as its syntax (`:`
scores, `/` separators, `#`, `@`). Case is kept: Kroko's vocabulary is
cased, as the screen is.

**When a CONFIG applies.** A CONFIG never cuts an utterance short:
- `wake_phrases` and `wake_required` apply at once while the helper waits
  for a wake phrase or for speech. An utterance already being recognized
  finishes first; a `wake` already heard is still answered by its `final`.
- `hotwords` and `vad_silence_ms` apply from the next utterance.

### 2.3 RESET

Drops the utterance in progress, if any: no `speech_end` or `final` is
written for it, nor a `final` for a `wake` not yet answered. The helper then
waits as CONFIG says (for the wake phrase, or for speech). The voice
activity detector and the spotter start afresh, so if the user is still
speaking, a new utterance may start in the middle of their sentence
(`wake_required` false).

Events written before the helper read the RESET are still in the pipe. Every
event carries `epoch`, the number of RESET frames the helper had processed
when it wrote it (§3): after sending its n-th RESET, the browser ignores
events whose `epoch` is below n.

### 2.4 SHUTDOWN, end of input

SHUTDOWN, or the end of stdin (the browser closed the pipe or exited),
stops the helper: it writes nothing more, and exits with status 0 within
about 100 ms. An utterance in progress is dropped.

## 3. Helper to browser: events on stdout

One JSON object per line (`\n`), at most 4,096 bytes with the newline. Text
fields are valid UTF-8 of at most 1,000 bytes, with no control characters.
Every object has `"type"`; every event but `ready` and `error` also has
`"epoch"` (§2.3) and `"audio_ms"`: how many milliseconds of audio the helper
had received when the event happened (its clock; it starts at 0 with the
first AUDIO frame).

**The browser treats every line as untrusted.** A hijacked helper can write
anything: the browser checks the line length, that it parses as one JSON
object, the types and ranges of the fields it uses, UTF-8 and lengths of the
text, and that the event fits the sequence of §4, and stops the helper on any
violation. Recognized text is what the user said as far as the browser
knows, not an instruction from the helper: it goes to the voice loop's
grammar like typed text, with the same checks.

### 3.1 `ready`

```json
{"type":"ready","protocol":1,"engine":"sherpa-onnx 2.0.1, onnxruntime 1.28.2","models":["kws-zipformer-gigaspeech-3.3m-2024-01-01","silero-vad-v4","kroko-en-2025-08-06"],"sample_rate":16000}
```

Written once, after the models loaded. `protocol` is this page's version;
the browser stops a helper whose version it doesn't speak.

### 3.2 `wake`

```json
{"type":"wake","epoch":0,"audio_ms":5160}
```

A wake phrase was heard (`wake_required` true). The next utterance is the
command; exactly one `final` answers this wake (§4.1).

### 3.3 `speech_start`

```json
{"type":"speech_start","epoch":0,"audio_ms":5480}
```

The user started saying something that counts (§4).

### 3.4 `partial`

```json
{"type":"partial","epoch":0,"audio_ms":6600,"text":"Open You"}
```

The recognizer's current guess for the whole utterance so far (not just the
new words), written when it changes. For live captions only: it can change
completely until the `final`.

### 3.5 `speech_end`

```json
{"type":"speech_end","epoch":0,"audio_ms":7420}
```

The user stopped speaking: `vad_silence_ms` of silence, or the utterance
reached 20 s.

### 3.6 `final`

```json
{"type":"final","epoch":0,"audio_ms":7420,"text":"Open YouTube","confidence":0.93}
```

What was said, after the end of speech. `confidence` (0 to 1) is the
geometric mean of the recognizer's per-token probabilities: a hint, not a
calibrated probability. The text is the recognizer's own output (Kroko
writes case and punctuation); it may be empty, with confidence 0, when
nothing was recognized, including after a `wake` with no command (§4.1).

### 3.7 `error`

```json
{"type":"error","code":"protocol","message":"frame length 70000 is over 65536"}
```

Then the helper exits with status 2. Codes:

| code | Meaning |
|---|---|
| `args` | bad command line |
| `model` | a model file is missing, unreadable or didn't load |
| `protocol` | a malformed frame (§2) |
| `config` | a CONFIG that breaks §2.2 |
| `engine` | the speech engine failed while running |
| `internal` | anything else |

The message is ASCII, at most 200 bytes, for logs. It never echoes the
browser's input.

### 3.8 Exit status

| Status | When |
|---|---|
| 0 | SHUTDOWN, or the end of stdin |
| 2 | after an `error` line |
| 3 | stdout broke, or a line couldn't be written for 2 s (the browser stopped reading); nothing more is written |
| anything else, or a signal | a crash. sherpa-onnx ends the process with status 255 (`_Exit(-1)`) on some model errors. |

The browser treats every exit but its own SHUTDOWN as the helper failing.

## 4. The sequence of events

`ready` comes first, once. After it:

### 4.1 With a wake phrase (`wake_required` true)

```
wake  [speech_start  partial*  speech_end]  final
```

- The spotter and the voice activity detector run; the recognizer waits.
- On a wake phrase: `wake`. The recognizer starts on the audio right after
  the phrase, so "Hey Molt, open YouTube" in one breath works.
- `speech_start` comes with the first recognized word after the phrase, or
  when new speech starts after a pause ("Hey Molt." pause "Open YouTube.").
  If the speech that held the wake phrase ends with nothing more recognized,
  the helper keeps waiting for the command.
- At the end of speech: `speech_end`, then `final`. The helper then waits
  for the wake phrase again.
- If no speech starts within 6 s of the `wake`: `final` with empty text,
  without `speech_start` or `speech_end`, and back to waiting.
- A wake phrase heard while the command is spoken is part of the command
  (the voice grammar drops wake phrases from the text).

### 4.2 Without (`wake_required` false)

```
speech_start  partial*  speech_end  final
```

for each utterance the voice activity detector finds. The recognizer runs
only while someone speaks (with 0.6 s of audio from before the detector
noticed, so the first word isn't lost). A wake phrase said here is just part
of the utterance.

### 4.3 Limits

- An utterance ends after 20 s of speech even if the user goes on
  (`speech_end`, `final`); what follows is a new utterance, which needs a
  wake phrase again if `wake_required` is true.
- `final` text is at most 1,000 bytes; the recognizer's output is cut at a
  word or character boundary before that. A transducer emits at most one
  token per 40 ms frame, so 20 s give at most 500 tokens; nothing loops.
- The helper holds at most 2 s of audio of its own (the pre-roll), and
  sherpa-onnx's buffers for the utterance in progress.

## 5. Example

A browser session, as hex and as lines (the audio frames are abridged):

```
02 00 00 00 52 00 00 00  {"wake_phrases":["hey molt"],"wake_required":true,"hotwords":["Bohemian Rhapsody","Queen"],"vad_silence_ms":500}
01 00 00 00 80 0c 00 00  <100 ms of PCM: 3,200 bytes>
01 00 00 00 80 0c 00 00  ...
```

```
{"type":"ready","protocol":1,"engine":"sherpa-onnx 2.0.1, onnxruntime 1.28.2","models":["kws-zipformer-gigaspeech-3.3m-2024-01-01","silero-vad-v4","kroko-en-2025-08-06"],"sample_rate":16000}
{"type":"wake","epoch":0,"audio_ms":1320}
{"type":"speech_start","epoch":0,"audio_ms":2600}
{"type":"partial","epoch":0,"audio_ms":2600,"text":"Play Bohemian"}
{"type":"partial","epoch":0,"audio_ms":3900,"text":"Play Bohemian Rhapsody"}
{"type":"speech_end","epoch":0,"audio_ms":4200}
{"type":"final","epoch":0,"audio_ms":4200,"text":"Play Bohemian Rhapsody.","confidence":0.91}
```

## 6. What the helper needs from the sandbox

*(Measured with strace on Linux x64 in round 38; §6 is filled in with the
measured lists.)*

## 7. Building and shipping

*(See `scripts/bundle-speech.sh` and docs/BUILD_MACHINE_CHECKLIST.md.)*
