#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Lists the tensor types of GGUF files and checks them against the weight
types MoltBrowser's Vulkan build has shaders for.

The GPU build leaves out the Vulkan shaders of weight types no catalogue
model uses (`_ggml_vk_dropped_types` in src/third_party/llama_cpp/BUILD.gn,
docs/design/SANDBOXED_INFERENCE.md section 11.14). A model with tensors in
one of those types still works, but the operations on them run on the CPU.
Run this on the files of a model before adding or re-pinning it:

  tools/models/gguf_types.py model.gguf [mmproj.gguf ...]

Prints each file's tensor count per type, and exits 1 if a file has a
tensor in a type the Vulkan build left out. Reads only the GGUF header and
tensor table (no weights), with bounds on every count and length.
"""

import argparse
import os
import re
import struct
import sys

BUILD_GN = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                        '..', 'src', 'third_party', 'llama_cpp', 'BUILD.gn')

# ggml_type (ggml/include/ggml.h) -> the name ggml_type_name() gives it.
GGML_TYPES = {
    0: 'f32', 1: 'f16', 2: 'q4_0', 3: 'q4_1', 6: 'q5_0', 7: 'q5_1',
    8: 'q8_0', 9: 'q8_1', 10: 'q2_K', 11: 'q3_K', 12: 'q4_K', 13: 'q5_K',
    14: 'q6_K', 15: 'q8_K', 16: 'iq2_xxs', 17: 'iq2_xs', 18: 'iq3_xxs',
    19: 'iq1_s', 20: 'iq4_nl', 21: 'iq3_s', 22: 'iq2_s', 23: 'iq4_xs',
    24: 'i8', 25: 'i16', 26: 'i32', 27: 'i64', 28: 'f64', 29: 'iq1_m',
    30: 'bf16', 34: 'tq1_0', 35: 'tq2_0', 39: 'mxfp4', 40: 'nvfp4',
    41: 'q1_0', 42: 'q2_0',
}

# GGUF metadata value types -> struct format (fixed size) or None.
_SCALARS = {0: '<B', 1: '<b', 2: '<H', 3: '<h', 4: '<I', 5: '<i', 6: '<f',
            7: '<B', 10: '<Q', 11: '<q', 12: '<d'}
_STRING, _ARRAY = 8, 9

MAX_COUNT = 1 << 20      # tensors, metadata entries, array items
MAX_STRING = 1 << 24     # bytes
MAX_DIMS = 8


class GGUFError(ValueError):
  pass


class _Reader:

  def __init__(self, f):
    self.f = f

  def read(self, n):
    data = self.f.read(n)
    if len(data) != n:
      raise GGUFError('truncated GGUF header')
    return data

  def unpack(self, fmt):
    return struct.unpack(fmt, self.read(struct.calcsize(fmt)))[0]

  def count(self, limit=MAX_COUNT):
    n = self.unpack('<Q')
    if n > limit:
      raise GGUFError('implausible count %d' % n)
    return n

  def string(self):
    return self.read(self.count(MAX_STRING))

  def skip_value(self, vtype, depth=0):
    if vtype in _SCALARS:
      self.read(struct.calcsize(_SCALARS[vtype]))
    elif vtype == _STRING:
      self.string()
    elif vtype == _ARRAY and depth == 0:
      item = self.unpack('<I')
      for _ in range(self.count()):
        self.skip_value(item, depth + 1)
    else:
      raise GGUFError('unknown metadata value type %d' % vtype)


def tensor_types(path):
  """{type name: tensor count} of the GGUF at |path|."""
  with open(path, 'rb') as f:
    r = _Reader(f)
    if r.read(4) != b'GGUF':
      raise GGUFError('not a GGUF file')
    version = r.unpack('<I')
    if version not in (2, 3):
      raise GGUFError('unsupported GGUF version %d' % version)
    n_tensors = r.count()
    n_kv = r.count()
    for _ in range(n_kv):
      r.string()
      r.skip_value(r.unpack('<I'))
    counts = {}
    for _ in range(n_tensors):
      r.string()
      n_dims = r.unpack('<I')
      if n_dims > MAX_DIMS:
        raise GGUFError('implausible tensor rank %d' % n_dims)
      r.read(8 * n_dims)
      ggml_type = r.unpack('<I')
      r.unpack('<Q')  # offset
      name = GGML_TYPES.get(ggml_type, 'type%d' % ggml_type)
      counts[name] = counts.get(name, 0) + 1
    return counts


def dropped_types(build_gn=BUILD_GN):
  """The weight types the Vulkan build has no shaders for."""
  with open(build_gn) as f:
    text = f.read()
  m = re.search(r'_ggml_vk_dropped_types\s*=\s*\[(.*?)\]', text, re.S)
  if not m:
    raise GGUFError('no _ggml_vk_dropped_types in %s' % build_gn)
  return set(re.findall(r'"([^"]+)"', m.group(1)))


def main(argv):
  ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
  ap.add_argument('files', nargs='+')
  ap.add_argument('--build-gn', default=BUILD_GN)
  args = ap.parse_args(argv)
  dropped = dropped_types(args.build_gn)
  status = 0
  for path in args.files:
    try:
      counts = tensor_types(path)
    except (OSError, GGUFError, struct.error) as err:
      print('FAIL %s: %s' % (path, err), file=sys.stderr)
      status = 1
      continue
    print('%s: %s' % (path, ', '.join(
        '%s x%d' % (t, n) for t, n in sorted(counts.items()))))
    # ggml_type_name() names k-quants with a capital K; the build's list is
    # lower case.
    left_out = sorted(t for t in counts if t.lower() in dropped)
    if left_out:
      print('  on the CPU with the Vulkan build: %s' % ', '.join(left_out))
      status = 1
  return status


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
