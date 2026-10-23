#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Offline signing for MoltBrowser releases.

MoltBrowser only installs an update whose installer matches a release
manifest signed by a key listed in
src/chrome/browser/molt_ai/update/update_signing_keys.cc. This tool creates
that key, writes and signs manifests, and verifies published releases.

`sign` also signs the release's build provenance (the
molt-provenance-<platform>-<arch>.json statements the release scripts wrote,
see molt_provenance.py) with the same key, into
molt-build-provenance.intoto.jsonl.

Run `keygen` and `sign` ONLY on an offline machine. The private key must never
be copied to a CI runner, a cloud machine, or this repository. See
docs/SIGNED_UPDATES.md for the full runbook.

Commands:
  keygen   --out FILE              create a passphrase-protected signing key
  pubkey   --key FILE              print the public key of a signing key
  add-key  PUBLIC_KEY              trust a public key in the browser build
  manifest --tag vX.Y.Z FILES...   write molt-release-manifest.json
  sign     --key FILE --assets-dir DIR
                                   write molt-release-manifest.json.sig and
                                   molt-build-provenance.intoto.jsonl
  verify   --manifest F --sig F [FILES...]
                                   check a signature (and asset hashes)

Requires: python3 -m pip install cryptography
"""

import argparse
import base64
import getpass
import hashlib
import json
import os
import re
import stat
import sys

try:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey, Ed25519PublicKey)
except ImportError:  # pragma: no cover
    sys.exit('This tool needs the "cryptography" package: '
             'python3 -m pip install cryptography')

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import molt_provenance as prov  # noqa: E402  (same folder)

# Must match release_manifest.h exactly.
SIGNING_CONTEXT = b'MoltBrowser release manifest v1\n'
MANIFEST_NAME = 'molt-release-manifest.json'
SIGNATURE_NAME = MANIFEST_NAME + '.sig'
# Signed with the same key, next to the manifest; like the signature it is
# not listed in the manifest (it is made in the same signing step).
PROVENANCE_NAME = prov.BUNDLE_NAME
NOT_MANIFEST_ASSETS = (MANIFEST_NAME, SIGNATURE_NAME, PROVENANCE_NAME)
PRODUCT = 'MoltBrowser'
SCHEMA = 1
MAX_MANIFEST_BYTES = 64 * 1024
MAX_SIGNATURE_BYTES = 4 * 1024

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
KEYS_SOURCE = os.path.join(REPO_ROOT, 'src', 'chrome', 'browser', 'molt_ai',
                           'update', 'update_signing_keys.cc')
KEYS_BEGIN = '// MOLT_UPDATE_SIGNING_KEYS_BEGIN'
KEYS_END = '// MOLT_UPDATE_SIGNING_KEYS_END'

# Matched with fullmatch(), in ASCII (as molt_provenance.py's patterns).
PLAIN_NAME = re.compile(r'[A-Za-z0-9_-][A-Za-z0-9._-]{0,127}', re.ASCII)
VERSION = re.compile(r'[0-9]+(\.[0-9]+)*', re.ASCII)


Error = prov.Error  # one error type for both tools


# ---------------------------------------------------------------- key helpers

def key_id(public_raw):
    return hashlib.sha256(public_raw).hexdigest()[:16]


def public_raw(public_key):
    return public_key.public_bytes(serialization.Encoding.Raw,
                                   serialization.PublicFormat.Raw)


def decode_public_key(b64):
    try:
        raw = base64.b64decode(b64, validate=True)
    except ValueError as e:
        raise Error('public key is not valid base64') from e
    if len(raw) != 32:
        raise Error('public key must be 32 bytes (Ed25519)')
    return Ed25519PublicKey.from_public_bytes(raw)


def load_private_key(path):
    with open(path, 'rb') as f:
        data = f.read()
    passphrase = getpass.getpass('Signing key passphrase: ').encode()
    try:
        key = serialization.load_pem_private_key(data, password=passphrase)
    except (ValueError, TypeError) as e:
        raise Error('could not unlock the signing key (wrong passphrase?)') from e
    if not isinstance(key, Ed25519PrivateKey):
        raise Error('not an Ed25519 key')
    return key


def trusted_keys_from_source(path=KEYS_SOURCE):
    """Returns the base64 keys listed in update_signing_keys.cc."""
    with open(path, encoding='utf-8') as f:
        text = f.read()
    try:
        block = text[text.index(KEYS_BEGIN):text.index(KEYS_END)]
    except ValueError as e:
        raise Error(f'key markers not found in {path}') from e
    return [k for k in re.findall(r'"([^"]*)"', block) if k]


# ------------------------------------------------------------ manifest rules

def validate_manifest(data):
    """Applies the same structural rules as ParseReleaseManifest()."""
    if len(data) == 0 or len(data) > MAX_MANIFEST_BYTES:
        raise Error('manifest missing or too large')
    try:
        m = json.loads(data.decode('utf-8'))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise Error('manifest is not valid JSON') from e
    if not isinstance(m, dict) or m.get('schema') != SCHEMA \
            or isinstance(m.get('schema'), bool):
        raise Error('unsupported manifest schema')
    if m.get('product') != PRODUCT:
        raise Error('manifest is for a different product')
    version, tag = m.get('version'), m.get('tag')
    if not isinstance(version, str) or not VERSION.fullmatch(version) \
            or not isinstance(tag, str) or not tag:
        raise Error('manifest version or tag is invalid')
    if (tag[1:] if tag[:1] in ('v', 'V') else tag) != version:
        raise Error('manifest version does not match its tag')
    assets = m.get('assets')
    if not isinstance(assets, list) or not assets:
        raise Error('manifest lists no assets')
    seen = set()
    for a in assets:
        if not isinstance(a, dict):
            raise Error('manifest has a malformed asset entry')
        name, size, sha = a.get('name'), a.get('size'), a.get('sha256')
        if not isinstance(name, str) or not PLAIN_NAME.fullmatch(name) \
                or not isinstance(sha, str) or not re.fullmatch(r'[0-9a-f]{64}', sha) \
                or isinstance(size, bool) or not isinstance(size, int) \
                or not 1 <= size <= 2 ** 53:
            raise Error('manifest has a malformed asset entry')
        if name in seen:
            raise Error('manifest lists an asset twice')
        seen.add(name)
    return m


def file_digest(path):
    h = hashlib.sha256()
    size = 0
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
            size += len(chunk)
    return size, h.hexdigest()


# ------------------------------------------------------------------ commands

def cmd_keygen(args):
    if os.path.exists(args.out):
        raise Error(f'{args.out} already exists; refusing to overwrite a key')
    print('Create a passphrase for the signing key. Store it separately from '
          'the key file (e.g. in your password manager).')
    p1 = getpass.getpass('New passphrase: ')
    p2 = getpass.getpass('Repeat passphrase: ')
    if p1 != p2:
        raise Error('passphrases do not match')
    if len(p1) < 12:
        raise Error('use a passphrase of at least 12 characters')
    key = Ed25519PrivateKey.generate()
    pem = key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.BestAvailableEncryption(p1.encode()))
    fd = os.open(args.out, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                 stat.S_IRUSR | stat.S_IWUSR)
    with os.fdopen(fd, 'wb') as f:
        f.write(pem)
    raw = public_raw(key.public_key())
    b64 = base64.b64encode(raw).decode()
    print(f'\nSigning key written to {args.out} (mode 600, passphrase-encrypted).')
    print(f'\nPublic key : {b64}')
    print(f'Key id     : {key_id(raw)}')
    print('\nNext: give ONLY the public key above to engineering, or run\n'
          f'  python3 tools/release_signing/molt_release_sign.py add-key {b64}\n'
          'on a development machine. Back up the key file to a second\n'
          'offline medium. Never copy it to an online machine.')


def cmd_pubkey(args):
    key = load_private_key(args.key)
    raw = public_raw(key.public_key())
    print(base64.b64encode(raw).decode())
    print(f'key id: {key_id(raw)}', file=sys.stderr)


def cmd_add_key(args):
    pub = decode_public_key(args.public_key)
    b64 = base64.b64encode(public_raw(pub)).decode()
    keys = trusted_keys_from_source(args.source)
    if b64 in keys:
        print(f'Key {key_id(public_raw(pub))} is already trusted.')
        return
    keys.append(b64)
    with open(args.source, encoding='utf-8') as f:
        text = f.read()
    begin = text.index(KEYS_BEGIN)
    end = text.index(KEYS_END)
    lines = [KEYS_BEGIN,
             'constexpr std::string_view kTrustedUpdateSigningKeys[] = {']
    for k in keys:
        raw = base64.b64decode(k)
        lines.append(f'    "{k}",  // key id {key_id(raw)}')
    lines.append('};')
    lines.append('')
    text = text[:begin] + '\n'.join(lines) + text[end:]
    with open(args.source, 'w', encoding='utf-8') as f:
        f.write(text)
    print(f'Trusted key {key_id(public_raw(pub))} added to '
          f'{os.path.relpath(args.source, REPO_ROOT)}.')


def cmd_manifest(args):
    tag = args.tag
    version = tag[1:] if tag[:1] in ('v', 'V') else tag
    if not VERSION.fullmatch(version):
        raise Error(f'tag {tag!r} is not a version like v0.3.0')
    assets = []
    for path in sorted(args.files, key=os.path.basename):
        name = os.path.basename(path)
        if name in NOT_MANIFEST_ASSETS:
            continue
        if not PLAIN_NAME.fullmatch(name):
            raise Error(f'asset name {name!r} must be a plain file name')
        size, sha = file_digest(path)
        assets.append({'name': name, 'size': size, 'sha256': sha})
    if not assets:
        raise Error('no assets given')
    manifest = {'schema': SCHEMA, 'product': PRODUCT, 'version': version,
                'tag': tag, 'assets': assets}
    data = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()
    validate_manifest(data)
    with open(args.out, 'wb') as f:
        f.write(data)
    print(f'Wrote {args.out}:')
    for a in assets:
        print(f'  {a["sha256"]}  {a["size"]:>12}  {a["name"]}')


def provenance_for_signing(m, assets_dir):
    """[(name, statement bytes, facts)] for the build provenance statements
    the manifest lists, read from ASSETS_DIR and checked: exactly the bytes
    the manifest lists, valid release statements for this version, and
    together covering every asset a build produced, with the same hashes."""
    names = sorted(a['name'] for a in m['assets']
                   if prov.STATEMENT_NAME.fullmatch(a['name']))
    if not names:
        raise Error('the manifest lists no build provenance '
                    '(molt-provenance-<platform>-<arch>.json). Release builds '
                    'write it next to the SBOMs; rebuild with the release '
                    'scripts, or pass --no-provenance (the release then '
                    'cannot be published)')
    if not assets_dir:
        raise Error('pass --assets-dir with the folder of the release assets '
                    '(the provenance statements are read from there)')
    by_name = {a['name']: a for a in m['assets']}
    out = []
    for name in names:
        with open(os.path.join(assets_dir, name), 'rb') as f:
            data = f.read(prov.MAX_STATEMENT_BYTES + 1)
        if len(data) != by_name[name]['size'] or \
                hashlib.sha256(data).hexdigest() != by_name[name]['sha256']:
            raise Error(f'{name} in {assets_dir} is not the file the manifest '
                        'lists')
        st = prov.validate_statement(data, release=True)
        if prov.statement_facts(st)['name'] != name:
            raise Error(f'{name} holds the statement of another build')
        out.append((name, data, st))
    facts = prov.check_release(
        [st for _, _, st in out], {a['name']: a['sha256'] for a in m['assets']},
        tag=m['tag'], require_all=True)
    return [(name, data, f) for (name, data, _), f in zip(out, facts)]


def cmd_sign(args):
    with open(args.manifest, 'rb') as f:
        data = f.read()
    m = validate_manifest(data)
    statements = [] if args.no_provenance else \
        provenance_for_signing(m, args.assets_dir)
    print(f'About to sign MoltBrowser {m["version"]} (tag {m["tag"]}):')
    for a in m['assets']:
        print(f'  {a["sha256"]}  {a["size"]:>12}  {a["name"]}')
    if statements:
        print('and its build provenance:')
        for _, _, facts in statements:
            print('  ' + prov.summarize(facts))
    else:
        print('WITHOUT build provenance: publish-release-signature.sh will '
              'refuse this release.', file=sys.stderr)
    if not args.yes:
        typed = input(f'Type the version ({m["version"]}) to sign: ').strip()
        if typed != m['version']:
            raise Error('version not confirmed; nothing signed')
    key = load_private_key(args.key)
    raw = public_raw(key.public_key())
    signature = key.sign(SIGNING_CONTEXT + data)
    key.public_key().verify(signature, SIGNING_CONTEXT + data)
    out = args.out or args.manifest + '.sig'
    sig_doc = {'algorithm': 'ed25519', 'key_id': key_id(raw),
               'signature': base64.b64encode(signature).decode()}
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(sig_doc, f, indent=2, sort_keys=True)
        f.write('\n')
    print(f'Wrote {out} (key id {key_id(raw)}).')
    if statements:
        # DSSE envelopes (domain-separated from the manifest signature: the
        # signed bytes start with "DSSEv1 ", a manifest's with "MoltBrowser").
        bundle = prov.sign_bundle([(n, d) for n, d, _ in statements], key.sign,
                                  key_id(raw))
        prov.verify_bundle(bundle, [(key_id(raw), key.public_key())])
        bundle_out = args.provenance_out or os.path.join(
            os.path.dirname(args.manifest), PROVENANCE_NAME)
        with open(bundle_out, 'wb') as f:
            f.write(bundle)
        print(f'Wrote {bundle_out} ({len(statements)} build(s), key id '
              f'{key_id(raw)}).')
    try:
        trusted = trusted_keys_from_source()
    except (OSError, Error):
        trusted = None
    if trusted is not None and base64.b64encode(raw).decode() not in trusted:
        print('WARNING: this key is not in update_signing_keys.cc of this '
              'checkout. Installed browsers will reject this signature unless '
              'their build trusts the key.', file=sys.stderr)


def verify_files(manifest_path, sig_path, keys_b64, asset_paths=(),
                 require_all=False):
    with open(manifest_path, 'rb') as f:
        data = f.read()
    with open(sig_path, 'rb') as f:
        sig_data = f.read()
    if len(sig_data) > MAX_SIGNATURE_BYTES:
        raise Error('signature file too large')
    try:
        sig_doc = json.loads(sig_data.decode('utf-8'))
        if sig_doc.get('algorithm') != 'ed25519':
            raise Error('unsupported signature algorithm')
        signature = base64.b64decode(sig_doc['signature'], validate=True)
    except (ValueError, KeyError, AttributeError) as e:
        raise Error('malformed signature file') from e
    if len(signature) != 64:
        raise Error('malformed signature')
    good_key = None
    for k in keys_b64:
        pub = decode_public_key(k)
        try:
            pub.verify(signature, SIGNING_CONTEXT + data)
            good_key = key_id(public_raw(pub))
            break
        except InvalidSignature:
            continue
    if good_key is None:
        raise Error('signature does not match any trusted key'
                    if keys_b64 else 'no trusted key to verify with')
    m = validate_manifest(data)
    by_name = {a['name']: a for a in m['assets']}
    given = {os.path.basename(p) for p in asset_paths} - set(NOT_MANIFEST_ASSETS)
    if require_all and set(by_name) != given:
        missing = sorted(set(by_name) - given)
        extra = sorted(given - set(by_name))
        raise Error('release assets and signed manifest differ'
                    + (f'; missing: {", ".join(missing)}' if missing else '')
                    + (f'; unsigned: {", ".join(extra)}' if extra else ''))
    for path in asset_paths:
        name = os.path.basename(path)
        if name in NOT_MANIFEST_ASSETS:
            continue
        if name not in by_name:
            raise Error(f'{name} is not listed in the signed manifest')
        size, sha = file_digest(path)
        if size != by_name[name]['size'] or sha != by_name[name]['sha256']:
            raise Error(f'{name} does not match the signed manifest')
    return m, good_key


def cmd_verify(args):
    keys = [args.pubkey] if args.pubkey else trusted_keys_from_source()
    m, kid = verify_files(args.manifest, args.sig, keys, args.files,
                          require_all=args.require_all)
    n = len([f for f in args.files if os.path.basename(f) not in
             NOT_MANIFEST_ASSETS])
    print(f'OK: MoltBrowser {m["version"]} manifest signed by key {kid}; '
          f'{n} asset(s) match.')


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('keygen'); s.add_argument('--out', required=True)
    s.set_defaults(fn=cmd_keygen)
    s = sub.add_parser('pubkey'); s.add_argument('--key', required=True)
    s.set_defaults(fn=cmd_pubkey)
    s = sub.add_parser('add-key'); s.add_argument('public_key')
    s.add_argument('--source', default=KEYS_SOURCE); s.set_defaults(fn=cmd_add_key)
    s = sub.add_parser('manifest'); s.add_argument('--tag', required=True)
    s.add_argument('--out', default=MANIFEST_NAME); s.add_argument('files', nargs='+')
    s.set_defaults(fn=cmd_manifest)
    s = sub.add_parser('sign'); s.add_argument('--key', required=True)
    s.add_argument('--manifest', default=MANIFEST_NAME); s.add_argument('--out')
    s.add_argument('--assets-dir', help='the release assets, for the build '
                   'provenance statements the manifest lists')
    s.add_argument('--provenance-out', help=f'default: {PROVENANCE_NAME} next '
                   'to the manifest')
    s.add_argument('--no-provenance', action='store_true',
                   help='sign the manifest only (not publishable)')
    s.add_argument('--yes', action='store_true', help='skip the version prompt')
    s.set_defaults(fn=cmd_sign)
    s = sub.add_parser('verify'); s.add_argument('--manifest', default=MANIFEST_NAME)
    s.add_argument('--sig', default=SIGNATURE_NAME)
    s.add_argument('--pubkey', help='verify with this key instead of the build\'s')
    s.add_argument('--require-all', action='store_true',
                   help='the given files must be exactly the signed assets')
    s.add_argument('files', nargs='*'); s.set_defaults(fn=cmd_verify)
    args = p.parse_args(argv)
    try:
        args.fn(args)
    except Error as e:
        print(f'error: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
