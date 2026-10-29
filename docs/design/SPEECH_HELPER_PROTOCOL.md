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
(about 0.5 s on the 4-core server it was measured on; seconds on a slow
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
  `final`. New wake phrases rebuild the spotter (about 0.1 s).
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
  is true. The voice activity detector itself also starts afresh after
  20 s of speech without a pause, so endless sound (a television, music
  with vocals) can't grow its buffers.
- `final` and `partial` text is at most 1,000 bytes, cut at a word near the
  end, else at a character. A transducer emits at most one token per
  frame and loops never; 20 s give at most a few hundred tokens.
- The helper keeps 2.5 s of audio of its own (for the pre-rolls), and
  sherpa-onnx's buffers for the utterance in progress. Peak memory is in
  §8.

## 5. Example

A browser session, as hex and as lines (the audio frames are abridged):

```
02 00 00 00 59 00 00 00  {"wake_phrases":["hey molt"],"wake_required":true,"hotwords":["Bohemian Rhapsody","Queen"]}
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

*(Measured with strace on Linux x64; filled in below.)*

## 7. Building and shipping

*(See `scripts/bundle-speech.sh`.)*

## 8. Measured

*(Filled in below.)*
