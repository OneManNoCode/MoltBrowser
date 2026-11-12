#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for hf_survey.py, offline: the API and the downloads are stand-ins.

Run: python3 tools/models/hf_survey_test.py -v
"""

import contextlib
import hashlib
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hf_survey  # noqa: E402

REVISION = '0123456789abcdef0123456789abcdef01234567'
CONTENT = b'GGUF' + b'\0' * 60


def gguf_string(text):
  data = text.encode()
  return struct.pack('<Q', len(data)) + data


def tiny_gguf():
  """A GGUF header: three metadata entries and one q4_K tensor."""
  kv = (gguf_string('general.architecture') + struct.pack('<I', 8) +
        gguf_string('qwen3') +
        gguf_string('qwen3.context_length') + struct.pack('<II', 4, 40960) +
        gguf_string('tokenizer.chat_template') + struct.pack('<I', 8) +
        gguf_string('<|im_start|>{{ m }}'))
  tensor = (gguf_string('blk.0.attn_q.weight') + struct.pack('<I', 1) +
            struct.pack('<Q', 8) + struct.pack('<I', 12) +
            struct.pack('<Q', 0))
  return (b'GGUF' + struct.pack('<IQQ', 3, 1, 3) + kv + tensor +
          b'\0' * 32)


def reply(files, license_name='apache-2.0'):
  return {
      'sha': REVISION,
      'gated': False,
      'cardData': {'license': license_name},
      'tags': ['gguf', 'license:' + license_name],
      'siblings': [{'rfilename': name,
                    'lfs': {'sha256': sha, 'size': size}}
                   for name, sha, size in files],
  }


class Response(io.BytesIO):

  def __enter__(self):
    return self

  def __exit__(self, *unused):
    self.close()


def run(fn, args):
  out = io.StringIO()
  with contextlib.redirect_stdout(out):
    code = fn(args)
  return code, out.getvalue()


class HfSurveyTest(unittest.TestCase):

  def setUp(self):
    self.tmp = tempfile.mkdtemp()
    self.addCleanup(shutil.rmtree, self.tmp)
    self.entry = next(e for e in hf_survey.entries([])
                      if e['id'] == 'tinyllama-1.1b')

  def test_clean_drops_control_characters(self):
    self.assertEqual(hf_survey.clean('a\x1b[31mb\nc'), 'a?[31mb?c')
    self.assertEqual(hf_survey.clean('x' * 10, cap=4), 'xxxx...')

  def test_license_and_files(self):
    info = reply([(self.entry['file'], 'ab' * 32, 64)], 'mit')
    self.assertEqual(hf_survey.license_of(info), 'mit')
    self.assertTrue(
        hf_survey.describe_file(info, self.entry['file']).startswith('found'))
    self.assertEqual(hf_survey.describe_file(info, 'other.gguf'), 'MISSING')
    no_lfs = {'siblings': [{'rfilename': 'a.gguf', 'size': 3}]}
    self.assertIn('not an LFS file', hf_survey.describe_file(no_lfs, 'a.gguf'))

  def test_survey_saves_replies_and_lists_files_when_missing(self):
    info = reply([('other-Q4_K_M.gguf', 'ab' * 32, 64)])
    with mock.patch.object(hf_survey, 'get_json',
                           return_value=(200, info)) as get:
      code, out = run(hf_survey.survey,
                      mock.Mock(out=self.tmp, only=['tinyllama-1.1b']))
    self.assertEqual(code, 0)
    self.assertEqual(get.call_count, 1)
    self.assertIn('file: MISSING', out)
    self.assertIn('other-Q4_K_M.gguf', out)
    self.assertIn('survey: 1 problem(s)', out)
    with open(hf_survey.json_path(self.tmp, self.entry['repo'])) as f:
      self.assertEqual(json.load(f)['sha'], REVISION)

  def test_survey_without_a_reply(self):
    with mock.patch.object(hf_survey, 'get_json', return_value=(404, None)):
      _, out = run(hf_survey.survey,
                   mock.Mock(out=self.tmp, only=['tinyllama-1.1b']))
    self.assertIn('NO REPLY: 404', out)
    self.assertFalse(os.listdir(self.tmp))

  def test_card(self):
    info = {'cardData': {'license': 'apache-2.0',
                         'base_model': 'google/gemma-4-E4B-it'},
            'gated': 'manual'}
    with mock.patch.object(hf_survey, 'get_json', return_value=(200, info)):
      _, out = run(hf_survey.card,
                   mock.Mock(repos=['google/gemma-4-E4B-it', '../x']))
    self.assertIn('license apache-2.0; gated manual', out)
    self.assertIn('NOT A REPO NAME', out)

  def write_reply(self, sha256, size):
    with open(hf_survey.json_path(self.tmp, self.entry['repo']), 'w') as f:
      json.dump(reply([(self.entry['file'], sha256, size)]), f)

  def verify(self, content=CONTENT):
    opened = []

    def urlopen(req, timeout):
      opened.append(req.full_url)
      return Response(content)

    with mock.patch.object(hf_survey.urllib.request, 'urlopen', urlopen):
      code, out = run(hf_survey.verify,
                      mock.Mock(out=self.tmp, ids=['tinyllama-1.1b']))
    return code, out, opened

  def test_verify_matches_the_pin(self):
    self.write_reply(hashlib.sha256(CONTENT).hexdigest(), len(CONTENT))
    code, out, opened = self.verify()
    self.assertEqual(code, 0, out)
    self.assertIn('VERIFIED', out)
    self.assertEqual(len(opened), 1)
    self.assertIn('/resolve/%s/' % REVISION, opened[0])

  def test_verify_reads_the_gguf_header(self):
    content = tiny_gguf()
    self.write_reply(hashlib.sha256(content).hexdigest(), len(content))
    code, out, _ = self.verify(content)
    self.assertEqual(code, 0, out)
    self.assertIn('architecture qwen3', out)
    self.assertIn('context 40960', out)
    self.assertIn('chat template 19 characters, markers <|im_start|>', out)
    self.assertIn('tensors: q4_K 1', out)
    self.assertIn('Vulkan: every type has shaders', out)
    # Not a GGUF: said so, and the hash still decides.
    self.write_reply(hashlib.sha256(CONTENT).hexdigest(), len(CONTENT))
    code, out, _ = self.verify()
    self.assertEqual(code, 0, out)
    self.assertIn('header: unreadable', out)

  def test_verify_catches_another_file(self):
    self.write_reply('ab' * 32, len(CONTENT))
    code, out, _ = self.verify()
    self.assertEqual(code, 1)
    self.assertIn('MISMATCH', out)
    # A longer file than the pin says is a mismatch too, read no further.
    self.write_reply(hashlib.sha256(CONTENT[:8]).hexdigest(), 8)
    code, out, _ = self.verify()
    self.assertEqual(code, 1)
    self.assertIn('MISMATCH', out)

  def test_verify_needs_a_surveyed_pin(self):
    code, out, opened = self.verify()
    self.assertEqual(code, 1)
    self.assertIn('no API reply surveyed', out)
    self.assertEqual(opened, [])


if __name__ == '__main__':
  unittest.main()
