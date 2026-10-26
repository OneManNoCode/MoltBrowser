# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Compiler flags: an approximation of what GN gives a chrome/browser source
file in the Linux x64 official build (configs/linux-x64.gn), read off
build/config/{compiler,c++,clang,linux}/BUILD.gn and each library's public
configs at the pinned revision. Update together with a Chromium rebase.
"""

# Chromium-relative include dirs, in search order. "gen:" = under the
# generated-files root (GN's $root_gen_dir).
INCLUDE_DIRS = [
    "",
    "gen:",
    "buildtools/third_party/libc++",
    "base/allocator/partition_allocator/src",
    "gen:base/allocator/partition_allocator/src",
    "third_party/perfetto/include",
    "gen:third_party/perfetto/build_config",
    "gen:third_party/perfetto",
    "third_party/abseil-cpp",
    "third_party/protobuf/src",
    "gen:protoc_out",
    "gen:third_party/omnibox_proto",
    "gen:third_party/private_membership/src",
    "gen:third_party/shell-encryption/src",
    "third_party/protobuf/third_party/utf8_range",
    "third_party/boringssl/src/include",
    "third_party/icu/source/common",
    "third_party/icu/source/i18n",
    "skia/config",
    "skia/ext",
    "third_party/skia",
    "v8/include",
    "gen:v8/include",
    "third_party/googletest/src/googletest/include",
    "third_party/googletest/src/googlemock/include",
    "third_party/re2/src",
    "third_party/khronos",
    "gpu",
    "third_party/vulkan-headers/src/include",
    "third_party/libyuv/include",
    # libwebm includes itself by its source root (media/muxers/webm_muxer.h).
    "third_party/libwebm/source",
    # So does libaom (media/video/av1_video_encoder.h).
    "third_party/libaom/source/libaom",
    # fp16.h includes <fp16/fp16.h> (media/gfx/paint_canvas_video_renderer.cc).
    "third_party/fp16/src/include",
    "third_party/webrtc",
    "gen:third_party/webrtc",
    "gen:components/policy/proto",
    "third_party/zlib",
    "third_party/jsoncpp/source/include",
    "third_party/flatbuffers/src/include",
    "third_party/brotli/include",
    "third_party/dawn/include",
    "gen:third_party/dawn/include",
    "third_party/angle/include",
    "third_party/harfbuzz/src/src",
    "third_party/highway/src",
    "third_party/openscreen/src",
    "gen:third_party/openscreen/src",
    "third_party/freetype/include",
    "third_party/freetype/src/include",
    "third_party/crashpad/crashpad",
    "net/third_party/quiche/overrides",
    "net/third_party/quiche/src/quiche/common/platform/default",
    "net/third_party/quiche/src",
    "gen:net/third_party/quiche/src",
    "third_party/leveldatabase/src/include",
    "third_party/leveldatabase/src",
    "third_party/leveldatabase",
    "third_party/tflite/src",
    "gen:third_party/tflite/src",
    "third_party/tflite_support/src",
    "gen:third_party/tflite_support/src",
    "third_party/pdfium",
    "third_party/pdfium/public",
    "third_party/fuzztest/src",
    "third_party/llama_cpp",
    "third_party/llama_cpp/include",
    "third_party/llama_cpp/ggml/include",
    # third_party/libaddressinput:libaddressinput_config
    "third_party/libaddressinput/src/cpp/include",
    # The well-known protos' lite libraries (third_party/protobuf's
    # proto_wkt(): each proto_library() adds its proto_out_dir), so
    # "google/protobuf/any.pb.h" resolves as in a build.
    "gen:third_party/protobuf/lite/any_proto/src",
    "gen:third_party/protobuf/lite/duration_proto/src",
    "gen:third_party/protobuf/lite/struct_proto/src",
    "gen:third_party/protobuf/lite/timestamp_proto/src",
]

# Include dirs only some libraries' own files and their dependents get (a
# library's internal config, or the public config of a target only it
# depends on), keyed by the library's directory. The scan searches them
# after INCLUDE_DIRS for a file under that directory; a compile adds them
# when the file's include closure reaches that directory (as the configs
# of its dependencies would). Searching them for everyone would let one
# library's "src/..." shadow another's.
_FFMPEG = ["third_party/ffmpeg",
           "third_party/ffmpeg/chromium/config/Chromium/{os}/{cpu}"]
_GLSLANG = ["third_party/glslang/src",
            "gen:third_party/glslang/src/include",
            "third_party/spirv-tools/src/include",
            "third_party/spirv-headers/src/include"]
SCOPED_INCLUDE_DIRS = {
    # v8/BUILD.gn:internal_config_base ("src/..." from //v8, the generated
    # "torque-generated/..." from $target_gen_dir).
    "v8/": ["v8", "gen:v8", "gen:v8/include",
            # v8:simdutf_include_config, through :v8_simdutf.
            "third_party/simdutf"],
    # third_party/llvm-libc:config (its "src/__support/..." includes).
    "third_party/llvm-libc/": ["third_party/llvm-libc/src"],
    # third_party/opus:opus_config and :opus_private_config.
    "third_party/opus/": ["third_party/opus/src/include",
                          "third_party/opus/src",
                          "third_party/opus/src/celt",
                          "third_party/opus/src/silk",
                          "third_party/opus/src/silk/fixed",
                          "third_party/opus/src/silk/float"],
    # third_party/llama_cpp:llama_cpp_config, and :ggml_vulkan's generated
    # shader declarations (ggml-vulkan-shaders.hpp).
    "third_party/llama_cpp/": ["third_party/llama_cpp",
                               "third_party/llama_cpp/include",
                               "third_party/llama_cpp/ggml/include",
                               "third_party/llama_cpp/ggml/src",
                               "third_party/llama_cpp/ggml/src/ggml-cpu",
                               "third_party/llama_cpp/common",
                               "third_party/llama_cpp/src",
                               "gen:third_party/llama_cpp/ggml-vulkan",
                               # //third_party/spirv-headers/src:
                               # spv_headers, a dependency of :ggml_vulkan.
                               "third_party/spirv-headers/src/include"],
    # The glslang and SPIRV-Tools targets :ggml_vk_glslc depends on
    # (glslang_public, glslang_sources_common's build_info dir,
    # spvtools_public_config).
    "third_party/llama_cpp/chromium/ggml_vk_glslc": _GLSLANG,
    "third_party/glslang/src/": _GLSLANG,
    "third_party/spirv-tools/src/": ["third_party/spirv-tools/src/include"],
    # Skia's Rust bridges (rust/exif/FFI.rs.h, src/ports/fontations/src/
    # ffi.rs.h): generated under the skia root in gen/.
    "third_party/skia/": ["gen:third_party/skia"],
    # //third_party/ffmpeg:ffmpeg_dependent_config (libavutil/...), for
    # MoltBrowser's portable-media switch.
    # (and its per-platform config dir, for libavutil/avconfig.h; the
    # Chromium branding, as configs/*.gn leave proprietary_codecs off).
    "media/base/molt_portable_media": _FFMPEG,
    "third_party/ffmpeg/": _FFMPEG,
}


# Flags given to the files under a directory to compile a configuration the
# real build reaches by a path that can't run here, and the source file
# that is the stand-in's mark (`report` names the stand-in for every file
# whose include closure reaches it). V8 harvests its instance types with
# libclang from the compile flags in ninja's files (tools/metagen, the
# default since v8_use_metagen_instance_types); Torque writes the same
# lists, which v8_use_metagen_instance_types = false selects.
STAND_IN_FLAGS = {
    "v8/": (["-DV8_USE_METAGEN_INSTANCE_TYPES=0"],
            "v8/src/objects/instance-types-gen.h",
            "V8's instance types from Torque's lists, not metagen's"),
}


def include_dirs(platform="linux"):
    """INCLUDE_DIRS for a platform (the same on every platform today: what
    differs, the system headers, comes from the sysroot or SDK)."""
    return INCLUDE_DIRS


# What "{os}" and "{cpu}" in SCOPED_INCLUDE_DIRS stand for: the platform
# being scanned and compiled (set_platform()).
PLATFORM_VARS = {"os": "linux", "cpu": "x64"}


def set_platform(plat):
    PLATFORM_VARS.update(os=plat.gn_os, cpu=plat.gn_cpu)


def expand_dir(d):
    return d.format(**PLATFORM_VARS) if "{" in d else d


def scoped_include_dirs(path):
    """SCOPED_INCLUDE_DIRS for a file at `path` (src- or gen-relative)."""
    out = []
    for prefix, dirs in SCOPED_INCLUDE_DIRS.items():
        if path.startswith(prefix):
            out += [expand_dir(d) for d in dirs]
    return out

# Under the sysroot, as -isystem: what pkg-config adds for glib, nss, dbus
# (build/config/linux/BUILD.gn and friends).
SYSROOT_DIRS = [
    "usr/include/glib-2.0",
    "usr/lib/x86_64-linux-gnu/glib-2.0/include",
    "usr/include/nss",
    "usr/include/nspr",
    "usr/include/dbus-1.0",
    "usr/lib/x86_64-linux-gnu/dbus-1.0/include",
]

# The same, as -isystem (libc++ is searched after the -I dirs).
SYSTEM_DIRS = [
    "third_party/libc++/src/include",
    "third_party/libc++abi/src/include",
]

DEFINES = [
    # build/config/BUILD.gn:feature_flags + release
    "USE_UDEV", "USE_AURA=1", "USE_GLIB=1", "USE_OZONE=1", "OFFICIAL_BUILD",
    "NDEBUG", "NVALGRIND", "DYNAMIC_ANNOTATIONS_ENABLED=0",
    # build/config/compiler + linux
    "_GNU_SOURCE", "__STDC_CONSTANT_MACROS", "__STDC_FORMAT_MACROS",
    "_FORTIFY_SOURCE=2", "_FILE_OFFSET_BITS=64", "_LARGEFILE_SOURCE",
    "_LARGEFILE64_SOURCE",
    "_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE",
    "_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS",
    "_LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS",
    "_LIBCPP_INSTRUMENTED_WITH_ASAN=0",
    "CR_LIBCXX_REVISION=compile_check",
    "GLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_56",
    "GLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_56",
    # third_party/abseil-cpp:absl_define_config
    "ABSL_ALLOCATOR_NOTHROW=1",
    # third_party/protobuf:protobuf_config
    "GOOGLE_PROTOBUF_NO_RTTI", "PROTOBUF_INTERNAL_BOUNDS_CHECK_MODE_ABORT",
    "HAVE_PTHREAD",
    # third_party/icu:icu_config
    "U_USING_ICU_NAMESPACE=0", "U_ENABLE_DYLOAD=0", "USE_CHROMIUM_ICU=1",
    "U_ENABLE_TRACING=1", "U_ENABLE_RESOURCE_TRACING=0",
    "U_STATIC_IMPLEMENTATION",
    "ICU_UTIL_DATA_IMPL=ICU_UTIL_DATA_FILE",
    # third_party/boringssl
    "OPENSSL_SMALL",
    # v8 (public config of //v8:v8_headers on x64)
    "V8_COMPRESS_POINTERS", "V8_COMPRESS_POINTERS_IN_SHARED_CAGE",
    "V8_31BIT_SMIS_ON_64BIT_ARCH", "V8_ENABLE_SANDBOX",
    "V8_EXTERNAL_CODE_SPACE", "V8_DEPRECATION_WARNINGS",
    "V8_IMMINENT_DEPRECATION_WARNINGS", "CPPGC_CAGED_HEAP",
    "CPPGC_YOUNG_GENERATION", "CPPGC_POINTER_COMPRESSION",
    "CPPGC_ENABLE_LARGER_CAGE", "CPPGC_SLIM_WRITE_BARRIER",
    # llama.cpp public config
    "GGML_USE_CPU",
    # third_party/webrtc:common_inherited_config (Linux, in Chromium)
    "WEBRTC_CHROMIUM_BUILD", "WEBRTC_POSIX", "WEBRTC_LINUX",
    # third_party/libaddressinput:libaddressinput_config
    "I18N_ADDRESS_VALIDATION_DATA_URL="
    "\"https://chromium-i18n.appspot.com/ssl-aggregate-address/\"",
]

# Headers only ever included through a macro the GN configs define
# (#include SK_REF_CNT_MIXIN_INCLUDE): the include scanner can't follow
# those, so they are fetched up front.
MACRO_INCLUDED = [
    "skia/config/",
    # libc++'s own headers include <__config_site> and
    # <__assertion_handler> from here, and the scanner does not follow them.
    "buildtools/third_party/libc++/",
]

CFLAGS = [
    "-std=c++23", "-fno-exceptions", "-fno-rtti", "-fsyntax-only",
    "-nostdinc++", "-fno-strict-aliasing", "-fno-delete-null-pointer-checks",
    "-fcomplete-member-pointers", "-fmerge-all-constants",
    "-m64", "-msse3", "-fPIC", "-pthread",
    "-ferror-limit=0", "-fno-color-diagnostics",
    # chromium_code warnings (treat_warnings_as_errors = false in
    # configs/linux-x64.gn, so these are reported, not fatal).
    "-Wall", "-Wextra", "-Wimplicit-fallthrough", "-Wextra-semi",
    "-Wunreachable-code-aggressive", "-Wthread-safety",
    "-Wno-missing-field-initializers", "-Wno-unused-parameter",
    "-Wno-psabi", "-Wno-invalid-offsetof", "-Wloop-analysis",
    "-Wno-unneeded-internal-declaration", "-Wno-cast-function-type",
    "-Wno-thread-safety-reference-return", "-Wno-nontrivial-memcall",
    "-Wno-uninitialized-const-pointer", "-Wno-unused-but-set-global",
    "-Wenum-compare-conditional", "-Wno-nullability-completeness",
    "-Wno-trigraphs", "-Wno-unused-template", "-Wheader-hygiene",
    "-Wstring-conversion", "-Wtautological-overlap-compare",
    "-Wno-gnu-anonymous-struct", "-Wno-gnu-conditional-omitted-operand",
    "-Wno-gnu-include-next", "-Wno-gnu-label-as-value",
    "-Wno-gnu-redeclared-enum", "-Wno-gnu-statement-expression",
    "-Wno-gnu-zero-variadic-macro-arguments", "-Wno-zero-length-array",
]

# build/config/clang:find_bad_constructs + raw_ptr_check (the plugins are
# built into Chromium's clang). enable_check_raw_ptr_fields and
# enable_check_raw_ref_fields are off in official builds; DEV_PLUGIN_FLAGS
# adds them (what a developer build and the Chromium CQ run).
# plugin_flags() gives each platform's set.
PLUGIN_FLAGS = [
    "-Xclang", "-add-plugin", "-Xclang", "find-bad-constructs",
    "-Xclang", "-plugin-arg-find-bad-constructs", "-Xclang",
    "check-stack-allocated",
    "-Xclang", "-plugin-arg-find-bad-constructs", "-Xclang",
    "relax-ctor-checks-for-aggregates",
    "-Xclang", "-plugin-arg-find-bad-constructs", "-Xclang", "check-ipc",
    "-Xclang", "-add-plugin", "-Xclang", "raw-ptr-plugin",
    "-Xclang", "-plugin-arg-raw-ptr-plugin", "-Xclang",
    "check-raw-ptr-to-stack-allocated",
    "-Xclang", "-plugin-arg-raw-ptr-plugin", "-Xclang",
    "disable-check-raw-ptr-to-stack-allocated-error",
    "-Xclang", "-plugin-arg-raw-ptr-plugin", "-Xclang",
    "no-special-treatment-for-oilpanized-paths",
]

DEV_PLUGIN_FLAGS = [
    "-Xclang", "-plugin-arg-raw-ptr-plugin", "-Xclang",
    "check-raw-ptr-fields",
    "-Xclang", "-plugin-arg-raw-ptr-plugin", "-Xclang", "check-span-fields",
    "-Xclang", "-plugin-arg-raw-ptr-plugin", "-Xclang",
    "check-raw-ref-fields",
]
# The paths build/config/clang:raw_ptr_check leaves out of the field
# checks.
RAW_PTR_EXCLUDE_PATHS = ("third_party/dawn/",
                         "chrome/test/fuzzing/webidl_fuzzing/")
# Platforms whose configs/*.gn is not an official build: their build runs
# the field checks (clang.gni: _raw_ptr_plugin_default).
NON_OFFICIAL = ("win",)


def plugin_flags(platform, root, dev=False):
    """The plugins' flags as build/config/clang/BUILD.gn gives them for the
    platform (check-ipc only on Linux; the raw_ptr/raw_ref field checks in
    a non-official build, or with `dev`). root: the tree, for the excluded
    paths."""
    out = list(PLUGIN_FLAGS)
    if not platform.startswith("linux"):
        i = out.index("check-ipc")
        del out[i - 3:i + 1]
    if dev or platform in NON_OFFICIAL:
        out += DEV_PLUGIN_FLAGS
        for p in RAW_PTR_EXCLUDE_PATHS:
            out += ["-Xclang", "-plugin-arg-raw-ptr-plugin", "-Xclang",
                    "raw-ptr-exclude-path=%s/%s" % (root, p)]
    return out


# -- Other platforms and languages (platforms.py) --------------------------
#
# DEFINES and CFLAGS above are Linux x64's (C++). The other platforms'
# lists below are read off the same BUILD.gn files under their own args
# (configs/windows-x64.gn: not an official build, so DCHECKs are on;
# configs/macos-arm64.gn: official). C files get -std=c11 and no C++-only
# flags; Objective-C++ gets ARC, as every Mac target does.

# The defines every platform's build has (feature_flags, release,
# chromium_code, c++, and the libraries' public configs).
_LIB_DEFINES = [d for d in DEFINES if d.startswith((
    "ABSL_", "GOOGLE_PROTOBUF", "PROTOBUF_", "HAVE_PTHREAD", "U_", "USE_CHROMIUM_ICU",
    "ICU_", "OPENSSL_", "V8_", "CPPGC_", "GGML_", "I18N_",
    "_LIBCPP_", "_LIBCXXABI_", "CR_LIBCXX"))]

WIN_DEFINES = [
    # build/config/BUILD.gn:feature_flags + release (not official: DCHECKs
    # on, build/config/dcheck_always_on.gni)
    "NDEBUG", "NVALGRIND", "DYNAMIC_ANNOTATIONS_ENABLED=0",
    "DCHECK_ALWAYS_ON=1", "USE_AURA=1",
    # build/config/win:runtime_library, :winver, :unicode, :lean_and_mean,
    # :nominmax, :compiler
    "__STD_C", "_CRT_RAND_S", "_CRT_SECURE_NO_DEPRECATE",
    "_SCL_SECURE_NO_DEPRECATE", "_ATL_NO_OPENGL", "_WINDOWS",
    "CERT_CHAIN_PARA_HAS_EXTRA_FIELDS", "PSAPI_VERSION=2", "WIN32",
    "_SECURE_ATL", "WINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP",
    "NTDDI_VERSION=NTDDI_WIN11_BR", "_WIN32_WINNT=0x0A00", "WINVER=0x0A00",
    "_UNICODE", "UNICODE", "WIN32_LEAN_AND_MEAN", "NOMINMAX",
    "__WRL_ENABLE_FUNCTION_STATICS__",
    # build/config/compiler:chromium_code
    "__STDC_CONSTANT_MACROS", "__STDC_FORMAT_MACROS",
    # third_party/webrtc:common_inherited_config (Windows, in Chromium)
    "WEBRTC_CHROMIUM_BUILD", "WEBRTC_WIN",
] + [d for d in _LIB_DEFINES if d not in (
    "_LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS", "HAVE_PTHREAD")]

MAC_DEFINES = [
    "OFFICIAL_BUILD", "NDEBUG", "NVALGRIND", "DYNAMIC_ANNOTATIONS_ENABLED=0",
    # build/config/mac:compiler (Xcode's build number; read by nothing)
    "CR_XCODE_BUILD=compile_check",
    "__STDC_CONSTANT_MACROS", "__STDC_FORMAT_MACROS", "_FORTIFY_SOURCE=2",
    "_FILE_OFFSET_BITS=64", "_LARGEFILE_SOURCE", "_LARGEFILE64_SOURCE",
    "WEBRTC_CHROMIUM_BUILD", "WEBRTC_POSIX", "WEBRTC_MAC",
] + _LIB_DEFINES

# The Mac build's minimum system (build/config/mac/mac_sdk.gni).
MAC_DEPLOYMENT_TARGET = "13.0"

# C++-only flags of CFLAGS (dropped for C and Objective-C).
_CXX_ONLY = ("-std=c++23", "-fno-exceptions", "-fno-rtti", "-nostdinc++",
             "-fcomplete-member-pointers", "-Wheader-hygiene",
             "-Wextra-semi", "-Wno-invalid-offsetof")
# Linux x64-only flags of CFLAGS.
_X64_ONLY = ("-m64", "-msse3")


def _c(flags):
    return ["-std=c11"] + [f for f in flags if f not in _CXX_ONLY]


# Objective-C(++) on the Mac: build/config/compiler:enable_arc and the
# chromium_code warnings for Objective-C.
OBJC_FLAGS = ["-fobjc-arc", "-fno-objc-arc-exceptions",
              "-Wobjc-property-assign-on-object-type",
              "-Wimplicit-retain-self", "-Wobjc-missing-property-synthesis"]

# clang-cl's equivalents of CFLAGS (build/config/win:compiler and
# build/config/compiler on Windows), /Zs for a syntax-only run.
WIN_CFLAGS = [
    "/Zs", "/Gy", "/FS", "/bigobj", "/utf-8", "/Zc:twoPhase",
    "-fmsc-version=1934", "-m64", "-msse3", "/Zc:sizedDealloc-",
    "/Zc:inline", "-fcomplete-member-pointers", "-ferror-limit=0",
    "-fno-color-diagnostics", "-fno-delete-null-pointer-checks",
    "/W4", "-Wimplicit-fallthrough", "-Wextra-semi",
    "-Wunreachable-code-aggressive", "-Wthread-safety",
    "-Wno-missing-field-initializers", "-Wno-unused-parameter",
    "-Wno-invalid-offsetof", "-Wloop-analysis",
    "-Wno-unneeded-internal-declaration", "-Wno-cast-function-type",
    "-Wno-thread-safety-reference-return", "-Wno-nontrivial-memcall",
    "-Wno-uninitialized-const-pointer", "-Wno-unused-but-set-global",
    "-Wenum-compare-conditional", "-Wno-nullability-completeness",
    "-Wno-trigraphs", "-Wno-unused-template", "-Wheader-hygiene",
    "-Wstring-conversion", "-Wtautological-overlap-compare",
    # build/config/compiler:default_warnings (clang-cl)
    "/wd4091", "/wd4127", "/wd4251", "/wd4275", "/wd4312", "/wd4324",
    "/wd4351", "/wd4355", "/wd4503", "/wd4589", "/wd4611", "/wd4100",
    "/wd4121", "/wd4244", "/wd4505", "/wd4510", "/wd4512", "/wd4610",
    "/wd4838", "/wd4995", "/wd4996", "/wd4456", "/wd4457", "/wd4458",
    "/wd4459", "/wd4200", "/wd4201", "/wd4204", "/wd4221", "/wd4245",
    "/wd4267", "/wd4305", "/wd4389", "/wd4702", "/wd4701", "/wd4703",
    "/wd4661", "/wd4706", "/wd4715", "/wd4800",
    "-Wno-microsoft-cast", "-Wno-microsoft-template-shadow",
    "-Wno-language-extension-token",
]
WIN_CFLAGS_CC = ["/std:c++23preview", "/GR-"]
WIN_CFLAGS_C = ["/std:c11", "/TC"]


def platform_cflags(plat, lang):
    """(driver, flags, defines) for a platform and language. driver:
    "clang" (GCC-style, clang or clang++) or "clang-cl"."""
    if plat == "win":
        extra = WIN_CFLAGS_C if lang == "c" else WIN_CFLAGS_CC
        return "clang-cl", WIN_CFLAGS + extra, WIN_DEFINES
    if plat == "mac":
        base = [f for f in CFLAGS if f not in _X64_ONLY + ("-pthread",)]
        base += ["--target=arm64-apple-macos",
                 "-mmacos-version-min=" + MAC_DEPLOYMENT_TARGET]
        if lang in ("c", "objective-c"):
            base = _c(base)
        if lang in ("objective-c", "objective-c++"):
            base += OBJC_FLAGS
        return "clang", base, MAC_DEFINES
    if plat == "linux-arm64":
        base = [f for f in CFLAGS if f not in _X64_ONLY]
        base += ["--target=aarch64-linux-gnu"]
        if lang == "c":
            base = _c(base)
        return "clang", base, DEFINES
    if lang == "c":
        return "clang", _c(CFLAGS), DEFINES
    return "clang", list(CFLAGS), DEFINES
