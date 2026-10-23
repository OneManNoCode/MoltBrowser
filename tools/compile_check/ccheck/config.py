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
]

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
