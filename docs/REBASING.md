# Rebasing MoltBrowser onto a new Chromium

MoltBrowser is a Chromium fork. Every Chromium security fix reaches our users
only after we move our base revision forward and ship a build, so rebasing is a
routine, scheduled job with a deadline, not a project. This page is the
process and the tooling for it, plus the first dry run.

## What is pinned, and where

| What | Where |
|---|---|
| The Chromium revision we build on | `chromium_version.txt` (`CHROMIUM_VERSION`, `CHROMIUM_REVISION`). The only copy: `scripts/setup.sh`, `build/docker/*`, every workflow and `tools/chromium/*` read it. Shell code uses `tools/chromium/pin.sh`. |
| MoltBrowser's own code | `src/` files that do not exist upstream (3824 today: `chrome/browser/molt_ai/`, `ui/webui/molt_ai/`, toolbar `molt_*`, vendored llama.cpp, the anydoc Rust crates, ...) |
| Our changes to upstream files | `src/` still holds whole copies (the build does `cp -R src/. chromium/src/`). `patches/` holds the same changes as one diff per file against the pin, generated from `src/`. |

`patches/` layout:

- `patches/<path>.patch`: git-format diff of `chromium/src/<path>` from
  upstream@pin to ours. The header and `index` line record the upstream base.
  Applies with `git apply -p1` from `chromium/src`.
- `patches/series`: the patches, in order.
- `patches/identical.txt`: `src/` files that are byte-identical to upstream at
  the pin (49 today, all `.grdp` string files). They change nothing now, but
  after a rebase a stale copy silently reverts upstream's changes. Drop them
  from `src/` when convenient; the rebase tool refreshes them meanwhile.
- `patches/whole_files.txt`: upstream files that live in a DEPS subproject (a
  separate repo the mirror cannot diff). Today only
  `third_party/search_engines_data/resources/definitions/prepopulated_engines.json`.
  Check it by hand on every rebase.

Until the build switches to the patch series, `src/` is the source of truth:
edit `src/`, then run `tools/chromium/overlay_to_patches.py` and
`tools/chromium/verify_patches.py`, and commit both.

## Tools (`tools/chromium/`)

All of them work without a Chromium checkout. Upstream content comes from a
bare, shallow, blob-less clone cached in `.molt-chromium-cache/` (gitignored):
one ~20 MB tree fetch per revision, then only the files needed, in one batch.
The default remote is the GitHub mirror (`https://github.com/chromium/chromium`);
on the build machine you can use `--remote https://chromium.googlesource.com/chromium/src.git`
or set `MOLT_CHROMIUM_REMOTE`.

| Command | Does |
|---|---|
| `tools/chromium/pin.sh [revision\|version\|env]` | Prints the validated pin. |
| `tools/chromium/overlay_to_patches.py` | Regenerates `patches/` from `src/` (deterministic). |
| `tools/chromium/verify_patches.py` | Applies the series to upstream@pin in a scratch tree and fails unless every result equals `src/` byte for byte, `identical.txt` is still identical, and every upstream-owned `src/` file is accounted for. |
| `tools/chromium/rebase.py TAG` | Dry run: 3-way merges every patched file (base = upstream@pin, ours = `src/`, theirs = upstream@TAG) into `.molt-chromium-cache/rebase/TAG/merged/` and writes `report.md` / `report.json`. Never touches `src/`. |
| `tools/chromium/rebase.py TAG --apply [--allow-conflicts]` | Writes the merges (with conflict markers if allowed) into `src/` and moves `chromium_version.txt` to TAG. |
| `tools/chromium/apply_patches.py [--reset] [--check]` / `scripts/setup.sh --patches` | Experimental patch-based build path: applies the series to `chromium/src` and copies only first-party files. |

## Which Chromium to track

**Policy: track Chrome Extended Stable, and ship every security respin of it
within 7 days of Google's release.** Extended Stable gets security fixes
backported for longer, so between milestone moves a respin is usually a
same-branch update with few or no conflicts, which a one-person team can turn
around inside the 7-day window. Moving the milestone (the bigger rebase) then
happens when Extended Stable moves, not every time Stable does.

Finding the tags (chromiumdash is the authority; the GitHub mirror works when
it is unreachable):

```bash
git ls-remote --tags https://github.com/chromium/chromium 'refs/tags/15*' \
  | sed 's#.*refs/tags/##; s#\^{}##' | sort -u \
  | grep -E '^[0-9]+\.0\.[0-9]+\.[0-9]+$' | sort -t. -k1,1n -k3,3n -k4,4n | tail
```

A release branch is the `MAJOR.0.BRANCH.*` series with many patch numbers;
`MAJOR.0.NNNN.0`–`.4` tags are trunk/canary builds. The pin before the 152
rebase (`148.0.7729.0`, a trunk commit from 2026-03-10) was a canary-era
build, not a release branch: it had none of M148's branch security merges.

Which line is Extended Stable right now: Google's Version History API
answers without chromiumdash, and is the quickest check (no `extended` rows
on Linux, which has no such channel):

```bash
for p in win mac; do
  curl -sS "https://versionhistory.googleapis.com/v1/chrome/platforms/$p/channels/extended/versions/all/releases?order_by=starttime%20desc&pageSize=6" \
    | python3 -c 'import json,sys; [print(r["version"], r["serving"]["startTime"][:10], r.get("fraction")) for r in json.load(sys.stdin)["releases"]]'
done
```

State on 2026-10-23, from the Version History API and the tags on the
mirror (chromiumdash was not reachable from the machine that ran this):

| Milestone | Branch | Latest tag | Its date | Reading |
|---|---|---|---|---|
| 158 | trunk | 158.0.8120.1 | 2026-10-22 | canary |
| 157 | 8103 | 157.0.8103.20 | 2026-10-22 | beta (157.0.8103.15 on 0.5% of Windows stable) |
| **156** | 8078 | 156.0.8078.69 | 2026-10-22 | **stable and extended stable**: Extended Stable serves **156.0.8078.48** on Windows and macOS since 2026-10-20 (25% of the extended population on 2026-10-22, rising); Stable rolls out 156.0.8078.47-.49 |
| 155 | 8059 | 155.0.8059.91 | 2026-10-19 | previous stable |
| 154 | 8037 | 154.0.8037.154 | 2026-10-13 | dead |
| 152 | 7977 | 152.0.7977.169 | 2026-10-14 | previous extended stable, being replaced by 156 |
| 150 | 7871 | 150.0.7871.287 | 2026-10-21 | `[M150-LTS]` merges (ChromeOS LTS), not a desktop channel |

So **Extended Stable moved from M152 to M156** on 2026-10-20, and
152.0.7977.169 (our pin since 2026-10-16) was its last M152 release. The
milestone move happened earlier than "~4 releases behind stable" suggested:
with 2-week milestones, Google moved the extended channel to the current
stable milestone. **Target: `156.0.8078.48`**, the Extended Stable release on
Windows and macOS (the [156 rebase](#the-156-rebase-2026-10-23)). Same-branch
respins of it follow (`156.0.8078.*` tags that Extended Stable ships); when the
extended channel moves to a new milestone again, do the big rebase to it.

## The process

### On every Chrome security release (target: shipped within 7 days)

1. Watch [Chrome Releases](https://chromereleases.googleblog.com/) (or the tag
   listing above) for a new tag on our branch, e.g. `156.0.8078.49`.
2. On a branch `rebase/<tag>`:
   `tools/chromium/rebase.py <tag>` and read `report.md`. Same-branch respins
   are normally all clean.
3. `tools/chromium/rebase.py <tag> --apply`, then
   `tools/chromium/overlay_to_patches.py && tools/chromium/verify_patches.py`
   and `tools/moltnet_locale/generate_tables.py` (step 5 below).
4. Compile check and smoke test on the build machine (below), then release.
   Note the Chromium version in the release notes.

### When the tracked milestone moves (the big rebase)

1. Branch `rebase/<tag>`. Run the dry run, read the whole report, and file one
   task per conflict group and per API finding.
2. `tools/chromium/rebase.py <tag> --apply --allow-conflicts`. This writes
   `<<<<<<<` / `|||||||` / `>>>>>>>` (diff3) markers into the conflicted `src/`
   files and moves `chromium_version.txt` to the new tag. Commit this state
   alone so the resolution is reviewable as its own diff.
3. Resolve (see below), until `grep -rn '^<<<<<<< ' src/` is empty.
4. Handle the report's other sections: deleted/moved files, identical copies,
   collisions, the DEPS file, and the API changes in first-party code.
5. `tools/chromium/overlay_to_patches.py && tools/chromium/verify_patches.py`,
   then `tools/moltnet_locale/generate_tables.py`: MoltNet's en-US strings
   and default fonts are generated from Chromium's `.grd` files. If it
   reports drift, rerun it with `--write`; if it names a new Blink string,
   decide whether pages can read it and classify it in the script
   (docs/FINGERPRINTING.md, "MoltNet: the browser's UI language").
6. Compile check, run `molt_ai_unittests`, smoke test, then merge and release.

### Resolving conflicts

- **Read all three sides.** Markers are diff3: ours (`src/...`), the pin
  (`upstream@pin`), and the new upstream (`upstream@new`). Start from
  upstream@new and re-apply *our intent*, rather than editing our old copy.
- **String files (`.grd`/`.grdp`)**: almost all of these conflicts come from
  our "Chrome"/"Chromium" -> "MoltBrowser" rewording (often in comments too)
  colliding with upstream string edits. Take upstream's text and re-apply the
  rebrand. Better: replace these patches with a scripted substitution at apply
  time (what other Chromium forks do); that removes most conflict hunks.
- **`BUILD.gn`**: take upstream, re-add our `deps`/`sources` lines (they are
  usually additive).
- **`third_party/rust/chromium_crates_io/Cargo.lock` and `gnrt_config.toml`**:
  never hand-merge. Take upstream's files, re-add our crates to
  `Cargo.toml`, and regenerate with `tools/crates/run_gnrt.py vendor` and
  `run_gnrt.py gen` in the checkout.
- **Deleted or moved upstream files**: find where the code went
  (`git log --diff-filter=DR -- <path>` in a full checkout) and port the
  change there, or drop it if the feature is gone.
- **Identical copies that upstream changed**: drop them from `src/` (preferred)
  or let `--apply` refresh them; never keep the old copy.
- **First-party collisions** (paths we added that upstream now also has):
  usually vendored crates upstream started shipping. Delete ours and build
  against upstream's version.
- **Binary files** (branding PNGs): ours always wins; no merge needed.

### Compile check (build machine)

There is no Chromium checkout in CI that can build MoltBrowser, so this step
is manual. On the Mac (or the Linux box):

```bash
./scripts/setup.sh --resync --target-os mac,linux   # moves chromium/src to the new pin, syncs DEPS, applies src/
./scripts/configure.sh --platform macos --arch arm64
autoninja -C chromium/src/out/MoltBrowser chrome 2>&1 | tee /tmp/build.log
grep -nE 'error:|FAILED|finished with an error' /tmp/build.log   # autoninja can exit 0 on a failed compile
autoninja -C chromium/src/out/MoltBrowser molt_ai_unittests && chromium/src/out/MoltBrowser/molt_ai_unittests
./scripts/dev-install-mac.sh                        # then smoke test
```

For a faster first signal, compile only the first-party objects (the
`release-linux-ci.yml` checkpoint does this on Linux), fix, and then do the
full build. Expect a full rebuild after a rebase (hours); for the Linux box use
`build/docker/build-linux-fresh.sh`, which reads the same pin.

### Moving fully to the patch series

`src/` overlay copies remain the build input until the patch path has built and
shipped once. After that: build with `scripts/setup.sh --patches`, delete the
`identical.txt` files and every patched file from `src/` (so `src/` is only
first-party code), make `patches/` the thing people edit, and turn the
"patch series in sync" CI job into a required check.

## First dry run (2026-10-14)

Base: `148.0.7729.0` (`51a413be46f7`). `verify_patches.py`: 131 patches apply
and reproduce `src/` byte for byte; 49 copies identical to upstream; 1 DEPS
whole file.

| | to 152.0.7977.168 (extended stable, target) | to 155.0.8059.87 (stable) |
|---|---|---|
| Patched files | 131 | 131 |
| Clean 3-way merges | 56 | 52 |
| Upstream unchanged (ours kept) | 23 | 19 |
| **Conflicts** | **49 files, 216 hunks** | **56 files, 237 hunks** |
| Upstream file deleted or moved | 4 | 5 |
| Identical copies changed upstream | 13 of 49 | 15 of 49 |
| DEPS file subproject moved | 1 | 1 |
| First-party paths now upstream | 180 | 184 |
| Removed upstream symbols still used by our code | 7 | 14 |
| Included upstream headers gone | 5 | 5 |

The 49 conflicts at 152 break down as 21 string files (122 hunks), Cargo.lock +
gnrt_config.toml (51 hunks, regenerate instead), and 26 code/build files (43
hunks). Stable 155 adds 7 more: `chrome/browser/prefs/session_startup_pref.cc`,
`chrome/browser/resources/settings/settings_menu/settings_menu.html`,
`chrome/browser/search/search.cc`, `chrome/browser/ui/startup/infobar_utils.cc`,
`chrome/common/webui_url_constants.h`, `components/arc_strings.grdp`,
`components/page_info_strings.grdp`.

Conflicted files at 152 (hunks):

- Build/code: `chrome/BUILD.gn` (1), `chrome/browser/BUILD.gn` (2),
  `chrome/browser/chrome_content_browser_client.cc` (1),
  `chrome/browser/profiles/BUILD.gn` (1),
  `chrome/browser/profiles/chrome_browser_main_extra_parts_profiles.cc` (1),
  `chrome/browser/resources/side_panel/bookmarks/power_bookmarks_list.ts` (1),
  `chrome/browser/ui/BUILD.gn` (1), `chrome/browser/ui/actions/chrome_action_id.h` (1),
  `chrome/browser/ui/browser.cc` (1), `chrome/browser/ui/browser_actions.cc` (1),
  `chrome/browser/ui/browser_command_controller.cc` (1),
  `chrome/browser/ui/browser_commands.cc` (3), `chrome/browser/ui/cocoa/accelerators_cocoa.mm` (1),
  `chrome/browser/ui/side_panel/side_panel_entry_id.h` (1),
  `chrome/browser/ui/startup/startup_browser_creator_impl.cc` (2),
  `chrome/browser/ui/startup/startup_tab_provider.cc` (1), `chrome/browser/ui/ui_features.cc` (1),
  `chrome/browser/ui/views/frame/browser_frame_view_mac.mm` (1),
  `chrome/browser/ui/views/frame/browser_widget.cc` (2),
  `chrome/browser/ui/views/omnibox/omnibox_view_views.cc` (1),
  `chrome/browser/ui/views/side_panel/side_panel_coordinator.cc` (1),
  `chrome/browser/ui/views/side_panel/side_panel_helper.cc` (1),
  `chrome/browser/ui/views/toolbar/toolbar_view.cc` (4), `chrome/browser/ui/views/toolbar/toolbar_view.h` (4),
  `chrome/browser/ui/webui/BUILD.gn` (1), `chrome/browser/ui/webui/new_tab_page/new_tab_page_ui.cc` (3),
  `components/omnibox/browser/BUILD.gn` (3), `components/omnibox/common/omnibox_features.cc` (1)
- Strings: `chrome/app/bookmarks_strings.grdp` (1), `chromium_strings.grd` (19),
  `extensions_strings.grdp` (1), `generated_resources.grd` (9), `glic_strings.grdp` (3),
  `os_settings_search_tag_strings.grdp` (2), `os_settings_strings.grdp` (2),
  `password_manager_ui_strings.grdp` (3), `profiles_strings.grdp` (7),
  `settings_chromium_strings.grdp` (6), `settings_google_chrome_strings.grdp` (7),
  `settings_strings.grdp` (40), `skills_strings.grdp` (3), `support_tool_strings.grdp` (1)
  (all under `chrome/app/`); `components/autofill_payments_strings.grdp` (2),
  `autofill_strings.grdp` (10), `management_strings.grdp` (1),
  `omnibox_pedal_ui_strings.grdp` (1), `omnibox_strings.grdp` (4)
- Rust: `third_party/rust/chromium_crates_io/Cargo.lock` (26), `gnrt_config.toml` (25)

Deleted or moved upstream at 152: `chrome/browser/resources/side_panel/bookmarks/power_bookmarks_list.html`,
`chrome/browser/ui/views/toolbar/live_toolbar_background.cc`,
`components/os_crypt/sync/BUILD.gn` (OSCrypt sync is gone),
`components/sms_strings.grdp` (an identical copy; just drop it). 155 also
removes `components/privacy_sandbox_chrome_strings.grdp`.

Collisions at 152: upstream now vendors `regex` 1, `aho-corasick` 1,
`getrandom` 0.4, `version_check` 0.9 and `r-efi` 6 (which the anydoc crates
brought in) and has its own `third_party/rust/zip/v8` build files. The vendored
`getrandom`, `version_check` and `r-efi` sources are byte-identical to ours;
`regex` and `aho-corasick` are different patch versions. Drop our copies and
our `third_party/rust/<crate>/` BUILD/README files for them. (What the rebase
actually did is in [The 152 rebase](#the-152-rebase-2026-10-16): upstream
builds `regex`, `aho-corasick`, `regex-automata` and `version_check` test-only,
so we patch their BUILD.gn files instead of dropping them.)

Upstream API changes that hit first-party code (found by diffing the upstream
headers our code includes; heuristic, the compile is authoritative):

| Change (pin -> 152) | Our code |
|---|---|
| `chrome/browser/ui/browser_finder.h` removed | 5 files, e.g. `molt_ai/automation/automation_recorder_tab_helper.cc` |
| `browser_navigator.h`, `browser_navigator_params.h` moved to `chrome/browser/ui/navigator/` | `molt_ai/automation/automation_background_browser.cc` and 1 more |
| `profile_password_store_factory.h` moved to `chrome/browser/password_manager/factories/` | `ui/webui/molt_ai/molt_ai_settings_ui.cc` and 1 more |
| `components/os_crypt/sync/os_crypt.h` removed (move to `os_crypt_async`) | 4 files, e.g. `molt_ai/keys/molt_keys_store.cc` (done: `molt_ai/keys/molt_os_crypt`) |
| `crypto::kdf::DeriveKeyPbkdf2HmacSha1` -> `Pbkdf2HmacSha1(Pbkdf2HmacSha1Params)` | `molt_ai/import/browser_importer.cc:335` |
| `SidePanelEntry::PanelType` -> `SidePanelType` (the shared `kContent` panel) | `ui/views/toolbar/toolbar_view.cc:602` |
| `URLLoaderThrottle` request-header out-params (`modified_request_headers`, `to_be_removed_request_headers`, `modified_cors_exempt_request_headers`) replaced (likely by `HttpRequestHeadersUpdateParams`) | `molt_ai/shield/molt_shield_url_loader_throttle.cc` (`WillRedirectRequest`) |
| `sql::Database::set_preload` removed | `molt_ai/memory/memory_storage.cc:51` (done: dropped, it was a page-cache hint) |

At 155, additionally: `Browser::RequestMediaAccessPermission`,
`Browser::CreateParams::{initial_bounds, initial_show_state, trusted_source}`,
`Browser::is_type_normal` and `BrowserWindowFeatures::side_panel_ui` no longer
appear in the headers our code includes (removed or moved elsewhere),
which hits the automation background browser and the AI-chat side panel. (The
scan also lists `on_confirm`, a false positive: it is only a parameter name,
in the old `browser.h` and in our `web_agent.cc`.)

The full reports regenerate in about 30 seconds:
`tools/chromium/rebase.py 152.0.7977.168` (and `155.0.8059.87`).

## The 152 rebase (2026-10-16)

`tools/chromium/rebase.py 152.0.7977.169 --apply --allow-conflicts`, then the
conflicts resolved in reviewable commits on top (strings, Rust crates, browser
UI, build/startup files, first-party API ports, OSCrypt, Blink). Result:
171 patched upstream files, 48 identical copies, 1 DEPS whole file;
`verify_patches.py` reproduces `src/` byte for byte. Nothing has been
compiled yet.

### Decisions and behaviour changes

- **Strings.** Merged message by message (not hunk by hunk): upstream's text
  plus our rebrand. Messages new at 152 that name Chromium in the product-name
  files (`chromium_strings.grd`, `settings_chromium_strings.grdp`,
  `password_manager_ui_strings.grdp`) were rebranded; new strings of
  Google-only features (Gemini, ChromeOS, iOS promos) were left as upstream
  wrote them.
- **Fingerprinting.** Hooks carried over; AudioWorklet inputs are farbled on
  the new `ArrayBufferContents` span, WebGPU uploads before the new
  `CopyStaticImagBitmapToWGPUTexture`, and farbled copies keep HDR metadata.
  New at 152: the HTML-in-canvas origin trial (`HTMLInCanvas`) would let a
  site render page content into WebGPU and read it back unfarbled, so the
  origin trial policy refuses its tokens.
- **Privacy defaults.** Global Privacy Control (new upstream, flag-gated) is
  on by default. Google AI Mode's omnibox entry point is now `kAim3pEntrypoint`
  (plus the `kAimEnabled` kill switch); both are forced off.
- **UI.** Cmd+Shift+L (now upstream's "collapse vertical tabs") no longer
  opens the AI panel; the command stays for a new shortcut. The toolbar's live
  page reflection is gone with upstream's capture pipeline (the rest of the
  glass look is a `CustomCornersBackground`). An explicit widget colour-mode
  override now wins over our forced dark mode. The macOS vibrancy view in the
  remote_cocoa bridge is dormant (upstream's `kGlassFrame` owns translucency).
- **Rust.** One version per crate epoch, upstream's: regex 1.12.4,
  aho-corasick 1.1.4, regex-automata 0.4.16 (adblock and anydoc accept them;
  `tools/shield_bench` tests on them). Upstream builds regex, aho-corasick,
  regex-automata and version_check test-only; we ship them, so their
  BUILD.gn files are patched (`testonly` dropped). encoding_rs 0.8 (dropped
  upstream) is vendored by us again.
- **Encryption.** OSCrypt (sync) is gone at 152. MoltBrowser's stores use
  `molt_ai/keys/molt_os_crypt`, a shim over the `os_crypt_async` Encryptor
  that `MoltOSCryptStartupFactory` fetches when the first profile is
  created; until it arrives, every store fails closed (chat history and
  memory wait for it). The 152 key providers use the same tags and keys as
  OSCrypt did (Linux v10/v11, macOS v10 Keychain, Windows v10 DPAPI), so
  existing files open; on Linux the secret stores accept v11 and the secret
  portal's v12, never the hard-coded v10.
- **Removed APIs.** `BrowserList` / `browser_finder.h` → `GlobalBrowserCollection`
  / `ProfileBrowserCollection`; `Browser::profile()` → `GetProfile()`;
  `URLLoader::FollowRedirect` and `URLLoaderThrottle::WillRedirectRequest`
  take `HttpRequestHeadersUpdateParams`; `PasswordStoreInterface::AddLogin`
  takes a `StoredCredential`. MoltNet's profile reaper now waits until the
  closed window's `Browser` is destroyed before destroying the profile.

### Still to do on the build machine (in order)

1. `./scripts/setup.sh --resync` to move `chromium/src` to 152.0.7977.169.
2. Refresh the search engines overlay from the synced subproject (this
   environment can't reach it). Step 1 has already copied our version over
   the synced file, so read upstream's from the subproject's own git:
   `git -C chromium/src/third_party/search_engines_data/resources show HEAD:definitions/prepopulated_engines.json > /tmp/prepopulated_engines.upstream.json`,
   then
   `tools/chromium/search_engines_overlay.py /tmp/prepopulated_engines.upstream.json -o src/third_party/search_engines_data/resources/definitions/prepopulated_engines.json`
   and commit the result.
3. `scripts/vendor-rust-crates.sh` (adblock and its crates, windows-sys 0.61
   for Windows; gnrt is the authority on Cargo.lock / BUILD.gn details).
4. `autoninja ... chrome` and `molt_ai_unittests`; send the error log back.
5. Manual checks: close the last MoltNet window (profile destroyed, no
   crash); toolbar look in horizontal and vertical tabs; existing
   keys.enc / vault.enc / profile.enc / chat history still open after the
   os_crypt_async move on each platform.


## The 156 rebase (2026-10-23)

`tools/chromium/rebase.py 156.0.8078.48 --apply --allow-conflicts` from
152.0.7977.169: 154 clean 3-way merges, 55 conflicted files, 3 files
deleted or moved upstream, 8 identical copies refreshed, 2 patches now
empty (the change is upstream), the search-engine definitions to check by
hand, 3 vendored Rust paths that now exist upstream, and 12 removed
symbols that first-party code used. The conflict state is its own commit;
the resolution follows it.
