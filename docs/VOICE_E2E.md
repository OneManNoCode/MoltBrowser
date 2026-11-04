# Voice mode end to end: the spoken test kit

`tools/voice_e2e/` speaks to a built MoltBrowser through a virtual
microphone and checks what voice mode does, on local copies of the sites it
is used on. It measures touchless browsing's Phase 1 exit
([TOUCHLESS_BROWSING.md](design/TOUCHLESS_BROWSING.md) §10, §12):

- the owner's song scenario and at least 50 scripted spoken tasks pass at
  90% or better, in quiet and with music playing;
- a direct command acts within 1.5 s of the end of speech (reported: the
  machine that runs it isn't the minimum hardware);
- nothing a page says, plays or shows acts (Phase 3's exit, for the
  security scenarios).

It needs a Linux build (`chrome`) and the speech bundle
(`scripts/bundle-speech.sh`); it has no part in the browser itself apart
from the test log the browser writes when a test switch asks for it.

## Running it

On the CI machine, from the build workflow's last stage:

```bash
tools/voice_e2e/ci_run.sh <out dir> <bundle dir> <report dir>
```

- `<out dir>`: the build's output, with `chrome`;
- `<bundle dir>`: `scripts/bundle-speech.sh --build-only --parts
  speech,voice`'s output (`<cache>/build/linux-x64`, holding `speech/` and
  `voice/`);
- `<report dir>`: where `report.md` and `report.json` go, with the
  rendered speech, the speech check, and each scenario's logs, test log
  and screenshots.

On Ubuntu it installs what is missing (Xvfb, PulseAudio, fonts, openssl,
cmake and a compiler, numpy pinned by hash), lets the browser's and the
helpers' sandboxes make user namespaces (Ubuntu 24.04 restricts them),
fetches the pinned voices, renders the speech, checks it against the
bundle's recognizer, and runs four runs: PulseAudio in quiet
(`pulse:clean`), PulseAudio with music (`pulse:music10`), Chromium's file
capture (`file:clean`) and typed (`typed`). It exits 0 when the exit is
met, 1 when it isn't, 2 when it couldn't run. Expect about three hours for
everything on a 4-core runner; `VOICE_E2E_RUNS`, `VOICE_E2E_SCENARIOS`
and `VOICE_E2E_VOICES` narrow it.

By hand, the same steps:

```bash
python3 -I tools/voice_bench/fetch.py --dir DATA --only \
  sherpa-onnx-linux-x64,tts-piper-en_US-ljspeech-medium,tts-piper-en_US-libritts_r-medium-int8,tts-lexicon-en-us
python3 -I tools/voice_bench/run_bench.py --dir DATA --build
python3 -I tools/voice_e2e/run_e2e.py render --data DATA \
  --driver DATA/build/sherpa_bench --out SPEECH --hashes tools/voice_e2e/render_hashes.json
python3 -I tools/voice_e2e/run_e2e.py check-speech --speech SPEECH --bundle BUNDLE \
  --out REPORT/speech-check.json
python3 -I tools/voice_e2e/run_e2e.py run --browser OUT/chrome --bundle BUNDLE \
  --speech SPEECH --report REPORT --runs pulse:clean,typed --scenarios owner-song
python3 -I tools/voice_e2e/run_e2e.py list      # the scenarios
python3 -I tools/voice_e2e/run_e2e.py serve     # the fixture sites, to look at
```

`run` takes `--modes wake,always`, `--voices`, `--speeds`, `--no-loopback`
(no echo of the browser into the microphone), `--no-sandbox` (as root),
`--model` (an on-device AI model is installed: runs the scenarios that
need one), `--gate-latency` (the 1.5 s target fails the run) and
`--browser-flag` (one more flag for the browser), and `--display current`
to watch it on a real screen.

The kit's own tests need no browser and no downloads:
`python3 -I -m unittest discover -s tools/voice_e2e/tests -t tools/voice_e2e`
(CI's `voice-e2e-kit` job in `lint.yml`).

## The pieces

| File | What it does |
|---|---|
| `scenarios/*.json` | The scenarios, as data (below). |
| `fixtures/` | The fixture sites (below). |
| `e2ekit/scenarios.py` | Reads and checks the scenario files. |
| `e2ekit/expect.py` | Whether a step's outcome is there yet, and why not. |
| `e2ekit/render.py` | The synthetic speech, and the mixes. |
| `e2ekit/audio_path.py` | PulseAudio, and the file capture's WAVs. |
| `e2ekit/ears.py` | The kit's own recognizer (the bundle's `molt-speech`), to hear the browser. |
| `e2ekit/browser.py` | Launches the browser and reads it over the DevTools pipe. |
| `e2ekit/testlog.py` | Reads voice mode's test log. |
| `e2ekit/runner.py` | Runs the scenarios. |
| `e2ekit/report.py` | `report.json`, `report.md`, the verdict. |
| `e2ekit/speech_check.py` | The rendered speech through the bundle's recognizer. |
| `e2ekit/fixtures.py` | The fixture server. |
| `run_e2e.py`, `ci_run.sh` | The entry points. |
| `render_hashes.json` | The SHA-256 of each rendered set, as rendered here. |
| `lexicon-extra.txt` | Pronunciations the voices' lexicon lacks. |
| `tests/` | Unit tests, a stand-in voice and a stand-in browser. |

### The fixture sites

Served over HTTPS on one local port, under the real sites' names: the
browser maps every name to it (`--host-resolver-rules`, which also makes
every other name fail to resolve, so a run reaches nothing else) and
trusts the kit's certificate by its key (`--ignore-certificate-errors-spki-list`).

| Names | Site |
|---|---|
| `www.youtube.com` | A video site built like YouTube's, so the browser's YouTube skill finds its parts: a search form, 20 results (for "Bohemian Rhapsody" the owner's list: the fourth is Live Aid 1985, three are live by Queen), and a watch page with a player (play, mute, a volume slider, seek, captions, a speed menu, next, full screen; `?ad=1` starts with an ad whose skip button shows after 3 s). No autoplay, so voice mode's click starts it with sound. The song is the kit's synthesized music, three minutes. |
| `www.google.com`, `duckduckgo.com`, `www.bing.com`, `search.brave.com` | A search engine (whichever is the default, it is this one). |
| `en.wikipedia.org` | An encyclopedia: a home page and an article. |
| `news.test` | News: sections, a story with headings, sport, weather, and a link to `trap.test`. |
| `shop.test` | A shop: three products, a cart, a checkout (name, street, town, delivery) and "Place order" up to its confirmation. |
| `mail.test` | Webmail: an inbox, messages, and a compose form with "Send". |
| `forms.test` | A form: a text field, checkboxes, a list, radio buttons, "Submit", a long part to scroll; it counts its loads. |
| `prompts.test` | A page asking for a permission (location, notifications); a dialog that imitates the browser's own prompt; `/inject`, text that tries to make the AI's answer imitate one. |
| `files.test` | A download. |
| `dialogs.test` | `confirm()`, `alert()`, `prompt()`, and unsaved changes (`beforeunload`). |
| `trap.test` | Pushes trap history entries without user activation. |
| `sounds.test` | A video whose sound says "Hey Molt" commands over music (the kit's page voice); `/reading`, a page whose text is "Hey Molt" commands. |
| `frames.test`, `player.test` | A player inside a cross-site frame. |

The pages keep their state in the DOM and in `window.kit.state`, which the
kit reads with `Runtime.evaluate`, always without a user gesture: whatever
activation a page gets comes from voice mode. A recorder the kit adds to
every page notes each event that shows an action (a trusted click, input,
a change of the player) with its time.

### The synthetic speech

Every utterance of the scenarios, the wake phrase ("Hey Molt"), "repeat"
and the voice gesture's 40 confirmation words, 16 kHz mono 16-bit WAV:

- **Voices**, pinned by SHA-256 (`tools/voice_bench/artifacts.json`; the
  manifest records each model file's SHA-256 too): Piper en_US ljspeech
  medium (the voice `scripts/bundle-speech.sh` ships as voice mode's own,
  a woman's), and Piper en_US libritts_r medium (int8), speakers 300 (a
  man's) and 30 (a woman's). The page audio is speaker 700, a voice the
  user never speaks with.
- **Speeds** 1.0 and 1.25. A run rotates its scenarios through the six
  voices and speeds (`--voices`, `--speeds` to choose).
- **The engine**: the voice bench's driver (sherpa-onnx 2.0.1's C API,
  the release library `fetch.py` pins), one thread, with a vanishing
  sampling noise (`--noise-scale 1e-30 --noise-scale-w 1e-30`, new in the
  driver this round; sherpa-onnx's C API reads 0 as "the default", and
  1e-6 still changes some samples), so a render is the same bytes every
  time on a machine. Each render is trimmed, resampled to 16 kHz and set
  to -26 dBFS.
- **Mixes**: pink noise and the voice bench's synthesized music, each at
  10 dB under the speech (`audio.py`'s `mix_at_snr`), with 0.4 s before
  and 1.3 s after and a -65 dBFS floor; every random choice comes from a
  generator seeded with the clip's name. In PulseAudio runs with music, the
  music plays into the microphone through the whole scenario, at 10 dB
  under the speech.
- **Determinism**: `manifest.json` holds the SHA-256 of every clip and of
  every mix's PCM; `render_hashes.json` one SHA-256 per set (each voice
  and speed, each condition's mixes, the page audio, the texts). Two
  renders here gave the same 3,209 files and mixes, byte for byte. A CI
  render is compared with the recorded sets (`render --hashes`); another
  CPU may round a sample differently, so a difference is reported, not
  fatal (`--strict` makes it fatal). After a scenario's words change,
  record the sets again with `render --write-hashes`.

### The audio path

**PulseAudio** (preferred): a daemon of the kit's own, its socket in the
run directory (or a short temporary one: a Unix socket's path is limited),
with a null sink `molt_out` (the default output, where the browser's
speech and its pages' sound go; its monitor is recorded for the whole
scenario), a null sink `molt_mic_in` the kit plays into, and its monitor,
remapped, as the default input `molt_mic`, which the browser captures. A
loopback plays `molt_out`'s monitor into `molt_mic_in` at -6 dB, as a
microphone next to a speaker hears it, for echo cancellation and barge-in
(`--no-loopback` to leave it out). Each utterance is played with `paplay`,
which returns when it has been played: its end is the end of speech, to
within 20 to 30 ms here. The next one is played when the step is done and
voice mode has been listening (not thinking or speaking) for 1.5 s, more
than the gesture's quiet period (`kGestureQuietMs`), so a scenario waits
on the browser, never on fixed gaps.

**File capture** (fallback): one WAV per scenario for Chromium's fake
capture device (`--use-fake-device-for-media-stream
--use-file-for-fake-audio-capture=<wav>%noloop`, 44.1 kHz stereo), the
utterances at fixed times (the first 6 s in, each next 9 s after the one
before), timed from the moment the test log shows the microphone on. No
loopback and no recording: the scenarios that need the confirmation word
or a page's sound are skipped there.

### Seeing what voice mode heard and said: the test log

With `--molt-voice-test-log=<absolute path>` the browser's voice loop
appends one JSON line per event (`voice/voice_test_log.*`, one hook object
in `VoiceController`):

```
{"t":1790000000123,"w":1,"e":"start","kinds":["unknown","refused","open_site",...]}
{"t":...,"w":1,"e":"heard","text":"Hey Molt, open YouTube","source":"mic","clean":true,"intent":"open_site"}
{"t":...,"w":1,"e":"said","text":"YouTube is open."}
{"t":...,"w":1,"e":"activity","activity":"listening","mic":true,"waiting":true}
{"t":...,"w":1,"e":"prompt","op":"open","id":3,"kind":"permission"}
{"t":...,"w":1,"e":"confirm","op":"open","what":"press Place order"}
```

- `t` is the wall clock in milliseconds, which the kit compares with its
  own; `w` the window's number. `start` lists the grammar's command names
  (`VoiceCommandKindName()`), so the kit tells a command the build doesn't
  have from one that failed.
- `heard`: every utterance that reached the loop, typed or from the
  microphone (and whether heard cleanly), with the command it was
  understood as.
- `said`: what voice mode says, as it starts saying it.
- `activity`: voice mode's state, the microphone, and whether only the wake
  phrase counts.
- `prompt`, `confirm`: the browser's prompts and a page's dialogs, and the
  loop's own yes-or-no questions (a purchase, the activation guard's), as
  they open and close.

Like Chromium's own test switches (`--use-fake-ui-for-media-stream`,
`--log-net-log`), nothing is logged without it and the browser never sets
it; like `--moltnet-tor-binary` it takes an absolute path only. The file
is appended to as a regular file, never through a link
(`AppendToMoltFile()`), created readable by its owner alone, on one
sequence for the whole browser. MoltNet windows log nothing.

**It never writes the confirmation word.** What is said with the word
reaches the log as the word's twin, without it ("To allow it, say the
word on your screen."), as every observer of the loop gets it; and every
text (heard, said, what a question is about) is masked again: a word that
is a confirmation word of either language's list, or the waiting prompt's
word, its plural, a word of six letters or more heard one letter off
("harbour"), or the word split in two ("har bor"), becomes `[word]`. The
gesture's own sound-alike rule is broader than a log can be (it would
take "cart" for "garden"), so the log masks by letters.

The kit learns a prompt's word the way the user does: it hears the
browser say it. The browser says the word aloud when nothing else can
hear it (its own voice, echo cancellation, no page using a microphone);
the prompt scenarios also turn on "Say the confirmation word aloud even
when another program could hear it". The kit's ears (the bundle's
`molt-speech`, run by the kit, with the 40 words as its hotwords, which
the browser's helper never gets) transcribe the recording of the
browser's output since the prompt opened, and take the word after "say"
(when one word of the list is clearly the nearest). When it isn't clear,
the kit says "repeat", as a user would, and listens to the browser's new
take, three times in all. A wrong word would refuse the prompt, so the
kit says none rather than guess.

### The runner

Each scenario gets a fresh profile and HOME (`~/.moltbrowser/settings.json`
with voice mode on at start-up, always listening for the always-listening
mode), the bundle's helpers copied next to the browser (`<out>/speech`,
`<out>/voice`, where it looks on Linux), `--enable-features=MoltVoiceMode`,
the test log, the fixture sites, `--autoplay-policy=document-user-activation-required`
(as the browser tests: media with sound shows voice mode's click gave the
page activation) and the DevTools pipe (the leak kit's client: only the
kit can reach it, and no port opens). It waits for voice mode to listen,
opens the scenario's start page and tabs, then for each step:

1. says it: the WAV (with "Hey Molt" first in wake mode, except an answer
   to the browser's question), or the confirmation word, or a replayed
   recording of an earlier step;
2. takes the end of speech;
3. polls every expectation (0.2 s) until all hold or the step's timeout
   (20 s by default) passes; `no_action`, `said_none` and anything with a
   `quiet` are checked once, at the end of their quiet period;
4. measures from the end of speech to the first `heard` line (recognition),
   to the first action on the page (a trusted page event, a new document's
   start), to the first `said` line, and to the last expectation met;
5. on a failure: a screenshot of the tab, what was heard against what was
   said (word errors), what it was understood as, and why: "not in the
   grammar yet" when the step's `needs` names a command the build's
   grammar doesn't list, "not heard" when nothing reached the loop,
   "misheard" when what did wasn't the words said. The scenario stops
   there (the rest would only fail after it).

`--typed` (`--runs typed`) sends the same words through Agent mode's
"Type what you would say" field (`chrome://molt-ai-agent/` in a window of
its own behind the browser's), so recognition problems can be told apart
from action problems. Typed text never answers with a confirmation word,
and the browser reads prompts aloud only while the microphone listens, so
the prompt scenarios are skipped there.

### The scenarios

`scenarios/*.json`, data (the format is in `e2ekit/scenarios.py`): 92 of
them, 93 runs of counted tasks in a spoken run (the owner's scenario runs
with "Hey Molt" and in always-listening mode):

| Group | What |
|---|---|
| `owner.json` | The owner's song scenario: open YouTube, search, "play the live one by Queen", "the Live Aid one", louder, pause, skip ahead one minute, full screen, back; and by number. |
| `go.json` | Open by name and by address, a name it doesn't know, search the web and the site, back and forward, reload, new tab, a site in a new tab, switch and close tabs, a site's name alone. |
| `read.json` | The title, headings, page, a section, links, what's on the screen, find, next heading, the selection, stopping a reading. |
| `choose.json` | By number, by name, by position, a readback answered by number, a bare number after a list, the numbers. |
| `act.json` | Press, follow, dictate, edit what was dictated, spoken punctuation, check, choose an option, a radio button, scroll, zoom, send a form after "yes", a checkout up to its confirmation, webmail read and sent. |
| `media.json` | Play, pause, resume, louder and quieter, the volume to a level, mute, seek, speed, captions, full screen, next video, skip the ad, a player in a frame. |
| `browser.json` | Bookmarks, history, reopening a tab, downloads, settings, a new window, a private window, a MoltNet window, the AI chat, asking the AI, the menu, the browser's buttons, find's next match. |
| `recover.json` | Cancel, no, what can I say, where am I, repeat, a correction, undo (typing, a closed tab, a navigation), start over. |
| `prompts.json` | A permission allowed by its word and refused by "no", notifications, a download after voice mode's click, `confirm()`, `alert()`, `prompt()`, leaving unsaved changes. |
| `security.json` | A video whose sound says commands, a page whose text read aloud is commands, a replayed confirmation word, a dialog and an AI answer that imitate the browser's prompt, trap history entries. Each passes only if nothing acts. |

Each step names the commands it needs (`needs`). Many are this round's new
commands (volume to a level, speed, captions, undo, start over, bookmarks,
history, downloads, settings, windows, the AI chat, the menus, find's next
match, spoken punctuation, a site's name alone): their names follow the
grammar's pattern (`set_volume`, `speed_up`, `normal_speed`,
`captions_on`, `undo`, `start_over`, `bookmark_page`, `open_bookmarks`,
`open_bookmark`, `show_history`, `reopen_tab`, `show_downloads`,
`open_settings`, `new_window`, `new_private_window`,
`new_moltnet_window`, `open_ai_chat`, `ask_ai`, `open_menu`,
`show_browser_buttons`, `next_match`). A build without the name reports
the step as "not in the grammar yet" rather than as a failure; if the
grammar names a command otherwise, change the scenario's `needs` (data).
Their expected outcomes follow `docs/VOICE_COMMANDS.md`; check them against
the commands as built.

### The report

`report.json` (schema `molt-voice-e2e-report/1`) and `report.md`: per run,
the counted tasks passed, the rate without the missing commands, the
"not in the grammar yet" ones, the owner's scenario, the security
scenarios, the action latency (median, 90th percentile, share within
1.5 s, for passed direct commands), recognition (word error rate and
exact transcripts over the spoken steps), every scenario's result and
reason, what went wrong step by step with screenshots, and what was
misheard; and the speech check's table when `speech-check.json` is in the
report directory. The verdict fails on a run under 90%, an owner's
scenario that didn't pass, a security scenario that acted, or a scenario
that couldn't run (and on the latency median with `--gate-latency`).

## Measured here

This machine (4 cores, no sound card, no Chromium build, November 2026)
can't run the browser. What ran:

- **The speech helper**, built with `scripts/bundle-speech.sh
  --build-only --parts speech,voice` from this tree (ONNX Runtime and
  sherpa-onnx from the cache of rounds 38 and 40; `molt-speech` SHA-256
  `578858a1…`, `molt-voice` `8c1cfe9e…`).
- **The renders**: 178 texts by six voices and speeds, and the page audio:
  1,073 clips, 37 MB; rendered twice, identical (above).
- **The PulseAudio plumbing**, with the kit's own `Pulse` class: rendered
  utterances played into the virtual microphone, recorded from the default
  source as the browser captures it, and heard by `molt-speech` ("Set the
  volume to fifty percent", "Search for Bohemian repsody", also with the
  music bed); the browser's output played into the default sink and
  recorded from its monitor and, through the loopback, from the
  microphone. `paplay` returned 20 to 30 ms after the speech's end.
- **The runner**, against a stand-in browser (`tests/fake_browser.py`)
  over the real DevTools pipe, typed and spoken through a player, with the
  report: passed, failed with a screenshot, "not in the grammar yet",
  quiet and skipped scenarios as expected.
- **Recognition**: every utterance through the bundle's `molt-speech`
  (Kroko EN, every utterance counting, as in always-listening mode),
  `check-speech`:

<!-- speech check: filled from check-speech -->
| Voice, speed | Condition | Utterances exact | WER | Words right | "Hey Molt" woke it |
|---|---|---|---|---|---|
| ljspeech@1 | clean | 51% (70/138) | 23.7% | 18/40 | 138/138 |
| ljspeech@1.25 | clean | 54% (75/138) | 21.5% | 16/40 | 138/138 |
| libritts-m@1 | clean | 86% (119/138) | 7.2% | 37/40 | 138/138 |
| libritts-m@1.25 | clean | 86% (119/138) | 6.7% | 35/40 | 0/138 |
| libritts-f@1 | clean | 79% (109/138) | 11.5% | 35/40 | 0/138 |
| libritts-f@1.25 | clean | 70% (96/138) | 17.7% | 32/40 | 0/138 |

  "Exact" is the whole transcript equal to the utterance after the voice
  bench's normalization; the grammar takes many near misses (sound-alike
  numbers, fillers), so it is a lower bound on what voice mode
  understands. After "Hey Molt", ljspeech's commands came out right 71
  and 72 times of 138, libritts-m@1's 115.

What this says before the build runs:

- **The ljspeech voice is a hard test.** Rendered deterministically it is
  heard about as the voice bench and the voice helper's own check found
  (VOICE_HELPER_PROTOCOL.md §8.5: 29% of words wrong): "open" as "hoping",
  "fourth" as "thirst", onsets lost on short words. Its tasks will fail
  more often for that reason alone; the report's "misheard" and the typed
  run tell them apart.
- **"Hey Molt" by three of the six voices and speeds never wakes the
  spotter** (libritts speaker 30 at both speeds, speaker 300 at 1.25): a
  wake-mode run fails their scenarios at the first step as "not heard",
  whatever the rest of voice mode does. The same voices' commands are
  heard well in always-listening mode. Compare `pulse:clean` with the
  owner's always-listening run, or narrow the voices with `--voices`.
  It is the wake phrase model's finding (round 38 measured a 23% miss
  rate on the bench's voices), worth a look with real voices on the
  build machine.
- **Misheard by every voice** (a grammar or vocabulary matter, not
  noise): "MoltNet" ("mold net", "moldenet"), "AI" ("the A chat", "the
  eye"), "Lyon" ("lion", "Leon"), "Pentatonix" ("Pentatonics"), "trams"
  ("trims", "trems"), "type" before a short word ("tight by bread"), "see
  you" ("CU"), "click name" run together ("Clickname"), and names
  dictated into fields ("Ada Lovelace" as "A to Lovelace", "Alan Turing"
  as "Allen touring"). On a page, its names are the recognizer's hotwords
  (titles, links, buttons), which should help "Lyon", "trams" and
  "Pentatonix"; "MoltNet" and "AI" need the grammar to take what is
  heard ("mold net window", "the A chat"). Dictated names are what the
  user would correct ("no, I said ...").
- **The kit's ears on the confirmation words** said alone: 67 of 80 found
  (ljspeech 28 of 40, libritts-f 39 of 40), none wrong; the browser says
  the word in a sentence ("To allow it, say harbor."), which helps, and the
  kit asks for "repeat" when it isn't sure.

## Limits

- Synthetic voices flatter a recognizer in some ways and fail it in
  others; the owner's and other people's real voices are the build
  machine's checklist (§7.3, "speech-session").
- The CI machine isn't the minimum hardware: the latency is reported, and
  gates only on request.
- No MoltNet traffic: `new MoltNet window` only checks a window opens and
  voice mode stays where it is (Tor isn't reachable from the fixture
  network).
- The scenarios that need an on-device AI model (asking the AI, an answer
  that imitates a prompt) run only with `--model`.
- A permission the page asks for is the real prompt; whether the browser
  then gets a location (no location service here) isn't checked, only the
  permission's state.
