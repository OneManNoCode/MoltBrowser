# The voice helper protocol (`molt-voice`, version 1)

Status: **version 1**, October 2026 (touchless browsing, round 40). The
browser side (`voice/voice_speech_bundled.h`) and the helper both implement
exactly this page. A change to it bumps the version in READY (§3.1) and is
written here first.

`molt-voice` is voice mode's own voice
([TOUCHLESS_BROWSING.md](TOUCHLESS_BROWSING.md) §5.4, §6.6): a small
resident helper process that turns what voice mode says into speech, on the
CPU and with nothing from the network. The browser starts it while voice
mode is on, sends it text and plays the audio it sends back through
Chromium's audio service, so the microphone's echo canceller has that audio
as its reference and the user can talk over it. When the helper isn't
installed or keeps failing, voice mode speaks with the platform's voices as
before.

Inside: a Piper (VITS) voice, `en_US-ljspeech-medium`, run by sherpa-onnx
2.0.1 over ONNX Runtime 1.28.2, the same pinned builds as `molt-speech`
([SPEECH_HELPER_PROTOCOL.md](SPEECH_HELPER_PROTOCOL.md) §7). It is the
only English Piper voice the voice bench cleared for shipping (trained from
scratch on the public-domain LJ Speech; [VOICE_BENCH.md](../VOICE_BENCH.md)
§3.9, §4, §6.7). sherpa-onnx 2.x reads English text only through a lexicon
and silently skips the words it lacks, so the helper never gives it text:
its own front end (§4) turns the text into phonemes and hands those over.

## Why a second helper

The speech helper could have grown a TTS mode (protocol version 2). A
helper of its own was chosen:

- **The page's text never shares a process with the user's voice.** What
  voice mode reads aloud comes from pages. If hostile text ever took over
  the voice helper, it would have the voice's model and its own stdout, not
  the microphone stream `molt-speech` hears.
- **Failures stay apart.** sherpa-onnx ends its process on some errors
  ([VOICE_BENCH.md](../VOICE_BENCH.md) §5.3). A voice that fails falls back
  to the platform's voices while listening goes on, and the other way round.
- **Audio out doesn't fit `molt-speech`'s output.** Its stdout is JSON lines
  of at most 4,096 bytes, checked line by line; audio needs binary frames
  of up to 64 KiB. A second protocol on the same stdout would complicate
  the checks of both.
- **Lifetimes and limits differ.** Listening runs all session long at a few
  percent of a core; the voice runs in bursts (about a tenth of a core per
  second of speech) and only while voice mode is on.

The cost: a second 24 MB executable (the two share no code at run time)
and a second ONNX Runtime in memory while both run (§8).

## 1. Starting it

```
molt-voice --models <dir> [--threads 1|2]
```

- `--models` (required): the bundled, read-only model directory (§7.2).
- `--threads` (default 1): ONNX Runtime threads for the voice.
- Anything else on the command line is an `args` error (§3.4).

The helper checks that every model file is there, reads the voice's
tokens and the lexicon, loads the voice (1.3 s here, §8), then writes
READY.
Requests sent before READY wait in the pipe.

## 2. Framing, both ways

Every frame, in both directions, is an 8-byte header and a payload:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `type`, unsigned, little-endian |
| 4 | 4 | `length` of the payload in bytes, unsigned, little-endian |
| 8 | `length` | payload |

Each side checks a header as soon as its 8 bytes are in (a type known for
its direction, a length that type allows), so a bad frame is refused
without waiting for or buffering its payload. Frames may be split across
reads any way at all.

### 2.1 Browser to helper (stdin)

| type | name | payload |
|---|---|---|
| 1 | SPEAK | `id` (4 bytes), `rate` (2 bytes, percent, 50 to 250), 2 reserved bytes (0), then the text: 1 to 4,096 bytes |
| 2 | CANCEL | empty |
| 3 | SHUTDOWN | empty |

- **SPEAK** asks for `text` to be said at `rate` (100 is the voice's own
  pace). The text is any bytes: the helper's front end cleans it (§4). The
  helper answers each SPEAK with exactly one DONE, in the order sent, and
  the request's AUDIO frames before it. At most 16 requests may wait behind
  the one being said; the browser sends one or two at a time.
- **CANCEL** cancels every request sent so far: the one being said stops
  after the unit being synthesized (under a second, §4.3), or at once if
  it is pacing itself, and the ones waiting are answered at once. Each still gets its DONE, with status
  `cancelled`. The browser drops their late audio.
- **SHUTDOWN**, or the end of stdin, stops the helper at once: it writes
  what it already queued (for at most 0.5 s) and exits with status 0.

Anything else is a protocol error: an unknown type, a SPEAK of fewer than 9
or more than 4,104 bytes, a rate out of range, a reserved field that isn't
0, a CANCEL or SHUTDOWN with a payload, a 17th waiting request.

### 2.2 Helper to browser (stdout)

| type | name | payload |
|---|---|---|
| 1 | READY | `protocol` (4 bytes, 1), `sample_rate` (4 bytes, 8,000 to 48,000), the voice's name (0 to 64 bytes of printable ASCII) |
| 2 | AUDIO | `id` (4 bytes), then 16-bit signed little-endian mono samples at `sample_rate`: 1 to 32,766 of them (at most 64 KiB); the helper writes at most 4,096 (0.19 s) per frame |
| 3 | DONE | `id`, `status`, `audio_ms`, `first_audio_ms`, `synth_ms` (4 bytes each) |
| 4 | ERROR | `code` (4 bytes), a message (0 to 200 bytes of printable ASCII) |

## 3. The helper's frames

**The browser treats every frame as untrusted** (§5). A helper that the
page text it reads took over could write anything.

### 3.1 READY

Once, first, after the voice loaded: the protocol's version (1), the
voice's sample rate (22,050 Hz for `en_US-ljspeech-medium`) and its name
(`piper-en_US-ljspeech-medium (sherpa-onnx 2.0.1, onnxruntime 1.28.2)`),
for logs.

### 3.2 AUDIO

The next samples of request `id`, which is always the oldest request not
yet answered by a DONE. The helper writes a request's audio as it
synthesizes it, unit by unit (§4.3), at most 3 s ahead of real time.

### 3.3 DONE

| Field | Meaning |
|---|---|
| `id` | the request, the oldest one not yet answered |
| `status` | 0 `spoken`; 1 `cancelled`; 2 `nothing` (the text had nothing to say: emoji, punctuation, another script); 3 `cut` (its audio reached the per-request limit, §4.4); 4 `engine` (the engine failed on a unit; the helper goes on) |
| `audio_ms` | the audio written for it, in ms: its samples times 1,000 over the sample rate, rounded down |
| `first_audio_ms` | from the request reaching the front of the queue to its first AUDIO frame; 0 without audio |
| `synth_ms` | time spent synthesizing it |

### 3.4 ERROR, exit status

ERROR, then the helper exits with status 2. Codes: 1 `args`, 2 `model` (a
model file is missing or didn't load), 3 `protocol` (§2.1), 4 `engine`,
5 `internal`. The message is for logs and never echoes the browser's input
(it may give a number: a length, a rate).

| Status | When |
|---|---|
| 0 | SHUTDOWN, or the end of stdin |
| 2 | after an ERROR frame |
| 3 | stdout broke, a frame couldn't be written for 2 s (the browser stopped reading), or more than 4 MiB of frames waited; nothing more is written |
| anything else, or a signal | a crash (sherpa-onnx ends the process with status 255 on some model errors it finds while loading) |

## 4. What the helper does with a SPEAK

### 4.1 Text to words (`voice_text.h`)

The text is page-derived and untrusted: any bytes, at most 4,096. One pass,
linear in the input:

- **Unicode:** invalid UTF-8, control and format characters (zero-width
  characters, bidirectional overrides, the BOM, tags, variation selectors)
  and combining marks go; Latin letters lose their accents ("Beyoncé" is
  "Beyonce"); quotes, dashes, spaces and fullwidth letters become ASCII;
  other scripts, emoji and symbols separate words and are not read.
- **Numbers:** "1,234" one thousand two hundred thirty four; "3.5" three
  point five; "-5" minus five; "1st", "22nd"; "50%"; "$349.99" three
  hundred forty nine dollars and ninety nine cents (also €, £, ¥, ₹);
  "1.2M", "15K" (counts as pages show them); "1985" alone nineteen eighty
  five; "1980s", "'90s"; "#1" number one; "No. 5"; "5-10" five to ten;
  "4/5" four out of five; "2.0.1"; units after a number ("10GB", "72°F",
  "60fps"); a number with leading zeros, or longer than 15 digits, digit
  by digit, at most 24 digits.
- **Dates and times:** "2026-10-30" and "10/30/2026" October thirtieth,
  twenty twenty six; "6:07" six oh seven, "10:00" ten o'clock, "10:30pm";
  "1:02:03" one hour two minutes three seconds; "0:45" and "45:30" (no
  hour that could be a clock's) as durations.
- **Words:** abbreviations ("Mr.", "Dr.", "St.", "vs", "etc.", "approx",
  months), dotted initials ("e.g." for example, "i.e." that is, "U.S."
  spelled), "&" and, "+" plus, "@" at, "C++", "C#", "w/" with; letters and
  digits run together are split ("mp3" m p three).
- **Addresses:** a URL (with a scheme or "www.") or a host with a known
  top-level domain is said as its host: "https://www.example.com/path?q=1"
  example dot com; "someone@example.org" someone at example dot org. A
  two-letter last label is spelled ("bbc dot co dot u k").
- **Sentences** end at ". ! ?"; ", ; : ( ) - |" and the like are pauses.
- **Bounds:** at most 1,500 words per request (pauses included), 40
  letters per word.

### 4.2 Words to phonemes (`voice_phonemes.h`, `voice_lts.h`)

Every word gets phonemes in espeak-ng's IPA for American English, which the
Piper voice was trained on:

1. a few function words as said in running text ("a", "an", "or", "her",
   "o'clock", "OK"), which the lexicon lacks or gives as letter names;
2. sherpa-onnx's English lexicon (`lexicon-en-us.txt`: 168,000 words from
   CMUdict and espeak-ng's word list);
3. acronyms spelled: a word in capitals of at most three letters ("GPU",
   "US") unless it is a common word ("GET", "THE"), one of four or five the
   lexicon lacks ("HTML"), a word without vowels ("cpp") or of one or two
   letters; in a sentence mostly in capitals (a title) capitals are words;
4. forms of a lexicon word: "'s", plurals, "-ed", "-ing", "-er", "-est",
   "-ly", "-ness", "-ment", "-ful", "-less" and others, with the dropped
   "e" and doubled consonant undone, and prefixes ("un-", "re-", "non-",
   "pre-", "dis-", "mis-", "anti-", "multi-", "super-", "inter-", "micro-",
   "auto-");
5. joined words: two (or, up to 20 letters, three) lexicon words of at
   least three letters each, the split with the longest shortest part
   ("QuietComfort", "chillhop", "MoltBrowser"); words in camel case split
   at their capitals ("iPhone");
6. letter-to-sound rules in the style of the Naval Research Laboratory's
   (1976): context rules per letter. On words the lexicon has, they get 28%
   of the words exactly right and 22% of the phoneme bytes wrong; they are
   for the words nothing above knows ("Eilish").

Every code point that reaches the voice is one of its tokens
(`tokens.txt`); anything else is dropped. A sentence is the words'
phonemes separated by spaces, pauses as ",", the sentence's own mark at the
end.

### 4.3 Units and pacing (`voice_synth.h`)

A sentence is synthesized in units of at most 128 phoneme code points
(about 9 s of speech), cut after a pause in the second half of a unit,
else between words. The engine can't be stopped inside a unit, so this
also bounds how long a CANCEL takes (§8.2). The first
unit of a request is shorter: cut at its first pause after 8 code points,
else between words within 48, so the first sound comes after about a
second of speech is synthesized, not a whole sentence. Each unit goes to
sherpa-onnx with the voice's own settings (noise 0.667, duration noise
0.333) and a length scale of 100 over the request's rate.

The helper stays at most 3 s of audio ahead of real time (counted from the
request's first AUDIO frame), so the browser holds little more than that
and a CANCEL has little to undo.

### 4.4 Limits

At most 30 s of audio per unit and 360 s per request (DONE `cut`); 4 MiB of
frames waiting to be written (exit 3).

## 5. The browser's side

`voice/voice_speech_bundled.h` (`BundledVoice`), under `VoiceSpeech`:

- **Started** while voice mode is on in the window
  (`VoiceController::Start()`), as `molt-voice --models <folder>/models`
  from the bundled folder only (`helper_binary.h`; no system copy, no
  PATH), under the helper sandbox's resident mode (§6). It ends when voice
  mode is off and nothing more is to be said.
- **Every frame** passes `molt_voice::FrameReader` (the header first) and
  `VoiceStreamChecker`: READY first and once with protocol 1 and a sample
  rate of 8 to 48 kHz; AUDIO and DONE only for the oldest request sent and
  not yet answered; at most 361 s of audio per request; a DONE whose
  `audio_ms` is what arrived. Anything else ends the helper.
- **Played** through an `audio::OutputDevice` on the default output device,
  made from the audio service's `StreamFactory` (as the earcons are), in
  20 ms buffers at the voice's rate. With Chrome-wide echo cancellation
  (`media::kChromeWideEchoCancellation`, Windows, macOS and Linux), the
  audio service's `OutputDeviceMixerManager` mixes every output stream to
  that device and gives the mix to the echo canceller of an input stream
  that asked for processing, as voice mode's microphone does
  (`voice_audio.h`): the voice is in its reference. With the platform's
  echo canceller the reference is the system's output, which holds it too.
- **Requests finish** when their DONE arrived and their audio played out.
  `VoiceSpeech` hands over at most two at a time (the next is synthesized
  while the last plays). "Stop", barge-in and a new command cancel: a
  CANCEL to the helper, the player's buffer flushed at once.
- **Failures:** no READY within 30 s, a crash, a protocol violation, the
  resident limits (§6), or audio that stops moving for 4 s (the output
  device failed): the helper ends, what it hadn't finished is said again by
  the platform's voice, and it starts again after 2 s. After two failures
  in a row (a helper that ran for a minute resets the count) the
  platform's voices speak until voice mode is next turned on.
  While the helper is starting, an answer waits for it at most 3 s, then
  the platform's voice says it.

## 6. What the helper needs from the sandbox

The same resident confinement as `molt-speech`
([SPEECH_HELPER_PROTOCOL.md](SPEECH_HELPER_PROTOCOL.md) §6;
`molt_ai/common/helper_resident.h`): no deadline; nothing writable at all;
only its own folder (`voice/`, which holds `models/`) and the system's
libraries readable (plus `/proc/cpuinfo` on Linux); no network, no child
processes; memory at most 1 GiB, CPU at most 0.75 of a core averaged over
30 s; stderr nowhere; it dies with the browser. One process with four
threads of its own (reading stdin, synthesizing, writing stdout, and the
writer's watchdog) and ONNX Runtime's pool. Its system calls are a subset
of `molt-speech`'s (the same libraries, no file but its models).

## 7. Building and shipping

### 7.1 The build

`scripts/bundle-speech.sh --parts voice` (or `speech,voice`) builds it
like `molt-speech`: ONNX Runtime and sherpa-onnx from the same pinned
tarballs (sherpa-onnx now with its TTS code, which adds no dependency; each
function in its own section, so `molt-speech` links none of it), then
`molt-voice` from `chrome/browser/molt_ai/voice/voice_helper/`
(`CMakeLists.txt`): the std-only core GN also builds (the frames, the front
end, the work for one request), plus `standalone/` (the engine over
sherpa-onnx's C API, stdout and `main`), linked statically.

### 7.2 Where it goes

| OS | Folder |
|---|---|
| macOS | `MoltBrowser.app/Contents/Resources/voice/` |
| Linux | `/opt/moltbrowser/voice/` |
| Windows | `<install dir>\voice\` (needs the installer's read grant, like `speech\`) |

Opt-in, like the speech helper: `MOLT_BUNDLE_VOICE=1`.

```
voice/molt-voice[.exe]
voice/models/{en_US-ljspeech-medium.onnx, en_US-ljspeech-medium.onnx.json,
              tokens.txt, MODEL_CARD, lexicon-en-us.txt}
voice/LICENSES/...
voice/.build-info
```

Every model file is pinned (`VOICE_FILES` in the script; `--verify DIR
--parts voice` checks an installed copy). The archive's `espeak-ng-data/`
isn't used and doesn't ship. On macOS the helper is signed like
`molt-speech`.

### 7.3 Licences

`voice/LICENSES/README.md` (from `LICENSES.md` in the helper's folder):
the voice (LJ Speech, public domain; its model card ships), the lexicon
(built from CMUdict, BSD-2-Clause, and espeak-ng's word list,
GPL-3.0-or-later: used under the GPL like MoltBrowser), the letter-to-sound
rules (MoltBrowser's code), and the libraries' licences and notices.

## 8. Measured

October 2026, Linux x64, on the shared 4-vCPU server the voice bench and
the speech helper were measured on (Xeon Sapphire Rapids under KVM;
[VOICE_BENCH.md](../VOICE_BENCH.md) §2.4), one ONNX Runtime thread, with
no other heavy job running. `tools/voice_bench/voice_helper_check.py`
drives the built helper over its stdin and stdout as the browser does; the
build machine runs it again on the minimum hardware
(`docs/BUILD_MACHINE_CHECKLIST.md` §3.5). Expect a laptop to be slower.

### 8.1 The build

| | |
|---|---|
| `molt-voice` | 24.8 MB, stripped; links only glibc (`libc`, `libm`, `libmvec`), the C++ runtime is inside |
| The voice | 69.8 MB: the model 63.5 MB, the lexicon 6.3 MB, its tokens, settings and model card |
| `molt-speech` | 24.7 MB, down from 27.3 MB: sherpa-onnx is now compiled with function sections, so the linker drops what neither helper calls |
| Build | sherpa-onnx with its TTS code 5 minutes, each helper under a minute (`--jobs 2`; ONNX Runtime reused) |
| Every source and model | matched its pinned SHA-256 |

### 8.2 The protocol (§2, §3)

16 checks, all as specified:

| Check | The helper |
|---|---|
| Frame type 9; a length of 70,000 and of 4 GiB (refused from the header alone); CANCEL with a payload; SPEAK without text, at rates 49 and 251, with its reserved field set | ERROR `protocol`, exit 2 |
| 17 requests sent at once | ERROR `protocol` ("too many requests wait"), exit 2 |
| SHUTDOWN; end of input; end of input inside a frame | exit 0 |
| CANCEL while a long reading is said, with another request behind it | both answered `cancelled`, the last DONE 0.77 s after the CANCEL (the unit being synthesized finishes first; the browser stops playing at once) |
| SPEAK after a CANCEL; SHUTDOWN after work | spoken; exit 0 |
| A browser that stops reading stdout (a 64 KiB pipe) | exit 3, 3.5 s after the request: filling the pipe, then the 2 s limit |

The same 16 checks and the sentences of §8.3 also ran with the helper
inside MoltBrowser's Linux helper sandbox (`helper_sandbox_linux.cc`:
Landlock ABI 7 and the seccomp-bpf filter, through the speech helper's
test wrapper, which grants its folder and an empty scratch directory): the
same answers, the same timings.

### 8.3 Speaking

Page-like sentences (`voice_helper_check.py`'s set: a short answer, a
result, a list of results, a product with a price and a date, times and a
duration, an address and an e-mail, acronyms and joined names, a question
with choices, and hostile text: a bidirectional override, zalgo, emoji,
another script, zero-width characters, 30 digits, a 50-letter word), after
one request to warm the model up:

| Sentence | Audio | First audio | RTF |
|---|---|---|---|
| "YouTube is open." | 1.3 s | 129 ms | 0.10 |
| a result | 4.4 s | 191 ms | 0.10 |
| a list of two results | 13.3 s | 106 ms | 0.10 |
| a product, a price, a date | 12.2 s | 197 ms | 0.09 |
| times and a duration | 7.4 s | 200 ms | 0.09 |
| an address and an e-mail | 4.3 s | 224 ms | 0.09 |
| acronyms and names | 7.2 s | 106 ms | 0.09 |
| a question with choices | 8.2 s | 237 ms | 0.09 |
| hostile text | 6.4 s | 211 ms | 0.10 |

- **The first sound comes 0.1 to 0.24 s after the request**, the bench's
  0.13 s for "YouTube is open." included: the first unit is cut at the
  first pause (§4.3). Without that cut, a sentence whose first clause is
  long waited up to 0.58 s.
- **Synthesis runs at a tenth of real time** (RTF 0.09 to 0.10; the bench
  measured 0.099 for this voice on one thread).
- **The rate:** the same sentence lasts 7.7 s at 50%, 4.5 s at 100% and
  2.3 s at 250%.

### 8.4 Cost

| | |
|---|---|
| Ready | 1.3 s after it starts, holding 146 MB |
| Reading a long page (117 s of audio), taken in real time | 10.9 s of CPU: 9.3% of one core while it speaks |
| Peak memory | 365 MB |
| Idle (10 s between answers) | 0% of a core |

The bench measured 365 MB for this voice in sherpa-onnx's own driver.
Both helpers together, while voice mode listens and speaks: about 560 MB
at most, each under its own 1 GiB limit.

### 8.5 Heard back

The speech helper's recognizer (`molt-speech`, Kroko EN, every utterance
counting) on the voice's output, resampled to 16 kHz, against what it
should have said (written as a recognizer writes it, and normalized on both
sides):

| | Words wrong |
|---|---|
| This voice through its own front end (§4) | 28.6% of 140 (27.1% in an earlier run: Piper samples a new take each time) |
| The same sentences through sherpa-onnx's own lexicon path, numbers and symbols written out by hand | 31.4% (it skips "or", "QuietComfort", "MoltBrowser" and "MoltNet") |

The short answer and the question with choices came back word for word;
most of the rest is names and numbers that a synthetic voice and Kroko
both find hard ("Bohemian Rhapsody" heard "Bohemian Ripsody", "Bose"
"boast", "macOS" "Mc OS"). A recognizer is a rough judge of a voice:
whether people understand it, and like listening to it, is for the build
machine's checklist ("bundled-voice").

The letter-to-sound rules alone, on every 50th word of the lexicon (3,357
words it has, so a harder test than the names they are for): 27.8% of the
words exactly right, 22.3% of the phoneme bytes wrong (stress ignored).

### 8.6 Robustness

- **Unit tests:** 30 of the helper's std-only parts and 30 of the
  browser's (the stream checker, the session's rules), under ASan and
  UBSan.
- **Fuzzers:** `molt_voice_helper_fuzzer` (page text through the whole
  front end; the frames a browser writes, each SPEAK said through a fake
  engine) and `molt_voice_speech_fuzzer` (the helper's stdout as the
  browser reads it), under ASan and UBSan: 304,633 runs in 301 s and
  1,703,098 runs in 241 s, no finding; their 20,000-run smokes in 18 s and
  2 s. The first run of the helper's fuzzer found, in 39 s, text on which
  the scan stood still (".1.2" read as a version with no digits before
  it); fixed, and the input is in its seed corpus.
- **Hostile text costs little:** a request of 195 random 20-letter words,
  one of words joined from many lexicon words and suffixes, and one of
  100 40-letter words each take under 20 ms of the front end (the
  lexicon loads once, in 70 ms).
- **Bounded:** frames of at most 64 KiB; 4,096 bytes of text, 1,500 words
  and 40 letters a word per request; units of 128 phoneme code points,
  30 s of audio a unit and 360 s a request; 16 waiting requests; 4 MiB of
  unwritten frames; the browser holds at most a request's lookahead of
  audio plus one unit.
