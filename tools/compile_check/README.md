# compile_check: MoltBrowser's C, C++ and Objective-C++ against Chromium's real headers

A syntax-and-type check (`clang -fsyntax-only`) of every C, C++ and
Objective-C++ file MoltBrowser adds or changes, against the real headers of
the pinned Chromium (`chromium_version.txt`), without a Chromium checkout,
on each platform the file is built for (Linux x64 and arm64, Windows x64,
macOS arm64, and Linux with the off-by-default Vulkan switch). It catches what hand-written stand-ins hide: renamed APIs,
changed signatures, missing includes, a header that no longer exists,
`consteval`/`static_assert` failures, chromium-style and raw_ptr plugin
errors, and a hook in another platform's `#if` branch that no Linux build
compiles. It is a checker only: nothing here feeds the real build. The build
machine (`docs/BUILD_MACHINE_CHECKLIST.md`) stays the authority; see "What
it can't cover".

## Running it

Linux x86-64, Python 3, git, `clang++`, `ar` and `cargo` (for the small host
tools below), `rustc` (its `--print=cfg`), `apt-get` (only if
`libprotoc-dev` is not installed), and HTTPS to github.com, gitlab.com (one
mirror), commondatastorage.googleapis.com (the toolchain, the sysroots and
Chromium's source tarball), crates.io, the Ubuntu archive, api.nuget.org
(the Windows SDK) and files.pythonhosted.org (Darwin's libc headers).
googlesource.com is not needed (some sandboxes can't reach it): every DEPS
repository comes from a mirror or from the tarball.

```bash
tools/compile_check/compile_check.py all              # everything changed
tools/compile_check/compile_check.py all @hooks       # a group, or files
tools/compile_check/compile_check.py check FILE...    # re-check after a fix
tools/compile_check/compile_check.py check --platforms win FILE...
tools/compile_check/compile_check.py report           # [--markdown]
tools/compile_check/compile_check.py clean [--dry-run] # stale outputs
python3 tools/compile_check/compile_check_test.py -v  # the harness's tests
```

`all` downloads the toolchain, repeats fetch (the include closure of the
targets) and gen (the headers a build generates) until nothing new turns up,
then checks every target with 2 parallel compiles (each capped at 6 GB;
each result records the compile's peak memory, and `report` names the
largest, to size `--jobs` on a shared machine), platform by platform. A
cold run takes several hours, most of it fetching and the Linux compiles.
Everything is cached in the work dir (default `.molt-compile-check-cache/`,
gitignored; `--work DIR` or `MOLT_COMPILE_CHECK_DIR`), about 3 GB. The
Chromium tree comes from the blob-less cache `tools/chromium/upstream.py`
keeps in `.molt-chromium-cache/` (`--chromium-cache` to point elsewhere).
When the GitHub mirror answers 5xx, fetches are retried for up to 20
minutes.

Re-runs are incremental, without trusting the cache for correctness:

* A file is compiled again only if its result could differ: before
  compiling, its command runs as `-E -dD` (preprocess only), and the
  result stored for the file and platform (`check/cache/`) is reused when
  the hash of that output, of the command and of the compiler's build is
  the one it was stored under (`ccheck/result_cache.py`). The preprocessed
  output holds every header's content and path, every macro definition and
  the outcome of every header search and `__has_include`, so an edited,
  added (shadowing) or regenerated header, or another flag, compiles the
  file again; this costs about a tenth of a compile per file. A compile
  the memory limit or a signal stopped is never stored; `--no-cache`
  compiles everything.
* A generated file is made again only when its inputs changed, and is
  never compiled against when they did: `check` first remakes the mojom
  bindings, grit headers and cxx bridge headers whose inputs changed (an
  edited `.mojom` or one it imports, a typemap, an edited `.grd`/`.grdp`,
  an edited `.rs` bridge), the outputs of GN actions whose script,
  inputs, sources, generated inputs, args or BUILD.gn changed
  (`action_stamps.json`), and the buildflag headers whose flags changed
  (`buildflag_stamps.json`). When the platform's `configs/*.gn` or one of
  MoltBrowser's GN files changes, the flags, the mojom targets'
  `enabled_features` and the grit actions' args are evaluated again, so
  what they changed is remade too. One that can't be remade is removed,
  so the check reports it missing. If a subproject's source changed it
  fetches and generates again.

`clean` removes what no run reads: results of files no longer checked,
generated sources and mojom parse results (only headers are compiled; the
mojom generator no longer leaves its parse results either), the objects of
host tools once built, unpacked archives, leftovers of interrupted runs,
clones no subproject uses and the state of platforms no longer modeled
(`--dry-run` lists them by kind and size). `--generated` also drops every
generated file and the generator state, to start the generators afresh;
`--results` drops all results.

### Targets

`ccheck/targets.py`. The default, `@changed`, is every `.cc`, `.cpp`, `.c`,
`.mm` and `.m` file of the `src/` overlay that is new or differs from the
pinned upstream file (Chromium's or a mounted subproject's), except
vendored third-party code that is not Chromium's: llama.cpp's own sources
(built and tested by its CMake build; MoltBrowser's glue in
`third_party/llama_cpp/chromium/` is checked) and the vendored Rust crates'
C++ (a Qt test tool no GN target builds). Also: a path or directory under
`src/`, `@first-party` (MoltBrowser's own directories), `@hooks` (the
upstream files of section 4 of the build checklist), `@patched` (every
modified upstream source) and `@list:FILE`.

### Platforms

`ccheck/platforms.py`. A file is checked for the platform its name and
directory say, as Chromium's BUILD.gn files pick sources: `*_win.cc` and
`win/` on Windows; `.mm`, `*_mac.cc`, `mac/`, `apple/`, `cocoa/` on the Mac;
NEON sources (`arm/*.c`, `*_neon*.c`) on Linux arm64 (the Mac arm64 build
compiles them too); llama.cpp's Vulkan glue and host tools
(`third_party/llama_cpp/chromium/ggml-vulkan*`, `ggml_vk_*`) and the
inference service's `vulkan_*` test on `linux-vulkan`: Linux x64 with the
GN switch `molt_llama_vulkan = true`, which every `configs/*.gn` leaves
off; everything else on Linux x64. Android, ChromeOS, iOS and Fuchsia
files are reported as not checked (no platform modeled).

A file is also checked on another platform when some of its changed lines
(the lines that differ from the pinned upstream file; every line of a new
file) are in a branch its own platform certainly skips and that platform
doesn't: `utility_main.cc`'s hook in its `#if BUILDFLAG(IS_WIN)` block is
compiled for Windows, `font_metrics.cc`'s in its `IS_APPLE` branch for the
Mac, `browser_ai_runtime.cc`'s `BUILDFLAG(MOLT_LLAMA_VULKAN)` block on
`linux-vulkan`. The same naming conventions keep a file off a platform
that can't build it (no `*_posix.cc` on Windows, no `linux/` file on the
Mac). MoltBrowser's own sources (new files, not upstream copies) are
compiled for Windows too, when a Windows GN target builds them
(`configs/windows-x64.gn`; the others are left to Linux): the same code
compiles differently there, since `base::FilePath` holds wide strings and
Win32's headers bring their own macros. `report` lists such a file once
per platform. `--platforms win,mac` limits a `check` or `all` run.

Each platform has its own build directory (`out/cc` for Linux x64,
`out/cc-<platform>` for the others) and its own generator state in the
work dir (`platform-<name>/`), since a build generates headers from that
platform's GN args (`configs/<platform>.gn`): buildflag headers, mojom
bindings with `[EnableIf=is_win]` fields, grit headers. The checkout, the
subprojects and the toolchain are shared.

| Platform | Compiler | System headers |
|---|---|---|
| Linux x64 | clang++ / clang (C) | the Debian sysroot Chromium builds with |
| Linux arm64 | `--target=aarch64-linux-gnu` | the Debian arm64 sysroot |
| Linux x64, `molt_llama_vulkan` (`linux-vulkan`) | as Linux x64 | as Linux x64; plus Vulkan-Headers, SPIRV-Headers, glslang and SPIRV-Tools at their DEPS pins |
| Windows x64 | clang-cl, `/std:c++23preview`, Chromium's Windows defines (not an official build there: DCHECKs on) | the Windows SDK of `build/vs_toolchain.py` (10.0.28000) and the MSVC runtime headers, from Microsoft's packages on nuget.org (pinned SHA-512), found case-insensitively (a clang VFS overlay) |
| macOS arm64 | `--target=arm64-apple-macos -mmacos-version-min=13.0`, ARC for Objective-C++ | Darwin's libc and libSystem headers (as the Zig toolchain ships them; its PyPI wheel, pinned SHA-256) and stand-ins for the Apple frameworks (`mac_stand_ins/`) |

`report` puts each file in one of five classes: **checked** (compiled
against real headers), **partly on stand-ins** (its include closure reaches
one of the stand-ins below; which one is named), **ERRORS**, **PLUGIN**
(it compiles, but Chromium's clang plugins report something in
MoltBrowser's code, a file of the `src/` overlay: `[chromium-style]`,
`[chromium-rawptr]`, ...; listed. Warnings in these configs, which set
`treat_warnings_as_errors = false`; errors in a `-Werror` build such as
Chromium's CQ), or **not checked** (a header it needs can't be had here,
or its platform isn't modeled; named, with the reason when one is known:
the generator's failure from `gen_report.json`, an Apple framework
`mac_stand_ins/` has no stand-in for, the Apple declarations a Mac file
needs that the stand-ins lack, or that no GN target lists the file with
the platform's args). Details: `<work>/check.json` (every diagnostic, and each
file's platform, language, GN target and exception/RTTI flags),
`<work>/check/<file>[@platform].log` and `check/commands/` (the exact
command), `<work>/[platform-<name>/]gen_report.json` (what could not be
generated and why), `scan.json` (headers found nowhere),
`<work>/repos/sources.json` (where each subproject came from).

### Coverage at 156 (round 28)

A whole run (`all`) over every changed source (858 files; a file built on
several platforms has a row for each):

| Platform | rows | checked | partly on stand-ins | not checked | ERRORS | PLUGIN |
|---|---|---|---|---|---|---|
| Linux x64 | 837 | 831 | 6 | 0 | 0 | 0 |
| Linux arm64 | 2 | 2 | 0 | 0 | 0 | 0 |
| Linux x64, `molt_llama_vulkan` | 6 | 6 | 0 | 0 | 0 | 0 |
| macOS arm64 | 34 | 0 | 30 | 4 | 0 | 0 |
| Windows x64 | 503 | 503 | 0 | 0 | 0 | 0 |
| none | 1 | 0 | 0 | 1 | 0 | 0 |
| **all** | **1383** | **1342** | **36** | **5** | **0** | **0** |

Partly on stand-ins: every Mac file (the Apple frameworks), three WebUI
pages' grit resource headers, the HSTS/pins preload headers
(`transport_security_state.cc`), the build date
(`transport_security_state.cc`, `network_context.cc`) and V8's instance
types from Torque (`v8.cc`). Not checked: `app_controller_mac.mm` and
`chrome_browser_main_mac.mm` (AuthenticationServices),
`browser_frame_view_mac.mm` and `native_widget_ns_window_bridge.mm`
(IOSurface, then much of AppKit), all Apple frameworks with no stand-in
here; and `chrome/browser/molt_ai/ios/MoltAIChatViewController.mm` (iOS,
not modeled). The round before (Linux only, the files then listed):
861 files, 806 checked, 39 partly on stand-ins, 16 not checked (the
Windows and Mac files, opus's C files, llvm-libc, CrabbyAvif, V8, the cxx
bridge, protobuf and proto_extras headers, the preload headers), and no
plugin findings reported.

## How it works

* `ccheck/sources.py`: `<work>/chromium/src` is laid out like a checkout.
  Upstream files are fetched as needed, in batches, at the pinned commit;
  every file of MoltBrowser's `src/` overlay is a symlink to the worktree, so
  an edit shows up at once; generated files go to `out/cc*/gen`. DEPS
  repositories (perfetto, boringssl, skia, v8, quiche, crabbyavif, ...) are
  mounted at their Chromium path at the gitlink commit, each from the first
  source that has it:
  1. a git mirror (blob-less clone; git checks the commit id, so any mirror
     that has the commit serves the exact tree): GitHub's, or a second one
     where GitHub's lacks release branches (pdfium);
  2. Chromium's official source tarball (`ccheck/tarball.py`): the pinned
     release's if it is published, else the newest published release of
     the same branch, used only if it matches the SHA-256 pinned for it
     (`PINNED_SHA256`; one without a pin is refused). It is streamed once
     (about 5 minutes; only the files of the mounts that need it are kept,
     about 450 MB, none outside its mount) and serves the
     repositories no mirror has: the release-branch commits of dawn and
     webrtc, Chromium's own libc++, libc++abi, ICU and dom_distiller_js,
     libaom, and llvm-libc (Chromium pins a libc-only split of
     llvm-project). A mount whose gitlink in that release is the pin's is
     exact; otherwise it is the nearest public tree, a stand-in;
  3. a substitute (`SUBSTITUTES`) or the mirror's HEAD, both stand-ins.

  `repos/sources.json` records each mount's source; when it changes, the
  mount's files, its generated files (on every platform) and what actions
  made from it (`gen_inputs.json`) are dropped and made again.
* `ccheck/scan.py`: the include closure. A conditional branch is followed
  unless it is certainly false on the platform: its OS and CPU atoms
  (`platforms.py`), and every buildflag a generated buildflag header of
  that platform defines (`BUILDFLAG(USE_VAAPI)` true and
  `BUILDFLAG(ENABLE_OOP_VIDEO_DECODER)` false on Linux, once the headers
  are made). It still over-fetches a little. Angle includes found in the
  platform's SDK are the system's. A library's internal include roots
  (`config.SCOPED_INCLUDE_DIRS`: V8's `src/...` and `torque-generated/`,
  llvm-libc's `src/`, opus's) are searched only from that library's files;
  a compile adds them when the file's include closure reaches the library,
  as its dependency's public config would.
* `ccheck/gn.py`: a small GN interpreter (BUILDCONFIG, imports,
  `declare_args`, `set_defaults`, templates, `foreach`,
  `get_target_outputs`, `process_file_template`, `write_file`,
  `exec_script` for `version.py` and `compute_build_timestamp.py`) run
  with the platform's `configs/<platform>.gn`. `build/util/LASTCHANGE` and
  its `.committime` are written as gclient's hook writes them for the
  pinned commit (its `Cr-Commit-Position` and committer time, read from
  the cache), so the build date (`base/generated_build_date.h`, from
  base's own actions) is the official build's.
* Generated headers, made the way the build makes them:
  - buildflag headers: the `buildflag_header()` target's flags, evaluated,
    written by `build/write_buildflag_header.py` (`gen_buildflags.py`);
  - mojom: Chromium's own parser and C++ generator (chromium and blink
    variants), with the typemaps of every `mojom()` target (`gen_mojom.py`).
    Each `.mojom` is parsed with its own target's `enabled_features` plus
    the platform's (`is_linux`, `is_win`, ...), after the modules it
    imports, as `mojom.gni` does; a file the parser rejects (a ChromeOS-only
    mojom the scan reached through a branch it could not rule out) fails
    alone, with its importers, and is reported. Outputs are keyed by the
    content of the `.mojom` and everything it imports, the typemaps and its
    `enabled_features` (`mojom_stamps.json`);
  - grit resource headers: Chromium's grit, run with the args of the
    `.grd`'s `grit()` action and the first-ids file the build makes
    (`tools/gritsettings:default_resource_ids`, grit's
    `update_resource_ids` over `resource_ids.spec`), writing only the
    headers (`grit_headers.py`; the `.pak` files would need the resources
    themselves). Keyed by the `.grd`, its parts, the first-ids file and the
    args (`grit_stamps.json`);
  - cxx bridge headers (`.rs.h`): cxxbridge built from Chromium's vendored
    cxxbridge-cmd (its dependencies the versions its `Cargo.lock` pins,
    `cargo install --locked`), run as `run_cxxbridge.py` runs it, with a
    `--cfg` for every line `rustc --print=cfg` gives for the platform's
    Rust target (so `#[cfg(target_family = "unix")]` items are decided as
    in the build). Remade when the `.rs`, the cfgs or cxxbridge change
    (`cxx_stamps.json`);
  - everything a GN `action`/`action_foreach` writes (protos, the
    well-known protos' lite libraries, Blink's IDL bindings, names and
    runtime features, ukm builders, extensions API types, histogram
    variants, ...): the action itself, run from the build directory after
    the generated inputs it needs (`gen_actions.py`);
  - host tools Chromium builds from source are replaced: protoc is the
    release matching `third_party/protobuf` (33.0); perfetto's protozero and
    cppgen plugins, flatc and cxxbridge are built here from the pinned
    sources; the proto_extras plugin (`.equal.h`, `.to_value.h`,
    `.ostream.h`) is built against the pinned protobuf and abseil
    (`third_party/protobuf`'s GN source lists, compiled once with the
    host's clang++), V8's `torque` and `bytecode_builtins_list_generator`
    from their `v8/BUILD.gn` targets (with abseil and MoltBrowser's
    portable llvm-libc math), llama.cpp's `vulkan-shaders-gen` from its
    target, and the HSTS/pins preload generator
    (`net/tools/transport_security_state_generator`, for
    `transport_security_state_static*.h`) from its targets' sources and
    the pinned BoringSSL's libcrypto, with `host_shims/hsts_generator/` for
    `//base` (`host_tools.py`). Where a host tool can't be built, the
    generated stand-in below is written instead, and replaced once it can.
* `ccheck/gnconfigs.py`: the defines, include dirs and cflags of the GN
  `config()`s a file gets from its dependencies (`//ui/views:flags` for
  `TOOLKIT_VIEWS`, skia, perfetto, boringssl, webrtc, v8) and, by
  directory, from its own target (`content_implementation`, Blink's
  `inside_blink`, `features` and its plugin config), read from the real
  BUILD.gn files. Each file also gets its own GN target's `defines`,
  `include_dirs`, `cflags` (and `cflags_c`/`cflags_cc`/`cflags_objcc` by
  language) and those of the configs it adds beyond the defaults (a
  template invocation, `v8_source_set()` and the like, is expanded to the
  target it defines): opus's `opus_private_config`, for example. Its
  exception and RTTI setting comes from that target too: on when the target
  removes `//build/config/compiler:no_exceptions` (`:no_rtti`), adds
  `:exceptions` (`:rtti`) or a config of its own with `-fexceptions`
  (`-frtti`); llama.cpp's exception boundary, `llama_guard.cc`, is the
  current case.
* `ccheck/check.py`: Chromium's own clang (the `tools/clang` pin), C++23
  (C11 for C), `-fno-exceptions -fno-rtti` unless the file's target says
  otherwise, the platform's defines (Linux's and the Mac's official build,
  Windows' developer build), Chromium's clang plugins on every C++ and
  Objective-C++ file with the platform's arguments
  (`build/config/clang/BUILD.gn`, `config.plugin_flags()`): the
  chromium-style checks (find-bad-constructs; `check-ipc` on Linux) and
  the raw_ptr checks, with the raw_ptr/raw_ref field checks where the
  platform's build runs them (a non-official build: Windows'
  configuration) and everywhere with `--dev-plugins` (what a developer
  build and Chromium's CQ run; on an official configuration, Linux's and
  the Mac's, `//base`'s `RAW_PTR_EXCLUSION` compiles to nothing, so the
  fields it marks are reported there too: Windows' run is the faithful
  one); `--no-plugins` turns them off.
  `-Wall -Wextra` and the `chromium_code` warnings
  (`treat_warnings_as_errors` is off in the configs, so warnings are
  reported, not fatal; the plugins' ones in MoltBrowser's code put a file
  in the PLUGIN class).
* `ccheck/sdks.py`: the Windows SDK and the macOS headers (above).

## Stand-ins and substitutes

A diagnostic inside one of these is a harness problem, not a MoltBrowser
bug. Every generated stand-in is marked `compile_check STAND-IN` in its
first line, and `report` names it for each file that reaches it.

| What | Here | The real build |
|---|---|---|
| WebUI resource headers (`chrome/grit/*_resources{,_map}.h`, `ui/webui/resources/grit/webui_resources{,_map}.h`) | the map declaration; the `IDR_<STEM>_*` names its includers use | grit on the `.grd` that `generate_grd.py` writes from the manifests of the WebUI build's TypeScript compile, minifier, bundler and code cache (Node tools and a host V8 binary), none of which runs here |
| Apple frameworks (macOS) | `mac_stand_ins/`: the declarations used, with the SDK's names, values and signatures, each file naming its SDK headers; availability not modeled (every Mac file is "partly on stand-ins") | the macOS SDK, only distributed with Xcode (`mac_stand_ins/README.md` says why no real headers can stand in) |
| MSVC runtime headers (Windows) | Visual Studio 2017's (14.11, the newest on nuget.org), with the C++17 aligned allocation functions that 14.12's `vcruntime_new.h` added (libc++'s `<new>` takes them from vcruntime) | Visual Studio 2022's (only vcruntime's few headers are reached; libc++ replaces the STL) |
| V8's instance types (`v8/src/objects/instance-types-gen.h`) | Torque's lists (`v8_use_metagen_instance_types = false`: V8 is compiled with `-DV8_USE_METAGEN_INSTANCE_TYPES=0`) | metagen, which harvests them with libclang from ninja's compile flags |
| `//base` in the proto_extras host plugin | `host_shims/proto_extras/` (the eight functions it calls, with `//base`'s behaviour) | `//base` |
| `//base` in the HSTS/pins preload generator | `host_shims/hsts_generator/` (what the tool calls, with `//base`'s behaviour, `shim_test.cc` checks it; a C++ JSON reader with `//base`'s options in place of its Rust one) | `//base` |
| `build/config/gclient_args.gni`, `build/util/LASTCHANGE.dummy` | fixed values | written by gclient hooks |
| mojom typemaps | the union over all targets | each target's own and its deps' |
| perfetto protoc plugins | linked against Ubuntu's libprotoc 3.21 | against third_party/protobuf |

Only when their source can't be had (no tarball for the branch, or no
network to it, or a host tool that can't be built), these stand in too,
and `report` names them per file: the preload headers from their real
`.template` with placeholder data, the build date as a fixed timestamp,
another grit header (every `name` in its `.grd`, made-up numbers), a
subproject from its mirror's HEAD (pdfium, webrtc, libyuv, openscreen,
dawn), dom_distiller_js's `proto/` from the dom-distiller repository's
HEAD, libc++/libc++abi from llvm-project at the commit Chromium's clang was
built from, ICU from upstream `release-78.3`, and `icu/config.gni`,
`webrtc.gni`, `dawn_features.gni` from their Linux values in `gn.py`.

## What it can't cover

* Linking, ODR and anything past the front end (no object files).
* Android, ChromeOS, iOS and Fuchsia files (no platform modeled), and on
  the Mac anything an Apple framework stand-in declares differently from
  the SDK (`mac_stand_ins/README.md`).
* Files whose headers can't be had here are reported as "not checked",
  with the header; `gen_report.json` lists every header that stayed
  missing and why.
* An API that a stand-in repository (above) has differently from the
  pinned commit shows up as errors that a real build would not have (or
  hides one): `report` adds "reaches <repository> (...)" to such a file's
  result, for a human to judge.
* Code built only under other GN args than `configs/*.gn` (besides
  `molt_llama_vulkan`, which `linux-vulkan` models), or a flag a file gets
  only from a dependency's public config not listed in `gnconfigs.py` or
  `SCOPED_INCLUDE_DIRS`.
* GN itself: `gn check`, DEPS rules (checkdeps), visibility and target
  dependencies are not checked; a file compiles here if its includes resolve
  anywhere in the tree (with the union of every target's include dirs).
* Rust (`.rs`) and the WebUI JavaScript (see `tools/webui_checks`).
