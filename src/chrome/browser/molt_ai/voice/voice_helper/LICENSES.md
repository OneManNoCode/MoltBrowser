# molt-voice: licences and attribution

`molt-voice` is voice mode's own voice in MoltBrowser's touchless browsing.
It is part of MoltBrowser (GPLv3, GenEye AI Labs Inc.); its source is in
MoltBrowser's repository under `src/chrome/browser/molt_ai/voice/voice_helper/`,
and `scripts/bundle-speech.sh --parts voice` builds it, with the libraries
below, from pinned sources.

## Libraries linked into molt-voice

| Library | Version | Licence | Notices shipped |
|---|---|---|---|
| sherpa-onnx (k2-fsa) | 2.0.1 | Apache-2.0 | `sherpa-onnx-LICENSE.txt` |
| ONNX Runtime (Microsoft) | 1.28.2 | MIT | `onnxruntime-LICENSE.txt`, `onnxruntime-ThirdPartyNotices.txt` (abseil, protobuf, ONNX, flatbuffers, re2, Eigen, cpuinfo, date, GSL, SafeInt, mp11, nlohmann/json and others) |
| kaldi-native-fbank | 1.22.4 | Apache-2.0 | (sherpa-onnx's licence) |
| kaldi-decoder | 0.3.0 | Apache-2.0 | |
| OpenFst (csukuangfj's fork) | 1.8.5 | Apache-2.0 | |
| simple-sentencepiece | 0.7 | Apache-2.0 | |
| utf8proc | 2.12.0 | MIT | |
| nlohmann/json | 3.12.0 | MIT | |
| Eigen | 5.0.1 | MPL-2.0 (unmodified; source at https://gitlab.com/libeigen/eigen) | |
| kissfft (in kaldi-native-fbank) | | BSD-3-Clause | |

## The voice and the lexicon in `models/`

**The voice: Piper `en_US-ljspeech-medium`.** A VITS voice for Piper
(https://github.com/OHF-Voice/piper1-gpl, formerly rhasspy/piper), trained
from scratch on the LJ Speech dataset, whose recordings and texts are in
the public domain (https://keithito.com/LJ-Speech-Dataset/); its model card
(`MODEL_CARD`, shipped) says so. In the ONNX form the k2-fsa project
publishes for sherpa-onnx (`vits-piper-en_US-ljspeech-medium.tar.bz2`,
release `tts-models` of https://github.com/k2-fsa/sherpa-onnx). The files
shipped (`en_US-ljspeech-medium.onnx`, `en_US-ljspeech-medium.onnx.json`,
`tokens.txt`, `MODEL_CARD`) are unmodified; the archive's `espeak-ng-data/`
isn't used and doesn't ship.

**The lexicon: `lexicon-en-us.txt`.** sherpa-onnx's English pronunciation
lexicon (release `tts-models` of https://github.com/k2-fsa/sherpa-onnx),
unmodified. Its header says it was built from CMUdict (BSD-2-Clause,
Carnegie Mellon University) and espeak-ng's English word list
(https://github.com/espeak-ng/espeak-ng, GPL-3.0-or-later); it is used here
under the GPL, version 3, like MoltBrowser.

**The letter-to-sound rules** for words the lexicon lacks are MoltBrowser's
own code (`voice_lts.cc`), written in the style of the Naval Research
Laboratory's rules (Elovitz, Johnson, McHugh and Shore, NRL Report 7948,
1976, a work of the US government).
