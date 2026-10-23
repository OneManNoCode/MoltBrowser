# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Include-closure scanner.

Finds every header the target files can reach, over-approximating the
preprocessor: a conditional branch is skipped only when its condition is
certainly false on Linux (BUILDFLAG(IS_WIN), defined(__APPLE__), #if 0, ...);
every other branch is followed. Headers found upstream are fetched level by
level, in big batches. Headers found nowhere are reported, split into the
ones a build generates (mojom, buildflags, protos, grit, ...) and the rest
(system headers, or real misses).
"""
import re
from pathlib import Path

from .sources import log

INCLUDE_RE = re.compile(
    rb'^[ \t]*#[ \t]*(include|import|include_next)[ \t]*([<"])([^>"]+)[>"]')
COND_RE = re.compile(rb'^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif|'
                     rb'elifdef|elifndef)\b(.*)$')

# Atoms that are certainly false / true for a Linux desktop build.
FALSE_ATOMS = {
    "BUILDFLAG(IS_WIN)", "BUILDFLAG(IS_MAC)", "BUILDFLAG(IS_APPLE)",
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
    "BUILDFLAG(IS_CHROMEOS_LACROS)", "BUILDFLAG(IS_OZONE_ANDROID)",
    "defined(OS_WIN)", "defined(OS_MAC)", "defined(OS_ANDROID)",
    "defined(OS_IOS)", "defined(OS_FUCHSIA)", "defined(OS_CHROMEOS)",
    "defined(_WIN32)", "defined(_WIN64)", "defined(WIN32)",
    "defined(_MSC_VER)", "defined(__APPLE__)", "defined(__ANDROID__)",
    "defined(ANDROID)", "defined(__Fuchsia__)", "defined(__EMSCRIPTEN__)",
    "defined(__MINGW32__)", "defined(__CYGWIN__)", "defined(__FreeBSD__)",
    "defined(__OpenBSD__)", "defined(__NetBSD__)", "defined(__wasm__)",
    "defined(__MACH__)", "defined(__native_client__)", "defined(_AIX)",
    "defined(__sun)", "defined(__HAIKU__)", "defined(__QNX__)",
    "defined(__OBJC__)", "defined(__aarch64__)", "defined(__arm__)",
    "defined(ARCH_CPU_ARM64)", "defined(ARCH_CPU_ARMEL)",
    "defined(__riscv)", "defined(__powerpc__)", "defined(__mips__)",
    "defined(__s390x__)", "defined(__loongarch__)", "defined(__ppc64__)",
    "defined(_M_ARM64)", "defined(_M_ARM)", "defined(__hexagon__)",
    "defined(GTEST_OS_WINDOWS)", "defined(PERFETTO_OS_WIN)",
    "BUILDFLAG(PA_IS_WIN)", "PA_BUILDFLAG(IS_WIN)", "PA_BUILDFLAG(IS_APPLE)",
    "PA_BUILDFLAG(IS_ANDROID)", "PA_BUILDFLAG(IS_MAC)", "PA_BUILDFLAG(IS_IOS)",
    "PA_BUILDFLAG(IS_FUCHSIA)", "PA_BUILDFLAG(IS_CHROMEOS)",
    "PA_BUILDFLAG(PA_ARCH_CPU_ARM64)", "PA_BUILDFLAG(IS_CAST_ANDROID)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_MAC)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_IOS)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_NACL)",
    "PERFETTO_BUILDFLAG(PERFETTO_OS_QNX)",
    "BUILDFLAG(USE_BLINK_V8_BINDING_TRACE)",
    "0", "false",
}
TRUE_ATOMS = {
    "BUILDFLAG(IS_LINUX)", "BUILDFLAG(IS_POSIX)", "defined(__linux__)",
    "defined(__linux)", "defined(OS_LINUX)", "defined(OS_POSIX)",
    "defined(__clang__)", "defined(__x86_64__)", "defined(__GNUC__)",
    "BUILDFLAG(USE_OZONE)", "defined(USE_OZONE)", "defined(USE_AURA)",
    "defined(__cplusplus)", "1", "true", "PA_BUILDFLAG(IS_LINUX)",
    "PA_BUILDFLAG(IS_POSIX)", "PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX)",
}

_TOK_RE = re.compile(r"\s*(defined\s*\(\s*\w+\s*\)|defined\s+\w+|"
                     r"\w+\s*\(\s*\w+\s*\)|&&|\|\||!|\(|\)|[^\s()!&|]+)")


def _atom(tok):
    tok = re.sub(r"\s+", "", tok)
    m = re.match(r"defined(\w+)$", tok)
    if m:
        tok = "defined(%s)" % m.group(1)
    if tok in FALSE_ATOMS:
        return False
    if tok in TRUE_ATOMS:
        return True
    return None


def eval_cond(expr):
    """Three-valued evaluation: True, False or None (unknown)."""
    expr = re.sub(r"/\*.*?\*/", " ", expr)
    expr = expr.split("//", 1)[0].strip()
    toks = [t.strip() for t in _TOK_RE.findall(expr) if t.strip()]
    pos = [0]

    def peek():
        return toks[pos[0]] if pos[0] < len(toks) else None

    def take():
        t = peek()
        pos[0] += 1
        return t

    def primary():
        t = take()
        if t is None:
            return None
        if t == "!":
            v = primary()
            return None if v is None else (not v)
        if t == "(":
            v = orexpr()
            if peek() == ")":
                take()
            return v
        # Comparisons and arithmetic: unknown.
        v = _atom(t)
        while peek() not in (None, "&&", "||", ")"):
            take()
            v = None
        return v

    def andexpr():
        v = primary()
        while peek() == "&&":
            take()
            w = primary()
            if v is False or w is False:
                v = False
            elif v is True and w is True:
                v = True
            else:
                v = None
        return v

    def orexpr():
        v = andexpr()
        while peek() == "||":
            take()
            w = andexpr()
            if v is True or w is True:
                v = True
            elif v is False and w is False:
                v = False
            else:
                v = None
        return v

    try:
        return orexpr()
    except Exception:  # noqa: BLE001 - unknown syntax: follow the branch
        return None


def extract_includes(data):
    """[(kind, name, is_angle)] for includes in live (not certainly-false)
    branches."""
    out = []
    # Join continuation lines.
    data = data.replace(b"\\\r\n", b"").replace(b"\\\n", b"")
    # stack entries: [active_parent, taken_any_certain, this_branch_active]
    stack = []
    active = True
    for line in data.split(b"\n"):
        s = line.lstrip()
        if not s.startswith(b"#"):
            continue
        m = COND_RE.match(line)
        if m:
            d = m.group(1).decode()
            rest = m.group(2).decode("latin-1").strip()
            if d in ("if", "ifdef", "ifndef"):
                if d == "if":
                    v = eval_cond(rest)
                elif d == "ifdef":
                    v = _atom("defined(%s)" % rest.split()[0]) if rest else None
                else:
                    a = _atom("defined(%s)" % rest.split()[0]) if rest else None
                    v = None if a is None else (not a)
                stack.append([active, v is True, v is not False])
                active = active and v is not False
            elif d in ("elif", "elifdef", "elifndef"):
                if not stack:
                    continue
                top = stack[-1]
                if d == "elif":
                    v = eval_cond(rest)
                else:
                    v = None
                if top[1]:
                    top[2] = False
                else:
                    top[2] = v is not False
                    if v is True:
                        top[1] = True
                active = top[0] and top[2]
            elif d == "else":
                if not stack:
                    continue
                top = stack[-1]
                top[2] = not top[1]
                active = top[0] and top[2]
            elif d == "endif":
                if stack:
                    active = stack.pop()[0]
            continue
        if not active:
            continue
        m = INCLUDE_RE.match(line)
        if m:
            out.append((m.group(1).decode(), m.group(3).decode("latin-1"),
                        m.group(2) == b"<"))
    return out


# Generated-header classification (by name, for headers found nowhere).
GEN_PATTERNS = [
    ("mojom", re.compile(r"\.mojom(-[a-z-]+)?\.h$")),
    ("proto", re.compile(r"\.pb\.h$")),
    ("pbzero", re.compile(r"\.(pbzero|gen|ipc)\.h$")),
    ("grit", re.compile(r"(^|/)grit/[^/]+\.h$")),
    ("buildflags", re.compile(r"(buildflags?|_features|features)\.h$")),
    ("vector_icons", re.compile(r"vector_icons\.h$")),
]

SYSTEM_PREFIXES = (
    "sys/", "linux/", "bits/", "asm/", "netinet/", "arpa/", "net/if", "glib",
    "gio/", "gtk/", "gdk/", "X11/", "xcb/", "wayland-", "dbus/dbus.h",
    "nss", "nspr", "pk11", "cert.h", "secmod", "keyhi", "ssl.h", "pango",
    "cairo", "fontconfig/", "ft2build.h", "freetype/", "alsa/", "pulse/",
    "cups/", "security/", "libudev.h", "drm", "xf86", "gbm.h", "EGL/",
    "GL/", "GLES", "KHR/", "va/", "valgrind/", "sanitizer/", "execinfo.h",
    "pthread", "sched.h", "unistd.h", "fcntl.h", "dlfcn.h", "link.h",
    "elf.h", "poll.h", "signal.h", "spawn.h", "termios.h", "ucontext.h",
    "malloc.h", "features.h", "endian.h", "byteswap.h", "syscall.h",
    "semaphore.h", "netdb.h", "ifaddrs.h", "resolv.h", "pwd.h", "grp.h",
    "dirent.h", "fnmatch.h", "glob.h", "libgen.h", "langinfo.h", "iconv.h",
    "mntent.h", "utmp.h", "paths.h", "err.h", "getopt.h", "error.h",
    "wchar.h", "wctype.h", "xlocale.h", "cpuid.h", "immintrin.h",
    "emmintrin.h", "xmmintrin.h", "smmintrin.h", "tmmintrin.h", "nmmintrin.h",
    "pmmintrin.h", "x86intrin.h", "wmmintrin.h", "arm_neon.h", "intrin.h",
    "mm_malloc.h", "unwind.h", "cxxabi.h", "zlib.h", "gnu/", "scsi/",
    "mtd/", "rdma/", "sound/", "video/", "xen/", "regex.h", "libintl.h",
    "printf.h", "ulimit.h", "utime.h", "stdatomic.h", "threads.h", "uchar.h",
)

STD_NAMES = re.compile(r"^[a-z_0-9]+$|^__|^experimental/|^ext/|^tr1/|"
                       r"^(assert|ctype|errno|fenv|float|inttypes|iso646|"
                       r"limits|locale|math|setjmp|signal|stdalign|stdarg|"
                       r"stdbool|stddef|stdint|stdio|stdlib|string|strings|"
                       r"tgmath|time|alloca|memory|stdnoreturn|"
                       r"complex)\.h$")


def classify_missing(name):
    for kind, rx in GEN_PATTERNS:
        if rx.search(name):
            return kind
    if STD_NAMES.match(name) or name.startswith(SYSTEM_PREFIXES):
        return "system"
    return "other"


class Scanner:
    def __init__(self, tree, include_dirs, gen_dir):
        """include_dirs: Chromium-relative dirs ("" = src root), in order.
        gen_dir: the generated-files root (searched like include dirs,
        under the "gen/" prefix)."""
        self.tree = tree
        self.include_dirs = include_dirs
        self.gen_dir = Path(gen_dir)
        self.missing = {}      # name -> set(includer)
        self.resolved = {}     # file -> [resolved paths]

    def _gen_exists(self, rel):
        return (self.gen_dir / rel).is_file()

    def resolve(self, name, includer, angle):
        """A key for the file: 'src:<path>' (overlay/upstream) or
        'gen:<path>', or None."""
        cands = []
        if not angle and includer is not None:
            kind, path = includer
            d = path.rsplit("/", 1)[0] if "/" in path else ""
            cands.append((kind, (d + "/" + name) if d else name))
        for inc in self.include_dirs:
            if inc.startswith("gen:"):
                g = inc[4:]
                cands.append(("gen", (g + "/" + name) if g else name))
            else:
                cands.append(("src", (inc + "/" + name) if inc else name))
        for kind, p in cands:
            p = _normpath(p)
            if p is None:
                continue
            if kind == "gen":
                if self._gen_exists(p):
                    return ("gen", p)
            elif self.tree.exists(p):
                return ("src", p)
        return None

    def read(self, key):
        kind, path = key
        if kind == "gen":
            return (self.gen_dir / path).read_bytes()
        return self.tree.read(path)

    def run(self, targets, max_levels=200):
        """BFS over includes from `targets` (src-relative paths). Returns
        the set of reached (kind, path) keys."""
        seen = set()
        frontier = [("src", t) for t in targets]
        level = 0
        self.missing = {}
        while frontier and level < max_levels:
            level += 1
            # Fetch everything this level needs in one batch.
            self.tree.materialize([p for k, p in frontier if k == "src"])
            nxt = []
            for key in frontier:
                if key in seen:
                    continue
                seen.add(key)
                data = self.read(key)
                if data is None:
                    continue
                for kind, name, angle in extract_includes(data):
                    if angle and "/" not in name and "." not in name:
                        continue  # <vector> and friends
                    if angle and (name.startswith(SYSTEM_PREFIXES) or
                                  STD_NAMES.match(name)):
                        continue
                    if kind == "include_next":
                        continue
                    r = self.resolve(name, key, angle)
                    if r is None:
                        # A generated file's sibling ("foo.pb.h" from
                        # gen/x/bar.pb.h) is generated next to it.
                        if key[0] == "gen" and not angle and "/" not in name \
                                and "/" in key[1]:
                            name = key[1].rsplit("/", 1)[0] + "/" + name
                        self.missing.setdefault(name, set()).add(key[1])
                    elif r not in seen:
                        nxt.append(r)
            frontier = list(dict.fromkeys(nxt))
            if frontier:
                log("scan level %d: %d new file(s), %d seen" % (
                    level, len(frontier), len(seen)))
        return seen


def _normpath(p):
    parts = []
    for c in p.split("/"):
        if c in ("", "."):
            continue
        if c == "..":
            if not parts:
                return None
            parts.pop()
        else:
            parts.append(c)
    return "/".join(parts)
