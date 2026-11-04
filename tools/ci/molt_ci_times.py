#!/usr/bin/env python3
# MoltBrowser — file times for a Chromium build resumed in another job
# Copyright 2026 GenEye AI Labs Inc.
#
# Ninja decides what to rebuild by comparing file times. A CI job that
# syncs Chromium afresh gives every source file a new time, so an out dir
# restored from an earlier job would be rebuilt from scratch. This tool:
#
#   1. sets every file and directory under the Chromium tree (except out/)
#      to one fixed old time;
#   2. hashes every regular file and writes the list (the manifest);
#   3. if the manifest of the build being restored is given, sets each file
#      whose hash differs from it, or that it doesn't list, to now.
#
# Ninja then rebuilds exactly what changed since the restored build: within
# one run nothing (the tree is the same), across runs the overlay's edits.
#
# Usage:
#   molt_ci_times.py --tree chromium/src --write manifest.txt [--previous old.txt]
#
# The manifest is "<sha1>  <path>" per line, sorted by path; paths are
# relative to the tree.

import argparse
import concurrent.futures
import hashlib
import os
import sys
import time

# 2020-01-01T00:00:00Z: older than any output an earlier job wrote.
OLD_TIME = 1577836800
SKIP_TOP = {'out'}


def walk(tree):
    """Yields (relative path, is_file) for everything under tree but out/."""
    for root, dirs, files in os.walk(tree, followlinks=False):
        rel_root = os.path.relpath(root, tree)
        if rel_root == '.':
            dirs[:] = [d for d in dirs if d not in SKIP_TOP]
        # Directories that are symlinks are listed in dirs but not walked.
        for d in dirs:
            yield os.path.normpath(os.path.join(rel_root, d)), False
        for f in files:
            yield os.path.normpath(os.path.join(rel_root, f)), True


def file_hash(path):
    h = hashlib.sha1()
    try:
        with open(path, 'rb') as f:
            while True:
                chunk = f.read(1 << 20)
                if not chunk:
                    break
                h.update(chunk)
    except OSError:
        return None
    return h.hexdigest()


def set_time(path, when):
    try:
        os.utime(path, (when, when), follow_symlinks=False)
    except OSError:
        pass


def read_manifest(path):
    out = {}
    with open(path, encoding='utf-8', errors='surrogateescape') as f:
        for line in f:
            line = line.rstrip('\n')
            if len(line) > 42 and line[40:42] == '  ':
                out[line[42:]] = line[:40]
    return out


def main(argv):
    p = argparse.ArgumentParser(description='File times for a resumed Chromium build.')
    p.add_argument('--tree', required=True)
    p.add_argument('--write', required=True, help='manifest to write')
    p.add_argument('--previous', help='manifest of the restored build')
    p.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = p.parse_args(argv)

    tree = os.path.abspath(args.tree)
    started = time.time()
    files = []
    count = 0
    for rel, is_file in walk(tree):
        full = os.path.join(tree, rel)
        set_time(full, OLD_TIME)
        count += 1
        if is_file and not os.path.islink(full) and os.path.isfile(full):
            files.append(rel)
    set_time(tree, OLD_TIME)
    files.sort()
    print('molt_ci_times: %d entries set to the old time in %.0f s' %
          (count, time.time() - started))

    started = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        hashes = list(pool.map(lambda r: file_hash(os.path.join(tree, r)),
                               files, chunksize=256))
    current = {r: h for r, h in zip(files, hashes) if h}
    with open(args.write, 'w', encoding='utf-8',
              errors='surrogateescape') as f:
        for rel in files:
            h = current.get(rel)
            if h:
                f.write('%s  %s\n' % (h, rel))
    print('molt_ci_times: %d files hashed in %.0f s' %
          (len(current), time.time() - started))

    if args.previous:
        previous = read_manifest(args.previous)
        now = time.time()
        changed = [r for r, h in current.items() if previous.get(r) != h]
        for rel in changed:
            set_time(os.path.join(tree, rel), now)
        print('molt_ci_times: %d files changed since the restored build '
              '(set to now)' % len(changed))
        for rel in sorted(changed)[:200]:
            print('  ' + rel)
        if len(changed) > 200:
            print('  ... and %d more' % (len(changed) - 200))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
