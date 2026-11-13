#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for fetch_model.py, offline: the download is a stand-in.

Run: python3 tools/models/fetch_model_test.py -v
"""

import contextlib
import hashlib
import io
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fetch_model  # noqa: E402

CONTENT = b'GGUF' + b'\1' * 60


class Response(io.BytesIO):

  def __enter__(self):
    return self

  def __exit__(self, *unused):
    self.close()


def entry(content=CONTENT, **kw):
  e = {'id': 'tinyllama-1.1b', 'repo': 'TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF',
       'file': 'tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf',
       'revision': '0123456789abcdef0123456789abcdef01234567',
       'sha256': hashlib.sha256(content).hexdigest(),
       'size': str(len(content)), 'hidden': False}
  e.update(kw)
  return e


class FetchModelTest(unittest.TestCase):

  def setUp(self):
    self.tmp = tempfile.mkdtemp()
    self.addCleanup(shutil.rmtree, self.tmp)
    self.opened = []

  def urlopen(self, content):
    def opener(req, timeout):
      self.opened.append(req.full_url)
      return Response(content)
    return opener

  def run_main(self, e, content=CONTENT, flags=()):
    out, err = io.StringIO(), io.StringIO()
    with mock.patch.object(fetch_model, 'find_entry', return_value=e), \
        contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
      code = fetch_model.main(list(flags) + [e['id'] if e else 'x', self.tmp],
                              urlopen=self.urlopen(content))
    return code, out.getvalue(), err.getvalue()

  def test_the_registry_entries(self):
    tiny = fetch_model.find_entry('tinyllama-1.1b')
    self.assertFalse(tiny['hidden'])
    self.assertEqual(tiny['file'], 'tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf')
    # Hidden ones, the last entry included (its "hidden" is its own, not a
    # later table's).
    self.assertFalse(fetch_model.find_entry('qwen3.5-4b')['hidden'])
    self.assertTrue(fetch_model.find_entry('gemma4-e4b')['hidden'])
    self.assertTrue(fetch_model.find_entry('voxtral-mini-3b')['hidden'])
    self.assertFalse(fetch_model.find_entry('gpt-oss-20b')['hidden'])
    self.assertIsNone(fetch_model.find_entry('no-such-model'))

  def test_fetches_at_the_pin_and_keeps_a_file_that_matches(self):
    e = entry()
    code, out, err = self.run_main(e)
    self.assertEqual(code, 0, err)
    path = os.path.join(self.tmp, e['file'])
    self.assertEqual(out.strip(), path)
    with open(path, 'rb') as f:
      self.assertEqual(f.read(), CONTENT)
    self.assertEqual(len(self.opened), 1)
    self.assertIn('/resolve/%s/' % e['revision'], self.opened[0])
    # Again: the file there matches; nothing is downloaded.
    code, _, _ = self.run_main(e)
    self.assertEqual(code, 0)
    self.assertEqual(len(self.opened), 1)

  def test_another_file_is_never_kept(self):
    e = entry()
    code, _, err = self.run_main(e, content=CONTENT[:-1] + b'\2')
    self.assertEqual(code, 1)
    self.assertIn('the pin is', err)
    self.assertEqual(os.listdir(self.tmp), [])
    # A longer one, nor a wrong file left there before.
    code, _, _ = self.run_main(e, content=CONTENT + b'more')
    self.assertEqual(code, 1)
    with open(os.path.join(self.tmp, e['file']), 'wb') as f:
      f.write(b'stale')
    code, _, _ = self.run_main(e, content=CONTENT)
    self.assertEqual(code, 0)
    with open(os.path.join(self.tmp, e['file']), 'rb') as f:
      self.assertEqual(f.read(), CONTENT)

  def test_refuses_what_the_browser_wouldnt_load(self):
    for e in (entry(hidden=True), entry(sha256=''), entry(revision='main'),
              entry(size='0'), entry(file='../x.gguf')):
      code, _, _ = self.run_main(e)
      self.assertEqual(code, 2, e)
    self.assertEqual(self.opened, [])
    code, _, _ = self.run_main(None)
    self.assertEqual(code, 2)
    self.assertEqual(fetch_model.main(['../etc', self.tmp]), 2)

  def test_a_hidden_entry_with_allow_hidden_but_only_at_its_pin(self):
    # The spoken test kit measures a hidden entry before it is offered: it
    # is fetched at its pin as any other.
    code, out, err = self.run_main(entry(hidden=True), flags=['--allow-hidden'])
    self.assertEqual(code, 0, err)
    self.assertTrue(out.strip().endswith('.gguf'))
    # Never one that isn't pinned, and the flag comes first or not at all.
    for e in (entry(hidden=True, sha256=''), entry(hidden=True, size='0')):
      code, _, _ = self.run_main(e, flags=['--allow-hidden'])
      self.assertEqual(code, 2, e)
    self.assertEqual(
        fetch_model.main(['gemma4-e4b', self.tmp, '--allow-hidden']), 2)


if __name__ == '__main__':
  unittest.main()
