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
VISION_ID = 'qwen3-vl-2b'
REVISION = '0123456789abcdef0123456789abcdef01234567'
SHA256 = 'ab' * 32
PROJECTOR_SHA256 = 'cd' * 32


def entries(path):
  with open(path) as f:
    return {m.group('id'): m.groupdict()
            for m in pin_models.ENTRY_RE.finditer(f.read())}


def projectors(path):
  with open(path) as f:
    return {m.group('id'): m.groupdict()
            for m in pin_models.PROJECTOR_RE.finditer(f.read())}


class PinModelsTest(unittest.TestCase):

  def setUp(self):
    self.tmp = tempfile.mkdtemp()
    self.addCleanup(shutil.rmtree, self.tmp)
    self.registry = os.path.join(self.tmp, 'embedding_model_registry.cc')
    shutil.copy(pin_models.EMBEDDING_REGISTRY, self.registry)
    self.replies = os.path.join(self.tmp, 'replies')
    os.mkdir(self.replies)

  def reply(self, repo, filename, sha256=SHA256, size=639150592, more=()):
    """|more|: further (filename, sha256, size) files in the reply."""
    path = os.path.join(self.replies, repo.replace('/', '__') + '.json')
    files = [(filename, sha256, size)] + list(more)
    with open(path, 'w') as f:
      json.dump({'sha': REVISION, 'siblings': [
          {'rfilename': name, 'lfs': {'sha256': h, 'size': n}}
          for name, h, n in files]}, f)

  def chat_registry(self):
    path = os.path.join(self.tmp, 'browser_ai_runtime.cc')
    shutil.copy(pin_models.REGISTRY, path)
    return path

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

  def test_vision_model_and_projector_are_found(self):
    chat = entries(pin_models.REGISTRY)
    found = projectors(pin_models.REGISTRY)
    self.assertIn(VISION_ID, found)
    # Every projector belongs to a model of the registry.
    for model_id, p in found.items():
      self.assertIn(model_id, chat)
      self.assertTrue(p['file'].endswith('.gguf'))
      self.assertNotEqual(p['file'], chat[model_id]['file'])
    # No other registry has projectors.
    self.assertEqual(projectors(pin_models.EMBEDDING_REGISTRY), {})

  def test_vision_pin_is_all_or_nothing(self):
    # Fail closed: the browser offers a vision model only when the model and
    # its projector are both pinned (IsPinnedForVision()).
    e = entries(pin_models.REGISTRY)[VISION_ID]
    p = projectors(pin_models.REGISTRY)[VISION_ID]
    parts = [e['revision'] != '', e['sha256'] != '', e['size'] != '0',
             p['sha256'] != '', p['size'] != '0']
    self.assertIn(parts, ([False] * 5, [True] * 5))

  def test_pins_a_vision_model_with_its_projector(self):
    registry = self.chat_registry()
    e = entries(registry)[VISION_ID]
    p = projectors(registry)[VISION_ID]
    self.reply(e['repo'], e['file'], size=1107409952,
               more=[(p['file'], PROJECTOR_SHA256, 445053792),
                     ('README.md', SHA256, 10)])
    with open(registry) as f:
      before = f.read()
    code, log = self.run_main('--registry', registry, '--from-json',
                              self.replies, '--only', VISION_ID, '--write')
    self.assertEqual(code, 0, log)
    pinned = entries(registry)[VISION_ID]
    self.assertEqual(pinned['revision'], REVISION)
    self.assertEqual(pinned['sha256'], SHA256)
    self.assertEqual(pinned['size'], '1107409952')
    pinned_projector = projectors(registry)[VISION_ID]
    self.assertEqual(pinned_projector['sha256'], PROJECTOR_SHA256)
    self.assertEqual(pinned_projector['size'], '445053792')
    # Only those five values changed.
    with open(registry) as f:
      after = f.read()
    self.assertEqual(len(after.splitlines()), len(before.splitlines()))
    changed = [(a, b) for a, b in zip(before.splitlines(), after.splitlines())
               if a != b]
    self.assertEqual(len(changed), 5, changed)
    # Every other entry is untouched.
    for model_id, entry in entries(pin_models.REGISTRY).items():
      if model_id != VISION_ID:
        self.assertEqual(entries(registry)[model_id], entry)
    code, log = self.run_main('--registry', registry, '--from-json',
                              self.replies, '--only', VISION_ID)
    self.assertEqual(code, 0, log)
    self.assertIn('already pinned', log)

  def test_no_vision_pin_without_the_projector(self):
    registry = self.chat_registry()
    e = entries(registry)[VISION_ID]
    self.reply(e['repo'], e['file'])
    with open(registry) as f:
      before = f.read()
    code, log = self.run_main('--registry', registry, '--from-json',
                              self.replies, '--only', VISION_ID, '--write')
    self.assertEqual(code, 1)
    self.assertIn('not found', log)
    with open(registry) as f:
      self.assertEqual(f.read(), before)

  def test_projector_needs_a_proper_hash(self):
    registry = self.chat_registry()
    e = entries(registry)[VISION_ID]
    p = projectors(registry)[VISION_ID]
    self.reply(e['repo'], e['file'], more=[(p['file'], 'nope', 10)])
    with open(registry) as f:
      before = f.read()
    code, log = self.run_main('--registry', registry, '--from-json',
                              self.replies, '--only', VISION_ID, '--write')
    self.assertEqual(code, 1)
    with open(registry) as f:
      self.assertEqual(f.read(), before)

  def test_unknown_model_id(self):
    code, log = self.run_main('--registry', self.registry, '--only', 'nope')
    self.assertEqual(code, 2)
    self.assertIn('unknown model id', log)


if __name__ == '__main__':
  unittest.main()
