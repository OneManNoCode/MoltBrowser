# molt-speech: licences and attribution

`molt-speech` is MoltBrowser's speech helper for touchless browsing. It is
part of MoltBrowser (GPLv3, GenEye AI Labs Inc.); its source is in
MoltBrowser's repository under `src/chrome/browser/molt_ai/voice/speech_helper/`,
and `scripts/bundle-speech.sh` builds it, with the libraries below, from
pinned sources.

## Libraries linked into molt-speech

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

## Models in `models/`

**Recognition: Kroko EN streaming model (2025-08-06), `models/asr/`.**
"Kroko ASR" English community model by Banafo (https://kroko.ai), licensed
under the Creative Commons Attribution-ShareAlike 4.0 International licence
(CC BY-SA 4.0, https://creativecommons.org/licenses/by-sa/4.0/). Source:
https://huggingface.co/Banafo/Kroko-ASR, in the ONNX form the k2-fsa
project publishes for sherpa-onnx
(`sherpa-onnx-streaming-zipformer-en-kroko-2025-08-06.tar.bz2`, release
`asr-models` of https://github.com/k2-fsa/sherpa-onnx). The model files
(`encoder.onnx`, `decoder.onnx`, `joiner.onnx`, `tokens.txt`, `README.md`)
are unmodified. `bpe.vocab` is derived from `tokens.txt` by MoltBrowser
(each token with a score, for splitting names into the model's word
pieces) and is shared under the same licence, CC BY-SA 4.0.

**Wake phrase: GigaSpeech Zipformer keyword spotter, 3.3M parameters
(2024-01-01), `models/kws/`.** By the k2-fsa / icefall project, trained on
GigaSpeech XL; licence Apache-2.0 as its model card (`README.md`, shipped)
states. Source: `sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01.tar.bz2`,
release `kws-models` of https://github.com/k2-fsa/sherpa-onnx. The model
files are unmodified; `pieces.vocab` is derived from its `bpe.model` and
`tokens.txt` by MoltBrowser.

**End of speech: Silero VAD v4, `models/vad/silero_vad.onnx`.** By the
Silero team (https://github.com/snakers4/silero-vad), as published in
release `asr-models` of https://github.com/k2-fsa/sherpa-onnx. Unmodified.
Its licence:

```
MIT License

Copyright (c) 2020-present Silero Team

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The licences of sherpa-onnx's own dependencies are next to this file, as
`<dependency>-<file>` (copied from their pinned sources).
