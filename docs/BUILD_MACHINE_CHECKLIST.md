# Build machine checklist: from a fresh sync to a signed v0.3.0 draft

One ordered runbook for everything that needs the build machine before
v0.3.0 ships: the compile, the unit tests, the fuzzers, the device and
manual checks, the MoltNet leak kit, packaging and the offline signing.

**Why it exists.** The last release is v0.2.5 (macOS arm64 and Linux x64;
Windows is still at v0.2.1). Since then the October v0.3 work landed: the
workstream merges and pull requests #2 to #55, including the rebase onto
Chromium 152 (#11), and then the rebase onto Chromium 156.0.8078.48 (#64),
when Chrome's Extended Stable channel moved to M156. **None of it has been
compiled against Chromium 156.**
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
  *Expected:* `chromium/src @ 156.0.8078.48 (d0c02bfad08ec92a6ffcf989e53cd995e6381a4c)`,
  then `gclient sync`, hooks and `Overlay applied.` It resets `chromium/src`
  (`reset --hard`, `clean -fd`; the gitignored `out/` survives), so the
  first run from an older pin means a full rebuild.
- [ ] The checkout is at the pin (from #11, `chromium_version.txt`):
  ```bash
  [ "$(git -C chromium/src rev-parse HEAD)" = "$(tools/chromium/pin.sh revision)" ] && echo pin-ok
  ```
- [ ] The overlay and the patch series agree (CI runs the same check):
  `python3 tools/chromium/verify_patches.py` → `OK: patches/ reproduces the
  overlay exactly`. The build input is still the `src/` overlay copy;
  `patches/` holds the same changes as diffs (from REBASING.md).
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
  At 156 expect to regenerate (from #64): our copy was last refreshed from
  the 148-era subproject, and 156 pins `search_engines_data` at
  `5dc9f1afd38a8ac2b26b1972eb4fc1bbd5754e8c` (check `git -C
  chromium/src/third_party/search_engines_data/resources rev-parse HEAD`),
  which this repository's cloud tools couldn't reach.
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
- [ ] Optional: the chat models, `python3 tools/models/pin_models.py` (dry
  run, then `--write`); hidden entries (`qwen3.5-4b`, `gemma4-e4b`) stay
  hidden. Set `HF_TOKEN` for gated repositories (from
  `tools/models/pin_models.py`).
- [ ] `python3 tools/models/pin_models_test.py` passes (fails on a
  half-filled pin) and the `TODO(pin)` comments are gone (from #28, #38).
  *Recorded in* `src/chrome/browser/molt_ai/runtime/browser_ai_runtime.cc`
  (`kModelRegistry`, `kVisionProjectors`) and
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
#15), the helper job object and deadline (from #16), PDFium under win32k
lockdown (from #18), the Vulkan pre-lockdown step (from #52), Windows
signing (§11). Once a runner exists again: give it `MOLT_BUILDER_ID` in its
`.env`, wipe its workspace once (the workflow's steps were reordered), and
dispatch it from the tag with `--ref vX.Y.Z -f tag=vX.Y.Z` (from #43).

## 4. Compile-risk hot spots

When the build fails, find the file here; the PR tag says which change to
read. Upstream files are whole copies under `src/`, each hook marked
`// MoltBrowser:`; `patches/<path>.patch` shows exactly what changed.

- [ ] Optional, on any Linux machine before the slow build:
  `tools/compile_check/compile_check.py all` type-checks MoltBrowser's own
  C++ and the hook files below against the real 156 headers with
  Chromium's clang (no checkout needed; see its README for what it can't
  cover). *Expected:* `report` shows no errors (at 156, from #64: 518 of
  558 files checked, 38 partly on grit stand-ins, 0 errors).

### 4.1 Mojo: new fields, unions and interfaces

| File | What to watch | From |
|---|---|---|
| `services/network/public/mojom/network_context.mojom` | `[MinVersion=1] bool molt_basic_and_digest_auth_only` (`HttpAuthStaticNetworkContextParams`), `[MinVersion=1] string? molt_socks_isolation_password` and the four `[MinVersion=1] bool molt_uncached_cert_verification`, `molt_no_shared_cache_entries`, `molt_hsts_top_level_navigations_only`, `molt_no_network_quality_estimator` (`NetworkContextParams`): last in their structs, so positional constructors still compile | #51, #55, #62, network quality |
| `services/cert_verifier/public/mojom/cert_verifier_service_factory.mojom` | `[MinVersion=1] bool molt_isolate_each_cert_fetch` | #51 |
| `third_party/blink/public/mojom/worker/shared_worker_info.mojom`, `.../service_worker/service_worker.mojom` | `[MinVersion=1] bool molt_shields_down`; `ServiceWorker.InitializeGlobalScope` change | #24 |
| `chrome/services/molt_ai/inference/public/mojom/molt_inference.mojom` | the generated union API (`GenerationParams.constraint`), `LoadEmbeddingModel`/`EmbeddingModel.Embed`, the projector handle, GPU fields | #15, #28, #30, #38, #52 |
| `chrome/services/molt_ai/document_parser/public/mojom/document_parser.mojom` | module constants, `kServiceSandbox` | #13, #18 |
| `chrome/common/molt_ai/fingerprinting/molt_fingerprinting.mojom` | the farbling seed over mojo | #10 |

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
| `molt_ai/common/helper_sandbox_*` (Landlock and seccomp, Seatbelt, job object) | | #16 |
| `third_party/llama_cpp:mtmd` (exception and CFI configs), `:llama_guard` (a TU built with exceptions), `:constrained_decoding` deps | | #30, #38, #52 |
| `runtime/model_image_prep.cc` (real Skia, the data decoder), `molt_model_impl.cc`, `molt_inference_service.cc` (syntax-checked only) | | #38 |
| `embedding_process_host`, `runtime:embedding_host`, `models:download` | | #28 |
| `inference_process_host.cc` and its tests | | #15, #52, #53 |

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

### 4.8 WebUI handlers and the agents

| File | Risk | From |
|---|---|---|
| `ui/webui/molt_ai/molt_ai_chat_handler.cc` | not type-checked at all (too many dependencies); the download section, `ChooseChatWindow` with `BrowserWindowInterface`, `SidePanelUI::Show`, `HasTransientUserActivation` | #36, #40, #54 |
| `molt_ai_chat_handler_actions.cc`, `molt_ai_chat_form_fill.*`, `molt_ai_memory_ui.cc`, `molt_ai_settings_ui.cc` (`BoolArg`, `BoundedStringArg`), `molt_ai_automation_ui.cc` (`HasCallbackArgs`) | | #28, #32, #36, #40, #54 |
| `agents/web_agent.cc`, `agent_actions.cc` (new `agent_actions` → `constrained_decoding` dependency and DEPS rule), `agent_screenshot.cc` (`CopyFromSurface` 4-arg form, observer overrides), `agent_vision.*`, `agents:agent_plan` (`-Wswitch` for `kReplan`) | | #35, #39, #45, #46, #47, #50 |
| `automation/automation_ai_steps.cc`, `automation_runner.cc`, `agent_run_draft.*`, the new automation GN targets | | #35, #54 |
| `chrome/browser/about_flags.cc`, `flag-metadata.json`, `flag-never-expire-list.json` | `#molt-agent-screenshots`, `#molt-agent-site-tools`, `#molt-agent-planner` | #39, #45, #47 |
| `models/model_download_job.cc` (`CancelAndDiscard`), `models:download_plan` | | #40 |
| `tor/molt_net_window.cc`, `tor/tor_control_connection.cc` (`poll()` on POSIX) | | #49 |
| `update/update_manager.cc`, `vault/vault_store.cc` (`base::saturated_cast`) | | #41 |
| every fuzzer target's `BUILD.gn`, `seed_corpus` and `dict` paths, DEPS and visibility | build with the real `//base`, `//url`, `//crypto`, BoringSSL, llama.cpp's grammar API | #41 and later |

### 4.9 GN targets and DEPS edges

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
  New edges the PRs flagged: memory → `runtime:embedding_host`,
  `models:download`, the inference mojom (from #28); `:constrained_decoding`
  (from #30); `agent_actions` → `constrained_decoding` (from #45); the
  content utility directory → `//sandbox/linux:sandbox_services`, the
  browser → `public:buildflags` and `//content/public/common` (from #52);
  `universal_web_contents_observers.cc` and `:letterbox_view` (from #27).
  New at MoltNet 156: `chrome_content_browser_client.cc` includes
  `molt_net_requests_policy.h`, `molt_net_network_context_policy.h` and
  `molt_net_profile_id.h` (the `:requests_policy`,
  `:network_context_policy` and `:tor` targets), and the two
  `content/browser/network/` observers include
  `molt_uniform_network_state.h`, which no target lists (see §4.2).
- [ ] DEPS include rules (checkdeps), for the new `DEPS` files under
  `chrome/{browser,renderer,services}/molt_ai/` and Blink's
  `core/molt_fingerprinting` (from #10, #19, #37, #41) (verify the command):
  ```bash
  cd chromium/src
  for d in chrome/browser/molt_ai chrome/services/molt_ai chrome/common/molt_ai \
           chrome/renderer/molt_ai third_party/blink/renderer/core/molt_fingerprinting chrome/utility; do
    python3 buildtools/checkdeps/checkdeps.py "$d" || echo "CHECKDEPS FAILED: $d"
  done
  ```

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
| `chrome/browser/webshare/mac/sharing_service_operation.mm` | file shares refused after the random delay | #23, rebase 156 (now on `IsPrimaryOTRProfileWithRegularParent()`) |
| `chrome/browser/gcm/gcm_profile_service_factory.cc`, `.../instance_id/instance_id_profile_service_factory.cc` | no GCM for MoltNet | #23 |
| `chrome/browser/media/chrome_media_session_client.cc` | `ShouldHideMetadata` | #23, rebase 156 (as above) |
| `chrome/browser/net/net_error_diagnostics_dialog_win.cc` | no Network Diagnostics | #23, rebase 156 (as above) |
| `chrome/browser/app_controller_mac.mm` | `OpenStartupTabsInBrowserWithProfile` skips MoltNet | #23 |
| `chrome/browser/picture_in_picture/auto_picture_in_picture_tab_helper.cc` | "ask" counts as "block" | #23, rebase 156 (as above) |
| `chrome/browser/media/webrtc/webrtc_logging_controller.cc` | `IsWebRtcTextLogAllowed` false for MoltNet | rebase 156 |
| `chrome/browser/reduce_accept_language/reduce_accept_language_factory.cc` | `ReduceAcceptLanguageService` gets `is_incognito` for MoltNet | rebase 156 |
| `chrome/browser/molt_ai/tor/molt_net_pref_isolation_policy.h` (MoltBrowser's own) | three more pinned prefs; `molt_net_pref_isolation_unittest.cc` now needs `//components/autofill/core/common` and `//components/universal_optout` | rebase 156 |

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
  | `MoltNetProxyPolicyTest` (new), and the new cases of `MoltNetNetworkContextPolicyTest` (client certificates, DoH probes, `UniformNetworkStateForMoltNetOnly`) and `MoltNetRequestsPolicyTest` (`client_certificates`) | MoltNet 156 |
  | `GpuOffloadTest`, `GpuFallbackGuardTest`, `StreamValidatorTest`, `MoltGpuInferenceSandboxPolicyTest` (Linux) | #52, #53, #55 |
  | `AgentRunDraftTest`, `AutomationSecurityTest` | #54 |
  | `MoltOSCryptTest`, `MoltKeysStoreTest`, `VaultStoreTest` (the os_crypt_async move) | #11 |
  | `PromptUtilsTest`, `GemmaThoughtFilterTest`, `ThinkTagFilterTest` | #7, #8 |
  | `SanitizeHelperTextTest`, `Utf8Test` (OCR and voice output) | #14 |

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
| `content_unittests` | `ServiceWorker*:SharedWorker*` (the service and shared worker fakes) | #24 |
| `unit_tests` | `WebRtcLoggingControllerTest.*:ReduceAcceptLanguageServiceTest.*` | rebase 156 |
| `net_unittests` | `SSLClientSocketTest.*:*ClientAuth*:*ClientCert*:HttpNetworkTransactionTest.*` | MoltNet 156 |
| `services_unittests` | `NetworkContextTest.*Doh*:NetworkContextTest.*ClientCert*` (inside the `NetworkContextTest.*` row too) | MoltNet 156 |
| `content_unittests` | `NetworkQualityObserverImplTest.*` | MoltNet 156 |
| `unit_tests` | `*SSLClientCertificateSelector*:*ClientCertificate*` | MoltNet 156 |

- [ ] Build and run each row above (from the PRs in the table):
  ```bash
  cd chromium/src
  autoninja -C out/MoltTests services_unittests net_unittests components_unittests unit_tests \
    blink_unittests blink_platform_unittests content_unittests
  out/MoltTests/services_unittests --gtest_filter='NetworkContextTest.*:...'   # one run per row
  ```
  *Expected:* as in §5.3.

### 5.3 Known-expected failures, and what to do

| Test | Why it fails | What to do |
|---|---|---|
| `TranslateManagerTest.*`, `TranslateLanguageListTest.*` (components_unittests), `TranslateServiceTest.*`, `TranslateBubbleViewTest.*` (unit_tests), Translate `browser_tests` | They assume Google's path: the translate script fetch, the language-list fetcher, the API-key and offline gates, partial translate, `hrefTranslate` auto-translate, the Google wordmark (docs/design/PAGE_TRANSLATION.md §9) | Expected (from #48). Go through each failing case: it is fine only if it asserts one of those behaviours. A crash, or a failure about prefs, language detection or the bubble's states, is a bug. List the expected failures here, under this row, so the next rebase compares against them. |
| `AboutFlagsHistogramTest.CheckHistograms` | The `molt-*` flags aren't in `LoginCustomFlags` in `enums.xml` (from #39) | Either accept it (owner's call, §11), or in `chromium/src` run `python3 tools/metrics/histograms/generate_flag_enums.py out/MoltTests`, which runs the test and adds the entries, then import the changed upstream file into `src/` (an unchanged import commit first, then the edit) (verify the command). |
| `NetworkContextTest`, `CertNetFetcherURLLoaderTest`, `CertVerifierServiceFactoryTest`, `SOCKSConnectJobTest` | Should not fail: the new fields default to off, and contexts without a password keep the fixed one | A failure here is a bug in #51 or #55. |
| `AboutFlagsTest.*` | Should pass (metadata and never-expire entries were added) | Fix `flag-metadata.json` if not. |

### 5.4 The DCHECK pass

- [ ] Run the `out/MoltTests` browser through the MoltNet basics: open and
  close MoltNet windows, New Identity, an extension allowed in Incognito,
  the ⋮ menu and a video's controls (Cast), then quit (from #21, #23).
  *Expected:* no `DCHECK` or `CHECK` failure (`MediaRouterFactory` CHECKs
  `MediaRouterEnabled()`).

## 6. Fuzzers

Two passes: the GN targets against the real libraries, and the standalone
tier CI doesn't run. The GN build uses Chromium's own clang, so it runs on
the Mac or on Linux (a third out dir); the standalone tier needs an LLVM
clang with libFuzzer (Apple's has none), so it is easiest on Linux
(docs/FUZZING.md).

- [ ] libFuzzer build of every target `tools/fuzz/molt_fuzz.py list` names
  (34 at #55; from #41 and the PRs that added fuzzers since: #44, #45, #47,
  #48, #49, #53, #54) (verify the command):
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
- [ ] The manual parts of MOLTNET_COMPLIANCE.md §4: step 2 (the toolbar
  label), 9 (a proxy set by an extension), 11 (the Onion-Location bar), 14
  (bridges with the distribution's obfs4 and snowflake, from #6), 16 (split
  mode, shortcuts, toolbar, PDF viewer), 17 (a prepared profile), 19
  (Incognito's protections, from #23) and the manual bullets of 20.

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
    `webcodecs.*` checks (fp-media); `diff` shows no difference in a
    promised value, nor in the codec support it compares (`mc.opus`,
    `mc.h264`, `mc.hevc`, `media.can_play`, `webcodecs.decoder_software`,
    `webcodecs.encoder_*`) between two machines of an OS.
  - `net.client_cert.handshakes` passes (MoltNet 156): the server that
    asks for a client certificate saw only completed handshakes, none with
    a certificate. A FAIL listing a dropped handshake means MoltNet's
    context still asked the browser (check the `network_context.cc` hook
    and `molt_send_no_client_certificates` in the context's params).
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

- [ ] On macOS arm64 first, then Linux: the packet capture, NetLog and
  optional mitmproxy run in that section, on a fresh profile, without
  `--disable-field-trial-config`. *Expected:* every **Likely** and
  **Capture** row of its tables confirmed; no Google Translate, TranslateKit
  or component download caused by a page (from #26, #34, #48).

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
  Expect about +42 MB of binary and a host build of glslang and
  SPIRV-Tools. Then `gn check` on the real tree, and `molt_ai_unittests`
  (`GpuOffloadTest`, `GpuFallbackGuardTest`, `InferenceProcessHostTest`,
  `MoltGpuInferenceSandboxPolicyTest`).
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
- [ ] Deferred: Windows needs the pre-lockdown step of §11.4 first (from #52).
- [ ] Before turning it on: a lasting per-driver GPU record and a user
  setting, a pipeline cache, and the binary-size options (from #52, §11.9).

## 10. Packaging, signing and the draft release

macOS and Linux in either order (each has its own out dir). The private
key never leaves the offline machine; the full runbook is
docs/SIGNED_UPDATES.md "Every release".

### 10.1 Before the tag

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
  156.0.8078.48, builder label and build times, and refuses unless the
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
- [ ] **`.onion` as a secure context**, in MoltNet windows only: needs a
  browser-wide trust change plus mixed-content patches; until then
  `http://` onion sites show "Not secure" and some features stay off. Ship
  v0.3.0 with the documented gap? (from #6; MOLTNET_COMPLIANCE.md §5.7)
- [ ] **Incognito policies in MoltNet.** Turning Incognito off, the
  Incognito-only URL blocklist, extensions required for Incognito, and
  closing a supervised user's Incognito windows don't cover MoltNet
  windows (forcing Incognito does stop MoltNet). Apply them to MoltNet, or
  document? (from #23; MOLTNET_COMPLIANCE.md §5.7)
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
