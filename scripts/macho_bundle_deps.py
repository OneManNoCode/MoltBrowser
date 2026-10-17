#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Makes a bundled macOS helper binary self-contained.

The helpers MoltBrowser bundles under Contents/Resources/ (tesseract,
whisper-cli) come from Homebrew or a local build, so their load commands
point at /opt/homebrew, /usr/local, a build tree. In the app none of that may
load: the helper sandbox (molt_ai/common/seatbelt_profile.cc) maps code only
from the helper's own directory and the OS; library validation under the
hardened runtime refuses libraries our team didn't sign; and a user's Homebrew
is not ours to run.

So this copies every library BINARY needs, apart from the OS's (/usr/lib,
/System/Library), into LIB_DIR, recursively, and rewrites the load commands:
BINARY loads them as @executable_path/<LIB_DIR>/<name>, the libraries load
each other as @loader_path/<name>, and every LC_RPATH goes (nothing is left
that needs one). Each changed file is re-signed ad hoc, since arm64 runs only
signed code; the release signing step signs them again with the Developer ID.
It then runs the --check below and fails if anything is left.

  scripts/macho_bundle_deps.py Resources/ocr/tesseract \\
      --lib-dir Resources/ocr/lib --origin /opt/homebrew/bin/tesseract

--origin is where BINARY was copied from, for @executable_path and
@loader_path in its own load commands.

  scripts/macho_bundle_deps.py --check Resources/ocr/tesseract \\
      --lib-dir Resources/ocr/lib

--check changes nothing. It fails unless every library BINARY and the files in
LIB_DIR load is an OS library or a file inside BINARY's directory, and none of
them has an LC_RPATH.

Needs otool, install_name_tool and codesign (Xcode command line tools); the
--otool, --install-name-tool and --codesign flags pick others (the test uses
LLVM's and no codesign).
"""

import argparse
import os
import shutil
import subprocess
import sys

# What the helper sandbox lets a helper map besides its own directory.
SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/")

DYLIB_COMMANDS = {
    "LC_LOAD_DYLIB",
    "LC_LOAD_WEAK_DYLIB",
    "LC_REEXPORT_DYLIB",
    "LC_LAZY_LOAD_DYLIB",
    "LC_LOAD_UPWARD_DYLIB",
}

LOADER_PATH = "@loader_path/"
EXECUTABLE_PATH = "@executable_path/"
RPATH = "@rpath/"


class BundleError(Exception):
    pass


class Tools:
    def __init__(self, otool, install_name_tool, codesign):
        self.otool = otool
        self.install_name_tool = install_name_tool
        self.codesign = codesign

    def run(self, argv):
        proc = subprocess.run(argv, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, universal_newlines=True)
        if proc.returncode != 0:
            raise BundleError("%s failed (exit %d):\n%s"
                              % (" ".join(argv), proc.returncode,
                                 proc.stderr.strip()))
        return proc.stdout


class LoadCommands:
    def __init__(self):
        self.id = None
        self.deps = []
        self.rpaths = []


def _command_value(line, key):
    # "name /usr/lib/libz.1.dylib (offset 24)" -> "/usr/lib/libz.1.dylib"
    value = line[len(key):].strip()
    head, sep, _ = value.rpartition(" (offset ")
    return head if sep else value


def parse_load_commands(text):
    """Parses `otool -l` output. Fat files list each slice; the union is
    returned, in order, without duplicates."""
    result = LoadCommands()
    cmd = None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("Load command") or line.endswith(":"):
            cmd = None
        elif line.startswith("cmd "):
            cmd = line[len("cmd "):].strip()
        elif line.startswith("name ") and cmd == "LC_ID_DYLIB":
            result.id = _command_value(line, "name ")
        elif line.startswith("name ") and cmd in DYLIB_COMMANDS:
            name = _command_value(line, "name ")
            if name not in result.deps:
                result.deps.append(name)
        elif line.startswith("path ") and cmd == "LC_RPATH":
            path = _command_value(line, "path ")
            if path not in result.rpaths:
                result.rpaths.append(path)
    return result


def load_commands(tools, path):
    return parse_load_commands(tools.run([tools.otool, "-l", path]))


def is_system(name):
    return name.startswith(SYSTEM_PREFIXES)


def origin_dirs(path):
    """The directories @loader_path may stand for: as loaded, and resolved."""
    dirs = [os.path.dirname(os.path.abspath(path))]
    real = os.path.dirname(os.path.realpath(path))
    if real not in dirs:
        dirs.append(real)
    return dirs


def expand(name, loader_dirs, exe_dirs):
    """The candidate paths for a name that may start with @loader_path/ or
    @executable_path/."""
    if name.startswith(LOADER_PATH):
        return [os.path.join(d, name[len(LOADER_PATH):]) for d in loader_dirs]
    if name.startswith(EXECUTABLE_PATH):
        return [os.path.join(d, name[len(EXECUTABLE_PATH):]) for d in exe_dirs]
    return [name]


def system_library_exists(path):
    """Whether an OS library exists: on disk or, from macOS 11, only in the
    dyld shared cache."""
    if os.path.exists(path):
        return True
    if sys.platform != "darwin":
        return False
    import ctypes
    try:
        contains = ctypes.CDLL(None)._dyld_shared_cache_contains_path
    except (OSError, AttributeError):
        return False
    contains.restype = ctypes.c_bool
    contains.argtypes = [ctypes.c_char_p]
    return bool(contains(os.fsencode(path)))


def resolve(name, loader_dirs, exe_dirs, rpath_dirs):
    """Where dyld would find `name`: an OS path, an existing file, or None."""
    if not name.startswith("@"):
        return name if is_system(name) or os.path.isfile(name) else None
    if name.startswith(RPATH):
        candidates = [os.path.join(d, name[len(RPATH):]) for d in rpath_dirs]
    else:
        candidates = expand(name, loader_dirs, exe_dirs)
    for candidate in candidates:
        if is_system(candidate):
            if system_library_exists(candidate):
                return candidate
        elif os.path.isfile(candidate):
            return candidate
    return None


def bundle(tools, binary, lib_dir, origin):
    """Copies BINARY's non-OS libraries into LIB_DIR and rewrites the load
    commands. Returns the files it changed."""
    binary = os.path.abspath(binary)
    lib_dir = os.path.abspath(lib_dir)
    helper_dir = os.path.dirname(binary)
    if not (lib_dir + os.sep).startswith(helper_dir + os.sep):
        raise BundleError("%s is not inside %s" % (lib_dir, helper_dir))
    exe_prefix = EXECUTABLE_PATH + os.path.relpath(lib_dir, helper_dir) + "/"
    exe_dirs = origin_dirs(origin)

    name_for_real = {}  # source library (realpath) -> its name in lib_dir
    real_for_name = {}  # the reverse
    edits = {}          # file -> install_name_tool arguments
    # (file to edit, where it came from, its @loader_path dirs, the rpaths it
    # inherits)
    work = [(binary, origin, exe_dirs, [])]
    while work:
        dest, source, loader_dirs, inherited_rpaths = work.pop(0)
        cmds = load_commands(tools, dest)
        is_lib = dest != binary
        # dyld searches the image's own rpaths, then those of the images that
        # loaded it, up to the executable.
        rpath_dirs = []
        for rpath in cmds.rpaths:
            rpath_dirs.extend(expand(rpath, loader_dirs, exe_dirs))
        rpath_dirs.extend(inherited_rpaths)

        args = []
        if is_lib:
            args += ["-id", RPATH + os.path.basename(dest)]
        for name in cmds.deps:
            found = resolve(name, loader_dirs, exe_dirs, rpath_dirs)
            if found is None:
                raise BundleError("%s: can't find %s (looked in rpaths %s)"
                                  % (source, name, rpath_dirs or "none"))
            if is_system(found):
                if found != name:
                    args += ["-change", name, found]
                continue
            real = os.path.realpath(found)
            lib_name = name_for_real.get(real)
            if lib_name is None:
                lib_name = os.path.basename(name)
                if lib_name in real_for_name:
                    raise BundleError("two different libraries are named %s: "
                                      "%s and %s" % (lib_name,
                                                     real_for_name[lib_name],
                                                     real))
                name_for_real[real] = lib_name
                real_for_name[lib_name] = real
                copy = os.path.join(lib_dir, lib_name)
                os.makedirs(lib_dir, exist_ok=True)
                shutil.copyfile(real, copy)
                os.chmod(copy, 0o644)
                work.append((copy, found, origin_dirs(found), rpath_dirs))
            new = (LOADER_PATH if is_lib else exe_prefix) + lib_name
            args += ["-change", name, new]
        for rpath in cmds.rpaths:
            args += ["-delete_rpath", rpath]
        if args:
            edits[dest] = args

    for dest, args in edits.items():
        tools.run([tools.install_name_tool] + args + [dest])
        # A path listed twice in one slice needs a second -delete_rpath.
        for _ in range(8):
            left = load_commands(tools, dest).rpaths
            if not left:
                break
            delete = []
            for rpath in left:
                delete += ["-delete_rpath", rpath]
            tools.run([tools.install_name_tool] + delete + [dest])
    if tools.codesign:
        for dest in edits:
            tools.run([tools.codesign, "--force", "--sign", "-", dest])
    return sorted(edits)


def check(tools, binary, lib_dir):
    """Every problem that would make BINARY load code from outside its
    directory or the OS."""
    binary = os.path.abspath(binary)
    helper_dir = os.path.realpath(os.path.dirname(binary))
    files = [binary]
    if lib_dir and os.path.isdir(lib_dir):
        for entry in sorted(os.listdir(lib_dir)):
            path = os.path.join(os.path.abspath(lib_dir), entry)
            if os.path.isfile(path) and not os.path.islink(path):
                files.append(path)
    problems = []
    for path in files:
        cmds = load_commands(tools, path)
        for rpath in cmds.rpaths:
            problems.append("%s: LC_RPATH %s" % (path, rpath))
        for name in cmds.deps:
            if is_system(name):
                continue
            if name.startswith((LOADER_PATH, EXECUTABLE_PATH)):
                target = expand(name, [os.path.dirname(path)],
                                [os.path.dirname(binary)])[0]
                real = os.path.realpath(target)
                if (os.path.isfile(real)
                        and real.startswith(helper_dir + os.sep)):
                    continue
            problems.append("%s: loads %s" % (path, name))
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("binary", help="the bundled helper executable")
    ap.add_argument("--lib-dir", help="where its libraries go")
    ap.add_argument("--origin",
                    help="where BINARY was copied from (default: BINARY)")
    ap.add_argument("--check", action="store_true",
                    help="change nothing; fail on any non-OS load")
    ap.add_argument("--otool", default="otool")
    ap.add_argument("--install-name-tool", default="install_name_tool")
    ap.add_argument("--codesign",
                    default="codesign" if sys.platform == "darwin" else "",
                    help="'' skips the ad hoc re-sign")
    args = ap.parse_args()
    tools = Tools(args.otool, args.install_name_tool, args.codesign)
    prog = os.path.basename(sys.argv[0])

    try:
        if not args.check:
            if not args.lib_dir:
                ap.error("--lib-dir is required unless --check")
            changed = bundle(tools, args.binary, args.lib_dir,
                             args.origin or args.binary)
            for path in changed:
                print("%s: rewrote %s" % (prog, path))
            if os.path.isdir(args.lib_dir) and not os.listdir(args.lib_dir):
                os.rmdir(args.lib_dir)
        problems = check(tools, args.binary, args.lib_dir)
    except BundleError as e:
        print("%s: %s" % (prog, e), file=sys.stderr)
        return 1
    if problems:
        print("%s: %s still loads code from outside its bundle:"
              % (prog, args.binary), file=sys.stderr)
        for problem in problems:
            print("  " + problem, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
