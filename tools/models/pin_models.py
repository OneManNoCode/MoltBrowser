#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Pin the on-device model registries to exact Hugging Face files.

For every entry of the registries (the chat models' kModelRegistry in
src/chrome/browser/molt_ai/runtime/browser_ai_runtime.cc, and the memory
search model in src/chrome/browser/molt_ai/memory/embedding_model_registry.cc)
this queries

    https://huggingface.co/api/models/<repo>/revision/<rev>?blobs=true

and fills the entry's /*revision=*/ (commit sha), /*sha256=*/ (the GGUF's
LFS sha256) and /*size=*/ (exact byte size). The browser then downloads
from /resolve/<revision>/<file> and rejects any file whose size or SHA-256
differs. Chat models without a hash are downloaded as "unverified"; the
memory search model is not offered at all until it is pinned (fail closed).

A model that sees images also has a vision projector entry
(kVisionProjectors in browser_ai_runtime.cc): its /*mmproj_sha256=*/ and
/*mmproj_size=*/ are pinned from the same API reply, so from the same
revision as the model. Both are pinned or neither: if the projector isn't
in the reply, the model isn't pinned either. Such a model is offered only
once both are pinned (fail closed), and the browser downloads, verifies,
counts and deletes the two files as one unit
(src/chrome/browser/molt_ai/models/model_download_plan.h).

Usage:
  tools/models/pin_models.py                  # dry run: show what changes
  tools/models/pin_models.py --write          # rewrite the registries
  tools/models/pin_models.py --only qwen3-8b  # one entry
  tools/models/pin_models.py --revision <sha> --only qwen3-8b
  tools/models/pin_models.py --from-json DIR  # offline: DIR/<owner>__<repo>.json
  tools/models/pin_models.py --registry FILE  # only this registry (repeatable)

Set HF_TOKEN for gated repositories. Review the diff, then re-check the
licenses of new revisions before committing. Hidden registry entries
(`hidden` = true, the trailing bool) are pinned too but stay hidden: flip
them to false by hand once the repo/file names are confirmed. The API's
sha256 is what Hugging Face computed on upload: check it independently
against a download before committing (docs/design/SEMANTIC_SEARCH.md,
"Pinning the model").
"""

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request

_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..',
                    'src', 'chrome', 'browser', 'molt_ai')
REGISTRY = os.path.join(_SRC, 'runtime', 'browser_ai_runtime.cc')
EMBEDDING_REGISTRY = os.path.join(_SRC, 'memory', 'embedding_model_registry.cc')
REGISTRIES = [REGISTRY, EMBEDDING_REGISTRY]

# One registry entry: {"<id>", "<display>", "<repo>", "<file>",
#   /*revision=*/"...", /*sha256=*/"...", /*size=*/NULL, ...
ENTRY_RE = re.compile(
    r'\{"(?P<id>[^"]+)",\s*'
    r'"(?P<display>[^"]*)",\s*'
    r'"(?P<repo>[^"]+)",(?P<repo_tail>[^\n]*)\n\s*'
    r'"(?P<file>[^"]+)",(?P<file_tail>[^\n]*)\n\s*'
    r'/\*revision=\*/"(?P<revision>[^"]*)",\s*'
    r'/\*sha256=\*/"(?P<sha256>[^"]*)",\s*'
    r'/\*size=\*/(?P<size>\d+)ULL', re.S)

# One vision projector entry: {"<model id>", "<file>",
#   /*mmproj_sha256=*/"...", /*mmproj_size=*/NULL},
PROJECTOR_RE = re.compile(
    r'\{"(?P<id>[^"]+)",\s*'
    r'"(?P<file>[^"]+)",(?P<file_tail>[^\n]*)\n\s*'
    r'/\*mmproj_sha256=\*/"(?P<sha256>[^"]*)",\s*'
    r'/\*mmproj_size=\*/(?P<size>\d+)ULL', re.S)

SHA1_RE = re.compile(r'^[0-9a-f]{40}$')
SHA256_RE = re.compile(r'^[0-9a-f]{64}$')


def fetch_info(repo, revision, from_json):
  if from_json:
    path = os.path.join(from_json, repo.replace('/', '__') + '.json')
    with open(path) as f:
      return json.load(f)
  url = 'https://huggingface.co/api/models/%s/revision/%s?blobs=true' % (
      repo, revision)
  req = urllib.request.Request(url, headers={'User-Agent': 'molt-pin/1'})
  token = os.environ.get('HF_TOKEN')
  if token:
    req.add_header('Authorization', 'Bearer ' + token)
  with urllib.request.urlopen(req, timeout=60) as resp:
    return json.load(resp)


def pin_entry(entry, info):
  """Returns (revision, sha256, size) for entry['file'] from an API reply."""
  revision = info.get('sha', '')
  if not SHA1_RE.match(revision):
    raise ValueError('API reply has no commit sha')
  for sib in info.get('siblings', []):
    if sib.get('rfilename') != entry['file']:
      continue
    lfs = sib.get('lfs') or {}
    sha256 = lfs.get('sha256', '')
    size = lfs.get('size', sib.get('size'))
    if not SHA256_RE.match(sha256):
      raise ValueError('%s is not an LFS file with a sha256' % entry['file'])
    if not isinstance(size, int) or size <= 0:
      raise ValueError('%s has no size' % entry['file'])
    return revision, sha256, size
  raise ValueError('%s not found in %s@%s' % (entry['file'], entry['repo'],
                                              revision))


def pin_projector_text(text, sha256, size):
  text = re.sub(r'/\*mmproj_sha256=\*/"[^"]*"',
                '/*mmproj_sha256=*/"%s"' % sha256, text)
  return re.sub(r'/\*mmproj_size=\*/\d+ULL', '/*mmproj_size=*/%dULL' % size,
                text)


def pin_registry(path, args):
  """Pins the entries of one registry file. Returns (changed, failures)."""
  with open(path) as f:
    src = f.read()
  entries = list(ENTRY_RE.finditer(src))
  projectors = {m.group('id'): m.groupdict()
                for m in PROJECTOR_RE.finditer(src)}
  # Model id -> (sha256, size) of its projector, from the model's reply.
  projector_pins = {}
  out = []
  last = 0
  changed = 0
  failures = 0
  for m in entries:
    e = m.groupdict()
    out.append(src[last:m.start()])
    last = m.end()
    text = m.group(0)
    if args.only and e['id'] not in args.only:
      out.append(text)
      continue
    try:
      info = fetch_info(e['repo'], args.revision, args.from_json)
      revision, sha256, size = pin_entry(e, info)
      projector = projectors.get(e['id'])
      if projector:
        # The same reply, so the same revision as the model.
        _, p_sha256, p_size = pin_entry(
            {'file': projector['file'], 'repo': e['repo']}, info)
        projector_pins[e['id']] = (p_sha256, p_size)
    except (OSError, ValueError, urllib.error.URLError, json.JSONDecodeError) as err:
      print('FAIL %-20s %s: %s' % (e['id'], e['repo'], err), file=sys.stderr)
      failures += 1
      out.append(text)
      continue
    new_text = text
    new_text = re.sub(r'/\*revision=\*/"[^"]*"',
                      '/*revision=*/"%s"' % revision, new_text)
    new_text = re.sub(r'/\*sha256=\*/"[^"]*"', '/*sha256=*/"%s"' % sha256,
                      new_text)
    new_text = re.sub(r'/\*size=\*/\d+ULL', '/*size=*/%dULL' % size, new_text)
    if new_text != text:
      changed += 1
      print('pin  %-20s %s@%s sha256=%s size=%d' %
            (e['id'], e['repo'], revision[:12], sha256[:16] + '...', size))
    else:
      print('ok   %-20s already pinned' % e['id'])
    out.append(new_text)
  out.append(src[last:])

  if projector_pins:
    joined = ''.join(out)
    out = []
    last = 0
    for m in PROJECTOR_RE.finditer(joined):
      out.append(joined[last:m.start()])
      last = m.end()
      text = m.group(0)
      pin = projector_pins.get(m.group('id'))
      if pin:
        new_text = pin_projector_text(text, *pin)
        if new_text != text:
          changed += 1
          print('pin  %-20s projector %s sha256=%s size=%d' %
                (m.group('id'), m.group('file'), pin[0][:16] + '...', pin[1]))
        text = new_text
      out.append(text)
    out.append(joined[last:])

  if args.write and changed:
    with open(path, 'w') as f:
      f.write(''.join(out))
    print('wrote %d change(s) to %s' % (changed, path))
  return changed, failures


def main(argv):
  ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
  ap.add_argument('--write', action='store_true',
                  help='rewrite the registry files in place')
  ap.add_argument('--only', action='append', default=[],
                  help='model id(s) to pin (default: all)')
  ap.add_argument('--revision', default='main',
                  help='repo revision to pin to (default: main)')
  ap.add_argument('--from-json', help='read API replies from this directory')
  ap.add_argument('--registry', action='append', default=[],
                  help='registry file(s) to pin (default: the chat models '
                  'and the memory search model)')
  args = ap.parse_args(argv)
  registries = args.registry or REGISTRIES

  ids = set()
  for path in registries:
    with open(path) as f:
      found = {m.group('id') for m in ENTRY_RE.finditer(f.read())}
    if not found:
      print('no registry entries found in %s' % path, file=sys.stderr)
      return 2
    ids |= found
  unknown = set(args.only) - ids
  if unknown:
    print('unknown model id(s): %s' % ', '.join(sorted(unknown)),
          file=sys.stderr)
    return 2

  changed = 0
  failures = 0
  for path in registries:
    c, f = pin_registry(path, args)
    changed += c
    failures += f
  if changed and not args.write:
    print('%d change(s); re-run with --write to apply' % changed)
  return 1 if failures else 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
