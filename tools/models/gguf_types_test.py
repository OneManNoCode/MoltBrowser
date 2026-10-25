#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for gguf_types.py on small GGUF headers written here.

Run: python3 tools/models/gguf_types_test.py -v
"""

import contextlib
import io
import os
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gguf_types  # noqa: E402


def gguf_string(s):
  b = s.encode()
  return struct.pack('<Q', len(b)) + b


def header(tensors, kv=(), version=3):
  """A GGUF header: |kv| as (key, type, packed value), |tensors| as (name,
  dims, ggml type). No tensor data (the tool never reads it)."""
  out = b'GGUF' + struct.pack('<IQQ', version, len(tensors), len(kv))
  for key, vtype, value in kv:
    out += gguf_string(key) + struct.pack('<I', vtype) + value
  for name, dims, ggml_type in tensors:
    out += gguf_string(name) + struct.pack('<I', len(dims))
    out += b''.join(struct.pack('<Q', d) for d in dims)
    out += struct.pack('<IQ', ggml_type, 0)
  return out


KV = [
    ('general.architecture', 8, gguf_string('llama')),
    ('llama.block_count', 4, struct.pack('<I', 2)),
    ('tokenizer.ggml.tokens', 9,
     struct.pack('<IQ', 8, 2) + gguf_string('a') + gguf_string('b')),
    ('tokenizer.ggml.scores', 9,
     struct.pack('<IQ', 6, 2) + struct.pack('<ff', 0.0, -1.0)),
    ('general.flag', 7, b'\x01'),
]


class GGUFTypesTest(unittest.TestCase):

  def setUp(self):
    self.tmp = tempfile.mkdtemp()
    self.addCleanup(shutil.rmtree, self.tmp)

  def write(self, name, data):
    path = os.path.join(self.tmp, name)
    with open(path, 'wb') as f:
      f.write(data)
    return path

  def run_main(self, *argv):
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
      code = gguf_types.main(list(argv))
    return code, out.getvalue() + err.getvalue()

  def test_counts_types_past_the_metadata(self):
    path = self.write('m.gguf', header(
        [('token_embd.weight', [256, 32], 14), ('blk.0.attn_q.weight',
                                                [256, 256], 12),
         ('blk.0.attn_k.weight', [256, 64], 12),
         ('output_norm.weight', [256], 0)], KV))
    self.assertEqual(gguf_types.tensor_types(path),
                     {'q6_K': 1, 'q4_K': 2, 'f32': 1})

  def test_the_build_list_is_found(self):
    dropped = gguf_types.dropped_types()
    self.assertIn('iq4_xs', dropped)
    self.assertIn('q4_0', dropped)
    # What the catalogue's models use is kept.
    for kept in ('q4_k', 'q5_k', 'q6_k', 'q8_0', 'f16', 'f32', 'mxfp4'):
      self.assertNotIn(kept, dropped)

  def test_a_left_out_type_fails(self):
    ok = self.write('ok.gguf', header([('w', [256, 256], 12),
                                       ('n', [256], 0)], KV))
    left_out = self.write('iq.gguf', header([('w', [256, 256], 23)], KV))
    code, log = self.run_main(ok)
    self.assertEqual(code, 0, log)
    code, log = self.run_main(ok, left_out)
    self.assertEqual(code, 1)
    self.assertIn('iq4_xs', log)

  def test_refuses_what_is_not_a_header(self):
    bad = [
        b'',
        b'GGML' + bytes(20),
        header([], version=7),
        header([('w', [1] * 9, 0)]),
        header([('w', [256], 0)])[:-6],
        b'GGUF' + struct.pack('<IQQ', 3, 1 << 40, 0),
        b'GGUF' + struct.pack('<IQQ', 3, 0, 1) + gguf_string('k') +
        struct.pack('<I', 99),
        # An array of arrays.
        b'GGUF' + struct.pack('<IQQ', 3, 0, 1) + gguf_string('k') +
        struct.pack('<IIQ', 9, 9, 1) + struct.pack('<IQ', 4, 0),
    ]
    for i, data in enumerate(bad):
      path = self.write('bad%d.gguf' % i, data)
      with self.assertRaises(gguf_types.GGUFError, msg=i):
        gguf_types.tensor_types(path)
      code, log = self.run_main(path)
      self.assertEqual(code, 1, i)
      self.assertIn('FAIL', log)


if __name__ == '__main__':
  unittest.main()
