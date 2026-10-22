#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""MoltBrowser's apt and dnf/yum repositories, signed OFFLINE.

Like the update manifest (docs/SIGNED_UPDATES.md), repository metadata is
signed by a key that never touches CI, a cloud machine or this repository.
CI builds the repository unsigned; the owner signs it on the offline
machine; anything online only verifies.

  build    (CI)       .deb/.rpm -> unsigned tree: apt pool + Packages +
                      Release; the .rpm; repo-summary.json. --pack writes
                      the unsigned bundle (metadata only: the packages are
                      release assets already).
  sign     (OFFLINE)  unsigned bundle + the release's .deb/.rpm ->
                      rpmsign the .rpm, createrepo_c, repomd.xml.asc,
                      InRelease + Release.gpg, the public keyring; checks
                      every signature with gpgv; writes the signed bundle.
  verify   (anywhere) every signature (gpgv, a pinned keyring) and every
                      hash, from Release down to each package.
  unpack   (online)   signed bundle + release assets -> the complete tree to
                      serve, verified first (the Pages deploy uses this).

The bundle (MoltBrowser-Linux-repository.tar) never holds the .deb: the
pool is filled from the release asset with the same SHA-256. It does hold
the signed .rpm, which differs from the release asset only by its signature
header (sign checks that the header and payload digests are unchanged).

Tree, as served (https://onemannocode.github.io/MoltBrowser/linux/):
  moltbrowser-archive-keyring.gpg / .asc     the public key
  moltbrowser.sources / moltbrowser.repo     apt / dnf configuration
  apt/dists/stable/{Release,InRelease,Release.gpg}
  apt/dists/stable/main/binary-amd64/Packages{,.gz}
  apt/pool/main/m/moltbrowser/moltbrowser_<version>_amd64.deb
  rpm/packages/moltbrowser-<version>-1.x86_64.rpm
  rpm/repodata/repomd.xml{,.asc} + its data files
  repo-summary.json

Needs: gpg/gpgv (sign, verify); dpkg-deb (build and sign, to read .deb
control files); rpm, rpmsign and createrepo_c (rpm parts). Python 3.9+,
standard library only. Runbook: docs/SIGNED_UPDATES.md; users:
docs/LINUX_REPOSITORIES.md.
"""

import argparse
import base64
import datetime
import gzip
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ET

BUNDLE_NAME = 'MoltBrowser-Linux-repository.tar'
SUMMARY = 'repo-summary.json'
KEYRING_GPG = 'moltbrowser-archive-keyring.gpg'
KEYRING_ASC = 'moltbrowser-archive-keyring.asc'
SITE_BASE = 'https://onemannocode.github.io/MoltBrowser/linux'
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
COMMITTED_KEY = os.path.join(REPO_ROOT, 'installer', 'linux', KEYRING_ASC)
TEMPLATES = {
    'moltbrowser.sources': os.path.join(REPO_ROOT, 'installer', 'linux',
                                        'moltbrowser.sources'),
    'moltbrowser.repo': os.path.join(REPO_ROOT, 'installer', 'linux',
                                     'moltbrowser.repo'),
}
DEB_ARCHES = {'amd64', 'arm64', 'all'}
MAX_BUNDLE_MEMBERS = 2000
MAX_MEMBER_BYTES = 2 * 1024 ** 3
FPR_RE = re.compile(r'^[0-9A-F]{40}$')

DAYS = ('Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun')
MONTHS = ('Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep',
          'Oct', 'Nov', 'Dec')


class Error(Exception):
    pass


# --------------------------------------------------------------- utilities


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, check=True, **kw):
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, **kw)
    except FileNotFoundError as e:
        raise Error('%s is not installed' % cmd[0]) from e
    if check and proc.returncode != 0:
        raise Error('%s failed (exit %d): %s' % (
            ' '.join(cmd[:3]), proc.returncode,
            proc.stderr.decode('utf-8', 'replace').strip()[-2000:]))
    return proc


def need(tool):
    path = shutil.which(tool)
    if not path:
        raise Error('%s is needed for this step but is not installed' % tool)
    return path


def rfc2822(epoch):
    t = datetime.datetime.fromtimestamp(epoch, tz=datetime.timezone.utc)
    return '%s, %02d %s %04d %02d:%02d:%02d UTC' % (
        DAYS[t.weekday()], t.day, MONTHS[t.month - 1], t.year, t.hour,
        t.minute, t.second)


def build_epoch(args_date):
    if args_date is not None:
        return int(args_date)
    if os.environ.get('SOURCE_DATE_EPOCH'):
        return int(os.environ['SOURCE_DATE_EPOCH'])
    return int(datetime.datetime.now(tz=datetime.timezone.utc).timestamp())


def write_bytes(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def gzip_bytes(data):
    buf = io.BytesIO()
    with gzip.GzipFile(filename='', mode='wb', fileobj=buf, mtime=0) as g:
        g.write(data)
    return buf.getvalue()


def rel(tree, path):
    return os.path.relpath(path, tree).replace(os.sep, '/')


# ---------------------------------------------------------- .deb control


def parse_control(text):
    """A deb822 paragraph: [(field, value)] keeping order and folding."""
    fields = []
    for line in text.splitlines():
        if not line.strip():
            if fields:
                break
            continue
        if line[0] in ' \t':
            if not fields:
                raise Error('control continuation before any field')
            fields[-1] = (fields[-1][0], fields[-1][1] + '\n' + line)
            continue
        name, sep, value = line.partition(':')
        if not sep or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9-]*', name):
            raise Error('bad control line %r' % line[:80])
        fields.append((name, value.strip()))
    return fields


def ar_members(path):
    """{name: bytes} of a small ar archive's members (the .deb header and
    control parts; data.tar is skipped)."""
    out = {}
    with open(path, 'rb') as f:
        if f.read(8) != b'!<arch>\n':
            raise Error('%s is not a .deb (no ar header)' % path)
        while True:
            header = f.read(60)
            if not header:
                break
            if len(header) != 60:
                raise Error('%s: truncated ar header' % path)
            name = header[:16].decode('ascii', 'replace').strip().rstrip('/')
            size = int(header[48:58].decode('ascii').strip())
            if name.startswith('control.tar') or name == 'debian-binary':
                out[name] = f.read(size)
            else:
                f.seek(size, 1)
            if size % 2:
                f.seek(1, 1)
    return out


def deb_control(path):
    """The control file of a .deb, as text. dpkg-deb when present (it reads
    every compression); else the standard library (gz, xz, bz2, none)."""
    if shutil.which('dpkg-deb'):
        return run(['dpkg-deb', '--field', path]).stdout.decode('utf-8')
    members = ar_members(path)
    for name, data in members.items():
        if name.startswith('control.tar'):
            if name.endswith('.zst'):
                raise Error('%s uses zstd; install dpkg-deb to read it' % path)
            with tarfile.open(fileobj=io.BytesIO(data)) as t:
                for m in t.getmembers():
                    if m.name in ('./control', 'control') and m.isfile():
                        return t.extractfile(m).read().decode('utf-8')
    raise Error('%s has no control file' % path)


def pool_path(fields, component):
    f = dict(fields)
    name, version, arch = f.get('Package'), f.get('Version'), f.get('Architecture')
    if not name or not version or not arch:
        raise Error('control file lacks Package/Version/Architecture')
    if not re.fullmatch(r'[a-z0-9][a-z0-9+.-]+', name):
        raise Error('bad package name %r' % name)
    if arch not in DEB_ARCHES:
        raise Error('unsupported architecture %r' % arch)
    upstream = version.split(':', 1)[-1]
    if not re.fullmatch(r'[A-Za-z0-9.+~-]+', upstream):
        raise Error('bad version %r' % version)
    prefix = name[:4] if name.startswith('lib') else name[0]
    return 'pool/%s/%s/%s/%s_%s_%s.deb' % (component, prefix, name, name,
                                           upstream, arch)


def packages_stanza(fields, filename, size, sha256):
    lines = []
    desc = None
    for name, value in fields:
        if name in ('Filename', 'Size', 'MD5sum', 'SHA1', 'SHA256', 'SHA512'):
            continue
        if name == 'Description':
            desc = value
            continue
        lines.append('%s: %s' % (name, value))
    lines += ['Filename: %s' % filename, 'Size: %d' % size,
              'SHA256: %s' % sha256]
    if desc is not None:
        lines.append('Description: %s' % desc)
    return '\n'.join(lines) + '\n'


# --------------------------------------------------------------------- rpm


def rpm_query(path, fmt):
    need('rpm')
    return run(['rpm', '-qp', '--nosignature', '--nodigest', '--qf', fmt,
                path]).stdout.decode('utf-8').strip()


def rpm_identity(path):
    """(name, version-release, arch, canonical file name)."""
    if not shutil.which('rpm'):
        return None, None, None, os.path.basename(path)
    q = rpm_query(path, '%{NAME}\\t%{VERSION}-%{RELEASE}\\t%{ARCH}')
    name, verrel, arch = q.split('\t')
    return name, verrel, arch, '%s-%s.%s.rpm' % (name, verrel, arch)


def rpm_digests(path):
    """Header and payload digests: rpmsign must leave them unchanged."""
    return rpm_query(path, '%{SHA256HEADER} %{SHA1HEADER} %{PAYLOADDIGEST}')


def rpm_check_signature(path, key_asc):
    """rpm's own signature check against KEY_ASC only (a scratch rpmdb)."""
    need('rpmkeys')
    db = tempfile.mkdtemp(prefix='molt-rpmdb-')
    try:
        run(['rpmkeys', '--dbpath', db, '--import', key_asc])
        proc = run(['rpmkeys', '--dbpath', db, '--define',
                    '_pkgverify_level all', '--checksig', '-v', path],
                   check=False)
        out = proc.stdout.decode('utf-8', 'replace')
        if proc.returncode != 0 or 'Signature' not in out \
                or re.search(r'NOKEY|NOT OK|BAD', out):
            raise Error('%s: rpm signature check failed:\n%s' % (path, out.strip()))
    finally:
        shutil.rmtree(db, ignore_errors=True)


# ------------------------------------------------------------------- build


def build_apt(tree, debs, suite, codename, component, origin, label, epoch,
              valid_days):
    apt = os.path.join(tree, 'apt')
    by_arch = {}
    entries = []
    for deb in debs:
        fields = parse_control(deb_control(deb))
        path = pool_path(fields, component)
        dest = os.path.join(apt, path)
        if os.path.exists(dest):
            raise Error('two packages map to %s' % path)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copyfile(deb, dest)
        size, sha = os.path.getsize(dest), sha256_file(dest)
        f = dict(fields)
        by_arch.setdefault(f['Architecture'], []).append(
            packages_stanza(fields, path, size, sha))
        entries.append({'type': 'deb', 'path': 'apt/' + path,
                        'name': f['Package'], 'version': f['Version'],
                        'arch': f['Architecture'], 'size': size, 'sha256': sha,
                        'asset': os.path.basename(deb)})
    if not entries:
        return entries
    arches = sorted(a for a in by_arch if a != 'all') or ['amd64']
    if 'all' in by_arch:
        for a in arches:
            by_arch.setdefault(a, []).extend(by_arch['all'])
        del by_arch['all']
    dist = os.path.join(apt, 'dists', suite)
    indexes = []
    for arch in arches:
        text = '\n'.join(sorted(by_arch.get(arch, []))).encode('utf-8')
        base = '%s/binary-%s/Packages' % (component, arch)
        write_bytes(os.path.join(dist, base), text)
        write_bytes(os.path.join(dist, base + '.gz'), gzip_bytes(text))
        indexes += [base, base + '.gz']
    lines = ['Origin: %s' % origin, 'Label: %s' % label, 'Suite: %s' % suite,
             'Codename: %s' % codename, 'Date: %s' % rfc2822(epoch)]
    if valid_days:
        lines.append('Valid-Until: %s' % rfc2822(epoch + valid_days * 86400))
    lines += ['Architectures: %s' % ' '.join(arches),
              'Components: %s' % component,
              'Description: MoltBrowser packages, signed offline by '
              'GenEye AI Labs',
              'SHA256:']
    for idx in sorted(indexes):
        p = os.path.join(dist, idx)
        lines.append(' %s %d %s' % (sha256_file(p), os.path.getsize(p), idx))
    write_bytes(os.path.join(dist, 'Release'), ('\n'.join(lines) + '\n').encode())
    return entries


def cmd_build(args):
    if not args.deb and not args.rpm:
        raise Error('give at least one --deb or --rpm')
    tree = os.path.abspath(args.out)
    if os.path.exists(tree) and os.listdir(tree):
        raise Error('%s is not empty' % tree)
    os.makedirs(tree, exist_ok=True)
    epoch = build_epoch(args.date)
    entries = build_apt(tree, args.deb, args.suite, args.codename,
                        args.component, args.origin, args.label, epoch,
                        args.valid_days)
    for rpm in args.rpm:
        name, verrel, arch, fname = rpm_identity(rpm)
        dest = os.path.join(tree, 'rpm', 'packages', fname)
        if os.path.exists(dest):
            raise Error('two packages map to %s' % rel(tree, dest))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copyfile(rpm, dest)
        entries.append({'type': 'rpm', 'path': rel(tree, dest), 'name': name,
                        'version': verrel, 'arch': arch,
                        'size': os.path.getsize(dest), 'sha256': sha256_file(dest),
                        'asset': os.path.basename(rpm), 'signed': False})
    summary = {'format': 1, 'suite': args.suite, 'codename': args.codename,
               'component': args.component, 'date': rfc2822(epoch),
               'packages': entries}
    write_bytes(os.path.join(tree, SUMMARY),
                (json.dumps(summary, indent=2, sort_keys=True) + '\n').encode())
    verify_apt(tree, require_signed=False, keyring=None, check_expiry=False)
    if args.pack:
        pack(tree, args.pack, include_debs=False, include_rpms=False)
        print('Wrote unsigned bundle %s' % args.pack)
    print('Built unsigned repository in %s:' % tree)
    for e in entries:
        print('  %-3s %s %s %s  %s' % (e['type'], e['name'], e['version'],
                                       e['arch'], e['sha256']))


# ------------------------------------------------------------ pack / unpack


def pack(tree, out, include_debs, include_rpms):
    """A reproducible tar of TREE (sorted, fixed owners and times)."""
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH', '0'))
    paths = []
    for dirpath, dirnames, filenames in os.walk(tree):
        dirnames.sort()
        for fn in sorted(filenames):
            p = os.path.join(dirpath, fn)
            r = rel(tree, p)
            if os.path.islink(p):
                raise Error('%s is a symlink; refusing to pack it' % r)
            if r.endswith('.deb') and not include_debs:
                continue
            if r.endswith('.rpm') and not include_rpms:
                continue
            paths.append((r, p))
    tmp = out + '.tmp'
    with tarfile.open(tmp, 'w', format=tarfile.PAX_FORMAT) as t:
        for r, p in sorted(paths):
            info = tarfile.TarInfo(r)
            info.size = os.path.getsize(p)
            info.mtime = epoch
            info.mode = 0o644
            info.uid = info.gid = 0
            info.uname = info.gname = ''
            with open(p, 'rb') as f:
                t.addfile(info, f)
    os.replace(tmp, out)


def safe_extract(bundle, dest):
    """Extracts only plain files with plain relative names."""
    with tarfile.open(bundle) as t:
        members = t.getmembers()
        if len(members) > MAX_BUNDLE_MEMBERS:
            raise Error('%s has too many members' % bundle)
        for m in members:
            name = m.name
            parts = name.split('/')
            if m.isdir():
                continue
            if not m.isfile() or name.startswith('/') or '\\' in name \
                    or any(p in ('', '.', '..') for p in parts) \
                    or not all(re.fullmatch(r'[A-Za-z0-9._+~-]+', p) for p in parts):
                raise Error('%s: refusing member %r' % (bundle, name))
            if m.size > MAX_MEMBER_BYTES:
                raise Error('%s: member %s is too large' % (bundle, name))
            target = os.path.join(dest, *parts)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            src = t.extractfile(m)
            with open(target, 'xb') as f:
                shutil.copyfileobj(src, f)


def materialize(tree, assets_dir, kinds=('deb',)):
    """Puts each package of repo-summary.json in place, taken from
    ASSETS_DIR by SHA-256 (the release assets)."""
    summary = load_summary(tree)
    by_size = {}
    for fn in sorted(os.listdir(assets_dir)):
        p = os.path.join(assets_dir, fn)
        if os.path.isfile(p):
            by_size.setdefault(os.path.getsize(p), []).append(p)
    for e in summary['packages']:
        if e['type'] not in kinds:
            continue
        dest = os.path.join(tree, *e['path'].split('/'))
        if os.path.isfile(dest) and sha256_file(dest) == e['sha256']:
            continue
        found = None
        for cand in by_size.get(e['size'], []):
            if sha256_file(cand) == e['sha256']:
                found = cand
                break
        if not found:
            raise Error('no file in %s has the SHA-256 of %s (%s)' % (
                assets_dir, e['path'], e['sha256']))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copyfile(found, dest)


def load_summary(tree):
    path = os.path.join(tree, SUMMARY)
    try:
        with open(path, encoding='utf-8') as f:
            s = json.load(f)
    except (OSError, ValueError) as e:
        raise Error('%s missing or unreadable' % path) from e
    if s.get('format') != 1 or not isinstance(s.get('packages'), list):
        raise Error('%s: unsupported format' % path)
    for e in s['packages']:
        if e.get('type') not in ('deb', 'rpm') or not re.fullmatch(
                r'(apt/pool|rpm/packages)/[A-Za-z0-9._+~/-]+\.(deb|rpm)',
                str(e.get('path', ''))) or '..' in e['path'].split('/') \
                or not re.fullmatch(r'[0-9a-f]{64}', str(e.get('sha256', ''))):
            raise Error('%s: malformed package entry %r' % (path, e))
    return s


# ------------------------------------------------------------------ verify


def crc24(data):
    """OpenPGP armor checksum (RFC 4880, 6.1)."""
    crc = 0xB704CE
    for byte in data:
        crc ^= byte << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= 0x1864CFB
    return crc & 0xFFFFFF


def armor(data):
    """ASCII-armored public key block from binary OpenPGP data."""
    b64 = base64.b64encode(data).decode('ascii')
    lines = [b64[i:i + 64] for i in range(0, len(b64), 64)]
    crc = base64.b64encode(crc24(data).to_bytes(3, 'big')).decode('ascii')
    return ('-----BEGIN PGP PUBLIC KEY BLOCK-----\n\n' + '\n'.join(lines)
            + '\n=' + crc + '\n-----END PGP PUBLIC KEY BLOCK-----\n').encode()


def dearmor(data):
    """Binary OpenPGP from an ASCII-armored block (or the bytes as they are)."""
    text = data.decode('ascii', 'replace') if isinstance(data, bytes) else data
    if '-----BEGIN PGP PUBLIC KEY BLOCK-----' not in text:
        return data
    body = text.split('-----BEGIN PGP PUBLIC KEY BLOCK-----', 1)[1]
    body = body.split('-----END PGP PUBLIC KEY BLOCK-----', 1)[0]
    lines = body.strip().splitlines()
    # Armor headers end at the first empty line, if there are any.
    if any(':' in l for l in lines[:1]) and '' in lines:
        lines = lines[lines.index('') + 1:]
    b64 = ''.join(l.strip() for l in lines if l.strip() and not l.startswith('='))
    try:
        return base64.b64decode(b64, validate=True)
    except ValueError as e:
        raise Error('the armored key does not decode') from e


class Keyring:
    """A gpgv keyring file (binary) from a .gpg or .asc key."""

    def __init__(self, path, fingerprint=None):
        if not path or not os.path.isfile(path):
            raise Error('no public key at %s' % path)
        with open(path, 'rb') as f:
            data = dearmor(f.read())
        self.dir = tempfile.mkdtemp(prefix='molt-keyring-')
        self.path = os.path.join(self.dir, 'keyring.gpg')
        with open(self.path, 'wb') as f:
            f.write(data)
        # rpmkeys --import takes only armored keys.
        self.asc = os.path.join(self.dir, 'keyring.asc')
        with open(self.asc, 'wb') as f:
            f.write(armor(data))
        self.fingerprint = fingerprint.upper().replace(' ', '') if fingerprint else None
        if self.fingerprint and not FPR_RE.match(self.fingerprint):
            raise Error('--fingerprint must be 40 hex digits')

    def close(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def verify(self, sig, data=None, output=None):
        """gpgv SIG [DATA]; returns the primary key fingerprint."""
        need('gpgv')
        cmd = ['gpgv', '--keyring', self.path, '--status-fd', '1']
        if output:
            cmd += ['--output', output]
        cmd.append(sig)
        if data:
            cmd.append(data)
        proc = run(cmd, check=False)
        status = proc.stdout.decode('utf-8', 'replace')
        valid = [l.split() for l in status.splitlines()
                 if l.startswith('[GNUPG:] VALIDSIG ')]
        if proc.returncode != 0 or not valid:
            raise Error('bad or missing signature: %s\n%s' % (
                sig, proc.stderr.decode('utf-8', 'replace').strip()))
        for bad in ('EXPKEYSIG', 'REVKEYSIG', 'BADSIG', 'ERRSIG'):
            if bad in status:
                raise Error('%s: %s' % (sig, bad))
        primary = valid[0][-1].upper()
        if self.fingerprint and primary != self.fingerprint:
            raise Error('%s is signed by %s, not %s' % (sig, primary,
                                                        self.fingerprint))
        return primary


def release_hashes(text):
    """{path: (sha256, size)} from a Release file's SHA256 field."""
    out, inside = {}, False
    for line in text.splitlines():
        if line.startswith('SHA256:'):
            inside = True
            continue
        if inside:
            if not line.startswith(' '):
                inside = False
                continue
            parts = line.split()
            if len(parts) != 3:
                raise Error('bad SHA256 line in Release: %r' % line)
            out[parts[2]] = (parts[0], int(parts[1]))
    return out


def verify_apt(tree, require_signed, keyring, check_control=True,
               check_expiry=True):
    dists = os.path.join(tree, 'apt', 'dists')
    if not os.path.isdir(dists):
        return []
    checked = []
    for suite in sorted(os.listdir(dists)):
        dist = os.path.join(dists, suite)
        release = os.path.join(dist, 'Release')
        with open(release, 'rb') as f:
            release_bytes = f.read()
        if keyring is not None:
            keyring.verify(os.path.join(dist, 'Release.gpg'), release)
            signed_text = os.path.join(keyring.dir, 'inrelease.txt')
            if os.path.exists(signed_text):
                os.remove(signed_text)
            keyring.verify(os.path.join(dist, 'InRelease'), output=signed_text)
            with open(signed_text, 'rb') as f:
                inner = f.read()
            if inner.rstrip(b'\n') != release_bytes.rstrip(b'\n'):
                raise Error('%s/InRelease does not carry the same text as '
                            'Release' % suite)
        elif require_signed:
            raise Error('no keyring to verify %s with' % release)
        text = release_bytes.decode('utf-8')
        if check_expiry and 'Valid-Until:' in text:
            m = re.search(r'^Valid-Until: (.+)$', text, re.M)
            until = datetime.datetime.strptime(
                m.group(1).strip(), '%a, %d %b %Y %H:%M:%S UTC').replace(
                    tzinfo=datetime.timezone.utc)
            if until < datetime.datetime.now(tz=datetime.timezone.utc):
                raise Error('%s/Release expired on %s' % (suite, m.group(1)))
        hashes = release_hashes(text)
        if not hashes:
            raise Error('%s/Release lists no indexes' % suite)
        for path, (sha, size) in hashes.items():
            p = os.path.join(dist, *path.split('/'))
            if not os.path.isfile(p) or os.path.getsize(p) != size \
                    or sha256_file(p) != sha:
                raise Error('%s/%s does not match Release' % (suite, path))
        for path in hashes:
            if not path.endswith('/Packages'):
                continue
            with open(os.path.join(dist, *path.split('/')), 'rb') as f:
                packages = f.read().decode('utf-8')
            with open(os.path.join(dist, *path.split('/')) + '.gz', 'rb') as f:
                if gzip.decompress(f.read()).decode('utf-8') != packages:
                    raise Error('%s/%s.gz differs from %s' % (suite, path, path))
            for stanza in [s for s in packages.split('\n\n') if s.strip()]:
                fields = parse_control(stanza)
                f = dict(fields)
                deb = os.path.join(tree, 'apt', *f['Filename'].split('/'))
                if not os.path.isfile(deb):
                    raise Error('%s is listed in %s but missing' % (
                        f['Filename'], path))
                if os.path.getsize(deb) != int(f['Size']) \
                        or sha256_file(deb) != f['SHA256']:
                    raise Error('%s does not match %s' % (f['Filename'], path))
                if check_control:
                    own = parse_control(deb_control(deb))
                    want = packages_stanza(own, f['Filename'], int(f['Size']),
                                           f['SHA256'])
                    if want.rstrip('\n') != stanza.strip('\n'):
                        raise Error('the %s entry for %s differs from the '
                                    'package\'s own control file' % (
                                        path, f['Filename']))
                checked.append(f['Filename'])
    return checked


def verify_rpm(tree, keyring):
    rpmdir = os.path.join(tree, 'rpm')
    repomd = os.path.join(rpmdir, 'repodata', 'repomd.xml')
    if not os.path.isfile(repomd):
        return []
    if keyring is not None:
        keyring.verify(repomd + '.asc', repomd)
    ns = {'r': 'http://linux.duke.edu/metadata/repo',
          'c': 'http://linux.duke.edu/metadata/common'}
    root = ET.parse(repomd).getroot()
    primary = None
    for data in root.findall('r:data', ns):
        loc = data.find('r:location', ns).get('href')
        cs = data.find('r:checksum', ns)
        p = os.path.join(rpmdir, *loc.split('/'))
        if cs.get('type') != 'sha256' or not os.path.isfile(p) \
                or sha256_file(p) != cs.text.strip():
            raise Error('rpm/%s does not match repomd.xml' % loc)
        if data.get('type') == 'primary':
            primary = p
    if primary is None:
        raise Error('repomd.xml lists no primary metadata')
    with open(primary, 'rb') as f:
        raw = f.read()
    if primary.endswith('.gz'):
        raw = gzip.decompress(raw)
    elif not primary.endswith('.xml'):
        raise Error('unsupported primary metadata compression: %s' % primary)
    listed = []
    for pkg in ET.fromstring(raw).findall('c:package', ns):
        loc = pkg.find('c:location', ns)
        if loc.get('{http://www.w3.org/XML/1998/namespace}base'):
            raise Error('rpm metadata points outside the repository')
        href = loc.get('href')
        cs = pkg.find('c:checksum', ns)
        p = os.path.join(rpmdir, *href.split('/'))
        if '..' in href.split('/') or cs.get('type') != 'sha256' \
                or not os.path.isfile(p) or sha256_file(p) != cs.text.strip():
            raise Error('rpm/%s does not match the primary metadata' % href)
        if keyring is not None and shutil.which('rpmkeys'):
            rpm_check_signature(p, keyring.asc)
        listed.append('rpm/' + href)
    present = sorted('rpm/' + rel(rpmdir, os.path.join(d, fn))
                     for d, _, fns in os.walk(os.path.join(rpmdir, 'packages'))
                     for fn in fns if fn.endswith('.rpm'))
    if sorted(listed) != present:
        raise Error('rpm packages and metadata differ: %s vs %s' % (present, listed))
    return listed


def verify_tree(tree, keyring_path, fingerprint=None, require_signed=True):
    keyring = Keyring(keyring_path, fingerprint) if keyring_path else None
    try:
        summary = load_summary(tree)
        debs = verify_apt(tree, require_signed, keyring)
        rpms = verify_rpm(tree, keyring)
        has_rpm = any(e['type'] == 'rpm' for e in summary['packages'])
        if require_signed and has_rpm and not rpms:
            raise Error('the rpm repository has no signed metadata')
        if require_signed and has_rpm and not all(
                e.get('signed') for e in summary['packages']
                if e['type'] == 'rpm'):
            raise Error('%s lists an rpm that was not signed' % SUMMARY)
        for e in summary['packages']:
            p = os.path.join(tree, *e['path'].split('/'))
            if not os.path.isfile(p) or sha256_file(p) != e['sha256']:
                raise Error('%s does not match %s' % (e['path'], SUMMARY))
        return debs, rpms
    finally:
        if keyring:
            keyring.close()


def cmd_verify(args):
    work = None
    tree = args.tree
    try:
        if args.bundle:
            work = tempfile.mkdtemp(prefix='molt-repo-verify-')
            tree = os.path.join(work, 'tree')
            os.makedirs(tree)
            safe_extract(args.bundle, tree)
            materialize(tree, args.assets_dir or os.path.dirname(
                os.path.abspath(args.bundle)))
        if not tree:
            raise Error('give --tree or --bundle')
        debs, rpms = verify_tree(tree, args.keyring or COMMITTED_KEY,
                                 args.fingerprint)
        print('OK: %d .deb and %d .rpm verified; every signature is good.'
              % (len(debs), len(rpms)))
    finally:
        if work:
            shutil.rmtree(work, ignore_errors=True)


def cmd_unpack(args):
    out = os.path.abspath(args.out)
    if os.path.exists(out) and os.listdir(out):
        raise Error('%s is not empty' % out)
    work = tempfile.mkdtemp(prefix='molt-repo-unpack-', dir=os.path.dirname(out))
    try:
        safe_extract(args.bundle, work)
        materialize(work, args.assets_dir)
        verify_tree(work, args.keyring or COMMITTED_KEY, args.fingerprint)
        if os.path.exists(out):
            os.rmdir(out)
        os.replace(work, out)
        work = None
    finally:
        if work:
            shutil.rmtree(work, ignore_errors=True)
    print('Verified repository ready to serve in %s' % out)


# -------------------------------------------------------------------- sign


def gpg_cmd(args):
    cmd = [need('gpg')]
    if args.gnupghome:
        cmd += ['--homedir', args.gnupghome]
    return cmd


def cmd_sign(args):
    fpr = args.key.upper().replace(' ', '')
    if not FPR_RE.match(fpr):
        raise Error('--key must be the full 40-hex fingerprint of the '
                    'primary key (gpg --list-keys --with-fingerprint)')
    need('gpgv')
    bundle = os.path.abspath(args.bundle)
    out = os.path.abspath(args.out or bundle)
    work = tempfile.mkdtemp(prefix='molt-repo-sign-')
    keydir = None
    try:
        tree = os.path.join(work, 'tree')
        os.makedirs(tree)
        safe_extract(bundle, tree)
        summary = load_summary(tree)
        if any(e.get('signed') for e in summary['packages']) or \
                os.path.exists(os.path.join(tree, KEYRING_GPG)):
            raise Error('%s is already signed' % bundle)
        assets = args.assets_dir or os.path.dirname(bundle)
        materialize(tree, assets, kinds=('deb', 'rpm'))
        # What CI built must match the packages themselves.
        verify_apt(tree, require_signed=False, keyring=None)

        print('About to sign the MoltBrowser Linux repository with key %s:' % fpr)
        for e in summary['packages']:
            print('  %-3s %-12s %-14s %-7s %s' % (e['type'], e['name'],
                                                 e['version'], e['arch'],
                                                 e['sha256']))
        versions = sorted({e['version'].split('-')[0].split(':')[-1]
                           for e in summary['packages']})
        if not args.yes:
            typed = input('Type the version (%s) to sign: '
                          % ', '.join(versions)).strip()
            if typed not in versions:
                raise Error('version not confirmed; nothing signed')

        gpg = gpg_cmd(args)
        keydir = tempfile.mkdtemp(prefix='molt-repo-key-')
        key_gpg = os.path.join(keydir, KEYRING_GPG)
        key_asc = os.path.join(keydir, KEYRING_ASC)
        exported = run(gpg + ['--export', '--export-options', 'export-minimal',
                              fpr]).stdout
        if not exported:
            raise Error('gpg has no public key %s' % fpr)
        write_bytes(key_gpg, exported)
        write_bytes(key_asc, run(gpg + ['--armor', '--export', '--export-options',
                                        'export-minimal', fpr]).stdout)

        # rpm: sign each package, then the metadata over the signed packages.
        rpms = [e for e in summary['packages'] if e['type'] == 'rpm']
        for e in rpms:
            path = os.path.join(tree, *e['path'].split('/'))
            before = rpm_digests(path)
            cmd = [need('rpmsign'), '--addsign', '--define', '_gpg_name %s' % fpr]
            if args.gnupghome:
                cmd += ['--define', '_gpg_path %s' % args.gnupghome]
            run(cmd + [path], stdin=None)
            if rpm_digests(path) != before:
                raise Error('%s: rpmsign changed more than the signature' % path)
            rpm_check_signature(path, key_asc)
            e['unsigned_sha256'] = e['sha256']
            e['sha256'] = sha256_file(path)
            e['size'] = os.path.getsize(path)
            e['signed'] = True
        if rpms:
            rpmdir = os.path.join(tree, 'rpm')
            shutil.rmtree(os.path.join(rpmdir, 'repodata'), ignore_errors=True)
            run([need('createrepo_c'), '--no-database', '--checksum', 'sha256',
                 '--general-compress-type', 'gz', rpmdir])
            repomd = os.path.join(rpmdir, 'repodata', 'repomd.xml')
            run(gpg + ['--batch', '--yes', '--local-user', fpr, '--digest-algo',
                       'SHA512', '--armor', '--detach-sign', '--output',
                       repomd + '.asc', repomd], stdin=None)

        # apt: InRelease (clearsigned) and Release.gpg (detached).
        dists = os.path.join(tree, 'apt', 'dists')
        if os.path.isdir(dists):
            for suite in sorted(os.listdir(dists)):
                release = os.path.join(dists, suite, 'Release')
                run(gpg + ['--batch', '--yes', '--local-user', fpr,
                           '--digest-algo', 'SHA512', '--clearsign', '--output',
                           os.path.join(dists, suite, 'InRelease'), release])
                run(gpg + ['--batch', '--yes', '--local-user', fpr,
                           '--digest-algo', 'SHA512', '--armor', '--detach-sign',
                           '--output', os.path.join(dists, suite, 'Release.gpg'),
                           release])

        shutil.copyfile(key_gpg, os.path.join(tree, KEYRING_GPG))
        shutil.copyfile(key_asc, os.path.join(tree, KEYRING_ASC))
        for name, template in TEMPLATES.items():
            if os.path.isfile(template):
                shutil.copyfile(template, os.path.join(tree, name))
        summary['signing_key'] = fpr
        write_bytes(os.path.join(tree, SUMMARY),
                    (json.dumps(summary, indent=2, sort_keys=True) + '\n').encode())

        debs, signed_rpms = verify_tree(tree, key_gpg, fpr)
        pack(tree, out + '.signing', include_debs=False, include_rpms=True)
        os.replace(out + '.signing', out)
        print('Signed and verified: %d .deb, %d .rpm. Wrote %s.'
              % (len(debs), len(signed_rpms), out))
        print('Next: put it with the release assets before writing the '
              'release manifest (docs/SIGNED_UPDATES.md).')
    finally:
        shutil.rmtree(work, ignore_errors=True)
        if keydir:
            shutil.rmtree(keydir, ignore_errors=True)


def cmd_pack(args):
    pack(os.path.abspath(args.tree), args.out, args.include_debs, True)
    print('Wrote %s' % args.out)


# -------------------------------------------------------------------- main


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = p.add_subparsers(dest='cmd', required=True)

    b = sub.add_parser('build', help='(CI) build the unsigned repository')
    b.add_argument('--out', required=True, help='an empty directory')
    b.add_argument('--deb', action='append', default=[])
    b.add_argument('--rpm', action='append', default=[])
    b.add_argument('--suite', default='stable')
    b.add_argument('--codename', default='stable')
    b.add_argument('--component', default='main')
    b.add_argument('--origin', default='MoltBrowser')
    b.add_argument('--label', default='MoltBrowser')
    b.add_argument('--date', type=int,
                   help='Release date, seconds since the epoch (default: '
                        '$SOURCE_DATE_EPOCH or now)')
    b.add_argument('--valid-days', type=int, default=0,
                   help='add Valid-Until (apt then refuses the metadata '
                        'after that many days; see docs/SIGNED_UPDATES.md)')
    b.add_argument('--pack', help='also write the unsigned bundle here')
    b.set_defaults(fn=cmd_build)

    s = sub.add_parser('sign', help='(OFFLINE) sign the repository')
    s.add_argument('--bundle', required=True,
                   help='the unsigned %s from the draft release' % BUNDLE_NAME)
    s.add_argument('--assets-dir', help='the release assets (.deb, .rpm); '
                                        'default: the bundle\'s folder')
    s.add_argument('--key', required=True,
                   help='fingerprint of the archive key (primary)')
    s.add_argument('--gnupghome', help='GnuPG home with the secret key')
    s.add_argument('--out', help='signed bundle (default: replace --bundle)')
    s.add_argument('--yes', action='store_true', help='skip the version prompt')
    s.set_defaults(fn=cmd_sign)

    v = sub.add_parser('verify', help='check signatures and hashes')
    v.add_argument('--tree')
    v.add_argument('--bundle')
    v.add_argument('--assets-dir')
    v.add_argument('--keyring', help='public key, .gpg or .asc (default: '
                                     'installer/linux/%s)' % KEYRING_ASC)
    v.add_argument('--fingerprint', help='require this primary key')
    v.set_defaults(fn=cmd_verify)

    u = sub.add_parser('unpack', help='verified tree to serve')
    u.add_argument('--bundle', required=True)
    u.add_argument('--assets-dir', required=True)
    u.add_argument('--out', required=True)
    u.add_argument('--keyring')
    u.add_argument('--fingerprint')
    u.set_defaults(fn=cmd_unpack)

    k = sub.add_parser('pack', help='tar a repository tree')
    k.add_argument('--tree', required=True)
    k.add_argument('--out', required=True)
    k.add_argument('--include-debs', action='store_true')
    k.set_defaults(fn=cmd_pack)

    args = p.parse_args(argv)
    try:
        args.fn(args)
    except Error as e:
        print('molt_linux_repo: error: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
