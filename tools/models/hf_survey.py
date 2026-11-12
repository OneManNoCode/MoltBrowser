#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""What Hugging Face has for each on-device model registry entry.

For the registries tools/models/pin_models.py pins, from a machine that can
reach huggingface.co (the model-pin workflow, .github/workflows/
molt-model-pin.yml):

  survey  For each entry: the repo's revision, the model card's license,
          whether the repo is gated, whether the registry's file (and its
          projector, if any) is in it, and the repo's GGUF files when it
          isn't. Saves each API reply as OUT/<owner>__<repo>.json, the form
          pin_models.py --from-json reads.
  search  The GGUF repos a query finds ("Qwen3.5-4B"), most downloaded
          first: for an entry whose repo name is unconfirmed.
  card    The license and gating of repos named (a GGUF repo's base
          model, "google/gemma-4-E4B-it"): the terms the weights come
          under, when a conversion's card doesn't say.
  verify  Downloads an entry's file at its pinned revision and hashes it
          here, against the sha256 and size the API gave (pin_models.py's
          docstring: check the API's hash independently before
          committing). Streamed: nothing is kept on disk.

Everything a reply says is printed as data, cleaned of control characters;
nothing is executed or unpickled.

Usage:
  hf_survey.py survey OUT [--only ID ...]
  hf_survey.py search QUERY ...
  hf_survey.py card REPO ...
  hf_survey.py verify OUT ID ...
"""

import argparse
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pin_models  # noqa: E402

API = 'https://huggingface.co/api/models/'
SHA1_RE = re.compile(r'^[0-9a-f]{40}$')
SHA256_RE = re.compile(r'^[0-9a-f]{64}$')
# Repo ids as Hugging Face names them: owner/name, letters, digits, . _ -.
REPO_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]*/[A-Za-z0-9][A-Za-z0-9._-]*$')
MAX_GGUF_LISTED = 60


def clean(value, cap=200):
  """|value| as printable text: no control characters, at most |cap|."""
  text = str(value)
  text = ''.join(c if c.isprintable() else '?' for c in text)
  return text if len(text) <= cap else text[:cap] + '...'


def request(url):
  req = urllib.request.Request(url, headers={'User-Agent': 'molt-pin/1'})
  token = os.environ.get('HF_TOKEN')
  if token:
    req.add_header('Authorization', 'Bearer ' + token)
  return req


def get_json(url):
  """(status, reply or None)."""
  try:
    with urllib.request.urlopen(request(url), timeout=60) as resp:
      return resp.status, json.load(resp)
  except urllib.error.HTTPError as err:
    return err.code, None
  except (urllib.error.URLError, OSError, json.JSONDecodeError) as err:
    return clean(err), None


def entries(only):
  out = []
  for path in pin_models.REGISTRIES:
    with open(path) as f:
      src = f.read()
    projectors = {m.group('id'): m.group('file')
                  for m in pin_models.PROJECTOR_RE.finditer(src)}
    for m in pin_models.ENTRY_RE.finditer(src):
      e = m.groupdict()
      if only and e['id'] not in only:
        continue
      e['projector'] = projectors.get(e['id'], '')
      e['registry'] = os.path.relpath(path)
      out.append(e)
  return out


def json_path(out_dir, repo):
  return os.path.join(out_dir, repo.replace('/', '__') + '.json')


def sibling(info, name):
  for sib in info.get('siblings', []) or []:
    if isinstance(sib, dict) and sib.get('rfilename') == name:
      return sib
  return None


def describe_file(info, name):
  sib = sibling(info, name)
  if sib is None:
    return 'MISSING'
  lfs = sib.get('lfs') or {}
  sha256 = lfs.get('sha256', '')
  size = lfs.get('size', sib.get('size'))
  if not SHA256_RE.match(str(sha256)) or not isinstance(size, int):
    return 'present, but not an LFS file with a sha256 and size'
  return 'found, %d bytes, sha256 %s' % (size, sha256)


def license_of(info):
  card = info.get('cardData') or {}
  names = []
  if isinstance(card, dict) and card.get('license'):
    names.append(clean(card.get('license'), 60))
  for tag in info.get('tags', []) or []:
    if isinstance(tag, str) and tag.startswith('license:'):
      names.append(clean(tag[len('license:'):], 60))
  seen = []
  for n in names:
    if n not in seen:
      seen.append(n)
  return ', '.join(seen) or '(none given)'


def survey(args):
  os.makedirs(args.out, exist_ok=True)
  seen = {}
  problems = 0
  for e in entries(args.only):
    repo = e['repo']
    print('== %s (%s)' % (e['id'], e['registry']))
    print('   repo %s, file %s%s' % (repo, e['file'],
                                     ', projector ' + e['projector']
                                     if e['projector'] else ''))
    if not REPO_RE.match(repo):
      print('   NOT A REPO NAME')
      problems += 1
      continue
    if repo not in seen:
      url = API + urllib.parse.quote(repo, safe='/') + '/revision/main?blobs=true'
      seen[repo] = get_json(url)
      status, info = seen[repo]
      if isinstance(info, dict):
        with open(json_path(args.out, repo), 'w') as f:
          json.dump(info, f)
    status, info = seen[repo]
    if not isinstance(info, dict):
      print('   NO REPLY: %s (no such repo, gated or private)' % status)
      problems += 1
      continue
    revision = info.get('sha', '')
    print('   revision %s; license %s; gated %s; last modified %s' %
          (clean(revision, 40) if SHA1_RE.match(str(revision)) else
           '(none)', license_of(info), clean(info.get('gated', False), 20),
           clean(info.get('lastModified', '?'), 30)))
    model = describe_file(info, e['file'])
    print('   file: %s' % model)
    ok = model.startswith('found')
    if e['projector']:
      projector = describe_file(info, e['projector'])
      print('   projector: %s' % projector)
      ok = ok and projector.startswith('found')
    if not ok:
      problems += 1
      ggufs = [s for s in info.get('siblings', []) or []
               if isinstance(s, dict) and
               str(s.get('rfilename', '')).endswith('.gguf')]
      print('   its GGUF files (%d):' % len(ggufs))
      for s in ggufs[:MAX_GGUF_LISTED]:
        lfs = s.get('lfs') or {}
        print('     %s  %s' % (clean(s.get('rfilename'), 120),
                               clean(lfs.get('size', s.get('size', '?')), 20)))
  print('survey: %d problem(s)' % problems)
  return 0


def search(args):
  for query in args.queries:
    url = API.rstrip('/') + '?' + urllib.parse.urlencode({
        'search': query, 'filter': 'gguf', 'sort': 'downloads',
        'direction': '-1', 'limit': '15'})
    status, reply = get_json(url)
    print('== search %s' % clean(query, 80))
    if not isinstance(reply, list):
      print('   NO REPLY: %s' % status)
      continue
    for m in reply:
      if isinstance(m, dict):
        print('   %-60s downloads %-9s gated %s' %
              (clean(m.get('id', '?'), 60), clean(m.get('downloads', '?'), 9),
               clean(m.get('gated', False), 10)))
  return 0


def card(args):
  for repo in args.repos:
    print('== card %s' % clean(repo, 100))
    if not REPO_RE.match(repo):
      print('   NOT A REPO NAME')
      continue
    status, info = get_json(API + urllib.parse.quote(repo, safe='/'))
    if not isinstance(info, dict):
      print('   NO REPLY: %s' % status)
      continue
    card_data = info.get('cardData') or {}
    base = card_data.get('base_model', '') if isinstance(card_data, dict) else ''
    print('   license %s; gated %s; base model %s' %
          (license_of(info), clean(info.get('gated', False), 20),
           clean(base or '(none given)', 100)))
  return 0


def verify(args):
  failures = 0
  by_id = {e['id']: e for e in entries(args.ids)}
  for model_id in args.ids:
    e = by_id.get(model_id)
    if e is None:
      print('verify %s: no such registry entry' % clean(model_id, 40))
      failures += 1
      continue
    try:
      with open(json_path(args.out, e['repo'])) as f:
        info = json.load(f)
    except (OSError, json.JSONDecodeError):
      print('verify %s: no API reply surveyed' % model_id)
      failures += 1
      continue
    revision = info.get('sha', '')
    files = [e['file']] + ([e['projector']] if e['projector'] else [])
    for name in files:
      sib = sibling(info, name)
      lfs = (sib or {}).get('lfs') or {}
      want_sha = lfs.get('sha256', '')
      want_size = lfs.get('size')
      if (not SHA1_RE.match(str(revision)) or
          not SHA256_RE.match(str(want_sha)) or
          not isinstance(want_size, int)):
        print('verify %s %s: nothing to check against' % (model_id, name))
        failures += 1
        continue
      url = 'https://huggingface.co/%s/resolve/%s/%s' % (
          urllib.parse.quote(e['repo'], safe='/'), revision,
          urllib.parse.quote(name))
      digest = hashlib.sha256()
      size = 0
      start = time.time()
      try:
        with urllib.request.urlopen(request(url), timeout=120) as resp:
          while True:
            chunk = resp.read(1 << 20)
            if not chunk:
              break
            digest.update(chunk)
            size += len(chunk)
            if size > want_size:
              break
      except (urllib.error.URLError, OSError) as err:
        print('verify %s %s: download failed: %s' % (model_id, name,
                                                     clean(err)))
        failures += 1
        continue
      got = digest.hexdigest()
      same = got == want_sha and size == want_size
      print('verify %s %s: %s (%d bytes in %.0f s, sha256 %s)' %
            (model_id, name, 'VERIFIED' if same else 'MISMATCH', size,
             time.time() - start, got))
      failures += 0 if same else 1
  return 1 if failures else 0


def main(argv):
  ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
  sub = ap.add_subparsers(dest='command', required=True)
  s = sub.add_parser('survey')
  s.add_argument('out')
  s.add_argument('--only', action='append', default=[])
  q = sub.add_parser('search')
  q.add_argument('queries', nargs='+')
  c = sub.add_parser('card')
  c.add_argument('repos', nargs='+')
  v = sub.add_parser('verify')
  v.add_argument('out')
  v.add_argument('ids', nargs='+')
  args = ap.parse_args(argv)
  return {'survey': survey, 'search': search, 'card': card,
          'verify': verify}[args.command](args)


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
