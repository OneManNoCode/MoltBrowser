#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Downloads one on-device model at its pin, for a test that runs a real one.

  tools/models/fetch_model.py [--allow-hidden] <model id> <dir>

Reads the entry from the chat model registry (kModelRegistry in
src/chrome/browser/molt_ai/runtime/browser_ai_runtime.cc, as
pin_models.py does), refuses one that isn't pinned or is hidden (the
browser never loads a hidden entry; with --allow-hidden, one the spoken
test kit measures before it is offered, which the browser lists only with
its --molt-ai-test-hidden-models switch), and fetches
https://huggingface.co/<repo>/resolve/<revision>/<file> into <dir>/<file>,
hashing as it streams. The file is kept only if its size and SHA-256 are
the pin's; one already there is kept only if they still are. Prints the
file's path.

The browser hashes a model only while it downloads it, so a file put in its
models directory by hand (tools/voice_e2e's --model-file) must be checked
first: this is that check. A multimodal entry's projector isn't fetched.
"""

import hashlib
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pin_models  # noqa: E402

ID_RE = re.compile(r'^[a-z0-9][a-z0-9.-]{0,63}$')
CHUNK = 1 << 20


def find_entry(model_id, registry=pin_models.REGISTRY):
  """The registry entry |model_id|, with 'hidden', or None."""
  with open(registry) as f:
    src = f.read()
  for m in pin_models.ENTRY_RE.finditer(src):
    if m.group('id') != model_id:
      continue
    e = m.groupdict()
    # The entry's last field, its "hidden" bool, closes it: the first
    # "true}" or "false}" after its pinned values, before the array ends.
    end = src.find('\n};', m.end())
    flag = re.search(r'\b(true|false)\}', src[m.end():end if end != -1 else None])
    e['hidden'] = flag is None or flag.group(1) == 'true'
    return e
  return None


def sha256_of(path):
  digest = hashlib.sha256()
  with open(path, 'rb') as f:
    while True:
      chunk = f.read(CHUNK)
      if not chunk:
        return digest.hexdigest()
      digest.update(chunk)


def matches(path, entry):
  return (os.path.isfile(path) and
          os.path.getsize(path) == int(entry['size']) and
          sha256_of(path) == entry['sha256'])


def fetch(entry, path, urlopen=urllib.request.urlopen):
  """Downloads |entry|'s file to |path| through a .partial; False when its
  size or hash isn't the pin's (nothing is kept then)."""
  url = 'https://huggingface.co/%s/resolve/%s/%s' % (
      urllib.parse.quote(entry['repo'], safe='/'), entry['revision'],
      urllib.parse.quote(entry['file']))
  partial = path + '.partial'
  want = int(entry['size'])
  digest = hashlib.sha256()
  size = 0
  req = urllib.request.Request(url, headers={'User-Agent': 'molt-fetch/1'})
  try:
    with urlopen(req, timeout=120) as resp, open(partial, 'wb') as out:
      while size <= want:
        chunk = resp.read(CHUNK)
        if not chunk:
          break
        digest.update(chunk)
        out.write(chunk)
        size += len(chunk)
  except (urllib.error.URLError, OSError) as err:
    print('download failed: %s' % err, file=sys.stderr)
    if os.path.exists(partial):
      os.remove(partial)
    return False
  if size != want or digest.hexdigest() != entry['sha256']:
    print('%s: %d bytes, sha256 %s; the pin is %d bytes, sha256 %s' %
          (entry['file'], size, digest.hexdigest(), want, entry['sha256']),
          file=sys.stderr)
    os.remove(partial)
    return False
  os.replace(partial, path)
  return True


def main(argv, urlopen=urllib.request.urlopen):
  allow_hidden = bool(argv) and argv[0] == '--allow-hidden'
  if allow_hidden:
    argv = argv[1:]
  if len(argv) != 2 or not ID_RE.match(argv[0]):
    print(__doc__.split('\n\n')[1], file=sys.stderr)
    return 2
  model_id, out_dir = argv
  entry = find_entry(model_id)
  if entry is None:
    print('no registry entry %s' % model_id, file=sys.stderr)
    return 2
  if entry['hidden'] and not allow_hidden:
    print('%s is hidden: the browser never loads it' % model_id,
          file=sys.stderr)
    return 2
  if not (pin_models.SHA1_RE.match(entry['revision']) and
          pin_models.SHA256_RE.match(entry['sha256']) and
          int(entry['size']) > 0):
    print('%s is not pinned' % model_id, file=sys.stderr)
    return 2
  if os.path.basename(entry['file']) != entry['file']:
    print('%s: not a plain file name' % entry['file'], file=sys.stderr)
    return 2
  os.makedirs(out_dir, exist_ok=True)
  path = os.path.join(out_dir, entry['file'])
  if os.path.exists(path) and not matches(path, entry):
    os.remove(path)
  if not os.path.exists(path) and not fetch(entry, path, urlopen):
    return 1
  print(path)
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
