#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Commits gnrt's vendored Rust crates from a CI log, without a Chromium checkout.

A test build asked for it ("vendor_dump": true in .github/molt-test-build.json)
runs scripts/vendor-rust-crates.sh with its copy-back on the CI runner's
Chromium checkout, and tools/ci/molt_test_build.sh prints what changed under
src/third_party/rust into stage 1's log: a base64 tar.gz between
MOLT-VENDORED-BEGIN and MOLT-VENDORED-END, its SHA-256, and the removed paths.

  python3 -I tools/ci/molt_vendored_from_log.py <job log> [--repo DIR] [--dry-run]

It checks the archive against its SHA-256, accepts only regular files and
directories under src/third_party/rust (no links, no absolute paths, no ..),
writes them into the repository, and removes the listed paths. Review with
`git status -- src/third_party/rust` before committing. The log comes from our
own CI, but it is checked as if it didn't.
"""

import argparse
import base64
import hashlib
import io
import os
import re
import sys
import tarfile

PREFIX = 'src/third_party/rust/'
# GitHub's job logs start each line with a timestamp.
STAMP = re.compile(r'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d+Z ')


def strip(line):
    # GitHub's logs come in chunks, each starting with a byte order mark.
    return STAMP.sub('', line.rstrip('\r\n').lstrip('\ufeff'))


def parse(text):
    lines = [strip(l) for l in text.splitlines()]
    if any(l == 'MOLT-VENDORED-NONE' for l in lines):
        return None, b'', []
    sha = None
    for l in lines:
        if l.startswith('MOLT-VENDORED-SHA256 '):
            sha = l.split(' ', 1)[1].strip()
    if not sha or not re.fullmatch(r'[0-9a-f]{64}', sha):
        raise SystemExit('no MOLT-VENDORED-SHA256 line in the log')

    def between(begin, end):
        try:
            i = lines.index(begin)
            j = lines.index(end, i + 1)
        except ValueError:
            raise SystemExit('the log has no %s ... %s block' % (begin, end))
        return lines[i + 1:j]

    data = base64.b64decode(''.join(between('MOLT-VENDORED-BEGIN',
                                            'MOLT-VENDORED-END')),
                            validate=True)
    removed = [l for l in between('MOLT-VENDORED-REMOVED-BEGIN',
                                  'MOLT-VENDORED-REMOVED-END') if l]
    if hashlib.sha256(data).hexdigest() != sha:
        raise SystemExit('the archive does not match its SHA-256 (%s)' % sha)
    return sha, data, removed


def safe(name):
    if name.startswith('/') or '\\' in name:
        return False
    parts = name.split('/')
    if any(p in ('', '.', '..') for p in parts[:-1]) or parts[-1] in ('.', '..'):
        return False
    return (name + '/').startswith(PREFIX) and name != PREFIX.rstrip('/')


def apply(repo, data, removed, dry_run):
    # Everything is checked before anything is written.
    for name in removed:
        if not safe(name):
            raise SystemExit('refused removed path: %r' % name)
    written = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as tar:
        members = tar.getmembers()
        for m in members:
            name = m.name[2:] if m.name.startswith('./') else m.name
            if not safe(name) or not (m.isfile() or m.isdir()):
                raise SystemExit('refused archive member: %r' % m.name)
        for m in members:
            name = m.name[2:] if m.name.startswith('./') else m.name
            dest = os.path.join(repo, name)
            if m.isdir():
                if not dry_run:
                    os.makedirs(dest, exist_ok=True)
                continue
            if not dry_run:
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with tar.extractfile(m) as src, open(dest, 'wb') as out:
                    out.write(src.read())
                os.chmod(dest, 0o755 if m.mode & 0o111 else 0o644)
            written += 1
    for name in removed:
        path = os.path.join(repo, name)
        if not dry_run and os.path.isfile(path) and not os.path.islink(path):
            os.remove(path)
    return written


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('log')
    p.add_argument('--repo', default=os.path.abspath(
        os.path.join(os.path.dirname(__file__), '..', '..')))
    p.add_argument('--dry-run', action='store_true')
    args = p.parse_args(argv)
    with open(args.log, encoding='utf-8', errors='replace') as f:
        sha, data, removed = parse(f.read())
    if sha is None:
        print('gnrt changed nothing (MOLT-VENDORED-NONE)')
        return 0
    n = apply(args.repo, data, removed, args.dry_run)
    print('%s %d files from archive %s, %d removed' %
          ('would write' if args.dry_run else 'wrote', n, sha, len(removed)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
