# compile_check: MoltBrowser's C++ against Chromium's real headers

A syntax-and-type check (`clang -fsyntax-only`) of MoltBrowser's C++ against
the real headers of the pinned Chromium (`chromium_version.txt`), without a
Chromium checkout. It catches what hand-written stand-ins hide: renamed
APIs, changed signatures, missing includes, a header that no longer exists,
`base::NoDestructor`/`raw_ptr`/chromium-style plugin errors. It is a checker
only: nothing here feeds the real build. The build machine
(`docs/BUILD_MACHINE_CHECKLIST.md`) stays the authority: see "What it can't
cover" below.

## Running it

Linux x86-64, Python 3, git, `clang++` (for the small host tools), `cargo`
(for cxxbridge), `apt-get` (only if `libprotoc-dev` is not installed), and
HTTPS to github.com, commondatastorage.googleapis.com, crates.io and the
Ubuntu archive. googlesource.com is not needed (it is unreachable from some
sandboxes): every DEPS repository comes from its GitHub mirror.

```bash
tools/compile_check/compile_check.py all            # everything, ~1-2 h cold
tools/compile_check/compile_check.py check FILE...  # re-check after a fix
tools/compile_check/compile_check.py report         # last results
```

`all` downloads the toolchain, then repeats fetch (the include closure of the
targets) and gen (the headers a build generates) until nothing new turns up,
then checks every target with 2 parallel compiles. Re-runs are incremental:
everything is cached in the work dir (default `.molt-compile-check-cache/`,
gitignored; `--work DIR` or `MOLT_COMPILE_CHECK_DIR` to move it, about 2 GB).
The Chromium tree comes from the blob-less cache `tools/chromium/upstream.py`
keeps in `.molt-chromium-cache/` (`--chromium-cache` to point elsewhere).

Targets (`targets.py`): by default every `.cc` in
`chrome/{browser,services,common,renderer}/molt_ai/`,
`chrome/browser/ui/webui/molt_ai/` and `components/omnibox/browser/molt_*`
(other platforms' files skipped), plus the upstream files with MoltBrowser
hooks listed in section 4 of the build checklist. Also: a path or directory
under `src/`, `@first-party`, `@hooks`, `@patched` (every modified upstream
`.cc`) and `@list:FILE`.

Results: `<work>/check.json` (every diagnostic), one log per file in
`<work>/check/`, `<work>/gen_report.json` (what could not be generated and
why), `<work>/scan.json` (headers found nowhere).

## How it works

* `ccheck/sources.py`: `<work>/chromium/src` is laid out like a checkout.
  Upstream files are fetched as needed (in batches) from the pinned commit;
  every file of MoltBrowser's `src/` overlay is a symlink to the worktree,
  so an edit shows up at once; generated files go to `out/cc/gen`. DEPS
  repositories (perfetto, boringssl, skia, v8, ...) are blob-less clones of
  their GitHub mirrors at the gitlink commit, mounted at their Chromium path.
* `ccheck/scan.py`: the include closure. Conditional branches are followed
  unless certainly false on Linux, so it over-fetches a little.
* `ccheck/gn.py`: a small GN interpreter (BUILDCONFIG, imports,
  `declare_args`, templates, `foreach`, `exec_script` for `version.py`) run
  with `configs/linux-x64.gn`.
* Generated headers, made the way the build makes them:
  - buildflag headers: the `buildflag_header()` target's flags, evaluated,
    written by `build/write_buildflag_header.py` (`gen_buildflags.py`);
  - mojom: Chromium's own parser and C++ generator, with the typemaps of
    every `mojom()` target (`gen_mojom.py`);
  - everything else a GN `action`/`action_foreach` writes (protos, ukm
    builders, blink runtime features, extensions API types, histogram
    variants, ...): the action itself, from the build directory, after the
    generated inputs it needs (`gen_actions.py`). Host tools Chromium builds
    from source are replaced: protoc is the release matching
    `third_party/protobuf` (33.0); perfetto's protozero and cppgen plugins,
    flatc and cxxbridge are built here from the pinned sources
    (`host_tools.py`).
* `ccheck/check.py`: Chromium's own clang (the `tools/clang` pin), C++23,
  `-fno-exceptions -fno-rtti`, the Linux official-build defines, the
  chromium-style and raw_ptr plugins as an official build runs them
  (`--dev-plugins` adds the raw_ptr/raw_ref field checks of a developer
  build), `-Wall -Wextra` and the `chromium_code` warnings
  (`treat_warnings_as_errors` is off in `configs/linux-x64.gn`, so warnings
  are reported, not fatal).

## Stand-ins and substitutes

A diagnostic inside one of these is a harness problem, not a MoltBrowser
bug. `gen_misc.py` marks each generated stand-in in its first line.

| What | Here | The real build |
|---|---|---|
| grit resource headers (`grit/*.h`) | every `name` in the `.grd` and its parts, made-up numbers | grit, numbers from `resource_ids.spec` |
| `base/generated_build_date.h` | fixed timestamp | the build's timestamp |
| `build/util/LASTCHANGE*`, `build/config/gclient_args.gni` | fixed values | written by gclient hooks |
| `.gni` files of DEPS repos without a mirror (`icu/config.gni`, `webrtc.gni`, `dawn_features.gni`) | the few `declare_args` the Linux build reads (`gn.py`) | the real files |
| libc++ / libc++abi headers | llvm-project at the commit Chromium's clang was built from | Chromium's libc++ mirror at its own pin (googlesource only) |
| ICU headers | upstream ICU `release-78.3` | Chromium's ICU fork |
| pdfium public headers | the GitHub mirror's HEAD (the pinned commit is not mirrored) | pdfium at the pin |
| C library headers | the host's glibc | Debian bullseye sysroot |
| mojom typemaps, `enabled_features` | the union over all targets | each target's own and its deps' |
| perfetto protoc plugins | linked against Ubuntu's libprotoc 3.21 | against third_party/protobuf |

## What it can't cover

* Linking, ODR and anything past the front end (no object files).
* Other platforms: macOS (`.mm`, Metal), Windows, Android code is not
  compiled.
* Files whose headers can't be had here: DEPS repositories with no usable
  GitHub mirror (dawn: `content/utility/on_device_model/` and the
  `services/on_device_model/ml` headers; webrtc; openscreen;
  `dom_distiller_js/dist`), and the Blink bindings that need the IDL
  compiler. `gen_report.json` lists every header that stayed missing; a
  check that stops at one reports a fatal error there.
* GN itself: `gn check`, DEPS rules (checkdeps), visibility and target
  dependencies are not checked; a file compiles here if its includes resolve
  anywhere in the tree.
* Rust (`.rs`) and the WebUI JavaScript (see `tools/webui_checks`).
