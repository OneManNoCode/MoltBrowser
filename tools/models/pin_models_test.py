#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for pin_models.py, offline (--from-json), on copies of the real
registries.

Run: python3 tools/models/pin_models_test.py -v
"""

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pin_models  # noqa: E402

EMBEDDING_ID = 'qwen3-embedding-0.6b'
REVISION = '0123456789abcdef0123456789abcdef01234567'
SHA256 = 'ab' * 32


def entries(path):
  with open(path) as f:
    return {m.group('id'): m.groupdict()
            for m in pin_models.ENTRY_RE.finditer(f.read())}


class PinModelsTest(unittest.TestCase):

  def setUp(self):
    self.tmp = tempfile.mkdtemp()
    self.addCleanup(shutil.rmtree, self.tmp)
    self.registry = os.path.join(self.tmp, 'embedding_model_registry.cc')
    shutil.copy(pin_models.EMBEDDING_REGISTRY, self.registry)
    self.replies = os.path.join(self.tmp, 'replies')
    os.mkdir(self.replies)

  def reply(self, repo, filename, sha256=SHA256, size=639150592):
    path = os.path.join(self.replies, repo.replace('/', '__') + '.json')
    with open(path, 'w') as f:
      json.dump({'sha': REVISION, 'siblings': [
          {'rfilename': filename, 'lfs': {'sha256': sha256, 'size': size}}]},
                f)

  def run_main(self, *argv):
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
      code = pin_models.main(list(argv))
    return code, out.getvalue() + err.getvalue()

  def test_both_registries_are_found_by_default(self):
    self.assertEqual(pin_models.REGISTRIES,
                     [pin_models.REGISTRY, pin_models.EMBEDDING_REGISTRY])
    chat = entries(pin_models.REGISTRY)
    self.assertIn('qwen3-8b', chat)
    self.assertGreaterEqual(len(chat), 5)
    embedding = entries(pin_models.EMBEDDING_REGISTRY)
    self.assertEqual(list(embedding), [EMBEDDING_ID])
    e = embedding[EMBEDDING_ID]
    self.assertEqual(e['repo'], 'Qwen/Qwen3-Embedding-0.6B-GGUF')
    self.assertTrue(e['file'].endswith('.gguf'))

  def test_embedding_pin_is_all_or_nothing(self):
    # Fail closed: the browser offers the model only when revision, sha256
    # and size are all set (IsPinned()); a half-filled pin is a mistake.
    e = entries(pin_models.EMBEDDING_REGISTRY)[EMBEDDING_ID]
    parts = [e['revision'] != '', e['sha256'] != '', e['size'] != '0']
    self.assertIn(parts, ([False] * 3, [True] * 3))
    if all(parts):
      self.assertRegex(e['revision'], pin_models.SHA1_RE)
      self.assertRegex(e['sha256'], pin_models.SHA256_RE)

  def test_pins_the_embedding_model(self):
    e = entries(self.registry)[EMBEDDING_ID]
    self.reply(e['repo'], e['file'])
    code, log = self.run_main('--registry', self.registry, '--from-json',
                              self.replies, '--only', EMBEDDING_ID, '--write')
    self.assertEqual(code, 0, log)
    pinned = entries(self.registry)[EMBEDDING_ID]
    self.assertEqual(pinned['revision'], REVISION)
    self.assertEqual(pinned['sha256'], SHA256)
    self.assertEqual(pinned['size'], '639150592')
    # Everything else in the file is untouched.
    with open(pin_models.EMBEDDING_REGISTRY) as f:
      original = f.read()
    with open(self.registry) as f:
      updated = f.read()
    self.assertEqual(
        updated.replace('/*revision=*/"%s"' % REVISION, '/*revision=*/""')
        .replace('/*sha256=*/"%s"' % SHA256, '/*sha256=*/""')
        .replace('/*size=*/639150592ULL', '/*size=*/0ULL'),
        original.replace(
            '/*revision=*/"%s"' % e['revision'], '/*revision=*/""').replace(
                '/*sha256=*/"%s"' % e['sha256'], '/*sha256=*/""').replace(
                    '/*size=*/%sULL' % e['size'], '/*size=*/0ULL'))
    code, log = self.run_main('--registry', self.registry, '--from-json',
                              self.replies, '--only', EMBEDDING_ID)
    self.assertEqual(code, 0, log)
    self.assertIn('already pinned', log)

  def test_dry_run_writes_nothing(self):
    e = entries(self.registry)[EMBEDDING_ID]
    self.reply(e['repo'], e['file'])
    with open(self.registry) as f:
      before = f.read()
    code, log = self.run_main('--registry', self.registry, '--from-json',
                              self.replies)
    self.assertEqual(code, 0, log)
    with open(self.registry) as f:
      self.assertEqual(f.read(), before)

  def test_refuses_a_reply_without_a_proper_hash(self):
    e = entries(self.registry)[EMBEDDING_ID]
    self.reply(e['repo'], e['file'], sha256='not-a-hash')
    with open(self.registry) as f:
      before = f.read()
    code, log = self.run_main('--registry', self.registry, '--from-json',
                              self.replies, '--write')
    self.assertEqual(code, 1)
    self.assertIn('FAIL', log)
    with open(self.registry) as f:
      self.assertEqual(f.read(), before)

  def test_refuses_a_missing_file(self):
    e = entries(self.registry)[EMBEDDING_ID]
    self.reply(e['repo'], 'some-other-file.gguf')
    code, log = self.run_main('--registry', self.registry, '--from-json',
                              self.replies, '--write')
    self.assertEqual(code, 1)
    self.assertIn('not found', log)

  def test_unknown_model_id(self):
    code, log = self.run_main('--registry', self.registry, '--only', 'nope')
    self.assertEqual(code, 2)
    self.assertIn('unknown model id', log)


if __name__ == '__main__':
  unittest.main()
