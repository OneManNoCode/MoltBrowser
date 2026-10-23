# compile_check: MoltBrowser's C++ against Chromium's real headers

A syntax-and-type check (`clang -fsyntax-only`) of MoltBrowser's C++ against
the real headers of the pinned Chromium (`chromium_version.txt`), without a
Chromium checkout. It catches what hand-written stand-ins hide: renamed
APIs, changed signatures, missing includes, a header that no longer exists,
`consteval`/`static_assert` failures, chromium-style and raw_ptr plugin
errors. It is a checker only: nothing here feeds the real build. The build
machine (`docs/BUILD_MACHINE_CHECKLIST.md`) stays the authority; see "What
it can't cover".

## Running it

Linux x86-64, Python 3, git, `clang++` and `cargo` (for the small host tools
below), `apt-get` (only if `libprotoc-dev` is not installed), and HTTPS to
github.com, commondatastorage.googleapis.com, crates.io and the Ubuntu
archive. googlesource.com is not needed (some sandboxes can't reach it):
every DEPS repository comes from its GitHub mirror.

```bash
tools/compile_check/compile_check.py all              # MoltBrowser + hooks
tools/compile_check/compile_check.py all @patched     # every patched .cc
tools/compile_check/compile_check.py check FILE...    # re-check after a fix
tools/compile_check/compile_check.py report           # [--markdown]
```

`all` downloads the toolchain, repeats fetch (the include closure of the
targets) and gen (the headers a build generates) until nothing new turns up,
then checks every target with 2 parallel compiles (each capped at 6 GB).
A cold run takes a few hours, most of it fetching; re-runs are incremental.
Everything is cached in the work dir (default `.molt-compile-check-cache/`,
gitignored; `--work DIR` or `MOLT_COMPILE_CHECK_DIR`), about 3 GB. The
Chromium tree comes from the blob-less cache `tools/chromium/upstream.py`
keeps in `.molt-chromium-cache/` (`--chromium-cache` to point elsewhere).

Targets (`ccheck/targets.py`): by default every `.cc` in
`chrome/{browser,services,common,renderer}/molt_ai/`,
`chrome/browser/ui/webui/molt_ai/`, Blink's `core/molt_fingerprinting/` and
`core/molt_net/`, and the `molt_*` files in `components/omnibox/browser/`,
`chrome/browser/ui/views/toolbar/` and `content/utility/on_device_model/`
(other platforms' files skipped), plus the upstream files with MoltBrowser
hooks named in section 4 of the build checklist. Also: a path or directory
under `src/`, `@first-party`, `@hooks`, `@patched` (every modified upstream
`.cc`, Blink included) and `@list:FILE`.

`report` puts each file in one of four classes: **checked** (compiled
against real headers), **partly on stand-ins** (its include closure reaches
one of the stand-ins below; which one is named), **ERRORS**, or **not
checked** (a header it needs can't be had here; named). Details:
`<work>/check.json` (every diagnostic), `<work>/check/<file>.log`,
`<work>/gen_report.json` (what could not be generated and why),
`<work>/scan.json` (headers found nowhere).

## How it works

* `ccheck/sources.py`: `<work>/chromium/src` is laid out like a checkout.
  Upstream files are fetched as needed, in batches, at the pinned commit;
  every file of MoltBrowser's `src/` overlay is a symlink to the worktree, so
  an edit shows up at once; generated files go to `out/cc/gen`. DEPS
  repositories (perfetto, boringssl, skia, v8, quiche, ...) are blob-less
  clones of their GitHub mirrors at the gitlink commit, mounted at their
  Chromium path.
* `ccheck/scan.py`: the include closure. Conditional branches are followed
  unless certainly false on Linux, so it over-fetches a little.
* `ccheck/gn.py`: a small GN interpreter (BUILDCONFIG, imports,
  `declare_args`, templates, `foreach`, `get_target_outputs`,
  `process_file_template`, `write_file`, `exec_script` for `version.py`)
  run with `configs/linux-x64.gn`.
* Generated headers, made the way the build makes them:
  - buildflag headers: the `buildflag_header()` target's flags, evaluated,
    written by `build/write_buildflag_header.py` (`gen_buildflags.py`);
  - mojom: Chromium's own parser and C++ generator (chromium and blink
    variants), with the typemaps of every `mojom()` target (`gen_mojom.py`);
  - everything a GN `action`/`action_foreach` writes (protos, Blink's IDL
    bindings, names and runtime features, ukm builders, extensions API
    types, histogram variants, ...): the action itself, run from the build
    directory after the generated inputs it needs (`gen_actions.py`);
  - host tools Chromium builds from source are replaced: protoc is the
    release matching `third_party/protobuf` (33.0); perfetto's protozero and
    cppgen plugins, flatc and cxxbridge are built here from the pinned
    sources (`host_tools.py`).
* `ccheck/gnconfigs.py`: the defines, include dirs and cflags of the GN
  `config()`s a file gets from its dependencies (`//ui/views:flags` for
  `TOOLKIT_VIEWS`, skia, perfetto, boringssl, webrtc, v8) and, by
  directory, from its own target (`content_implementation`, Blink's
  `inside_blink`, `features` and its plugin config), read from the real
  BUILD.gn files.
* `ccheck/check.py`: Chromium's own clang (the `tools/clang` pin) with its
  Debian sysroot, C++23, `-fno-exceptions -fno-rtti`, the Linux
  official-build defines, the chromium-style and raw_ptr plugins as an
  official build runs them (`--dev-plugins` adds the raw_ptr/raw_ref field
  checks of a developer build), `-Wall -Wextra` and the `chromium_code`
  warnings (`treat_warnings_as_errors` is off in `configs/linux-x64.gn`, so
  warnings are reported, not fatal).

## Stand-ins and substitutes

A diagnostic inside one of these is a harness problem, not a MoltBrowser
bug. `gen_misc.py` marks each generated stand-in in its first line.

| What | Here | The real build |
|---|---|---|
| grit resource headers (`grit/*.h`) | every `name` in the `.grd` and its parts, made-up numbers | grit, numbers from `resource_ids.spec` |
| WebUI resource headers (`chrome/grit/*_resources{,_map}.h`, generated `.grd`) | the map declaration; the `IDR_<STEM>_*` names its includers use | grit on the `.grd` the WebUI build generates |
| `base/generated_build_date.h` | fixed timestamp | the build's timestamp |
| `build/util/LASTCHANGE*`, `build/config/gclient_args.gni` | fixed values | written by gclient hooks |
| `icu/config.gni`, `webrtc.gni`, `dawn_features.gni` | the `declare_args` the Linux Chromium build reads, with their Linux values (`gn.py`) | the real files |
| pdfium, webrtc, libyuv, openscreen, dawn | the GitHub mirror's HEAD (the pinned commit is not mirrored) | the pinned commit |
| `third_party/dom_distiller_js/dist` (a CIPD package) | `proto/` of the dom-distiller repository's HEAD | the CIPD package |
| libc++ / libc++abi headers | llvm-project at the commit Chromium's clang was built from | Chromium's libc++ mirror at its own pin (googlesource only) |
| ICU headers | upstream ICU `release-78.3` | Chromium's ICU fork |
| mojom typemaps, `enabled_features` | the union over all targets | each target's own and its deps' |
| perfetto protoc plugins | linked against Ubuntu's libprotoc 3.21 | against third_party/protobuf |

The first two kinds and the mirror-HEAD repositories are named per file by
`report`; the rest apply to every file.

## What it can't cover

* Linking, ODR and anything past the front end (no object files).
* Other platforms: macOS (`.mm`, Metal), Windows, Android code is not
  compiled.
* Files whose headers can't be had here are reported as "not checked",
  with the header; `gen_report.json` lists every header that stayed
  missing and why.
* Code built only under other GN args, or only by a target whose own
  configs are not in `gnconfigs.py`: the file is compiled with the configs
  listed there, not with its target's exact set.
* GN itself: `gn check`, DEPS rules (checkdeps), visibility and target
  dependencies are not checked; a file compiles here if its includes resolve
  anywhere in the tree (with the union of every target's include dirs).
* Rust (`.rs`) and the WebUI JavaScript (see `tools/webui_checks`).
