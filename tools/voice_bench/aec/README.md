# What the speech helper hears through the echo canceller

The spoken suite's speakers run (tools/voice_e2e, `pulse:clean`) takes an hour
and a browser build. This directory runs the same echo cancellation offline in
minutes. It uses the pinned Chromium's own WebRTC audio processing, built
standalone.

- **`build.sh [WORK_DIR]`** builds `apm_sim`. It streams the pinned version's
  official source tarball (about 2 GB) and keeps only `third_party/webrtc`'s
  audio processing and abseil (40 MB). Then it compiles them with clang, along
  with `src/media/webrtc/molt_speech_recognizer_processing.cc` as is. Ten
  minutes on four cores; the default `WORK_DIR` is `~/.cache/molt-aec`.
- **`apm_sim`** processes a render WAV (what the browser plays) and a capture
  WAV (what the microphone hears), both 16 kHz mono, the way voice mode's
  stream is processed:
  - echo cancellation and noise suppression, no gain control;
  - the browser's WebRTC field trials;
  - `--tuning=chromium` (pages' streams) or `--tuning=voice` (voice mode's,
    `AudioProcessingSettings::speech_recognizer`);
  - `--linear=OUT.wav` also writes what the echo canceller's linear filter
    alone left.
- **`scenes.py media`** says "Hey Molt, louder" and "Hey Molt, mute" in the
  suite's voices over the suite's song, with its echo 6 dB down, and counts how
  often molt-speech woke and wrote the command.
- **`scenes.py security`** has nobody speak:
  - a video says "Hey Molt, ..." commands: does the helper wake?
  - voice mode's own voice says the 40 confirmation-word prompts: does the
    helper write a word? Both as is and with what was left amplified.

```
tools/voice_bench/aec/build.sh
tools/voice_bench/aec/scenes.py media --apm-sim ~/.cache/molt-aec/apm_sim \
    --helper .../molt-speech --models .../speech/models \
    --tts-driver .../sherpa_bench --tts-dir <voice bench work dir> \
    --tuning chromium,voice,linear --at 8,12,16,20
```

The helper, models and TTS driver are the speech bundle's and the voice
bench's (tools/voice_bench/README.md), as the spoken suite uses them.

What it measured for TOUCHLESS_BROWSING.md §6.1 (round 48), out of 48 (12 per
point of the song):

| Tuning | woke | command written right |
|---|---|---|
| chromium | 2 | 1 |
| voice | 32 | 21 |
| linear (the linear filter alone; not something the browser does) | 42 | 31 |

## Reading the numbers

- **Fragile counts.** A scene near the helper's threshold can flip on one
  least-significant bit of the audio, so two builds of the same code, or a
  WAV written back with different rounding, can differ by a scene or two.
  Compare tunings over several points of the song (`--at`), not single counts.
- **Song length matters.** `synth_music()`'s random draws depend on the
  song's length, so `scenes.py` always makes the same minute of it.
- **Not modelled here:**
  - a real loudspeaker's distortion;
  - a room;
  - a device's own echo canceller;
  - PulseAudio's loopback, whose rate it adjusts.

  The spoken suite and the build machine's device checks
  (docs/BUILD_MACHINE_CHECKLIST.md) cover those.
