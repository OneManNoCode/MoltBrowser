# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Chromium's own clang (the revision tools/clang/scripts/update.py pins),
downloaded from the public chromium-browser-clang bucket."""
import re
import shutil
import subprocess
import tarfile
import urllib.request
from pathlib import Path

from .sources import log

BUCKET = ("https://commondatastorage.googleapis.com/chromium-browser-clang/"
          "Linux_x64/")


def package_version(tree):
    text = tree.read("tools/clang/scripts/update.py").decode()
    rev = re.search(r"^CLANG_REVISION = '([^']+)'", text, re.M).group(1)
    sub = re.search(r"^CLANG_SUB_REVISION = (\d+)", text, re.M).group(1)
    return "%s-%s" % (rev, sub)


def clang_dir(work):
    return Path(work) / "toolchain" / "clang"


def ensure_clang(work, tree):
    want = package_version(tree)
    d = clang_dir(work)
    stamp = d / "cr_build_revision"
    if stamp.exists() and stamp.read_text().strip() == want:
        return d
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    url = BUCKET + "clang-%s.tar.xz" % want
    tmp = Path(work) / "toolchain" / "clang.tar.xz"
    log("downloading %s" % url)
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    with tarfile.open(tmp) as t:
        t.extractall(d, filter="data")
    tmp.unlink()
    return d


def llvm_commit(work):
    """The llvm-project commit the downloaded clang was built from."""
    exe = clang_dir(work) / "bin" / "clang"
    if not exe.exists():
        return None
    out = subprocess.run([str(exe), "--version"], capture_output=True,
                         text=True).stdout
    m = re.search(r"llvm-project ([0-9a-f]{40})", out)
    return m.group(1) if m else None
