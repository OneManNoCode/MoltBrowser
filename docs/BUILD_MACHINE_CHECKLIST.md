# Build machine checklist: from a fresh sync to a signed v0.3.0 draft

One ordered runbook for everything that needs the build machine before
v0.3.0 ships: the compile, the unit tests, the fuzzers, the device and
manual checks, the MoltNet leak kit, packaging and the offline signing.

**Why it exists.** The last release is v0.2.5 (macOS arm64 and Linux x64;
Windows is still at v0.2.1). Since then the October v0.3 work landed: the
workstream merges and pull requests #2 to #55, including the rebase onto
Chromium 152 (#11), and then the rebase onto Chromium 156.0.8078.48 (#64),
when Chrome's Extended Stable channel moved to M156, and its first
security respin, 156.0.8078.74 (round 31; "(from the .74 respin)" below).
**None of it has been compiled against Chromium 156.**
Each PR body and design doc lists what it still needs; this page collects
those lists in the order you do them. (#31 changed only CI, and #42 is an
open Dependabot PR, §11.)

**Written** 2026-10-23 at `main` = PR #55 (`4a08afd`). A new PR that needs
the build machine adds its items under the right phase, with its tag.

## How to use it

- Work top to bottom. Each phase assumes the ones before it.
- `[ ]` items are things to do or check. *Expected* says what passing
  looks like.
- "(from #N)" names the pull request the item came from, so a failure maps
  to the change that caused it. Other sources are named the same way, e.g.
  "(from CLAUDE.md)".
- "(verify the command)" marks a command that could not be checked against
  a real Chromium 156 tree. The intent is right; the exact flags may not be.
- Commands run from the repository root (`$REPO`, e.g.
  `/Users/raj/Desktop/MoltBrowser/repo`) unless a step says `cd chromium/src`.
- Keep the logs in one place, e.g. `~/molt-v0.3.0/`, and send back the build
  log's `error:` lines, the test output, and the leak kit's `report.json`.
- A fix found here goes in through a pull request (`main` is protected),
  with `tools/chromium/overlay_to_patches.py` and `verify_patches.py` run if
  it touches an upstream file (from CLAUDE.md).
- The detail behind the items: [REBASING.md](REBASING.md),
  [SIGNED_UPDATES.md](SIGNED_UPDATES.md), [LEAK_TESTING.md](LEAK_TESTING.md),
  [MOLTNET_COMPLIANCE.md](MOLTNET_COMPLIANCE.md) §4,
  [NETWORK_TRAFFIC.md](NETWORK_TRAFFIC.md), [FUZZING.md](FUZZING.md),
  [FINGERPRINTING.md](FINGERPRINTING.md), and the design docs
  ([SANDBOXED_INFERENCE](design/SANDBOXED_INFERENCE.md),
  [WEB_AI_APIS](design/WEB_AI_APIS.md),
  [PAGE_TRANSLATION](design/PAGE_TRANSLATION.md),
  [AGENT_ACTIONS](design/AGENT_ACTIONS.md),
  [SEMANTIC_SEARCH](design/SEMANTIC_SEARCH.md)).

**Platforms.** macOS arm64 (native) and Linux x64 (Docker on the Mac, or a
native Linux box). Windows builds are paused (CLAUDE.md, 2026-07-03); its
items are marked *deferred*.

> **Out dirs.** macOS builds in `chromium/src/out/MoltBrowser`, Linux in
> `chromium/src/out/MoltBrowser-linux` (`scripts/configure.sh` picks it for a
> Linux target, `scripts/build.sh --platform linux` and
> `scripts/package-linux.sh` use it). So the Docker Linux build no longer
> throws away the macOS build in the same checkout: the phases below can
> interleave, given the disk for both (§0).

### The phases

| § | Phase | Where | Rough time |
|---|---|---|---|
| 0 | Before you start | Mac | 30 min |
| 1 | Sync to the pin, vendor the Rust crates | Mac | 1-2 h |
| 2 | Pins and keys the owner must do first | Mac + offline machine | 2-3 h |
| 3 | Configure and build | Mac, Linux | a full build each (hours) |
| 4 | Compile-risk hot spots (read when the build fails) | | |
| 5 | Unit tests | Mac, Linux | hours |
| 6 | Fuzzers | Mac or Linux (GN); Linux (standalone) | 3-6 h |
| 7 | Device and manual checks | Mac, Linux | a day |
| 8 | Leak kit and traffic capture | Linux (kit), Mac (capture) | half a day |
| 9 | Vulkan hardware matrix (not needed for v0.3.0) | Linux GPU machines | |
| 10 | Packaging, signing, draft release | Mac, Linux, offline machine | a day |
| 11 | The owner's open decisions | | |

### First build: the order in one place

The first build against Chromium 156 in the order to run it. The steps
point to the sections below for the detail; this list is what to do and
what to send back.

1. **Static checks, before the sync** (any Linux machine, no checkout,
   minutes). They read 156's own build files with the overlay on top:
   ```bash
   python3 tools/chromium/gn_static_check.py fetch      # once per pin: streams 156's tarball (5 min), keeps ~190 MB
   python3 tools/chromium/gn_static_check.py check --warnings 2>&1 | tee ~/molt-v0.3.0/static-gn-check.log
   python3 tools/chromium/gn_static_check.py checkdeps     2>&1 | tee ~/molt-v0.3.0/static-checkdeps.log
   python3 tools/chromium/gn_static_check.py blink-audit   2>&1 | tee ~/molt-v0.3.0/static-blink-audit.log
   python3 tools/compile_check/compile_check.py all && python3 tools/compile_check/compile_check.py report
   ```
   *Expected:* `check` finds no error in any of its six configs (Linux,
   macOS, Windows, Linux and Windows with `molt_llama_vulkan`, a Linux
   component build) and warns only about `//third_party/rust/adblock/v0_13:lib`
   (step 2 vendors it); `checkdeps` and `blink-audit` report 0 errors;
   compile_check's `report` lists no errors (§4). The three
   `gn_static_check.py` checks exit 1 on an error. What they model and what
   they can't: the header of
   `tools/chromium/gn_static_check.py`, and §4.9.
2. **Sync, pins and the version** (§1, §2): `setup.sh --resync`, the patch
   series check, the search engines and en-US tables, the Rust crates
   (gnrt), then the pins and the version bump in one PR before the first
   full build (§2.6: `webui_url_constants.h` costs a ~2 h recompile).
3. **Configure, then let GN check the real graph** (minutes; §3, §4.9):
   ```bash
   ./scripts/configure.sh --platform macos --arch arm64 2>&1 | tee ~/molt-v0.3.0/configure-mac.log   # runs gn gen
   (cd chromium/src && gn check out/MoltBrowser) 2>&1 | tee ~/molt-v0.3.0/gn-check.log
   ./scripts/vendor-rust-crates.sh --check 2>&1 | tee ~/molt-v0.3.0/vendor-rust-check.log
   ```
   `gn gen` refuses an unknown label, a visibility or `testonly` violation
   and a dependency cycle; `gn check` the include rule. Then checkdeps over
   the real tree (§4.9).
4. **Build the small targets first** (`-k 0` so one run lists every broken
   file; read the whole log, `autoninja` can exit 0 on a failure):
   ```bash
   cd chromium/src
   B=out/MoltBrowser    # out/MoltBrowser-linux on Linux
   for t in third_party/llvm-libc:molt_portable_math ui/gfx/geometry:geometry \
            third_party/llama_cpp:llama_cpp chrome/services/molt_ai/inference:inference \
            net:net services/network:network_service storage/browser:browser \
            components/services/storage:storage content:content \
            third_party/blink/renderer/core:core third_party/blink/renderer/modules:modules \
            molt_ai_unittests chrome; do
     autoninja -C $B -k 0 $t 2>&1 | tee ~/molt-v0.3.0/build-${t//[\/:]/_}.log
   done
   grep -lE 'error:|FAILED:|finished with an error' ~/molt-v0.3.0/build-*.log
   ```
   (verify the command; on Linux, run it on a native box or inside the
   Docker image of §3.2.) In order: the portable math (minutes), llama.cpp
   and the sandboxed inference service, `//net` and the network service
   with MoltNet's hooks, the storage service, `//content`, Blink core and
   modules (hours), MoltBrowser's own unit tests (the first link of most of
   MoltBrowser's browser code), then `chrome`. With `-k 0` a failing step
   still compiles everything else it can: go on to the next target rather
   than stop at the first failure, and send all the logs. Then §3's own
   build command for the bundled helpers, and §5's tests.
5. **Send back:** every log above, or for a long one the lines
   `grep -nE 'error:|FAILED:|finished with an error|undefined symbol|duplicate symbol'`
   prints and 30 lines after each `FAILED:`; `gn-check.log` and the
   checkdeps output whole; `gn args --list --short --overrides-only` for
   each out dir (§3.1); the four static-check logs if any of them was not
   clean.

**Known risky files, each once** (the rows hold the detail):

| Risk | Where |
|---|---|
| New mojom fields and methods: the storage controls' `MoltKeepCachesPerSite()` and `MoltSetGrowthObserver()` need an override in every fake or mock of them | [§4.1](#41-mojo-new-fields-unions-and-interfaces) |
| Widely included `//net` headers (`connect_job.h`, `client_socket_pool.h`, `transport_client_socket_pool.h`, `ssl_client_socket.h`, `http_network_session.h`): long rebuilds | [§4.2](#42-net-and-the-network-service-moltnet-circuits) |
| `chrome/browser/ui/autofill/chrome_autofill_client.cc`: only two functions type-checked | [§4.2](#42-net-and-the-network-service-moltnet-circuits) |
| `third_party/llama_cpp:ggml_vulkan` (with the switch on): built in a stub GN root only | [§4.6](#46-sandboxes-and-utility-processes) |
| Blink's font hooks on macOS and Windows (`font_platform_data_*`, `font_cache_*`, `font_metrics.cc`'s Apple branch, `skia_text_metrics.cc`): never compiled | [§4.7](#47-blink-and-fingerprinting-moltnet-uniformity) |
| The cxx bridges that must be regenerated, not taken from an old out dir (`rustfft_ffi.rs`, Fontations' `ffi.rs`, the exif `FFI.rs`) | [§4.7](#47-blink-and-fingerprinting-moltnet-uniformity) |
| `third_party/skia/include/core/SkScalar.h`: the math macros call an exported function now | [§4.7](#47-blink-and-fingerprinting-moltnet-uniformity) |
| `chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler.cc`: not type-checked at all | [§4.8](#48-webui-handlers-and-the-agents) |
| A component build (developer builds only): a symbol used across libraries without its export macro | [§4.9](#49-gn-targets-and-deps-edges) |
| Windows-only hooks (deferred) | [§3.3](#33-windows-deferred) |
| The vendored adblock crates (gnrt) | [§1](#1-sync) |

---

## 0. Before you start

- [ ] Disk: at least 120 GB free for the checkout and one out dir; each
  extra out dir (the Linux build's `out/MoltBrowser-linux` in the same
  checkout, §5 tests, §6 fuzzers) needs tens of GB more (from
  `build/docker/build-linux-fresh.sh`, `scripts/setup.sh`). Building with
  the Mac's release runner (§10.1, route A) needs room for its own checkout
  as well.
- [ ] `main` is current and clean, and there is a log folder (from CLAUDE.md
  "Git Workflow"):
  ```bash
  git switch main && git pull --ff-only && git status --short   # prints nothing
  git log -1 --oneline                                          # note the commit
  mkdir -p ~/molt-v0.3.0
  ```
- [ ] Toolchain on the Mac (from CLAUDE.md, BUILD_PROGRESS.md):
  - use depot_tools' `python3`, not Homebrew's:
    `export PATH="$PWD/depot_tools:/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:$PATH"`;
  - the Metal toolchain: `xcodebuild -downloadComponent MetalToolchain`;
  - `cmake` (for `scripts/bundle-whisper.sh`), Homebrew's `tesseract`
    (`scripts/bundle-tesseract.sh` copies it and its dylibs into the app;
    without it OCR is unavailable, from #17), Docker Desktop (Linux build),
    `gh` logged in with `repo` and `workflow` scopes.
- [ ] The builder label is set in the build account's shell profile (§2.5):
  `echo "$MOLT_BUILDER_ID"` prints e.g. `geneye-mac-1` (from #43).

## 1. Sync

- [ ] `.gclient` lists both target OSes (from CLAUDE.md):
  ```bash
  grep target_os chromium/.gclient    # Expected: target_os = ['mac', 'linux']
  ```
  `setup.sh --target-os` writes it only when it creates `.gclient`, and
  `build/docker/build-linux.sh` appends `['linux']` only when the line is
  missing. Fix the line by hand if it lists one OS.
- [ ] Move the checkout to the pin and apply the overlay (from #11,
  REBASING.md "Still to do on the build machine"):
  ```bash
  ./scripts/setup.sh --resync --target-os mac,linux 2>&1 | tee ~/molt-v0.3.0/setup.log
  ```
  *Expected:* `chromium/src @ 156.0.8078.74 (d331de35ca8ab1358c7a17e4bd41627d55eaef51)`,
  then `gclient sync`, hooks and `Overlay applied.` It resets `chromium/src`
  (`reset --hard`, `clean -fd`; the gitignored `out/` survives), so the
  first run from an older pin means a full rebuild. A checkout already at
  156.0.8078.48 needs this run too (from the .74 respin): the respin moves
  V8 (15.6.75.25), Skia, WebRTC, ANGLE, Dawn, devtools-frontend, ICU,
  libaom, libvpx and PDFium, so `gclient sync` fetches all of them and
  most of the build recompiles. No Rust crate changed: gnrt needs no rerun
  for it.
- [ ] The checkout is at the pin (from #11, `chromium_version.txt`):
  ```bash
  [ "$(git -C chromium/src rev-parse HEAD)" = "$(tools/chromium/pin.sh revision)" ] && echo pin-ok
  ```
- [ ] The overlay and the patch series agree (CI runs the same check):
  `python3 tools/chromium/verify_patches.py` → `OK: patches/ reproduces the
  overlay exactly`. The build input is still the `src/` overlay copy;
  `patches/` holds the same changes as diffs (from REBASING.md). It also
  fails while one of the 66 whole files `src/` keeps in DEPS subprojects
  (V8, Skia, WebRTC, the search engines) is behind the pin's subproject
  commit (from the .74 respin): *Expected:* `DEPS whole: 66 of 66 merged
  with the pin's subproject commit`.
  - Optional, once: try the patch-based path, `python3 tools/chromium/apply_patches.py --reset`
    (restores the patched files to upstream@pin, applies `patches/series`
    all or nothing, copies the first-party files). REBASING.md "Moving fully
    to the patch series" says when it replaces the overlay.
- [ ] The search engines file, a DEPS subproject the patch series can't
  diff (from #11, `patches/whole_files.txt`). `setup.sh` has already copied
  our version over the synced one, so compare against the subproject's own
  git:
  ```bash
  git -C chromium/src/third_party/search_engines_data/resources \
    show HEAD:definitions/prepopulated_engines.json > /tmp/prepopulated_engines.upstream.json
  python3 tools/chromium/search_engines_overlay.py /tmp/prepopulated_engines.upstream.json \
    --check src/third_party/search_engines_data/resources/definitions/prepopulated_engines.json \
    && echo engines-ok
  ```
  *Expected:* `engines-ok`. If not, regenerate with `-o <that src/ path>`
  instead of `--check`, commit it in a PR, and copy it into `chromium/src`.
  Our copy was rebuilt from 156's `search_engines_data`
  (`5dc9f1afd38a8ac2b26b1972eb4fc1bbd5754e8c`, which 156.0.8078.74 still
  pins; `git -C chromium/src/third_party/search_engines_data/resources
  rev-parse HEAD`) from the release's source tarball (from the .74
  respin), so expect `engines-ok`; before, it was the 148-era list.
- [ ] MoltNet's fixed en-US tables match the 156 `.grd` files (from #24,
  REBASING.md step 5):
  ```bash
  python3 tools/moltnet_locale/generate_tables.py --grd-root chromium/src
  ```
  *Expected:* exit 0. Exit 1: rerun with `--write` and commit. Exit 2: a
  new Blink string needs a decision (docs/FINGERPRINTING.md, "MoltNet: the
  browser's UI language").
- [ ] Vendor the Rust crates with gnrt (from #11, MoltShield on adblock-rust).
  The adblock crates are **not** in `src/` yet, so the shield targets don't
  build without this:
  ```bash
  ./scripts/vendor-rust-crates.sh 2>&1 | tee ~/molt-v0.3.0/vendor-rust.log
  git status --short src/third_party/rust | head
  ```
  *Expected:* about 15 new crates under
  `src/third_party/rust/chromium_crates_io/vendor/` (adblock, arrayvec 0.7,
  flatbuffers, seahash, url, idna, ...) and regenerated `BUILD.gn` files,
  per the script's header. Review, then commit the result in a PR: the
  release build provenance records the `src/third_party/rust` tree, so it
  must be committed before the tag. After §3's `configure.sh`, run it again
  with `--check` (gn check plus a build of the shield and anydoc targets).
  The vendoring keeps symphonia-core's portable FFT (fp-cpu): afterwards
  `git diff src/third_party/rust/chromium_crates_io/vendor/symphonia-core-v0_6/src/dsp/fft/simd.rs`
  is empty (gnrt applied `patches/symphonia-core-v0_6/0001-Add-a-portable-FFT-mode.patch`,
  which the script copies with the other crate patches); if gnrt reports
  that the patch doesn't apply, the crate moved: redo the change on the new
  version and regenerate the patch.
  The same holds for read-fonts' and skrifa's portable glyph-transform
  math (fp-geometry): afterwards `git diff` of
  `vendor/read-fonts-v0_43/` and `vendor/skrifa-v0_46/` is empty
  (`patches/read-fonts-v0_43/0001-Add-portable-glyph-transform-math.patch`,
  `patches/skrifa-v0_46/0001-Use-portable-glyph-transform-math.patch`), and
  gnrt's regenerated `third_party/rust/read_fonts/v0_43/BUILD.gn` still
  lists `src/moltbrowser_math.rs` and depends on
  `//third_party/rust/libm/v0_2:lib` (the patch adds `libm` to read-fonts'
  `Cargo.toml`; `Cargo.lock` lists it under read-fonts). If gnrt drops the
  source or the dependency, put them back by hand.

## 2. Pins and keys the owner must do first

These go in before the release build: the build compiles them in, release
builds refuse to bundle unpinned downloads (`MOLT_RELEASE_BUILD=1`), and
the SBOM refuses a shipped file with no pin (from #25). Put the code
changes in one PR, together with the version bump (§2.6), so the first
full build already has them.

### 2.1 The release signing key (offline)

Without a key in the build, v0.3.0 never installs an update
(`update_signing_keys.cc` holds `""` today). From docs/SIGNED_UPDATES.md,
"One-time: create the release key":

- [ ] On the offline machine (from SIGNED_UPDATES.md):
  `python3 tools/release_signing/molt_release_sign.py keygen --out /Volumes/MOLTKEY/molt-update-signing.key`.
  Strong passphrase in the password manager; back the key file up to a
  second encrypted drive. The key never touches CI, a cloud machine or
  this repository.
- [ ] On a dev machine, with the **public** key it printed (from
  SIGNED_UPDATES.md):
  `python3 tools/release_signing/molt_release_sign.py add-key <PUBLIC KEY>`.
  *Recorded in* `src/chrome/browser/molt_ai/update/update_signing_keys.cc`.
- [ ] Optional now, needed before the apt/rpm repository goes live: the
  Linux archive key (SIGNED_UPDATES.md, "One-time: create the Linux archive
  key"), committed as `installer/linux/moltbrowser-archive-keyring.asc`
  (from #25). Without it the repository is simply not published (§11).

### 2.2 Tor 15.0.15 hashes

Release builds fail at `scripts/bundle-tor.sh` until these are filled in
(MOLTNET_COMPLIANCE.md §5.7). The bump to 15.0.15 came with the MoltNet
P1-P15 work, when the Tor archive was unreachable. On a trusted machine
with gpg, from the script's own instructions:

- [ ] Verify Tor's signed checksum file (from the MoltNet P1-P15 work):
  ```bash
  V=15.0.15
  B=https://archive.torproject.org/tor-package-archive/torbrowser/$V
  curl -fLO $B/sha256sums-signed-build.txt
  curl -fLO $B/sha256sums-signed-build.txt.asc
  gpg --auto-key-locate nodefault,wkd --locate-keys torbrowser@torproject.org
  gpg --verify sha256sums-signed-build.txt.asc sha256sums-signed-build.txt   # Good signature
  grep -E "tor-expert-bundle-(macos-aarch64|macos-x86_64|windows-x86_64)-$V.tar.gz" sha256sums-signed-build.txt
  ```
- [ ] Cross-check one digest against an independent download (from
  `scripts/bundle-tor.sh`):
  `curl -fLO $B/tor-expert-bundle-macos-aarch64-$V.tar.gz && shasum -a 256 tor-expert-bundle-macos-aarch64-$V.tar.gz`.
- [ ] Write them down (from the MoltNet P1-P15 work).
  *Recorded in* `scripts/bundle-tor.sh`: `EXPECTED_SHA256_TOR_AARCH64`
  and `EXPECTED_SHA256_TOR_X86_64`. Also the Windows digest in
  `.github/workflows/release-windows-selfhosted.yml`,
  `$pins["tor-15.0.15"]` (deferred while Windows is paused, but cheap now).
  Linux packages use the distribution's Tor and pin nothing.

### 2.3 whisper's model

- [ ] `ggml-tiny.en.bin` (from the release-integrity work of 2026-10-14,
  `scripts/bundle-whisper.sh`):
  ```bash
  curl -fL -o /tmp/ggml-tiny.en.bin https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin
  shasum -a 1 /tmp/ggml-tiny.en.bin     # must be c78c86eb1a8faa21b369bcd33207cc90d64ae9df
  shasum -a 256 /tmp/ggml-tiny.en.bin   # must equal the LFS SHA256 on the file's huggingface.co page
  ```
  *Recorded in* `scripts/bundle-whisper.sh` (`EXPECTED_SHA256_WHISPER_TINY_EN`)
  and `release-windows-selfhosted.yml` (`"ggml-tiny.en.bin"`). The
  whisper.cpp commit (v1.7.5) and tesseract's `eng.traineddata` are already
  pinned.

### 2.4 The models (`tools/models/pin_models.py`)

The memory search model and the vision model are never offered until
pinned (fail closed); chat models without a pin download as "unverified".
Never type a hash by hand (docs/design/SEMANTIC_SEARCH.md §6).

- [ ] Embedding model (from #28):
  ```bash
  python3 tools/models/pin_models.py --only qwen3-embedding-0.6b           # dry run
  python3 tools/models/pin_models.py --only qwen3-embedding-0.6b --write
  ```
  Then SEMANTIC_SEARCH.md §6 steps 3-5: download from the pinned
  `/resolve/<revision>/<file>` URL, check SHA-256 and size, and
  `gguf-dump --no-tensors` shows `general.architecture = qwen3` and
  `qwen3.embedding_length = 1024`; re-read the licence (Apache 2.0).
- [ ] Vision model and its projector, pinned together (from #38, #40):
  ```bash
  python3 tools/models/pin_models.py --only qwen3-vl-2b                    # dry run: model + mmproj
  python3 tools/models/pin_models.py --only qwen3-vl-2b --write
  ```
  Check both files independently the same way. The download flow fetches
  the pair as one unit since #40.
- [ ] Audio model and its projector, pinned together (from ai-finish;
  SANDBOXED_INFERENCE.md §13.4):
  ```bash
  python3 tools/models/pin_models.py --only voxtral-mini-3b                # dry run: model + mmproj
  python3 tools/models/pin_models.py --only voxtral-mini-3b --write
  ```
  Check both files independently as above; the entry stays hidden
  (`true` as its last field) until the audio check of §7.3 passes with
  these files, then flip it to `false` by hand in the same PR as the pins.
- [ ] Every pinned model's files have only weight types the Vulkan build
  keeps (from ai-finish; SANDBOXED_INFERENCE.md §11.14):
  `python3 tools/models/gguf_types.py <model>.gguf [<mmproj>.gguf]` exits 0
  for each. A failure names the type: take it out of
  `_ggml_vk_dropped_types` in `src/third_party/llama_cpp/BUILD.gn`, or the
  model's operations on it run on the CPU with the GPU switch on.
- [ ] Optional: the chat models, `python3 tools/models/pin_models.py` (dry
  run, then `--write`); hidden entries (`qwen3.5-4b`, `gemma4-e4b`) stay
  hidden. Set `HF_TOKEN` for gated repositories (from
  `tools/models/pin_models.py`).
- [ ] `python3 tools/models/pin_models_test.py` passes (fails on a
  half-filled pin) and the `TODO(pin)` comments are gone (from #28, #38).
  *Recorded in* `src/chrome/browser/molt_ai/runtime/browser_ai_runtime.cc`
  (`kModelRegistry`, `kProjectors`) and
  `src/chrome/browser/molt_ai/memory/embedding_model_registry.cc`.

### 2.5 The builder label

- [ ] Pick one per build machine (lower-case letters, digits, `.`, `_`, `-`;
  never the hostname or your user name; published in every release), from
  #43 and SIGNED_UPDATES.md "One-time: label your build machines":
  - release scripts: `export MOLT_BUILDER_ID=geneye-mac-1` in the build
    account's shell profile;
  - each self-hosted runner (the Mac runner, the Linux
    `linux-chromium-builder`): `MOLT_BUILDER_ID=<label>` in the runner's
    `.env`, then restart it.

  *Recorded in* every `molt-provenance-*.json` (`runDetails.builder.id`).
  Keep the label-to-machine list private.

### 2.6 The version

- [ ] Bump `kMoltBrowserVersion` from `"0.2.5"` to `"0.3.0"` in
  `src/chrome/common/webui_url_constants.h`, then
  `python3 tools/chromium/overlay_to_patches.py && python3 tools/chromium/verify_patches.py`.
  This file fans out a ~2 h recompile (from CLAUDE.md), so do it in this PR,
  before the first full build, not after the tests.
- [ ] Merge the pins-and-version PR; `git pull --ff-only`; `./scripts/setup.sh`
  copies the overlay again (from CLAUDE.md "Git Workflow").

## 3. Configure and build

The first time, follow the order of "First build" at the top: the static
checks, then configure and `gn check`, the small targets, and `chrome`
last. The steps below are the per-platform detail.

### 3.1 macOS arm64

- [ ] Configure from `configs/macos-arm64.gn` (official build,
  `safe_browsing_mode = 1`, `enable_supervised_users = true`, no Google
  keys):
  ```bash
  ./scripts/configure.sh --platform macos --arch arm64
  (cd chromium/src && gn args out/MoltBrowser --list --short --overrides-only) | tee ~/molt-v0.3.0/gn-args-mac.txt
  ```
  *Expected:* `molt_llama_vulkan` is **not** among the overrides (it stays
  `false`; macOS uses Metal and refuses it) (from #52).
- [ ] `./scripts/vendor-rust-crates.sh --check` (from #11): *Expected:* gn
  check and the adblock and anydoc targets build. At 156 (from #64) gnrt is
  the authority over the hand-merged `Cargo.lock` (156's crate set plus
  anydoc's 77 packages): it should add only the adblock tree. Check that
  winapi-util's windows-sys moves to 0.61, that the regex and version_check
  BUILD.gn files stay non-testonly, and that the overlay notice lists
  `unicode-bidi-v0_3` (ours wins over 156's empty placeholder: lopdf needs
  the real crate).
- [ ] Build and read the whole log, because `autoninja` can exit 0 on a
  failed compile (from CLAUDE.md):
  ```bash
  autoninja -C chromium/src/out/MoltBrowser -k 0 chrome 2>&1 | tee ~/molt-v0.3.0/build-mac.log
  grep -nE 'error:|FAILED|finished with an error' ~/molt-v0.3.0/build-mac.log
  ```
  *Expected:* no match. `-k 0` keeps going past failures, so one run lists
  every broken file. On errors, §4 maps each file to its PR.
- [ ] Day-to-day install loop: `./scripts/dev-install-mac.sh` (stages the
  app, adds `default.metallib`, signs inside-out, runs a headless renderer
  gate, installs to `/Applications`). *Expected:* `signature valid,
  allow-jit intact` and `gate passed`. It keeps the `tor`, `ocr` and
  `whisper` the out-dir app already has, and copies only missing ones (and
  `molt_models`) from the installed app. Plain `autoninja` bundles none, so
  to test freshly bundled helpers (§7.1, from #16 and #17), build with
  `./scripts/build.sh`, which bundles them after the compile.

### 3.2 Linux x64

Linux builds in its own out dir, `out/MoltBrowser-linux`, so this can run
next to the macOS build in the same checkout, or on a native Linux
machine. Docker on an Apple Silicon Mac runs under Rosetta and took 24-40 h
per attempt; a native x86_64 Linux box takes about 4 h (from
`.github/workflows/release-linux.yml`).

- [ ] Docker on the Mac, same checkout (from CLAUDE.md "Linux (via Docker
  from Mac)"):
  `./build/docker/build-linux.sh 2>&1 | tee ~/molt-v0.3.0/build-linux.log`
  (syncs the Linux toolchains if `buildtools/linux64/gn` is missing, then
  `configure.sh --platform linux --arch x64` and
  `build.sh --platform linux --jobs 4`).
  Or, on a fresh Linux machine: `./build/docker/build-linux-fresh.sh`
  (with `--package`, the packages take `kMoltBrowserVersion`'s version, 0.3.0
  after §2.6; `--version X.Y.Z` overrides it).
- [ ] `grep -nE 'error:|FAILED|finished with an error' ~/molt-v0.3.0/build-linux.log`
  → no match. The binary is `chromium/src/out/MoltBrowser-linux/chrome`
  (from CLAUDE.md "Build gotchas").
- [ ] `molt_llama_vulkan` stays off: `configs/linux-x64.gn` only mentions it
  in a comment. Turning it on is §9 (from #52).

### 3.3 Windows (deferred)

Paused (CLAUDE.md). Windows-only items: `tor_control_connection.cc` on
Windows (from #49), the LPAC process start from the NSIS install (from
#15), the OCR and voice helpers' AppContainer, job and installer grant
(helpers-win, which replaced #16's job object; checks in §7.5), PDFium under win32k
lockdown (from #18), the Vulkan pre-lockdown step (from #52; built by
ai-finish, its test plan in §9), Windows signing (§11). Once a runner exists again: give it `MOLT_BUILDER_ID` in its
`.env`, wipe its workspace once (the workflow's steps were reordered), and
dispatch it from the tag with `--ref vX.Y.Z -f tag=vX.Y.Z` (from #43).

## 4. Compile-risk hot spots

When the build fails, find the file here; the PR tag says which change to
read. Upstream files are whole copies under `src/`, each hook marked
`// MoltBrowser:`; `patches/<path>.patch` shows exactly what changed.
Where a row below says a header is "in no target's `sources`", or to add a
dep "if `gn check` complains", that predates round 28: the headers are
listed and the deps are in (§4.9).

- [ ] Optional, on any Linux machine before the slow build:
  `tools/compile_check/compile_check.py all` type-checks every C, C++ and
  Objective-C++ file MoltBrowser adds or changes (and the changed headers
  no source includes) against the real 156 headers with Chromium's clang
  and its plugins, on each platform that builds it: Linux x64 and arm64,
  Windows x64 (clang-cl against the Windows SDK), macOS arm64 (Darwin's
  headers, Apple framework stand-ins) and Linux with `molt_llama_vulkan`
  on (no checkout needed; see its README for what it can't cover).
  *Expected:* `report` shows no ERRORS and no PLUGIN rows (at 156, round 30:
  896 files in 1437 platform rows, 1384 checked, 40 partly on stand-ins, the
  Mac files' Apple frameworks among them, 1 not checked: an iOS file that
  no BUILD.gn lists, and 12 PLUGIN: `[blink-style]` on the `std::vector`
  data members of `molt_fingerprinting/moltnet_audio_clock.h` and
  `moltnet_fonts.cc`, to become `blink::Vector`s or be annotated
  `ALLOW_DISCOURAGED_TYPE`).

### 4.1 Mojo: new fields, unions and interfaces

| File | What to watch | From |
|---|---|---|
| `services/network/public/mojom/network_context.mojom` | `[MinVersion=1] bool molt_basic_and_digest_auth_only` (`HttpAuthStaticNetworkContextParams`), `[MinVersion=1] string? molt_socks_isolation_password` and the four `[MinVersion=1] bool molt_uncached_cert_verification`, `molt_no_shared_cache_entries`, `molt_hsts_top_level_navigations_only`, `molt_no_network_quality_estimator` (`NetworkContextParams`): last in their structs, so positional constructors still compile | #51, #55, #62, network quality |
| `services/cert_verifier/public/mojom/cert_verifier_service_factory.mojom` | `[MinVersion=1] bool molt_isolate_each_cert_fetch` | #51 |
| `services/network/public/mojom/cert_verifier_service.mojom` (new upstream copy), `services/network/public/cpp/cert_verifier/cert_verifier_mojom_traits.{h,cc}` (new upstream copies) | `[MinVersion=1] string? molt_cert_fetch_site`, last in `RequestParams`; the traits' new getter returns `std::optional<std::string>` (null for none) and `Read()` calls `ReadMoltCertFetchSite()` | moltnet-shared2 |
| `third_party/blink/public/mojom/worker/shared_worker_info.mojom`, `.../service_worker/service_worker.mojom` | `[MinVersion=1] bool molt_shields_down`; `ServiceWorker.InitializeGlobalScope` change | #24 |
| `chrome/services/molt_ai/inference/public/mojom/molt_inference.mojom` | the generated union API (`GenerationParams.constraint`), `LoadEmbeddingModel`/`EmbeddingModel.Embed`, the projector handle, GPU fields | #15, #28, #30, #38, #52 |
| `chrome/services/molt_ai/document_parser/public/mojom/document_parser.mojom` | module constants, `kServiceSandbox` | #13, #18 |
| `chrome/common/molt_ai/fingerprinting/molt_fingerprinting.mojom` | the farbling seed over mojo | #10 |
| `services/network/public/mojom/network_context.mojom` | `[MinVersion=1] bool molt_per_site_http_cache` and `[MinVersion=1] bool molt_stop_all_when_refusing`, set for MoltNet by `molt_net_network_context_policy.h` | moltnet-shared3 |
| `components/services/storage/public/mojom/local_storage_control.mojom`, `session_storage_control.mojom` (new upstream copies) | a new method `MoltKeepCachesPerSite()` on both (not `[Stable]` interfaces, so no `[MinVersion]`): every C++ implementation needs it. `LocalStorageImpl` and `SessionStorageImpl` have it; a test fake or mock of either control elsewhere in the tree (none in the 156 files read) fails to compile until it gets an empty override | moltnet-shared3 |
| `components/services/storage/public/mojom/local_storage_control.mojom`, `session_storage_control.mojom` | a new interface `MoltDomStorageGrowthObserver` (`OnGrowthBeyondFloor()`) in `local_storage_control.mojom`, which `session_storage_control.mojom` now imports, and a new method `MoltSetGrowthObserver(pending_remote<MoltDomStorageGrowthObserver>)` on both controls: as for `MoltKeepCachesPerSite()`, a test fake or mock of either control fails to compile until it gets an empty override | moltnet-shared4 |
| `components/services/storage/public/mojom/storage_service.mojom` (new upstream copy) | two new methods on `StorageService`, `MoltBindPerSiteLocalStorageControl(pending_receiver<LocalStorageControl>)` and `MoltBindPerSiteSessionStorageControl(pending_receiver<SessionStorageControl>)` (not `[Stable]`, no `[MinVersion]`; the interface keeps its `[ServiceSandbox]`): `StorageServiceImpl` is its only implementation in the 156 tree; a fake elsewhere would need empty overrides | moltnet-shared7 |
| `components/services/storage/public/mojom/local_storage_control.mojom`, `session_storage_control.mojom` | a new method `MoltDropNonce(mojo_base.mojom.UnguessableToken nonce, bool closed)` on both controls (both now import `mojo/public/mojom/base/unguessable_token.mojom`): as for `MoltKeepCachesPerSite()`, a test fake or mock of either control fails to compile until it gets an empty override; `LocalStorageImpl`, `SessionStorageImpl` and the own-thread hosts have it | moltnet-shared8 |
| `components/services/storage/public/mojom/local_storage_control.mojom`, `session_storage_control.mojom` | a new method `MoltGetUsageForStorageKey(blink.mojom.StorageKey storage_key)` on both controls, answering `array<StorageUsageInfo>` (local) and `array<SessionStorageUsageInfo>` (session): as for `MoltKeepCachesPerSite()`, a test fake or mock of either control fails to compile until it gets an override; `LocalStorageImpl`, `SessionStorageImpl` and the own-thread hosts have it | moltnet-shared9 |
| `services/network/public/mojom/network_context.mojom`, `services/network/test/test_network_context.h` (new upstream copy) | `[MinVersion=1] bool molt_nonce_limits` (set for MoltNet by `molt_net_network_context_policy.h`), a new `enum MoltNonceDrop { kClosed, kGone, kForget }` and a new method `MoltDropNonce(mojo_base.mojom.UnguessableToken nonce, MoltNonceDrop drop)` on `NetworkContext` (not `[Stable]`, no `[MinVersion]`; the file already imports `unguessable_token.mojom`): every C++ implementation needs it. `network::NetworkContext` overrides it and `TestNetworkContext` has an empty override (the fakes deriving from it inherit that); another fake or mock of `NetworkContext` elsewhere in the tree (none other in the 156 files read) fails to compile until it gets one | moltnet-shared10 |
| `services/network/public/mojom/network_context.mojom` | `[MinVersion=1] array<mojo_base.mojom.UnguessableToken>? molt_refused_nonces` (set for MoltNet by `ProfileNetworkContextService`), `[MinVersion=1] bool molt_per_site_network_caches` and `[MinVersion=1] bool molt_no_quic` (set for MoltNet by `molt_net_network_context_policy.h`), last in `NetworkContextParams` (the file already imports `unguessable_token.mojom`) | moltnet-shared11 |
| `media/mojo/mojom/audio_processing.mojom`, `audio_processing_mojom_traits.{h,cc}`, `media/base/audio_processing.{h,cc}` (new upstream copies) | `bool cpu_independent`, last in `AudioProcessingSettings` (not `[Stable]`, no `[MinVersion]`): the traits get a getter and `Read()` copies it; `operator==` and `ToString()` include it. Designated initialisers of the struct elsewhere (`kDummyWebrtcSettings` in Blink) leave it false | fp-probes2 |

### 4.2 `net/` and the network service (MoltNet circuits)

Expect a large `//net` rebuild: `http_network_session.h` and
`connect_job.h` are widely included (from #55).

| File | Hook | From |
|---|---|---|
| `net/socket/socks_connect_job.cc` | per-site username (also for opaque top-level sites), the per-context password | #49, #55 |
| `net/socket/connect_job.h` | `CommonConnectJobParams` holds a `raw_ptr` to the session's password | #55 |
| `net/http/http_network_session.{h,cc}` | `HttpNetworkSessionParams` carries the password | #55 |
| `services/network/network_context.cc` | copies the password; Basic/Digest-only auth in the constructor and after each browser-wide auth update; no `CachingCertVerifier`/`CoalescingCertVerifier` in `MakeURLRequestContext()`, `SharedResourceChecker::MoltDisableSharing()` in the constructor and `MoltSetUpgradeTopLevelNavigationsOnly()` after `Build()`, and no `set_network_quality_estimator()` in `MakeURLRequestContext()` (so `CreateURLLoaderFactory()` hands the `ResourceSchedulerClient` a null estimator), each only for MoltNet's params | #51, #55, #62, network quality |
| `net/http/transport_security_state.{h,cc}` | a per-instance `kHstsTopLevelNavigationsOnly` in `GetSSLUpgradeDecision()`. `transport_security_state.h` is widely included | #62 |
| `services/network/pervasive_resources/shared_resource_checker.h` | inline `MoltDisableSharing()` | #62 |
| `services/cert_verifier/cert_net_url_loader/cert_net_fetcher_url_loader.{h,cc}`, `services/cert_verifier/cert_verifier_service_factory.cc` | numbered fetches, `IsolationInfo::CreateTransient` in `Job::StartURLLoader` | #51 |
| `chrome/browser/net/profile_network_context_service.cc` | sets the MoltNet params (via `molt_net_network_context_policy.h`); `ShouldSplitAuthCacheByNetworkIsolationKey()` and both `block_trust_tokens` places (creation, `UpdateAntiAbuseSettings()`) ask it too | #21, #51, #55, #62 |
| `chrome/browser/molt_ai/tor/molt_net_routing.cc` | `ProcessWidePartitioningError()` calls `net::NetworkAnonymizationKey::IsPartitioningEnabled()` and `net::HttpCache::IsSplitCacheEnabled()` (`//net`) | #62 |
| `chrome/browser/ssl/chrome_security_blocking_page_factory.cc` | `OpenLoginTab()` returns early for MoltNet | #51 |
| `chrome/browser/profiles/off_the_record_profile_impl.cc` | no zoom from the regular profile | #20, #21, #49 |
| `net/dns/host_resolver_manager.cc`, `net/base/features.cc` | `.onion` and MoltNet resolver rules | #2 and the MoltNet P1-P15 work, carried over in #11 |
| `net/socket/ssl_client_socket.{h,cc}` | `SSLClientContext::MoltSendNoClientCertificates()` (inline) and a member flag; `GetClientCertificate()` answers "no certificate" while it is set. `ssl_client_socket.h` is included by `http_network_session.h`, so expect another large `//net` rebuild | MoltNet 156 |
| `services/network/network_context.cc`, `services/network/public/mojom/network_context.mojom` | two `[MinVersion=1]` fields (`molt_send_no_client_certificates`, `molt_no_doh_probes`); after `Build()` in `MakeURLRequestContext()`, `http_transaction_factory()->GetSession()->ssl_client_context()->MoltSendNoClientCertificates()`; `ActivateDohProbes()` returns early | MoltNet 156 |
| `chrome/browser/chrome_content_browser_client.{h,cc}` | `SelectClientCertificate()` answers a MoltNet profile with `ContinueWithCertificate(nullptr, nullptr)` before the ChromeOS block and the auto-select policy; the `ShouldSendUniformNetworkState()` override | MoltNet 156 |
| `content/public/browser/content_browser_client.{h,cc}` | new virtual `ShouldSendUniformNetworkState(BrowserContext*)`, default `false` | MoltNet 156 |
| `content/browser/network/network_quality_observer_impl.cc`, `content/browser/network/browser_online_state_observer.cc`, new `content/browser/network/molt_uniform_network_state.h` | the five `OnNetworkQualityChanged()`/`OnNetworkConnectionChanged()` sends go through the header-only helpers (fixed values for MoltNet's renderers, no quality updates to them). The header is in no GN target's `sources` (header-only, to leave `content/browser/BUILD.gn` alone); if `gn check` complains, list it in `//content/browser:browser` | MoltNet 156 |
| `chrome/browser/molt_ai/tor/molt_net_routing.cc` | `ProxyExternallyControlled()` also reads `proxy_config::prefs::kProxyOverrideRules` (new target `:proxy_policy`, header-only) | MoltNet 156 |
| `chrome/browser/net/profile_network_context_service.cc` | `device_bound_sessions_enabled` comes from `DeviceBoundSessionsEnabledFor()` (false for MoltNet) | MoltNet 156 |
| `chrome/browser/net/proxy_config_monitor.{h,cc}` | a new `raw_ptr<Profile> moltnet_profile_` member, set in the profile constructor for MoltNet profiles; `AddToNetworkContextParams()` leaves `initial_proxy_config` unset, and `OnProxyConfigChanged()` sends nothing, unless `MoltNetRouting::MayApplyProxyConfig()` agrees. `proxy_config_monitor.h` is included by `profile_network_context_service.h` | moltnet-live |
| `chrome/browser/molt_ai/tor/molt_net_routing.{h,cc}` | `SettingsError()` (shared by the open check and the live one; reads `prefs::kPartitionedBlobUrlUsage`) and `MayApplyProxyConfig()` (`PrefProxyConfigTrackerImpl::PrefConfigToNetConfig()`, `net::ProxyConfig::Equals()`); new header-only target `:live_policy` | moltnet-live |
| `chrome/browser/molt_ai/tor/molt_net_live_guard.cc`, `molt_net_window.cc` | `PrefChangeRegistrar`s on the MoltNet and the regular profile's prefs, a `ProfileObserver` (`Profile::AddObserver()`), `chrome::ShowWarningMessageBoxAsync()` with a `char16_t` title; `OpenVerifiedWindow()` now gets the profile's `OTRProfileID` | moltnet-live |
| `chrome/browser/ui/autofill/chrome_autofill_client.cc` | `GetCrowdsourcingManager()` and `GetAddressNormalizer()` ask `molt_net_autofill.h` first (only these two functions were type-checked here: the whole file type-checks since round 28, which generates the proto_extras plugin's outputs such as `components/personal_context/proto/features/at_memory.equal.h`) | moltnet-live |
| `chrome/browser/payments/chrome_payment_request_delegate.cc` | `GetAddressNormalizer()` and `GetRegionDataLoader()` ask `molt_net_autofill.h` first | moltnet-live |
| `chrome/browser/molt_ai/tor/molt_net_autofill.cc` (new target `:autofill`, a public dep of `:tor`) | subclasses `autofill::AutofillCrowdsourcingManager` (overrides the two virtual `Start*Request()` and inherits its constructors), and builds an `AddressNormalizerImpl` and a `RegionDataLoaderImpl` on a `::i18n::addressinput::Source` of its own and libaddressinput's `NullStorage`; deps `//components/autofill/core/browser`, `//third_party/libaddressinput` | moltnet-live |
| `chrome/browser/net/proxy_config_monitor.{h,cc}` | now asks `MoltNetRouting::ProxyConfigFor()` (replaces `MayApplyProxyConfig()`), and holds a `base::CallbackListSubscription` from `MoltNetRouting::AddIdentityEndedCallback()` that sends `RefuseAllProxyConfig()` to every `ProxyConfigClient` of its MoltNet profile | moltnet-policy |
| `chrome/browser/molt_ai/tor/molt_net_routing.{h,cc}` | `EndIdentity()`/`HasIdentityEnded()` (profile `SupportsUserData`), `AddIdentityEndedCallback()` (`base::RepeatingCallbackList<void(Profile*)>`), `RefuseAllProxyConfig()` (`ProxyConfig::ProxyRules::Type::PROXY_LIST` with an empty list, `ProxyBypassRules::AddRulesToSubtractImplicit()`, a new traffic annotation `moltnet_ended_identity`), `IncognitoPolicyError()` (`IncognitoModePrefs::GetIncognitoModeType()`); the language write is gone (no `//components/language` dep) | moltnet-policy |
| `chrome/browser/molt_ai/tor/molt_net_window.cc` | ending an identity: `UnloadController::From(browser)->set_force_skip_warning_user_on_close(true)`, `DevToolsAgentHost::GetOrCreateAll()` + `ForceDetachAllSessions()`, `RenderProcessHost::Shutdown(content::RESULT_CODE_KILLED)`, `TabStripModel::CloseAllTabs()`; new dep `//content/public/common` | moltnet-policy |
| `chrome/browser/molt_ai/tor/molt_net_pref_isolation.{h,cc}` | `CreateMoltNetPrefService()` takes the `ValueMapPrefStore` it puts in the extension slot; `FixMoltNetLanguages()` writes into it (`WriteablePrefStore::DEFAULT_PREF_WRITE_FLAGS`); new deps `:language_policy` and `//chrome/common/molt_ai/fingerprinting` (neither depends on `//chrome/browser`, so `//chrome/browser/profiles:misc` keeps no cycle) | moltnet-policy |
| `chrome/browser/chrome_content_browser_client.cc` | `AppendExtraCommandLineSwitches()` and `ShouldReduceAcceptLanguage()` pass the `ReduceAcceptLanguageEnabled` value through `ReduceAcceptLanguageEnabledFor()` (`molt_net_language_policy.h`, `:language_policy`) | moltnet-policy |
| `net/socket/client_socket_pool_manager.{h,cc}` | new virtual `ClientSocketPoolManager::GetSocketPoolForSite()` (default: `GetSocketPool()`); `InitSocketPoolHelper()` asks `HttpNetworkSession::GetSocketPoolForSite()` with the group's network anonymization key | moltnet-policy |
| `net/socket/client_socket_pool_manager_impl.{h,cc}` | a constructor argument `molt_per_site_pool_size` (default 0), a `std::unique_ptr<MoltPerSiteSocketPools>`, the override, and the flush, idle-socket and info calls forwarded to it | moltnet-policy |
| `net/socket/transport_client_socket_pool.h` | inline `MoltIsUnused()` (reads `group_map_`, `pending_callback_map_`, `higher_pools_`). Widely included: another large `//net` rebuild | moltnet-policy |
| `net/socket/molt_per_site_socket_pool_policy.h`, `net/socket/molt_per_site_socket_pools.h` (new, header-only) | in no target's `sources`, like `molt_uniform_network_state.h`; if `gn check` complains, list them in `//net:net` | moltnet-policy |
| `net/http/http_network_session.{h,cc}` | `HttpNetworkSessionParams::molt_per_site_socket_pool_size`, passed to both pool managers; `GetSocketPoolForSite()` | moltnet-policy |
| `services/network/public/mojom/network_context.mojom`, `services/network/network_context.cc` | `[MinVersion=1] uint32 molt_per_site_socket_pool_size`, copied into the session params; set to 128 for MoltNet by `molt_net_network_context_policy.h` (which now includes the `net/` policy header; 48 since moltnet-shared) | moltnet-policy |
| `net/socket/client_socket_pool.{h,cc}` (new upstream copies) | a public nested interface `ClientSocketPool::MoltSocketGate`, `MoltSetSocketGate()` (also sets `additional_capacity_` to `SocketPoolAdditionalCapacity::CreateEmpty()` and calls `ResetExpandability()`) and a `raw_ptr<MoltSocketGate>` member; `UpdateExpandabilityBeforeAllocation()` asks the gate, `UpdateExpandabilityAfterRelease()` tells it. `client_socket_pool.h` is included by every pool: another large `//net` rebuild | moltnet-shared |
| `net/socket/transport_client_socket_pool.h` | inline `MoltHasStalledRequest()`, `MoltRetryStalledRequests()`, `MoltCloseOneIdleSocket()`, `MoltCloseIdleSessions()`: read `group_map_`, `idle_socket_count_`, `higher_pools_`, and call `CheckForStalledSocketGroups()`, `CloseOneIdleSocketExceptInGroup()`, `CloseOneIdleConnectionInHigherLayeredPool()` and the base class's `UpdateExpandabilityBeforeAllocation()` (protected) | moltnet-shared |
| `net/socket/molt_connection_budget.h` (new, header-only, in no target's `sources`), `molt_per_site_socket_pools.h`, `molt_per_site_socket_pool_policy.h` | `MoltConnectionBudget` posts tasks (`base::SequencedTaskRunner::GetCurrentDefault()`, `base::WeakPtrFactory`); `MoltBudgetSiteFor()` uses `NetworkAnonymizationKey::GetTopFrameSite()`, `url::Origin::GetDebugString()` and `SchemefulSite(GURL)`; `MoltSocksHandshakeLimiter` and its `Slot`; the pools object now implements `ClientSocketPool::MoltSocketGate` and `MoltConnectionBudget::Member` and keys its pools by `std::pair<ProxyChain, std::optional<SchemefulSite>>` | moltnet-shared |
| `net/socket/client_socket_pool_manager_impl.{h,cc}`, `net/http/http_network_session.{h,cc}` | a constructor argument `MoltConnectionBudget*` (default `nullptr`); the session owns a `std::unique_ptr<MoltConnectionBudget>` and a `std::unique_ptr<MoltSocksHandshakeLimiter>`, declared before the pool managers (so destroyed after them), made when `molt_per_site_socket_pool_size` is set, and passes the limiter in `CreateCommonConnectJobParams()` | moltnet-shared |
| `net/socket/connect_job.h`, `net/socket/socks_connect_job.{h,cc}` (`socks_connect_job.h` a new upstream copy) | `CommonConnectJobParams::molt_socks_handshake_limiter` (a `raw_ptr`); two new states before `STATE_TRANSPORT_CONNECT`, a `std::unique_ptr<MoltSocksHandshakeLimiter::Slot>` member, new cases in `GetLoadState()` and `ChangePriorityInternal()` | moltnet-shared |
| `services/network/keepalive_statistics_recorder.{h,cc}`, `services/network/url_loader_factory.{h,cc}` (new upstream copies), `services/network/molt_keepalive_partitions.h`, `molt_keepalive_site.h` (new, header-only, in no target's `sources`) | the service's recorder holds MoltNet's per-site recorders (`MoltRecorderFor()` returns a `base::WeakPtr`); the factory keeps a `base::WeakPtr<KeepaliveStatisticsRecorder>` chosen in its constructor from `IsolationInfo::top_frame_origin()` (`url::Origin::GetTupleOrPrecursorTupleIfOpaque()`, `net::SchemefulSite`) and hands it to each `URLLoader` | moltnet-shared |
| `services/network/network_context.cc`, `services/network/public/mojom/network_context.mojom` | `[MinVersion=1] bool molt_per_site_keepalive_limits`, set by `molt_net_network_context_policy.h`; the constructor registers the context with the service's recorder, the destructor removes it after `DeregisterNetworkContext()` | moltnet-shared |
| `storage/browser/quota/quota_settings.h`, `quota_manager_impl.cc`, `quota_temporary_storage_evictor.cc` (new upstream copies), `storage/browser/quota/molt_storage_budget.h`, `molt_storage_budget_policy.h` (new, header-only, in no target's `sources`) | `QuotaSettings::molt_per_site_quota` (0 by default; set by `molt_net_storage.cc`); `UsageAndQuotaInfoGatherer` gets `MoltOnGotGlobalUsage()` (bound to `QuotaManagerImpl::GetGlobalUsage()`, reads `UsageTracker::GetCachedStorageKeysUsage()`) and two members; `OnGotEvictionRoundInfo()` zeroes the overage. `quota_settings.h` is widely included in `//storage` and `//content` | moltnet-shared |
| `components/download/internal/background_service/in_memory_download.cc` (new upstream copy), `molt_registering_site_isolation.h` (new, header-only, in no target's `sources`) | `SendRequest()` puts `RequestParams::isolation_info` into the request's `trusted_params` (`ResourceRequest::TrustedParams::isolation_info`); the factories it uses must be trusted (they are: `StoragePartitionImpl::CreateURLLoaderFactoryParams()`, the profile's browser-process factory), or the network service reports a bad message | moltnet-shared |
| `content/browser/download/download_manager_impl.cc`, `save_file_manager.cc` (new upstream copies), `molt_download_isolation.h`, `molt_download_isolation_policy.h` (new, header-only, in no target's `sources`) | `BeginDownloadInternal()` and `SaveURL()` call the header's helpers: `RenderFrameHost::GetIsolationInfoForSubresources()`, `DownloadItem::GetIsolationInfo()`, `DownloadUrlParameters::set_isolation_info()` / `set_update_first_party_url_on_redirect()`, `net::IsolationInfo::CreateTransient()`, `net::SchemefulSite`, `GetContentClient()->browser()` | moltnet-shared |
| `content/public/browser/content_browser_client.{h,cc}`, `chrome/browser/chrome_content_browser_client.{h,cc}` | new virtual `ShouldKeepDownloadsWithTheirSite(BrowserContext*)`, default `false`; Chrome's answers `molt_ai::tor::KeepDownloadsWithTheirSite(IsMoltNetProfile(...))` (`molt_net_network_context_policy.h`) | moltnet-shared |
| `net/base/schemeful_site.h` (new upstream copy) | inline `MoltGetTupleOrPrecursorTupleIfOpaque()` (`url::Origin::GetTupleOrPrecursorTupleIfOpaque()`, returns `url::SchemeHostPort`); `MoltBudgetSiteFor()` uses it instead of reading `url::Origin::GetDebugString()`. Included by most of `//net`, `//services/network` and everything above: close to a full rebuild | moltnet-shared2 |
| `net/socket/molt_connection_budget.h`, `molt_per_site_socket_pools.h`, `molt_per_site_socket_pool_policy.h` | `MoltTorCapacity` (`base::RefCounted`, one per network service process: `GetShared()`, `base::WrapRefCounted`, `base::MakeRefCounted`) holds the connection budget and `MoltSocksHandshakeBudget` (`MoltWeightedSlots`); budget keys are `(owner, site)` with the session's address as owner (`uintptr_t`); `MoltSocksHandshakeWeight(host)`; each new per-site `TransportClientSocketPool` gets `MoltDisableBackupConnectJobs()`; the pools are keyed by a struct (proxy chain, site, one-off) with a defaulted `operator<=>` (`ProxyChain`'s and `SchemefulSite`'s), and a key with a nonce (`NetworkAnonymizationKey::GetNonce()`) of a nameable site gets the site's HTTP pool of 4 | moltnet-shared2 |
| `net/http/http_network_session.{h,cc}`, `net/socket/client_socket_pool_manager_impl.{h,cc}` | a `scoped_refptr<MoltTorCapacity>` replaces the session's `std::unique_ptr<MoltConnectionBudget>` (forward declaration and `base/memory/scoped_refptr.h`); `ClientSocketPoolManagerImpl` takes a further `uintptr_t molt_budget_owner` (default 0) | moltnet-shared2 |
| `net/socket/client_socket_pool.h`, `transport_client_socket_pool.h`, `transport_client_socket_pool.cc` (new upstream copy), `socks_connect_job.cc` | inline `MoltHasSocketGate()`; `client_socket_pool_manager.cc` keeps Chromium's `LOAD_IGNORE_LIMITS` line; `transport_client_socket_pool.h` now includes `molt_per_site_socket_pool_policy.h` (std-only) and has inline `MoltSetIgnoreLimitsOverflow()`, `MoltMayOverflow()`, `MoltRespectsPoolLimits(RespectLimits)`, `Group::MoltIgnoreLimitsRequestWaits()` (walks `unbound_requests_` with `RequestQueue::Pointer`, `FirstMax()`, `GetNextTowardsLastMin()`), private `MoltCanUseAdditionalSocketSlot(const Group&) const` and `MoltFindOverflowGroup(Group**, GroupId*)` (defined in the `.cc`) and a `size_t` member; in the `.cc`, `IsStalled()`, `FindTopStalledGroup()` and `ProcessPendingRequest()` ask `MoltCanUseAdditionalSocketSlot()`, `RequestSocketInternal()`'s pool check asks `MoltRespectsPoolLimits()`, and `CheckForStalledSocketGroups()` serves `MoltFindOverflowGroup()` at the limit; inline `MoltDisableBackupConnectJobs()` (`connect_backup_jobs_enabled_`); `RefreshGroup()` and `CancelAllConnectJobs()` call the base class's protected `UpdateExpandabilityAfterRelease()` when they removed a gated pool's jobs; `DoMoltHandshakeSlot()` passes `MoltSocksHandshakeWeight(socks_params_->destination().host())` to `Acquire()` | moltnet-shared2 |
| `net/cert/cert_verifier.{h,cc}`, `net/cert/multi_threaded_cert_verifier.cc`, `net/socket/ssl_client_socket_impl.cc` (new upstream copies), `net/socket/ssl_client_socket.h`, `net/cert/molt_cert_fetch_site.h` (new, header-only, in no target's `sources`) | `CertVerifier::RequestParams::molt_cert_fetch_site()` / `MoltSetCertFetchSite()` (compared in `operator==` and `operator<`, with `std::tie`); `NET_EXPORT` `MoltGetCertFetchSite()` / `MoltSetCertFetchSite()` declared in the new header and defined in `cert_verifier.cc` over an `ABSL_CONST_INIT thread_local` pointer (`third_party/abseil-cpp/absl/base/attributes.h`); `DoVerifyOnWorkerThread()` gets the site as a bound argument and holds a `MoltScopedCertFetchSite`; `SSLClientSocketImpl::VerifyCert()` builds the `RequestParams` first and names the site when `SSLClientContext::molt_carries_cert_fetch_site()`. On a component build, `//services/cert_verifier` calls the `NET_EXPORT` functions across the component boundary | moltnet-shared2 |
| `services/cert_verifier/cert_net_url_loader/cert_net_fetcher_url_loader.{h,cc}`, `molt_cert_fetch_isolation.h` (new, header-only, in no target's `sources`), `services/network/network_context.cc` | `RequestParams::molt_site` (a `std::string`, not compared), set in `DoFetch()` from `net::MoltGetCertFetchSite()`; an isolated fetch's `trusted_params->isolation_info` is `MoltCertFetchIsolationInfo()` (`net::IsolationInfo::Create()` with `RequestType::kOther`, the site's origin twice, an empty `SiteForCookies` and a `base::UnguessableToken` nonce); `MakeURLRequestContext()` calls `ssl_client_context()->MoltCarryCertFetchSite()` for `molt_uncached_cert_verification` contexts | moltnet-shared2 |
| `storage/browser/quota/quota_settings.h`, `quota_manager_impl.cc` | `QuotaSettings::molt_memory_reserve`; `UsageAndQuotaInfoGatherer::MoltOnGotGlobalUsage()` posts `base::SysInfo::AmountOfAvailablePhysicalMemory()` to `base::ThreadPool::PostTaskAndReplyWithResult()` (`MayBlock`, `USER_BLOCKING`) when `MoltNeedsMemoryCheck()`, replying to `MoltOnGotAvailableMemory()` through the gatherer's `weak_factory_`; a new `std::optional<int64_t>` member | moltnet-shared2 |
| `storage/browser/blob/blob_memory_controller.{h,cc}`, `blob_storage_constants.h`, `blob_data_builder.h`, `blob_storage_context.cc`, `blob_registry_impl.{h,cc}`, `blob_builder_from_stream.{h,cc}` (all but the controller new upstream copies), `molt_blob_budget_policy.h` (new, header-only, std-only, in no target's `sources`), `content/browser/blob_storage/blob_registry_wrapper.cc` (new upstream copy) | `BlobStorageLimits::molt_per_key_memory` / `molt_memory_reserve`; a `uint64_t molt_budget_key` parameter, defaulted, on `DetermineStrategy()`, `CanReserveQuota()`, `ReserveMemoryQuota()`, `GrantMemoryAllocations()`, `RevokeMemoryAllocation()` and `MemoryAllocation`'s constructor; the overload `GetAvailableMemoryForBlobs(uint64_t)` (`base::saturated_cast`), `MoltMemoryUsage()`, `MoltAvailableMemory()` (`base::TimeTicks`, `base::SysInfo::AmountOfAvailablePhysicalMemory().InBytes()`), `molt_set_testing_available_memory()`; members `std::map<uint64_t, size_t>` and `mutable` cache fields declared before `weak_factory_`; `BlobRegistryImpl::Delegate::MoltBudgetKey()` (inline virtual, default 0) overridden by content's `BindingDelegate`, which now also takes the `ChildProcessId` (`is_null()`, `GetUnsafeValue()`); `Register()` reads `delegate` and `RegisterFromStream()` reads `receivers_.current_context()`. `blob_storage_constants.h` and `blob_memory_controller.h` are included across `//storage` and `//content`: a large rebuild | moltnet-shared2 |
| `content/browser/renderer_host/render_process_host_impl.cc`, `content/browser/security/cpsp/child_process_security_policy_impl.{h,cc}` (new upstream copies), `molt_browsing_instance_processes.h`, `molt_browsing_instance_processes_policy.h` (new, header-only, in no target's `sources`), `content/public/browser/content_browser_client.{h,cc}`, `chrome/browser/chrome_content_browser_client.{h,cc}` | `IsSuitableHost()` returns false (with a `TRACE_EVENT_INSTANT`) before the embedder's check when `MoltMayServeBrowsingInstance()` refuses; that helper reads `IsolationContext::browsing_instance_id()` and `browser_context()`, `SiteInfo::site_url()`, `RenderProcessHost::GetID()` and calls the new `ChildProcessSecurityPolicyImpl::MoltHostsOtherBrowsingInstance()` (under `lock_`, `GetProcessStateForQuery()`, `browsing_instance_default_isolation_states()`) and the new virtual `ContentBrowserClient::ShouldKeepBrowsingInstancesInOwnProcesses(BrowserContext*, const GURL&)`, default `false`, which Chrome answers with `molt_ai::tor::KeepBrowsingInstancesApart()` (`:process_policy`). `child_process_security_policy_impl.h` is widely included in `//content` | moltnet-shared2 |
| `components/download/internal/background_service/in_memory_download.cc`, `molt_registering_site_isolation.h` | `MoltCarryFetchCredentialsAndInitiator()` sets `ResourceRequest::credentials_mode` and `request_initiator` from `RequestParams::credentials_mode` / `initiator` when the request carries an isolation | moltnet-shared2 |
| `net/socket/molt_per_site_socket_pool_policy.h`, `molt_per_site_socket_pools.h`, `molt_connection_budget.h`, `net/socket/client_socket_pool_manager{,_impl}.{h,cc}`, `net/http/http_network_session.{h,cc}`, `services/cert_verifier/cert_net_url_loader/cert_net_fetcher_url_loader.cc` | certificate fetches get pools of their own: `GetSocketPool()`'s path gains `bool molt_no_cert_fetches` (default `false`), from `ClientSocketPool::GroupId::disable_cert_network_fetches()` in `InitSocketPoolHelper()`; the pools' key struct gains `bool cert_fetch_pool` (in the defaulted `operator<=>`); `MoltTorCapacity` holds a second `MoltConnectionBudget` (`cert_fetch_budget()`, 48 of the 768) and the pools a nested `CertFetchMember`; the isolated fetches set `net::LOAD_DISABLE_CERT_NETWORK_FETCHES`. Supersedes moltnet-shared2's "site's HTTP pool of 4" for nonce keys: a credentialless key now uses its site's main pool. `static_assert`s on Tor's capacity in the policy header must still hold | moltnet-shared3 |
| `net/disk_cache/memory/mem_backend_impl.{h,cc}`, `mem_entry_impl.cc`, `net/disk_cache/disk_cache.h`, `net/http/http_cache.{h,cc}` (new upstream copies), `molt_mem_backend_budget_policy.h` (new, header-only, std-only, in no target's `sources`), `services/network/network_context.cc` | a virtual `Backend::MoltSetPerSiteBudget(const MoltCacheSiteBudget&)` (empty by default; `MemBackendImpl` overrides it); `ModifyStorageSize()` and `HasExceededStorageSize()` gain overloads taking the `const MemEntryImpl*` (the four calls in `mem_entry_impl.cc` pass `this`); a `std::map<std::string, int64_t, std::less<>>` and a `base::RepeatingTimer` member; `MaxFileSize()`, `OpenEntry()`, `SetMaxBytes()`, `OnUpdateMemoryLimit()` and `OnReleaseMemory()` are hooked; `HttpCache::MoltSetPerSiteBudget()` is applied in `OnBackendCreated()`; `network_context.cc` calls it after the context is built when `molt_per_site_http_cache`. `disk_cache.h` and `http_cache.h` are widely included: a large `//net` rebuild | moltnet-shared3 |
| `services/network/proxy_config_service_mojo.{h,cc}` (new upstream copies), `services/network/molt_ended_identity_policy.h` (new, header-only, in no target's `sources`), `services/network/network_context.cc` | `ProxyConfigServiceMojo::MoltSetOnRefuseAll(base::RepeatingClosure)`, run after the observers in `OnProxyConfigUpdated()` when the new configuration refuses everything; `network_context.cc` binds `MoltStopEverything(base::WeakPtr<NetworkContext>)` (anonymous namespace: `url_request_context()->url_requests()` through `const_cast`, each request's `GetWeakPtr()`, `Cancel()`, then `CloseAllConnections()`) when `molt_stop_all_when_refusing`, keeping a raw pointer to the proxy config service it created | moltnet-shared3 |
| `content/browser/loader/keep_alive_url_loader.cc`, `keep_alive_url_loader_service.cc`, `content/browser/background_fetch/background_fetch_delegate_proxy.cc` (new upstream copies), `content/browser/download/download_manager_impl.cc`, `save_file_manager.cc`, `content/public/browser/content_browser_client.{h,cc}`, `chrome/browser/chrome_content_browser_client.{h,cc}`, `chrome/browser/molt_ai/tor/molt_net_ended_identity_throttle.{h,cc}` | the new virtual `ContentBrowserClient::HasNetworkIdentityEnded(BrowserContext*)`, default `false`, Chrome's answering `molt_ai::tor::HasMoltNetIdentityEnded()`; `KeepAliveURLLoader::StartInternal()` posts `OnCompleteInternal()` with `ERR_ABORTED` through `weak_ptr_factory_` (`base/task/single_thread_task_runner.h`) after `OnKeepaliveRequestStarted()`, `WillFollowRedirect()` returns `ERR_ABORTED`; `CreateKeepAliveURLLoader()` returns `nullptr`; `BackgroundFetchDelegateProxy::StartRequest()` returns early; `BeginDownloadInternal()` goes to `CreateInterruptedDownload()` with `DOWNLOAD_INTERRUPT_REASON_NETWORK_INVALID_REQUEST`; `SaveURL()` fails the save item | moltnet-shared3 |
| `storage/browser/quota/quota_settings.h`, `quota_manager_impl.cc`, `storage/browser/blob/blob_storage_constants.h`, `blob_memory_controller.{h,cc}`, `molt_storage_budget_policy.h`, `molt_blob_budget_policy.h` | supersedes moltnet-shared2's rows for these files: `molt_memory_reserve` is gone from both settings, and with it the machine read (`MoltOnGotAvailableMemory()`, `MoltAvailableMemory()`, `molt_set_testing_available_memory()`, the cache members); both settings gain `base::RepeatingClosure molt_on_growth_beyond_floor` (`blob_storage_constants.h` now includes `base/functional/callback.h`); `Completed()` runs it past the floor, `BlobMemoryController::MoltNoteGrowth() const` after `GrantMemoryAllocations()`; the blob policy gains `MoltBlobSiteKeys` (`std::map<std::string, uint64_t, std::less<>>`) | moltnet-shared3 |
| `components/download/internal/background_service/blob_task_proxy.{h,cc}` (new upstream copies), `in_memory_download.cc`, `molt_registering_site_isolation.h` | `SaveAsBlob()` gains a defaulted `uint64_t molt_budget_key` (`storage::kMoltBlobBrowserKey`), `SaveAsBlobOnIO()` a plain one, bound through `base::BindOnce`, and calls `BlobDataBuilder::MoltSetBudgetKey()`; `in_memory_download.cc` keeps one `storage::MoltBlobSiteKeys` for the browser (`base::NoDestructor`, under a `base::Lock`) and passes `MoltBlobBudgetKeyForFetch()` (`url::Origin::GetTupleOrPrecursorTupleIfOpaque()`, `url::SchemeHostPort::GetURL()`, `net::SchemefulSite::Serialize()`); `//components/download/internal/background_service` now includes `storage/browser/blob/molt_blob_budget_policy.h` and `net/base/schemeful_site.h` | moltnet-shared3 |
| `chrome/browser/molt_ai/tor/molt_net_memory_valve.{h,cc}` (new), `molt_net_window.cc`, `molt_net_live_policy.h`, `chrome/browser/chrome_content_browser_client.cc`, `chrome/browser/molt_ai/fingerprinting/molt_net_storage.cc` | `MoltNetMemoryValve` (`base::RefCountedThreadSafe`, `std::atomic` members, `base::ThreadPool::PostTask()` with `MayBlock`, `base::SysInfo::AmountOfAvailablePhysicalMemory().InBytes()`), kept per profile in `base::SupportsUserData`; `GetFixedQuotaSettings()` / `GetFixedBlobStorageLimits()` set the callback from `MoltNetMemoryValveCallback(Profile::FromBrowserContext(...), kMoltNetStorageMemoryReserveBytes)`; `EndMoltNetIdentityForMemory()` defined in `molt_net_window.cc` (`LookUpMoltNetProfile()`, `EndForChangedSettings()`) | moltnet-shared3 |
| `components/services/storage/dom_storage/local_storage_impl.{h,cc}`, `session_storage_impl.{h,cc}`, `session_storage_namespace_impl.{h,cc}` (new upstream copies), `molt_dom_storage_site_policy.h` (new, header-only, std-only, in no target's `sources`), `content/browser/dom_storage/dom_storage_context_wrapper.cc` (new upstream copy), `content/public/browser/content_browser_client.{h,cc}`, `chrome/browser/chrome_content_browser_client.{h,cc}` | `MoltKeepCachesPerSite()` overrides; `LocalStorageImpl` gains a `base::RepeatingTimer` (`base/timer/timer.h`), `StorageAreaHolder` a `base::TimeTicks`; per-site purges compare `blink::StorageKey::top_level_site()` (`net::SchemefulSite`; `session_storage_namespace_impl.h` now includes `net/base/schemeful_site.h`), the session one reads `SessionStorageDataMap::map_locator().storage_key()`; `GetOrCreateStorageArea()`, `BindStorageArea()`, `BindNamespace()` and both `PurgeMemory()`s are hooked; the wrapper's `MaybeBind*Control()` ask the new virtual `ContentBrowserClient::ShouldKeepDomStorageCachesPerSite(BrowserContext*)` (default `false`; Chrome: `molt_ai::tor::KeepDomStorageCachesPerSite()`) | moltnet-shared3 |
| `net/socket/molt_identity_end.h` (new, header-only, in no target's `sources`), `net/socket/socks5_client_socket.{h,cc}`, `socks_connect_job.cc`, `connect_job.h`, `net/http/http_network_session.{h,cc}` | `MoltIdentityEnd` (`base::RefCounted`, `Mark()`, `ended()`); `CommonConnectJobParams` gains `raw_ptr<MoltIdentityEnd> molt_identity_end` (forward-declared in `connect_job.h`); `HttpNetworkSession` holds a `scoped_refptr<MoltIdentityEnd>` (made when `molt_per_site_socket_pool_size > 0`), sets it in `CreateCommonConnectJobParams()` and gains `MoltEndIdentity()`; `SOCKSConnectJob::DoSOCKSConnect()` refuses once it is marked and hands the socket the signal (`base::WrapRefCounted()`); `SOCKS5ClientSocket` gains `MoltSetIdentityEnd(scoped_refptr<MoltIdentityEnd>)`, and `Write()` and `DoHandshakeWrite()` return `ERR_ABORTED` once it is marked. `connect_job.h` and `http_network_session.h` are widely included | moltnet-shared4 |
| `services/network/network_context.{h,cc}` (`network_context.h` a new upstream copy), `url_loader_factory.cc`, `proxy_config_service_mojo.{h,cc}`, `molt_ended_identity_policy.h` | supersedes moltnet-shared3's `MoltStopEverything()` (gone): `NetworkContext::MoltEndIdentity()` takes `kMoltEndSteps` in order (the session's `MoltEndIdentity()`, `ResetURLLoaderFactories()`, `websocket_factory_.reset()` under `BUILDFLAG(ENABLE_WEBSOCKETS)`, `web_transports_` moved out and cleared, `url_requests()` cancelled through weak pointers, `CloseAllConnections()`); `MoltAdmits(MoltNetworkEntry) const` and `bool molt_identity_ended_`, and `network_context.h` includes `molt_ended_identity_policy.h`; entry checks at the top of `CreateWebSocket()` (`OnFailure()`), `CreateWebTransport()` (`OnHandshakeFailed()` with a `WebTransportError`), `ResolveHost()` (`OnComplete(ERR_ABORTED, ...)`), `CreateHostResolver()`, `PreconnectSockets()`, `Prefetch()` and `URLLoaderFactory::CreateLoaderAndStartWithSyncClient()` (`OnComplete(URLLoaderCompletionStatus(ERR_ABORTED))`); `ProxyConfigServiceMojo::MoltRefusesAll() const`, read by `MakeURLRequestContext()` to start a context ended (`MoltStartsEnded()`). `network_context.h` is widely included | moltnet-shared4 |
| `chrome/browser/net/proxy_config_monitor.cc`, `chrome/browser/molt_ai/tor/molt_net_routing.{h,cc}`, `molt_net_window.cc`, `molt_net_identity_end_policy.h` | `MoltNetRouting::IdentityEndedCallback` gains a `const EndAckHolder&` (`base::RepeatingCallback<base::OnceClosure()>`); the monitor sends `FlushProxyConfig()` after the refusal, its callback wrapped in `mojo::WrapCallbackWithDefaultInvokeIfNotRun()` (`mojo/public/cpp/bindings/callback_helpers.h`); `MoltNetRouting::WhenNetworkHasEnd(Profile*, base::OnceClosure)` and a `NetworkEnd` (`base::SupportsUserData::Data`, a `base::OneShotTimer` for the 5-second deadline); `EndIdentity()` no longer calls `FlushNetworkState()`; `EndIdentityAndCloseWindows()` stops pages from `StopPagesThenConfirmRefusal()` | moltnet-shared4 |
| `components/download/internal/common/download_utils.cc` (new upstream copy), `molt_fetch_credentials.h` (new, header-only, in no target's `sources`) | every profile: after `request->mode = network::mojom::RequestMode::kNavigate`, `MoltApplyDownloadCredentialsMode(params->credentials_mode(), *request)` (a credentials mode other than `kInclude` makes the request `kNoCors` with that mode; `kSameOrigin` without an initiator, `kOmit`) | moltnet-shared4 |
| `storage/browser/blob/blob_storage_context.h` (new upstream copy), `blob_memory_controller.{h,cc}`, `molt_blob_budget_policy.h`, `components/download/internal/background_service/blob_task_proxy.{h,cc}`, `in_memory_download.{h,cc}` (`in_memory_download.h` a new upstream copy) | `BlobMemoryController::MoltHoldMemory(uint64_t, size_t)` returning `MoltBlobHold` (counts toward `blob_memory_used_`, the key's use and `MoltNoteGrowth()`) and `MoltReleaseHeldMemory()`, forwarded by two public `BlobStorageContext` methods; `BlobTaskProxy::MoltHoldMemory(uint64_t, size_t, MoltHoldCallback)` and `MoltReleaseHeldMemory()`, posted to the IO thread, the held key and bytes kept as members and released in the destructor and in `SaveAsBlobOnIO()`; `InMemoryDownloadImpl::OnDataReceived()` resumes only once the hold is answered (`MoltOnBufferHeld()`, a generation counter, `MoltContinueReading()` with the upstream tail), and a refused hold resets the loader and calls `OnComplete(false)`; `OnComplete(false)` and `Reset()` release (`MoltReleaseBuffer()`) | moltnet-shared4 |
| `components/services/storage/dom_storage/storage_area_impl.{h,cc}`, `session_storage_data_map.{h,cc}` (new upstream copies), `local_storage_impl.{h,cc}`, `session_storage_impl.{h,cc}`, `molt_dom_storage_site_policy.h`, `molt_dom_storage_site_budget.h` (new, header-only, in no target's `sources`), `content/browser/dom_storage/dom_storage_context_wrapper.cc`, `content/public/browser/content_browser_client.{h,cc}`, `chrome/browser/chrome_content_browser_client.{h,cc}` | `StorageAreaImpl::Delegate` gains four virtuals with default bodies (`MoltMayGrowTo()`, `MoltShrankTo()`, `MoltSiteCommitDelay()`, `MoltSiteCommitted()`), called in `Put()` (a refused growth restores the moved-out old value, then `KeyChangeFailed`, never `ReportBadMessage`), `Delete()`, `DeleteAll()`, `ComputeCommitDelay()`, `CollectCommit()` and `OnForkStateLoaded()`; `SessionStorageDataMap::Listener` gains the same four with the map id and storage key; `LocalStorageImpl` and `SessionStorageImpl` hold a `MoltDomStorageSiteBudget` (`mojo::Remote<MoltDomStorageGrowthObserver>`, `base::TimeTicks`) and override `MoltSetGrowthObserver()`; the wrapper binds a self-owned `MoltDomStorageGrowthObserver` (`mojo::MakeSelfOwnedReceiver`) when the new virtual `ContentBrowserClient::GetDomStorageGrowthBeyondFloorCallback(BrowserContext*)` (default null; Chrome: the memory valve's callback) returns one | moltnet-shared4 |
| `chrome/browser/download/bubble/download_bubble_ui_controller.cc`, `chrome/browser/ui/webui/downloads/downloads_dom_handler.cc` (new upstream copies), `chrome/browser/download/molt_retried_download_isolation.h` (new, header-only, in no target's `sources`), `content/browser/download/molt_download_isolation{,_policy}.h` | both `RetryDownload()`s call `MoltRetryDownloadWithItsSite()` (`DownloadItem::GetRequestInitiator()`, `GetIsolationInfo()`, `DownloadUrlParameters::set_initiator()`, `set_isolation_info()`, `set_update_first_party_url_on_redirect()`); content's helper reads `DownloadUrlParameters::download_source()` (`download::DownloadSource::RETRY`, `RETRY_FROM_BUBBLE`) | moltnet-shared2 |
| `net/disk_cache/memory/mem_backend_impl.{h,cc}`, `molt_mem_backend_budget_policy.h`; `components/services/storage/dom_storage/local_storage_impl.cc`, `session_storage_impl.cc`, `session_storage_namespace_impl.{h,cc}`, `molt_dom_storage_site.h` (new, header-only, in no target's `sources`, like `molt_dom_storage_site_policy.h`), `molt_dom_storage_site_policy.h`; `storage/browser/quota/molt_storage_budget.h` | `MemBackendImpl::CreateEntry()` refuses (`ERR_INSUFFICIENT_RESOURCES`) an entry `MoltMayCreateEntry()` refuses (with a budget only; `HttpCache::Transaction` then bypasses the cache); the DOM storage budget functions and per-site purges name a key's group with `MoltDomStorageSiteOf()` instead of `top_level_site().Serialize()`, and `SessionStorageNamespaceImpl::MoltPurgeUnboundAreasOfSite()` now takes that `const std::string&` (it no longer reads `net::SchemefulSite`); `MoltSiteUsage()` compares keys with `MoltSameBudgetSite()` (a key with a nonce only with its nonce's) | redteam2 (round 27) |
| `components/services/storage/dom_storage/storage_area_impl.{h,cc}`, `local_storage_impl.{h,cc}`, `session_storage_impl.{h,cc}`, `session_storage_data_map.{h,cc}`, `session_storage_namespace_impl.{h,cc}`, `molt_dom_storage_site_policy.h`, `molt_dom_storage_site_budget.h` | every profile: `StorageAreaImpl::Put()` gains a lambda `restore_old_value` (captures by reference) called before each early return after the old value is moved out, the upstream quota refusal included; MoltNet: a fifth `StorageAreaImpl::Delegate` virtual `MoltMayWrite(size_t)` (default body, checked in `Put()` right after the map is loaded, refusal = `KeyChangeFailed` + `false`) and a public inline `molt_holds_items()` (reads both maps); `SessionStorageDataMap::Listener::MoltMayWrite()`; `LocalStorageImpl::InitiateConnection()` and `SessionStorageImpl::InitiateConnection()` call `OnConnectionFinished()` and return before opening a database when `MoltDomStorageKeepsNoDatabase()` (so `database_` stays null: the storage service's existing no-database mode); `LocalStorageImpl::MoltMayDrop(StorageAreaHolder&)` (private, the nested class forward-declared in the header); `RetrieveStorageUsage()`'s no-database branch reports `storage_used()` for MoltNet; `MoltDomStorageSiteBudget::MayWrite()` keeps a `std::map<std::string, MoltDomStorageWriteAllowance, std::less<>>` | moltnet-shared5 |
| `storage/browser/quota/client_usage_tracker.{h,cc}`, `usage_tracker.{h,cc}` (new upstream copies), `quota_manager_impl.cc`, `molt_storage_budget.h`, `molt_storage_usage_counts.h` (new, header-only, std-only, in no target's `sources`) | `molt_storage_budget.h` now defines `MoltBudgetSite` (an `std::optional<base::UnguessableToken>` and an `std::optional<net::SchemefulSite>`, `operator==` defaulted, `operator<` hand-written) and `MoltBudgetSiteOf()`, includes `base/unguessable_token.h` and `net/base/schemeful_site.h`, and is now included by `client_usage_tracker.h` and `usage_tracker.h` (widely included in `//storage`); `ClientUsageTracker` gains `MoltCountSites()`, `MoltSiteUsage()`, `MoltCachedUsage()`, `MoltHasCachedUsage()` (`std::map::contains` with `CompareBucketLocators`) and a private `MoltCountChange()` called from `UpdateBucketUsageCache()`, `DeleteBucketCache()` (an `if` with initializer), `SetUsageCacheEnabled()` and `CacheBucketUsage()`; `UsageTracker` gains `MoltRefreshSiteUsage()` (a `base::BarrierClosure` over `GetStorageKeyUsageWithBreakdown()`, a lambda taking `blink::mojom::UsageBreakdownPtr`), `MoltDidCountEverything()` (bound to `GetGlobalUsage()` with `weak_factory_`), `MoltSiteUsage()`, `MoltTotalUsage()` and three members; `UsageAndQuotaInfoGatherer` drops moltnet-shared's `MoltOnGotGlobalUsage()` and its two members for a `bool`, calls the private `QuotaManagerImpl::EnsureDatabaseOpened()` (the gatherer is a nested class) and reads the counts in `Completed()` | moltnet-shared5 |
| `content/browser/background_fetch/background_fetch_scheduler.{h,cc}`, `background_fetch_context.cc`, `components/download/content/factory/download_service_factory_helper.cc` (new upstream copies), `content/browser/background_fetch/molt_background_fetch_site_policy.h`, `components/download/internal/background_service/molt_in_memory_download_limits.h` (new, header-only, std-only, in no target's `sources`), `content/public/browser/content_browser_client.{h,cc}`, `chrome/browser/chrome_content_browser_client.{h,cc}`, `chrome/browser/molt_ai/tor/molt_net_network_context_policy.h` | `BackgroundFetchScheduler` gains a public `MoltSchedulePerSite()`, a private `MoltScheduleDownload()` (erases from `controller_ids_` and `active_controllers_` while iterating, `base::circular_deque::erase()`'s returned iterator) and a `std::unique_ptr<MoltBackgroundFetchSites>`; `ScheduleDownload()`, `DidCompleteRequest()`, `FinishJob()` and `OnRegistrationLoadedAtStartup()` are hooked; the scheduler's `.cc` includes `components/services/storage/dom_storage/molt_dom_storage_site.h` (header-only, blink's `StorageKey`, so a registration groups as DOM storage does; `//content/browser` already depends on `//components/services/storage`; if `gn check` objects to a header in no target, list it in that source set); the context's constructor asks the new virtual `ContentBrowserClient::ShouldScheduleBackgroundFetchesPerSite(BrowserContext*)` (default `false`; Chrome: `molt_ai::tor::ScheduleBackgroundFetchesPerSite()`); `BuildInMemoryDownloadService()` raises the three `Configuration` limits with `MoltInMemoryDownloadLimit()` | moltnet-shared5 |
| `net/disk_cache/memory/mem_entry_impl.h` (new upstream copy), `mem_backend_impl.{h,cc}`, `molt_mem_backend_site_lru.h` (new, header-only, in no target's `sources`) | `MemEntryImpl` gains a member `MoltCacheSiteLink<MemEntryImpl> molt_site_link_{this}` (a class template deriving from `base::LinkNode<MoltCacheSiteLink<Entry>>` with a `const raw_ptr<Entry>` to its owner) and an inline accessor; `mem_entry_impl.h` now includes the new header (`base/containers/linked_list.h`, `base/memory/raw_ptr.h`, `<unordered_map>`), so `//net`'s disk cache rebuilds. `MemBackendImpl` replaces its `std::map` of site sizes with `MoltCacheSiteLru<MemEntryImpl>` (an `std::unordered_map<std::string, std::unique_ptr<Site>, NameHash, std::equal_to<>>`, heterogeneous lookup with a transparent hash); `OnEntryInserted()`, `OnEntryUpdated()` and `OnEntryDoomed()` touch it under a budget only; `MoltEvictSiteTill()` passes two lambdas to the template's `EvictTill()`; `MoltDropExpiredEntries()` stops at the first entry that hasn't expired | moltnet-shared6 |
| `components/services/storage/dom_storage/storage_area_impl.{h,cc}`, `session_storage_area_impl.{h,cc}` (new upstream copies), `session_storage_data_map.{h,cc}`, `local_storage_impl.{h,cc}`, `session_storage_impl.{h,cc}`, `molt_dom_storage_fair_queue.h`, `molt_dom_storage_fair_scheduler.h` (new, header-only, in no target's `sources`), `molt_dom_storage_site_policy.h`, `molt_dom_storage_site_budget.h` | MoltNet: a sixth `StorageAreaImpl::Delegate` virtual `MoltTaskRunner()` returning `scoped_refptr<base::SequencedTaskRunner>` (default null), passed to `receivers_.Add(this, receiver, runner)` in `Bind()` and to `mojo::Remote(observer, runner)` in `AddObserver()`; `SessionStorageAreaImpl::Bind()` and `AddObserver()` the same with `shared_data_map_->MoltTaskRunner()` (`RemoteSet::Add(PendingRemote, runner)`); `SessionStorageDataMap::Listener::MoltTaskRunner(const blink::StorageKey&)`; `LocalStorageImpl` and `SessionStorageImpl` hold a `scoped_refptr<MoltDomStorageFairScheduler>` made in `MoltKeepCachesPerSite()` from `base::SequencedTaskRunner::GetCurrentDefault()`. `MoltDomStorageSiteTaskRunner` derives from `base::SequencedTaskRunner` (overrides `PostDelayedTask`, `PostNonNestableDelayedTask`, `RunsTasksInCurrentSequence`, private destructor) and the scheduler from `base::RefCountedThreadSafe` with a `base::Lock` and `GUARDED_BY`; both define every member out of the class body, `inline`, for the chromium-style plugin. `StorageAreaImpl::Put()` gains a `bool molt_new_key`, the inline `molt_item_count()` and `MoltDomStorageCountedBytes()` in its budget calls (also `Delete()` and `OnForkStateLoaded()`); `storage_area_impl.cc` now includes `molt_dom_storage_site_policy.h`. `MoltDomStorageSiteTotals` and `MoltDomStorageSiteBudget` declare their constructors and destructors and define them after the class | moltnet-shared6 |
| `services/network/network_context.{h,cc}`, `molt_own_session_sockets_policy.h`, `molt_refusing_p2p_socket_manager.h` (new, header-only, in no target's `sources`) | MoltNet: `NetworkContext::MoltAllowsOwnSessionSockets()` (reads `params_`, null for the URLRequestContext-only constructor; calls the namespace function by its qualified name, `network::MoltAllowsOwnSessionSockets()`); `CreateProxyResolvingSocketFactory()` returns early, dropping the receiver; `CreateP2PSocketManager()` (under `IS_P2P_ENABLED`) binds the renderer's receiver with `MoltRefusingP2PSocketManager::Bind()` (`mojo::MakeSelfOwnedReceiver`; the class implements `mojom::P2PSocketManager` with out-of-class inline definitions) and returns; the new header is included under the same `#if`. The raw socket entry points return early too: `CreateTCPConnectedSocket()` runs its callback with `net::ERR_ACCESS_DENIED`, two `std::nullopt`s and default-constructed `mojo::ScopedDataPipeConsumerHandle`/`ProducerHandle`; `CreateTCPServerSocket()`, `CreateTCPBoundSocket()` and `CreateRestrictedUDPSocket()` with `net::ERR_ACCESS_DENIED` and `std::nullopt`; `CreateUDPSocket()`, `CreateSocketFactory()` and `CreateMdnsResponder()` (before its `#if BUILDFLAG(ENABLE_MDNS)`) drop their pipes | moltnet-shared6 |
| `components/download/internal/common/download_item_impl.cc` (new upstream copy), `molt_resumed_fetch.h` (new, header-only, listed in `:molt_fetch_credentials` with `molt_fetch_credentials.h`, which now also has `//net` and `//url` as public deps) | every profile: in `ResumeInterruptedDownload()`, before `TransitionTo(RESUMING_INTERNAL)`, `MoltKeepResumedFetchMetadata(download_source_ == DownloadSource::INTERNAL_API, request_info_.request_initiator, request_info_.isolation_info, request_info_.credentials_mode, *download_params)` (a template on the parameters' type: `set_initiator()`, `set_isolation_info()` for a non-empty isolation, `set_update_first_party_url_on_redirect(false)`, `set_credentials_mode()`, only for a download-service download whose first request had an initiator) | redteam3 (round 29) |
| `services/network/network_context.cc`, `molt_own_session_sockets_policy.h` | MoltNet: `ResolveHost()`, after the ended-identity check, answers `OnComplete(net::ERR_ACCESS_DENIED, net::ResolveErrorInfo(net::ERR_ACCESS_DENIED), {}, {})` through a temporary `mojo::Remote<mojom::ResolveHostClient>` when `!MoltAllowsOwnSessionSockets()`; `CreateHostResolver()` returns early (dropping the receiver) after its ended-identity check | redteam3 (round 29) |
| `content/browser/background_fetch/background_fetch_scheduler.{h,cc}` | MoltNet: a member `bool molt_pending_changed_` (set in `OnRegistrationCreated()` after `controller_ids_.push_back()`, and in `FinishJob()` for a registration that was active); `MoltScheduleDownload()`'s first step runs only while it is set (and clears it), keeps the active storage keys in a `std::set<blink::StorageKey>` (`#include <set>`; `StorageKey` has `operator<=>`), takes each pending id by reference (used before its `erase()`), and works out `MoltSiteOf()` only for a key not active; the activations are the same | redteam3 (round 29) |
| `components/services/storage/storage_service_impl.{h,cc}` (new upstream copies), `dom_storage/molt_dom_storage_own_thread.{h,cc}` (new, in `//components/services/storage:storage`), `content/browser/dom_storage/dom_storage_context_wrapper.cc` | `StorageServiceImpl` gains a member `MoltDomStorageOwnThreads molt_own_threads_` (its header now includes `molt_dom_storage_own_thread.h`, which includes the two control mojoms and `base/threading/sequence_bound.h`) and two overrides. The new classes: `MoltPerSiteLocalStorage` / `MoltPerSiteSessionStorage` implement the controls and own a `scoped_refptr<MoltDomStorageFairScheduler>`, a `mojo::Remote` to the implementation's own control pipe and `std::unique_ptr<LocalStorageImpl>` / `<SessionStorageImpl>` (forward-declared in the header, destructors defined in the `.cc`; constructed with `base::DoNothing()` as the destruct callback); their receiver is bound with `Bind(receiver, scheduler_->RunnerFor(...))`; per-key messages go through `scheduler_->Post(site, base::BindOnce(&...InTurn, weak, ...))`. `MoltDomStorageOwnThreads` keeps `std::map<uint64_t, base::SequenceBound<...>>`, made on `base::ThreadPool::CreateSingleThreadTaskRunner({USER_BLOCKING, SKIP_ON_SHUTDOWN}, SingleThreadTaskRunnerThreadMode::DEDICATED)` with `base::BindPostTaskToCurrentDefault()`. The wrapper calls the new methods for an in-memory partition (`GetStoragePartitionPath()` empty) whose embedder keeps DOM storage per site | moltnet-shared7 |
| `components/services/storage/dom_storage/storage_area_impl.{h,cc}`, `molt_dom_storage_split_work.h` (new, std-only) | every profile compiles it, MoltNet's partition runs it: `StorageAreaImpl` gains `MoltNotify` (`base::RepeatingCallback<void(blink::mojom::StorageAreaObserver*)>`), `MoltAskSplitRunner()`, `MoltSplitsNotifications(size_t)`, `MoltNotifyObservers()`, `MoltNotifyObserver(mojo::RemoteSetElementId, MoltNotify)` and two members; the header now includes `mojo/public/cpp/bindings/remote_set.h`. The `.cc` binds free functions in an anonymous namespace with `base::BindRepeating(&MoltSendKeyChanged, key, value, std::move(old_value), source.Clone())` (a move-only `StorageAreaSourcePtr` bound to a repeating callback, passed on as `const&`), walks `observers_` with the iterator's `id()` and `observers_.Get(id)`, and frees maps through a function template `MoltFreeChunks<Map>` posted with `base::BindOnce(&MoltFreeChunks<Map>, runner, std::move(map))`; the destructor ends with two `MoltFreeInTurns()` calls | moltnet-shared7 |
| `components/services/storage/dom_storage/local_storage_impl.{h,cc}`, `session_storage_impl.{h,cc}`, `session_storage_namespace_impl.{h,cc}`, `session_storage_area_impl.cc`, `session_storage_data_map.{h,cc}`, `molt_dom_storage_fair_scheduler.h`, `molt_dom_storage_fair_queue.h`, `molt_dom_storage_site_policy.h` | `MoltKeepCachesPerSiteOn(scoped_refptr<MoltDomStorageFairScheduler>)` on both implementations; `SessionStorageNamespaceImpl::Delegate` gains a non-pure virtual `MoltNamespaceTaskRunner(const std::string&)` (defined in the `.cc`; `SessionStorageNamespaceImplTest`'s mock needs nothing) used in `receivers_.Add(this, receiver, runner)`; `SessionStorageDataMap::Listener` gains a non-pure `MoltAreaUnbound(const std::string&, const blink::StorageKey&)`, called from `SessionStorageAreaImpl::OnConnectionError()`; `LocalStorageImpl::MoltAreaUnused()` / `MoltDropIfUnused()` post to the site's runner with `weak_ptr_factory_`. The scheduler now uses `base::ThreadTicks::IsSupported()`, `WaitUntilInitialized()` and `Now()` (`base/time/time.h`), and `molt_dom_storage_site_policy.h` defines `kMoltDomStorageControlGroup` (`inline constexpr char[]`), `MoltDomStorageNamespaceGroup()` and `MoltDomStorageChargeFor()` | moltnet-shared7 |
| `net/disk_cache/memory/mem_backend_impl.{h,cc}`, `molt_mem_backend_site_lru.h` | `MoltCacheSiteLru::Touch()` takes a third argument (`int64_t` microseconds); the template now has `Reserve(size_t, uint64_t)`, `HasRoomFor()`, `StartSweep()`, `SweepStep()` (three lambdas) and a nested `Site` deriving from `base::LinkNode<Site>` with a `raw_ptr<Site>` cursor; its `NameHash` holds a seed (an `std::unordered_map` constructed with a bucket count and a hasher, then move-assigned). `MemBackendImpl` replaces `MoltDropExpiredEntries()` with `MoltStartSweep()` / `MoltSweepStep()` (posted with `weak_factory_`), adds `static MoltNowUs()` and `MoltIsExpiredSince()`; `MoltIsExpired()` is no longer `const`; the `.cc` includes `base/rand_util.h` and `base/task/sequenced_task_runner.h` | moltnet-shared7 |
| `content/browser/background_fetch/background_fetch_scheduler.{h,cc}`, `molt_background_fetch_site_policy.h`, `background_fetch_context.cc`, `content/public/browser/content_browser_client.{h,cc}`, `chrome/browser/chrome_content_browser_client.{h,cc}`, `components/download/internal/background_service/molt_in_memory_download_limits.h` | supersedes the rows above on `molt_pending_changed_` and `MoltScheduleDownload()`: the scheduler has `MoltSchedule(const std::string& site)`, `MoltSchedulePerSite(base::RepeatingClosure)` (which sets `max_running_downloads_` to `std::numeric_limits<int>::max()`; the `.cc` includes `<limits>`) and a member `base::RepeatingClosure molt_on_growth_beyond_floor_`; `ScheduleDownload()` returns early when per site; `FinishJob()` and `DidCompleteRequest()` read the registration's site before the controller goes. The policy's `MoltBackgroundFetchSites` keeps `std::deque<std::string>` lists and a `std::multiset<std::string>` per site; `NextToStart()` takes `const std::function<bool(const std::string&)>&`. `ContentBrowserClient` gains a virtual `GetBackgroundFetchGrowthBeyondFloorCallback(BrowserContext*)` returning `base::RepeatingClosure` (null by default; `ChromeContentBrowserClient` returns `MoltNetMemoryValveCallback()`); `kMoltInMemoryDownloadsLimit` is `UINT32_MAX` and `MoltInMemoryDownloadLimit()` ignores its argument | moltnet-shared8 |
| `components/services/storage/dom_storage/local_storage_impl.{h,cc}`, `session_storage_impl.{h,cc}`, `session_storage_namespace_impl.{h,cc}`, `molt_dom_storage_own_thread.{h,cc}`, `molt_dom_storage_split_work.h`, `molt_dom_storage_site_policy.h`, `molt_dom_storage_site_budget.h` | MoltNet's partition: both implementations gain a nested `class MoltUsageWalk` (`base::RefCounted`, declared in the header, defined in the `.cc`) bound into tasks as `scoped_refptr` with `std::optional<blink::StorageKey>` / `std::optional<int64_t>` resume points, and `MoltDropNonce()` / `MoltDropGroup()`; `LocalStorageImpl` keeps `molt_group_areas_` (`std::map<std::string, std::set<blink::StorageKey>, std::less<>>`, declared before `areas_`: `StorageAreaHolder`'s destructor unindexes itself) and `molt_closed_groups_`. `SessionStorageImpl` gains a nested `struct MoltNamespaceJob` (out-of-line special members; it holds `SessionStorageNamespaceImpl::StorageKeyAreas` and a `std::unique_ptr<SessionStorageNamespaceImpl>`) in a `std::deque` declared after `namespaces_`, `MoltCollectGroups()` taking `base::OnceCallback<void(std::set<std::string>)>` (replacing `MoltGroupsOfNamespace()`), and uses `SessionStorageMetadata::GetOrCreateNamespaceEntry()`, `namespace_storage_key_map()`, `TakeNamespace()` and `SharedMapLocator::AddSession()`, `RemoveSession()`, `Clone()` directly; its header includes `<deque>` and `base/functional/callback.h`. `SessionStorageNamespaceImpl` gains `MoltPopulateWithClonedAreas()`, `MoltCloseReceivers()` (`receivers_.Clear()`), `MoltFitsOneFreeStep()` and `MoltFreeAreasChunk()` (`HasOneRef()` on the data map, an anonymous-namespace helper returning `std::pair<size_t, size_t>`). The host's `MoltPerSiteSessionStorage` keeps nested `HeldMessage` and `Barrier` structs (out-of-line special members), binds `&SessionStorageImpl::CloneNamespace` / `DeleteNamespace` with `base::Unretained(impl_.get())` and `&MoltPerSiteSessionStorage::ReplayHeld` with `weak_factory_` into a `base::RepeatingClosure`. The budget's commit rates and write allowances are a template `MoltDomStorageExpiringRecords<Value>` (`std::map<std::string, Entry, std::less<>>` and `std::set<std::pair<int64_t, std::string>>`) | moltnet-shared8 |
| `content/public/browser/dom_storage_context.h`, `content/browser/dom_storage/dom_storage_context_wrapper.{h,cc}` (both new upstream copies, the wrapper's header too), `storage/browser/quota/quota_manager_impl.{h,cc}` (`quota_manager_impl.h` a new upstream copy), `usage_tracker.{h,cc}`, `molt_storage_usage_counts.h`, `chrome/browser/molt_ai/tor/molt_net_nonce_storage.{h,cc}`, `molt_net_nonce_storage_policy.h` (new), `chrome/browser/ui/tab_helpers.cc` | `DOMStorageContext` gains a non-pure virtual `MoltDropNonce(const base::UnguessableToken&, bool)` with an empty body (the header forward-declares `base::UnguessableToken`; test fakes need nothing), which the wrapper overrides; `QuotaManagerImpl` gains `MoltDropNonce(const base::UnguessableToken&, const std::vector<blink::StorageKey>&, bool)`, `MoltIsSiteClosed()` (read by `UsageAndQuotaInfoGatherer::Completed()` through `manager()`) and `std::set<MoltBudgetSite> molt_closed_sites_` (its header includes `molt_storage_budget.h`; the site is built as `MoltBudgetSite{nonce, std::nullopt}`); `UsageTracker` gains `MoltForgetSite()`, `MoltRefreshStaleKeys()`, `MoltDidRefresh()`, `MoltMarkStale()`, `MoltSettle()` (posted delayed with `weak_factory_`), a `MoltSiteRefreshes<MoltBudgetSite, base::OnceClosure>` and a `std::set<MoltBudgetSite>`. The tab helper is a `WebContentsObserver` + `WebContentsUserData` with a `content::PageUserData` per page and a `base::SupportsUserData::Data` registry on the `BrowserContext`, and posts `&storage::QuotaManagerImpl::MoltDropNonce` to `content::GetIOThreadTaskRunner({})` with `base::WrapRefCounted()` on the `storage::QuotaManager*` (a `QuotaManagerImpl` subclass) | moltnet-shared8 |
| `services/network/keepalive_statistics_recorder.{h,cc}`, `molt_keepalive_partitions.h`, `storage/browser/blob/molt_blob_budget_policy.h`, `components/download/internal/background_service/in_memory_download.cc`, `molt_registering_site_isolation.h` | the recorder gains `MoltIsIdle() const`, `MoltNotifyIfIdle()`, a static `MoltForgetIfIdle(base::WeakPtr<KeepaliveStatisticsRecorder>, uintptr_t, std::string)` bound with `AsWeakPtr()` into a `base::RepeatingClosure molt_on_idle_` and posted with `base::SequencedTaskRunner::GetCurrentDefault()`; the registry gains `ForgetIfIdle()`. `MoltBlobSiteKeys` is gone (supersedes moltnet-shared3's rows): `in_memory_download.cc` takes a site's key from `MoltBlobSiteKeyFromDigest()` of the first 8 bytes of `base::SHA1Hash()` over a 32-byte `base::RandBytes()` secret and the site (`base/hash/sha1.h`, `base/rand_util.h`, `base/numerics/byte_conversions.h`), and `MoltBlobBudgetKeyForFetch()` is now a template over the digest function | moltnet-shared8 |
| `content/browser/download/download_manager_impl.cc`, `molt_resumed_fetch_factory.h` (new, header-only, in `:browser`), `molt_resumed_fetch_policy.h` (new, std-only, in `:molt_policies`); `components/download/internal/background_service/controller_impl.cc`, `molt_restored_fetch.h` | every profile: the default-factory branch of `BeginResourceDownloadOnChecksComplete()` first asks `MoltLnaFactoryForFetch()` (taking `download::DownloadUrlParameters&`: its `url_loader_factory()` isn't `const`), which builds a `network::mojom::ClientSecurityState` with `DeriveLocalNetworkAccessRequestPolicy(IPAddressSpace::kPublic, bool, LocalNetworkAccessRequestContext::kWorker)` and `OverrideLocalNetworkAccessPolicy()` (`content/browser/renderer_host/local_network_access_util.h`), `StoragePartitionImpl::CreateURLLoaderFactoryParams()` and `CreateURLLoaderNetworkObserverForServiceOrSharedWorker(network::OriginatingProcessId::renderer(network::RendererProcessId(ChildProcessHost::kInvalidUniqueID)), origin)`, then `network::WrapperPendingSharedURLLoaderFactory`. The controller (on redteam4's hook): `OnDownloadReadyToStart()` posts a delayed lambda that reaches `model_`, `pending_uploads_` and `HandleCompleteDownload()` through a `base::WeakPtr<ControllerImpl>` (a lambda in a member function may), and `StartDownload()`'s `UNEXPECTED_GUID` branch calls `UpdateDriverState()` | moltnet-shared8 |
| `components/services/storage/dom_storage/session_storage_impl.cc`, `chrome/browser/molt_ai/tor/molt_net_nonce_storage.cc`, `molt_net_nonce_storage_policy.h` | every profile: `RegisterShallowClonedNamespace()` returns before `receiver_.ReportBadMessage()` (a populated destination) when `!mojo::IsInMessageDispatch()` (declared in `mojo/public/cpp/bindings/message.h`, `//mojo/public/cpp/bindings:bindings_base`); MoltNet: the tab helper counts a page to `MoltNetNonceGroupSite()` of its main document's origin (`url::Origin::GetTupleOrPrecursorTupleIfOpaque()`, `url::SchemeHostPort::IsValid()` and `GetURL()`, `net::SchemefulSite(const GURL&)`) | redteam5 (round 31) |
| `chrome/browser/molt_ai/tor/molt_net_nonce_storage.{h,cc}`, `molt_net_nonce_storage_policy.h`, `content/browser/storage_partition_impl.cc` | MoltNet: the tab helper overrides `DidStartNavigation()` and `DidRedirectNavigation()` and reads `NavigationHandle::GetIsolationInfo().nonce()`, `GetParentFrameOrOuterDocument()` and `GetURL()`; it builds `net::CookiePartitionKey::FromWire(net::SchemefulSite(url), AncestorChainBit::kCrossSite, nonce)` and `StorageKey::ToCookiePartitionKey()`, and deletes through `StoragePartition::GetCookieManagerForBrowserProcess()->DeleteCookies()` (a `network::mojom::CookieDeletionFilter` with `partitioned_state_only` and a `net::CookiePartitionKeyCollection` made from a `base::flat_set`), `GetCdmStorageDataModel()->DeleteDataForStorageKey()` under `BUILDFLAG(ENABLE_LIBRARY_CDMS)` (`media/media_buildflags.h`), and a second deletion posted with `content::GetUIThreadTaskRunner({})->PostDelayedTask()` and the registry's `base::WeakPtrFactory`. `GetPartitionUUIDPerStorageKey()` first asks a new anonymous-namespace `MoltDerivedNonceKeyUUID()`: `crypto::hmac::SignSha256()` (`crypto/hmac.h`, `crypto/hash.h`), a function-local `static const std::array<uint8_t, 32>` (made by a lambda), `base::RandBytes()`, `base::U64FromLittleEndian()` on fixed-extent spans, `base::UnguessableToken::Deserialize()` | moltnet-shared9 |
| `components/services/storage/dom_storage/local_storage_impl.{h,cc}`, `session_storage_impl.{h,cc}`, `molt_dom_storage_own_thread.{h,cc}`, `content/browser/dom_storage/dom_storage_context_wrapper.{h,cc}`, `content/browser/browsing_data/browsing_data_filter_builder_impl.h` (new upstream copy), `molt_one_key_deletion_policy.h` (new, std-only, in `:molt_policies`), `content/browser/storage_partition_impl.cc` | MoltNet: `MoltGetUsageForStorageKey()` on both implementations and hosts (in the key's site's turn); the wrapper gains `MoltGetLocalStorageUsageForStorageKey()`, `MoltGetSessionStorageUsageForStorageKey()` and `MoltKeepsPerSite()`; `BrowsingDataFilterBuilderImpl` an inline `molt_storage_key()`; `ClearQuotaManagedData()`, `ClearLocalStorage()` and `ClearSessionStorage()` take a defaulted `std::optional<blink::StorageKey> molt_only_key`, and `DataDeletionHelper::ClearData()` `static_cast`s the `BrowsingDataFilterBuilder*` to the impl (its only implementation); the quota branch binds `QuotaManagerImpl::GetBucketsForStorageKey()` (`delete_expired=false`) on the IO thread with a lambda turning `QuotaErrorOr<std::set<BucketInfo>>` into `std::set<BucketLocator>` | moltnet-shared9 |
| `components/download/internal/background_service/controller_impl.{h,cc}` (`controller_impl.h` a new upstream copy), `molt_restored_fetch.h` | every profile: `ControllerImpl` gains a member `MoltFetchTakeUps molt_fetch_take_ups_` (the header includes `molt_restored_fetch.h`); `StartDownload()`'s `UNEXPECTED_GUID` branch switches on `MoltOnFetchTakenUp()` (an `enum class` with three cases, no `default:`); `OnDownloadReadyToStart()` takes the remembered take-up before its wait and calls `UpdateDriverState()` in the wait branch; `HandleCompleteDownload()` forgets the GUID | moltnet-shared9 |
| `components/services/storage/dom_storage/session_storage_impl.{h,cc}`, `session_storage_namespace_impl.h`, `molt_dom_storage_own_thread.{h,cc}`, `molt_dom_storage_waits.h` (new, std-only, in `//components/services/storage:storage`) | MoltNet's partition: `SessionStorageImpl`'s job queue becomes `MoltDomStorageNamespaceJobs molt_job_order_` and `std::map<uint64_t, MoltNamespaceJob> molt_jobs_` (the job holds a `std::unique_ptr<SessionStorageNamespaceImpl>` and the cloned areas: move-only, emplaced by id; the header no longer includes `<deque>`); `MoltRunJobStep(uint64_t)` and a new `MoltPostJobStep(uint64_t)` post to `MoltDomStorageNamespaceGroup()`; a new `MoltClonesWaitingFor()` returns a `std::vector<std::string>` built from the namespace's new inline `molt_children_waiting_for_clone()` (a `const base::flat_set<std::string>&`). The host's `Barrier` loses `turns_left` and gains `source` and `std::optional<std::string> copy`; `AfterTurnsOf()` takes the copy; the deadline is posted with `scheduler_->RunnerFor(kMoltDomStorageControlGroup)->PostDelayedTask(FROM_HERE, ..., base::Milliseconds(kMoltDomStorageTurnsWaitMs))`; a `std::map<std::string, std::multiset<blink::StorageKey>> queued_deletions_` | moltnet-shared9 |
| `net/cookies/cookie_monster.{h,cc}` (new upstream copies), `net/cookies/molt_nonce_cookie_policy.h` (new, std-only, in `//net:molt_policies`), `services/network/network_context.cc` | MoltNet's context: `CookieMonster` gains `MoltKeepNonceCookieLimits()`, `MoltKeepsNonceCookieLimits() const`, `MoltDropNonce(const base::UnguessableToken&, bool refuse)` (through `DoCookieCallback()` and the store's `weak_ptr_factory_`), `MoltDropNonceNow()`, `MoltGarbageCollectNoncePartition(base::Time, const CookiePartitionKey&, const CanonicalCookie* keep)` (`CookieItVector`, `std::sort` with `LRACookieSorter`, `InternalDeletePartitionedCookie()` with `DELETE_COOKIE_EXPIRED`, `DELETE_COOKIE_EVICTED_PER_PARTITION_DOMAIN` and `DELETE_COOKIE_EXPLICIT`) and a member `std::unique_ptr<MoltNonceCookieTotals<base::UnguessableToken, CookiePartitionKey>>` (the header now includes `base/unguessable_token.h`, `net/cookies/cookie_partition_key.h` and the policy); hooks in `InternalInsertCookie()` (after `num_nonced_partitioned_cookie_bytes_`), `SetCanonicalCookie()` (an `ExclusionReason::EXCLUDE_USER_PREFERENCES` before `MaybeDeleteEquivalentCookieAndUpdateStatus()`, reading `NameValueSizeBytes()`, and the nonce partition's collection after `GarbageCollectPartitionedCookies()`) and `InternalDeletePartitionedCookie()` (the partition's `bytes_per_cookie_partition_` entry erased as it empties, before Chromium's own erase of the partition). `cookie_monster.h` is included across `//net` and `//services/network`: a large rebuild. With `molt_nonce_limits`, `MakeURLRequestContext()` makes the store of a context without a cookie file itself (`std::make_unique<net::CookieMonster>(/*store=*/nullptr, net_log)`, as `URLRequestContextBuilder::Build()` does) and calls `MoltKeepNonceCookieLimits()` on either store | moltnet-shared10 |
| `services/network/network_context.{h,cc}`, `services/network/molt_nonce_drop_policy.h` (new, std-only, in `//services/network:network_service`), `net/http/http_network_session.{h,cc}`, `net/socket/client_socket_pool_manager.{h,cc}`, `client_socket_pool_manager_impl.{h,cc}`, `molt_per_site_socket_pools.h`, `transport_client_socket_pool.h`, `net/spdy/spdy_session_pool.{h,cc}`, `net/ssl/ssl_client_session_cache.{h,cc}`, `net/http/http_auth_cache.{h,cc}`, `net/http/http_server_properties.{h,cc}`, `services/network/cors/preflight_cache.{h,cc}`, `preflight_controller.{h,cc}` (the SPDY, TLS session, auth, server properties and CORS files new upstream copies) | `NetworkContext::MoltDropNonce()` overrides the new mojom method (a `switch` over `mojom::MoltNonceDrop` with no `default:`) and `MoltNonceCookieStore()` `static_cast`s `url_request_context_->cookie_store()` to `net::CookieMonster*` (forward-declared in `network_context.h`); `HttpNetworkSession::MoltForgetNonce()` calls a new virtual `ClientSocketPoolManager::MoltCloseIdleSocketsOfNonce(const base::UnguessableToken&, const char*)` (an empty body in the base class, overridden by `ClientSocketPoolManagerImpl`, which asks the per-site pools; a test manager elsewhere inherits the empty one), `SpdySessionPool::MoltCloseIdleSessionsOfNonce()` (as `CloseCurrentSessionsHelper()`, idle sessions only), `SSLClientSessionCache::MoltFlushForNonce()` (`base::LRUCache::Erase(iterator)`), `HttpAuthCache::MoltClearEntriesForNonce()` (`std::erase_if` on the `std::multimap`, `EntryMapKey::network_anonymization_key`) and `HttpServerProperties::MoltClearForNonce()` (`Erase(iterator)` on `server_info_map_`, `servers_requiring_http_11_` and `quic_server_info_map_`, `base::EraseIf` on the two canonical `base::flat_map`s, then `MaybeQueueWriteProperties()`; the `.cc` now includes `base/containers/flat_map.h`); `TransportClientSocketPool::MoltCloseIdleSocketsOfNonce()` is inline (reads `group_map_`, `idle_socket_count_` and `GroupId::network_anonymization_key()`, calls `CloseIdleSocketsInGroup()`); `PreflightController::MoltClearCorsPreflightCacheForNonce()` calls `PreflightCache::MoltClearForNonce()` (`std::erase_if` on the cache's `std::map`, `std::get<2>` of its `std::tuple` key, a `net::NetworkIsolationKey`). Each header that gains a method now includes `base/unguessable_token.h`; `http_network_session.h`, `ssl_client_session_cache.h`, `http_server_properties.h` and `spdy_session_pool.h` are widely included in `//net` | moltnet-shared10 |
| `chrome/browser/molt_ai/tor/molt_net_nonce_storage.{h,cc}`, `molt_net_nonce_storage_policy.h`, `storage/browser/quota/quota_manager_impl.{h,cc}`, `quota_database.{h,cc}` (the database's new upstream copies), `content/browser/storage_partition_impl.cc`, `content/browser/browsing_data/molt_one_key_deletion_policy.h` | MoltNet: the tab helper no longer builds cookie partition keys or calls `CookieManager::DeleteCookies()`: it calls `StoragePartition::GetNetworkContext()->MoltDropNonce()` with a `network::mojom::MoltNonceDrop` (the `.cc` includes `services/network/public/mojom/network_context.mojom.h`), counts the pages using each nonce (`MoltNetNoncePageCounts`; `MoltNetNoncePage::Noted()` returns `bool`) and closes a group past `kMoltNetNonceStorageKeysPerGroup` (`MoltNetNonceGroups::Close()`); `QuotaManagerImpl::UpdateOrCreateBucket()` answers `base::unexpected(QuotaError::kQuotaExceeded)` for a closed site (`MoltIsSiteClosed(MoltBudgetSiteOf(...))`, after the expiration check); the new `QuotaManagerImpl::MoltGetBucketsForStorageKeyModifiedBetween()` posts to the database sequence as `GetBucketsModifiedBetween()` does and replies through `DidGetModifiedBetween()`; `QuotaDatabase::MoltGetBucketsForStorageKeyModifiedBetween()` runs a cached `sql::Statement` (`BindString()`, `BindTime()`, `ColumnStringView()`); `ClearQuotaManagedData()` binds it for a one-key deletion over a range; `OnClearSiteData()` builds `blink::StorageKey::CreateFirstParty()` or `CreateWithNonce()` from `url::Origin::Create(url)` when `MoltClearSiteDataKeyFor()` says so (`base::OptionalToPtr()` on `CookiePartitionKey::nonce()`, `DOMStorageContextWrapper::MoltKeepsPerSite()`), and `DataDeletionHelper::ClearData()` reads `molt_one_key->origin().opaque()` | moltnet-shared10 |
| `content/browser/storage_partition_impl.cc`, `content/browser/browsing_data/molt_one_key_deletion_policy.h` | `OnClearSiteData()`'s MoltBrowser `switch` gains `MoltClearSiteDataKey::kInRequestPartition`: `blink::StorageKey::Create(url::Origin, cookie_partition_key->site(), blink::mojom::AncestorChainBit)` (the default `third_party_partitioning_allowed`), `net::CookiePartitionKey::IsThirdParty()`, `net::SchemefulSite::IsSameSiteWith(const url::Origin&)`; `MoltClearSiteDataKeyFor()` takes a fifth `bool` and `MoltClearSiteDataKeyIsCrossSite()` is new (both `constexpr`, std-only) | redteam7 |
| `chrome/browser/net/profile_network_context_service.cc`, `chrome/browser/molt_ai/tor/molt_net_nonce_storage.{h,cc}`, `molt_net_nonce_storage_policy.h`, `net/cookies/cookie_monster.{h,cc}`, `services/network/network_context.cc` | `ConfigureNetworkContextParamsInternal()` sets `molt_refused_nonces` for MoltNet from the new `molt_ai::tor::MoltNetRefusedNonces(content::BrowserContext*, const base::FilePath&)` with `profile_->GetPath().Append(relative_partition_path)` (as `StoragePartitionImpl` makes its path; the `.cc` includes `molt_net_nonce_storage.h`); that function reads the tab helpers' registry with `GetUserData()` only (it makes none), whose new `MoltNetNonceRefusals<base::UnguessableToken, base::FilePath>` `Drop()` fills from `StoragePartition::GetPath()`; the new `CookieMonster::MoltRefuseNonces(const std::vector<base::UnguessableToken>&)` sets the totals' refusals at once, not through `DoCookieCallback()` (which would mark the store initialized before `CookieManager`'s `SetCookieableSchemes()`), and `MakeURLRequestContext()` calls it through an anonymous-namespace helper (a `static_assert` on `net::MoltNonceCookiesRefusedAfter()`) after each `MoltKeepNonceCookieLimits()` call | moltnet-shared11 |
| `net/cookies/cookie_monster.{h,cc}`, `net/cookies/molt_nonce_cookie_policy.h` | MoltNet's context: `InternalInsertCookie()` and `InternalDeletePartitionedCookie()` count a cookie of a partition without a nonce too (`MoltNonceCookieTotals::AddedToPartition()` / `RemovedFromPartition()`, an `else if` after the nonce's branch); `MoltGarbageCollectNoncePartition()` starts on `OverLimits(nonce, key)` (`nonce` the partition key's `std::optional`) and evicts while `AboveTarget()`; `MoltKeepNonceCookieLimits()` counts the cookies already held in partitions without a nonce; the policy's `MoltNonceCookieTotals` gains a `std::map<PartitionKey, MoltNonceCookieCounts>` | moltnet-shared11 |
| `net/base/molt_site_share_policy.h` (new, std-only, in `//net:molt_policies`), `net/base/molt_network_key_site.h`, `net/base/molt_site_lru_cache.h` (new, header-only, in `//net`'s `sources`), `net/socket/molt_connection_budget.h` | `MoltSiteLRU<Base, SiteOfKey>` derives from `base::LRUCache<K, V>` or `base::LRUCacheSet<K>` (`using Base::Base;`; `Base::NO_AUTO_EVICT`, `Base::Swap()`, `Peek()`, `rbegin()`/`rend()`) and hides `Get()` (a template, as the base's), `Put(value_type&&)`, `Put(K&&, V&&)`, `Erase(iterator)`, `Erase(reverse_iterator)`, `Clear()`, `Swap(Base&)`, `ShrinkToSize()`, `UpdateMaxSize()` and `max_size()`; without shares each calls the base's. `MoltSiteOfNetworkKey()` takes `NetworkAnonymizationKey` or `NetworkIsolationKey` (`GetTopFrameSite()`, `SchemefulSite::MoltGetTupleOrPrecursorTupleIfOpaque()`); `MoltBudgetSiteFor()` now calls it (`MoltBudgetSite` is the same `std::optional<SchemefulSite>`) | moltnet-shared11 |
| `net/ssl/ssl_client_session_cache.{h,cc}`, `net/http/http_server_properties.{h,cc}`, `net/http/http_auth_cache.{h,cc}`, `services/network/cors/preflight_cache.{h,cc}`, `preflight_controller.{h,cc}`, `services/network/shared_dictionary/shared_dictionary_manager.{h,cc}`, `shared_dictionary_manager_in_memory.{h,cc}` (the dictionary files new upstream copies), `services/network/network_context.cc` | MoltNet's per-site shares: `SSLClientSessionCache::cache_` is a `MoltSiteLRU<base::LRUCache<Key, Entry>, MoltKeySite>` (`Insert()` returns early when `MoltMakeRoomFor()` refuses; `OnUpdateMemoryLimit()`/`OnReleaseMemory()` return early with shares); `ServerInfoMap` derives from `MoltServerInfoMapBase` (`GetOrPut()` may return `end()` with shares, and `SetSupportsSpdy()`, `SetAlternativeServicesInternal()` and `SetServerNetworkStatsInternal()` return on it) and `servers_requiring_http_11_` is a `MoltSiteLRU<base::LRUCacheSet<ServerInfoMapKey>, ...>` (`SetHTTP11RequiredInternal()` builds the key first); `HttpAuthCache::Add()` may return null with shares (`HttpAuthController` and `NetworkContext::AddAuthCacheEntry()` ignore the result; `CopyProxyEntriesFrom()` skips an entry not kept) and skips the global eviction; `PreflightCache` keeps a `std::unique_ptr<net::MoltSiteShares<...>>` updated in `AppendEntry()`, `CheckIfRequestCanSkipPreflight()`, `ClearCache()` and `MoltClearForNonce()` (its `std::erase_if` lambda now captures `this`), and `MayPurge()` returns early with it; `SharedDictionaryManager::cached_storages_` is a `MoltSiteLRU` keyed by `SharedDictionaryIsolationKey::top_frame_site()`, `GetStorage()` returns before the memory-limit branch with shares and `OnReleaseMemory()` returns early; `SharedDictionaryManagerInMemory::SetCacheMaxSize()`, `MaybeRunCacheEvictionPerSite()` (to the new private `MoltRunCacheEvictionForSite()`, which reuses the file's `DictionaryReference`, `LastUsedTimeLess` and `EvictionCandidate`) and `MaybeRunCacheEviction()` follow `MoltKeepsPerSiteShares()`. `NetworkContext`'s constructor calls `MoltKeepPerSiteShares()` on the dictionary manager (if any) and `cors_preflight_controller_`, and `MakeURLRequestContext()` on `GetSession()->ssl_client_context()->ssl_client_session_cache()`, `http_auth_cache()` and `http_server_properties()` (it now includes `net/http/http_auth_cache.h` and `net/ssl/ssl_client_session_cache.h`). `http_server_properties.h` and `ssl_client_session_cache.h` are widely included in `//net` | moltnet-shared11 |
| `services/network/network_context.cc`, `net/http/broken_alternative_services.{h,cc}` (new upstream copies), `net/http/http_server_properties.cc` | `MakeURLRequestContext()` sets `is_quic_force_disabled` for `molt_no_quic` before `network_session_configurator::ParseCommandLineAndFieldTrials()`; `BrokenAlternativeServices::MoltClearForNonce()` (the list, the map, `RecentlyBrokenAlternativeServices::Erase(iterator)` and `std::erase_if` on the `std::set`; the header includes `base/unguessable_token.h`, the `.cc` `<set>`), called by `HttpServerProperties::MoltClearForNonce()` | moltnet-shared11 |
| `net/base/molt_site_share_policy.h`, `net/base/molt_site_lru_cache.h`, `net/http/http_auth_cache.{h,cc}`, `net/http/http_server_properties.{h,cc}` | MoltNet's per-site shares, an entry a cache must keep: `MoltSiteShareTakesFromLargest()` (`constexpr`, std-only); `MoltSiteShares` gains `NextToEvictToKeep()` (a pass over its `std::map` of sites) and a `uint64_t use` in each entry's `Place` with a counter `uses_` (`<cstdint>` was already included); `MoltSiteLRU` gains `MoltMakeRoomToKeep()` and a private `MoltMakeRoom(const key_type&, bool)` that `MoltMakeRoomFor()` now calls; `SetHTTP11RequiredInternal()` calls `MoltMakeRoomToKeep()`; `HttpAuthCache::MoltMakeRoomFor()` builds a `std::map<MoltNetworkKeySite, Held>` (`Held`: a local struct with a `size_t` and an `EntryMap::iterator`; `<map>` was already included) and erases the least recently used entry of the largest site | redteam8 |

### 4.3 Translate (page translation on the device)

| File | Hook | From |
|---|---|---|
| `components/translate/core/browser/translate_driver.h` | `TranslatesOnDevice()`, `TranslationOff()` virtuals | #48 |
| `components/translate/core/browser/translate_manager.cc` | no API-key/offline gate, no partial translate, `TranslatePage` calls the driver, no automatic translate from `hrefTranslate`; no target language added in MoltNet | #48, #55 |
| `components/translate/core/browser/translate_language_list.{h,cc}`, `chrome/browser/translate/translate_service.cc` | local 20-language list, no fetcher | #48 |
| `chrome/browser/translate/chrome_translate_client.cc` | creates `MoltTranslateDriver` | #48 |
| `chrome/browser/ui/views/translate/translate_bubble_view.cc` | footnote with progress, Stop, errors | #48 |
| `chrome/browser/molt_ai/translate/BUILD.gn` (new: `logic`, `io`, `job`, `footnote_view`, `translate`) | the footnote view and the job test on the real `base` | #48 |

### 4.4 Built-in AI APIs (`molt_ai/web_ai/`)

| File | Hook or risk | From |
|---|---|---|
| `chrome/browser/chrome_content_browser_client.cc` | `BindAIManager` → `MoltAIManager`; `BindTranslationManager` → `BindMoltTranslationManager` | #26, #34 |
| the binder's `FindDocument` | `content::internal::GetDocumentUserData` and `RenderProcessHost::ForEachRenderFrameHost` (the PR could not re-read the header) | #34 |
| `third_party/blink/renderer/modules/ai/ai_interface_proxy.cc` | LanguageDetector bodies answer "unavailable" | #32 |
| `chrome/browser/browsing_data/chrome_browsing_data_remover_delegate.cc`, `chrome/browser/profile_resetter/profile_resetter.cc`, `chrome/browser/profiles/chrome_browser_main_extra_parts_profiles.cc` | clear answers and pauses; the keyed-service factory | #32 |
| `components/infobars/core/infobar_delegate.h` | `MOLT_ONION_LOCATION_INFOBAR_DELEGATE` (1000) and `MOLT_WEB_AI_CONSENT_INFOBAR_DELEGATE` (1001) | #6, #26 |
| `molt_privacy_prefs.cc`, the feature-override tests | not compiled in #26 | #26 |
| `web_ai_consent.cc` (now a `WebContentsObserver`), `runtime_web_ai_backend.cc`, the binder's pref observers, `molt_ai_manager.cc` | | #46, #50 |

### 4.5 Omnibox (`@ai`, `@memory`)

| File | Hook | From |
|---|---|---|
| `components/omnibox/browser/autocomplete_controller.{h,cc}` | 2 provider pointers; registration in `InitializeSyncProviders()`; the gate at the top of `ShouldRunProvider()`; `Start()` stops leftover work | #44 |
| `chrome/browser/ui/omnibox/chrome_omnibox_client.cc` | `OnTextChanged` and `OnSuccessfulNavigation` skip local input | #44 |
| `components/omnibox/browser/BUILD.gn` | new std-only targets `molt_memory_query`, `molt_local_input`, `molt_memory_search`, public through `browser` | #44 |
| `components/omnibox/browser/ai_prompt_provider.cc` | the `?` branch removed; full-width folding | #44, #47 |

### 4.6 Sandboxes and utility processes

| File | Hook or risk | From |
|---|---|---|
| `chrome/utility/services.cc`, `chrome/utility/BUILD.gn`, `chrome/utility/DEPS` | the document parser and inference services registered (`!BUILDFLAG(IS_ANDROID)`) | #13, #15 |
| `content/utility/on_device_model/on_device_model_sandbox_init.cc` + `BUILD.gn` | `PreSandboxHook()`: a no-op without `--molt-inference-gpu`; new edge to `//sandbox/linux:sandbox_services` | #52, #55 |
| `ServiceProcessHost::Options().WithProcessCallback()`; anydoc cxx sub-target visibility | | #13 |
| document parser PDF rendering | PDFium config `version = 4`, `gfx::PNGCodec::Encode` with BGRA, spans and `base::U32FromBigEndian` | #18 |
| `molt_ai/common/helper_sandbox_*` (Landlock and seccomp, Seatbelt, job object) | type-checked against 156's headers on Linux and Windows (clang-cl), the POSIX part for the Mac too, on Apple framework stand-ins (round 28) | #16 |
| `molt_ai/common/helper_sandbox_win.cc` (the helpers' LPAC launch), `helper_sandbox_test_helper_win.cc` (a test `executable`), `ocr/ocr_service.cc` (the data decoder, Skia) | new deps: `//sandbox/win:sandbox` (its `AppContainerBase`, `StartupInformationHelper`, `Job`) from `chrome/browser`, `//services/data_decoder/public/cpp` and `//skia` from `molt_ai/ocr`; type-checked with clang-cl and for Linux and the Mac (round 33); `-Wunsafe-buffer-usage` isn't modelled by the compile check | helpers-win |
| `third_party/llama_cpp:mtmd` (exception and CFI configs), `:llama_guard` (a TU built with exceptions), `:constrained_decoding` deps | | #30, #38, #52 |
| `runtime/model_image_prep.cc` (real Skia, the data decoder), `molt_model_impl.cc`, `molt_inference_service.cc` (syntax-checked only) | | #38 |
| `embedding_process_host`, `runtime:embedding_host`, `models:download` | | #28 |
| `inference_process_host.cc` and its tests | | #15, #52, #53 |
| `content/utility/utility_main.cc` (Windows block, before `LowerToken()`) and `content/utility/on_device_model/molt_gpu_inference_sandbox_win.cc`, `molt_gpu_driver_warmup.cc`, `molt_gpu_inference_buildflags` | the GPU warm-up hook; since round 28 `utility_main.cc` (its Windows block included) and the Windows files type-check with clang-cl against the Windows SDK (MSVC's runtime headers are VS 2017's, see the compile check's README) | ai-finish |
| `third_party/llama_cpp:ggml_vulkan` with the switch: the shader pack tool (`//third_party/brotli:enc` for the host, `:dec` in the service), the two `ggml-vulkan.cpp` blocks | built in a stub GN root; MoltBrowser's glue (`chromium/ggml-vulkan-moltbrowser.cpp`, `ggml_vk_glslc.cc`, `ggml_vk_shader_pack.cc`) and the switch's hooks type-check against 156's headers with the switch on (compile check platform `linux-vulkan`, round 28); vendored `ggml-vulkan.cpp` itself is not compiled there | ai-finish |

### 4.7 Blink and fingerprinting (MoltNet uniformity)

| File | Hook | From |
|---|---|---|
| `core/scroll/scrollbar.cc`, `core/scroll/scrollable_area.cc`, `core/paint/paint_layer_scrollable_area.cc`, `core/layout/forms/layout_text_control.cc`, `core/frame/local_frame_view.cc`, `core/frame/dom_visual_viewport.cc` | 16 CSS px scrollbars, whole viewport lengths | #33 |
| `core/html/canvas/text_metrics.{h,cc}`, `modules/canvas/canvas2d/base_rendering_context_2d.cc`, `platform/fonts/font_platform_data.{h,cc}`, `platform/fonts/linux/font_cache_linux.cc` | `measureText` on a grid, one Linux rendering mode, one emoji font | #37, #50 |
| `chrome/browser/ui/views/frame/contents_web_view.{h,cc}`, `chrome/browser/universal_web_contents_observers.cc`, `:letterbox_view` deps, `base::ClampCeil` in `farbling.cc` | letterboxing at every zoom; `RendererZoomLevel()` in `molt_letterbox_view.cc` hand-copies `WebContentsImpl::GetPendingZoomLevel()` | #27 |
| `core/css/media_values.cc`, `core/frame/screen.cc`, `core/events/pointer_event_factory.cc`, `core/input/touch_event_manager.cc`, `platform/text/platform_locale.*` and others | screen, touch and locale uniformity | #20 |
| `modules/webgl/*`, `modules/webgpu/gpu.cc`, `platform/fonts/font_selector.*`, `font_fallback_list.cc`, `css/local_font_face_source.cc` | font allow-lists, uniform WebGL/WebGPU | #19 |
| `content/public/browser/client_hints_controller_delegate.h` (a new virtual in every implementation), `chrome/browser/client_hints/BUILD.gn`, `core/loader/frame_fetch_context.cc`, worker files | client hints and per-document Shields decisions | #24 |
| `platform/network/network_state_notifier.{h,cc}`, `core/molt_fingerprinting/moltnet_network.{h,cc}` | `SetUniformNetworkState()`, an early return in four setters, no per-host factor in `GetRandomMultiplier()` once pinned; a `std::atomic<bool>` member in a header much of Blink includes (expect a large Blink rebuild); `Configure(kUniform)` calls `PinUniformNetworkState()` | network quality |
| `core/molt_fingerprinting` DEPS (`gles2_interface.h`, `site_for_cookies.h`) | | #10 |
| `core/css/media_values.cc`, `core/exported/web_view_impl.cc` (a new upstream copy), `core/molt_fingerprinting/moltnet_display.{h,cc}` | MoltNet's display colour (four `MediaValues::Calculate*` hooks) and scrollbar style (`UpdateRendererPreferences` pins `use_overlay_scrollbar` inside the desktop `#if`) | fingerprinting 156 |
| `modules/webaudio/audio_context.cc`, `realtime_audio_destination_handler.cc`, `modules/encryptedmedia/encrypted_media_utils.cc`, `modules/media_capabilities/media_capabilities.cc`, `modules/webcodecs/gpu_factories_retriever.cc`, `video_decoder_broker.cc` (all six new upstream copies), `core/molt_fingerprinting/moltnet_media{,_values}.{h,cc}` | modules calling a new core glue file (`gn check`: modules already depend on core); `media::AudioGlitchInfo()` and `media::IsDecoderBuiltInVideoCodec` in the hooks; `WebEncryptedMediaClient` subclassed in core (its out-of-line destructor is exported from platform); `WebEncryptedMediaRequest` passed by value | fp-media |
| `platform/audio/audio_destination.{h,cc}`, `modules/mediastream/media_devices.cc` (new upstream copies), `audio_context.cc` (`IsValidSinkDescriptor`), `core/molt_fingerprinting/moltnet_audio_{clock,device}.{h,cc}` | a static setter on the `PLATFORM_EXPORT` `AudioDestination` holding a function pointer in a `std::atomic`, called from core; core now uses `media::FakeAudioWorker`, `media::MultiChannelResampler`, `media::AudioBus` and `NonMainThread` with `base::ThreadType::kRealtimeAudio` (`gn check`: core reaches `//media` through `platform`'s public deps, `platform/media:media_final` → `//media`; Chromium's presubmit `audit_non_blink_usage.py`, which the build does not run, would list the `media::` names used in `core/molt_fingerprinting`); `WebMediaDeviceInfo` (`blink/public/common`) in core | fp-audio |
| `modules/media/audio/audio_device_factory.cc`, `modules/audio_output_devices/html_media_element_audio_output_device.cc`, `modules/mediastream/user_media_processor.cc`, `core/timing/memory_info.cc`, `core/timing/measure_memory/measure_memory_controller.cc` (all five new upstream copies), `media_devices.cc` (new hooks), `core/molt_fingerprinting/moltnet_audio_{output,sink}.{h,cc}`, `moltnet_media_devices.{h,cc}`, `moltnet_memory{,_values}.{h,cc}` | a `CORE_EXPORT` `media::AudioRendererSink` subclass in core with a ref-counted engine posting `CrossThreadBindOnce` tasks to a leaked `NonMainThread`; `blink::BindOnce` named explicitly where a `base::OnceCallback` argument made it ambiguous with `base::BindOnce`; core uses `media::AudioLatency`, `media::FakeAudioWorker::ComputeFakeOutputDelay` and `media::AudioDeviceDescription` (`media/audio`; DEPS rules in `core/molt_fingerprinting/DEPS`, tests included); a garbage-collected `HeapMojoRemote<mojom::blink::PermissionService>` bound through `LocalDOMWindow::GetBrowserInterfaceBroker()` in core; `UserMediaProcessor::GetUserMediaRequestFailed` binding a lambda with `WrapWeakPersistent(this)`; `memory_info.cc`'s `HeapSizeCache::Update` and `measure_memory_controller.cc`'s `ConvertResult` call core glue (same component) | fp-mediaclock |
| `modules/mediarecorder/video_track_recorder.cc`, `media_recorder_handler.cc`, `media/base/video_encoder.{h,cc}` (all four new upstream copies), `core/molt_fingerprinting/moltnet_encoders.{h,cc}` | modules calling a new core glue file; core's DEPS now allow four `media/base` headers (`media::IsEncoderBuiltInVideoType`, `media::VideoType`, `media::SetProcessorCountForSoftwareEncoding`, a new `MEDIA_EXPORT` function); a `constinit std::atomic<int>` in `video_encoder.cc` | fp-probes |
| `core/css/local_font_face_source.cc`, `core/molt_fingerprinting/fonts.{h,cc}`, `system_fonts.{h,cc}` | `local()` through visible families: `GetLocalFontFaceData` calls `FontCache::GetFontData(description, family, AlternateFontName::kNoAlternate)` and reads `SkTypeface::fontStyle()`; `String::substr` with a `wtf_size_t` | fp-probes |
| `platform/fonts/font_platform_data.{h,cc}`, `font_cache.cc`, `font_selector.cc`, `skia/font_cache_skia.cc`, `linux/font_cache_linux.cc`, `win/font_cache_skia_win.cc`, `win/font_platform_data_win.cc`, `mac/font_cache_mac.mm`, `mac/font_platform_data_mac.mm`, `core/css/css_pending_system_font_value.cc`, `modules/font_access/font_access.cc` (the last eight new upstream copies, or newly hooked), `core/molt_fingerprinting/moltnet_fonts.{h,cc}`, `moltnet_font_set.{h,cc}` | MoltNet's bundled font set: a nested abstract `FontPlatformData::UniformFontSet` held in a `std::atomic` pointer, with `enum class AlternateFontName` and `FontFallbackPriority` forward-declared in `font_platform_data.h` (a header much of Blink includes: expect a large rebuild); `SkTypeface_Make_Fontations` with `SkFontArguments` (collection index, `wght`) from core; `base::MemoryMappedFile` from a `base::File` and `SkData::MakeWithoutCopy`; `FontCache::Invalidate()`; since round 28 the compile check compiles the Windows hooks with clang-cl against the Windows SDK and the macOS ones against Darwin's headers and Apple framework stand-ins (the CoreText/AppKit declarations are the stand-ins', so look at the macOS ones first on that build); `font_selector.cc` includes `font_platform_data.h` | fonts |
| `platform/fonts/font_metrics.cc`, `simple_font_data.cc`, `skia/skia_text_metrics.cc` (new upstream copies), `core/molt_fingerprinting/moltnet_fonts.cc`, `chrome/browser/chrome_content_browser_client.cc` (`InitOnUIThread`), `chrome/browser/molt_ai/fingerprinting/molt_net_fonts.cc` | MoltNet's metrics alike on every OS: the hooks sit in each file's `#if BUILDFLAG(IS_APPLE)` / non-Linux branches, which the compile check builds for those platforms since round 28 (Windows with clang-cl; macOS on Apple framework stand-ins; `tools/fonts/metrics_check/run.py` also builds them against stand-ins): watch `SkFont::isSubpixel()` in `font_metrics.cc`'s new `#else`, the early `return` in its macOS block, and `skia_text_metrics.cc`'s `#endif`s; worker font caches invalidated through `base::SingleThreadTaskRunner::GetCurrentDefault()` and `FontGlobalContext::TryGet()`; the browser posts a best-effort UI task (`content::GetUIThreadTaskRunner`) and keeps callbacks for renderers that launch before the set is open | fonts |
| `content/public/browser/content_browser_client.{h,cc}` (two new virtuals returning `std::optional` of `storage::QuotaSettings` and `storage::BlobStorageLimits`), `chrome/browser/chrome_content_browser_client.{h,cc}`, `content/browser/storage_partition_impl.cc`, `content/browser/blob_storage/chrome_blob_storage_context.cc`, `storage/browser/blob/blob_memory_controller.{h,cc}` (new upstream copies), `chrome/browser/molt_ai/fingerprinting/molt_net_storage{,_values}.*` | MoltNet's fixed storage limits; a new `:storage` target in `//chrome/browser/molt_ai/fingerprinting` that `//chrome/browser/molt_ai` depends on (`gn check` on `chrome_content_browser_client.cc`); the blob limits are posted to the IO thread after `InitializeOnIOThread` | fp-probes |
| `media/renderers/default_decoder_factory.cc`, `media/gfx/mappable_shared_image_video_frame_pool.cc`, `media/gfx/paint_canvas_video_renderer.cc`, `media/base/video_frame_converter_internals.cc`, `media/base/sinc_resampler.cc`, `media/base/vector_math.cc`, `media/base/BUILD.gn`, `third_party/opus/BUILD.gn`, `third_party/opus/src/celt/{celt.c,x86/x86cpu.c,arm/pitch_neon_intr.c,arm/celt_neon_intr.c}` (all new upstream copies), `media/base/molt_portable_{media,scale}.{h,cc}`, `core/molt_fingerprinting/moltnet_pixels.{h,cc}` | `//media/base` now depends on `//third_party/opus` and calls the C function `opus_moltbrowser_set_portable()` (defined in `celt.c`; declared `extern "C"` in `molt_portable_media.cc`); `OPUS_X86_PRESUME_SSE`/`SSE2` are gone, so the SSE kernels go through libopus's run-time tables (`x86_celt_map.c`, `x86_silk_map.c`; built and linked here with Chromium's source list); `base::subtle::reinterpret_span` in `PortableScaleFrame`; `molt_portable_media_unittest.cc` (in `media/base:unit_tests`) includes `media/gfx`, `media/renderers` and `media/video/mock_gpu_video_accelerator_factories.h` through `//media:test_support` (`gn check`); core's DEPS allow `media/base/molt_portable_media.h`. | fp-pixels |
| `third_party/llvm-libc/BUILD.gn` (`molt_portable_math`, a static library the `:headers` group now depends on publicly, visible to the file and `//ui/gfx/geometry`), `molt_portable_math.cc` (llvm-libc's shared math with the FMA macros undefined after `cpu_features.h`, an `#error` if they come back), `v8/src/base/ieee754.cc` and `v8/src/base/DEPS` (V8 includes `third_party/llvm-libc/molt_portable_math.h` and links it through `//third_party/llvm-libc:headers`, which `v8_libbase` already uses), `ui/gfx/geometry/molt_math.{h,cc}` and `molt_complex_math.h` (`COMPONENT_EXPORT(GEOMETRY)`), `ui/gfx/geometry/BUILD.gn`, the `gfx::molt_math::` call sites in `ui/gfx`, Blink (core, modules, platform) and `media/base/sinc_resampler.cc` (`media/base/BUILD.gn` gains `//ui/gfx/geometry`), `platform/audio/rustfft_ffi.rs` (a new cxx bridge function, `rustfft_new_portable`: the generated `rustfft_ffi.rs.h` must be regenerated, not taken from an old out dir), `third_party/skia/src/opts/SkOpts_moltbrowser.cpp` (skvx), `sandbox/policy/win/sandbox_win.cc` and `molt_job_memory_limit.h` (64-bit Windows only), `core/workers/worker_backing_thread.cc`, `chrome/browser/chrome_content_browser_client.cc` (`ShouldTryToUseExistingProcessHost`) | the portable math library and its switch; Chromium's presubmit `audit_non_blink_usage.py`, which the build does not run, would list the `gfx::molt_math` names Blink now calls | fp-libm |
| `third_party/skia/include/core/SkScalar.h` (the math macros now call `SkMoltBrowserMath::`, an exported function in the `skia` component; any code that used them in a constant expression or with a non-arithmetic type would fail), `include/private/SkMoltBrowserMath.h` and `src/core/SkMoltBrowserMath.cpp` (`SK_API`, in `skia_core_and_effects`, which now depends on `//third_party/llvm-libc:molt_portable_math`; that library's visibility gains `//skia:*`), `SkGraphics.{h,cpp}`, the Skia call sites (`SkCubics.cpp`, `SkPathOpsCubic.cpp`, `SkBlurEngine.cpp`, `SkGaussFilter.cpp`, `SkGradientBaseShader.cpp`, `SkGainmapShader.cpp`, `SkGainmapInfo.cpp`, `SkXmp.cpp`, `SkExif.cpp`, `SkHdrAgtm.cpp`, `SkHdrAgtmParse.cpp`), `src/ports/SkTypeface_fontations.cpp` and `fontations/src/ffi.rs` (a new cxx bridge function, `set_portable_math`: regenerate `ffi.rs.h`), `rust/exif/FFI.rs` and `exif_parse.rs` (a new shared-struct field, `hdr_headroom_log2`: regenerate `FFI.rs.h`), read-fonts and skrifa (vendored sources, `read_fonts/v0_43/BUILD.gn` with `libm`), `ui/gfx/geometry/BUILD.gn` (`//third_party/fdlibm` for `molt_math::Tanh`), `ui/gfx/color_utils.cc`, `cc/paint/tone_map_util.cc`, the `gfx::molt_math::` call sites in Blink (`core/style/basic_shapes.cc`, `style_shape.cc`, `superellipse.h` and `platform/geometry/contoured_rect.h`, both `constexpr` functions calling `molt_math::Pow` as they called `std::pow`, fine in C++23, `contoured_rect.cc`, `path_builder.cc`, `float_rounded_rect.cc`, `core/css/css_gradient_value.cc`, `platform/graphics/filters/fe_component_transfer.cc`, `fe_color_matrix.cc`, `fe_lighting.cc`, `platform/image-decoders/avif/avif_image_decoder.cc`, `modules/xr/xr_cube_map.cc`) | Skia's, Blink's and Fontations' math on the portable switch; checkdeps (not run by the build) would want `+third_party/fdlibm` in `ui/gfx/geometry`'s DEPS, and Chromium's presubmit `audit_non_blink_usage.py` the new `gfx::molt_math` names | fp-geometry |
| `third_party/blink/renderer/modules/mediastream/user_media_processor.cc` (`GenerateStreamForCurrentRequestInfo`), `core/molt_net/molt_net_renderer.{h,cc}` (+ test), `core/molt_fingerprinting/moltnet_uniformity.{h,cc}` (`ReportsPermissionDenied`, + test), `chrome/browser/media/webrtc/media_capture_devices_dispatcher.cc` (imported from 156; `ProcessMediaAccessRequest`), new `chrome/browser/molt_ai/fingerprinting/molt_net_display_capture{,_policy}.h` (+ `_policy_unittest.cc`) | no screen, window or tab capture in MoltNet. `//chrome/browser/media/webrtc:webrtc` includes the header-only `molt_net_display_capture.h` without a dep, as it includes `molt_net_profile_id.h` (4.10): if `gn check` flags it, add `//chrome/browser/molt_ai/fingerprinting:display_capture` there. `moltnet_uniformity.cc` now includes `core/molt_net/molt_net_renderer.h` (both in core) | fp-geometry |
| `chrome/browser/molt_ai/tor/molt_net_cpu_policy.h`, `molt_net_cpu.{h,cc}` (new; targets `:cpu_policy` and `:cpu`, which `:tor` depends on publicly), the checks in `molt_net_window.cc` (`OpenImpl`, `NewIdentity`), `molt_net_routing.cc` (`GetOrCreateMoltNetProfile`), `tor_manager.cc` (`Launch`), and the sentence up front in `ui/webui/molt_ai/molt_net_handler.cc` (`StatusDict`), `molt_ai_settings_ui.cc` (`TorUnavailableReason`) and `molt_ai_chat_handler_tor.cc` (`HandleGetTorStatus`, whose lambda now captures one more `std::string`) | `base::CPU::GetInstanceNoAllocation()` and `has_sse3()`, `has_ssse3()`, `has_sse41()` exist only under `ARCH_CPU_X86_FAMILY` (the call sits under `ARCH_CPU_X86_64`); a function-local `static const base::NoDestructor<std::string>`; `if (const std::string& e = MoltNetCpuError(); !e.empty())` in each check | fp-arch |
| `media/filters/wsola_internals.cc` (new upstream copy: `MultiChannelDotProduct`, `GetPeriodicHanningWindow`), `media/base/molt_portable_media.h` (a comment), `core/molt_fingerprinting/moltnet_cpu.h` (`--no-enable-f16c` in `kMoltNetV8Flags`) | `//media/filters` calls `media::IsPortableMediaProcess()` and `gfx::molt_math::Cos(float)` (both already used inside `//media`); V8 defines `enable_f16c` on every architecture, so arm64 builds take the flag too (`MoltNetCpuTest.Flags` fails if `SetFlagsFromString` rejects one) | fp-arch |
| WebRTC (new upstream copies under `third_party/webrtc/`): `rtc_base/cpu_info.{h,cc}` (`ScopedCpuIndependent`, `IsCpuIndependent()`), `rtc_base/molt_math.{h,cc}` (new, target `//third_party/webrtc/rtc_base:molt_math`), `api/audio/audio_processing.h` (`Config::moltbrowser_cpu_independent`), `modules/audio_processing/audio_processing_impl.{h,cc}`, `aec3/aec3_common.cc`, `agc2/cpu_features.cc`, `common_audio/third_party/ooura/fft_size_128/ooura_fft.{h,cc}`, `common_audio/resampler/sinc_resampler.cc`, the math call sites (`common_audio/include/audio_util.h`, `fft_size_256/fft4g.cc`, three AEC3, three noise suppression and four AGC2 files) and the `BUILD.gn` files of `rtc_base`, `common_audio`, its Ooura FFTs, `modules/audio_processing`, `aec3`, `ns`, `agc2`, `agc2/rnn_vad`; `media/webrtc/helpers.cc`; Blink's `modules/mediastream/media_stream_audio_processing_layout.cc` | a `constinit thread_local bool` in `cpu_info.cc` (also in component builds, where WebRTC is one component); `OouraFft`'s C functions now build on arm64 too, and `use_neon_` exists only under `WEBRTC_HAS_NEON` (an unused-function or unused-field warning on one architecture means a branch went missing); `webrtc::molt_math` has float and double overloads only, so an integer argument at a call site would be ambiguous (none at 156); `//third_party/llvm-libc:molt_portable_math` is visible to `//third_party/webrtc/rtc_base:molt_math` only; Blink's `modules/mediastream` calls `media::IsPortableMediaProcess()` (`+media/base` is already allowed there; `media::.+` is in the Blink audit's list for `modules/mediastream`) | fp-probes2 |
| jxl-rs: `third_party/rust/chromium_crates_io/vendor/jxl_simd-v0_7/src/{lib.rs,x86_64/mod.rs,aarch64/mod.rs}`, `vendor/jxl-v0_7/Cargo.toml`, `src/lib.rs`, `src/moltbrowser_math.rs` (new) and six call-site files, the crate patches in `patches/jxl_simd-v0_7/` and `patches/jxl-v0_7/`, `Cargo.lock`, `third_party/rust/jxl/v0_7/BUILD.gn` (the new file; `//third_party/rust/libm/v0_2:lib`), `third_party/rust/jxl/v0_7/wrapper/lib.rs` (a new cxx bridge function: the generated `lib.rs.h` gains `jxl_rs_moltbrowser_set_portable()`), `platform/image-decoders/jxl/jxl_image_decoder.cc` (constructor) | rustc on `simd_function!`'s expansion (an early return to the scalar descriptor in every function it makes, x86-64 and arm64); the `PortableMath` trait for `f32` and `f64`; `libm`'s visibility (`//third_party/rust/*`); `scripts/vendor-rust-crates.sh` must keep the two crate patches (afterwards `moltbrowser_math.rs` must still be in the vendored crate) | fp-arch |
| Blink's `platform/mediastream/media_stream_audio_processor_options.cc` (new upstream copy: `IsSystemWideAecAvailable`, `GetPreferredAec`, `GetSystemWideAec`), `modules/mediastream/user_media_processor.cc` (`SelectAudioDeviceSettings`, `InitializeAudioSourceObject`), `media_devices.cc` (`DevicesEnumerated`); `services/audio/reference_output.h`, `audio_processor_handler.{h,cc}`, `output_device_mixer_impl.{h,cc}`, `mixing_graph_impl.cc` (new upstream copies); `media/base/molt_portable_media.{h,cc}`, `vector_math.cc` (`FMAC`), `sinc_resampler.cc` (`Resample`, the kernel); `ui/gfx/geometry/molt_math.{h,cc}` (`PortableSin`, `PortableCos`) | Blink's `platform/mediastream` and `modules/mediastream` call `media::IsPortableMediaProcess()` and `media::PortableCaptureEffects()` (a new `MEDIA_EXPORT` function; `+media/base` is allowed in both); `media::ScopedPortableAudioThread` and `IsPortableAudioThread()` are `MEDIA_EXPORT` over a `constinit thread_local bool` in `molt_portable_media.cc` (media is a component); `ReferenceOutput::Listener::IsCpuIndependent()` is a new virtual with a default, so the services tests' mocks keep compiling (`AudioProcessorHandler` overrides it `final`); the default is defined out of line in `output_device_mixer_impl.cc` (`reference_output.h` has no `.cc`, and the style plugin refuses inline virtual bodies), which makes it `Listener`'s key function: its vtable is emitted there, and every `Listener` (all in the Chromium-wide echo cancellation sources and their tests) links with that file; `OutputDeviceMixerImpl` holds a `std::unique_ptr` to a nested class defined in the `.cc` (its destructor is out of line there) and a `std::atomic<int>`, and its output stream now plays through that class (`PortableMixing`, an `AudioSourceCallback`); `MixingGraphImpl::CreateConverter` opens a scope while the converter is built; `SincResampler::Resample` picks its `ConvolveProc` per call | fp-mic2 |
| Subnormals: `media/base/molt_portable_subnormals.cc` (new, in `//media/base`), `molt_portable_media.h`, `sinc_resampler.cc`, `media/filters/audio_renderer_algorithm.cc`, `services/audio/sync_mixing_graph_input.cc` (new upstream copies), `output_device_mixer_impl.cc`, Blink's `platform/audio/denormal_disabler.h` and `platform/audio/DEPS`, `modules/webaudio/audio_handler.cc` (new upstream copies), `platform/image-decoders/avif/avif_image_decoder.cc`, WebRTC's `rtc_base/cpu_info.{h,cc}` and `modules/audio_processing/audio_processing_impl.{h,cc}`. The format and names: `media/base/audio_processing.h`, `media/webrtc/audio_processor.cc`, `modules/mediastream/media_stream_constraints_util_audio.cc` (new upstream copies), `user_media_processor.cc`, `media_devices.cc`, `user_media_client_test.cc`, `core/molt_fingerprinting/moltnet_media_devices.{h,cc}` and `moltnet_media_values.{h,cc}`, `core/molt_fingerprinting/DEPS`. The platform: `media/audio/pulse/pulse_input.cc`, `pulse.sigs`, `media/audio/BUILD.gn`, `media/audio/win/audio_low_latency_input_win.cc` (new upstream copies), `media/audio/pulse/molt_capture_source.{h,cc}` and `molt_pulse_capture.{h,cc}` (new) | `denormal_disabler.h` now includes `media/base/molt_portable_media.h` (`+media/base/molt_portable_media.h` in `platform/audio/DEPS`), so much of Blink's audio rebuilds; its MSVC branch (`_DN_SAVE` in MoltNet) compiles only with MSVC's own headers. `ScopedPortableSubnormals` reads and writes MXCSR (`_mm_getcsr`) on x86 and FPCR or FPSCR with inline `mrs`/`msr` (`vmrs`/`vmsr`) on Arm, a no-op elsewhere; `ScopedCpuIndependent` does the same inside WebRTC. `AudioProcessingImpl` has six new members guarded by its two mutexes. `pulse.sigs` gained `pa_context_get_source_output_info_list`: the stub generator (`libpulse_stubs`, `link_pulseaudio = false`) must take it, and `molt_pulse_capture.cc` includes `pulse_stubs.h` under `DLOPEN_PULSEAUDIO` as `pulse_util.cc` does; the two new Pulse files are in `media/audio:audio`'s `use_pulseaudio` sources, the test in `media/audio:unit_tests`. `audio_low_latency_input_win.cc` compiles on Windows only. `core/molt_fingerprinting/moltnet_media_devices.cc` includes `media/base/audio_parameters.h` (a file-specific DEPS rule) and the blink mojom `media_devices.mojom-blink.h`; `user_media_client_test.cc` includes the generated `v8_long_range.h` and `v8_double_range.h` | fp-mic3 |
| Cameras: `modules/mediastream/moltnet_camera.{h,cc}` (new, listed in `modules/mediastream/BUILD.gn`, a new upstream copy), `core/molt_fingerprinting/moltnet_camera_values.{h,cc}` (new, in `:farbling`), `moltnet_camera_menu.{h,cc}` (new, in the core component's `build.gni`), and the new upstream copies `video_track_adapter_settings.{h,cc}`, `video_track_adapter.cc`, `media_stream_video_track.cc`, `apply_constraints_processor.cc` and `video_track_adapter_unittest.cc`; `user_media_processor.cc`, `media_devices.cc`, `user_media_client_test.cc`. The channel count: `media_stream_constraints_util_audio.cc`. Outputs: `modules/audio_output_devices/html_media_element_audio_output_device.cc`, `modules/webaudio/audio_context.cc`, `core/molt_fingerprinting/moltnet_media_{devices,values}.{h,cc}`. `IIRFilterNode`: `platform/audio/iir_filter.cc`. Other layouts: `media/base/molt_portable_downmix.cc` (new, in `//media/base`), `molt_portable_media.h`, `media/webrtc/audio_processor.cc` | `VideoTrackAdapterSettings` has two new `bool` members, compared in `operator==` (every adapter of every track rebuilds). `VideoTrackAdapter`'s `AdaptFrameResolution` returns `molt_fingerprinting::AdaptUniformCameraFrame()`'s `base::expected` and hands it the resolution adapter's own `frame_pool_` and `frame_converter_` (members of a class private to the `.cc`). `moltnet_camera.cc` uses `media::VideoFrameConverter`, `media::VideoFramePool`, `media::ConvertToMemoryMappedFrame` and `media::ComputeLetterboxRegion` (modules' `+media/base` rule), `WebMediaStreamTrack::kResizeModeNone`/`kResizeModeRescale` as `String`, and reaches `:farbling`'s camera functions only through `moltnet_camera_menu.h` (`CORE_EXPORT`; `:farbling` exports nothing from the core component), which passes them `std::span` over `WTF::Vector` storage. The `VideoCaptureSettings` it rebuilds keep the selection's (virtual) capture format, so the constructor's DCHECKs hold; the source gets the device mode in `CreateVideoSource` only. `iir_filter.cc` includes `media/base/molt_portable_media.h` (Blink platform's audio already does, `platform/audio/DEPS`). `audio_processor.cc`: `AudioProcessorCaptureFifo` takes a `std::unique_ptr<PortableCaptureDownmix>`; `molt_portable_media.h` now includes `channel_layout.h` and forward-declares `ChannelMixer` | fp-media4 |

### 4.8 WebUI handlers and the agents

| File | Risk | From |
|---|---|---|
| `ui/webui/molt_ai/molt_ai_chat_handler.cc` | type-checked against 156's headers (Linux and Windows) since round 28; the download section, `ChooseChatWindow` with `BrowserWindowInterface`, `SidePanelUI::Show`, `HasTransientUserActivation` | #36, #40, #54 |
| `molt_ai_chat_handler_actions.cc`, `molt_ai_chat_form_fill.*`, `molt_ai_memory_ui.cc`, `molt_ai_settings_ui.cc` (`BoolArg`, `BoundedStringArg`), `molt_ai_automation_ui.cc` (`HasCallbackArgs`) | | #28, #32, #36, #40, #54 |
| `agents/web_agent.cc`, `agent_actions.cc` (new `agent_actions` → `constrained_decoding` dependency and DEPS rule), `agent_screenshot.cc` (`CopyFromSurface` 4-arg form, observer overrides), `agent_vision.*`, `agents:agent_plan` (`-Wswitch` for `kReplan`) | | #35, #39, #45, #46, #47, #50 |
| `automation/automation_ai_steps.cc`, `automation_runner.cc`, `agent_run_draft.*`, the new automation GN targets | | #35, #54 |
| `chrome/browser/about_flags.cc`, `flag-metadata.json`, `flag-never-expire-list.json` | `#molt-agent-screenshots`, `#molt-agent-site-tools`, `#molt-agent-planner` | #39, #45, #47 |
| `models/model_download_job.cc` (`CancelAndDiscard`), `models:download_plan` | | #40 |
| `tor/molt_net_window.cc`, `tor/tor_control_connection.cc` (`poll()` on POSIX) | | #49 |
| `update/update_manager.cc`, `vault/vault_store.cc` (`base::saturated_cast`) | | #41 |
| every fuzzer target's `BUILD.gn`, `seed_corpus` and `dict` paths, DEPS and visibility | build with the real `//base`, `//url`, `//crypto`, BoringSSL, llama.cpp's grammar API | #41 and later |
| `agents/frame_tool_permissions.*` (a `ProfileKeyedServiceFactory`, `RegisterListPref`), `agents/site_tool_host.cc` (`RenderFrameHost::IsFeatureEnabled(network::mojom::PermissionsPolicyFeature::kTools)`), `automation/automation_runner.cc` (fallback locators, the run's questions, CALL_TOOL steps), the new GN targets (`agents:frame_tools` → `automation:agent_run_draft`; `automation:locators`, `:tool_steps`, `:run_questions`; `automation` → `molt_ai_agents`), and the one-line hooks in `chrome_browser_main_extra_parts_profiles.cc`, `chrome_browsing_data_remover_delegate.cc` and `profile_resetter.cc` | type-checked against 156's headers only | agent-replay |
| `ui/webui/molt_ai/molt_net_handler.cc`, `molt_net_ui.cc`, `fingerprinting/molt_net_fonts.{h,cc}` | the status gains `fonts` (`GetMoltNetFontsStatus()`, `std::optional<MoltNetFontsStatus>`; the handler calls `StartLoadingMoltNetFonts()`); the panel's note is a `<div role="note">` filled by `textContent`; `//chrome/browser/ui/webui/molt_ai` now depends on `//chrome/browser/molt_ai/fingerprinting:net_fonts` | moltnet-shared4 |

### 4.9 GN targets and DEPS edges

Since round 28 (gn-ready) the GN graph, the DEPS rules and Blink's audit
are checked without a checkout (First build, step 1).
`tools/chromium/gn_static_check.py` evaluates 156's `BUILD.gn` files with
the overlay for the three desktop configs, also with `molt_llama_vulkan` and
as a component build, loads what `gn gen` loads, and applies `gn check`'s
include rule and `gn gen`'s dependency rules (unknown labels, visibility,
`testonly`, `allow_circular_includes_from`, cycles) to every file and target
MoltBrowser adds or changes; `checkdeps` and `blink-audit` run Chromium's own
scripts from the pin. All three are clean, and every MoltBrowser header is
listed in the target that owns it, so the notes of earlier rounds of the
form "in no target's `sources`" or "if `gn check` complains, list X in Y /
add the dep" (in §4.2, §4.7 and §4.10 as well) are done. The real
`gn gen` and `gn check` below stay the authority; what the static check
approximates is in its header (values known only at `gn gen` time, other
toolchains, a third-party library's own `include_dirs`).

- [ ] List what the new code defines (from the PRs below):
  `(cd chromium/src && gn ls out/MoltBrowser '//chrome/browser/molt_ai/*')`.
- [ ] `gn check` the MoltBrowser directories first (fast), then the whole
  tree (authoritative) (from #13, #15, #19, #20, #24, #27, #28, #30, #37,
  #41, #45, #52):
  ```bash
  cd chromium/src
  for p in '//chrome/browser/molt_ai/*' '//chrome/services/molt_ai/*' \
           '//chrome/common/molt_ai/*' '//chrome/renderer/molt_ai/*' \
           '//chrome/browser/ui/webui/molt_ai/*' '//third_party/llama_cpp/*' \
           '//third_party/blink/renderer/core/molt_fingerprinting/*' \
           '//components/omnibox/browser/*' '//content/utility/on_device_model/*' \
           '//chrome/browser/client_hints/*' '//chrome/utility/*'; do
    gn check out/MoltBrowser "$p" || echo "GN CHECK FAILED: $p"
  done
  gn check out/MoltBrowser 2>&1 | tee ~/molt-v0.3.0/gn-check.log
  ```
  *Expected:* no error. If there is one, the static check missed it: send
  `gn-check.log`, and fix it as the message says (a dep, a `public` entry,
  a visibility line). Do it in both out dirs (macOS and Linux evaluate
  different files).
- [ ] DEPS include rules (checkdeps) on the real tree, for the new `DEPS`
  files under `chrome/{browser,renderer,services}/molt_ai/` and Blink's
  `core/molt_fingerprinting` and the hooked upstream directories (from #10,
  #19, #37, #41) (verify the command):
  ```bash
  cd chromium/src
  for d in chrome/browser/molt_ai chrome/services/molt_ai chrome/common/molt_ai \
           chrome/renderer/molt_ai third_party/blink/renderer/core/molt_fingerprinting chrome/utility; do
    python3 buildtools/checkdeps/checkdeps.py "$d" || echo "CHECKDEPS FAILED: $d"
  done
  python3 buildtools/checkdeps/checkdeps.py 2>&1 | tee ~/molt-v0.3.0/checkdeps.log   # the whole tree
  ```
  *Expected:* no violation. The static run used 156's own `checkdeps.py`
  over every C++ file MoltBrowser adds and every include it adds to an
  upstream file; Skia and V8 are repositories of their own, with their own
  rules, and were left out. WebRTC's files (fp-probes2) were checked as
  WebRTC's presubmit checks them, rooted at `third_party/webrtc` with
  WebRTC's DEPS files; on the real tree:
  `for d in rtc_base modules/audio_processing common_audio; do (cd third_party/webrtc && python3 ../../buildtools/checkdeps/checkdeps.py --root . "$d") || echo "CHECKDEPS FAILED: webrtc/$d"; done`.
- [ ] Blink's presubmit audit (`audit_non_blink_usage.py`, which the build
  doesn't run): `gn_static_check.py blink-audit` runs its `check()` over the
  lines MoltBrowser adds under `third_party/blink`. MoltBrowser's entries
  are at the end of `_CONFIG` in
  `src/third_party/blink/tools/blinkpy/presubmit/audit_non_blink_usage.py`.

How the graph fits together since round 28, where a reviewer may want it
otherwise:

- WebRTC's audio processing reaches the portable math library through one
  new target, `//third_party/webrtc/rtc_base:molt_math` (which depends on
  `:cpu_info` and `//third_party/llvm-libc:molt_portable_math`); the
  `common_audio`, Ooura FFT, AEC3, noise suppression, AGC2 and RNN VAD
  targets depend on it, and `modules/audio_processing:audio_processing` on
  `rtc_base:cpu_info` (fp-probes2). `//media/webrtc:molt_unittests` (the
  golden digests) is a test-only target that only `molt_ai_unittests`
  sees; since fp-mic2 it also holds the echo reference's goldens
  (`molt_echo_reference_unittest.cc`, which uses media's `SincResampler`
  and `vector_math` and `//base` directly).
- `//chrome/browser:core` depends on `//chrome/browser/molt_ai:core_hooks`,
  a group of the MoltBrowser targets whose headers the hooks in
  `chrome/browser/` include (the runtime, the agents' features,
  fingerprinting, the tor policies, the shield, the web AI APIs, the WebUI
  navigation throttle; the letterbox view on desktop, the updater on
  macOS).
- `molt_net_profile_id.h` and `molt_net_incognito_parity.h` are public
  headers of `//chrome/browser/profiles:profile`, which has
  `//chrome/browser/molt_ai/tor:incognito_parity_policy` as a public dep: any
  target that may include `profile.h` may include them, so the split-out
  targets of §4.10 need no dep of their own.
- `//chrome/browser/ui:ui` depends on `molt_ai/automation`, and lists it
  and `molt_ai/tor` in `allow_circular_includes_from` (`ui:ui` is on
  `//.gn`'s allowlist): their code includes `chrome/browser/ui` headers
  while `ui` includes theirs.
- The std-only policies MoltBrowser keeps inside components are small
  targets there, which std-only MoltBrowser targets and tests depend on
  instead of the whole component: `//net:molt_policies`,
  `//content/browser:molt_policies`,
  `//components/download/internal/background_service:molt_site_isolation`
  and `//components/download/internal/common:molt_fetch_credentials`. The
  header-only hook files are listed in the component that includes them
  (`//net`, `//services/network:network_service`, `//storage/browser`,
  `//components/services/storage`, `//content/browser`,
  `//services/cert_verifier/cert_net_url_loader`).
- The side panel's views code is its own target,
  `//chrome/browser/molt_ai:molt_ai_side_panel_views`.
- For these hooks 26 upstream `BUILD.gn` files are now in `src/`, each
  change in a `# MoltBrowser:` block: `chrome/browser/` `browsing_data`,
  `download`, `extensions`, `media/router`, `media/webrtc`, `metrics`,
  `net`, `payments`, `profile_resetter`, `ssl`, `translate`, `webauthn`;
  `chrome/browser/ui/` `autofill`, `extensions`, `tabs`, `views/frame`,
  `views/side_panel`, `views/translate`;
  `components/download/internal/{background_service,common}`,
  `components/services/storage`, `content/browser`, `net`,
  `services/cert_verifier/cert_net_url_loader`, `services/network`,
  `storage/browser`. Also two `DEPS` files (`chrome/browser/browsing_data`,
  `ui/gfx`) and Blink's audit script.
- A component build (`is_component_build = true`: developer builds only,
  never a release): every MoltBrowser function or class used from another
  library has that library's export macro (a scripted review in round 28
  added `MEDIA_EXPORT` to the portable scaler, `CORE_EXPORT` to two Blink
  functions Blink's tests call, and moved `MoltTorCapacity`'s per-process
  pointer out of line with `NET_EXPORT_PRIVATE`, so each library sees the
  same one). An `undefined symbol` naming a MoltBrowser function at link
  time means one was missed: add the export macro of the library that
  defines it.

Still worth knowing from earlier rounds:

- degoogle-kit: `//chrome/browser/molt_ai/privacy:feature_overrides` and its
  `:unit_tests` depend on `//chrome/browser/dictation:features`,
  `//chrome/common:chrome_features`, `//components/history_embeddings/core`,
  `//components/optimization_guide/core:prediction`,
  `//components/optimization_guide/proto:optimization_guide_proto`,
  `//components/optimization_guide/core:features`,
  `//components/safe_browsing/core/common:features` and `//media`, for the
  features they turn off. If the standalone `molt_ai_unittests` link grows
  too much, move those entries into a second overrides source set that only
  `//chrome/browser` links. `//chrome/browser/molt_ai/privacy:component_policy`
  (`//base` only) is a dep of `//chrome/browser/component_updater:impl`: a
  cycle there would mean it picked up a dep it must not have.
  `molt_ai_unittests` gains
  `//chrome/browser/molt_ai/privacy:on_device_manifest_unit_tests` where
  `use_on_device_model_service` is set, which links
  `//components/optimization_guide/core:manifest_broker`.
- The `:autofill` target of `//chrome/browser/molt_ai/tor` depends on
  `//components/autofill/core/browser` and `//third_party/libaddressinput`;
  the tor `DEPS` file allows `molt_net_autofill.cc` its two libaddressinput
  headers.
- Windows only: `molt_net_parental_controls.cc` includes
  `chrome/browser/win/parental_controls.h` (`//chrome/browser/win`, a
  Windows dep of `//chrome/browser/molt_ai/tor`).
- `public/web/web_molt_fingerprinting.h` forward-declares `base::File`;
  `//chrome/common/molt_ai/fingerprinting`'s mojom has
  `//mojo/public/mojom/base` as a public dep (`ReadOnlyFile`).

- moltnet-shared8: `//chrome/browser/molt_ai/tor:nonce_storage` (new source_set: the tab helper; deps `//storage/browser`, `//chrome/browser/profiles:profile`, `//net`) and `:nonce_storage_policy` (header-only), the first a dep of `//chrome/browser/ui:tab_helpers_impl`; `content/browser/download/molt_resumed_fetch_policy.h` in `//content/browser:molt_policies` and `molt_resumed_fetch_factory.h` in `:browser`; the tor `DEPS` lets `molt_net_download_isolation_unittest.cc` include the policy.

- moltnet-shared9: `//chrome/browser/molt_ai/tor:nonce_storage` gains deps `//media:media_buildflags` and `//services/network/public/mojom`; `components/services/storage/dom_storage/molt_dom_storage_waits.h` in `//components/services/storage:storage`; `content/browser/browsing_data/molt_one_key_deletion_policy.h` in `//content/browser:molt_policies`; the tor `DEPS` lets `molt_net_one_key_deletion_unittest.cc` and `molt_net_dom_storage_waits_unittest.cc` include their std-only headers; `molt_net_one_key_deletion_unittest.cc` and `molt_net_dom_storage_waits_unittest.cc` in `:unit_tests`.

- moltnet-shared10: `net/cookies/molt_nonce_cookie_policy.h` in `//net:molt_policies` (a public dep of `//net`); `services/network/molt_nonce_drop_policy.h` in `//services/network:network_service`; `molt_net_nonce_cookie_policy_unittest.cc` and `molt_net_nonce_drop_policy_unittest.cc` in the tor `:unit_tests` (which depends on `//net` and `//services/network:network_service`), and the tor `DEPS` lets `molt_net_nonce_drop_policy_unittest.cc` include its std-only header; `:nonce_storage` keeps its deps (`//services/network/public/mojom` now for `NetworkContext`).

- moltnet-shared11: `net/base/molt_site_share_policy.h` in `//net:molt_policies`; `net/base/molt_network_key_site.h` and `molt_site_lru_cache.h` in `//net`'s MoltBrowser `sources`; `//chrome/browser/net:impl` depends on `//chrome/browser/molt_ai/tor:nonce_storage` (for `MoltNetRefusedNonces()`), whose `public_deps` gain `//base`; `molt_net_site_shares_unittest.cc` and `molt_net_site_shares_caches_unittest.cc` in the tor `:unit_tests` (its existing deps: `//net`, `//services/network:network_service`, `//base/test:test_support` for `base::TestMemoryConsumerRegistry`; BoringSSL through `//net`'s public deps), and the tor `DEPS` lets the second include `services/network/cors/preflight_cache.h` and `preflight_result.h`.
- fp-media4: `//content/browser:molt_audio_capture_policy` (new
  source_set, header-only: `renderer_host/media/molt_audio_capture_processing.h`,
  public dep `//media/mojo/mojom`), a dep of `:browser` and of
  `//chrome/browser/molt_ai/fingerprinting:unit_tests` (which also gains
  `//media` and `//media/mojo/mojom`; the fingerprinting `DEPS` lets
  `molt_net_audio_capture_unittest.cc` include that header,
  `media/base/audio_processing.h` and the mojom);
  `//chrome/browser/molt_ai/fingerprinting:audio_capture` (new, header-only)
  in `//chrome/browser/molt_ai:core_hooks`. `content/browser/renderer_host/
  media/render_frame_audio_input_stream_factory.cc` (a new upstream copy)
  includes `content_browser_client.h` and `content_client.h`.
  `modules/mediastream/BUILD.gn` is a new upstream copy listing
  `moltnet_camera.{h,cc}` in `blink_modules_sources("mediastream")`;
  `core/molt_fingerprinting/build.gni` lists `moltnet_camera_menu.{h,cc}`
  and its `BUILD.gn` `moltnet_camera_values.{h,cc}` in `:farbling` (its
  test in `:unit_tests`); `media/base/BUILD.gn` lists
  `molt_portable_downmix.cc`.

### 4.10 MoltNet's Incognito protections (`molt_net_incognito_parity.h`)

Each hook keeps Chromium's check and adds `molt_ai::tor::IsMoltNetProfile()`
(header-only `molt_net_profile_id.h`) or calls `GetsIncognitoProtections()`
(header-only `molt_net_incognito_parity.h`). Several sit in split-out GN
targets that don't depend on `//chrome/browser/molt_ai/tor`
(`//chrome/browser/picture_in_picture`, `//chrome/browser/gcm`,
`//chrome/browser/media/webrtc:webrtc`,
`//chrome/browser/reduce_accept_language:impl`): if `gn check` (4.9)
flags the include, add the dep there.

| File | Hook | From |
|---|---|---|
| `chrome/browser/devtools/devtools_window.cc` | `GetProfileForDevToolsWindow` keeps the MoltNet profile | #23 |
| `chrome/browser/extensions/chrome_extension_cookies.cc` | in-memory extension cookie store | #23 |
| `chrome/browser/webshare/mac/sharing_service_operation.mm` | file shares refused after the random delay; type-checks on Apple framework stand-ins (round 28) | #23, rebase 156 (now on `IsPrimaryOTRProfileWithRegularParent()`) |
| `chrome/browser/gcm/gcm_profile_service_factory.cc`, `.../instance_id/instance_id_profile_service_factory.cc` | no GCM for MoltNet | #23 |
| `chrome/browser/media/chrome_media_session_client.cc` | `ShouldHideMetadata` | #23, rebase 156 (as above) |
| `chrome/browser/net/net_error_diagnostics_dialog_win.cc` | no Network Diagnostics | #23, rebase 156 (as above) |
| `chrome/browser/app_controller_mac.mm` | `OpenStartupTabsInBrowserWithProfile` skips MoltNet; type-checks on Apple framework stand-ins (round 30; AuthenticationServices among them), not the SDK: look at it first on the Mac build | #23 |
| `chrome/browser/picture_in_picture/auto_picture_in_picture_tab_helper.cc` | "ask" counts as "block" | #23, rebase 156 (as above) |
| `chrome/browser/media/webrtc/webrtc_logging_controller.cc` | `IsWebRtcTextLogAllowed` false for MoltNet | rebase 156 |
| `chrome/browser/reduce_accept_language/reduce_accept_language_factory.cc` | `ReduceAcceptLanguageService` gets `is_incognito` for MoltNet | rebase 156 |
| `chrome/browser/molt_ai/tor/molt_net_pref_isolation_policy.h` (MoltBrowser's own) | three more pinned prefs; `molt_net_pref_isolation_unittest.cc` now needs `//components/autofill/core/common` and `//components/universal_optout` | rebase 156 |
| `chrome/browser/prefs/incognito_mode_prefs.cc` (imported from 156) | `CanOpenBrowser()`: a MoltNet profile may open windows unless Incognito is off (forced Incognito no longer refuses it) | moltnet-policy |
| `chrome/browser/policy/chrome_policy_blocklist_service_factory.cc` (imported from 156) | the Incognito URL lists for `GetsIncognitoProtections()` profiles instead of `IsIncognitoProfile()` | moltnet-policy |
| `chrome/browser/enterprise/data_controls/chrome_rules_service.cc` (imported from 156) | `incognito_profile()`, `ExtractPasteActionContext()` and `ExtractPasteActionContextSource()`: a Data Controls rule's `incognito` condition covers `GetsIncognitoProtections()` profiles | moltnet-shared2 |
| `chrome/browser/enterprise/data_controls/chrome_clipboard_context.cc` (imported from 156) | both `GetClipboardSource()` overloads: text copied in a MoltNet window has the source `INCOGNITO`, with no URL | moltnet-shared2 |
| `chrome/browser/win/parental_controls.{h,cc}` (imported from 156) | `MoltReadWinParentalControlsNow()`: `WinParentalControlsValue::ReadNow()` (`GetParentalControlsFromApi()` + `UpdateParentalControlsFromRegistry()`, no histograms, not cached), asked on a COM STA runner (`base::ThreadPool::CreateCOMSTATaskRunner()`) each time a MoltNet window opens. Windows-only; type-checks with clang-cl against the Windows SDK (round 28) | moltnet-shared2 |

## 5. Unit tests

`molt_ai_unittests` builds in the release out dir (REBASING.md). The
upstream suites are big and link slowly with `is_official_build = true`;
build them in a second, non-official out dir with DCHECKs on, which also
serves as the "debug build" #21 and #23 asked for (DCHECKs around Incognito
extension enablement and `MediaRouterEnabled()`).

- [ ] The test out dir (from #21, #23) (verify the command):
  ```bash
  mkdir -p build/cache
  sed 's/^is_official_build = true/is_official_build = false/' configs/macos-arm64.gn > build/cache/macos-arm64-tests.gn
  echo 'dcheck_always_on = true' >> build/cache/macos-arm64-tests.gn
  ./scripts/configure.sh --config "$PWD/build/cache/macos-arm64-tests.gn" --build-dir out/MoltTests
  ```
  (`build/` is gitignored, so the release provenance's clean-tree check
  doesn't see the file.) On Linux, the same from `configs/linux-x64.gn`,
  with `--build-dir out/MoltTests-linux` so it doesn't replace the Mac's
  test out dir in a shared checkout; inside the Docker image the repository
  is `/moltbrowser`, so pass
  `--config /moltbrowser/build/cache/linux-x64-tests.gn`.

### 5.1 MoltBrowser's own tests

- [ ] `molt_ai_unittests` (from every PR; the table says which):
  ```bash
  autoninja -C chromium/src/out/MoltBrowser molt_ai_unittests
  chromium/src/out/MoltBrowser/molt_ai_unittests 2>&1 | tee ~/molt-v0.3.0/molt_ai_unittests-mac.log
  ```
  *Expected:* every test passes, on macOS and on Linux (there in
  `out/MoltBrowser-linux`; Linux adds `MoltGpuInferenceSandboxPolicyTest.*`
  and `HelperSandboxLinuxTest.*`).
  The suites the PRs said were never compiled, to look at first if
  something fails:

  | Suites | From |
  |---|---|
  | `DocumentParserClientTest`, `DocumentParserImplTest`, `PdfTextScraperTest` (Mojo runner, real pipes) | #13 |
  | `InferenceProcessHostTest` (crash mid-stream, hang, flood, bad UTF-8, the projector handle, GPU fallback, `kBackendFailed` off the GPU) | #15, #38, #52, #53 |
  | `HelperSandboxTest`, `HelperSandboxLinuxTest`, `SeatbeltProfileTest` | #16 |
  | `HelperSandboxWinPolicyTest` (every platform: the capability SID from its name, the SDDL, the attribute list, the mitigations), `HelperScratchDirTest`, `RecordedWavTest`, `OcrImageTest`, `OcrImageFileTest` (a fake data decoder service over real Mojo pipes, since the binary doesn't start Blink: the decode's limits, a hung or crashed decoder; the PNM written from a decoded bitmap), and `OcrRenderedPagesTest`, `RenderAndOcrPdfTest` (now asynchronous); on Windows (deferred) `HelperSandboxWinTest`, `HelperSandboxWinOutputTest` (§7.5) | helpers-win |
  | `OcrRenderedPagesTest`, `RenderAndOcrPdfTest`, `PdfPageImageSizeTest` | #18 |
  | `MoltNetContentSettingsTest` (real `HostContentSettingsMap`), `MoltNetPrefServiceTest`, `MoltNetIsolationTest` | #21 |
  | `MoltNetWebRtcBindersTest` (real mojo), `MoltNetRefusingP2PSocketManagerTest` | #22 |
  | `MoltNetIncognitoParityPolicyTest`, `MoltNetRequestBindersTest`, `MoltNetRequestsPolicyTest` | #23 |
  | `MoltNetLocaleStringsTest` (real en-US and French paks), `MoltNetLocalePrefsTest`, `MoltNetLocalePinTest`, `MoltShieldsDecisionTest` | #24 |
  | `MoltAIManagerTest`, `WebAIPolicyTest`, `WebAIConversationTest`, `WebAISitePermissions*Test`, `WebAITranslationTest`, `MoltFeatureOverridesTest` | #26, #32, #34, #38, #46, #50 |
  | `MoltLetterboxTest` | #27, #33 |
  | `EmbeddingProcessHostTest`, `MemoryBackendTest`, `MoltMemoryProviderTest`, `MoltMemoryControllerGateTest`, `MoltLocalInputTest` | #28, #44, #47 |
  | `JsonSchemaGrammarTest`, `BoundedJsonTest`, `GrammarTokensTest` | #30 |
  | `AgentActionsTest`, `AutomationAiStepsTest`, `ChatActionsTest` (with the real Rust JSON reader: a FILL_FORM reply 3 levels deep under depth limit 4 must parse) | #35, #36 |
  | `ImageInputTest`, `ModelImagePrepTest` (real Skia, data decoder), `ModelImageTest` | #38 |
  | `AgentVision*Test` | #39, #46, #50 |
  | `ModelDownloadPlanTest`, `DownloadPolicyTest`, `ChatFormFillTest`, `ChatWindowTest` | #40 |
  | `AgentSafetyTest.SanitizeFoldsLookalikesSplitByControlCharacters`, `AutomationScriptTest`, `ReleaseManifestTest` | #41 |
  | `AgentToolsTest` | #45 |
  | `AgentPlanTest`, `AgentPromptTest` | #47, #55 |
  | `PageTranslatePlanTest`, `PageTranslateIoTest`, `PageTranslateMessagesTest`, `PageTranslationJobTest` | #48, #55 |
  | `TorControlProtocolTest`, `TorrcBuilderTest`, `PluggableTransportsTest`, `OnionLocationTest` | #6, #49 |
  | `MoltNetNetworkContextPolicyTest`, `MoltNetPartitioningPolicyTest` | #51, #55, #62, network quality |
  | `MoltNetValuesTest.TheDisplayIsAPlainSrgbScreen`, `MoltNetValuesTest.ScrollbarStyleIsThePlatformDefault` | fingerprinting 156 |
  | `MoltNetMediaValuesTest.*` | fp-media |
  | `MoltNetAudioClockTest.*` (the output ring, drift control and a simulation against devices of 128 to 8192 frames) | fp-audio |
  | `MoltNetMemoryValuesTest.*` (V8's heap limit for 1 to 64 GB, the fixed table of sizes), and the device-list and `getUserMedia()` cases of `MoltNetMediaValuesTest.*` | fp-mediaclock |
  | `MoltNetMemoryValuesTest.UniformLimitIsAFourGigabyteMachines`, `MoltDataLimitTest.*` (Linux: a MoltNet renderer's data limit is 8 GiB whatever Chromium's tier) | fp-cpu |
  | `MoltPortableMathTest.*` (golden hashes of 32 functions: the same on every CPU and OS), `MoltMathTest.*` (off: the C library bit for bit; on: the portable functions; the complex ones), `MoltNetBrowserDeviceValuesTest.*` (the renderer process limit), `MoltJobMemoryLimitTest.*` (Windows only) | fp-libm |
  | `SkMoltBrowserMathTest.*` (off: Skia's functions and `SkScalar` macros are the C library's bit for bit; on: the portable ones; golden hashes of arcs, SVG arcs, rotations, contains over fills and strokes, dashes, contour lengths and path operations, the same on every CPU and OS: a mismatch on one platform means Skia's math there isn't portable, or Chromium's Skia defines changed what these compute), and `MoltMathTest.*`'s new `Tanh` cases (fdlibm) | fp-geometry |
  | `MoltNetDisplayCapturePolicyTest.*` (every screen, window and tab type refused, cameras and microphones not; the answer is `PERMISSION_DENIED_BY_USER`); in `blink_unittests`: `MoltNetRendererTest.*` (the same types; refused only once the process hosts MoltNet), `MoltNetUniformityTest.DisplayCaptureDeniedWhereMoltNetIs` | fp-geometry |
  | `MoltNetStorageTest.*`, `MoltNetStorageValuesTest.*` | fp-probes |
  | `MoltNetFontsTest.*` (the browser opens the whole set or nothing, its table equals Blink's, an absent set told from a damaged one), `MoltNetFontsDeliveryTest.*` (renderers launched while the set is opening get it later, a damaged set is never sent, the set is opened after startup; `content::BrowserTaskEnvironment`), `MoltNetFontSetTest.*` (generic families, CJK regions, face matching, fallback order) | fonts |
| `media_unittests --gtest_filter='MoltPortable*'` (not in `molt_ai_unittests`): `MoltPortableScaleTest.*` (the integer scaler, golden digests shared with arm64), `MoltPortableMediaTest.*` (frames of every format scaled, the converter, no hardware decoders, frames kept in CPU memory, no GPU YUV conversion; libopus's switch) | fp-pixels |
  | `MoltNetProxyPolicyTest` (new), and the new cases of `MoltNetNetworkContextPolicyTest` (client certificates, DoH probes, `UniformNetworkStateForMoltNetOnly`) and `MoltNetRequestsPolicyTest` (`client_certificates`) | MoltNet 156 |
  | `MoltNetLivePolicyTest` (new: the settings check, MoltNet's proxy-setting parser, the proxy-configuration rule), and `MoltNetRequestsPolicyTest`'s `autofill_server` and `address_metadata` | moltnet-live |
  | `MoltNetLanguagesTest` (real `LanguagePrefs`, a managed `ForcedLanguages`), `MoltNetIsolationTest.FixedLanguageNamesMatchChromiumsConstants`, `MoltNetPrefServiceTest.MoltNetWritesStayInMoltNet` (changed), `MoltNetSocketPoolsTest` (real `HttpNetworkSession` and pools, `MOCK_TIME`); built standalone only: `MoltNetLanguagePolicyTest`, `MoltNetSocketPoolPolicyTest`, `MoltNetLivePolicyTest`'s Incognito and ended-identity cases | moltnet-policy |
  | `MoltNetSocketPoolsTest` (now 7: a site's pools for its first- and third-party keys and its sandboxed pages, fixed limits with no random room, the handshake slots on a real task environment), `MoltNetNetworkContextPolicyTest` (48 connections, keepalive limits), `MoltNetStorageTest.QuotaSettings`; built standalone only: `MoltNetSocketPoolPolicyTest` (13, rewritten), `MoltNetKeepalivePartitionsTest`, `MoltNetStorageBudgetPolicyTest`, `MoltNetStorageValuesTest.EachSiteATenthOfTheTotal`, `TorrcBuilderTest.StatesTorsSharedCapacity`, `MoltNetDownloadIsolationTest` (new), `MoltNetNetworkContextPolicyTest.DownloadsKeptWithTheirSiteInMoltNetOnly` | moltnet-shared |
  | `MoltNetSocketPoolsTest` (now 11: `OnionHandshakesWeighMore`, `SessionsShareTorsBudgets`, `GreedySitesDontHoldBackAPoliteOne`, `ASitesOneOffKeysHaveAPoolOfTheirOwn`), `MoltNetCertFetchSiteTest` (new: the thread's site, a fetch's key in its site's pools, the site across the verifier service's mojo interface), `MoltNetBackgroundFetchRequestTest` (new), `MoltNetStorageTest.QuotaSettings`; built standalone only: `MoltNetSocketPoolPolicyTest` (17: the weighted slots, a site's one-off pool, requests that ignore the limits), `MoltNetStorageBudgetPolicyTest` (rewritten: the floor and the machine), `MoltNetStorageValuesTest.{EachSiteATenthOfThePool,MemoryReserve,BlobShare}`, `MoltNetBlobBudgetPolicyTest` (new), `MoltNetProcessPolicyTest` (new), `MoltNetIdentityEndPolicyTest` (new), `MoltNetDownloadIsolationTest` (now 7: retries), `MoltNetLivePolicyTest.{ExtensionsRequiredInIncognitoRefuse,ParentalControlsReadAgain}`, `TorrcBuilderTest.StatesTorsSharedCapacity` (256) | moltnet-shared2 |
  | `MoltNetSocketPoolsTest.CertificateFetchesHaveAPoolOfTheirOwn` and the budget cases (both parts), `MoltNetHttpCacheTest` (new: Chromium's real `MemBackendImpl` with the budget), `MoltNetEndedIdentityNetworkTest` (new: Chromium's real `ProxyConfigServiceMojo`), `MoltNetDomStorageTest` (new: the real `LocalStorageImpl` in memory, mock time), `MoltNetStorageTest` (the callbacks null in the fingerprinting settings); built standalone only: `MoltNetSocketPoolPolicyTest` (the certificate-fetch pools and budget split), `MoltNetHttpCacheBudgetPolicyTest` (new), `MoltNetIdentityEndPolicyTest` and `MoltNetNetworkContextPolicyTest` (new cases), `MoltNetStorageBudgetPolicyTest` and `MoltNetBlobBudgetPolicyTest` (rewritten: share only, the floor, site keys), `MoltNetMemoryValvePolicyTest` (new), `MoltNetDomStorageSitePolicyTest` (new) | moltnet-shared3 |
  | `MoltNetEndedIdentityNetworkTest` (new cases: the refusal's answer, a SOCKS5 socket's writes and handshake after the end), `MoltNetEndedContextTest` (new: Chromium's real `NetworkService` and `NetworkContext`), `MoltNetBackgroundFetchCredentialsTest` and `MoltNetBackgroundFetchCookiesTest` (new: `download::CreateResourceRequest()`, a regular profile's network context and an `EmbeddedTestServer`), `MoltNetBackgroundFetchBufferTest` (new: `BlobStorageContext` with shares), `MoltNetDomStorageTest` (new cases) and `MoltNetSessionStorageTest` (new: the real `SessionStorageImpl` in memory), `MoltNetFontsDeliveryTest.ThePanelKnowsWhatOpeningFound`; built standalone only: `MoltNetEndOrderingTest` (new: 3000 random pipe interleavings, about 12 s), `MoltNetIdentityEndPolicyTest`, `MoltNetBlobBudgetPolicyTest.HeldMemoryCountsLikeABlob`, `MoltNetDomStorageSitePolicyTest` (new cases). In `components_unittests`: `InMemoryDownloadTest.MoltNet*` | moltnet-shared4 |
  | `MoltNetStorageGroupsTest` (new: real storage keys, a credentialless frame's key counts with its nonce, opaque top-level sites apart, the quota share), `MoltNetDomStorageTest.CredentiallessFramesCountWithTheirNonce`, `MoltNetHttpCacheTest.KeysOfEntriesHeldOpenCountAgainstTheSite` and `ANewEntryMakesRoomFromItsSitesOwnEntries` (new); built standalone only: `MoltNetHttpCacheBudgetPolicyTest.ANewEntrysKeyCountsLikeData`, `MoltNetDomStorageSitePolicyTest.GroupsOfKeysThatDontNameTheirSite` and `ForgettingAClearedAreaLast` | redteam2 (round 27) |
  | `MoltNetDomStorageTest` (new cases: no database, areas holding items stay, `GetUsage()`, a greedy site's writes refused while another's go, write bytes, credentialless frames' allowance, a quota-refused write keeps the old value in a regular profile), `MoltStorageAreaPutTest` (new: `StorageAreaImpl` on its own), `MoltNetStorageSiteUsageTest` (new: Chromium's real `QuotaManagerImpl`, off the record, with a `MockQuotaClient`: a site's keys share its share, cached changes counted with no client call, a polite site's check asks for none of a greedy site's new buckets, credentialless frames count with their nonce, the floor), `MoltNetNetworkContextPolicyTest.BackgroundFetchPerSiteInMoltNetOnly`; built standalone only: `MoltNetDomStorageSitePolicyTest` (new cases: no database, the write allowance), `MoltNetStorageSiteUsageTest` in `molt_net_storage_site_usage_policy_unittest.cc` (the counting templates), `MoltNetBackgroundFetchSchedulingTest` (new) | moltnet-shared5 |
  | `MoltNetHttpCacheTest` (new cases: `TheSweepPassesEntriesInUse`, `SparseDataCountsAgainstItsSite`, `EntriesMadeBeforeTheLimitsAreInTheirSitesLists`), `MoltNetHttpCacheSiteLruTest` (new: the per-site lists with a stand-in entry), `MoltNetDomStorageTest` (new cases: `TinyItemsCountWhatTheyTake`, `AFloodingSiteDoesntDelayAnothersRead`, `WithoutCachesPerSiteAFloodDelaysEveryRead`), `MoltNetSessionStorageTest.AFloodingSiteDoesntDelayAnothersRead`, `MoltNetOwnSessionSocketsTest` (new), `MoltNetEndedContextTest` (new cases: `NoProxyResolvingSocketFactoryForMoltNet`, `NoRawSocketsForMoltNet` (the other context binds local TCP and UDP sockets on 127.0.0.1), `NoConnectedSocketForMoltNet`, `NoMdnsResponderForMoltNet` with `ENABLE_MDNS`, `MoltNetsP2PSocketManagerReachesNothing` with `IS_P2P_ENABLED`); built standalone only: `MoltNetDomStorageSitePolicyTest.ItemsAndAreasCountWhatTheyTake`, `MoltNetDomStorageFairQueueTest` (new, 8) | moltnet-shared6 |
  | `MoltNetBackgroundFetchCredentialsTest` (new cases: `AResumedFetchKeepsItsFirstRequests`, `OtherDownloadsResumeAsChromium`), `MoltNetBackgroundFetchCookiesTest.AResumedFetchStaysCrossSite` (new: a SameSite=Strict cookie of the test server is sent with a request resumed as Chromium 156 resumes it and not with one that keeps its first request's initiator and isolation), `MoltNetEndedContextTest.NoNameLookupForMoltNet` (new: every source refused for MoltNet, `localhost` resolved by another context, the host resolver's pipe); built standalone only: the scheduler's `MoltScheduleDownload()` verbatim against stand-ins (5 tests: the same activations as before, no pending registration's site worked out on a request's start) | redteam3 (round 29) |
  | `MoltNetDomStorageTest` (new cases: `WithoutADatabaseAnUnusedEmptyAreaGoesAtOnce`, `SessionStorageWithoutADatabaseDropsEmptyAreas`, `BindingAreasInALoopDoesntDelayAnotherSite`, `PerSitePartitionsRunOnThreadsOfTheirOwn` (real dedicated threads from the test's thread pool), `ObserversHearOfChangesInTasksOfTheirOwn`, `AClearOfManyItemsIsFreedInChunks`; `ASitePurgesItsOwn` and `IdleAreasExpire` now on a partition on disk in a temporary directory), `MoltNetHttpCacheTest` (new cases: `ExpiryFollowsAMonotonicClock`, `TheSweepDropsEntriesInStepsOfItsOwn`, `ASitePastTheCapIsNotCached`, which writes 4,097 sites; the old cases now move both the wall clock and the mock tick clock), `MoltNetHttpCacheSiteLruTest` (new: `TheSweepGoesSiteBySiteInSteps`, `ASweepStepThatDropsNothingMovesOn`, `ASweepStepStopsAtTheFirstFreshEntry`, `TheSweepsSiteMayGoBetweenSteps`, `TheTableOfSitesHasRoomForTheCap`); built standalone only: `MoltNetDomStorageSitePolicyTest.UnusedEmptyAreasGoAtOnceWithoutADatabase`, `TurnsThatArentASitesHaveTheirOwnNames`, `TheControlTurnCountsAnEighth`, `MoltNetDomStorageSplitWorkTest` (new, 2), `MoltNetDomStorageFairQueueTest.TheControlTurnHasEightShares` | moltnet-shared7 |
  | `MoltNetBackgroundFetchSchedulingTest` (rewritten, 8 cases: no total, a site's share whatever others run, its waiting registrations in order, the valve past the floor), `MoltNetNonceStoragePolicyTest` (new, 3), `MoltNetDomStorageSitePolicyTest` (new: the expiring records, totals dropped at zero), `MoltNetKeepalivePartitionsTest` (new: idle recorders go), `MoltNetBlobBudgetPolicyTest.SiteKeys` (rewritten), `MoltNetStorageSiteUsagePolicyTest` (new: one refresh at a time), `MoltNetDownloadIsolationTest.AResumedFetchKeepsItsLnaCheck`, `MoltNetBackgroundFetchRestoredTest.AFetchWithoutItsParametersWaits` (all built standalone here); needing the build: `MoltNetDomStorageTest.ANoncesStorageGoesWithItsPage`, `ACopyOfManyAreasIsCompleteWhenBound` (real dedicated threads: a copy of 200 areas made in steps, bound after it), `MoltNetStorageSiteUsageTest.ConcurrentChecksDontOvershoot`, `MoltNetBackgroundFetchRequestTest.BlobKeyIsTheSitesDigest` | moltnet-shared8 |
  | `MoltNetNonceStoragePolicyTest` (new cases: `TheSiteAPageCountsTo`, `SandboxedPagesCountToTheirOwnSite`; built standalone here); needing the build: `MoltNetDomStorageTest.ACopyOwedToAPopulatedNamespaceIsDropped` (real dedicated threads: the copy a deletion owes, in its own task, to a namespace populated meanwhile is dropped and the partition answers) | redteam5 (round 31) |
  | `MoltNetNonceStoragePolicyTest.AGonePagesCookiesGoAgainLater`, `MoltNetOneKeyDeletionTest` (new), `MoltNetDomStorageWaitsTest` (new), `MoltNetBackgroundFetchRestoredTest.ATakeUpWhileAskingIsRemembered` (new; built standalone here); needing the build: `MoltNetDomStorageTest.AUsageOfOneKeyListsThatKeyOnly`, `WithoutCachesPerSiteAUsageOfOneKeyIsFiltered`, `CopiesOfTwoTabsRunSideBySide` (real dedicated threads: two tabs' copies made a chunk a task side by side, each complete when bound, a deletion of a tab after its copy), `MoltNetSessionStorageTest.AUsageOfOneKeyListsItsNamespaces` | moltnet-shared9 |
  | `MoltNetNonceCookiePolicyTest` (new, 7: the limits, a partition's and a nonce's totals, a refused nonce, no room left by the other partitions, records gone when empty), `MoltNetNonceDropPolicyTest` (new, 3), `MoltNetNonceStoragePolicyTest` (new cases: `AGroupsKeysAreBounded`, `AGroupClosedPastItsOwnBound`, `ANonceSharedBySuccessivePages`), `MoltNetOneKeyDeletionTest` (new cases: `ATimeRangeReadsItsOwnKeysBuckets`, `AResponseWithNoDocumentNamesItsOwnKey`; `OtherDeletionsReadTheWholePartition` changed), `MoltNetNetworkContextPolicyTest` (`molt_nonce_limits` for MoltNet only); the policy suites built standalone here, `MoltNetNetworkContextPolicyTest` (real mojom parameters) needs the build | moltnet-shared10 |
  | `MoltNetOneKeyDeletionTest` (new case: `AnotherSitesRequestNamesItsPartition`; `AResponseWithNoDocumentNamesItsOwnKey` changed; built standalone here). In `media_unittests` on Linux (§5.2, fp-mic3's row) `MoltPulseCaptureSourceTest` has new cases `NeverGuessesAnotherDevice` and `RecordsVirtualMicrophonesAsAsked` and a changed `LoopsEnd` | redteam7 |
  | `MoltNetSiteSharesTest` (new, 13: the limits and decisions, a site evicting its own least recently used, a greedy site never evicting another's, the record, records gone with the last entry, the dictionaries' check, and `MoltSiteLRU` over `base::LRUCache` and `base::LRUCacheSet`: the base's behaviour without shares, each site's share with them, no eviction of its own, the record through `Erase()`, `Clear()` and `Swap()`), `MoltNetNonceCookiePolicyTest` (new cases: a partition without a nonce's totals and purge, cookies held before the limits), `MoltNetNonceStoragePolicyTest` (new: `RefusalsForAContextMadeAgain`, `RefusalsFollowTheSteps`), `MoltNetNetworkContextPolicyTest` (`molt_per_site_network_caches` and `molt_no_quic` for MoltNet only, no `molt_refused_nonces` without the browser's record); all but the last built standalone here (`MoltSiteLRU` against Chromium 156's `lru_cache.h`, the memory coordinator stood in); needing the build: `MoltNetSiteSharesTlsTest.*`, `MoltNetSiteSharesAuthTest.*`, `MoltNetSiteSharesServerPropertiesTest.*`, `MoltNetSiteSharesPreflightTest.*` (Chromium's real `SSLClientSessionCache`, also under `base::TestMemoryConsumerRegistry`'s memory pressure, `HttpAuthCache`, `HttpServerProperties` with a nonce's broken marks, and `PreflightCache`, each with and without shares; the preflight case without shares expects Chromium's random purge to reach one of five entries, which fails by chance with a probability of about 1e-12) | moltnet-shared11 |
  | `MoltNetSiteSharesTest` (new cases: `AnEntryToKeepTakesTheLargestSitesPlace`, `TheContainerKeepsAnEntryItMustKeep`; `TheDecisions` changed; built standalone here, 15 pass); needing the build: `MoltNetSiteSharesAuthTest.AGreedySiteEvictsOnlyItsOwnEntries` (changed: past the total, later sites' sign-ins are kept and the polite site keeps its own), `MoltNetSiteSharesServerPropertiesTest.AnHttp11DemandIsAlwaysKept` (new) | redteam8 |
  | `MoltNetCpuPolicyTest.*` (the requirement on x86-64, arm64 and other architectures, the sentence), `MoltNetCpuErrorTest.*` (what `base::CPU` reports; on a build machine with SSE4.1, and on every arm64 one, the sentence is empty, so a failure there means MoltNet would refuse to open) | fp-arch |
  | `MoltCpuIndependentProcessingTest.*` (WebRTC's audio processing in CPU-independent mode through `CreateWebRtcAudioProcessingModule()`: the golden digests of a talker with a far end's echo and of the talker alone, the same on every processor and OS; the setting fixed when the module is built; a CPU-independent module and one with the processor's code in turn on one thread each give what they give alone). A golden digest that differs on the build machine means the module computes otherwise there: send the digest the failure prints, the processor (`lscpu`, or the Mac's chip) and the OS | fp-probes2 |
  | `MoltCpuIndependentEchoReferenceTest.*` (the echo reference as the audio service mixes it while a MoltNet stream listens: a 44.1 kHz stream resampled by media's `SincResampler` and a 48 kHz one added with `vector_math::FMAC` in a `ScopedPortableAudioThread`, and WebRTC's processing in CPU-independent mode with that mix as the far end; golden digests the same on every processor and OS), `MoltMathTest.PortableEntryPointsIgnoreTheSwitch`. A golden digest that differs: send what the failure prints, the processor and the OS, as above | fp-mic2 |
  | `MoltPortableSubnormalsTest.*` (media's `SincResampler` and WebRTC's processing in CPU-independent mode over signals that reach every range the flush modes treat differently: golden digests the same on every processor and OS, also from a thread left in a flush mode), `MoltCpuIndependentProcessingTest.ConversionGoldenDigests` and `OneOutputFormatForEveryDevice` (a MoltNet stream's one format, converted from 44.1 kHz mono, 96 kHz stereo, 16 kHz mono and 48 kHz stereo devices with every effect off: golden digests), and the processing's earlier golden digests (`MoltCpuIndependentProcessingTest.GoldenDigests`, `MoltCpuIndependentEchoReferenceTest.*`), which changed with the processing's noise floor. A golden digest that differs: send what the failure prints, the processor and the OS | fp-mic3 |
  | `MoltNetCameraValuesTest.*` (the cameras' menu, the entries a camera shows and the mode it opens in), `MoltNetAudioCaptureTest.*` (the browser's decision on the CPU-independent processing replaces the renderer's), `MoltCpuIndependentProcessingTest.DownmixesOtherLayouts` (a QUAD and a 5.1 microphone: opened in their layout, processed as their down-mix) and the setSinkId case of `MoltNetMediaValuesTest.*` | fp-media4 |
  | `GpuOffloadTest`, `GpuFallbackGuardTest`, `StreamValidatorTest`, `MoltGpuInferenceSandboxPolicyTest` (Linux) | #52, #53, #55 |
  | `AudioInputTest`, `ModelAudioTest`, `MoltGpuDriverWarmupTest` (not on Android or iOS), `VulkanShaderPackTest` (only with `molt_llama_vulkan`), and the audio cases of `MoltAIManagerTest`, `WebAIPolicyTest`, `WebAIConversationTest`, `InferenceProcessHostTest`, `PageTranslationJobTest` | ai-finish |
  | `AgentRunDraftTest`, `AutomationSecurityTest` | #54 |
  | `AutomationLocatorsTest`, `RunQuestionsTest`, `AutomationToolStepsTest`, `FrameToolsTest` (new), and the new cases of `AgentToolsTest` (frames of other sites, `consequentialHint`), `ChatActionsTest`, `AgentRunDraftTest` and `AutomationScriptTest` | agent-replay |
  | `MoltOSCryptTest`, `MoltKeysStoreTest`, `VaultStoreTest` (the os_crypt_async move) | #11 |
  | `PromptUtilsTest`, `GemmaThoughtFilterTest`, `ThinkTagFilterTest` | #7, #8 |
  | `SanitizeHelperTextTest`, `Utf8Test` (OCR and voice output) | #14 |

  `MoltFeatureOverridesTest` also has `DisablesGoogleOnlyComponents`,
  `HistoryEmbeddingsCountsAsOverriddenOff` and
  `CommandLineCanTurnDictationBackOn` (degoogle-kit): the components only
  Google features use are off, history embeddings counts as overridden,
  and `--enable-features` still wins. After the degoogle-kit follow-up
  also `NoUnaskedOnDeviceModelDownloads` and
  `NoSecureDnsFallbackToGooglePublicDns`, `MoltComponentPolicyTest` (the
  four Google-only components are skipped unless
  `MoltGoogleOnlyComponents` is on) and, on desktop, `MoltOnDeviceManifestTest`
  (the on-device model manifest is not asked for at startup, and is asked
  for once by each kind of demand: a session, an eligibility check, an
  availability observer, binding the broker, requesting a use case).

- [ ] The DOM storage timing check (moltnet-shared7): MoltNet's
  `localStorage` against a regular window's, with the real storage
  classes, on this machine:
  ```bash
  chromium/src/out/MoltBrowser/molt_ai_unittests --gtest_also_run_disabled_tests \
    --gtest_filter='MoltNetDomStorageTimingTest.*' 2>&1 | tee ~/molt-v0.3.0/dom-storage-timing.log
  ```
  It takes about 20 seconds: twice, a polite page's reads every 10 ms,
  quiet and while two pages of the other partition flood their areas,
  with MoltNet's partition first on the storage sequence (as before) and
  then on a thread of its own. *Expected:* it passes, and the log's line
  for "MoltNet on its own thread" shows the median under either flood
  within about twice the quiet one (the standalone harness measured 17 µs
  against 21 µs quiet), while the line for "MoltNet on the storage
  sequence" shows MoltNet's reads waiting milliseconds behind the regular
  pages' flood. Close other heavy work first; if it fails, rerun once and
  report both logs.

- [ ] Blink's MoltBrowser tests (from #10, #19, #20, #22, #24, #27, #33, #37):
  ```bash
  autoninja -C chromium/src/out/MoltTests blink_unittests
  chromium/src/out/MoltTests/blink_unittests --gtest_filter='Molt*' 2>&1 | tee ~/molt-v0.3.0/blink-molt.log
  ```
  *Expected:* all pass. This covers `MoltLetterboxTest` (the zoom,
  scrollbar and viewport cases), `MoltNetTextTest` (with the real Roboto
  test font and shaper), `MoltNetRendererTest`, `MoltNetDeviceTest`,
  `MoltNetLocaleTest`, `MoltNetUniformityTest`, `MoltNetNetworkTest`,
  `MoltNetDisplayTest` (fingerprinting 156), `MoltNetMediaTest` (fp-media),
  `MoltNetAudioDeviceTest` (fp-audio: real threads and a few hundred
  milliseconds of real time each; on a loaded bot rerun a timing failure
  once before you report it), `MoltNetAudioSinkTest` (fp-mediaclock: the
  same, up to 1.5 s of real time), `MoltNetMediaDevicesTest` and
  `MoltNetMemoryTest` (fp-mediaclock),
  `MoltNetEncodersTest` and the pure `MoltLocalFaceTest` (fp-probes),
  `MoltNetPixelsTest` (fp-pixels),
  `MoltNetCpuTest` (fp-cpu: real isolates configured for 1 to 64 GB; the
  heap limit MoltNet reports is V8's; fp-libm: `--portable-math` among the
  flags) and `MoltNetMemoryTest.MeasurementEntries`,
  `MoltNetMemoryTest.SystemMemoryPressure` and the
  `MoltNetDeviceValuesTest.DeviceMemoryIsTheHeapsMachine` case (fp-libm),
  `MoltNetFontsTest` and `MoltNetFontSetTest` (fonts: with the Roboto and
  COLRv1 test fonts standing in for the set, nothing comes from the
  system before or after the files arrive, refused files leave the set
  empty, generic families and `queryLocalFonts()` from the set, a worker
  thread that looked a font up before the files came looks again after;
  the worker test runs a real `NonMainThread`, rerun a timing failure once),
  `MoltFingerprintingTest`,
  `MoltGpuUniformityTest`, `MoltSharedWorkerKeyTest`,
  `MoltShieldsDecisionTest`, `MoltSystemFontsTest` and the pure `MoltFarbling`,
  `MoltFont*`, `MoltWebGLUniform` and `MoltNet*Values` suites.

### 5.2 Upstream suites the hooks touch

Default (non-MoltNet) behaviour is meant to be unchanged, so these should
pass except where the table below says otherwise (verify the filters).

| Binary | `--gtest_filter` | From |
|---|---|---|
| `services_unittests` | `NetworkContextTest.*:NetworkContextTestWithMockTime.*:CertNetFetcherURLLoaderTest.*:CertVerifierServiceFactoryTest.*:CertVerifierServiceFactoryBuiltinVerifierTest.*:SharedResourceChecker*:ResourceScheduler*` | #51, #55, #62, network quality |
| `net_unittests` | `SOCKSConnectJobTest.*:*SOCKS5*:ConnectJobTest.*:HttpNetworkSession*:HostResolverManager*:TransportSecurityState*:URLRequestTest*HSTS*:*Hsts*` | #49, #55, #2, #62 |
| `components_unittests` | `AutocompleteControllerTest.*` | #44 |
| `components_unittests` | `TranslateManagerTest.*:TranslateLanguageListTest.*` | #48 (expected failures) |
| `unit_tests` | `TranslateServiceTest.*:TranslateBubbleViewTest.*` | #48 (expected failures) |
| `unit_tests` | `ChromeBrowsingDataRemoverDelegateTest.*:ProfileResetterTest.*` | #32 |
| `unit_tests` | `AboutFlagsTest.*:AboutFlagsHistogramTest.*` | #39, #45, #47 |
| `blink_unittests` | `*Scrollbar*:*ScrollableArea*:*VisualViewport*:*TextMetrics*:*MediaValues*:NetInfo*:*NetworkInformation*:LazyLoad*:FrameFetchContext*` | #20, #27, #33, #37, network quality |
| `blink_unittests` | `*MediaQuery*:*WebViewTest*:*OverlayScrollbar*` | fingerprinting 156 |
| `blink_unittests` | `AudioContext*:BaseAudioContext*:*ScriptProcessor*:*RealtimeAudioDestination*:*EncryptedMedia*:*MediaKey*:MediaCapabilities*:*VideoDecoder*:*VideoEncoder*:*DecoderTemplate*` (normal windows: the hooks only act in MoltNet) | fp-media |
| `blink_platform_unittests` | `FontPlatformDataTest.*:FontCache*:NetworkStateNotifierTest.*` | #37, #50, network quality |
| `blink_platform_unittests` | `AudioDestination*:PushPullFIFO*` (the device factory is unset outside MoltNet) | fp-audio |
| `blink_unittests` | `AudioContext*:AudioWorklet*:*MediaDevices*:*MediaDeviceInfo*` | fp-audio |
| `blink_unittests` | `*AudioRendererMixer*:*AudioRendererSinkCache*:*AudioDeviceFactory*:*HTMLMediaElementAudioOutputDevice*:*MediaDevices*:*UserMediaProcessor*:*UserMediaClient*:MemoryInfo*:*MeasureMemory*` (normal windows: the hooks only act in MoltNet) | fp-mediaclock |
| `blink_platform_unittests` | `WebMediaPlayerImpl*:*AudioRendererSink*` (unchanged code that a MoltNet sink now sits under) | fp-mediaclock |
| `blink_unittests` | `MediaRecorder*:VideoTrackRecorder*:AudioTrackRecorder*:*LocalFontFace*:FontFace*:*FontFaceSet*` (normal windows: unchanged) | fp-probes |
| `blink_platform_unittests` | `FontCache*:FontPlatformData*:FontFallback*:FontSelector*:*FontDescription*:*WebFontTypeface*:SimpleFontData*:*FontMetrics*:*SkiaTextMetrics*:*HarfBuzz*` (outside MoltNet nothing is set: unchanged) | fonts |
| `blink_unittests` | `*FontAccess*:*SystemFont*:*FontFaceSet*:*LocalFontFace*:CSSFontSelector*` (normal windows: unchanged) | fonts |
| `media_unittests` | `*VideoEncoder*:VpxVideoEncoder*:Av1VideoEncoder*` | fp-probes |
| `media_unittests` | `*PaintCanvasVideoRenderer*:*MappableSharedImageVideoFramePool*:*VideoFrameConverter*:*DefaultDecoderFactory*:*DecoderSelector*:SincResampler*:MultiChannelResampler*:*VectorMath*:*ChannelMixer*:*AudioOpusEncoder*:*AudioEncoder*:*AudioConverter*` (outside MoltNet: unchanged; the Opus encoder's output is the same as before on every CPU) | fp-pixels |
| `blink_unittests` | `*VideoFrame*:*WebMediaPlayer*:*AudioEncoder*:*VideoEncoder*:*AudioTrackRecorder*:*ImageBitmap*` (normal windows: unchanged) | fp-pixels |
| `v8_unittests` | `*Ieee754*:*FunctionBodyDecoder*:*WasmSimd*:*FlagList*` (defaults unchanged: relaxed SIMD on, `std::tanh`; with `--no-wasm-relaxed-simd` a relaxed opcode fails validation) | fp-cpu |
| `gin_unittests`, `blink_unittests` | `IsolateHolder*`, `*MeasureMemory*:MemoryInfo*:*ImageDecoder*:*AudioDecoder*` (outside MoltNet: unchanged) | fp-cpu |
| `media_unittests` | `*Symphonia*:*FFmpegAudioDecoder*:*AudioFileReader*:*AudioDecoder*:MoltPortable*` (the Rust bridge's `set_portable_fft`; outside MoltNet the decoders' output is unchanged) | fp-cpu |
| `skia_unittests`, `gfx_unittests` | `*ColorSpace*:*Skcms*:*ConvertPixels*:*RasterPipeline*` (the portable code is only installed in MoltNet) | fp-cpu |
| `sandbox_linux_unittests` | all (Linux) | fp-cpu |
| `blink_unittests`, `blink_platform_unittests` | `*AudioParam*:*Biquad*:*IIRFilter*:*Oscillator*:*PeriodicWave*:*Panner*:*HRTF*:*Analyser*:*DynamicsCompressor*:*FFTFrame*:*VectorMath*:*Delay*:*Convolver*:*WaveShaper*:DOMMatrix*:*CSSMath*:*CalculationExpression*:*AffineTransform*:*SVGTransform*:*SVGPath*:*PathTraversal*:*Path*:*CanvasPath*:*Interpolation*:*PointerEvent*:*RTCRtp*:*PeerConnectionUtil*:*XR*` (outside MoltNet every call is the C library's, bit for bit) | fp-libm |
| `gfx_unittests`, `media_unittests`, `skia_unittests` | `*Transform*:*Quaternion*:*Matrix44*:*Vector2d*:*Vector3d*:*ColorConversion*:*SinCos*`; `SincResampler*`; `*BlitRow*:*Blit*` | fp-libm |
| `v8_unittests`, V8's `mjsunit` | `*Ieee754*`; `mjsunit/math-*` and `mjsunit/es6/math-*` once more with `--portable-math` (the results of a correctly rounded or almost correctly rounded library; the tests' tolerances hold) | fp-libm |
| `skia_unittests`, `blink_unittests`, `blink_platform_unittests`, `gfx_unittests`, `cc_unittests` | `*Path*:*Geometry*:*Stroke*:*PathOps*:*Cubic*:*Blur*:*Gradient*:*Gainmap*:*Hdr*:*Exif*:*Typeface*:*Fontations*`; `*BasicShape*:*StyleShape*:*ContouredRect*:*PathBuilder*:*FloatRoundedRect*:*Superellipse*:*CornerShape*:*ShapeOutside*:*Shape*:*ClipPath*:*CSSGradient*:*FEComponentTransfer*:*FEColorMatrix*:*FELighting*:*AVIF*:*XR*:*Contrast*:*CapabilityElement*`; `*ColorUtils*`; `*ToneMap*` (outside MoltNet every call is the C library's, bit for bit) | fp-geometry |
| `storage_unittests` | `BlobMemoryController*:BlobStorageContext*:Quota*` | fp-probes |
| `content_unittests` | `StoragePartitionImpl*:*BlobStorage*:*Quota*` | fp-probes |
| `content_unittests` | `ServiceWorker*:SharedWorker*` (the service and shared worker fakes) | #24 |
| `unit_tests` | `WebRtcLoggingControllerTest.*:ReduceAcceptLanguageServiceTest.*` | rebase 156 |
| `net_unittests` | `SSLClientSocketTest.*:*ClientAuth*:*ClientCert*:HttpNetworkTransactionTest.*` | MoltNet 156 |
| `services_unittests` | `NetworkContextTest.*Doh*:NetworkContextTest.*ClientCert*` (inside the `NetworkContextTest.*` row too) | MoltNet 156 |
| `content_unittests` | `NetworkQualityObserverImplTest.*` | MoltNet 156 |
| `unit_tests` | `*SSLClientCertificateSelector*:*ClientCertificate*` | MoltNet 156 |
| `unit_tests` | `ProxyConfigMonitor*:PrefProxyConfigTracker*:ProfileNetworkContextService*:ChromeAutofillClient*:*PaymentRequest*` | moltnet-live |
| `components_unittests` | `PrefProxyConfigTracker*:AutofillCrowdsourcingManager*:AddressNormalizer*:RegionDataLoader*` | moltnet-live |
| `net_unittests` | `ClientSocketPoolManager*:TransportClientSocketPool*:HttpNetworkSession*:HttpStreamFactory*:HttpNetworkTransactionTest.*:WebSocket*` (no MoltNet params: one pool per proxy, as before) | moltnet-policy |
| `unit_tests` | `IncognitoModePrefs*:*PolicyBlocklist*:ProxyConfigMonitor*:ChromeContentBrowserClient*:*ReduceAcceptLanguage*` | moltnet-policy |
| `components_unittests` | `LanguagePrefs*:IncognitoModePolicyHandler*:*URLBlocklist*` | moltnet-policy |
| `net_unittests` | `ClientSocketPool*:TransportClientSocketPool*:SocketPoolAdditionalCapacity*:SOCKSConnectJobTest.*:ConnectJobTest.*:HttpNetworkSession*:WebSocketTransportClientSocketPool*` (no MoltNet params: Chromium's pool with its random room, no gate and no handshake slots) | moltnet-shared |
| `services_unittests` | `URLLoaderFactoryTest.*:KeepaliveStatisticsRecorder*:URLLoaderTest.*Keepalive*:NetworkContextTest.*:NetworkServiceTest.*` (every context without the field counts in the service's recorder, as before) | moltnet-shared |
| `storage_unittests` | `QuotaManagerImplTest.*:QuotaTemporaryStorageEvictorTest.*:UsageTracker*:*QuotaSettings*` (also inside the fp-probes `Quota*` row; `molt_per_site_quota` is 0 outside MoltNet) | moltnet-shared |
| `components_unittests` | `InMemoryDownloadTest.*:DownloadServiceControllerImplTest.*:BackgroundFetchDelegateBase*` (requests without an isolation unchanged) | moltnet-shared |
| `content_unittests` | `DownloadManagerTest.*:DownloadManagerImplTest.*:SavePackage*:SaveFileManager*:BackgroundFetch*` (no profile keeps downloads with their site there) | moltnet-shared |
| `browser_tests` | `BackgroundFetchBrowserTest.*:DownloadTest.*Incognito*:SavePageBrowserTest.*` (Background Fetch in Incognito now sends the site's isolation; every other expectation unchanged) | moltnet-shared |
| `net_unittests` | `SchemefulSite*:CertVerifier*:MultiThreadedCertVerifier*:CoalescingCertVerifier*:CachingCertVerifier*:SSLClientSocketTest.*:TransportClientSocketPool*:ClientSocketPoolManager*:HttpNetworkTransactionTest.*` (no MoltNet params: requests name no site, `LOAD_IGNORE_LIMITS` and backup jobs as before) | moltnet-shared2 |
| `services_unittests` | `CertNetFetcherURLLoaderTest.*:CertVerifierService*:*CertVerifierMojomTraits*:NetworkContextTest.*` | moltnet-shared2 |
| `storage_unittests` | `BlobMemoryControllerTest.*:BlobStorageContextTest.*:BlobRegistryImplTest.*:BlobBuilderFromStreamTest.*:BlobSlice*:QuotaManagerImplTest.*` (`molt_per_key_memory` and `molt_memory_reserve` are 0 outside MoltNet: one limit and Chromium's queue) | moltnet-shared2 |
| `content_unittests` | `RenderProcessHostUnitTest.*:SiteInstanceTest.*:ChildProcessSecurityPolicyTest.*:BlobRegistryWrapper*:DownloadManagerImplTest.*` (no embedder keeps BrowsingInstances apart there) | moltnet-shared2 |
| `unit_tests` | `ChromeRulesServiceTest.*:*DataControls*:*ClipboardContext*:DownloadBubbleUIControllerTest.*:DownloadsDOMHandlerTest.*:ChromeContentBrowserClient*` (Incognito, regular profiles and their retries as before) | moltnet-shared2 |
| `components_unittests` | `InMemoryDownloadTest.*` (requests without an isolation unchanged) | moltnet-shared2 |
| `browser_tests` | `DownloadTest.*Retry*:ProcessManagement*:*ProcessReuse*:DataControls*` (regular profiles unchanged) | moltnet-shared2 |
| `net_unittests` | `DiskCacheBackendTest.*Mem*:DiskCacheEntryTest.*Mem*:MemBackend*:HttpCache*:ProxyConfigServiceMojo*:TransportClientSocketPool*:ClientSocketPoolManager*` (no budget and no refusal callback outside MoltNet: Chromium's LRU, memory pressure and pools) | moltnet-shared3 |
| `services_unittests` | `NetworkContextTest.*:CertNetFetcherURLLoaderTest.*:KeepAliveURLLoader*` (the new fields off) | moltnet-shared3 |
| `storage_unittests` | `QuotaManagerImplTest.*:BlobMemoryControllerTest.*:BlobStorageContextTest.*:LocalStorageImplTest.*:SessionStorageImplTest.*:SessionStorageNamespaceImplTest.*` (the callbacks null and no caches per site: Chromium's behaviour) | moltnet-shared3 |
| `content_unittests` | `KeepAliveURLLoader*:BackgroundFetchDelegateProxyTest.*:DownloadManagerImplTest.*:SaveFileManager*:DOMStorageContextWrapperTest.*:*StoragePartition*` (no embedder ends an identity or keeps caches per site there) | moltnet-shared3 |
| `components_unittests` | `InMemoryDownloadTest.*:BlobTaskProxy*` (blobs without a site keep the browser's key) | moltnet-shared3 |
| `net_unittests` | `SOCKS5ClientSocketTest.*:SOCKSConnectJobTest.*:HttpNetworkSession*:ConnectJobTest.*` (no identity-end signal outside MoltNet's sessions) | moltnet-shared4 |
| `services_unittests` | `NetworkContextTest.*:URLLoaderFactoryTest.*:URLLoaderTest.*:ProxyConfigServiceMojo*:WebSocket*:WebTransport*` (the new field off: no context ends) | moltnet-shared4 |
| `storage_unittests` | `BlobMemoryControllerTest.*:BlobStorageContextTest.*:LocalStorageImplTest.*:SessionStorageImplTest.*:SessionStorageDataMapTest.*:StorageAreaImplTest.*` (no shares, no caches per site: nothing held, no totals, Chromium's commit rate) | moltnet-shared4 |
| `components_unittests` | `InMemoryDownloadTest.*:BlobTaskProxy*:*DownloadUtils*:DownloadItemImplTest.*` (without shares nothing is held; a download whose parameters keep `kInclude`, the default, is made as before) | moltnet-shared4 |
| `content_unittests` | `BackgroundFetch*:DOMStorageContextWrapperTest.*:DOMStorage*` (no embedder callback outside MoltNet) | moltnet-shared4 |
| `storage_unittests` | `StorageAreaImplTest.*:LocalStorageImplTest.*:SessionStorageImplTest.*:SessionStorageNamespaceImplTest.*:SessionStorageDataMapTest.*` (no caches per site: a database as before, every write taken; a write refused for an area's quota now keeps the old value in every profile) | moltnet-shared5 |
| `storage_unittests` | `UsageTrackerTest.*:ClientUsageTracker*:QuotaManagerImplTest.*` (without MoltNet's budget nothing is counted per site and the quota answer is Chromium's) | moltnet-shared5 |
| `content_unittests` | `BackgroundFetchSchedulerTest.*:BackgroundFetchJobControllerTest.*:BackgroundFetch*` (the test embedder doesn't ask for per-site scheduling: Chromium's 2 and 2) | moltnet-shared5 |
| `components_unittests` | `DownloadServiceControllerImplTest.*:*InMemoryDownload*` (the controller's tests build their own `Configuration`; only `BuildInMemoryDownloadService()` raises the limits) | moltnet-shared5 |
| `net_unittests` | `*MemoryOnly*:*InMemory*:*MemBackend*` (without a budget no site list is kept and eviction is Chromium's; the idle sweep runs only with one) | moltnet-shared6 |
| `storage_unittests` | `*StorageAreaImpl*:*LocalStorageImpl*:*SessionStorage*` (parameterized suites, hence the leading `*`; no caches per site: no scheduler, every pipe on the storage sequence as before; totals are kept only with caches per site) | moltnet-shared6 |
| `services_unittests` | `NetworkContextTest.*:*ProxyResolving*:*P2PSocket*:*TCPSocket*:*UDPSocket*:*SocketFactory*:*MdnsResponder*` (contexts without MoltNet's session settings make all of them as before) | moltnet-shared6 |
| `storage_unittests` | `StorageServiceImplTest.*:*StorageAreaImpl*:*LocalStorageImpl*:*SessionStorage*` (no partition outside MoltNet is bound on a thread of its own; without a site's runner no notification is split and no map freed in chunks; namespaces' pipes stay on the sequence) | moltnet-shared7 |
| `net_unittests` | `*MemoryOnly*:*InMemory*:*MemBackend*` (without a budget there is no table of sites and no sweep) | moltnet-shared7 |
| `content_unittests` | `DOMStorageContextWrapperTest.*:DOMStorage*:*StoragePartition*` (the test embedder keeps no DOM storage per site: the controls are bound as before) | moltnet-shared7 |
| `storage_unittests` | `*LocalStorageImpl*:*SessionStorage*:StorageServiceImplTest.*:UsageTrackerTest.*:QuotaManagerImplTest.*` (no caches per site: `GetUsage()` and the memory dump as before, no namespace job; a session storage that isn't the own-thread host's copies and deletes at once; no quota site closed) | moltnet-shared8 |
| `content_unittests` | `BackgroundFetch*:DownloadManagerImplTest.*:*Download*:DOMStorage*` (the test embedder doesn't schedule per site; a download with its own factory or without an initiator gets Chromium's) | moltnet-shared8 |
| `components_unittests` | `DownloadServiceControllerImplTest.*:*InMemoryDownload*` (a client other than Background Fetch, or a fetch whose parameters come back, goes on as before) | moltnet-shared8 |
| `services_unittests` | `*KeepaliveStatisticsRecorder*:NetworkContextTest.*` (without per-site contexts no recorder is made or forgotten) | moltnet-shared8 |
| `content_unittests` | `*StoragePartition*:BrowsingDataRemover*:*ClearSiteData*:DOMStorageContextWrapperTest.*:ClipboardHostImpl*` (the test embedder keeps no DOM storage per site: every deletion lists the partition as before, and every key's clipboard salt comes from the table) | moltnet-shared9 |
| `storage_unittests` | `*LocalStorageImpl*:*SessionStorage*:StorageServiceImplTest.*` (no caches per site: no namespace job and no copy wait; `MoltGetUsageForStorageKey()` answers as `GetUsage()` filtered to the key) | moltnet-shared9 |
| `components_unittests` | `DownloadServiceControllerImplTest.*` (every profile: a take-up while the parameters are asked for is remembered and asks again; other clients' downloads go on as before) | moltnet-shared9 |
| `net_unittests` | `CookieMonster*:*CookieMonster*:SSLClientSessionCacheTest.*:HttpAuthCacheTest.*:HttpServerProperties*:SpdySessionPool*:TransportClientSocketPool*:ClientSocketPoolManager*:HttpNetworkSession*` (without `MoltKeepNonceCookieLimits()` the store keeps no nonce totals and keeps its size entries as before; the per-nonce methods are called by MoltNet's context only) | moltnet-shared10 |
| `services_unittests` | `NetworkContextTest.*:*CookieManager*:PreflightCache*:PreflightController*:*Cors*` (the new field off: the context's cookie store is made as before and `MoltDropNonce()` does nothing) | moltnet-shared10 |
| `storage_unittests` | `QuotaManagerImplTest.*:QuotaDatabaseTest.*` (no site closed outside MoltNet: buckets made as before; the new query is used by MoltNet's deletions only) | moltnet-shared10 |
| `content_unittests` | `*StoragePartition*:BrowsingDataRemover*:*ClearSiteData*` (the test embedder keeps no DOM storage per site: a response without a document names no key, and a deletion over a range lists every bucket, as before) | moltnet-shared10 |
| `net_unittests` | `SSLClientSessionCacheTest.*:HttpAuthCacheTest.*:HttpServerProperties*:BrokenAlternativeServicesTest.*:CookieMonster*:*CookieMonster*` (without `MoltKeepPerSiteShares()` each cache is its base: the same evictions, memory-pressure behaviour, `max_size()` and `GetOrPut()` that never returns `end()`; `HttpAuthCache::Add()` never returns null; partitions without a nonce are counted only with `MoltKeepNonceCookieLimits()`; `MoltClearForNonce()` is called by MoltNet's context only) | moltnet-shared11 |
| `services_unittests` | `NetworkContextTest.*:PreflightCache*:PreflightController*:*Cors*:SharedDictionaryManager*:*SharedDictionary*` (the new fields off: no shares, QUIC as configured, no refused nonce; the preflight cache purges at random as before, the in-memory dictionary manager follows `SetCacheMaxSize()` and memory pressure as before) | moltnet-shared11 |
| `browser_tests` | `ProfileNetworkContextService*` (other profiles get no `molt_refused_nonces`; the hook reads the tab helpers' record without making one) | moltnet-shared11 |
| `media_unittests`, `blink_platform_unittests` | `*Wsola*:AudioRendererAlgorithm*`; `*JXL*:*Jxl*` (outside MoltNet both pick their code as before) | fp-arch |
| `v8_unittests`, V8's `mjsunit` | `*Float16*`; the `mjsunit` tests with `float16` in their name once more with `--no-enable-f16c` (V8's software conversion everywhere) | fp-arch |
| `blink_unittests`, `media_unittests`, `services_unittests` | `AudioProcessingPropertiesToAudioProcessingSettingsTest.*:MediaStreamAudioProcessor*:ProcessedLocalAudioSource*`; `CreateWebRtcAudioProcessingModuleTest.*:AudioProcessor*`; `AudioProcessorHandler*:InputController*:*InputStream*` (outside MoltNet the module is built and runs as before; `MoltNetAsksForCpuIndependentProcessing` is the new case) | fp-probes2 |
| `blink_unittests`, `blink_platform_unittests`, `media_unittests`, `services_unittests` | `UserMediaClientTest.*:*EchoCancellationMode*:MediaStreamAudioProcessingLayout*:MediaStreamConstraintsUtilAudioTest.*:MediaStreamConstraintsRemoteAPMTest.*:MediaDevicesTest.*`; `AudioServiceAudioProcessorProxyTest.*:MediaStreamAudioTest.*`; `MoltPortableMediaTest.*:SincResampler*:VectorMath*:AudioConverter*`; `OutputDeviceMixerImplTest*:OutputDeviceMixerManagerTest.*:MixingGraphImpl.*:MixingGraphInputTest.*:AudioProcessorHandlerTest.*` (outside MoltNet the effects, the echo canceller's choice and the mixing are as before; the new cases are `UserMediaClientTest.MoltNetMicrophoneHasNoPlatformEffects`, `EchoCancellationModeTest.MoltNetUsesWebRtcEchoCancellation`, `MoltPortableMediaTest.NoCaptureEffectsInPortableMode`, `PortableAudioThreadScope`, `PortableAudioThreadMixesWithTheCCode` and `OutputDeviceMixerImplTest.MixesPortablyWhileCpuIndependentListens`) | fp-mic2 |
| `blink_unittests`, `blink_platform_unittests`, `media_unittests`, `services_unittests` | `MoltNetSubnormalsTest.*:MoltNetMediaDevicesTest.*:MoltNetMediaValuesTest.*:UserMediaClientTest.*:MediaDevicesTest.*:MediaStreamConstraintsUtilAudioTest.*:*WebAudio*:*AudioWorklet*:*Biquad*:*DynamicsCompressor*:*AVIF*`; `DenormalDisablerTest.*`; `MoltPortableMediaTest.*:MoltPulseCaptureSourceTest.*:SincResampler*:AudioRendererAlgorithm*:AudioProcessorTest*:*Pulse*`; `MixingGraphImpl.*:MixingGraphInputTest.*:OutputDeviceMixerImplTest*:AudioProcessorHandlerTest.*:InputController*` (outside MoltNet the flush modes, the format, the names and the device a stream opens are as before; the new cases are `MoltNetSubnormalsTest.*`, `MoltNetMediaDevicesTest.TracksGetTheSameNames` and `GenericNamesAndGroups`, `UserMediaClientTest.MoltNetMicrophoneHasUniformFormatAndNames`, `MoltPortableMediaTest.FlushTinyPortableSamplesKeepsTheFloor`, `ScopedPortableSubnormalsComputesSubnormals`, `PortableResamplerFlushesItsKernelAndInput`, `PortableCaptureParametersAreTheSameForAll`, `CommunicationsCategoryOnlyWithRawMode`, `PortableCaptureStreamEffects`, `PortableCaptureOutputFormat` and `MoltPulseCaptureSourceTest.*`, the last on Linux) | fp-mic3 |

| `blink_unittests`, `blink_platform_unittests`, `media_unittests`, `content_unittests` | `MoltNetCameraValuesTest.*:MoltNetCameraTest.*:UserMediaClientTest.*:VideoTrackAdapter*:MediaStreamVideoTrack*:ApplyConstraintsProcessor*:MediaStreamConstraintsUtilVideoDevice*:MediaStreamVideoSource*:MediaDevicesTest.*:InputDeviceInfo*:MoltNetMediaDevicesTest.*:MoltNetMediaValuesTest.*:MoltNetSubnormalsTest.*:*IIRFilter*:*AudioContext*:*HTMLMediaElementAudioOutputDevice*:MediaStreamConstraintsUtilAudio*`; `*IIR*`; `MoltPortableMediaTest.*:ChannelMixer*:AudioProcessor*`; `RenderFrameAudioInputStreamFactory*:ForwardingAudioStreamFactory*:AudioInputStreamBroker*` (outside MoltNet cameras, channels, outputs, `IIRFilterNode`, a device's layout and a stream's processing are as before; the new cases are `MoltNetCameraTest.*`, `UserMediaClientTest.MoltNetCameraTrackReportsTheMenu`, `MoltNetApplyConstraintsKeepsTheCameraOpen` and `MoltNetChannelCountFollowsTheRequest`, `VideoTrackAdapterTest.MoltNetCameraDesiredSizeIsTheTarget`, `VideoTrackAdapterFixtureTest.MoltNetCameraFramesHaveTheTrackSize` and `MoltNetCameraDropsFramesItCannotRead`, `MoltNetMediaDevicesTest.CommunicationsOutputIsRefused`, `MoltNetSubnormalsTest.MoltNetFloorsTheIirFiltersHistory` and `IirTailTimeKeepsItsValue`, `MoltPortableMediaTest.FlushTinyPortableSampleFloorsDoubles`, `PortableCaptureNeedsDownmix` and `DownmixGoldenDigests`) | fp-media4 |

- [ ] Build and run each row above (from the PRs in the table):
  ```bash
  cd chromium/src
  autoninja -C out/MoltTests services_unittests net_unittests components_unittests unit_tests \
    blink_unittests blink_platform_unittests content_unittests media_unittests storage_unittests
  out/MoltTests/services_unittests --gtest_filter='NetworkContextTest.*:...'   # one run per row
  ```
  *Expected:* as in §5.3.

- [ ] MoltNet on an old processor (fp-arch). On a Linux machine with KVM,
  start two virtual machines with the Linux package installed, one whose
  processor lacks SSE4.1 and one that has it:
  `qemu-system-x86_64 -enable-kvm -cpu Conroe ...` (a 65 nm Core 2: SSE3
  and SSSE3, no SSE4.1) and the same with `-cpu Penryn` (SSE4.1). In each,
  open a normal window and run in the DevTools console
  `WebAssembly.validate(new Uint8Array([0,97,115,109,1,0,0,0,1,5,1,96,0,1,123,3,2,1,0,10,8,1,6,0,65,0,253,15,11]))`
  (a module with one SIMD instruction).
  *Expected:* on Conroe it is `false`, MoltBrowser's normal and Incognito
  windows browse as usual, and MoltNet doesn't open from the MoltNet
  button (its panel shows "MoltNet can't open on this computer: its
  processor lacks SSE4.1 ..." before any click), from `molt://ai-settings`
  (the same sentence as the MoltNet status), from the AI chat (`/tor
  launch` and opening a page in MoltNet answer with it) or from an
  `.onion` address's "Open in MoltNet" (a message box with it); `ps`
  shows no `tor` started by MoltBrowser. On Penryn it is `true` and
  MoltNet opens and loads a page.

- [ ] The microphone in MoltNet and in a normal window at once
  (fp-probes2). With speakers (not headphones) playing a video in a normal
  window, record 10 s in a MoltNet window and in a normal window at the
  same time on a voice-memo page (or `webrtc.github.io`'s getUserMedia
  sample with "echo cancellation" on, or the leak kit's `mic.html`), on an
  x86-64 machine and on an Apple silicon Mac. *Expected:* both recordings
  have the video's sound cancelled the same way (Chromium-wide echo
  cancellation works for MoltNet's stream too) and no dropouts;
  `chrome://media-internals`' Audio tab shows both input streams with echo
  cancellation, noise suppression and gain control. Note the audio
  service's CPU use (the task manager's "Audio Service" row) with one
  MoltNet stream against one normal stream: FINGERPRINTING.md "MoltNet: the
  microphone's processing" measured about 0.25 ms against 0.1 ms per 10 ms
  of audio on a 2.1 GHz Xeon; correct it with what you see.

- [ ] The microphone's platform processing, on each OS (fp-mic2), with a
  real microphone: a Mac's built-in one (macOS reports its noise
  reduction), a Windows laptop's (once more with
  `--enable-features=EnforceSystemEchoCancellation`, which makes Windows
  report its communications effects), and Linux with PipeWire (and
  PulseAudio where you have it). In a MoltNet window and in a normal
  window, on any https page, run in the DevTools console:
  ```js
  const s = await navigator.mediaDevices.getUserMedia({audio: {echoCancellation: 'all'}});
  const t = s.getAudioTracks()[0];
  JSON.stringify([t.getCapabilities(), t.getSettings()]);
  ```
  *Expected* in MoltNet, the same on every OS: `echoCancellation` lists
  `true`, `false`, `"remote-only"` and `"all"`, `autoGainControl` and
  `noiseSuppression` `true` and `false`, `voiceIsolation` only `false`;
  the settings say `echoCancellation: "all"`, `noiseSuppression: true`,
  `autoGainControl: true`, `voiceIsolation: false` (and, since fp-mic3,
  `sampleRate` 48000, `channelCount` 1 and `latency` 0.01 whatever the
  device). In the normal window
  156's own answers (`"all"` on Windows and macOS only). Then
  `chrome://media-internals`' Audio tab: MoltNet's input stream has no
  platform effects and echo cancellation, noise suppression and gain
  control in the audio service; its log shows no system echo cancellation
  (on Windows no "attempting to enable system AEC", and
  `RawProcessingSupported=true` on a device that has raw mode; on macOS
  "No voice processing requested"). Record 10 s of speech in MoltNet with
  a video playing on the speakers in a normal window, with
  `echoCancellation: 'all'` and with the default: the video's sound is
  cancelled the same way in both. On the Mac, record with a fan or
  background noise in MoltNet and in a normal window, one after the other
  and then at once: MoltNet's recording is denoised by WebRTC's
  suppressor (the stream turns the microphone's own noise reduction off
  while it runs, a setting of the device); note what the normal window's
  recording sounds like meanwhile.
- [ ] The kit's microphone cases on macOS (fp-mic2; the leak kit itself
  runs on Linux). Write the kit's WAV
  (`python3 -I -c "import sys; sys.path.insert(0, 'tools/leak_test'); from leakkit import micinput; open('/tmp/mic-input.wav', 'wb').write(micinput.wav_bytes())"`),
  start the app with `--use-fake-device-for-media-stream
  --use-file-for-fake-audio-capture=/tmp/mic-input.wav%noloop
  --use-fake-ui-for-media-stream --disable-features=AudioServiceSandbox`
  (the sandboxed audio service can't read the file), open a MoltNet
  window on any https page, paste `tools/leak_test/pages/static/ml.js` and
  then `mic.js` into its DevTools console, and run
  `await mlMicProcessing()`, then `await mlMicPlayback()`. *Expected:* the
  values the kit records on Linux (`audio.mic_processed`,
  `audio.mic_system_aec` and `audio.mic_remote_only` the same hash as on
  Linux x86-64 and arm64, the capabilities and settings as in
  docs/LEAK_TESTING.md); send the output with the Mac's chip.
- [ ] A microphone's format and the devices' names (fp-mic3), on each OS
  with real devices: a USB or Bluetooth headset (16 or 44.1 kHz, or a
  96 kHz interface if you have one) and the built-in microphone. In a
  MoltNet window and a normal one, on any https page, in the DevTools
  console:
  ```js
  const s = await navigator.mediaDevices.getUserMedia({audio: true});
  const t = s.getAudioTracks()[0];
  const d = await navigator.mediaDevices.enumerateDevices();
  JSON.stringify([t.label, t.getSettings(), t.getCapabilities(),
      d.map((x) => [x.kind, x.label, x.deviceId.slice(0, 8), x.groupId.slice(0, 8),
          x.getCapabilities ? x.getCapabilities() : null])]);
  ```
  *Expected* in MoltNet, the same on every device and OS: `sampleRate`
  48000, `sampleSize` 16, `channelCount` 1, `latency` 0.01; the
  capabilities' `sampleRate` 48000 to 48000, `channelCount` 1 to 2,
  `latency` 0.01 to 0.01, and the same in every `audioinput`'s
  `getCapabilities()`; labels "Default" or "Microphone", "Camera" or
  "Speaker" and the ID's first four characters; every `groupId` its
  device's ID; on Windows no `communications` device. In the normal
  window the device's values and names. Then, in MoltNet:
  `getUserMedia({audio: {deviceId: {exact: <the headset's ID>}}})` opens
  the headset (the label says which ID), `{channelCount: 2}` gives two
  channels from a mono headset, `{sampleRate: {exact: 44100}}` fails with
  `OverconstrainedError`, and with every effect off
  (`{echoCancellation: false, noiseSuppression: false, autoGainControl:
  false}`) a recording of a 1 kHz tone at the headset's own rate plays
  back at the right pitch and without crackle (the processing converts it).
  Unplug the headset while recording: the track ends and `devicechange`
  fires, as in the normal window.
- [ ] The OS's processing outside the browser (fp-mic3). **Linux:** with
  PipeWire, load the echo canceller (`pactl load-module
  module-echo-cancel`, or PipeWire's `libpipewire-module-echo-cancel` in a
  `pipewire.conf.d` file) and make its source the default (`pactl
  set-default-source` with the "echo-cancel" source); in MoltNet record
  10 s of speech with a video playing on the speakers in a normal window,
  then pick the echo-cancel source explicitly by `deviceId`; then the same
  with PulseAudio instead of pipewire-pulse if you have a machine with
  it. *Expected:* `pactl list source-outputs` shows MoltNet's recording
  stream on the microphone's own source (an `alsa_input...`), not on the
  echo-cancel source, while the normal window's stream is on the
  echo-cancel source; MoltNet's recording is cancelled by WebRTC's echo
  canceller. Send `pactl list sources` (the filter's `device.class`,
  `device.master_device`, `node.link-group` and flags) and `pactl list
  source-outputs` either way. Then (redteam7) a virtual microphone that
  plays applications' output (`pactl load-module module-null-sink
  sink_name=virtual_speaker`, `pactl load-module module-remap-source
  master=virtual_speaker.monitor source_name=virtual_mic`; on PipeWire
  also a `pw-loopback --capture-props='media.class=Audio/Sink'
  --playback-props='media.class=Audio/Source'`), picked by `deviceId` in
  MoltNet with every effect off while a video plays into
  `virtual_speaker`: *Expected:* `pactl
  list source-outputs` shows MoltNet's stream on `virtual_mic` (or the
  loopback's source), never on an `alsa_input...` microphone, and the
  recording is the video's sound. **Windows:** a microphone without raw mode
  (Device Manager's driver, or `System.Devices.AudioDevice.RawProcessingSupported`
  false in the device's properties; many webcam microphones), and one
  with more than 8 channels if you have one (a microphone array or a
  multichannel interface): `chrome://media-internals`' log for MoltNet's
  stream shows `RawProcessingSupported=false` and no
  `SetCommunicationsCategoryAndMaybeRawCaptureMode` on the first, and on
  the second no `SetCommunicationsCategoryAndMaybeRawCaptureMode` (a
  normal window's stream shows it there, with `channels` above 8); both
  record. Note what the recordings sound like against a normal window's.
- [ ] A real camera on each OS (fp-media4): a Mac's built-in camera, a
  Windows laptop's, a USB webcam on Linux (and, if you have them, an HD
  USB webcam with MJPEG modes on Windows or Linux, a camera whose largest
  mode is 640x480, and an iPhone or other continuity camera on the Mac).
  In a MoltNet window and in a normal one, on any https page, in the
  DevTools console:
  ```js
  const s = await navigator.mediaDevices.getUserMedia({video: true});
  const t = s.getVideoTracks()[0];
  const r = new MediaStreamTrackProcessor({track: t}).readable.getReader();
  const f = (await r.read()).value;
  const d = await navigator.mediaDevices.enumerateDevices();
  JSON.stringify([t.getSettings(), t.getCapabilities(),
      [f.codedWidth, f.codedHeight, f.visibleRect, f.displayWidth, f.format],
      d.filter((x) => x.kind === 'videoinput').map((x) => x.getCapabilities())]);
  f.close();
  ```
  then the same with `{video: {width: {ideal: 1920}, height: {ideal:
  1080}}}`, `{video: {width: {exact: 800}, height: {exact: 600}}}`,
  `{video: {width: {min: 1281}}}` and, on the first track,
  `t.applyConstraints({width: 1280, height: 720})`. *Expected* in
  MoltNet, the same on every OS: 640x480 at 30 fps with `resizeMode`
  `"none"`; capabilities `width` 1 to 1280, `height` 1 to 720,
  `frameRate` 0 to 30 (`InputDeviceInfo`: 1 to 30) for an HD camera,
  640 and 480 for a camera that isn't one; the frame's coded size,
  visible rectangle and display size the track's size exactly, `format`
  `"I420"`; Full HD asked for gives 1280x720 (`"none"`), 800x600
  `"crop-and-scale"`, a width above 1280 `OverconstrainedError`
  (`width`), and `applyConstraints()` gives 1280x720 without the camera's
  light going off and on (the device isn't reopened). In the normal
  window the device's own modes. Watch the picture in a `<video>` for 30 s
  at each size: it is the camera's centre, cropped to 4:3 from a 16:9
  camera, without stretching; send the CPU use of the renderer and the GPU
  process in MoltNet against the normal window (FINGERPRINTING.md "A
  camera's formats" measured the conversion at 2.1 to 2.5 ms per frame on
  a 2.1 GHz Xeon). Windows only: note whether `chrome://media-internals`
  shows the camera's frames as GPU textures; a MoltNet track whose
  frames all drop (a black `<video>`) means frames the CPU can't map:
  send the log.
- [ ] A microphone of another layout (fp-media4), where you have one: a
  Windows microphone array that reports QUAD or 5.1 (Sound settings or
  `chrome://media-internals` show its channel count and layout), or on
  Linux a four- or six-channel PulseAudio source. In MoltNet,
  `getUserMedia({audio: true})` and the same with every effect off record
  (10 s of speech plays back cleanly), `getSettings()` shows 48000 and
  one channel, and `chrome://media-internals` shows the device opened in
  its own layout; in a normal window the processed stream fails or falls
  back as 156 does (note which).
- [ ] `setSinkId("communications")` on Windows (fp-media4): on a page with
  an `<audio>` element and an `AudioContext`, after the microphone
  permission, in MoltNet `await audio.setSinkId('communications')`
  rejects with `NotFoundError` (and with `NotAllowedError` before the
  permission), as `setSinkId` of an unknown ID does; `new
  AudioContext({sinkId: 'communications'})` and `ctx.setSinkId
  ('communications')` fail as for an unknown ID. In a normal window both
  select the communications device.
- [ ] Subnormal floats on real arm64 hardware (fp-mic3): the leak kit's
  `audio.subnormal_graph` and `audio.subnormal_resampled` (§8) on an
  Apple silicon Mac (paste `ml.js` and `fp.js` into a MoltNet window's
  console and run the page's collection, or run the kit on Linux arm64)
  equal an x86-64 machine's. Note the CPU use of a Web Audio demo that
  plays a long reverb tail into silence (a `ConvolverNode` or a delay
  with feedback) in MoltNet and in a normal window, on the arm64 machine
  and an x86-64 one: FINGERPRINTING.md "MoltNet: subnormal floats"
  measured x86-64 only.

### 5.3 Known-expected failures, and what to do

| Test | Why it fails | What to do |
|---|---|---|
| `TranslateManagerTest.*`, `TranslateLanguageListTest.*` (components_unittests), `TranslateServiceTest.*`, `TranslateBubbleViewTest.*` (unit_tests), Translate `browser_tests` | They assume Google's path: the translate script fetch, the language-list fetcher, the API-key and offline gates, partial translate, `hrefTranslate` auto-translate, the Google wordmark (docs/design/PAGE_TRANSLATION.md §9) | Expected (from #48). Go through each failing case: it is fine only if it asserts one of those behaviours. A crash, or a failure about prefs, language detection or the bubble's states, is a bug. List the expected failures here, under this row, so the next rebase compares against them. |
| `AboutFlagsHistogramTest.CheckHistograms` | The `molt-*` flags aren't in `LoginCustomFlags` in `enums.xml` (from #39) | Either accept it (owner's call, §11), or in `chromium/src` run `python3 tools/metrics/histograms/generate_flag_enums.py out/MoltTests`, which runs the test and adds the entries, then import the changed upstream file into `src/` (an unchanged import commit first, then the edit) (verify the command). |
| `NetworkContextTest`, `CertNetFetcherURLLoaderTest`, `CertVerifierServiceFactoryTest`, `SOCKSConnectJobTest` | Should not fail: the new fields default to off, and contexts without a password keep the fixed one | A failure here is a bug in #51 or #55. |
| `AboutFlagsTest.*` | Should pass (metadata and never-expire entries were added) | Fix `flag-metadata.json` if not. |
| `BlobMemoryControllerTest`, `BlobRegistryImplTest`, `RenderProcessHostUnitTest`, `ChildProcessSecurityPolicyTest`, `CertVerifier*`, `CertNetFetcherURLLoaderTest`, `TransportClientSocketPoolTest` | Should not fail: the new limits and fields default to 0 or empty, and only Chrome's client keeps BrowsingInstances apart, for MoltNet | A failure here is a bug in moltnet-shared2. |

- A compile error that a class "is abstract" because of
  `MoltKeepCachesPerSite()` (moltnet-shared3): a test fake or mock of
  `storage::mojom::LocalStorageControl` or `SessionStorageControl`; add
  an empty override (`void MoltKeepCachesPerSite() override {}`) there.
- The same for `MoltSetGrowthObserver()` (moltnet-shared4): add
  `void MoltSetGrowthObserver(mojo::PendingRemote<storage::mojom::MoltDomStorageGrowthObserver>) override {}`.
- An upstream `StorageAreaImplTest`, `LocalStorageImplTest` or
  `SessionStorageImplTest` case that expects a key to read back empty
  after a write its area's quota refused (moltnet-shared5): the old value
  now stays, in every profile; update the expectation.
- A regular profile's Background Fetch test that expects cookies on a
  request whose fetch asked for `credentials: 'omit'` (moltnet-shared4):
  the request now carries the fetch's mode in every profile, as the
  site's own `fetch()` would; update the expectation.

### 5.4 The DCHECK pass

- [ ] Run the `out/MoltTests` browser through the MoltNet basics: open and
  close MoltNet windows, New Identity, an extension allowed in Incognito,
  the ⋮ menu and a video's controls (Cast), then quit (from #21, #23).
  *Expected:* no `DCHECK` or `CHECK` failure (`MediaRouterFactory` CHECKs
  `MediaRouterEnabled()`).
- [ ] The same build, MoltNet's per-site pools and ended identities
  (moltnet-policy): open a dozen sites in MoltNet tabs, close them, wait
  two minutes and open another (unused per-site pools are dropped then),
  then New Identity with pages still loading, and with DevTools open on a
  page that asks "Leave site?". *Expected:* no `CHECK` in
  `~TransportClientSocketPool()` (`higher_pools_`, `group_map_`) or in
  `UnloadController`/`BrowserCloseManager`, and no MoltNet window left.
- [ ] The same build, MoltNet's shared limits (moltnet-shared): fifteen
  MoltNet tabs of different sites, each loading a page with a few hundred
  images from hosts of its own; close half of them while they load, open
  an onion site, then New Identity with pages still loading; a page that
  sends 300 `navigator.sendBeacon()`s and closes at once. *Expected:* no
  `CHECK` or `DCHECK` in `ClientSocketPool`/`TransportClientSocketPool`
  (`handed_out_socket_count_`, the stalled-group loop), `SOCKSConnectJob`
  (`next_state_`), `KeepaliveStatisticsRecorder` or `URLLoaderFactory`, no
  dangling-pointer report for `molt_socket_gate_` or
  `molt_socks_handshake_limiter`, and every tab of the new identity loads.
- [ ] The same build, MoltNet's blobs, processes and identity end
  (moltnet-shared2): six MoltNet tabs each building and dropping 16 MiB
  blobs in a loop (and one recording a `MediaRecorder` of a canvas),
  a popup opened from one of them and a tab of the same site; New
  Identity while they run and while one of them keeps calling `fetch()`;
  close the browser during the first second of a New Identity. *Expected:*
  no `DCHECK` or `CHECK` in `BlobMemoryController` (`DCHECK_LE(length,
  blob_memory_used_)`, `pending_memory_quota_tasks_`), `BlobStorageContext`,
  `RenderProcessHostImpl` (the process selection checks after
  `IsSuitableHost()`), `ChildProcessSecurityPolicyImpl` or
  `MoltNetRouting`'s refusal check (no use after the profile is gone), and
  the new identity's window opens. Then a MoltNet page whose twenty workers
  each send synchronous `XMLHttpRequest`s to six slow hosts while the page
  holds six downloads from each: no `DCHECK` in `TransportClientSocketPool`
  (`Group::SanityCheck()`, `CheckForStalledSocketGroups()`), and no hang.
- [ ] The same build, MoltNet's caches and ended identities
  (moltnet-shared3): twenty MoltNet tabs of different sites loading
  image-heavy pages and storing `localStorage` from a dozen subdomain
  iframes each, a page whose server leaves out its intermediate
  certificate in each; leave them 35 minutes, then New Identity while
  they load, while one sends `fetch(url, {keepalive: true})` every 50 ms
  and one runs a Background Fetch. *Expected:* no `DCHECK` or `CHECK` in
  `MemBackendImpl` (`current_size_`, the LRU list), `MemEntryImpl`,
  `LocalStorageImpl`, `SessionStorageImpl` (`data_maps_` emptied in the
  destructor), `KeepAliveURLLoader` (its state checks after the posted
  completion), `URLRequest` (cancelled requests' delegates) or
  `ClientSocketPool` (the certificate-fetch member), and the new
  identity's window opens.
- [ ] The same build, ends that race a page's requests (moltnet-shared4):
  a MoltNet page that opens a WebSocket, a `fetch()` and an `<img>` every
  10 ms, and one whose service worker runs a Background Fetch of a large
  file while another site's does; New Identity while they run, three
  times; then a page writing `localStorage` from ten subdomain iframes
  and `sessionStorage` in a tab it opened with `window.open()`, until
  writes fail. *Expected:* no `DCHECK` or `CHECK` in `NetworkContext`
  (`url_loader_factories_`, `web_transports_`), `URLLoaderFactory`,
  `WebSocketFactory`, `SOCKS5ClientSocket` (`next_state_`, a write
  callback run twice), `SOCKSConnectJob`, `URLRequest`,
  `BlobMemoryController` (`blob_memory_used_` after the held memory is
  released), `InMemoryDownloadImpl`, `BlobTaskProxy` (released on the IO
  thread), `StorageAreaImpl` (`storage_used_`, `map_state_` after a fork)
  or `SessionStorageImpl`, and the new identity's window opens.

## 6. Fuzzers

Two passes: the GN targets against the real libraries, and the standalone
tier CI doesn't run. The GN build uses Chromium's own clang, so it runs on
the Mac or on Linux (a third out dir); the standalone tier needs an LLVM
clang with libFuzzer (Apple's has none), so it is easiest on Linux
(docs/FUZZING.md).

- [ ] libFuzzer build of every target `tools/fuzz/molt_fuzz.py list` names
  (34 at #55; from #41 and the PRs that added fuzzers since: #44, #45, #47,
  #48, #49, #53, #54; agent-replay adds `molt_automation_locators_fuzzer`,
  `molt_automation_tool_steps_fuzzer` and `molt_frame_tools_fuzzer`)
  (fonts adds `molt_moltnet_font_set_fuzzer`)
  (helpers-win adds `molt_ocr_image_fuzzer` and `molt_recorded_wav_fuzzer`)
  (verify the command):
  ```bash
  cd chromium/src
  gn gen out/libfuzzer --args='use_libfuzzer=true is_asan=true is_ubsan_security=true is_debug=false dcheck_always_on=true'
  autoninja -C out/libfuzzer chrome/browser/molt_ai/fuzzers:molt_fuzzers 2>&1 | tee ~/molt-v0.3.0/fuzz-build.log
  ```
  *Expected:* every target builds. Watch for: `base::Value`/`JSONReader`,
  GURL and Origin, `base::span` in the farbling fuzzer, llama.cpp's internal
  grammar API, `seed_corpus` and `dict` paths, DEPS rules and visibility
  (from #41).
- [ ] Run each for 5-10 minutes on its seed corpus (from #41; docs/FUZZING.md):
  ```bash
  cd "$REPO"
  python3 tools/fuzz/molt_fuzz.py list | while read -r name tier dir; do
    mkdir -p ~/molt-v0.3.0/fuzz/$name
    chromium/src/out/libfuzzer/$name -max_total_time=300 -timeout=25 -rss_limit_mb=2560 \
      -artifact_prefix=$HOME/molt-v0.3.0/fuzz/$name/ \
      ~/molt-v0.3.0/fuzz/$name "$dir/corpus/$name" < /dev/null || echo "FINDING: $name"
  done
  ```
  *Expected:* no `FINDING`. A finding: replay it, fix, add a unit test and
  the input as `corpus/<name>/regress_<what>` (docs/FUZZING.md).
- [ ] The standalone `stand_ins` tier, which CI skips (from #41; docs/FUZZING.md
  "Not covered yet"), on Linux with clang and `libssl-dev`:
  `python3 tools/fuzz/molt_fuzz.py smoke --tier stand_ins` → every fuzzer
  passes.

## 7. Device and manual checks

Use the dev-installed build for the features, and the signed, notarized
release build (§10.2) for the codesign, notarization and helper checks.
Run each feature in a regular, an Incognito and a MoltNet window unless the
item says otherwise.

### 7.1 macOS

- [ ] Pages render: `dev-install-mac.sh`'s gate passes, and the release app
  too (renderer and GPU helpers keep `allow-jit`) (from CLAUDE.md).
- [ ] On-device AI in the inference sandbox (from #15; SANDBOXED_INFERENCE.md §6):
  start with `--enable-sandbox-logging` and watch
  `log stream --predicate 'sender=="Sandbox"'`, then load a model in
  `molt://ai` and chat.
  *Expected:* Metal initialises with `default.metallib` from the framework's
  `Resources/`; no sandbox denial except `machdep.cpu.brand_string`; it
  works in the notarized build too. Note first-token latency and tokens/s
  against v0.2.5's in-process engine. Once the vision model is pinned:
  image input on Metal, with mtmd warming up inside the sandbox, and its
  real RAM use (from #38).
- [ ] Inference process recovery (from #15, #53): `kill -9` the
  inference utility mid-answer. *Expected:* the chat says the answer
  stopped and the next message reloads the model; three crashes within ten
  minutes refuse the model for a while. After 10 minutes idle the process
  exits; under `sudo memory_pressure -S -l critical` the model unloads
  (verify the command); deleting a loaded model works; the browser's Task
  Manager shows the process with its label.
- [ ] The tesseract bundle (from #17):
  `./scripts/bundle-tesseract.sh --app chromium/src/out/MoltBrowser/MoltBrowser.app`.
  *Expected:* it ends with its load check (only `Resources/ocr/lib/` and
  the OS's libraries) and a `tesseract --version` run from the bundle.
- [ ] OCR in its sandbox (from #16, #17, #18): `/pdf` on a scanned PDF with
  no text layer, and an image attached in the AI chat.
  *Expected:* text with `--- Page N ---` headers (at most 20 pages, a note
  when pages were left out); no tesseract denial in the sandbox log.
- [ ] Voice in its sandbox (from #16, #17): the mic button transcribes;
  `whisper-cli` loads only OS code; an over-long recording hits the
  deadline cleanly.
- [ ] PDFium page rendering under the macOS sandbox profile (from #18): the
  scanned-PDF check above.
- [ ] Codesign and notarization of the release app (from CLAUDE.md "Packaging gotchas"):
  ```bash
  APP=chromium/src/out/MoltBrowser/MoltBrowser.app
  codesign --verify --deep --strict --verbose=2 "$APP"
  codesign -d --entitlements - "$APP/Contents/Frameworks/MoltBrowser Framework.framework/Versions/Current/Helpers/MoltBrowser Helper (Renderer).app" 2>&1 | grep allow-jit
  find "$APP/Contents/Resources/tor" "$APP/Contents/Resources/ocr" "$APP/Contents/Resources/whisper" \
       "$APP/Contents/Frameworks/MoltBrowser Framework.framework/Versions/Current/Libraries" -type f -print0 |
  while IFS= read -r -d '' f; do
    file "$f" | grep -q Mach-O && { codesign -dv "$f" 2>&1 | grep -q 'TeamIdentifier=83DA5U8N7Z' || echo "UNSIGNED: $f"; }
  done
  ls "$APP/Contents/Frameworks/MoltBrowser Framework.framework/Versions/Current/Resources/default.metallib"
  xcrun stapler validate dist/MoltBrowser-macOS-arm64.dmg
  hdiutil attach dist/MoltBrowser-macOS-arm64.dmg
  spctl -a -vvv -t exec /Volumes/MoltBrowser/MoltBrowser.app
  hdiutil detach /Volumes/MoltBrowser
  ```
  *Expected:* valid signatures, `allow-jit` present, no `UNSIGNED` line
  (`release.sh` now stops on a failure to sign the `Resources/` helpers and
  checks their hardened runtime; this re-checks them and the framework's
  `Libraries/` dylibs in the shipped app),
  `default.metallib` present, `accepted` / `source=Notarized Developer ID`,
  `The validate action worked!`.
- [ ] Upgrade over a v0.2.5 profile, here and on Linux (from #11): existing
  `keys.enc`, `vault.enc`, `profile.enc` and chat history still open after
  the move to `os_crypt_async`.
- [ ] Close the last MoltNet window: the profile is destroyed, no crash
  (from #11).
- [ ] Toolbar look in horizontal and vertical tabs (from #11).

### 7.2 Linux

- [ ] The inference sandbox (from #15): load a model and chat.
  *Expected:* no SIGSYS (the `MAP_POPULATE` patch); the process is listed
  on `chrome://sandbox`; an 8B and a 20B model load with repacking under
  `RLIMIT_DATA`; `socket(AF_INET)` is denied.
- [ ] The helper sandbox (from #16): with the distribution's root-owned
  `/usr/bin/tesseract` (and `/usr/bin/whisper-cli` if packaged), `/pdf` on a
  scanned PDF and voice input work; on Linux 5.13 or later Landlock is
  applied (an older kernel logs that the layer is skipped).
- [ ] PDF rendering under seccomp (from #18): the scanned PDF above gives
  text.
- [ ] Packages (from #25): `dpkg-deb -c dist/MoltBrowser-Linux-x64.deb | awk '{print $2}' | sort -u`
  prints only `root/root`; the `.deb` (xz) installs on Debian 11 and
  Ubuntu; the `.rpm` installs on Fedora.
- [ ] CPUs without AVX2 (from the CPU-dispatch PR; SANDBOXED_INFERENCE.md
  §12): on an x86-64-v2 machine without AVX (e.g. a Nehalem/Westmere VM, or
  QEMU `-cpu Nehalem`) and on an AVX-only one (Sandy/Ivy Bridge), a model
  loads and answers, and `--v=1` logs the kernel level (`x86-64-v2` /
  `avx`); on an AVX2 machine it logs `avx2`. A CPU below x86-64-v2 (no
  SSE4.2/POPCNT) gets the "doesn't support SSE4.2" message, not a crash.
- [ ] On the official (ThinLTO) build: `llvm-nm` on the
  `ggml_cpu_x86_{v2,avx,avx2}` objects shows only the `molt_x86_<level>_*`
  functions and no weak symbols (from the CPU-dispatch PR;
  `src/third_party/llama_cpp/README.chromium`) (verify the command).

### 7.3 Features

**Page translation** (from #48, #55; PAGE_TRANSLATION.md §9). Steps 1-4 in
a regular and an Incognito window, 5 in MoltNet, 6 once:

- [ ] Without a model: no bubble on a German page; the icon's bubble says a
  model is needed; *Choose a model* opens `molt://ai`; nothing downloads
  (from #48).
- [ ] With a model, `de.wikipedia.org`: the visible part changes first, the
  footnote counts up, *Stop* stops, *Show original* restores the page
  exactly (`document.body.innerHTML` before and after) (from #48).
- [ ] `translate="no"`, forms, `contenteditable`, code blocks and a shadow
  DOM component: only visible prose changes (from #48); a textarea's
  placeholder is translated and restored, and text directly inside a
  `display: contents` element is translated (from #55).
- [ ] A capture: no request to `translate.googleapis.com` or
  `translate-pa.googleapis.com`, at startup or while translating (from #48).
- [ ] MoltNet: no bubble or address-bar icon; *Translate…* from the menu
  explains it's off; nothing is read (from #48, #55).
- [ ] `chrome://translate-internals` lists the 20 languages (from #48).

**Built-in AI APIs and consent** (from #26, #30, #32, #34, #38, #46, #50;
WEB_AI_APIS.md §12):

- [ ] `LanguageModel`, `Summarizer`, `Writer`, `Rewriter`, `Translator` from
  a page: "downloadable" until the site is allowed; the infobar names the
  top-level site; Allow, Block, *Choose a model*, navigating away. MoltNet:
  all unavailable. Writer and Rewriter exist without an origin-trial token
  (from #26).
- [ ] A click on Allow or Block within 0.5 s of the bar appearing, or right
  after another press, does nothing (from #46, #50).
- [ ] Three closes pause the prompt for 7 days; answers and pauses survive a
  restart; `molt://ai-settings` → "Sites using on-device AI" lists them and
  *Remove* ends a live session; Clear browsing data ("Site settings", "last
  hour"; history) and a profile reset clear them; *Save Settings* keeps
  `"web_ai_apis"`; the settings page works in a regular, Incognito, Guest
  and MoltNet window (from #32).
- [ ] Turning the APIs off (the switch, a policy, the site's answer) ends
  that site's live sessions and later calls fail with a generic error
  (from #50).
- [ ] `LanguageDetector` is unavailable in every window, and Translate still
  detects page languages (from #32).
- [ ] Translator: as a page's first AI call; in an iframe with and without
  `allow="translator"`; in a worker ("unavailable"); `translate()` and
  `translateStreaming()` of a short string and a long article on each
  catalog model; abort mid-article; `TranslatorAPIAllowed=false`; a capture
  with no TranslateKit or component-update request (from #34).
- [ ] `prompt()` with a `responseConstraint` on each catalog model: the
  answer parses and matches; `format` or `pattern` rejects with
  `NotSupportedError`; the gpt-oss and Gemma 4 prefills work; note the
  speed on 262k-token vocabularies (from #30).
- [ ] The chat and a page generating at the same time; abort during a
  generation (from #26).
- [ ] Once the vision model is pinned: `prompt()` with an image element, a
  canvas, a Blob, a huge photo, four images then a fifth (from #38).
- [ ] **The audio check**, with the pinned audio model unhidden in a local
  build (from ai-finish; WEB_AI_APIS.md §5.2, SANDBOXED_INFERENCE.md
  §13.6). The model must pass this before its entry is unhidden in the
  repository:
  - download it from `molt://ai` (model and projector as one unit), allow a
    test page, and `LanguageModel.create({expectedInputs: [{type:
    "audio"}]})`;
  - `prompt()` with a spoken question as a WAV `Blob`, an MP3 `Blob`, an
    `ArrayBuffer` and a 44.1 kHz stereo `AudioBuffer`: a relevant answer
    each time, and the inference process keeps running (a crash shows as
    "the model stopped" and in the browser log; if it crashes, keep the
    entry hidden and note the log);
  - a 30 s clip answers, a 31 s clip and five clips reject with
    `NotSupportedError`; `measureInputUsage()` of a 10 s clip with a short
    question is about 380;
  - the browser log's load line shows the projector loaded with audio on
    (the pinned projector passed the service's own audio check at load: its
    type and stacking factor on the allow-list, and 375 tokens reserved for
    a window), and with `--v=1` the service's prompt token count for one
    clip is 375 plus the text (the per-clip count assumes at most that);
  - on the CPU, and with the Vulkan out dir of §9 on a GPU;
  - `availability()` for audio: `"downloadable"` on a site not yet allowed
    (with and without the model), `"unavailable"` on an allowed site with
    only a text model, `"available"` with the audio model; `create()` with
    audio while the chat's text model is loaded fails with the generic
    error; MoltNet: unavailable.

**The web agent** (from #35, #39, #45, #47, #54):

- [ ] A run with a real model: the agent bar shows a plan of 1-6 steps and
  the current step moves; with `chrome://flags/#molt-agent-planner` off
  there is no plan (from #47). Note the extra start-up time.
- [ ] The step trace shows elements (`button "Go"`), and a malformed reply
  is retried, not acted on (from #35).
- [ ] Save as automation: after a run ends with DONE, *Save as automation*
  → *Review in Agent mode* opens the studio with the draft first; it can't
  run or be scheduled until *Confirm* (its exact sites); then it replays on
  the real sites. In MoltNet the bar explains saving isn't available. A run
  that typed into a password field, or called a site tool, can't be saved
  (from #54).
- [ ] `molt://ai-automation`: trust changes need a real click (from #54).
- [ ] WebMCP site tools on a trial page, with `chrome://flags/#enable-webmcp-testing`
  on and `#molt-agent-site-tools` on (default) (from #45):
  - `getTools()` and `executeTool()` work from the agent's isolated world
    (if not, no tools are listed: it fails closed);
  - a `readOnlyHint: true` tool runs without asking; a form tool always
    asks; `toolautosubmit` submits, and without it the form stays filled and
    the step fails after 20 s.
- [ ] Agent-mode replay on real sites (from agent-replay;
  docs/design/AGENT_ACTIONS.md §3.4): record workflows on three real sites
  (a shop's add-to-cart, a sign-in form with a saved password, a site
  search), then break each recorded selector in DevTools (rename the id and
  classes, move the element, wrap it in a new `<div>`) and replay:
  - a step still finds its element by a strong locator; the Runs view says
    "Found by its role & name (a fallback)" (or label, or attribute);
  - with only the text or the position left, the studio asks "Use it?";
    only a click answers; "Stop the run" stops it; no answer in 120 s fails
    the step;
  - never another kind of element (a link with the button's text), never a
    password field for an ordinary step or the reverse, never a page of
    another origin;
  - a `.molt` file from v0.2.x imports and replays as before.
- [ ] Site-tool steps on a WebMCP trial page (from agent-replay;
  AGENT_ACTIONS.md §3.5): start recording, ask the chat to use a tool,
  Allow: the step appears with what it gives the tool. Replay: a read-only
  tool runs unasked; a tool that changes something asks unless the
  workflow is trusted; a changed schema, a new required argument or a tool
  that became a form stops the step with the reason; a secret-looking value
  is asked at each run; a card or code argument isn't saved.
- [ ] A WebMCP trial page with a cross-origin frame (from agent-replay;
  AGENT_ACTIONS.md §2.7.14): the main page embeds `<iframe
  src="https://<another origin>/tools.html" allow="tools">` whose document
  registers a read-only tool, a tool that changes something, and a card
  tool; a second frame without `allow="tools"`; a third nested inside the
  first. Run the agent there:
  - it asks once: "https://<frame origin>, a frame on https://<page
    origin>, offers 2 tools ..." (the card tool isn't counted as offered
    later, and the other frames never ask);
  - "Not now": the run goes on without them, and doesn't ask again;
  - "This session": TOOLS shows them "from https://<frame origin>"; every
    call asks, the read-only one too, naming both sites; a call with a
    password-looking argument is refused; the call runs in the frame's own
    process (`chrome://process-internals`, out-of-process frame);
  - "Always here": still allowed after a restart; the studio lists it and
    "Forget" removes it; Clear browsing data → "Site settings" removes it;
  - Incognito: no "Always here"; MoltNet window: no question and no frame
    tools; the chat offers the frame's tool only once allowed, naming both
    sites;
  - recorded while the chat calls the frame's tool: the replay asks every
    time, naming both sites.
- [ ] Screenshots, once the vision model is pinned: password and card fields
  and `:autofill` previews masked; same-origin frames masked; a page with a
  frame of another origin (check a PDF `<embed>` too) stays text-only;
  high-DPI, page and pinch zoom; swapping the model mid-run falls back to
  text (from #39, #46, #50).

**The AI chat** (from #36, #40):

- [ ] A typed page-action request in a regular and a MoltNet window: Allow,
  Deny, and navigating away before Allow. Summarising a page that contains
  `[[ACTION ...]]` text does nothing (from #36).
- [ ] `/fill ai` and `/paste` with a real model always ask first and show
  every value; side-panel commands act on the page in the chat's window;
  typing into a `<textarea>` works (from #40).
- [ ] Once the vision model is pinned: its download shows one progress for
  both files; cancel, resume and delete work as one unit (from #38, #40).
- [ ] "This page" commands (`/tldr`, `/translate`) use the tab shown now,
  after switching tabs; a command that finishes at once leaves the chat
  ready for the next message (from #9, #12).

**`@memory` and `@ai` in the address bar** (from #28, #44, #47):

- [ ] `@memory <words>` shows keyword matches (and matches by meaning once
  the embedding model is loaded); `molt://memory/?q=<words>` opens the
  results page. Incognito, Guest and MoltNet: empty dropdown (from #44).
- [ ] `@ai <prompt>` shows no search suggestions; full-width `＠ａｉ` behaves
  the same; a NetLog while typing either shows no request (from #44, #47).
- [ ] After pinning: download the embedding model from `molt://memory`;
  *Search by meaning* works; note RAM, speed, and the re-index time of a
  large index on low-end hardware (from #28).

**Earlier fixes that need a real browser** (from #7):

- [ ] MoltShield strips tracking parameters on every leg of a real redirect
  chain; a Gemma 4 model's thinking never shows in the chat (from #7).

**MoltNet fingerprinting** (from #10, #19, #20, #24, #27, #33, #37). The leak kit
checks most of this on Linux (§8); on macOS by hand, in a MoltNet window
(docs/FINGERPRINTING.md, each section's "Needs the build machine"):

- [ ] Every zoom level: `innerWidth`, `screen.width` and
  `matchMedia('(width: ...)')` stay on the 200×100 steps; background tabs,
  cross-site navigation between differently zoomed sites, the PDF viewer,
  and moving between monitors of different scale factors (from #27).
- [ ] Classic scrollbars (scrollbars always shown) measure 16 CSS px; thin
  12; at 25 % and 500 % zoom; `textarea`, `scrollbar-gutter`,
  `overflow: scroll` with `vw`; regular windows unchanged (from #33).
- [ ] `measureText` and emoji widths match between two Linux machines (1×
  full hinting and 2×), and between two Macs on the same macOS release
  (13, 14 or 15); canvas apps (charts,
  editors) and an ExtendedTextMetrics page still work; with the desktop's
  anti-aliasing off, MoltNet text is still anti-aliased (from #37, #50).
- [ ] Font-probing pages, webglreport-style pages and a WebGPU sample, Shields
  on and off; the macOS font allow-list against a fresh macOS install
  (from #19).
- [ ] A Shields-off site: the decision is in place before page script runs
  (canvas, `hardwareConcurrency`, an about:blank and a cross-site iframe, a
  dedicated worker) (from #10, #24).
- [ ] Japanese, German and Russian UIs give en-US validation messages and
  date inputs; a touchscreen laptop and a multi-monitor setup show the
  uniform values (from #20, #24).
- [ ] Farbled video exports (`captureStream`) in MoltNet: measure the
  readback cost at 720p30 and 1080p30/60 on real hardware (from #10, #20).
- [ ] The display in MoltNet (fingerprinting 156): on a Mac with a P3/XDR
  screen and on an HDR monitor (Windows, Linux), `matchMedia` reports
  `(color-gamut: srgb)`, not `p3`/`rec2020`, `(dynamic-range: standard)`,
  `(video-dynamic-range: standard)`, `(color: 8)` and no `(monochrome)`;
  DevTools' color-gamut emulation still applies; a regular window shows the
  real display. HDR and P3 video and images still play and render.
- [ ] Scrollbar style in MoltNet (fingerprinting 156): on macOS with "Show
  scroll bars" set to Always, and with a mouse plugged in under
  "Automatically", MoltNet pages get overlay scrollbars (a scrolling box's
  `offsetWidth - clientWidth` is 0) and switching the setting or
  unplugging the mouse doesn't relayout them; regular windows follow the
  setting. On Windows and Linux with `chrome://flags/#overlay-scrollbars`
  on, MoltNet pages keep classic 16 CSS px scrollbars. The scrollbars are
  drawn, animate and can be dragged normally in all three cases, and the
  DevTools device toolbar (mobile emulation) still shows overlay ones.
- [ ] Audio in MoltNet on real hardware (fp-media): with the output device
  set to 44.1 kHz and then to 96 kHz (Windows' "Default format", macOS's
  Audio MIDI Setup, `pw-metadata -n settings 0 clock.force-rate` or
  PulseAudio's `default-sample-rate`), and on a 48 kHz device as the
  control, a MoltNet page plays a Web Audio tone, a `<audio>` file routed
  through `createMediaElementSource()` and an `AudioWorklet` synth
  correctly: right pitch and speed, no clicks or dropouts over a few
  minutes, A/V sync of a video with Web Audio sound within what you'd
  expect from 24 ms. Report the audio service's CPU (the browser's task
  manager) while it plays, with and without the 44.1/96 kHz
  resampling. `new AudioContext().sampleRate` is 48000,
  `baseLatency` 0.01, `outputLatency` 0.024 and
  `destination.maxChannelCount` 2 in MoltNet, and the device's values in
  a regular window.
- [ ] More audio devices in MoltNet (fp-media): a 5.1/7.1 device (HDMI
  receiver) plays a 6-channel Web Audio graph as stereo and
  `destination.channelCount = 6` throws `IndexSizeError`; Bluetooth
  headphones play without dropouts; `playbackStats` (while the page is
  visible) reads 24 ms and no underruns while the CPU is loaded;
  `createScriptProcessor().bufferSize` is 2048; a page that asks for
  `sampleRate: 22050` gets it and plays at the right pitch; after granting
  microphone access, `setSinkId()` to another output device still works.
- [ ] When MoltNet's Web Audio renders, on real hardware (fp-audio). Open
  the leak kit's `fp.html` in a MoltNet window, click the page once (an
  `AudioContext` needs a gesture) and run `await mlAudioCadence()` in
  DevTools' console: `audio.cadence_interval_ms` is 10 and
  `audio.cadence_burst_frames` `[384, 512]` on every device below; in a
  regular window it follows the device (e.g. 21 and `[1024]` on a
  1024-frame device). Then, on each device, play a Web Audio tone, an
  `AudioWorklet` synth and an `<audio>` file through
  `createMediaElementSource()` for a few minutes: right pitch and speed, no
  clicks, dropouts or drift (a 440 Hz tone against a tuner stays within a
  cent), and A/V sync of a video with Web Audio sound within about 40 ms.
  Devices: the output set to 44.1 kHz and then to 96 kHz (Windows'
  "Default format", Audio MIDI Setup, `pw-metadata -n settings 0
  clock.force-rate`, PulseAudio's `default-sample-rate`), a 48 kHz device
  as the control; a 1024-frame device (PipeWire `pw-metadata -n settings
  0 clock.force-quantum 1024`, or a `latencyHint: "playback"` page on
  macOS) and a 128-frame one (`clock.force-quantum 128`); Bluetooth
  headphones. Report the renderer's CPU (the browser's task manager) with
  the tone playing in MoltNet and in a regular window.
- [ ] Device changes and no device in MoltNet (fp-audio). With a tone
  playing in a MoltNet page that logs `onerror`, `onstatechange` and
  `navigator.mediaDevices.ondevicechange`: unplug wired headphones, connect
  and then switch off Bluetooth headphones, and change the system's
  default output: the sound moves to the new device after a short gap,
  the page logs nothing and `currentTime` keeps advancing (note what a
  regular window logs, for comparison). On a machine or VM with no audio output device at all
  (a VM without a sound card; Linux with `snd_dummy` unloaded and
  PipeWire/PulseAudio stopped; Windows with every playback device
  disabled): `new AudioContext()` reads 48000, 0.01, 0.024 and 2 channels,
  `setSinkId("")` resolves, no `error` fires within a minute,
  `mlAudioCadence()` gives 10 and `[384, 512]`, and
  `navigator.mediaDevices.enumerateDevices()` lists one `audiooutput`
  (empty ID and label); in a regular window the same machine shows 44.1
  kHz (or an `error`) and no `audiooutput`. Plug a device in afterwards
  and report whether, and after how long, the MoltNet tone is heard
  without reloading (a regular window is the reference).
- [ ] Media playback on real hardware (fp-mediaclock). In a MoltNet window
  play a video with sound for a few minutes on: the output set to
  44.1 kHz and then 96 kHz, a 48 kHz device as the control, a 1024-frame
  and a 128-frame device (as in the fp-audio item above), Bluetooth
  headphones; with each, pause and play, seek, and play at 0.5x and 2x.
  Right pitch and speed, no clicks, dropouts or drift. Measure A/V sync
  with an A/V sync test clip (a flash and a beep each second), filmed with
  a phone at 120 fps or more next to the speaker: on wired outputs expect
  the sound up to about 35 ms behind the picture in the first seconds and
  within about 15 ms either way after 30 s; with Bluetooth report how far
  behind it is (a regular window is the reference, where the system
  reports the headset's latency). Report the renderer's CPU while it
  plays, in MoltNet and in a regular window.
- [ ] When MoltNet's media plays, on real hardware (fp-mediaclock). On the
  same devices, in the leak kit's `fp.html`, click the page once and run
  `await mlMediaClock()` five times: `media.clock_running` is true and
  `media.clock_start_ms` stays within a few tens of milliseconds of the
  same value on every device (report them); in a regular window note how
  it follows the device. Then a page that logs `currentTime` at every
  `requestAnimationFrame` and `requestVideoFrameCallback()`'s `mediaTime`
  and `expectedDisplayTime` for 30 s of a 30 fps video: the frame-to-
  display relation is the same on every device in MoltNet, apart from
  scheduling noise.
- [ ] Device changes and no device during playback in MoltNet
  (fp-mediaclock). With a video playing in a MoltNet page that logs
  `error`, `stalled`, `waiting` and `devicechange`: unplug wired
  headphones, switch Bluetooth headphones off, change the system's default
  output: the page logs nothing, `currentTime` keeps advancing and the
  sound comes back on the new device within about a second (a regular
  window for comparison). On a machine or VM with no output device (as in
  the fp-audio item above): the video plays at the right speed with no
  `error` and `new Audio().setSinkId("")` resolves (note what a regular
  window answers).
- [ ] Cameras and microphones in MoltNet (fp-mediaclock). On a desktop
  without a camera and a VM without a microphone, and on a laptop with
  both as the control: `enumerateDevices()` lists `audioinput`,
  `videoinput`, `audiooutput`, one each, with empty IDs, labels and
  groups, on all three. `getUserMedia({video: true})` on the machine
  without a camera shows the camera prompt; Block gives `NotAllowedError`,
  closing the prompt `NotAllowedError`, Allow `NotFoundError`; with the
  camera, `getUserMedia({video: {width: {exact: 12345}}})` prompts and
  then gives `OverconstrainedError` after Allow; the laptop behaves as in
  a regular window (prompt, then a stream). Reset the permissions between
  runs. With a page listening for `devicechange`, plug in a USB headset:
  no event before a permission, an event after microphone access is
  granted. After granting the microphone, the outputs are listed with
  labels and `setSinkId()` to another one works for a `<video>`.
- [ ] `performance.memory` in MoltNet (fp-mediaclock). On a machine with
  8 GB or more and one (or a VM) with 4 GB: `jsHeapSizeLimit` is
  2330000000 in MoltNet on both since fp-cpu (a regular window shows each
  machine's own precise value); `usedJSHeapSize` and `totalJSHeapSize` are multiples of
  100000 and don't change for 20 minutes after allocating 100 MB. On a
  cross-origin isolated page (COOP `same-origin`, COEP `require-corp`),
  `performance.measureUserAgentSpecificMemory()` in a window and in a
  shared worker reports rounded byte counts (one of the 100 sizes, or 0).
  A memory-heavy page (a large canvas game, a long spreadsheet) still runs
  on the 4 GB machine.
- [ ] EME, Media Capabilities and WebCodecs in MoltNet (fp-media): a
  Clear Key test page (e.g. the W3C EME test player) fails with
  `NotSupportedError` in MoltNet and plays in a regular window;
  `decodingInfo()` for VP9 1080p and AV1 4K reads `{supported: true,
  smooth: true, powerEfficient: false}` on a machine with hardware VP9/AV1
  decoding (regular window: power efficient); `VideoDecoder.isConfigSupported(
  {codec: "vp09.00.10.08", hardwareAcceleration: "prefer-hardware"})` is
  `false` in MoltNet and `true` in a regular window on that machine; a
  WebCodecs player (e.g. the WebCodecs samples' video decode demo) still
  plays in MoltNet, in software; on macOS a `VideoEncoder` for
  `avc1.42001E` is unsupported in MoltNet.
- [ ] MediaRecorder in MoltNet (fp-probes): on a machine with hardware
  video encoders (a Mac; Windows or Linux with a recent Intel, AMD or
  NVIDIA GPU), a MoltNet page records a 1280x720 canvas `captureStream()`
  with no `mimeType`: after `start` (or `stop`), `mimeType` is
  `video/webm;codecs=vp8`, and `video/mp4;codecs=vp9` for `video/mp4`; the
  files play back; `chrome://media-internals` shows the build's encoder
  for them, never a hardware one; `isTypeSupported()` is the same as on a
  machine without a GPU encoder. A regular window on that machine still
  uses the hardware encoder (its default `mimeType` may differ). On a
  machine with 8 or more cores, a 1080p AV1 recording
  (`video/webm;codecs=av01`) in MoltNet plays back and the encoder's
  threads (the renderer's thread list in the OS's task manager or `top -H`)
  number at most 4.
- [ ] `src: local()` (fp-probes): on each OS with a family installed that
  is not on its allow-list (Ubuntu or Cantarell on Linux, Open Sans
  installed by hand, `Segoe UI Variable` on Windows 11), `@font-face {
  src: local("<its full name>") }` and `local("<its PostScript name>")`
  fall back in MoltNet and in a regular window with Shields on (unless the
  family is in the site's sample), and load with Shields off; `local("DejaVu Sans Bold")`,
  `local("DejaVuSans-Bold")` (Linux), `local("Arial Bold")`,
  `local("Arial-BoldMT")` (Windows, macOS) load the bold face in MoltNet;
  a page that times `new FontFace("x", 'local("<installed, hidden face>")')
  .load()` against a missing name over a thousand tries finds no
  difference in MoltNet; on Windows a `local()` page in MoltNet renders
  without waiting for the font table (a fresh browser start). East Asian
  sites still render (with fallback fonts where they name local CJK
  faces).
  With the bundled font set (fonts), MoltNet windows load only the set's
  faces: there the installed families' names above fall back too, and
  `local("Tinos Bold")` and `local("NotoSans-Regular")` load.
- [ ] MoltNet's bundled font set (fonts), on macOS, Linux and (deferred)
  Windows, with the release build (the set is in `molt_fonts`):
  - A MoltNet window renders Latin pages, Wikipedia's main pages in
    Arabic, Hebrew, Hindi, Bengali, Tamil, Thai, Khmer, Georgian, Amharic,
    Japanese, Korean, Simplified and Traditional Chinese (zh.wikipedia
    with the zh-tw and zh-hk variants: Han glyphs follow the variant), a
    page of emoji (a recent Unicode version's) and a math page (MathML on
    Wikipedia) without boxes in the set's scripts, and they look alike on
    the three OSes (screenshots side by side). Note any script that shows
    boxes or reads badly, and how text looks on macOS and Windows next to
    a regular window.
  - `document.fonts.check('16px "Segoe UI"')`, `"Helvetica Neue"`,
    `"DejaVu Sans"` and `"Ubuntu"` are false in MoltNet on every OS;
    `"Arial"`, `"Arimo"`, `"Noto Sans CJK JP"` and `"Noto Color Emoji"`
    are true. `queryLocalFonts()` (after the prompt) lists the set's 51
    faces and nothing else.
  - `getComputedStyle()` of `font: menu`, `font: caption` and
    `font: status-bar` elements is the same in MoltNet on every OS.
  - After a cold start (right after a reboot), open a MoltNet window as
    soon as the browser is up: its first page already renders with the
    set (`document.fonts.check('16px Arimo')` is true in the DevTools
    console). A developer build without `molt_fonts` keeps the per-OS
    allow-list (the leak kit's `fonts.moltnet_set` fails there).
  - A page that starts a worker at once which draws text on an
    `OffscreenCanvas` and reports `measureText('Hamburg').width` every
    100 ms: in a MoltNet window opened right after a cold start the value
    settles on Arimo's width (the worker looked again once the set
    arrived) and is the same on the three OSes.
  - Metrics on the three OSes, MoltNet window, the same page: a `span`'s
    `getBoundingClientRect()` in `16px serif`, `sans-serif` and
    `monospace`, a `1ex` and a `1ch` box, `line-height: normal`, and
    canvas `measureText()` of a few strings with
    `actualBoundingBox*`/`fontBoundingBox*`: identical numbers on Linux,
    Windows and macOS (send any difference back).
  - On Linux, `python3 tools/fonts/metrics_check/run.py` (cargo, g++ and
    clang++; downloads the set and the skrifa crate the first time): "measure
    alike on Linux, Windows and macOS: yes". Rerun it after every rebase
    and font update.
  - A regular window is unchanged: the system's fonts, `system-ui`,
    emoji and per-script fallback as before.
  - Memory: with ten MoltNet tabs open, the renderers' resident memory
    grows by at most a few MB for the set (the files are mapped, shared
    through the page cache).
- [ ] The per-OS font allow-lists against fresh installs (fonts). On a
  freshly installed VM of each supported version (Windows 10 22H2,
  Windows 11 23H2 and 24H2, macOS 13, 14 and 15, Ubuntu 22.04 and 24.04,
  Fedora, Debian 12, Linux Mint, openSUSE), before installing anything
  else, copy `tools/fonts/` over and run
  `python3 molt_font_inventory.py` (Linux), `/usr/bin/perl
  collect_fonts.pl` (macOS) or `powershell -NoProfile -ExecutionPolicy
  Bypass -File collect_windows.ps1` (Windows; `collect_windows.ps1` has
  not run on a real Windows yet: report any error), then on this machine
  `python3 tools/fonts/molt_font_inventory.py import molt-fonts-raw.json`
  and `merge` the reports of one OS. *Expected:* no listed family is
  missing from any install; send the `.json` and `.md` reports back, with
  the unlisted families every install of an OS has, so the lists in
  `font_allowlists.cc` can be corrected (then rerun
  `tools/fonts/generate_tables.py --write`).
- [ ] Storage limits in MoltNet (fp-probes): on a 16 GB machine, a
  MoltNet page writes 1 MiB IndexedDB records until `QuotaExceededError`:
  it stops at 60 MiB (the site's budget, moltnet-shared); the same for
  Cache Storage (`cache.put` of 1 MiB responses) and OPFS
  (`createSyncAccessHandle().write()` in a worker); ten more sites writing
  at the same time each stop at 60 MiB too (moltnet-shared2: beyond 600 MiB
  for all sites the machine's available memory decides, and 16 GB has it).
  In a fresh MoltNet tab, a 250 MiB `Blob` built from five 50 MiB
  `Uint8Array`s can't be read (`arrayBuffer()` rejects) and a 150 MiB one
  can (moltnet-shared2: 200 MiB per renderer process). Repeat on a
  4 GB machine (or VM): the same limits while all sites together hold
  less than 600 MiB (800 MiB of blobs), and no process is killed for
  memory. An Incognito window keeps Chromium's limits (more than 200 MiB
  per site on 16 GB).
- [ ] Pixels and encoded bytes in MoltNet (fp-pixels): run the leak kit (§8) on two machines of each OS with different CPUs and GPUs: on macOS an Intel Mac and an Apple silicon Mac (and the x86-64 build under Rosetta on the Apple silicon one), on Windows and Linux an Intel and an AMD CPU with different GPU vendors. `diff` shows no difference in `pixels.*`; in a regular window the same pages may differ (hardware decoding, `pixels.video_frame_format` `NV12` or null). On a machine with hardware VP9 or AV1 decoding, `chrome://media-internals` shows a software decoder (`VpxVideoDecoder`, `Dav1dVideoDecoder`) for a MoltNet tab playing a 1080p and a 2160p YouTube video, and a hardware one for the same video in a regular window. Note the CPU use (Activity Monitor, Task Manager, `top`) and, on a laptop on battery, the power draw (`powermetrics` on macOS) of each, at 1080p30, 2160p30 and 2160p60, and whether MoltNet drops frames (Stats for nerds): FINGERPRINTING.md "MoltNet: pixels and encoded bytes" estimates 0.5 to 1 W more at 1080p and 2 to 3 W at 2160p; correct it with what you measure. A WebGL video page (e.g. a video texture demo) and a canvas video filter page still run in MoltNet.
- [ ] CPU features and V8's heap in MoltNet (fp-cpu). Run the leak kit (§8) on two x86-64 machines of different CPU generations (one without AVX2 and FMA if you have one, e.g. a pre-2013 Intel or a VM started with `-cpu Nehalem`; an Intel and an AMD one otherwise) and, on macOS, an Intel and an Apple silicon Mac: `cpu.wasm_relaxed_simd` is false, and `diff` shows no difference in `cpu.math`, `pixels.p3_readback` or `audio.decoded_vorbis`, also between the Intel and the Apple silicon Mac; in a regular window the same values may differ (FMA, AVX2, AVX-512, Arm). In MoltNet: a WebAssembly machine-learning demo that uses relaxed SIMD where it can (ONNX Runtime Web or TensorFlow.js with the WASM backend) still runs, with its SIMD build; a page with a 12-megapixel Display P3 photo (an iPhone HEIC converted to JPEG keeps its profile) shows it with the right colours, and note how much longer it takes to appear than in a regular window; a Vorbis file plays in `<audio>` and through `decodeAudioData()`. V8's heap: on a machine with 16 GB or more, a page that pushes 1 MB strings into an array until its tab runs out of memory stops at about 2 GB of JavaScript heap in MoltNet (about 4 GB in a regular window); `chrome://process-internals` or the task manager shows the renderer's memory. On Linux with more than 16 GB, `cat /proc/<renderer pid>/limits` shows a data limit of 8589934592 bytes for a MoltNet renderer (16 or 32 GiB for a regular one).
- [ ] Portable math and the memory signals in MoltNet (fp-libm). Run the leak kit (§8) on an x86-64 and an arm64 machine (an Intel and an Apple silicon Mac, or Linux on both) and on Windows: `diff` shows no difference in `cpu.math_cross_arch`, `cpu.css_math`, `cpu.dommatrix` or `audio.math`, between the architectures and between the OSes too; in a regular window the same values may differ. `cpu.css_math` holds sixteen numbers and angles, none of them `0` (an expression the browser didn't accept shows the initial value). In MoltNet: a Web Audio demo with a `ConvolverNode` reverb, an `AnalyserNode` visualiser and HRTF panning plays without glitches on the slowest machine you have (compare its CPU use in the task manager with a regular window); a CSS animation that rotates and skews (`rotate3d`, `skew`) and a canvas drawing with arcs look as in a regular window. On 64-bit Windows: a page that allocates 1 GiB `ArrayBuffer`s until a `RangeError` stops at 8 GiB in MoltNet on a machine that can commit that much (a regular window goes on to the system's commit limit). In `chrome://process-internals`, MoltNet frames of one site start sharing renderers once there are 20, whatever the RAM. A page that registers a `FinalizationRegistry` callback for 1000 objects and waits sees no callbacks when the system is short of memory (test with a memory-pressure tool, or `chrome://memory-internals` on a debug build), only after V8's own collections.
- [ ] Geometry, Skia's math and web fonts in MoltNet (fp-geometry). Run the leak kit (§8) on an x86-64 and an arm64 machine (an Intel and an Apple silicon Mac, or Linux on both), on Windows, and on an x86-64 machine without FMA if you have one (a VM started with `-cpu Nehalem`): `diff` shows no difference in `geom.hit_testing`, `geom.path_length`, `geom.shape_outside` or `geom.contrast_color`, between the architectures and between the OSes too; in a regular window the same values may differ. In MoltNet: a page with `shape-outside` around an image, a gradient and a `shape()`, a canvas drawing with arcs, rotated ellipses and dashed round-joined strokes, an SVG with `clip-path` and a `textPath`, and boxes with `corner-shape: superellipse()`, `squircle` and `scoop` look as in a regular window, and their clicks land where they look; a COLRv1 colour web font with rotated and skewed layers (Google Fonts' Nabla or a COLRv1 test font) and a variable font with VARC glyphs render as in a regular window; an HDR gain-map JPEG and an AVIF with a gain map, and an iPhone photo with an HDR headroom tag, show the same tone mapping in both. Note the time a page with 10,000 arcs and strokes on a canvas takes to draw in each (FINGERPRINTING.md "MoltNet: portable math" estimates no measurable difference; Skia's portable sine and power are about 2.5 times slower than the C library's).
- [ ] Components on a fresh profile (degoogle-kit; NETWORK_TRAFFIC.md §1, §3). Start the build with a new `--user-data-dir`, wait two minutes, open `chrome://components`. *Expected:* the security components are listed (CRLSet, PKI Metadata, Safety Tips, File Type Policies, SSL Error Assistant, Origin Trials, ...), and **none** of Indigo, Dictation's connector, History Search Strings, On-device head suggest, or Optimization Guide Geolocation Permission Predictions; after the degoogle-kit follow-up also none of Actor Safety Lists, Captcha Providers, Amount Extraction Heuristic Regexes, Commerce Heuristics, or the Optimization Guide on-device model manifest (its entry appears only once a feature asks for an on-device model, for example after requesting a model in `chrome://on-device-internals`; verify that this brings it). With `--enable-features=Dictation` the Dictation entry comes back (the overrides yield), and with `--enable-features=MoltGoogleOnlyComponents` the four Google-only components do. `chrome://version` (`molt://version`) with `?show-variations-cmd`: the testing config's `Indigo`, `Dictation`, `HistoryEmbeddingsV2Images` and `PredictionModelComponentDelivery` studies are not active, nor (after the follow-up) `OnDeviceModelBackgroundDownload` and `EnableOnDeviceWebSpeechSmallExpertModel`. With Google chosen as the search engine, typing in the address bar still shows Google's server suggestions (on-device head suggestions are off).
- [ ] Screen capture in MoltNet (fp-geometry). In a MoltNet window, a `getDisplayMedia()` demo page (e.g. WebRTC samples' getdisplaymedia) fails at once with `NotAllowedError: Permission denied by user` and no picker opens, also with "prefer current tab"; a video-call site's "share screen" button fails cleanly. `navigator.permissions.query({name: 'display-capture'})` gives `denied` in the console of the page and of a worker. In a regular and an Incognito window the same demo opens the picker and shares the chosen screen, window or tab.

### 7.4 MoltNet

The leak kit (§8) automates most of MOLTNET_COMPLIANCE.md §4 on Linux. On
macOS, and for the manual steps everywhere:

- [ ] New Identity timing (from #49): it takes about 1-2 s; a second one
  right after opens its window about 10 s later; a page that asks "Leave
  site?" closes without asking; opening a MoltNet window meanwhile is
  refused ("switching"); with DevTools open and a prompt unanswered it fails
  after 60 s.
- [ ] Circuits per identity (from #55): an IP-echo site normally shows a new
  exit after New Identity and after closing and reopening MoltNet; on Linux
  the kit's `identity.session_circuits` passes.
- [ ] External applications always ask first; MoltNet zoom starts at 100 %
  (from #49).
- [ ] Regular windows: the onion request blocker (one extra in-process hop
  per request) costs no measurable page-load time (from #6); `.onion`
  requests from a regular page, including a worker's WebSocket, are refused
  (§4 step 13, which the kit automates on Linux) (from #6, #12).
- [ ] Per-site network state (#62; MOLTNET_COMPLIANCE.md §2
  *Shared network state*, §4 steps 8 and 9): in `chrome://net-export`, two
  sites embedding the same third-party HTTPS host each get a certificate
  verification for it in a MoltNet window (one in a regular window); page
  loads in MoltNet feel no slower (a verification is a few milliseconds);
  starting with `--disable-features=` and each of
  `PartitionConnectionsByNetworkIsolationKey`,
  `SplitCacheByNetworkIsolationKey`, `ThirdPartyStoragePartitioning`
  refuses MoltNet with an error naming it (the kit's
  `identity.partitioning_override_refused` on Linux); after one site's
  subresource response set HSTS for a host, a plain-HTTP page of another
  site (opened past the HTTPS-Only warning) loading an image from that
  host's `http://` URL isn't upgraded (DevTools shows no internal 307),
  while a navigation to it still is.
- [ ] Connection and network quality (network quality; FINGERPRINTING.md
  *MoltNet: connection and network quality*): start the browser with
  `--force-effective-connection-type=Slow-2G`. A regular window reports
  `navigator.connection.effectiveType` "slow-2g" (and its `rtt` and
  `downlink` vary a little between sites); a MoltNet window and a worker
  in it report 4g, 200, 1.6 and `saveData` false on every site, and a
  `change` listener never fires, also while another window loads a large
  file. In MoltNet, an image with `loading=lazy` 2000 px below the fold
  isn't fetched until scrolled near (4g's distance is 1250 px, slow-2g's
  8000, so the regular window fetches it at once), text in a slow
  `font-display: auto` web font stays hidden until the font arrives
  rather than showing a fallback at once, and in `chrome://net-export` a
  MoltNet page with many images over HTTP/2 has as many of them in flight
  with the switch as without it (a regular window has fewer with it).
  Without the switch:
  regular windows still follow the network (e.g. DevTools' "Slow 4G"
  throttling changes `effectiveType` there, and in MoltNet too while
  DevTools emulates it).
- [ ] Captive portals and authentication (from #51; §4 step 20): no
  `generate_204` request and no sign-in tab after a certificate error; a
  `401` offering only `Negotiate` and `NTLM` shows its page, one also
  offering `Basic` prompts.
- [ ] TLS client certificates (MoltNet 156; §4 step 20): with a client
  certificate installed (on macOS in the login keychain, on Linux in
  `~/.pki/nssdb`) and `openssl s_server -accept 4433 -verify 1 -www` (asks,
  doesn't require) behind an onion service or a public test host, a
  MoltNet window loads the page with no picker and `s_server` prints no
  client certificate; reloading, and opening it from another site's page,
  shows one handshake per connection. A regular window shows the picker.
  With `-Verify 1` (requires) MoltNet shows a TLS error page. With the
  `AutoSelectCertificateForUrls` policy matching that host, MoltNet still
  sends none.
- [ ] DNS-over-HTTPS probes (MoltNet 156; §4 step 20): set secure DNS to
  Cloudflare, run `sudo tcpdump -n 'host 1.1.1.1 or host 1.0.0.1'`, open a
  MoltNet window and press *New Identity* twice: no packets (a regular
  window's network context probes as usual, once at startup).
- [ ] Device-bound sessions (MoltNet 156, Windows and macOS): sign in to
  a site that registers one (e.g. a Google account, or a test server that
  sends `Secure-Session-Registration`) in a regular window: DevTools'
  Application panel lists the session. The same in a MoltNet window: no
  session, no registration request, and the sign-in still works.
- [ ] Proxy override rules (MoltNet 156; §4 step 9): with the
  `ProxyOverrideRules` policy set (any rule), MoltNet refuses to open with
  the proxy error and the panel says the proxy is overridden; removing the
  policy and restarting lets it open.
- [ ] Network state sent to MoltNet renderers (MoltNet 156): in a MoltNet
  page, `navigator.connection` still reports 4g, 200, 1.6 (as above), and
  `navigator.onLine` turns `false` and back to `true` when the network
  cable or Wi-Fi goes away and comes back. With
  `--enable-logging=stderr --vmodule=render_thread_impl=2` (or a debugger on
  `RenderThreadImpl::OnNetworkQualityChanged`), a MoltNet renderer gets one
  network-quality message, at start-up, while a regular one gets one per
  change.
- [ ] A policy that changes while MoltNet is open (moltnet-live; §4 step
  9): with a MoltNet window open on a page that fetches a test URL every
  second (and a second MoltNet window whose page asks "Leave site?"), set
  `ProxyMode` to `direct` (Windows: registry, then `chrome://policy` →
  *Reload policies*; macOS: a configuration profile or `defaults write`
  into the managed domain, then *Reload policies*; Linux: the kit's
  `identity.policy_change_closes`, or a file in
  `/etc/chromium/policies/managed/`). Expected: both MoltNet windows
  close within a few seconds of Chromium applying the policy, a dialog
  "MoltNet windows closed" gives the proxy error, the test server sees no
  request from your own address, and opening MoltNet then fails with the
  same error; after removing the policy (and reloading policies) MoltNet
  opens. Repeat with `ProxyOverrideRules` (any rule), `WebRtcIPHandling`
  and `PartitionedBlobUrlUsage` = false. With DevTools open on a MoltNet
  page whose "Leave site?" prompt you don't answer, that window closes
  too (moltnet-policy). A change only the regular profile sees (an
  extension that sets the proxy, allowed in Incognito) leaves MoltNet
  open.
- [ ] Ending an identity with something open (moltnet-policy; §4 steps 7
  and 9), on macOS and Linux: in MoltNet windows open DevTools on a page
  (docked in one window, undocked in another), an `alert(1)`, a sign-in
  prompt (a `401` with `WWW-Authenticate: Basic`), print preview, a
  download's "Save as" dialog ("Ask where to save each file" on), an
  `<input type=file>` chooser and a page that asks "Leave site?", then
  press *New Identity*. *Expected:* every MoltNet window and DevTools
  window is gone within about two seconds (a forced close may take a
  little longer; never more than about ten), the new window opens, and a
  test page that fetched a URL every second sends nothing more once the
  old windows start closing (its last requests come from a Tor exit).
  Repeat by setting `ProxyMode` to `direct` instead of New Identity.
- [ ] Incognito's availability (moltnet-policy; §4 step 9), on Windows
  (registry), macOS (configuration profile) and Linux (the kit, or a file
  in `/etc/chromium/policies/managed/`): `IncognitoModeAvailability` = 1
  set while MoltNet is open closes it with "Incognito is turned off
  (IncognitoModeAvailability or parental controls), and MoltNet windows
  follow it", and MoltNet then refuses to open with that message; with
  `IncognitoModeUrlAllowlist` = a test site as well, MoltNet opens and only
  that site loads; = 2: MoltNet stays open and opens (regular windows
  don't); `IncognitoModeUrlBlocklist` = a test site: it doesn't load in
  MoltNet. A supervised (Family Link) profile: MoltNet doesn't open.
- [ ] Language policies (moltnet-policy; §4 step 9): with `ForcedLanguages`
  = `["ja"]` and `ReduceAcceptLanguageEnabled` = false set while MoltNet
  is open, and with `--accept-lang=fr` at launch: MoltNet stays open and its
  pages keep `navigator.languages` `["en-US", "en"]` and `Accept-Language`
  `en-US,en;q=0.9` (or `en-US` with Reduce Accept-Language on), also after
  visiting a few pages in other languages; regular windows follow the
  policy.
- [ ] Per-site connection limits (moltnet-policy; MOLTNET_COMPLIANCE.md §2
  *Shared network state*): in a MoltNet window, a test page on site A that
  opens 200 long-polling requests to 40 hosts of its own, and a page on
  site B in another tab: B loads at once (in a regular window with a proxy
  set, B waits behind A's requests). `chrome://net-internals/#sockets`
  lists MoltNet's pools as `socks5://127.0.0.1:<port> (per site)`, never
  with a site's name.
- [ ] Shared limits per site (moltnet-shared; MOLTNET_COMPLIANCE.md §2
  *Tor's capacity*, *Keepalive requests*, *Storage*, §4 step 21), with
  Tor's log at `Log info`:
  - Greedy and polite: a test page on site A opening 1000 long-polling
    requests to 200 hosts of its own, and a page of site B opened after
    it: B loads as fast as without A. A's pool in
    `chrome://net-internals/#sockets` stays at 48 (16 for WebSockets).
    Ten such greedy sites in tabs, each also holding 16 WebSockets open: a
    polite site still loads at once, and Tor's log never says "Failing
    because we have". With seventeen the budget is full: a polite site's
    new connections wait for one to be released (a long poll ending) and
    then go before the greedy sites'; idle connections of the largest
    site are closed for it.
  - Handshakes: a page that opens 100 connections to hosts needing new
    circuits (onion addresses, or ports exits rarely allow): a
    `chrome://net-export` log shows at most 16 of its `SOCKS5_CONNECT`
    events open at a time (5 for onion addresses, which count three each,
    moltnet-shared2), the others' connect jobs waiting in "resolving
    host" without a backup job; a polite site's
    page opened meanwhile loads (its first request may take a few seconds
    longer). Page loads of ordinary sites feel no slower than before.
  - Keepalive: a page on site A keeping 2048 `fetch(url, {keepalive:
    true})` requests in flight from nine pages (to a server that answers
    after a minute): A's 2049th fails, while site B's beacons in another
    MoltNet tab and a regular window's still go out (DevTools shows them
    sent).
  - Storage: two sites each write until `QuotaExceededError`: each stops
    at 60 MiB, also when the first is full before the second starts; an
    iframe of a subdomain on the first counts toward its 60 MiB. Store
    1 MiB on site C, fill nine other sites, wait 40 minutes: C's data is
    still there. In an Incognito window, Chromium's limits and eviction
    are unchanged.
  - Downloads and Background Fetch: with Tor's log at `SafeLogging 0` +
    `Log info` (or a `chrome://net-export` log), two sites whose service
    workers `backgroundFetch.fetch()` the same file of a third server: the
    two streams carry the two sites' SOCKS usernames, and the fetches
    complete (the download bubble shows them; Incognito too, without
    Tor). A frame of site T embedded on sites A and B, with a link
    `<a download>` to a file on T: clicking it in each gives streams with
    A's and B's usernames; "Save link as" and "Save image as" in a top-level
    page of A on a file of another site: A's username; "Save page as"
    (complete) of a page of A with images from a CDN: A's username for the
    images. Interrupt a large download (stop the server), resume it: the
    resumed request has the first one's username. In a regular window all
    of these behave as before (a link's file downloads with that site's
    cookies).
- [ ] Shared limits per site, continued (moltnet-shared2;
  MOLTNET_COMPLIANCE.md §2 *Tor's capacity*, *Certificate fetches*,
  *Storage*, *Blob memory*, *Renderer processes*, §4 steps 7, 9 and 21),
  with Tor's log at `Log info`:
  - Circuits: twelve greedy sites in tabs, each opening 100 connections to
    onion addresses or to ports exits rarely allow, and a polite site
    opened meanwhile: the polite site's first page starts loading within a
    handshake's time, and Tor's log never says "we already have 256
    general-purpose client circuits pending". New Identity while they
    run: the new identity's pages load as the old ones close.
  - Synchronous requests: a page whose eight workers each send synchronous
    `XMLHttpRequest`s to 100 slow hosts of its own: its pool in
    `chrome://net-internals/#sockets` holds at most 52 (its 48, and 4 past
    them once all 48 are held), and a polite site loads at once. A page
    with six large downloads in flight from one HTTP/1.1 test server (one
    that sends a byte a second), then a synchronous `XMLHttpRequest` to
    that server from the page: it completes and the page doesn't freeze,
    in a MoltNet window as in a regular one.
  - Certificate fetches: a site whose server leaves out its intermediate
    certificate (a test server that sends only its leaf): the page loads,
    and a `chrome://net-export` log shows the intermediate's fetch with a
    network key whose top-frame site is that site and a nonce of its own
    (`chrome://net-internals/#sockets` lists a pool with a limit of 4 for
    it, apart from the site's 48);
    with two sites embedding that server, each site's connections fetch it
    with that site's key.
  - Blob memory: a MoltNet tab of site A keeps 16 MiB blobs until a new
    one's `arrayBuffer()` rejects: twelve fit (192 MiB); a tab of site B
    doing the same at the same time also keeps twelve; with five such
    tabs (960 MiB together) on a 16 GB machine each still keeps twelve,
    and on a 4 GB machine (or VM) the blobs beyond 800 MiB fail once the
    machine has less than 1 GiB available, for the tab that makes them,
    and no tab crashes. A MediaRecorder recording in another tab keeps
    recording throughout. An Incognito window keeps Chromium's one limit
    (one tab holds more than 200 MiB).
  - Renderer processes: in Chrome's Task Manager, two MoltNet tabs of one
    site, and an iframe of one third party embedded in two MoltNet tabs of
    different sites, are in different processes; a popup a MoltNet page
    opens (with an opener) of the same site shares its process. A regular
    window's tabs, `chrome://` pages and the PDF viewer behave as before.
  - Ending an identity: a MoltNet page that `fetch()`es a URL every 10 ms
    and sends a `sendBeacon()` on `pagehide`, and a second page that sets
    `location` to a new URL every 50 ms, then *New Identity*: in Tor's log
    and the test server's, the old pages' requests stop at the click (none
    started after it), and no navigation of the old windows commits after
    the click; the windows still close within about two seconds.
  - Incognito's policies: `MandatoryExtensionsForIncognitoNavigation` set
    to an extension's ID while MoltNet is open closes it with "Incognito
    requires extensions (MandatoryExtensionsForIncognitoNavigation), and
    MoltNet windows run none", and MoltNet then refuses to open with it.
    A `DataControlsRules` rule blocking paste from `incognito` sources:
    text copied in a MoltNet window can't be pasted into a regular one,
    and a reporting rule's event for it names no URL; a rule on
    `incognito` destinations applies to pasting into a MoltNet page.
  - Parental controls (Windows, once a Windows build exists): turn on
    activity reporting for the Windows account (the family settings; what
    Chromium's Incognito check reads) while the browser runs: the next
    MoltNet window refuses to open with the Incognito-off message without
    a browser restart; turn it off: MoltNet opens again. Web filtering
    alone changes nothing, for Incognito as for MoltNet.
- [ ] Shared state per site, round 25 (moltnet-shared3;
  MOLTNET_COMPLIANCE.md §2 *Certificate fetches*, *HTTP cache*, *After an
  identity ends*, *Storage*, *Blob memory*, *DOM storage caches*, §4
  step 21):
  - Certificate fetches under load: twelve MoltNet tabs of sites whose
    servers each leave out their intermediate, each page opening 100
    connections at once to hosts of its own: every page loads without a
    pause of tens of seconds (a `chrome://net-export` log shows the
    intermediates' fetches starting while the budget is full), and a
    polite site opened meanwhile loads at once.
    `chrome://net-internals/#sockets` lists the fetch pools apart (limit
    4).
  - HTTP cache: site A's page loads 8 MB of images; site B's loads
    200 MB: A reloaded (DevTools *Disable cache* off) is served from the
    cache. Wait 35 minutes without touching A: A's images come from the
    network again. In an Incognito window, Chromium's one limit is
    unchanged (B's load evicts A's).
  - Ended identity: a page sending `fetch(url, {keepalive: true})` and
    `navigator.sendBeacon()` to a slow test server every 50 ms, one
    running a Background Fetch, and a download in progress: *New
    Identity*; the test server's log shows no request started after the
    click, the download shows as failed, and the Background Fetch never
    completes for the old identity.
  - Memory valve: on a 4 GB machine (or VM), six MoltNet tabs of
    different sites each fill 120 MiB of IndexedDB and 150 MiB of blobs
    while another program takes memory: the MoltNet windows close with
    "The computer was running out of memory..." before the system ends
    a process, and MoltNet opens again afterwards. On a 16 GB machine the
    same pages keep everything and nothing closes. Each tab's own writes
    fail only at its own 60 MiB / 200 MiB.
  - Background Fetch blobs: two sites' service workers each fetch a
    150 MB file with `backgroundFetch.fetch()`: both complete (each counts
    to its own site's 200 MiB, not one shared key).
  - DOM storage: site A stores `localStorage` from 20 subdomain iframes;
    site B opens 100 subdomain iframes that each store: in a
    `chrome://tracing` memory-infra dump the LocalStorage cache size
    still includes A's areas; an Incognito window's cache behaves as
    Chromium's.
- [ ] Shared state per site, round 26 (moltnet-shared4;
  MOLTNET_COMPLIANCE.md §2 *After an identity ends*, *Fail-closed*, *Blob
  memory*, *DOM storage totals*, *Downloads and Background Fetch*, §4
  steps 7 and 21):
  - Requests racing the end: a MoltNet page that starts a `fetch()`, a
    WebSocket and an image to a test server every 10 ms (keep the
    server's log with timestamps), *New Identity*: a
    `chrome://net-export` log of the session shows no request of the old
    identity's network context started after its proxy configuration
    changed to the refusal, and no SOCKS write on its sockets after it;
    the server sees none of them after that moment. Repeat ten times.
  - Background Fetch's buffer: a service worker of site A fetches a
    300 MB file with `backgroundFetch.fetch()`: it fails
    (`failureReason` `fetch-error`) once about 200 MiB arrived, and a
    `chrome://tracing` memory-infra dump shows the browser's blob memory
    back down afterwards; one of 150 MB from A and one from site B, at
    the same time, both complete. In a regular window the 300 MB fetch
    completes.
  - DOM storage totals: site A's page writes 3 MiB of `localStorage`
    from each of five subdomain iframes: three stick, the fourth and
    fifth don't (gone after a reload); site B's page still writes its
    own 3 MiB from three iframes. A tab A opened with `window.open()`
    gets a copy of A's 4 MiB of `sessionStorage`; once a page changes
    either, the copy counts, and a third `sessionStorage` area of A then
    takes about 2 MiB at most.
  - Background Fetch's credentials in a regular profile: a site sets a
    cookie, then its service worker starts `backgroundFetch.fetch(id,
    new Request(url, {credentials: 'omit'}))` to its own server: the
    server's log shows the request without a `Cookie` header; without
    `credentials: 'omit'` it has the cookie.
  - The fonts note: an installation without the bundled font set (move
    `molt_fonts/`, next to the executable or in macOS's
    `Contents/Resources/`, away; a test install only): MoltNet's panel
    shows the note that this build has no bundled fonts, and MoltNet
    windows still open; with the set back, no note.
- [ ] Shared work per site, round 27 (moltnet-shared5;
  MOLTNET_COMPLIANCE.md §2 *Downloads and Background Fetch*, *DOM storage
  caches*, *DOM storage totals*, *Storage*):
  - Background Fetch: a test server that sends a 100 MB file at 10 KB/s.
    Site A's service worker (and one of a subdomain of A) each start a
    `backgroundFetch.fetch()` of it; then site B's service worker fetches
    a small file: B's fetch completes within seconds
    (`backgroundfetchsuccess`) while A's are still downloading. In a
    regular window the same B fetch waits until one of A's finishes (or
    is aborted). `chrome://background-service-internals` (Background
    Fetch, recording on) shows each registration's "Background Fetch
    started" in that order.
  - DOM storage without a database: in a MoltNet window, a page writes a
    few `localStorage` items on 20 subdomains of one site (iframes), then
    opens 20 more subdomains that store nothing; reload the first page
    after 40 minutes: its items are all there. A `chrome://tracing`
    memory-infra dump of the session shows no `site_storage/localstorage`
    database (`/leveldb` or `/sqlite`) for the MoltNet partition (a
    regular window's partition has one).
  - The write allowance: a MoltNet page runs
    `for (let i = 0; i < 3000; ++i) localStorage.setItem('k' + i, 'v')`;
    after a second `localStorage.length` is about 1000 (a regular window:
    3000); a page of another site, loaded at the same time, writes its own
    1000. The leak kit's `dom_storage_rate` checks the same.
  - Quota checks: a page of site A embeds iframes of 500 of its own
    subdomains (a test server with wildcard DNS, reached through Tor) that
    each write 10 KB to IndexedDB; meanwhile a page of site C times
    `navigator.storage.estimate()` 100 times: the times stay as they are
    with A idle, and A's own estimate reports its 5 MB.
- [ ] Autofill in MoltNet (moltnet-live; §4 step 20): start with
  `--enable-features=AutofillServerCommunication` and Tor's log at
  `SafeLogging 0` + `Log info`. A MoltNet page with an address form and a
  sign-in form, filled and submitted: no stream to
  `content-autofill.googleapis.com` in Tor's log (a regular window
  queries it). Fields are still classified
  (the Autofill dropdown offers a saved address), and saved addresses and
  passwords fill. With a saved address whose state doesn't match the
  page's `<select>` options word for word, no lookup or stream of
  `chromium-i18n.appspot.com`; a page's Payment Request with
  `requestShipping` shows the sheet, keeps the chosen address as entered,
  and its address editor asks for the region as text.
- [ ] The manual parts of MOLTNET_COMPLIANCE.md §4: step 2 (the toolbar
  label), 9 (a proxy set by an extension), 11 (the Onion-Location bar), 14
  (bridges with the distribution's obfs4 and snowflake, from #6), 16 (split
  mode, shortcuts, toolbar, PDF viewer), 17 (a prepared profile), 19
  (Incognito's protections, from #23) and the manual bullets of 20.

### 7.5 OCR and voice helpers (helpers-win)

Every image tesseract OCRs is now decoded by Chromium's data decoder and
handed over as a PNM the browser writes; whisper-cli reads a WAV the browser
writes; on Windows both run as a less-privileged AppContainer per run
(`helper_sandbox_win_policy.h`). On each OS (Windows: deferred):

- [ ] OCR of attachments (helpers-win): attach a PNG, a JPEG and a WebP with
  text in the AI chat, one with a transparent background, a phone photo
  (12 MP or more) and a scan of more than 33.5 MP (which the decoder
  halves on each side first). *Expected:* the text, as before. A TIFF renamed to
  `.png` gets "TIFF images can't be read for OCR: save the image as PNG or
  JPEG and attach that"; a GIF renamed to `.png`, "this file isn't a PNG,
  JPEG or WebP image". While it runs, the scratch directory
  (`molt_ocr_*` in the temp directory) holds `input.pnm`, never the file.
- [ ] OCR of a scanned PDF (helpers-win): `/pdf` on a scanned PDF of more
  than 20 pages. *Expected:* `--- Page N ---` headers, the "OCR read 20 of
  N pages" line, and the browser's task manager shows a data decoder
  process while it runs.
- [ ] Voice (helpers-win): the mic button transcribes a few sentences.
  *Expected:* the scratch directory (`molt_voice_*`) holds the 44-byte
  header WAV the browser wrote. `molt_ai_unittests
  --gtest_filter='RecordedWav*:OcrImage*:OcrImageFile*:HelperScratchDir*'`
  passes here too.
- [ ] Windows, the installer's grant (helpers-win, deferred): after
  installing, `icacls "%ProgramFiles%\MoltBrowser\ocr"` and `...\whisper`
  list `*S-1-15-3-1024-3499754957-3417772683-1255750666-3341737774-2438675945-3280382303-1781483004-2555713060:(OI)(CI)(RX)`
  (the capability `lpacMoltBrowserHelperFiles`), and no other folder of
  the install does.
- [ ] Windows, the sandbox's own tests (helpers-win, deferred):
  `molt_ai_unittests --gtest_filter='HelperSandboxWin*:HelperScratchDir*'`
  with `molt_helper_sandbox_test_helper.exe` next to it. *Expected:* all
  pass: the helper runs and its output is captured; it reads its scratch
  directory and its own folder but not another file of the user's; it
  writes only its scratch directory; a loopback connection is refused and
  never reaches the test's listener; it can't start `whoami.exe` or
  itself; 5 GiB is refused (the job's 4 GiB); its environment is its own;
  the deadline kills it; from a folder without the grant it doesn't start
  and the error names the installer.
- [ ] Windows, OCR and voice in the AppContainer (helpers-win, deferred):
  the two checks above with the installed build. While tesseract runs,
  Process Explorer shows it with integrity "AppContainer", in a job, with
  the capability above and no `internetClient`. If OCR or voice fails with
  "couldn't start in its sandbox" and `0xC0000142`, a helper's DLL loads
  user32 and its LPAC token can't open the desktop: report it (giving the
  helper an alternate desktop, as Chromium's broker does, is not done).
- [ ] Windows, the portable ZIP (helpers-win, deferred): unpack it and
  attach an image. *Expected:* "tesseract sandbox unavailable: on Windows,
  OCR and voice need MoltBrowser's installer, …" and no tesseract process;
  after `icacls <unpacked>\ocr /grant "*S-1-15-3-1024-3499754957-…:(OI)(CI)(RX)"`
  OCR works.

## 8. Leak kit and traffic capture

### 8.1 The MoltNet leak kit (Linux)

The first run on a MoltBrowser build is also the kit's own first real test
(from #29; docs/LEAK_TESTING.md). It needs a Linux machine with sudo,
kernel 5.13 or later with cgroup v2, nftables 1.0 or later, `tor`,
`tcpdump`, `iproute2`, `util-linux`, `openssl`, optionally `xvfb`, and a
reachable Tor network. Nobody has tried it in Docker Desktop's VM on the
Mac; use a real Linux host.

- [ ] The kit's own tests (from #29):
  `python3 -I -m unittest discover -s tools/leak_test/tests -t tools/leak_test`.
- [ ] The full run against the build, then the installed `.deb` under
  another locale and time zone (from #29, #49, #51, #55):
  ```bash
  sudo tools/leak_test/leaktest.sh run --browser chromium/src/out/MoltBrowser-linux/chrome --out ~/molt-v0.3.0/leak-a
  sudo tools/leak_test/leaktest.sh run --browser /opt/moltbrowser/moltbrowser --out ~/molt-v0.3.0/leak-b \
       --locale de_DE.UTF-8 --timezone Asia/Tokyo
  tools/leak_test/leaktest.sh diff ~/molt-v0.3.0/leak-a/*/report.json ~/molt-v0.3.0/leak-b/*/report.json
  ```
- [ ] What passing looks like (from #29 and the PRs named below):
  - `verdict: PASS (0 FAIL, 0 ERROR, ...)`, not INCOMPLETE;
  - the harness checks pass: the firewall held, only Tor reached the
    network, a MoltNet window opened through `chrome://molt-net/`, the
    seeded settings control and the test extension control pass;
  - the identity checks pass, including `identity.session_circuits` (from
    #55), `identity.sandboxed_circuits` (from #49) and
    `identity.cert_fetch_circuits` with `cert_fetch_b` (from #51),
    `identity.partitioning_override_refused` (#62); New
    Identity's order (a beforeunload page, an immediate second New
    Identity) (from #49);
  - the uniformity checks pass, including the scrollbar and media-query
    exactness (from #33), `text.metrics_whole` (from #37),
    `nav.connection`, `worker.connection` and `nav.connection_changes`
    (network quality), and `scrollbar.overlay`, the colour media queries
    (`mq.color_gamut_*`, `mq.dynamic_range_high`,
    `mq.video_dynamic_range_high`, `mq.color_bits`, `mq.monochrome`),
    `storage.quota_over_usage` and `audio.render_quantum_hardware`
    (fingerprinting 156), and the `audio.*`, `eme.*`, `mc.*` and
    `webcodecs.*` checks (fp-media), `audio.cadence_interval_ms`,
    `audio.cadence_burst_frames` and `media.audiooutput` (fp-audio), the
    `media.clock_running`, `media.sink_default`, `media.devices`,
    `media.device_order`, `media.device_blank`, `media.gum`,
    `media.gum_overconstrained`, `perf.jsHeapSizeLimit`,
    `perf.heap_rounded` and `perf.heap_cached` checks (fp-mediaclock; the
    `media.gum*` probes leave a prompt open for 1.5 s in a frame they then
    remove, and `media.clock_start_ms` is recorded only: report its spread
    between machines); `diff`
    shows no difference in a
    promised value, nor in the codec support it compares (`mc.opus`,
    `mc.h264`, `mc.hevc`, `media.can_play`, `webcodecs.decoder_software`,
    `webcodecs.encoder_*`) between two machines of an OS.
  - What pages can learn about the processor and the microphone
    (fp-probes2): `uniform.cpu.wasm_simd` passes (a plain SIMD module
    validates), `uniform.cpu.float16_nan_canonical` passes on x86-64 (INFO
    on arm64, where the optimized code keeps payload bits: compare
    `cpu.float16_nan` between two arm64 runs), `harness.mic_processing`
    passes and so do `uniform.audio.mic_processed_stable` and
    `uniform.audio.mic_settings`. Run the kit on an x86-64 machine without
    AVX2 (or a VM started with `-cpu Nehalem`), a recent x86-64 machine and
    an arm64 one, then `diff` each pair: no difference in `pixels.jxl`,
    `audio.stretched` or `audio.mic_processed` (and in `cpu.float16_nan`
    between machines of one architecture; `diff` shows the architectures
    and lists a Float16 difference between them as information). First-run
    questions, report them with the values: `pixels.jxl` reading
    `unsupported` (the build has no JPEG XL), `audio.stretched` reading
    `silent`, `short` or `dropout` (the element didn't play into Web Audio
    in time), `audio.mic_processed` reading `silent` (the fake device
    couldn't read the WAV: is the audio service sandboxed?), `gap` (a
    dropped buffer) or a hash with `audio.mic_processed_stable` false (the
    two captures started at different points of the file). With
    `--target regular` the same values differ between processors with and
    without AVX2 (a check of the probes themselves).
  - The microphone's platform processing and playback (fp-mic2):
    `uniform.audio.mic_capabilities`, `uniform.audio.mic_system_aec`,
    `uniform.audio.mic_system_aec_settings`,
    `uniform.audio.mic_remote_only` and
    `uniform.audio.mic_remote_only_settings` pass, and `diff` between the
    three machines above shows no difference in `audio.mic_system_aec` or
    `audio.mic_remote_only` (both are also checked against
    `audio.mic_processed` in each run). `audio.mic_playback` (the default
    echo cancellation while the page plays two tones) is recorded only:
    report its value from two runs on one machine and from the three
    machines (it is expected to change from run to run; a value equal to
    `audio.mic_processed` would mean the echo canceller heard no
    reference), and `audio.mic_playback_tones` (both contexts `running`,
    48000 and 44100). First-run questions: `audio.mic_remote_only` reading
    `silent`, `short` or `gap`, or an error from `getUserMedia()` (send the
    value), and `uniform.audio.mic_remote_only` failing with a hash (the
    renderer's processing gave other samples than the audio service's for
    the same input: send both hashes and `chrome://media-internals`' log of
    the two streams).
  - Subnormal floats and the microphone's format and names (fp-mic3):
    `uniform.audio.mic_format`, `uniform.audio.mic_format_ranges`,
    `uniform.audio.mic_device_ranges` and `uniform.audio.mic_names` pass
    (48 kHz, 16 bits, one channel, 10 ms; one to two channels; generic
    labels, every device a group of its own, no `communications` device),
    and `diff` between the three machines above, and between OSes, shows
    no difference in `audio.subnormal_graph` or
    `audio.subnormal_resampled`. First-run questions: an
    `audio.mic_device_ranges` that is empty or lists more than one set
    (send the value and `enumerateDevices()`' entries), and an
    `audio.subnormal_resampled` with `frames` other than 9600 (the WAV
    wasn't decoded at 44.1 kHz). With `--target regular` the subnormal
    probes may differ between x86-64 and arm64, and the microphone's
    format follows the fake device (44.1 kHz).
  - The camera (fp-media4): `uniform.video.camera_settings`,
    `uniform.video.camera_capabilities`, `uniform.video.camera_frame`,
    `uniform.video.camera_hd_settings`, `uniform.video.camera_hd_frame`,
    `uniform.video.camera_device_capabilities` and
    `uniform.video.camera_overconstrained` pass (the fake device's camera
    as 640x480 and 1280x720 at 30 fps, I420 frames of exactly the track's
    size, `OverconstrainedError` for a width above 1280), and `diff`
    between the three machines above, and between OSes, shows no
    difference in them. First-run questions: a `video.camera_frame` that
    reads `absent` or `done` (no frame within 5 s: send
    `chrome://media-internals`' log of the camera), a
    `video.camera_settings` with a `frameRate` of 20 (the fake device's
    own rate reached `getSettings()`), and a `video.camera_frame` whose
    `codedWidth` is 1280 (the device's frame handed on). With `--target
    regular` the camera shows the fake device's own modes (3840x2160 at
    20 fps) and the larger width is granted. A real camera on each OS is
    §5.2's fp-media4 check.
  - `identity.background_fetch_circuits` passes (moltnet-shared): each
    Background Fetch test's request went to Tor with its page's site's
    SOCKS username. First-run question: whether the fetch starts at all in
    the kit (a SKIP says none reached Tor: look at the `bgfetch_a`/`_b`
    page results; `started: false` with a permission error, or `done:
    false`, means the download service didn't run it, for example because
    it saw no network in the kit's namespace).
  - `identity.nothing_held_after_end` passes (moltnet-shared3): the old
    identity's `fetchLater()` and `pagehide` beacon never reached the
    kit's server. First-run question: whether the evidence says
    `fetch_later: true` (if not, `fetchLater()` isn't exposed in the
    page and only the beacon was checked).
  - `dom_storage_rate` passes (moltnet-shared5): of 3000 `localStorage`
    writes the burst (1000) is kept and no more than what refilled while
    the page wrote; `regular_dom_storage_rate` keeps all 3000. First-run
    question: the evidence's `writes.seconds` (how long the loop took; if
    it is more than a few seconds the margin may be too small).
  - `identity.nothing_after_end_race` passes (moltnet-shared4): the old
    page saw one of its racing requests refused after New Identity began,
    then started a fetch, a keepalive fetch, a beacon, an image, a
    WebSocket and a prefetch, and none reached the kit's server.
    First-run questions: whether the evidence lists all six kinds as
    started (a kind with an error name after it threw in the page), and
    how many milliseconds after New Identity began the page saw the
    refusal (a SKIP saying the page closed before it saw one means the
    pages stopped before the network context's refusal reached the
    page's requests; run it again, and if it stays, look at the order of
    `EndIdentityAndCloseWindows()`).
  - `identity.tor_capacity` passes (moltnet-shared): the torrc the browser
    started Tor with states `ConnLimit 1000` and `MaxClientCircuitsPending
    256` (256 since moltnet-shared2). A SKIP saying the torrc wasn't found means the kit's Tor wrapper
    didn't record Tor's command line (`logs/tor-argv.txt`).
  - `recorder.mime_type`, `fonts.local_hidden` and
    `storage.enforced_quota` (60 MiB since moltnet-shared) pass
    (fp-probes), and `diff` shows no
    difference in `recorder.supported` or `fonts.local_listed` between
    two machines of an OS.
  - `fonts.visible`, `fonts.moltnet_set` and `fonts.generic_map` pass
    (fonts: only the bundled set's names are visible, all of them, and
    each generic keyword measures like its family), and `diff` shows no
    difference in `fonts.fallback_widths` or `fonts.local_bundled`
    between two machines, also between a Linux and a macOS run. Compare
    `text.metrics`, `text.emoji` and `text.dom_width` of the Linux and
    macOS runs too and send any difference back.
  - `pixels.video_frame_format` passes (`I420`: `<video>` decoded in
    software, its frames in CPU memory), and `diff` shows no difference in
    `pixels.webcodecs_frame`, `pixels.video_frame`,
    `pixels.encoded_vp8_*` or `pixels.encoded_opus_*` between two machines
    of an OS (fp-pixels). Every `pixels.*` value is a hash, not an error.
  - `cpu.wasm_relaxed_simd` passes (false: relaxed SIMD fails validation),
    and `diff` shows no difference in `cpu.math`, `pixels.p3_readback` or
    `audio.decoded_vorbis` between two machines of different CPU
    generations or architectures (fp-cpu). `pixels.p3_readback` and
    `audio.decoded_vorbis` are hashes, not errors (`audio.decoded_vorbis`
    has `frames` above 10000); `cpu.wasm_simd` is recorded only.
  - `nav.deviceMemory` passes (4, fp-libm), and `diff` shows no
    difference in `cpu.math_cross_arch`, `cpu.css_math`, `cpu.dommatrix`
    or `audio.math` between two machines of different architectures or
    OSes (fp-libm). They are hashes and lists, not errors; `audio.math`'s
    `reduction` is a negative number.
  - `diff` shows no difference in `geom.hit_testing`, `geom.path_length`,
    `geom.shape_outside` or `geom.contrast_color` between two machines of
    different architectures, CPU generations or OSes (fp-geometry). The
    first three are hashes, not errors; `geom.contrast_color` is 87 letters
    `b` and `w` (or `absent` on a build without `contrast-color()`).
  - `display_capture` passes (fp-geometry): `getDisplayMedia()`, with audio and with `preferCurrentTab`, rejects with `NotAllowedError` "Permission denied by user" within 2 s, and `display-capture` reads `denied` in the page and in a worker. A timeout there means a picker opened: check the `media_capture_devices_dispatcher.cc` and `user_media_processor.cc` hooks and that the renderer got `--molt-net-renderer`.
  - `net.client_cert.handshakes` passes (MoltNet 156): the server that
    asks for a client certificate saw only completed handshakes, none with
    a certificate. A FAIL listing a dropped handshake means MoltNet's
    context still asked the browser (check the `network_context.cc` hook
    and `molt_send_no_client_certificates` in the context's params).
  - `identity.policy_change_closes` and `net.autofill_server` pass
    (moltnet-live, phase `policy_live`): the MoltNet page closed within
    seconds of the kit's `ProxyMode` policy, its canary went only through
    Tor, MoltNet refused to open with the policy and opened without it; no
    connection to `content-autofill.googleapis.com` with
    `AutofillServerCommunication` on. A FAIL with the canary looked up
    outside Tor means the policy's proxy reached MoltNet's network context
    (check the `proxy_config_monitor.cc` hooks); pages still open after 60
    s means the settings watch didn't fire (check
    `MoltNetLiveGuard::Watch()` in `OnTorReady()`). First-run question:
    whether the "MoltNet windows closed" dialog over the
    `chrome://molt-net/` tab gets in the way of DevTools-pipe calls (if so,
    note it; the checks only need the targets list).
  - `identity.language_policy_ignored`, `identity.incognito_off_closes`
    and `identity.incognito_forced_allows` pass (moltnet-policy, phases
    `policy_live` and `policy_live_incognito`). An ERROR in the first that
    says the regular page didn't change either means the policy didn't
    arrive (or `ReduceAcceptLanguage` didn't take effect): look at
    `chrome://policy` in that launch. A FAIL in the second with a
    `devtools://` page left means the DevTools window survived (check
    `DetachDevTools()` in `molt_net_window.cc`); with the page left, the
    forced close didn't run. First-run questions: whether
    `--auto-open-devtools-for-tabs` opens DevTools undocked for the MoltNet
    tab with the kit's seeded dock state (`devtools_own_window` in the
    phase data), and whether the "Leave site?" prompt is reported
    (`dialog`); the check doesn't depend on either.
- [ ] First-run questions (from #29): DevTools can drive
  `chrome://molt-net/`; the browser accepts the kit's wrapper as
  `--moltnet-tor-binary`; whether `--display headless` works with MoltNet
  windows.
- [ ] Triage every "unexplained" host (expect one round, from #29):
  background traffic of the regular profile goes into
  `tools/leak_test/leakkit/background.py` and docs/NETWORK_TRAFFIC.md with
  the reason, then `tools/leak_test/leaktest.sh report <run dir>`; anything
  else is a leak.
- [ ] Send back both `report.json` files with the build's version (from #29).

### 8.2 The full traffic capture (docs/NETWORK_TRAFFIC.md "How we'll verify")

- [ ] On macOS arm64 (Linux has §8.3 for the hosts; run this there only for
  request bodies): the packet capture, NetLog and optional mitmproxy run in
  that section, on a fresh profile, without
  `--disable-field-trial-config`. *Expected:* every **Likely** and
  **Capture** row of its tables confirmed; no Google Translate, TranslateKit
  or component download caused by a page (from #26, #34, #48); no host
  that NETWORK_TRAFFIC.md §6 marks `forbidden` (degoogle-kit).

### 8.3 The regular-window traffic capture (Linux) (degoogle-kit)

The leak kit's `traffic` command (docs/LEAK_TESTING.md "Regular-window
traffic capture"). Same machine and packages as §8.1, without `tor`; it
needs no internet: nothing it does leaves the namespace, and the kit's
sinkhole answers every request with a 503.

- [ ] The kit's own tests, as root this time, so that the namespace test
  runs too (it is skipped in CI):
  `sudo python3 -I -m unittest discover -s tools/leak_test/tests -t tools/leak_test -v`.
  *Expected:* all pass, `SinkholeInANamespace.test_capture_pipeline`
  among them (not skipped).
- [ ] `tools/leak_test/leaktest.sh hosts`. *Expected:*
  `docs/network_traffic_hosts.json matches docs/NETWORK_TRAFFIC.md`.
- [ ] The capture against the build, then against the installed `.deb`:
  ```bash
  sudo tools/leak_test/leaktest.sh traffic --browser chromium/src/out/MoltBrowser-linux/chrome --out ~/molt-v0.3.0/traffic-a
  sudo tools/leak_test/leaktest.sh traffic --browser /opt/moltbrowser/moltbrowser --out ~/molt-v0.3.0/traffic-b
  ```
  About 17 minutes each (`--idle 600` is the 10-minute idle step).
- [ ] What passing looks like: `verdict: PASS (0 FAIL, 0 ERROR, ...)`; the
  harness checks pass, `traffic.harness.update_check` among them (it
  proves the sinkhole saw MoltBrowser's update check to `api.github.com`);
  `traffic.expected` lists at least `update.googleapis.com` (POST
  `/service/update2/json`, about a minute after each launch),
  `clients2.google.com` (GET `/time/1/current`, at the first run),
  `safebrowsing.googleapis.com` (GET `/v5/hashLists:batchGet`),
  `sb-ssl.google.com` during `download`, `api.github.com`, and the
  MoltShield list hosts; on Linux `redirector.gvt1.com` during or after
  `spellcheck`. `traffic.components` passes and lists the component IDs
  of each update check: none from NETWORK_TRAFFIC.md §6's component table
  (no actor safety lists, CAPTCHA providers, amount extraction regexes or
  commerce heuristics, no on-device model manifest). If it is a SKIP, the
  sinkhole couldn't read the IDs: send the `update.googleapis.com` lines
  of `logs/sinkhole.jsonl` back with the report.
- [ ] First-run questions: whether the first run opens a tab of its own
  (the `first_run` step's `tabs` in report.json); whether the `cert_error`
  step's expired certificate made the browser ask for network time
  (`clients2.google.com` during `cert_error`); whether the `dns_error`
  step's page showed `ERR_NAME_NOT_RESOLVED` (its step note); whether the
  download step's `.exe` triggered the Safe Browsing check at all.
- [ ] Triage every FAIL: the evidence gives the time, the step, the host,
  the TLS server name, the request line and a likely source. A host
  MoltBrowser needs goes into NETWORK_TRAFFIC.md (its section and the §6
  table) with the reason, then `tools/leak_test/leaktest.sh hosts --write`
  and `tools/leak_test/leaktest.sh report <run dir>`; anything else is
  traffic to turn off (`molt_feature_overrides.cc` or a pref default) and
  a reason to run the capture again.
- [ ] Send back both runs' `report.json` (and `report.md`) with the build's
  version, and the list of NETWORK_TRAFFIC.md rows the capture confirmed,
  so that their confidence can become **Captured**.

## 9. Vulkan: the hardware matrix before the switch can go on

Not needed for v0.3.0: `molt_llama_vulkan` stays off. This is the list from
docs/design/SANDBOXED_INFERENCE.md §11.9 (from #52, #55).

- [ ] Build with the switch in its own out dir on Linux (from #52) (verify
  the command):
  ```bash
  mkdir -p build/cache
  cp configs/linux-x64.gn build/cache/linux-x64-vulkan.gn
  echo 'molt_llama_vulkan = true' >> build/cache/linux-x64-vulkan.gn
  ./scripts/configure.sh --config "$PWD/build/cache/linux-x64-vulkan.gn" --build-dir out/MoltVulkan
  ./scripts/build.sh --build-dir out/MoltVulkan --target "chrome molt_ai_unittests"
  ```
  (`build.sh` passes `--target` to `autoninja` unquoted, so two targets
  work. In Docker, the `--config` path is `/moltbrowser/build/cache/...`.)
  Expect about +2.6 MB of shader data plus ggml-vulkan's code (it was
  +42 MB before the shaders were cut, SANDBOXED_INFERENCE.md §11.14; record
  the `chrome` binary's size with and without the switch) and a host build
  of glslang and SPIRV-Tools. Then `gn check` on the real tree, and
  `molt_ai_unittests` (`GpuOffloadTest`, `GpuFallbackGuardTest`,
  `InferenceProcessHostTest`, `MoltGpuInferenceSandboxPolicyTest`, and
  `VulkanShaderPackTest` from ai-finish).
- [ ] The cut shaders change nothing (from ai-finish; §11.14): on each GPU
  below, a Q4_K_M catalogue model and gpt-oss (MXFP4) give the same first
  answer on the GPU as on the CPU for three prompts at temperature 0 (the
  first tokens at least; a later tie may part them); the load log shows
  `gpu_status` on and `gpu_layers` > 0; no "a shader variant this build
  left out was requested" in the service's log.
- [ ] On each GPU, in the real sandbox (from #52, #55). The load log line
  shows `gpu_status` and `gpu_layers`.

  | | NVIDIA (proprietary) | AMD (Mesa RADV) | Intel (Mesa ANV) |
  |---|---|---|---|
  | device found and created with exactly the broker's file list | [ ] | [ ] | [ ] |
  | a model runs; nothing the driver needs is refused (watch for driver warnings) | [ ] | [ ] | [ ] |
  | speed against the CPU; load time (no pipeline cache) | [ ] | [ ] | [ ] |
  | a real device loss (GPU reset) and a driver crash fall back to the CPU (`GpuFallbackGuard`) | [ ] | [ ] | [ ] |
  | integrated GPU: does the footprint watchdog see device memory? | n/a | [ ] | [ ] |

- [ ] A GPU behind a PCI bridge, and a machine with two GPUs, each find
  theirs (from #55).
- [ ] Grant more broker files only by name, never a directory recursively
  (from #55).
- [ ] Windows, once a runner exists again (from ai-finish; the test plan of
  SANDBOXED_INFERENCE.md §11.15; only a Windows machine with a GPU can
  answer these). Build with `molt_llama_vulkan = true` in its own out dir
  (clang-cl), run with `--enable-logging=stderr`, and keep Process Monitor
  and Process Explorer on the inference process:

  | | NVIDIA | AMD | Intel |
  |---|---|---|---|
  | the warm-up line: `instance 1, GPUs n, devices n, pipelines n, modules pinned m, not pinned 0` | [ ] | [ ] | [ ] |
  | before lockdown the loader reads the driver's registry keys and DriverStore files (Process Monitor: no ACCESS DENIED from `vulkan-1.dll` or the ICD) | [ ] | [ ] | [ ] |
  | after `LowerToken()` no image load is refused (no `STATUS_INVALID_IMAGE_HASH`), and ggml-vulkan's own instance lists the GPU | [ ] | [ ] | [ ] |
  | the model loads with `gpu_status` on and `gpu_layers` > 0, and answers; speed against the CPU | [ ] | [ ] | [ ] |
  | which modules were pinned (Process Explorer): the loader, the driver, its shader compiler; none loaded later | [ ] | [ ] | [ ] |
  | a TDR (`dxcap -forcetdr`) mid-answer: the GPU record, the model again on the CPU | [ ] | [ ] | [ ] |

  Also: a laptop with two GPUs (both listed, the discrete one used); a
  machine without a Vulkan driver (a warning, the model on the CPU, no
  crash); an overlay that registers an implicit Vulkan layer (Steam, OBS,
  RivaTuner) installed: its DLL is not in the inference process; without
  `--molt-inference-gpu` (the CPU path) no `vulkan-1.dll` in the process.
- [ ] Before turning it on: a lasting per-driver GPU record and a user
  setting, a pipeline cache (from #52, §11.9), and the shader cut
  (ai-finish, §11.14) are built; the hardware rows above decide.

## 10. Packaging, signing and the draft release

macOS and Linux in either order (each has its own out dir). The private
key never leaves the offline machine; the full runbook is
docs/SIGNED_UPDATES.md "Every release".

### 10.1 Before the tag

- [ ] The bundled font set (fonts): `scripts/bundle-fonts.sh --dest
  /tmp/molt_fonts` downloads 47 fonts and 6 licences (about 48 MiB, once;
  later runs use `build/cache/fonts`) and `--verify` passes;
  `python3 -I tools/fonts/fonts_test.py` and `python3 -I
  tools/fonts/generate_tables.py` pass. The packages carry the set:
  `MoltBrowser.app/Contents/Resources/molt_fonts/` (and `codesign --verify
  --deep --strict` still passes on the app, notarization accepts it),
  `/opt/moltbrowser/molt_fonts/` in the .deb and .rpm, the AppImage's and
  the Flatpak's app directory; `release.sh` fails if it is missing. Note
  the installers' sizes against v0.2.5's (about 19 to 27 MiB more).

- [ ] Every fix from §3-§8 is merged, and the build at that commit passed
  §5 and §7. The pins, the key and the version (§2) are in (from #43:
  releases build from the tag's commit with a clean tree).
- [ ] `CHANGELOG.md`: `## [Unreleased]` becomes the dated v0.3.0 heading;
  README's download table and version text (from CHANGELOG.md).
- [ ] The installer names the updater looks for (from
  `src/chrome/browser/molt_ai/update/update_manager.cc`, `release.yml`,
  `release-linux.yml`):
  `MoltBrowser-macOS-arm64.dmg`, `MoltBrowser-Linux-x64.deb`,
  `MoltBrowser-Linux-x64.rpm`, `MoltBrowser-Linux-x64.tar.gz`
  (`UpdateManager::PlatformAssetName()`; the README's `/latest/download/`
  links use them too). Every release script and workflow now produces
  exactly these names, from `scripts/lib-release-names.sh`, so nothing is
  renamed by hand; CI checks them against the updater
  (`scripts/release_asset_names_test.py`, in `lint.yml`).
- [ ] Decide which machine builds what. **Pushing a `v*` tag starts two
  workflows on self-hosted runners**, if they are online: `release.yml`
  (macOS job on `[self-hosted, macOS, ARM64]`) and `release-linux.yml`
  (`[self-hosted, linux-chromium-builder]`). Pick one route per platform:
  two builds would upload the same names with `--clobber`, and the last
  one wins.
  - macOS, route A: the tag push runs `release.yml`'s macOS job. It runs
    the scripts of §10.2: `configure.sh`, `build.sh` (compile, then Tor,
    whisper and tesseract, failing if one can't be bundled), and
    `release.sh` (Info.plist version, `default.metallib`, inside-out
    signing including the framework's `Libraries/` and the `Resources/`
    helpers, the notarized and stapled `MoltBrowser-macOS-arm64.dmg`), then
    the SBOM, checksums and provenance. Its build number is the commit
    count up to the tag. It has not run end to end yet, so watch the run.
    Before the tag, on the runner: its workspace
    (`<runner dir>/_work/MoltBrowser/MoltBrowser/`) needs `depot_tools/` and a
    `chromium/` checkout at the pin (`./scripts/setup.sh --target-os mac`
    there once; a second full checkout on the Mac, §0), `MOLT_BUILDER_ID`
    in the runner's `.env` (§2.5), the four secrets in the `release.yml`
    header, the Metal toolchain, `cmake` and Homebrew's `tesseract` (§0).
    Then the §7.1 codesign checks on the draft's DMG.
  - macOS, route B: §10.2 on your own checkout. Stop the Mac runner before
    pushing the tag, or cancel that run
    (`gh run list --workflow release.yml --repo OneManNoCode/MoltBrowser`,
    then `gh run cancel <id>`).
  - Linux: §10.3. `release-linux.yml` is how v0.2.5's Linux packages were
    made.
- [ ] Tag the merged commit; `scripts/release-draft.sh` refuses a tag that
  isn't pushed (from #43):
  ```bash
  git switch main && git pull --ff-only
  git tag -a v0.3.0 -m "MoltBrowser v0.3.0" && git push origin v0.3.0
  ```

### 10.2 macOS (local scripts, route B)

- [ ] Build from the tag with a clean tree; the provenance refuses anything
  else (from #43):
  ```bash
  git switch --detach v0.3.0 && git status --short          # prints nothing
  export MOLT_RELEASE_BUILD=1                                # bundles must match their pins
  ./scripts/setup.sh                                         # overlay of the tag; checks the pin
  export MOLT_BUILD_STARTED_ON="$(./scripts/release-provenance.sh --start --version 0.3.0)"
  ./scripts/configure.sh --platform macos --arch arm64
  ./scripts/build.sh 2>&1 | tee ~/molt-v0.3.0/release-build-mac.log
  grep -nE 'error:|FAILED|finished with an error|bundling failed' ~/molt-v0.3.0/release-build-mac.log
  ```
  *Expected:* no match. `build.sh` bundles Tor, whisper and tesseract into
  the app after the compile; with `MOLT_RELEASE_BUILD=1` it fails when one
  can't be bundled (a development build only warns).
- [ ] Notarization credentials, once per machine (from `release.yml`):
  `xcrun notarytool store-credentials AC_PASSWORD --apple-id <Apple ID> --team-id 83DA5U8N7Z --password <app-specific password>`.
- [ ] Sign, notarize and make the DMG (from CLAUDE.md "Packaging gotchas",
  `scripts/release.sh`): Info.plist version, `default.metallib`, inside-out
  signing with per-helper entitlements, the `Resources/` helpers (any
  failure stops it) and `Libraries/` dylibs, then
  `dist/MoltBrowser-macOS-arm64.dmg`, signed, notarized and stapled. It
  refuses an app without its bundled tor, whisper-cli or tesseract, and
  `--notarize` without `--identity` and `--team-id`.
  ```bash
  ./scripts/release.sh --version 0.3.0 --build "$(git rev-list --count HEAD)" \
    --identity "Developer ID Application: GenEye AI Labs Inc. (83DA5U8N7Z)" \
    --team-id 83DA5U8N7Z --notarize --skip-build
  ```
  The build number is the commit count up to the tag, as in `release.yml`;
  check that it is above v0.2.5's
  (`defaults read /Applications/MoltBrowser.app/Contents/Info.plist CFBundleVersion`).
- [ ] The §7.1 codesign and notarization checks pass on this app and DMG
  (from CLAUDE.md).
- [ ] SBOM, checksums, provenance and upload, as `release.yml` does them
  (from #25, #43):
  ```bash
  APP=chromium/src/out/MoltBrowser/MoltBrowser.app
  ./scripts/release-sbom.sh --platform macos --arch arm64 --version 0.3.0 \
    --payload "$APP" dist/MoltBrowser-macOS-arm64.dmg
  ./scripts/release-checksums.sh dist/SHA256SUMS-macos-arm64 \
    dist/MoltBrowser-macOS-arm64.dmg dist/MoltBrowser-macOS-arm64-dmg.spdx.json
  ./scripts/release-provenance.sh --platform macos --arch arm64 --version 0.3.0 \
    --payload "$APP" --out-dir dist \
    dist/MoltBrowser-macOS-arm64.dmg dist/MoltBrowser-macOS-arm64-dmg.spdx.json dist/SHA256SUMS-macos-arm64
  ./scripts/release-draft.sh v0.3.0
  gh release upload v0.3.0 dist/MoltBrowser-macOS-arm64.dmg dist/MoltBrowser-macOS-arm64-dmg.spdx.json \
    dist/SHA256SUMS-macos-arm64 dist/molt-provenance-macos-arm64.json --clobber
  ```
  *Expected:* the SBOM step accepts the build in release mode (it runs
  `licenses.py` on the 156 checkout for the first time, from #25); the
  provenance statement has the toolchain versions (clang, rustc, Xcode and
  the macOS SDK) and the depot_tools commit, which no real host has
  produced yet (from #43); and the draft holds those four files.

### 10.3 Linux

- [ ] Route A, as for v0.2.5 (from #25, #43, `release-linux.yml`): the tag
  push runs `release-linux.yml` on the `linux-chromium-builder` runner (its
  `.env` needs `MOLT_BUILDER_ID`). Or start it by hand:
  `gh workflow run release-linux.yml --ref v0.3.0 -f tag=v0.3.0 -f jobs=4 --repo OneManNoCode/MoltBrowser`.
  *Expected:* the draft gains `MoltBrowser-Linux-x64.{deb,rpm,tar.gz}`,
  their three `*.spdx.json`, `SHA256SUMS-linux-x64`,
  `molt-provenance-linux-x64.json` and the unsigned
  `MoltBrowser-Linux-repository.tar`. The runner's first build after the
  move to `out/MoltBrowser-linux` is a full one (its warm build is in
  `out/MoltBrowser`, which can then be deleted).
- [ ] Route B, Docker on the Mac (slow; it builds in `out/MoltBrowser-linux`,
  next to the macOS build). `build-linux.sh` builds (syncing the Linux
  toolchains if needed); `scripts/release-linux.sh` packages in the same
  container and does what the workflow does after that, with the
  container's toolchain and image in the provenance (from #25, #43)
  (verify the command):
  ```bash
  git switch --detach v0.3.0 && git status --short
  export MOLT_RELEASE_BUILD=1
  export MOLT_BUILD_STARTED_ON="$(./scripts/release-provenance.sh --start --version 0.3.0)"
  ./build/docker/build-linux.sh 2>&1 | tee ~/molt-v0.3.0/release-build-linux.log
  grep -nE 'error:|FAILED|finished with an error' ~/molt-v0.3.0/release-build-linux.log
  ./scripts/release-linux.sh --version 0.3.0 --build 1 --docker --skip-build --upload
  ```
  *Expected:* no match from the `grep`; the same assets on the draft as
  route A (plus `MoltBrowser-Linux-x64.AppImage` and its SBOM if the image
  has `appimagetool`). The packages are written as
  `dist/MoltBrowser-Linux-x64.*`, the names the updater downloads, and the
  Flatpak manifest goes to `dist/flatpak/`, so the tree stays clean for the
  provenance.
- [ ] The §7.2 package check on the uploaded `.deb`; the leak kit (§8) on
  the installed package (from #25, #29).

### 10.4 Sign offline and publish

Follow docs/SIGNED_UPDATES.md "Every release", steps 2-9 (from #25, #43):

- [ ] Step 2, on the online machine: download the draft's assets
  (from SIGNED_UPDATES.md):
  `gh release download v0.3.0 --repo OneManNoCode/MoltBrowser --dir release-v0.3.0`,
  and copy the folder to the offline machine (step 3).
- [ ] Step 4, offline: every SBOM describes its installer (from #25):
  `python3 tools/sbom/molt_sbom.py validate --artifacts-dir release-v0.3.0 release-v0.3.0/*.spdx.json`.
- [ ] Step 5, offline: sign the Linux repository with the archive key
  (`tools/linux_repo/molt_linux_repo.py sign --bundle ... --key ...`, as in
  SIGNED_UPDATES.md), or skip it while there is no archive key (from #25; §11).
- [ ] Step 6, offline: write and sign the manifest and the provenance, one
  passphrase prompt (from #43):
  ```bash
  python3 tools/release_signing/molt_release_sign.py manifest --tag v0.3.0 release-v0.3.0/*
  python3 tools/release_signing/molt_release_sign.py sign \
    --key /Volumes/MOLTKEY/molt-update-signing.key --assets-dir release-v0.3.0
  ```
  *Expected:* `sign` lists every asset and, per build, its commit, Chromium
  156.0.8078.74, builder label and build times, and refuses unless the
  macOS and Linux statements cover every installer, SBOM and checksum file
  with the manifest's hashes and agree on the commit and Chromium.
- [ ] Step 7: copy back `molt-release-manifest.json`, `.sig`,
  `molt-build-provenance.intoto.jsonl` (and the signed repository bundle)
  (from SIGNED_UPDATES.md).
- [ ] Step 8, online: publish (from #25, #43):
  ```bash
  scripts/linux-repo-publish.sh --bundle MoltBrowser-Linux-repository.tar --tag v0.3.0   # only with an archive key
  scripts/publish-release-signature.sh v0.3.0 molt-release-manifest.json \
    molt-release-manifest.json.sig molt-build-provenance.intoto.jsonl
  gh release edit v0.3.0 --repo OneManNoCode/MoltBrowser --draft=false
  ```
  `publish-release-signature.sh` refuses unless the signature verifies
  against the keys in the checkout, the assets are exactly the signed ones,
  and the provenance covers every build output at the tag's commit.
- [ ] Step 9: the "Verify release signature" and "Deploy website to GitHub
  Pages" runs are green (from SIGNED_UPDATES.md).
- [ ] After publishing: a v0.2.5 install updates to v0.3.0 (the old updater
  checks no signature but needs the stable asset names); a clean install of
  each package starts (from SIGNED_UPDATES.md "Transition note").

## 11. The owner's open decisions

- [ ] **Safe Browsing.** The memo recommends option D for v0.3.0: default it
  off and say so in Settings and the privacy policy; not implemented yet
  (it's a pref default in `molt_privacy_prefs.cc` plus copy, so it needs a
  build). If D: on the build machine, confirm Safety Tips and lookalike
  warnings still show (from docs/SAFE_BROWSING_DECISION.md, ROADMAP.md).
  The memo's "What changed at Chromium 156" has the current facts: list
  updates on the V5 protocol, the real-time lookups' new relay host, and
  what each option sends and touches at 156 (degoogle-kit). The §8.3
  capture shows the Safe Browsing requests a build really makes.
- [ ] **`.onion` as a secure context**, in MoltNet windows only: needs a
  browser-wide trust change plus mixed-content patches; until then
  `http://` onion sites show "Not secure" and some features stay off. Ship
  v0.3.0 with the documented gap? (from #6; MOLTNET_COMPLIANCE.md §5.7)
- [ ] **Incognito policies in MoltNet.** Turning Incognito off (also for
  a supervised user) and the Incognito URL lists now cover MoltNet, and
  forcing Incognito no longer stops MoltNet (moltnet-policy;
  MOLTNET_COMPLIANCE.md §2 *Incognito's availability*). Still open:
  extensions required for Incognito and the "Incognito" conditions of
  data-control rules don't cover MoltNet windows. Apply them too, or keep
  them documented? (from #23; MOLTNET_COMPLIANCE.md §5.7)
- [ ] **Third-party audit and bug bounty**: "before 1.0" on the roadmap;
  MOLTNET_COMPLIANCE.md §3 lists the audit as not done. Choose an auditor
  and scope (MoltNet first?) and a bounty platform (from ROADMAP.md).
- [ ] **Windows signing**: "Windows signing is next" (ROADMAP.md "Signed
  builds everywhere"), and the Windows runner itself is paused. Pick a
  code-signing route and a non-work runner before a Windows v0.3 (from
  ROADMAP.md, CLAUDE.md).
- [ ] **Dependabot PR #42** (`actions/setup-java` 6.0.1 → 6.1.0, used only
  by `build-matrix.yml`): mergeable and clean; merge or close (from #42).
- [ ] **The Linux archive key**: create it now, or ship v0.3.0 without the
  apt/rpm repository (from #25). Without it, should the unsigned
  `MoltBrowser-Linux-repository.tar` stay in the release (the signed
  manifest would list it), or be removed from the draft before signing
  (`gh release delete-asset v0.3.0 MoltBrowser-Linux-repository.tar`)?
- [ ] **`AboutFlagsHistogramTest`**: add the `molt-*` flags to `enums.xml`,
  or accept the failure (from #39; §5.3).
- [ ] **Which models ship pinned**: the embedding and vision models (§2.4),
  and whether to pin the chat models now. The vision model's 4 GB RAM figure
  is a placeholder to replace with a measurement (from #38).
- [ ] **A bundled model in the DMG.** v0.2.x shipped TinyLlama inside the
  DMG, but `BrowserAIRuntime` never reads `Contents/Resources/molt_models/`
  (`scripts/package-dmg.sh`), and `release.sh` doesn't add it. Drop it (and
  the README's "bundles an on-device model"), or wire it up and pin it
  (from `scripts/package-dmg.sh`; a bundled model must match its pin, #43).
- [ ] **The Vulkan switch** stays off for v0.3.0 (§9) (from #52).
- [ ] **Still open in docs/design/SANDBOXED_INFERENCE.md §7**: repack versus
  memory limits (item 4), pre-warming after idle unload (5), keeping prompts
  out of crash dumps (9), whether the agent and automation reload a model
  after an idle unload (12), the macOS `sysctl` denial noise (14) (from #15).
- [ ] **Who builds the macOS release** (§10.1): `release.yml`'s macOS job
  now runs the same scripts as §10.2 (the metallib, helper bundling,
  `Libraries/` signing and DMG name it lacked when this runbook was
  written are fixed), but it has never run end to end. Route A (the runner,
  watched) or route B (by hand, runner stopped) for v0.3.0?
