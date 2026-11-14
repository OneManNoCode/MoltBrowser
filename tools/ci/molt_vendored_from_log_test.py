#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for molt_vendored_from_log.py: a CI log's vendored crates, checked."""

import base64
import hashlib
import io
import os
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import molt_vendored_from_log as v  # noqa: E402


def archive(members):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w:gz') as t:
        for kind, name, data in members:
            ti = tarfile.TarInfo(name)
            if kind == 'link':
                ti.type = tarfile.SYMTYPE
                ti.linkname = '/etc/passwd'
                t.addfile(ti)
            else:
                ti.size = len(data)
                ti.mode = 0o755 if kind == 'exec' else 0o644
                t.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def log(data, removed=(), sha=None, stamp=True):
    lines = ['MOLT-VENDORED-SHA256 ' + (sha or hashlib.sha256(data).hexdigest()),
             'MOLT-VENDORED-BEGIN']
    b64 = base64.b64encode(data).decode()
    lines += [b64[i:i + 76] for i in range(0, len(b64), 76)]
    lines += ['MOLT-VENDORED-END', 'MOLT-VENDORED-REMOVED-BEGIN', *removed,
              'MOLT-VENDORED-REMOVED-END']
    prefix = '2026-11-14T02:00:00.1234567Z ' if stamp else ''
    return '\n'.join(prefix + l for l in ['==> Vendored: ...'] + lines) + '\n'


class VendoredFromLogTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = self.tmp.name
        os.makedirs(os.path.join(self.repo, 'src/third_party/rust/old'))
        with open(os.path.join(self.repo, 'src/third_party/rust/old/gone.rs'), 'w') as f:
            f.write('old')

    def tearDown(self):
        self.tmp.cleanup()

    def run_log(self, text):
        path = os.path.join(self.repo, 'job.log')
        with open(path, 'w', encoding='utf-8') as f:
            f.write(text)
        return v.main([path, '--repo', self.repo])

    def test_writes_files_and_removes_paths(self):
        data = archive([
            ('file', 'src/third_party/rust/adblock/v0_13/src/lib.rs', b'pub fn f() {}\n'),
            ('file', 'src/third_party/rust/chromium_crates_io/vendor/a/tests/a file é.txt', b'x'),
            ('exec', 'src/third_party/rust/chromium_crates_io/vendor/a/build.sh', b'#!/bin/sh\n'),
        ])
        self.assertEqual(self.run_log(log(data, ['src/third_party/rust/old/gone.rs'])), 0)
        with open(os.path.join(self.repo, 'src/third_party/rust/adblock/v0_13/src/lib.rs')) as f:
            self.assertEqual(f.read(), 'pub fn f() {}\n')
        self.assertTrue(os.path.exists(os.path.join(
            self.repo, 'src/third_party/rust/chromium_crates_io/vendor/a/tests/a file é.txt')))
        self.assertTrue(os.access(os.path.join(
            self.repo, 'src/third_party/rust/chromium_crates_io/vendor/a/build.sh'), os.X_OK))
        self.assertFalse(os.path.exists(os.path.join(self.repo, 'src/third_party/rust/old/gone.rs')))

    def test_a_log_without_timestamps(self):
        data = archive([('file', 'src/third_party/rust/a.rs', b'a')])
        self.assertEqual(self.run_log(log(data, stamp=False)), 0)

    def test_nothing_vendored(self):
        self.assertEqual(self.run_log('x\nMOLT-VENDORED-NONE\n'), 0)

    def test_refuses(self):
        for members, removed in (
                ([('file', 'src/third_party/rust/../../evil', b'x')], ()),
                ([('file', '/src/third_party/rust/a.rs', b'x')], ()),
                ([('file', 'src/chrome/evil.cc', b'x')], ()),
                ([('file', 'src/third_party/rustacean/a.rs', b'x')], ()),
                ([('link', 'src/third_party/rust/x', b'')], ()),
                ([('file', 'src/third_party/rust/a.rs', b'x')], ('src/chrome/BUILD.gn',)),
                ([('file', 'src/third_party/rust/a.rs', b'x')], ('src/third_party/rust/../../x',)),
        ):
            with self.assertRaises(SystemExit, msg=(members, removed)):
                self.run_log(log(archive(members), removed))
        self.assertFalse(os.path.exists(os.path.join(self.repo, 'src/chrome')))
        # Nothing was written before a refusal.
        self.assertFalse(os.path.exists(os.path.join(self.repo, 'src/third_party/rust/a.rs')))

    def test_refuses_a_wrong_checksum_or_a_missing_block(self):
        data = archive([('file', 'src/third_party/rust/a.rs', b'x')])
        with self.assertRaises(SystemExit):
            self.run_log(log(data, sha='0' * 64))
        with self.assertRaises(SystemExit):
            self.run_log('MOLT-VENDORED-SHA256 ' + '0' * 64 + '\n')
        self.assertFalse(os.path.exists(os.path.join(self.repo, 'src/third_party/rust/a.rs')))


if __name__ == '__main__':
    unittest.main()
