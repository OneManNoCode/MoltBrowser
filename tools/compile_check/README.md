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
github.com, gitlab.com (one mirror), commondatastorage.googleapis.com (the
toolchain and Chromium's source tarball), crates.io and the Ubuntu archive.
googlesource.com is not needed (some sandboxes can't reach it): every DEPS
repository comes from a mirror or from the tarball.

```bash
tools/compile_check/compile_check.py all              # MoltBrowser + hooks
tools/compile_check/compile_check.py all @patched     # every patched .cc
tools/compile_check/compile_check.py check FILE...    # re-check after a fix
tools/compile_check/compile_check.py report           # [--markdown]
python3 tools/compile_check/compile_check_test.py -v  # the harness's tests
```

`all` downloads the toolchain, repeats fetch (the include closure of the
targets) and gen (the headers a build generates) until nothing new turns up,
then checks every target with 2 parallel compiles (each capped at 6 GB).
A cold run takes a few hours, most of it fetching; re-runs are incremental.
Everything is cached in the work dir (default `.molt-compile-check-cache/`,
gitignored; `--work DIR` or `MOLT_COMPILE_CHECK_DIR`), about 2 GB. The
Chromium tree comes from the blob-less cache `tools/chromium/upstream.py`
keeps in `.molt-chromium-cache/` (`--chromium-cache` to point elsewhere).

`check` never compiles against outdated generated files: it first remakes
the mojom bindings and grit headers whose inputs changed (an edited
`.mojom` or one it imports, a typemap, an edited `.grd`/`.grdp`), and if a
subproject's source changed it fetches and generates again.

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
`<work>/check.json` (every diagnostic, and each file's GN target and
exception/RTTI flags), `<work>/check/<file>.log`, `<work>/gen_report.json`
(what could not be generated and why), `<work>/scan.json` (headers found
nowhere), `<work>/repos/sources.json` (where each subproject came from).

Coverage at the time of writing (`all @first-party @hooks @patched`, plus
`third_party/llama_cpp/chromium/llama_guard.cc`): 552
files, **539 checked**, 13 partly on stand-ins (10 reach webrtc, 3 a WebUI
resource header), no errors, none not checked. Before the tarball, real
grit and per-target exception flags: 496 checked, 54 partly on stand-ins, 2
with errors (both the harness's).

## How it works

* `ccheck/sources.py`: `<work>/chromium/src` is laid out like a checkout.
  Upstream files are fetched as needed, in batches, at the pinned commit;
  every file of MoltBrowser's `src/` overlay is a symlink to the worktree, so
  an edit shows up at once; generated files go to `out/cc/gen`. DEPS
  repositories (perfetto, boringssl, skia, v8, quiche, ...) are mounted at
  their Chromium path at the gitlink commit, each from the first source
  that has it:
  1. a git mirror (blob-less clone; git checks the commit id, so any mirror
     that has the commit serves the exact tree): GitHub's, or a second one
     where GitHub's lacks release branches (pdfium);
  2. Chromium's official source tarball (`ccheck/tarball.py`): the pinned
     release's if it is published, else the newest published release of
     the same branch. It is streamed once (about 3 minutes; only the files
     of the mounts that need it are kept, about 350 MB) and serves the
     repositories no mirror has: the release-branch commits of dawn and
     webrtc, and Chromium's own libc++, libc++abi, ICU and
     dom_distiller_js. A mount whose gitlink in that release is the pin's
     is exact; otherwise it is the nearest public tree, a stand-in;
  3. a substitute (`SUBSTITUTES`) or the mirror's HEAD, both stand-ins.

  `repos/sources.json` records each mount's source; when it changes, the
  mount's files, its generated files and what actions made from it
  (`gen_inputs.json`) are dropped and made again.
* `ccheck/scan.py`: the include closure. Conditional branches are followed
  unless certainly false on Linux, so it over-fetches a little.
* `ccheck/gn.py`: a small GN interpreter (BUILDCONFIG, imports,
  `declare_args`, `set_defaults`, templates, `foreach`,
  `get_target_outputs`, `process_file_template`, `write_file`,
  `exec_script` for `version.py`) run with `configs/linux-x64.gn`.
* Generated headers, made the way the build makes them:
  - buildflag headers: the `buildflag_header()` target's flags, evaluated,
    written by `build/write_buildflag_header.py` (`gen_buildflags.py`);
  - mojom: Chromium's own parser and C++ generator (chromium and blink
    variants), with the typemaps of every `mojom()` target (`gen_mojom.py`).
    Outputs are keyed by the content of the `.mojom` and everything it
    imports, the typemaps and its `enabled_features` (`mojom_stamps.json`);
  - grit resource headers: Chromium's grit, run with the args of the
    `.grd`'s `grit()` action and the first-ids file the build makes
    (`tools/gritsettings:default_resource_ids`, grit's
    `update_resource_ids` over `resource_ids.spec`), writing only the
    headers (`grit_headers.py`; the `.pak` files would need the resources
    themselves). Keyed by the `.grd`, its parts, the first-ids file and the
    args (`grit_stamps.json`);
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
  BUILD.gn files. Each file's exception and RTTI setting comes from its own
  GN target (the one whose `sources` list it): on when the target removes
  `//build/config/compiler:no_exceptions` (`:no_rtti`), adds
  `:exceptions` (`:rtti`) or a config of its own with `-fexceptions`
  (`-frtti`); llama.cpp's exception boundary, `llama_guard.cc`, is the
  current case.
* `ccheck/check.py`: Chromium's own clang (the `tools/clang` pin) with its
  Debian sysroot, C++23, `-fno-exceptions -fno-rtti` unless the file's
  target says otherwise, the Linux official-build defines, the
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
| WebUI resource headers (`chrome/grit/*_resources{,_map}.h`, `ui/webui/resources/grit/webui_resources{,_map}.h`) | the map declaration; the `IDR_<STEM>_*` names its includers use | grit on the `.grd` the WebUI TypeScript build generates |
| webrtc | the tree of the newest published release before the pin (152.0.7977.132's commit, three release-branch rolls behind) | the pinned commit, on webrtc's release branch, which no reachable mirror carries |
| `base/generated_build_date.h` | fixed timestamp | the build's timestamp |
| `build/util/LASTCHANGE*`, `build/config/gclient_args.gni` | fixed values | written by gclient hooks |
| mojom typemaps, `enabled_features` | the union over all targets | each target's own and its deps' |
| perfetto protoc plugins | linked against Ubuntu's libprotoc 3.21 | against third_party/protobuf |

Only when their source can't be had (no tarball for the branch, or no
network to it), these stand in too, and `report` names them per file:
another grit header (every `name` in its `.grd`, made-up numbers), a
subproject from its mirror's HEAD (pdfium, webrtc, libyuv, openscreen,
dawn), dom_distiller_js's `proto/` from the dom-distiller repository's
HEAD, libc++/libc++abi from llvm-project at the commit Chromium's clang was
built from, ICU from upstream `release-78.3`, and `icu/config.gni`,
`webrtc.gni`, `dawn_features.gni` from their Linux values in `gn.py`.

The WebUI headers and the subprojects that are not at the pin are named
per file by `report`; the rest apply to every file.

## What it can't cover

* Linking, ODR and anything past the front end (no object files).
* Other platforms: macOS (`.mm`, Metal), Windows, Android code is not
  compiled.
* Files whose headers can't be had here are reported as "not checked",
  with the header; `gen_report.json` lists every header that stayed
  missing and why.
* An API that a stand-in repository (above) has differently from the
  pinned commit shows up as errors that a real build would not have (or
  hides one): `report` adds "reaches <repository> (...)" to such a file's
  result, for a human to judge.
* Code built only under other GN args, or only by a target whose own
  configs are not in `gnconfigs.py`: the file is compiled with the configs
  listed there (and its own target's exception/RTTI setting), not with its
  target's exact set. Generated files are not remade when
  `configs/linux-x64.gn` changes; start from a fresh work dir then.
* GN itself: `gn check`, DEPS rules (checkdeps), visibility and target
  dependencies are not checked; a file compiles here if its includes resolve
  anywhere in the tree (with the union of every target's include dirs).
* Rust (`.rs`) and the WebUI JavaScript (see `tools/webui_checks`).
