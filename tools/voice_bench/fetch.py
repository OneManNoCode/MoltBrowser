#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Downloads the voice bench's engines, models and data, pinned by SHA-256.

Every artifact is listed in artifacts.json next to this file with its URL,
byte size, SHA-256 and licence. A download that differs in size or hash is
deleted and the run fails. Archives are treated as untrusted: each is
unpacked into its own new, empty directory, members that would land outside
it (absolute paths, "..", links, devices) are refused, and the archive is
deleted afterwards unless --keep-archives is given.

Nothing goes into the repository. The data directory is, in order:
--dir, $VOICE_BENCH_DIR, or ~/.cache/moltbrowser-voice-bench
(%LOCALAPPDATA%\\moltbrowser-voice-bench on Windows).

Layout under the data directory:
  downloads/<file>          archives while they download and unpack
  a/<id>/                   one directory per artifact (unpacked or the file)
  a/<id>/.fetched.json      what was fetched: URL, SHA-256, size, licence

Usage (run Python isolated, -I, from any directory but the data directory):
  python3 -I tools/voice_bench/fetch.py --list
  python3 -I tools/voice_bench/fetch.py --set auto      # what run_bench.py needs
  python3 -I tools/voice_bench/fetch.py --set auto,sizes --dir /big/disk
  python3 -I tools/voice_bench/fetch.py --set all
  python3 -I tools/voice_bench/fetch.py --only silero-vad,kws-gigaspeech
  python3 -I tools/voice_bench/fetch.py --remove whisper-small-en

Only the Python standard library is used.
"""

import argparse
import fnmatch
import hashlib
import json
import os
import shutil
import sys
import tarfile
import time
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST = os.path.join(HERE, "artifacts.json")
CHUNK = 1 << 20


def default_dir():
    env = os.environ.get("VOICE_BENCH_DIR")
    if env:
        return env
    if os.name == "nt" and os.environ.get("LOCALAPPDATA"):
        return os.path.join(os.environ["LOCALAPPDATA"], "moltbrowser-voice-bench")
    return os.path.join(os.path.expanduser("~"), ".cache", "moltbrowser-voice-bench")


def load_manifest(path=MANIFEST):
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    seen = set()
    for a in data["artifacts"]:
        for key in ("id", "url", "sha256", "size", "kind", "sets", "license"):
            if key not in a:
                raise SystemExit("artifacts.json: %s lacks %r" % (a.get("id"), key))
        if a["id"] in seen:
            raise SystemExit("artifacts.json: duplicate id %s" % a["id"])
        seen.add(a["id"])
        if not a["url"].startswith("https://"):
            raise SystemExit("artifacts.json: %s is not https" % a["id"])
        if len(a["sha256"]) != 64:
            raise SystemExit("artifacts.json: %s has no SHA-256 pin" % a["id"])
    return data["artifacts"]


def file_name(a):
    return a.get("file") or a["url"].rsplit("/", 1)[1].split("?", 1)[0]


def download(a, dest):
    """Streams a["url"] to dest, checking size and SHA-256. Raises on mismatch."""
    part = dest + ".part"
    h = hashlib.sha256()
    got = 0
    req = urllib.request.Request(a["url"], headers={"User-Agent": "moltbrowser-voice-bench"})
    last = time.monotonic()
    with urllib.request.urlopen(req, timeout=120) as r, open(part, "wb") as out:
        while True:
            buf = r.read(CHUNK)
            if not buf:
                break
            got += len(buf)
            if got > a["size"]:
                break
            h.update(buf)
            out.write(buf)
            if time.monotonic() - last > 10:
                last = time.monotonic()
                print("  %s: %.0f of %.0f MB" % (a["id"], got / 1e6, a["size"] / 1e6), flush=True)
    digest = h.hexdigest()
    if got != a["size"] or digest != a["sha256"]:
        os.remove(part)
        raise RuntimeError("%s: got %d bytes sha256 %s, want %d bytes sha256 %s"
                           % (a["id"], got, digest, a["size"], a["sha256"]))
    os.replace(part, dest)


def _safe_member_path(root, name):
    """The path a member would be written to, or None if it leaves root."""
    if name.startswith(("/", "\\")) or ":" in name.split("/")[0]:
        return None
    parts = [p for p in name.replace("\\", "/").split("/") if p not in ("", ".")]
    if not parts or ".." in parts:
        return None
    path = os.path.join(root, *parts)
    if os.path.commonpath([os.path.abspath(path), os.path.abspath(root)]) != os.path.abspath(root):
        return None
    return path


def _wanted(name, members, strip):
    parts = [p for p in name.replace("\\", "/").split("/") if p not in ("", ".")]
    if strip:
        if len(parts) <= strip:
            return None
        parts = parts[strip:]
    rel = "/".join(parts)
    if members and not any(fnmatch.fnmatch(rel, m) for m in members):
        return None
    return rel


def extract(archive, root, a):
    """Unpacks archive into the new directory root. Only regular files and
    directories are written; everything else is refused."""
    members = a.get("members")
    strip = a.get("strip", 1)
    written = 0
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                rel = _wanted(info.filename, members, strip)
                if rel is None or info.is_dir():
                    continue
                path = _safe_member_path(root, rel)
                if path is None:
                    raise RuntimeError("%s: refusing member %r" % (a["id"], info.filename))
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with z.open(info) as src, open(path, "wb") as dst:
                    shutil.copyfileobj(src, dst, CHUNK)
                written += 1
        return written
    with tarfile.open(archive, "r:*") as t:
        for m in t:
            rel = _wanted(m.name, members, strip)
            if rel is None:
                continue
            if m.isdir():
                continue
            if not m.isfile():
                # Symlinks, hard links, devices, FIFOs: never followed or made.
                print("  %s: skipping non-regular member %s" % (a["id"], m.name))
                continue
            path = _safe_member_path(root, rel)
            if path is None:
                raise RuntimeError("%s: refusing member %r" % (a["id"], m.name))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            src = t.extractfile(m)
            with src, open(path, "wb") as dst:
                shutil.copyfileobj(src, dst, CHUNK)
            os.chmod(path, 0o755 if m.mode & 0o111 else 0o644)
            written += 1
    return written


def fetch_one(a, base, keep_archives=False):
    out = os.path.join(base, "a", a["id"])
    stamp = os.path.join(out, ".fetched.json")
    if os.path.exists(stamp):
        with open(stamp, encoding="utf-8") as f:
            if json.load(f).get("sha256") == a["sha256"]:
                print("ok      %s" % a["id"])
                return out
        raise SystemExit("%s exists with a different pin; remove it first (--remove %s)"
                         % (out, a["id"]))
    if os.path.exists(out):
        raise SystemExit("%s exists but is incomplete; remove it first (--remove %s)"
                         % (out, a["id"]))
    os.makedirs(os.path.join(base, "downloads"), exist_ok=True)
    archive = os.path.join(base, "downloads", file_name(a))
    have = False
    if os.path.exists(archive):
        h = hashlib.sha256()
        with open(archive, "rb") as f:
            for buf in iter(lambda: f.read(CHUNK), b""):
                h.update(buf)
        have = h.hexdigest() == a["sha256"]
        if not have:
            os.remove(archive)
    if not have:
        print("fetch   %s (%.1f MB)" % (a["id"], a["size"] / 1e6), flush=True)
        download(a, archive)
    tmp = out + ".unpacking"
    if os.path.exists(tmp):
        shutil.rmtree(tmp)
    os.makedirs(tmp)
    if a.get("extract"):
        n = extract(archive, tmp, a)
        if n == 0:
            shutil.rmtree(tmp)
            raise RuntimeError("%s: nothing extracted" % a["id"])
        if not keep_archives:
            os.remove(archive)
    else:
        shutil.move(archive, os.path.join(tmp, file_name(a)))
    with open(os.path.join(tmp, ".fetched.json"), "w", encoding="utf-8") as f:
        json.dump({k: a[k] for k in ("id", "url", "sha256", "size", "license")
                   if k in a}, f, indent=1)
    os.replace(tmp, out)
    print("done    %s" % a["id"])
    return out


def this_platform():
    import platform
    m = platform.machine().lower()
    arch = "arm64" if m in ("arm64", "aarch64") else "x64"
    if sys.platform == "darwin":
        return "osx-arm64" if arch == "arm64" else "osx-x64"
    if os.name == "nt":
        return "win-" + arch
    return "linux-" + ("aarch64" if arch == "arm64" else "x64")


def expand_sets(sets):
    """'auto' is this machine's runtime plus everything the benchmark runs
    (asr, kws, tts, real); 'all' is every artifact."""
    out = []
    for s in sets:
        if s == "auto":
            out += ["runtime-" + this_platform(), "asr", "kws", "tts", "real"]
        else:
            out.append(s)
    return out


def select(artifacts, sets, only):
    sets = expand_sets(sets)
    if "all" in sets:
        return list(artifacts)
    if only:
        wanted = set(only)
        unknown = wanted - {a["id"] for a in artifacts}
        if unknown:
            raise SystemExit("unknown artifact ids: %s" % ", ".join(sorted(unknown)))
        return [a for a in artifacts if a["id"] in wanted]
    return [a for a in artifacts if set(a["sets"]) & set(sets)]


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--dir", default=default_dir(), help="data directory")
    p.add_argument("--set", default="", help="comma-separated sets (see --list); "
                   "auto = this platform's runtime + asr, kws, tts, real; all = everything")
    p.add_argument("--only", default="", help="comma-separated artifact ids")
    p.add_argument("--list", action="store_true", help="list the artifacts")
    p.add_argument("--remove", default="", help="comma-separated ids to delete")
    p.add_argument("--keep-archives", action="store_true")
    args = p.parse_args(argv)
    artifacts = load_manifest()
    base = os.path.abspath(args.dir)
    if args.list:
        for a in artifacts:
            here = os.path.exists(os.path.join(base, "a", a["id"], ".fetched.json"))
            print("%-34s %8.1f MB  %-22s %-12s %s" % (
                a["id"], a["size"] / 1e6, ",".join(a["sets"]), a["license"],
                "fetched" if here else ""))
        return 0
    if args.remove:
        for i in args.remove.split(","):
            path = os.path.join(base, "a", i)
            if os.path.isdir(path):
                shutil.rmtree(path)
                print("removed %s" % path)
        return 0
    chosen = select(artifacts, [s for s in args.set.split(",") if s],
                    [s for s in args.only.split(",") if s])
    if not chosen:
        p.error("nothing selected: pass --set or --only (see --list)")
    failed = []
    for a in chosen:
        try:
            fetch_one(a, base, args.keep_archives)
        except (OSError, RuntimeError, tarfile.TarError, zipfile.BadZipFile) as e:
            print("FAILED  %s: %s" % (a["id"], e), file=sys.stderr)
            failed.append(a["id"])
    if failed:
        print("failed: %s" % ", ".join(failed), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
