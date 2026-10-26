# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The platforms a file is checked for, and its language.

Every platform has its own build directory (out/cc for Linux x64,
out/cc-<name> for the others) and its own state in the work dir (the
generated headers, the mojom and grit stamps, the include graph), because a
build generates them from that platform's GN args: buildflag headers,
mojom bindings with [EnableIf=is_win] fields, grit headers with <if
expr="is_macosx">. The checkout, the subprojects and the toolchain are
shared.

  linux        Linux x64, configs/linux-x64.gn, the Debian sysroot (the
               default; what the check always did).
  linux-arm64  Linux arm64 (configs/linux-x64.gn with target_cpu =
               "arm64"), the Debian arm64 sysroot: the arm-only sources
               (NEON) a Mac arm64 build compiles too.
  win          Windows x64, configs/windows-x64.gn, clang-cl against the
               Windows SDK (winsdk.py).
  mac          macOS arm64, configs/macos-arm64.gn, against Darwin's libc
               headers and stand-ins for the Apple frameworks (apple_sdk.py).
  linux-vulkan Linux x64 with the GN switch molt_llama_vulkan = true
               (llama_cpp.gni; off in configs/): llama.cpp's Vulkan backend,
               its glue and host tools, and the code under
               BUILDFLAG(MOLT_LLAMA_VULKAN) / BUILDFLAG(MOLT_GPU_INFERENCE),
               which no default build compiles.

A file's platform comes from its name and directory, the way Chromium's
GN files pick sources (*_win.cc, win/, *_mac.mm, mac/, arm/ ...).
"""
import re

from . import config


class Platform:
    def __init__(self, name, label, args_file, out_rel, gn_os, gn_cpu,
                 toolchain, true_atoms, false_atoms, extra_args=None):
        self.name = name
        self.label = label
        self.args_file = args_file
        self.out_rel = out_rel
        self.gn_os = gn_os
        self.gn_cpu = gn_cpu
        self.toolchain = toolchain
        self.true_atoms = frozenset(true_atoms)
        self.false_atoms = frozenset(false_atoms)
        self.extra_args = dict(extra_args or {})

    @property
    def mojom_features(self):
        """The features mojom.gni enables for every mojom() target."""
        return {"linux": ("is_posix", "is_linux"), "win": ("is_win",),
                "mac": ("is_posix", "is_mac", "is_apple")}[self.gn_os]

    @property
    def out(self):
        """The build directory as GN writes it (//out/cc...)."""
        return "//" + self.out_rel

    def state_dir(self, work):
        """Where this platform's generator state and include graph live."""
        return work if self.name == "linux" else work / ("platform-" +
                                                         self.name)

    def __repr__(self):
        return "<Platform %s>" % self.name


_OS_FLAGS = ("IS_WIN", "IS_MAC", "IS_APPLE", "IS_IOS", "IS_ANDROID",
             "IS_CHROMEOS", "IS_FUCHSIA", "IS_LINUX", "IS_POSIX")


def _buildflags(true_flags):
    """BUILDFLAG(IS_*) atoms: (true, false) for the OS flags."""
    t = {"BUILDFLAG(%s)" % f for f in true_flags}
    f = {"BUILDFLAG(%s)" % f for f in _OS_FLAGS if f not in true_flags}
    pa_t = {"PA_BUILDFLAG(%s)" % f for f in true_flags}
    pa_f = {"PA_BUILDFLAG(%s)" % f for f in _OS_FLAGS
            if f not in true_flags}
    return t | pa_t, f | pa_f


def _atoms(os_true, true_extra, false_extra):
    t, f = _buildflags(os_true)
    return t | set(true_extra), f | set(false_extra)


# Certainly-false/true atoms of the Linux x64 scan (scan.py's original
# lists) and their counterparts on the other platforms.
_COMMON_FALSE = {
    "BUILDFLAG(IS_IOS)", "BUILDFLAG(IS_ANDROID)", "BUILDFLAG(IS_CHROMEOS)",
    "BUILDFLAG(IS_FUCHSIA)", "BUILDFLAG(IS_NACL)", "BUILDFLAG(IS_IOS_TVOS)",
    "BUILDFLAG(IS_CHROMEOS_ASH)", "BUILDFLAG(IS_CHROMEOS_LACROS)",
    "BUILDFLAG(IS_CHROMEOS_DEVICE)", "BUILDFLAG(IS_ANDROID_DESKTOP)",
    "BUILDFLAG(IS_AIX)", "BUILDFLAG(IS_ZOS)", "BUILDFLAG(IS_ASMJS)",
    "BUILDFLAG(IS_FREEBSD)", "BUILDFLAG(IS_OPENBSD)", "BUILDFLAG(IS_SOLARIS)",
    "BUILDFLAG(IS_QNX)", "BUILDFLAG(IS_NETBSD)", "BUILDFLAG(IS_BSD)",
    "BUILDFLAG(IS_IOS_MACCATALYST)", "BUILDFLAG(IS_WATCHOS)",
    "BUILDFLAG(IS_CASTOS)", "BUILDFLAG(IS_CAST_ANDROID)",
    "BUILDFLAG(IS_COBALT)", "BUILDFLAG(IS_STARBOARD)",
    "BUILDFLAG(IS_OZONE_ANDROID)",
    "defined(OS_ANDROID)", "defined(OS_IOS)", "defined(OS_FUCHSIA)",
    "defined(OS_CHROMEOS)", "defined(__ANDROID__)", "defined(ANDROID)",
    "defined(__Fuchsia__)", "defined(__EMSCRIPTEN__)", "defined(__CYGWIN__)",
    "defined(__FreeBSD__)", "defined(__OpenBSD__)", "defined(__NetBSD__)",
    "defined(__wasm__)", "defined(__native_client__)", "defined(_AIX)",
    "defined(__sun)", "defined(__HAIKU__)", "defined(__QNX__)",
    "defined(__riscv)", "defined(__powerpc__)", "defined(__mips__)",
    "defined(__s390x__)", "defined(__loongarch__)", "defined(__ppc64__)",
    "defined(__hexagon__)", "defined(_M_ARM)", "defined(__arm__)",
    "defined(ARCH_CPU_ARMEL)",
    "PA_BUILDFLAG(IS_CAST_ANDROID)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_NACL)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_QNX)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_IOS)",
    "BUILDFLAG(USE_BLINK_V8_BINDING_TRACE)",
    # config.STAND_IN_FLAGS: V8 is checked on Torque's instance types.
    "V8_USE_METAGEN_INSTANCE_TYPES",
    "0", "false",
}
_COMMON_TRUE = {"defined(__clang__)", "defined(__cplusplus)", "1", "true"}
_X64_TRUE = {"defined(__x86_64__)"}
_X64_FALSE = {"defined(__aarch64__)", "defined(ARCH_CPU_ARM64)",
              "defined(_M_ARM64)", "PA_BUILDFLAG(PA_ARCH_CPU_ARM64)"}
_ARM64_TRUE = {"defined(__aarch64__)", "defined(ARCH_CPU_ARM64)",
               "PA_BUILDFLAG(PA_ARCH_CPU_ARM64)"}
_ARM64_FALSE = {"defined(__x86_64__)", "defined(_M_X64)",
                "defined(_M_ARM64)"}
_POSIX_NOT_APPLE_FALSE = {
    "defined(OS_WIN)", "defined(OS_MAC)", "defined(_WIN32)",
    "defined(_WIN64)", "defined(WIN32)", "defined(_MSC_VER)",
    "defined(__APPLE__)", "defined(__MINGW32__)", "defined(__MACH__)",
    "defined(__OBJC__)", "defined(GTEST_OS_WINDOWS)",
    "defined(PERFETTO_OS_WIN)", "BUILDFLAG(PA_IS_WIN)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_MAC)",
}
_LINUX_TRUE = {
    "defined(__linux__)", "defined(__linux)", "defined(OS_LINUX)",
    "defined(OS_POSIX)", "defined(__GNUC__)", "BUILDFLAG(USE_OZONE)",
    "defined(USE_OZONE)", "defined(USE_AURA)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX)",
}

# What the GN switch molt_llama_vulkan (off in every configs/*.gn) turns on.
_VULKAN = {"BUILDFLAG(MOLT_LLAMA_VULKAN)", "BUILDFLAG(MOLT_GPU_INFERENCE)",
           "defined(MOLT_LLAMA_VULKAN)", "defined(GGML_VULKAN_MOLTBROWSER)"}

_linux_t, _linux_f = _atoms(("IS_LINUX", "IS_POSIX"), (), ())
_win_t, _win_f = _atoms(("IS_WIN",), (), ())
_mac_t, _mac_f = _atoms(("IS_MAC", "IS_APPLE", "IS_POSIX"), (), ())

PLATFORMS = {
    "linux": Platform(
        "linux", "Linux x64", "configs/linux-x64.gn", "out/cc", "linux",
        "x64", "//build/toolchain/linux:clang_x64",
        _linux_t | _COMMON_TRUE | _X64_TRUE | _LINUX_TRUE,
        _linux_f | _COMMON_FALSE | _X64_FALSE | _POSIX_NOT_APPLE_FALSE |
        _VULKAN),
    "linux-vulkan": Platform(
        "linux-vulkan", "Linux x64 with molt_llama_vulkan",
        "configs/linux-x64.gn", "out/cc-vulkan", "linux", "x64",
        "//build/toolchain/linux:clang_x64",
        _linux_t | _COMMON_TRUE | _X64_TRUE | _LINUX_TRUE | _VULKAN,
        _linux_f | _COMMON_FALSE | _X64_FALSE | _POSIX_NOT_APPLE_FALSE,
        extra_args={"molt_llama_vulkan": True}),
    "linux-arm64": Platform(
        "linux-arm64", "Linux arm64", "configs/linux-x64.gn",
        "out/cc-linux-arm64", "linux", "arm64",
        "//build/toolchain/linux:clang_arm64",
        _linux_t | _COMMON_TRUE | _ARM64_TRUE | _LINUX_TRUE,
        _linux_f | _COMMON_FALSE | _ARM64_FALSE | _POSIX_NOT_APPLE_FALSE |
        _VULKAN,
        extra_args={"target_cpu": "arm64"}),
    "win": Platform(
        "win", "Windows x64", "configs/windows-x64.gn", "out/cc-win", "win",
        "x64", "//build/toolchain/win:win_clang_x64",
        _win_t | _COMMON_TRUE | _X64_TRUE | {
            "defined(OS_WIN)", "defined(_WIN32)", "defined(_WIN64)",
            "defined(WIN32)", "defined(_MSC_VER)", "BUILDFLAG(PA_IS_WIN)",
            "defined(GTEST_OS_WINDOWS)", "defined(PERFETTO_OS_WIN)",
            "PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)"},
        _win_f | _COMMON_FALSE | _X64_FALSE | {
            "defined(__linux__)", "defined(__linux)", "defined(OS_LINUX)",
            "defined(OS_POSIX)", "defined(OS_MAC)", "defined(__APPLE__)",
            "defined(__MACH__)", "defined(__OBJC__)", "defined(__MINGW32__)",
            "PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX)",
            "PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)",
            "PERFETTO_BUILDFLAG(PERFETTO_OS_MAC)"} | _VULKAN),
    "mac": Platform(
        "mac", "macOS arm64", "configs/macos-arm64.gn", "out/cc-mac", "mac",
        "arm64", "//build/toolchain/mac:clang_arm64",
        _mac_t | _COMMON_TRUE | _ARM64_TRUE | {
            "defined(OS_MAC)", "defined(OS_POSIX)", "defined(__APPLE__)",
            "defined(__MACH__)", "PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)",
            "PERFETTO_BUILDFLAG(PERFETTO_OS_MAC)"},
        _mac_f | _COMMON_FALSE | _ARM64_FALSE | {
            "defined(__linux__)", "defined(__linux)", "defined(OS_LINUX)",
            "defined(OS_WIN)", "defined(_WIN32)", "defined(_WIN64)",
            "defined(WIN32)", "defined(_MSC_VER)", "defined(__MINGW32__)",
            "defined(GTEST_OS_WINDOWS)", "defined(PERFETTO_OS_WIN)",
            "BUILDFLAG(PA_IS_WIN)", "PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)",
            "PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX)",
            "BUILDFLAG(USE_OZONE)", "defined(USE_OZONE)",
            "defined(USE_AURA)"} | _VULKAN),
}

DEFAULT = "linux"

LANG_BY_EXT = {".c": "c", ".cc": "c++", ".cpp": "c++", ".cxx": "c++",
               ".mm": "objective-c++", ".m": "objective-c",
               ".h": "c++"}

# Name and directory rules, as Chromium's BUILD.gn files apply them
# (build/config/BUILDCONFIG.gn's sources_assignment_filter is gone, but the
# conventions stand). First match wins.
_RULES = [
    # llama.cpp's Vulkan backend glue and host tools, and their test: only
    # with molt_llama_vulkan.
    (re.compile(r"^third_party/llama_cpp/chromium/ggml[-_]v(ulkan|k)|"
                r"^chrome/services/molt_ai/inference/vulkan_"),
     "linux-vulkan"),
    (re.compile(r"(^|/)(ios|android|chromeos|fuchsia|ash)/|"
                r"_(ios|android|chromeos|fuchsia|ash)"
                r"(_unittest|_browsertest|_test)?\.(cc|mm|cpp|c|m|h)$"), None),
    (re.compile(r"\.(mm|m)$|(^|/)(mac|apple|cocoa|remote_cocoa)/|"
                r"_(mac|apple)"
                r"(_unittest|_browsertest|_test)?\.(cc|cpp|c|h)$"), "mac"),
    (re.compile(r"(^|/)win/|_win(_unittest|_browsertest|_test)?"
                r"\.(cc|cpp|c|h)$"), "win"),
    # Arm-only sources (NEON): Mac arm64 builds them, Linux x64 doesn't.
    (re.compile(r"(^|/)(arm|arm64|aarch64|neon)/.*\.c$|_neon(_intr)?\.c$"),
     "linux-arm64"),
]


# Files the same conventions keep off some platforms although they are
# checked on Linux: POSIX-only and Linux-only sources.
_TEST = r"(_unittest|_browsertest|_test)?\.(cc|cpp|c|h)$"
_LINUX = {"linux", "linux-arm64", "linux-vulkan"}
_ONLY_ON = [
    (re.compile(r"(^|/)linux/|_linux" + _TEST), _LINUX),
    (re.compile(r"(^|/)posix/|_posix" + _TEST), _LINUX | {"mac"}),
]


def of(path):
    """The platform a file is checked for, or None if none is modeled
    (Android, ChromeOS, iOS, Fuchsia)."""
    for rx, plat in _RULES:
        if rx.search(path):
            return plat
    return DEFAULT


def may_build(path, name):
    """Whether platform `name` can build `path` by Chromium's naming
    conventions (a *_win.cc file only Windows, a *_posix.cc file not
    Windows, ...)."""
    own = of(path)
    if own is None:
        return False
    if own != DEFAULT:
        # Named for one platform (Arm-only sources: any arm64 build).
        return name == own or (own == "linux-arm64" and name == "mac")
    for rx, names in _ONLY_ON:
        if rx.search(path):
            return name in names
    return True


def language(path):
    """A file's language; a Mac header's is Objective-C++ (what includes
    it on the Mac is mostly Objective-C++, and C++ is its subset)."""
    if path.endswith(".h") and of(path) == "mac":
        return "objective-c++"
    for ext, lang in LANG_BY_EXT.items():
        if path.endswith(ext):
            return lang
    return "c++"


def not_modeled(path):
    """Why a file has no platform here (for the report)."""
    m = re.search(r"(ios|android|chromeos|fuchsia|ash)", path)
    return "built for %s only, which the check does not model" % (
        m.group(1) if m else "another platform")


def get(name):
    return PLATFORMS[name]


def all_names():
    return list(PLATFORMS)


def flags_kind(plat):
    """The flag set config.py has for a platform."""
    return config.PLATFORM_FLAGS[plat.name]
