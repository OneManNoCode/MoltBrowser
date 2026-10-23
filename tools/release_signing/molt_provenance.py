#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Signed build provenance for MoltBrowser releases.

Each platform build of a release (macOS, Linux, Windows) describes itself in
an in-toto Statement v1 with a SLSA Provenance v1 predicate:

  subject           what the build produced: its installers, their SBOMs and
                    its SHA256SUMS-<platform>, each with its SHA-256
  buildDefinition   what it was built from: the MoltBrowser commit and tag,
                    Chromium (pinned version and the checkout's commit), the
                    patches/ series, the configs/ GN args file and the
                    effective args.gn, the vendored Rust crates, any bundled
                    model
  runDetails        how: the builder (a label the owner picks, never the
                    hostname by default), the toolchain (clang, rustc, Xcode,
                    MSVC and Windows SDK, depot_tools), start and finish times

The release scripts write it on the build machine, after the SBOM, as
molt-provenance-<platform>-<arch>.json, and upload it to the draft release
with the other assets. The owner signs every statement OFFLINE in the same
session and with the same Ed25519 key as the release manifest
(`molt_release_sign.py sign`). That writes molt-build-provenance.intoto.jsonl,
an in-toto bundle: one DSSE envelope per statement. Anyone can check it with
`verify`, against the public keys in update_signing_keys.cc and the
downloaded assets.

Nothing personal goes in: paths under a home directory, the user name and the
hostname are scrubbed, GN args that hold secrets are redacted, and a statement
that still contains any of them is refused.

Commands:
  start     check the builder label and the source tree, print the start time
  probe     print this machine's toolchain as JSON (run it inside the Docker
            build container, then pass it to generate --builder-facts)
  generate  write molt-provenance-<platform>-<arch>.json
  verify    check a signed bundle against the trusted keys and the assets

Python 3.9+. start, probe and generate use only the standard library; verify
also needs the "cryptography" package, like molt_release_sign.py.
Format and runbook: docs/SIGNED_UPDATES.md, "Build provenance".
"""

import argparse
import base64
import datetime
import getpass
import hashlib
import json
import os
import platform as platform_module
import re
import shutil
import socket
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)  # molt_release_sign, also under python3 -I

REPO_ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))

GITHUB_REPO = 'OneManNoCode/MoltBrowser'
REPO_URL = 'https://github.com/' + GITHUB_REPO
STATEMENT_TYPE = 'https://in-toto.io/Statement/v1'
PREDICATE_TYPE = 'https://slsa.dev/provenance/v1'
PAYLOAD_TYPE = 'application/vnd.in-toto+json'
# What the fields of buildDefinition mean (docs/SIGNED_UPDATES.md).
BUILD_TYPE = REPO_URL + '/blob/main/docs/SIGNED_UPDATES.md#provenance-format-v1'
# A builder is <prefix><label>. The label names one of the owner's build
# machines (or a GitHub-hosted runner class) and is chosen by the owner.
BUILDER_ID_PREFIX = REPO_URL + '/builders/'
CHROMIUM_GIT = 'https://chromium.googlesource.com/chromium/src'
DEPOT_TOOLS_GIT = 'https://chromium.googlesource.com/chromium/tools/depot_tools.git'
HUGGING_FACE = 'https://huggingface.co/'

# Release asset names. MANIFEST_NAME and SIGNATURE_NAME must match
# molt_release_sign.py (not imported here: generate runs on build machines
# that may not have the "cryptography" package).
BUNDLE_NAME = 'molt-build-provenance.intoto.jsonl'
MANIFEST_NAME = 'molt-release-manifest.json'
SIGNATURE_NAME = MANIFEST_NAME + '.sig'
LINUX_REPOSITORY_NAME = 'MoltBrowser-Linux-repository.tar'
# Release assets that no build produces, so no statement lists them: the
# signed manifest and its signature, this bundle, and the Linux repository
# (built from the .deb/.rpm, which are subjects, and signed offline later).
NOT_BUILD_OUTPUTS = frozenset((MANIFEST_NAME, SIGNATURE_NAME, BUNDLE_NAME,
                               LINUX_REPOSITORY_NAME))

PLATFORMS = ('macos', 'linux', 'windows')
ARCHES = ('arm64', 'x64', 'universal')
# Every pattern below is matched with fullmatch() and compiled with
# re.ASCII: match() with "$" would take a value with a trailing newline,
# and a Unicode \d or \S other scripts' digits and spaces.
STATEMENT_NAME = re.compile(
    r'molt-provenance-(macos|linux|windows)-(arm64|x64|universal)\.json',
    re.ASCII)
# The in-app updater picks its installer by these substrings
# (UpdateManager::PlatformAssetMatcher); a provenance file must not match.
UPDATER_ASSET_MATCHERS = ('.dmg', 'Setup.exe', '.deb', '.rpm', '.tar.gz')

PLAIN_NAME = re.compile(r'[A-Za-z0-9_-][A-Za-z0-9._-]{0,127}', re.ASCII)
LABEL = re.compile(r'[a-z0-9](?:[a-z0-9._-]{0,62}[a-z0-9])?', re.ASCII)
SHA256 = re.compile(r'[0-9a-f]{64}', re.ASCII)
GIT_OID = re.compile(r'(?:[0-9a-f]{40}|[0-9a-f]{64})', re.ASCII)
TIME = re.compile(r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ', re.ASCII)
VERSION = re.compile(r'\d+(?:\.\d+)*(?:-[0-9A-Za-z.]+)?', re.ASCII)
CHROMIUM_VERSION = re.compile(r'\d+\.\d+\.\d+\.\d+', re.ASCII)
GN_ARGS_FILE = re.compile(r'configs/[a-z0-9][a-z0-9-]*\.gn', re.ASCII)
OUT_DIR = re.compile(r'out/[A-Za-z0-9._-]{1,64}', re.ASCII)
INVOCATION = re.compile(r'https://[A-Za-z0-9.-]{1,64}/[A-Za-z0-9._-]{1,100}/'
                        r'[A-Za-z0-9._-]{1,100}/actions/runs/\d{1,20}'
                        r'(?:/attempts/\d{1,6})?', re.ASCII)
CONTAINER_IMAGE = re.compile(
    r'([a-z0-9][a-z0-9._/-]{0,127})@sha256:([0-9a-f]{64})', re.ASCII)
MODEL_ID = re.compile(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', re.ASCII)
TOOL_VERSION = re.compile(r'[\x20-\x7e]{1,256}', re.ASCII)
URI = re.compile(r'(?:git\+https|https)://[\x21-\x7e]+', re.ASCII)
BUILDER_VERSION_KEYS = ('clang', 'rustc', 'xcode', 'macosSdk', 'msvc',
                        'windowsSdk')
DIGEST_ALGORITHMS = {'sha256': SHA256, 'gitCommit': GIT_OID,
                     'gitTree': GIT_OID, 'gitBlob': GIT_OID}

MAX_STATEMENT_BYTES = 256 * 1024
MAX_BUNDLE_BYTES = 4 * 1024 * 1024
MAX_ENVELOPES = 16
MAX_SIGNATURES = 4
MAX_SUBJECTS = 64
MAX_DEPENDENCIES = 64
MAX_GN_ARGS = 400
MAX_STRING = 1024

# Paths whose uncommitted changes don't change what a build produces. Any
# other uncommitted change (tracked or untracked) means the commit does not
# describe the source, and a release build refuses it. release.sh rewrites
# website/downloads.json during a release.
NON_SOURCE_PREFIXES = ('website/', 'docs/')

# GN args whose values may be secrets: recorded as "<redacted>" unless empty.
SECRET_ARG = re.compile(r'(?i)(api_?key|secret|token|passw|credential|'
                        r'client_id|private|auth)')
# Values that look like credentials, wherever they appear.
SECRET_VALUE = re.compile(
    r'AIza[0-9A-Za-z_-]{35}|-----BEGIN [A-Z ]*PRIVATE KEY-----|'
    r'\bgh[pousr]_[A-Za-z0-9]{30,}|\bgithub_pat_[A-Za-z0-9_]{20,}|'
    r'\bAKIA[0-9A-Z]{16}\b|\bxox[abpr]-[A-Za-z0-9-]{10,}')

# Paths inside someone's home directory: /Users/<name>, /home/<name>,
# C:\Users\<name> (also with / or GN's escaped \\), and root's home.
HOME_PATH = re.compile(
    r'(?i)(?<![\w.~-])/(?:Users|home)/[^/\\\s"\'<>:;,()\[\]]+|'
    r'(?<![\w])[A-Z]:(?:\\\\|\\|/)+Users(?:\\\\|\\|/)+[^\\/\s"\'<>:;,()\[\]]+|'
    r'(?<![\w.~-])/(?:var/)?root(?=[/\\\s"\']|$)')
GENERIC_NAMES = frozenset(('localhost', 'root', 'runner', 'builder', 'user',
                           'admin', 'ubuntu'))
# Words every statement contains (URLs, dependency names). A user or host
# called like that can't be told apart from them, so it isn't looked for.
FIXED_WORDS = frozenset(w.lower() for w in re.findall(
    r'[A-Za-z0-9_-]+', ' '.join((REPO_URL, CHROMIUM_GIT, DEPOT_TOOLS_GIT,
                                  HUGGING_FACE, BUILD_TYPE, BUILDER_ID_PREFIX,
                                  'moltbrowser chromium patches configs src '
                                  'third_party rust depot_tools model '
                                  'container-image out'))))


class Error(Exception):
    pass


def warn(msg):
    print('warning: ' + msg, file=sys.stderr)


# ------------------------------------------------------------ small helpers

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def read_first_line(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return f.readline().strip()
    except OSError:
        return ''


def run_text(argv, cwd=None):
    """Stdout of ARGV, or None if it can't run or fails."""
    try:
        r = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, check=False,
                           timeout=120)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    return r.stdout.decode('utf-8', errors='replace')


def utc_now():
    return datetime.datetime.now(tz=datetime.timezone.utc).strftime(
        '%Y-%m-%dT%H:%M:%SZ')


def parse_time(value, what):
    if not isinstance(value, str) or not TIME.fullmatch(value):
        raise Error('%s must be a UTC time like 2026-10-01T12:00:00Z' % what)
    try:
        return datetime.datetime.strptime(value, '%Y-%m-%dT%H:%M:%SZ')
    except ValueError as e:
        raise Error('%s is not a valid time' % what) from e


def split_tag(value):
    """'0.3.0' or 'v0.3.0' -> ('v0.3.0', '0.3.0')."""
    if not isinstance(value, str):
        raise Error('version is missing')
    version = value[1:] if value[:1] in ('v', 'V') else value
    if not VERSION.fullmatch(version):
        raise Error('%r is not a release version like v0.3.0' % value)
    tag = value if value[:1] in ('v', 'V') else 'v' + value
    return tag, version


def statement_name(platform, arch):
    return 'molt-provenance-%s-%s.json' % (platform, arch)


def builder_id(label):
    return BUILDER_ID_PREFIX + label


def check_label(label):
    if not label:
        raise Error('no builder label: set MOLT_BUILDER_ID to a name for this '
                    'build machine, e.g. MOLT_BUILDER_ID=geneye-mac-1 '
                    '(docs/SIGNED_UPDATES.md, "One-time: label your build '
                    'machines"). It is published in every release; do not '
                    'use your hostname or user name.')
    if not LABEL.fullmatch(label):
        raise Error('builder label %r: use 1-64 lower-case letters, digits, '
                    '".", "_" or "-"' % label)
    return label


# ----------------------------------------------------- personal data scrubbing

def personal_markers():
    """(home directories, user names, hostnames) of this machine."""
    homes = set()
    for h in (os.path.expanduser('~'), os.environ.get('HOME'),
              os.environ.get('USERPROFILE')):
        if h and len(h) > 3 and os.path.isabs(h):
            homes.add(os.path.normpath(h))
    users = set()
    try:
        users.add(getpass.getuser())
    except Exception:  # getpass may fail without a passwd entry
        pass
    for var in ('USER', 'USERNAME', 'LOGNAME'):
        if os.environ.get(var):
            users.add(os.environ[var])
    users = {u for u in users if len(u) >= 2 and re.fullmatch(r'[\w.-]+', u)
             and u.lower() not in FIXED_WORDS}
    hosts = set()
    for h in (socket.gethostname(), platform_module.node()):
        if h:
            hosts.add(h)
            hosts.add(h.split('.')[0])
    hosts = {h for h in hosts if len(h) >= 3 and h.lower() not in GENERIC_NAMES
             and h.lower() not in FIXED_WORDS}
    return sorted(homes), sorted(users), sorted(hosts)


def _path_variants(path):
    """PATH as it may be spelled in text: native, with /, with GN's \\."""
    out = {path, path.replace('\\', '/')}
    if '\\' in path:
        out.add(path.replace('\\', '\\\\'))
    return sorted(out, key=len, reverse=True)


class Scrubber:
    """Removes paths, user names and hostnames from free text."""

    def __init__(self, places=(), markers=None):
        homes, users, hosts = markers or personal_markers()
        self.rules = []
        end = r'(?=[/\\"\'\s]|$)'  # a whole path component ends there
        # Known directories first, longest first, so the most specific
        # placeholder wins.
        for path, placeholder in sorted(
                ((os.path.normpath(os.path.abspath(p)), ph)
                 for p, ph in places if p),
                key=lambda x: len(x[0]), reverse=True):
            for v in _path_variants(path):
                self.rules.append((re.compile(re.escape(v) + end, re.I),
                                   placeholder))
        for h in homes:
            for v in _path_variants(h):
                self.rules.append((re.compile(re.escape(v) + end, re.I), '~'))
        self.rules.append((HOME_PATH, '~'))
        self.user_rules = [
            re.compile(r'(?i)(?<=[/\\])' + re.escape(u) + r'(?=[/\\"\'\s]|$)')
            for u in users]
        self.host_rules = [
            re.compile(r'(?i)(?<![A-Za-z0-9-])' + re.escape(h) +
                       r'(?![A-Za-z0-9-])') for h in hosts]

    def text(self, s):
        for rx, placeholder in self.rules:
            s = rx.sub(placeholder, s)
        for rx in self.user_rules:
            s = rx.sub('<user>', s)
        for rx in self.host_rules:
            s = rx.sub('<host>', s)
        return s

    def problems(self, s):
        """What personal data S still contains (after scrubbing)."""
        found = []
        if HOME_PATH.search(s):
            found.append('a home directory path')
        if any(rx.search(s) for rx in self.user_rules):
            found.append('your user name in a path')
        if any(rx.search(s) for rx in self.host_rules):
            found.append("this machine's hostname")
        if SECRET_VALUE.search(s):
            found.append('something that looks like a credential')
        return found


def walk_strings(obj, where='statement'):
    if isinstance(obj, str):
        yield where, obj
    elif isinstance(obj, dict):
        for k, v in obj.items():
            yield where, k
            for item in walk_strings(v, '%s.%s' % (where, k)):
                yield item
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            for item in walk_strings(v, '%s[%d]' % (where, i)):
                yield item


def check_no_personal_data(obj, scrubber):
    for where, s in walk_strings(obj):
        found = scrubber.problems(s)
        if found:
            raise Error('refusing to write provenance: %s contains %s'
                        % (where, ' and '.join(found)))


# --------------------------------------------------------------------- git

def run_git(repo, args, input_bytes=None):
    try:
        r = subprocess.run(['git', '-C', repo] + list(args), input=input_bytes,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           check=False, timeout=600)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout if r.returncode == 0 else None


def git_toplevel_head(path):
    """HEAD of the git checkout rooted at PATH (not of one that contains it)."""
    out = run_git(path, ['rev-parse', '--show-toplevel', 'HEAD'])
    if out is None:
        return None
    lines = out.decode('utf-8', errors='replace').split('\n')
    if len(lines) < 2 or os.path.normcase(os.path.realpath(lines[0])) != \
            os.path.normcase(os.path.realpath(path)):
        return None
    rev = lines[1].strip()
    return rev if GIT_OID.fullmatch(rev) else None


def git_local_tag_commit(repo, tag):
    out = run_git(repo, ['rev-parse', '-q', '--verify',
                         'refs/tags/%s^{commit}' % tag])
    rev = out.decode().strip() if out else ''
    return rev if GIT_OID.fullmatch(rev) else None


def source_relevant(path):
    p = path.replace('\\', '/')
    return not (p.startswith(NON_SOURCE_PREFIXES) or p.lower().endswith('.md'))


def git_uncommitted(repo):
    """Repo-relative paths with uncommitted changes that matter to a build
    (tracked or untracked, not ignored). None if REPO is not a checkout."""
    out = run_git(repo, ['status', '--porcelain=v1', '-z',
                         '--untracked-files=all'])
    if out is None:
        return None
    paths = []
    records = out.split(b'\0')
    i = 0
    while i < len(records):
        rec = records[i]
        i += 1
        if len(rec) < 4:
            continue
        status, path = rec[:2], rec[3:].decode('utf-8', errors='replace')
        if b'R' in status or b'C' in status:
            i += 1  # the original path of a rename or copy follows
        if source_relevant(path):
            paths.append(path)
    return sorted(set(paths))


def _cat_blobs(repo, oids):
    """{oid: sha256 hex of the blob's content}."""
    if not oids:
        return {}
    unique = sorted(set(oids))
    out = run_git(repo, ['cat-file', '--batch'],
                  ('\n'.join(unique) + '\n').encode())
    if out is None:
        raise Error('git cat-file failed in %s' % repo)
    digests = {}
    pos = 0
    for oid in unique:
        nl = out.index(b'\n', pos)
        header = out[pos:nl].split(b' ')
        if len(header) != 3 or header[0].decode() != oid or header[1] != b'blob':
            raise Error('unexpected git cat-file output for %s' % oid)
        size = int(header[2])
        start = nl + 1
        digests[oid] = hashlib.sha256(out[start:start + size]).hexdigest()
        pos = start + size + 1
    return digests


def committed_digest(repo, commit, path):
    """Digests of PATH (a file or a directory) as committed in COMMIT.

    A directory gives {"gitTree": <tree id>, "sha256": <tree digest>}: the
    SHA-256 of its `sha256sum`-style listing, one "<sha256>  <path>\\n" line
    per file, by path relative to the directory, sorted bytewise. On a
    checkout without CRLF conversion that equals
      (cd DIR && find . -type f | sed 's|^\\./||' | LC_ALL=C sort \\
         | xargs -d '\\n' sha256sum) | sha256sum
    A file gives {"gitBlob": <blob id>, "sha256": <its SHA-256>}.
    """
    oid = run_git(repo, ['rev-parse', '-q', '--verify', '%s:%s' % (commit, path)])
    oid = oid.decode().strip() if oid else ''
    if not GIT_OID.fullmatch(oid):
        raise Error('%s is not in commit %s' % (path, commit[:12]))
    kind = run_git(repo, ['cat-file', '-t', oid])
    kind = kind.decode().strip() if kind else ''
    if kind == 'blob':
        return {'gitBlob': oid, 'sha256': _cat_blobs(repo, [oid])[oid]}
    if kind != 'tree':
        raise Error('%s in commit %s is a %s' % (path, commit[:12], kind or '?'))
    out = run_git(repo, ['ls-tree', '-r', '-z', '--full-tree', commit, '--', path])
    if out is None:
        raise Error('git ls-tree failed for %s' % path)
    entries = []
    for rec in out.split(b'\0'):
        if not rec:
            continue
        meta, _, name = rec.partition(b'\t')
        mode, typ, blob = meta.split(b' ')
        name = name.decode('utf-8')
        if typ != b'blob' or mode not in (b'100644', b'100755'):
            raise Error('%s: only regular files are supported' % name)
        entries.append((name[len(path) + 1:], blob.decode()))
    if not entries:
        raise Error('%s is empty in commit %s' % (path, commit[:12]))
    digests = _cat_blobs(repo, [b for _, b in entries])
    entries.sort(key=lambda e: e[0].encode('utf-8'))
    listing = ''.join('%s  %s\n' % (digests[b], rel) for rel, b in entries)
    return {'gitTree': oid,
            'sha256': hashlib.sha256(listing.encode('utf-8')).hexdigest()}


# ------------------------------------------------------------------ inputs

def read_chromium_pin(repo):
    pin = {}
    path = os.path.join(repo, 'chromium_version.txt')
    with open(path, encoding='utf-8') as f:
        for line in f:
            key, sep, value = line.partition('=')
            if sep and key.strip() in ('CHROMIUM_VERSION', 'CHROMIUM_REVISION'):
                pin.setdefault(key.strip(), value.strip())
    version = pin.get('CHROMIUM_VERSION', '')
    revision = pin.get('CHROMIUM_REVISION', '')
    if not CHROMIUM_VERSION.fullmatch(version) or not re.fullmatch(r'[0-9a-f]{40}',
                                                               revision):
        raise Error('chromium_version.txt has no valid CHROMIUM_VERSION / '
                    'CHROMIUM_REVISION')
    return version, revision


def chromium_checkout_version(chromium_src):
    parts = {}
    try:
        with open(os.path.join(chromium_src, 'chrome', 'VERSION'),
                  encoding='utf-8') as f:
            for line in f:
                k, sep, v = line.partition('=')
                if sep:
                    parts[k.strip()] = v.strip()
    except OSError:
        return None
    try:
        return '.'.join(parts[k] for k in ('MAJOR', 'MINOR', 'BUILD', 'PATCH'))
    except KeyError:
        return None


def strip_gn_comment(line):
    """LINE without its # comment (a # inside a "string" is kept)."""
    in_string = False
    i = 0
    while i < len(line):
        c = line[i]
        if in_string and c == '\\':
            i += 2
            continue
        if c == '"':
            in_string = not in_string
        elif c == '#' and not in_string:
            return line[:i]
        i += 1
    return line


GN_ASSIGNMENT = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)\s*(\+?=)\s*(.*)$')


def gn_arg_lines(text, scrubber):
    """The non-comment lines of an args.gn, normalized, scrubbed and with
    secrets redacted."""
    lines = []
    for raw in text.splitlines():
        line = strip_gn_comment(raw).strip()
        if not line:
            continue
        m = GN_ASSIGNMENT.match(line)
        if m:
            name, op, value = m.groups()
            if SECRET_ARG.search(name) and value != '""':
                value = '"<redacted>"'
            line = '%s %s %s' % (name, op, value)
        line = SECRET_VALUE.sub('<redacted>', scrubber.text(line))
        lines.append(line)
    return lines


def same_args(effective, config):
    """Whether two args lists agree, ignoring target_cpu / target_os (which
    scripts/build-universal.sh appends per architecture)."""
    def key(lines):
        return sorted(ln for ln in lines
                      if not re.match(r'^target_(cpu|os) =', ln))
    return key(effective) == key(config)


def out_dir_label(out_dir, chromium_src):
    rel = os.path.relpath(os.path.abspath(out_dir), os.path.abspath(chromium_src))
    rel = rel.replace(os.sep, '/')
    if OUT_DIR.fullmatch(rel):
        return rel
    label = 'out/' + os.path.basename(os.path.normpath(out_dir))
    return label if OUT_DIR.fullmatch(label) else 'out/MoltBrowser'


def find_depot_tools(explicit=None):
    if explicit:
        return explicit
    for tool in ('gclient', 'gclient.bat'):
        found = shutil.which(tool)
        if found:
            return os.path.dirname(os.path.realpath(found))
    for cand in (os.path.join(REPO_ROOT, 'depot_tools'), '/opt/depot_tools',
                 os.path.join(os.path.expanduser('~'), 'depot_tools')):
        if os.path.isdir(cand):
            return cand
    return None


def windows_toolchain(gn_out_dirs):
    """{"msvc": ..., "windowsSdk": ...} from the build's environment.x64 (the
    environment GN recorded from vcvarsall), else from this environment."""
    env = {}
    for d in gn_out_dirs:
        try:
            with open(os.path.join(d, 'environment.x64'), 'rb') as f:
                for item in f.read().split(b'\0'):
                    k, sep, v = item.decode('utf-8', 'replace').partition('=')
                    if sep:
                        env.setdefault(k.lower(), v)
        except OSError:
            continue
    out = {}
    for key, var in (('msvc', 'vctoolsversion'), ('windowsSdk', 'windowssdkversion')):
        value = env.get(var) or os.environ.get(var) or ''
        if not value:
            for k, v in os.environ.items():
                if k.lower() == var:
                    value = v
        value = value.strip().rstrip('\\')
        if re.fullmatch(r'[0-9]+(?:\.[0-9]+){1,3}', value):
            out[key] = value
    return out


def probe_builder(platform, chromium_src, depot_tools=None, gn_out_dirs=(),
                  scrubber=None):
    """The toolchain this machine builds with: {"version": {...},
    "depotTools": <commit or None>}."""
    scrub = scrubber or Scrubber(places=[(chromium_src, '<chromium-src>')])
    version = {}
    llvm = os.path.join(chromium_src, 'third_party', 'llvm-build',
                        'Release+Asserts')
    clang = ''
    for exe in ('clang', 'clang.exe', 'clang-cl.exe'):
        path = os.path.join(llvm, 'bin', exe)
        if os.path.isfile(path):
            out = run_text([path, '--version'])
            if out and out.strip():
                clang = out.strip().splitlines()[0]
                break
    if not clang:
        stamp = read_first_line(os.path.join(llvm, 'cr_build_revision'))
        if stamp:
            clang = 'clang package ' + stamp.partition(',')[0]
    if clang:
        version['clang'] = clang
    rust_dir = os.path.join(chromium_src, 'third_party', 'rust-toolchain')
    rustc = read_first_line(os.path.join(rust_dir, 'VERSION'))
    if not rustc:
        for exe in ('rustc', 'rustc.exe'):
            out = run_text([os.path.join(rust_dir, 'bin', exe), '-V'])
            if out and out.strip():
                rustc = out.strip().splitlines()[0]
                break
    if rustc:
        version['rustc'] = rustc
    if platform == 'macos':
        out = run_text(['/usr/bin/xcodebuild', '-version']) or ''
        m = re.search(r'^Xcode (\S+)', out, re.M)
        b = re.search(r'^Build version (\S+)', out, re.M)
        if m:
            version['xcode'] = m.group(1) + (' (%s)' % b.group(1) if b else '')
        sdk = (run_text(['/usr/bin/xcrun', '--sdk', 'macosx',
                         '--show-sdk-version']) or '').strip()
        if re.fullmatch(r'[0-9]+(?:\.[0-9]+){0,3}', sdk):
            version['macosSdk'] = sdk
    if platform == 'windows':
        version.update(windows_toolchain(gn_out_dirs))
    for key in list(version):
        value = scrub.text(version[key])
        if TOOL_VERSION.fullmatch(value) and not scrub.problems(value):
            version[key] = value
        else:
            warn('ignoring the %s version: unexpected text' % key)
            del version[key]
    depot = find_depot_tools(depot_tools)
    rev = git_toplevel_head(depot) if depot else None
    return {'version': version, 'depotTools': rev}


def check_builder_facts(facts):
    if not isinstance(facts, dict) or set(facts) - {'version', 'depotTools'}:
        raise Error('builder facts: expected {"version": {...}, "depotTools": ...}')
    version = facts.get('version') or {}
    if not isinstance(version, dict):
        raise Error('builder facts: "version" must be an object')
    for k, v in version.items():
        if k not in BUILDER_VERSION_KEYS or not isinstance(v, str) or \
                not TOOL_VERSION.fullmatch(v):
            raise Error('builder facts: bad version entry %r' % k)
    rev = facts.get('depotTools')
    if rev is not None and (not isinstance(rev, str) or not GIT_OID.fullmatch(rev)):
        raise Error('builder facts: depotTools must be a git commit')
    return {'version': dict(version), 'depotTools': rev}


def bundled_models(payload, repo, release):
    """resolvedDependencies for AI models bundled in the payload."""
    found = []
    for rel in (('Contents', 'Resources', 'molt_models'), ('molt_models',)):
        d = os.path.join(payload, *rel)
        if os.path.isdir(d):
            found.extend(os.path.join(d, n) for n in sorted(os.listdir(d)))
    if not found:
        return []
    sys.path.insert(0, os.path.join(repo, 'tools', 'sbom'))
    try:
        import molt_sbom  # noqa: E402  (the model registry parser)
        registry = molt_sbom.model_registry(os.path.join(
            repo, 'src', 'chrome', 'browser', 'molt_ai', 'runtime',
            'browser_ai_runtime.cc'))
    finally:
        sys.path.pop(0)
    deps = []
    for path in found:
        name = os.path.basename(path)
        model_id = name[:-len('.gguf')] if name.endswith('.gguf') else name
        if not os.path.isfile(path) or not MODEL_ID.fullmatch(model_id):
            raise Error('unexpected file in molt_models: %r' % name)
        actual = sha256_file(path)
        entry = registry.get(model_id)
        dep = {'name': 'model:' + model_id, 'digest': {'sha256': actual}}
        if entry is None:
            if release:
                raise Error('bundled model %s is not in the model registry' % name)
            warn('bundled model %s is not in the model registry' % name)
        else:
            if entry['sha256'] and entry['sha256'] != actual:
                raise Error('bundled model %s does not match its pinned SHA-256'
                            % name)
            dep['uri'] = '%s%s/resolve/%s/%s' % (
                HUGGING_FACE, entry['repo'], entry['revision'] or 'main',
                entry['file'])
        deps.append(dep)
    return deps


# --------------------------------------------------------------- statement

def build_statement(*, tag, version, platform, arch, gn_args_file, release,
                    commit, uncommitted, chromium_version, chromium_revision,
                    dependencies, gn_args, label, builder_version,
                    builder_dependencies, started_on, finished_on,
                    invocation_id, subjects):
    """The in-toto Statement (a dict). Pure: same inputs, same statement."""
    source = 'git+%s@refs/tags/%s' % (REPO_URL, tag)
    molt = {'name': 'moltbrowser', 'uri': source}
    if commit:
        molt['digest'] = {'gitCommit': commit}
    if uncommitted:
        molt['annotations'] = {'uncommittedChanges': list(uncommitted)}
    resolved = [molt]
    if chromium_revision:
        resolved.append({
            'name': 'chromium',
            'uri': 'git+%s@refs/tags/%s' % (CHROMIUM_GIT, chromium_version),
            'digest': {'gitCommit': chromium_revision}})
    resolved.extend(dependencies)
    builder = {'id': builder_id(label),
               'version': dict(sorted(builder_version.items()))}
    if builder_dependencies:
        builder['builderDependencies'] = list(builder_dependencies)
    metadata = {'startedOn': started_on, 'finishedOn': finished_on}
    if invocation_id:
        metadata['invocationId'] = invocation_id
    return {
        '_type': STATEMENT_TYPE,
        'subject': [{'name': n, 'digest': {'sha256': d}}
                    for n, d in sorted(subjects)],
        'predicateType': PREDICATE_TYPE,
        'predicate': {
            'buildDefinition': {
                'buildType': BUILD_TYPE,
                'externalParameters': {
                    'source': source,
                    'tag': tag,
                    'version': version,
                    'platform': platform,
                    'arch': arch,
                    'gnArgsFile': gn_args_file,
                    'releaseBuild': bool(release),
                },
                'internalParameters': {'gnArgs': list(gn_args)},
                'resolvedDependencies': resolved,
            },
            'runDetails': {'builder': builder, 'metadata': metadata},
        },
    }


def serialize(statement):
    return (json.dumps(statement, indent=2, sort_keys=True, ensure_ascii=True)
            + '\n').encode('utf-8')


def _strict_json(data, what):
    def pairs(items):
        d = {}
        for k, v in items:
            if k in d:
                raise Error('%s: duplicate key %r' % (what, k))
            d[k] = v
        return d

    def constant(name):
        raise Error('%s: %s is not allowed' % (what, name))

    try:
        text = data.decode('utf-8')
        return json.loads(text, object_pairs_hook=pairs, parse_constant=constant)
    except (UnicodeDecodeError, ValueError, RecursionError) as e:
        raise Error('%s is not valid JSON' % what) from e


def _keys(obj, where, required, optional=()):
    if not isinstance(obj, dict):
        raise Error('%s must be an object' % where)
    missing = [k for k in required if k not in obj]
    extra = [k for k in obj if k not in required and k not in optional]
    if missing or extra:
        raise Error('%s: %s' % (where, '; '.join(
            (['missing ' + ', '.join(missing)] if missing else []) +
            (['unexpected ' + ', '.join(sorted(extra))] if extra else []))))
    return obj


def _string(value, where, pattern=None, max_len=MAX_STRING):
    if not isinstance(value, str) or not value or len(value) > max_len or \
            any(ord(c) < 0x20 or ord(c) == 0x7f for c in value):
        raise Error('%s must be a short printable string' % where)
    if pattern is not None and not pattern.fullmatch(value):
        raise Error('%s is malformed: %r' % (where, value[:80]))
    return value


def _uri(value, where):
    _string(value, where, URI, 512)
    return value


def _digest(value, where):
    if not isinstance(value, dict) or not value or len(value) > 4:
        raise Error('%s must be a digest set' % where)
    for alg, hexval in value.items():
        if alg not in DIGEST_ALGORITHMS or not isinstance(hexval, str) or \
                not DIGEST_ALGORITHMS[alg].fullmatch(hexval):
            raise Error('%s: bad %r digest' % (where, alg))
    return value


def _descriptor(value, where):
    _keys(value, where, ('name',), ('uri', 'digest', 'annotations'))
    _string(value['name'], where + '.name', max_len=256)
    if 'uri' in value:
        _uri(value['uri'], where + '.uri')
    if 'digest' in value:
        _digest(value['digest'], where + '.digest')
    if 'uri' not in value and 'digest' not in value:
        raise Error('%s needs a uri or a digest' % where)
    if 'annotations' in value:
        ann = _keys(value['annotations'], where + '.annotations', (),
                    ('uncommittedChanges',))
        changes = ann.get('uncommittedChanges')
        if changes is not None:
            if not isinstance(changes, list) or len(changes) > 200:
                raise Error('%s.annotations.uncommittedChanges must be a list'
                            % where)
            for i, c in enumerate(changes):
                _string(c, '%s.annotations.uncommittedChanges[%d]' % (where, i))
    return value


def validate_statement(data, release=True):
    """Parses statement bytes and applies the rules of this format.

    With RELEASE, also requires everything a release statement carries. Raises
    Error; returns the statement as a dict."""
    if not isinstance(data, bytes) or not data or len(data) > MAX_STATEMENT_BYTES:
        raise Error('statement missing or too large')
    st = _keys(_strict_json(data, 'statement'), 'statement',
               ('_type', 'subject', 'predicateType', 'predicate'))
    if st['_type'] != STATEMENT_TYPE:
        raise Error('not an in-toto Statement v1')
    if st['predicateType'] != PREDICATE_TYPE:
        raise Error('not a SLSA Provenance v1 predicate')
    subjects = st['subject']
    if not isinstance(subjects, list) or not 1 <= len(subjects) <= MAX_SUBJECTS:
        raise Error('statement lists no subjects (or too many)')
    names = set()
    for i, s in enumerate(subjects):
        where = 'subject[%d]' % i
        _keys(s, where, ('name', 'digest'))
        _string(s['name'], where + '.name', PLAIN_NAME)
        _keys(s['digest'], where + '.digest', ('sha256',))
        _string(s['digest']['sha256'], where + '.digest.sha256', SHA256)
        if s['name'] in names:
            raise Error('subject %s listed twice' % s['name'])
        if s['name'] in NOT_BUILD_OUTPUTS or STATEMENT_NAME.fullmatch(s['name']):
            raise Error('subject %s is not a build output' % s['name'])
        names.add(s['name'])

    pred = _keys(st['predicate'], 'predicate', ('buildDefinition', 'runDetails'))
    bd = _keys(pred['buildDefinition'], 'buildDefinition',
               ('buildType', 'externalParameters', 'internalParameters',
                'resolvedDependencies'))
    if bd['buildType'] != BUILD_TYPE:
        raise Error('unknown buildType %r' % str(bd['buildType'])[:100])
    ep = _keys(bd['externalParameters'], 'externalParameters',
               ('source', 'tag', 'version', 'platform', 'arch', 'gnArgsFile',
                'releaseBuild'))
    tag, version = split_tag(_string(ep['tag'], 'externalParameters.tag'))
    if ep['tag'] != tag or ep['version'] != version:
        raise Error('externalParameters: tag and version disagree')
    if ep['source'] != 'git+%s@refs/tags/%s' % (REPO_URL, tag):
        raise Error('externalParameters.source must be this repository at the tag')
    if ep['platform'] not in PLATFORMS or ep['arch'] not in ARCHES:
        raise Error('externalParameters: unknown platform or arch')
    _string(ep['gnArgsFile'], 'externalParameters.gnArgsFile', GN_ARGS_FILE)
    if not isinstance(ep['releaseBuild'], bool):
        raise Error('externalParameters.releaseBuild must be true or false')

    ip = _keys(bd['internalParameters'], 'internalParameters', ('gnArgs',))
    gn = ip['gnArgs']
    if not isinstance(gn, list) or len(gn) > 8:
        raise Error('internalParameters.gnArgs must be a list')
    total = 0
    for i, entry in enumerate(gn):
        where = 'internalParameters.gnArgs[%d]' % i
        _keys(entry, where, ('outDir', 'args', 'matchesArgsFile'))
        _string(entry['outDir'], where + '.outDir', OUT_DIR)
        if not isinstance(entry['matchesArgsFile'], bool):
            raise Error(where + '.matchesArgsFile must be true or false')
        if not isinstance(entry['args'], list):
            raise Error(where + '.args must be a list')
        total += len(entry['args'])
        for j, line in enumerate(entry['args']):
            _string(line, '%s.args[%d]' % (where, j))
    if total > MAX_GN_ARGS:
        raise Error('internalParameters.gnArgs is too long')

    deps = bd['resolvedDependencies']
    if not isinstance(deps, list) or not 1 <= len(deps) <= MAX_DEPENDENCIES:
        raise Error('resolvedDependencies must be a non-empty list')
    by_name = {}
    for i, d in enumerate(deps):
        _descriptor(d, 'resolvedDependencies[%d]' % i)
        if d['name'] in by_name:
            raise Error('resolvedDependencies lists %s twice' % d['name'])
        by_name[d['name']] = d
    molt = deps[0]
    if molt['name'] != 'moltbrowser' or molt.get('uri') != ep['source'] or \
            set(molt.get('digest', {})) - {'gitCommit'}:
        raise Error('resolvedDependencies must start with moltbrowser at the tag')

    rd = _keys(pred['runDetails'], 'runDetails', ('builder', 'metadata'))
    b = _keys(rd['builder'], 'runDetails.builder', ('id', 'version'),
              ('builderDependencies',))
    _string(b['id'], 'runDetails.builder.id')
    if not b['id'].startswith(BUILDER_ID_PREFIX) or \
            not LABEL.fullmatch(b['id'][len(BUILDER_ID_PREFIX):]):
        raise Error('runDetails.builder.id is not a MoltBrowser builder')
    bv = b['version']
    if not isinstance(bv, dict):
        raise Error('runDetails.builder.version must be an object')
    for k, v in bv.items():
        if k not in BUILDER_VERSION_KEYS:
            raise Error('runDetails.builder.version: unknown key %r' % k[:40])
        _string(v, 'runDetails.builder.version.' + k, TOOL_VERSION)
    bdeps = b.get('builderDependencies', [])
    if not isinstance(bdeps, list) or len(bdeps) > 8:
        raise Error('runDetails.builder.builderDependencies must be a list')
    bdep_names = set()
    for i, d in enumerate(bdeps):
        _descriptor(d, 'runDetails.builder.builderDependencies[%d]' % i)
        bdep_names.add(d['name'])
    md = _keys(rd['metadata'], 'runDetails.metadata',
               ('startedOn', 'finishedOn'), ('invocationId',))
    started = parse_time(md['startedOn'], 'runDetails.metadata.startedOn')
    finished = parse_time(md['finishedOn'], 'runDetails.metadata.finishedOn')
    if finished < started:
        raise Error('runDetails.metadata: finishedOn is before startedOn')
    if 'invocationId' in md:
        _string(md['invocationId'], 'runDetails.metadata.invocationId',
                INVOCATION)

    if release:
        problems = []
        if ep['releaseBuild'] is not True:
            problems.append('not a release build')
        if 'gitCommit' not in molt.get('digest', {}) or 'annotations' in molt:
            problems.append('the MoltBrowser commit is missing or the tree '
                            'had uncommitted changes')
        chromium = by_name.get('chromium')
        if not chromium or 'gitCommit' not in chromium.get('digest', {}) or \
                not re.fullmatch(r'git\+%s@refs/tags/[0-9]+\.[0-9]+\.[0-9]+'
                                 r'\.[0-9]+' % re.escape(CHROMIUM_GIT),
                                 chromium.get('uri', '')):
            problems.append('Chromium is missing')
        for name, kinds in (('patches', ('gitTree', 'sha256')),
                            (ep['gnArgsFile'], ('gitBlob', 'sha256')),
                            ('src/third_party/rust', ('gitTree', 'sha256'))):
            d = by_name.get(name)
            if not d or any(k not in d.get('digest', {}) for k in kinds):
                problems.append('%s is missing' % name)
        if not gn or any(not e['args'] for e in gn):
            problems.append('the effective GN args are missing')
        for key in ('clang', 'rustc') + (('xcode',) if ep['platform'] == 'macos'
                                         else ()):
            if key not in bv:
                problems.append('the %s version is missing' % key)
        if 'depot_tools' not in bdep_names:
            problems.append('the depot_tools revision is missing')
        if problems:
            raise Error('not a complete release statement: ' + '; '.join(problems))
    return st


def statement_facts(st):
    """The parts of a validated statement that the release checks compare."""
    ep = st['predicate']['buildDefinition']['externalParameters']
    deps = {d['name']: d for d in
            st['predicate']['buildDefinition']['resolvedDependencies']}
    rd = st['predicate']['runDetails']
    return {
        'name': statement_name(ep['platform'], ep['arch']),
        'tag': ep['tag'], 'version': ep['version'],
        'platform': ep['platform'], 'arch': ep['arch'],
        'commit': deps['moltbrowser'].get('digest', {}).get('gitCommit'),
        'chromium': deps.get('chromium', {}).get('digest', {}).get('gitCommit'),
        'chromium_version': deps.get('chromium', {}).get('uri', '').rpartition('/')[2],
        'builder': rd['builder']['id'][len(BUILDER_ID_PREFIX):],
        'started': rd['metadata']['startedOn'],
        'finished': rd['metadata']['finishedOn'],
        'subjects': {s['name']: s['digest']['sha256'] for s in st['subject']},
    }


def check_release(statements, assets, tag=None, expect_commit=None,
                  require_all=False):
    """Cross-checks validated statements against a release.

    STATEMENTS: [(statement dict)]. ASSETS: {name: sha256} of the release
    assets. With REQUIRE_ALL, every statement file among the assets must be
    there and every asset that a build produces must be a subject."""
    facts = [statement_facts(st) for st in statements]
    if not facts:
        raise Error('no build provenance statements')
    seen = {}
    for f in facts:
        if f['name'] in seen:
            raise Error('two statements for %s-%s' % (f['platform'], f['arch']))
        seen[f['name']] = f
    if tag is not None:
        want_tag, want_version = split_tag(tag)
        for f in facts:
            if f['version'] != want_version:
                raise Error('%s is for %s, not %s' % (f['name'], f['tag'], want_tag))
    for key, what in (('commit', 'MoltBrowser commit'),
                      ('chromium', 'Chromium revision'), ('version', 'version')):
        if len({f[key] for f in facts}) > 1:
            raise Error('the statements disagree on the %s' % what)
    if expect_commit is not None:
        for f in facts:
            if f['commit'] != expect_commit:
                raise Error('%s was built from %s, but %s is commit %s'
                            % (f['name'], (f['commit'] or '?')[:12],
                               tag or 'the release', expect_commit[:12]))
    covered = {}
    for f in facts:
        for name, digest in f['subjects'].items():
            if name in covered:
                raise Error('%s is a subject of two statements' % name)
            covered[name] = f['name']
            if name not in assets:
                raise Error('%s (in %s) is not a release asset' % (name, f['name']))
            if assets[name] != digest:
                raise Error('%s does not match its signed provenance' % name)
    if require_all:
        listed = {n for n in assets if STATEMENT_NAME.fullmatch(n)}
        if listed != set(seen):
            missing = sorted(listed - set(seen))
            extra = sorted(set(seen) - listed)
            raise Error('signed provenance and release differ'
                        + ('; unsigned: ' + ', '.join(missing) if missing else '')
                        + ('; not attached: ' + ', '.join(extra) if extra else ''))
        uncovered = sorted(n for n in assets if n not in covered and
                           n not in NOT_BUILD_OUTPUTS and not STATEMENT_NAME.fullmatch(n))
        if uncovered:
            raise Error('no build provenance for: ' + ', '.join(uncovered))
    return facts


# --------------------------------------------------------------------- DSSE

def pae(payload_type, payload):
    """DSSE v1 pre-authentication encoding: what is actually signed."""
    t = payload_type.encode('utf-8')
    return b'DSSEv1 %d %s %d %s' % (len(t), t, len(payload), payload)


def envelope_line(payload, signature, keyid):
    env = {'payload': base64.b64encode(payload).decode('ascii'),
           'payloadType': PAYLOAD_TYPE,
           'signatures': [{'keyid': keyid,
                           'sig': base64.b64encode(signature).decode('ascii')}]}
    return json.dumps(env, sort_keys=True, separators=(',', ':'))


def sign_bundle(statements, sign, keyid):
    """The bundle (bytes) for [(name, statement bytes)], one DSSE envelope per
    line in name order. SIGN(message) -> 64-byte Ed25519 signature."""
    lines = [envelope_line(data, sign(pae(PAYLOAD_TYPE, data)), keyid)
             for _, data in sorted(statements)]
    return ('\n'.join(lines) + '\n').encode('ascii')


def _b64(value, where, max_len):
    if not isinstance(value, str) or len(value) > max_len * 4 // 3 + 4:
        raise Error('%s must be base64' % where)
    try:
        return base64.b64decode(value, validate=True)
    except ValueError as e:
        raise Error('%s is not valid base64' % where) from e


def parse_bundle(data):
    """[(payload bytes, [signature bytes])] of an in-toto bundle (JSON Lines
    of DSSE envelopes). Checks the shape only, not the signatures."""
    if not isinstance(data, bytes) or not data or len(data) > MAX_BUNDLE_BYTES:
        raise Error('provenance bundle missing or too large')
    if not data.endswith(b'\n') or b'\r' in data:
        raise Error('provenance bundle must be LF-terminated JSON lines')
    lines = data[:-1].split(b'\n')
    if lines == [b''] or len(lines) > MAX_ENVELOPES:
        raise Error('provenance bundle has no envelopes (or too many)')
    out = []
    for n, line in enumerate(lines, 1):
        where = 'bundle line %d' % n
        env = _keys(_strict_json(line, where), where,
                    ('payload', 'payloadType', 'signatures'))
        if env['payloadType'] != PAYLOAD_TYPE:
            raise Error('%s: payloadType is not %s' % (where, PAYLOAD_TYPE))
        payload = _b64(env['payload'], where + ' payload', MAX_STATEMENT_BYTES)
        sigs = env['signatures']
        if not isinstance(sigs, list) or not 1 <= len(sigs) <= MAX_SIGNATURES:
            raise Error('%s: expected 1-%d signatures' % (where, MAX_SIGNATURES))
        raw = []
        for s in sigs:
            _keys(s, where + ' signature', ('sig',), ('keyid',))
            if 'keyid' in s:
                _string(s['keyid'], where + ' keyid', max_len=128)
            sig = _b64(s['sig'], where + ' signature', 64)
            if len(sig) != 64:
                raise Error('%s: not an Ed25519 signature' % where)
            raw.append(sig)
        out.append((payload, raw))
    return out


def public_keys(keys_b64):
    """[(key id, Ed25519PublicKey)] for base64 raw keys."""
    import molt_release_sign as rs  # needs "cryptography"
    out = []
    for k in keys_b64:
        try:
            pub = rs.decode_public_key(k)
        except rs.Error as e:
            raise Error(str(e)) from e
        out.append((rs.key_id(rs.public_raw(pub)), pub))
    return out


def trusted_keys():
    """The keys in update_signing_keys.cc, as molt_release_sign reads them."""
    import molt_release_sign as rs
    try:
        return rs.trusted_keys_from_source()
    except rs.Error as e:
        raise Error(str(e)) from e


def verify_bundle(data, keys):
    """[(statement bytes, key id)] whose signatures verify with one of KEYS
    ([(key id, Ed25519PublicKey)]). Every envelope must verify."""
    from cryptography.exceptions import InvalidSignature
    if not keys:
        raise Error('no trusted key to verify with')
    out = []
    for n, (payload, sigs) in enumerate(parse_bundle(data), 1):
        message = pae(PAYLOAD_TYPE, payload)
        good = None
        for sig in sigs:
            for kid, pub in keys:
                try:
                    pub.verify(sig, message)
                    good = kid
                    break
                except InvalidSignature:
                    continue
            if good:
                break
        if good is None:
            raise Error('bundle line %d: signature does not match any trusted key'
                        % n)
        out.append((payload, good))
    return out


# ----------------------------------------------------------------- commands

def cmd_start(args):
    """Fails fast, before a long build, on what generate would refuse."""
    scrubber = Scrubber()
    label = check_label(args.builder_id or os.environ.get('MOLT_BUILDER_ID', ''))
    if scrubber.problems('/' + label + '/'):
        raise Error('builder label %r is your user name or hostname; choose a '
                    'name that says nothing about you or this machine' % label)
    repo = args.repo
    if git_toplevel_head(repo) is None:
        raise Error('%s is not a git checkout; the provenance needs the commit'
                    % repo)
    dirty = git_uncommitted(repo)
    if dirty:
        raise Error('uncommitted changes in %s (commit or stash them first): %s'
                    % (repo, ', '.join(dirty[:10]) + (' ...' if len(dirty) > 10 else '')))
    if args.version:
        tag, _ = split_tag(args.version)
        head = git_toplevel_head(repo)
        tagged = git_local_tag_commit(repo, tag)
        if tagged and tagged != head:
            raise Error('HEAD is %s but %s is %s: build the tagged commit'
                        % (head[:12], tag, tagged[:12]))
    started = os.environ.get('MOLT_BUILD_STARTED_ON', '')
    if started:
        parse_time(started, 'MOLT_BUILD_STARTED_ON')
    print(started or utc_now())


def cmd_probe(args):
    chromium_src = os.path.abspath(args.chromium_src)
    scrubber = Scrubber(places=[(chromium_src, '<chromium-src>'),
                                (args.repo, '<repo>')])
    facts = probe_builder(args.platform, chromium_src, args.depot_tools,
                          args.gn_out_dir or (), scrubber)
    print(json.dumps(facts, indent=2, sort_keys=True))


def cmd_generate(args):
    release = args.release
    repo = os.path.abspath(args.repo)
    tag, version = split_tag(args.version)
    platform, arch = args.platform, args.arch

    def need(ok, msg):
        if not ok:
            if release:
                raise Error(msg)
            warn(msg)
        return ok

    label = check_label(args.builder_id or os.environ.get('MOLT_BUILDER_ID', ''))
    started, finished = args.started_on, args.finished_on
    if parse_time(finished, '--finished-on') < parse_time(started, '--started-on'):
        raise Error('--finished-on is before --started-on')
    invocation = args.invocation_id or None
    if invocation:
        _string(invocation, '--invocation-id', INVOCATION)

    chromium_src = os.path.abspath(args.chromium_src or os.environ.get(
        'CHROMIUM_SRC') or os.path.join(repo, 'chromium', 'src'))
    out_dirs = [os.path.abspath(d) for d in (args.gn_out_dir or [
        os.environ.get('GN_OUT_DIR') or os.path.join(chromium_src, 'out',
                                                     'MoltBrowser')])]
    places = [(chromium_src, '<chromium-src>'), (repo, '<repo>')]
    if args.payload:
        places.append((args.payload, '<payload>'))
    scrubber = Scrubber(places=places)

    # The source: commit, tag, clean tree.
    commit = git_toplevel_head(repo)
    need(commit, '%s is not a git checkout; the provenance needs the commit' % repo)
    uncommitted = git_uncommitted(repo) if commit else None
    need(not uncommitted, 'uncommitted changes (commit them before a release '
         'build): %s' % ', '.join((uncommitted or [])[:10]))
    if commit:
        tagged = git_local_tag_commit(repo, tag)
        need(not tagged or tagged == commit,
             'HEAD is %s but %s is %s: build the tagged commit'
             % (commit[:12], tag, (tagged or '')[:12]))

    # Chromium: the pin, and the checkout that was built.
    pin_version, pin_revision = read_chromium_pin(repo)
    head = git_toplevel_head(chromium_src)
    need(head == pin_revision, 'the Chromium checkout %s is at %s, not the pin '
         '%s (chromium_version.txt)' % (chromium_src, head or 'no commit',
                                        pin_revision))
    if head == pin_revision:
        checkout_version = chromium_checkout_version(chromium_src)
        need(checkout_version in (None, pin_version),
             'chrome/VERSION says %s, chromium_version.txt %s'
             % (checkout_version, pin_version))
    chromium_revision = head

    # GN args: the configs/ file and what each out dir was generated with.
    gn_file = args.gn_config or 'configs/%s-%s.gn' % (platform, arch)
    gn_file = gn_file.replace('\\', '/')
    if os.path.isabs(gn_file):
        gn_file = os.path.relpath(gn_file, repo).replace(os.sep, '/')
    if not GN_ARGS_FILE.fullmatch(gn_file) or not os.path.isfile(
            os.path.join(repo, gn_file)):
        raise Error('GN args file %s is not a file under configs/' % gn_file)
    with open(os.path.join(repo, gn_file), encoding='utf-8', errors='replace') as f:
        config_args = gn_arg_lines(f.read(), scrubber)
    gn_args = []
    for d in out_dirs:
        path = os.path.join(d, 'args.gn')
        if not need(os.path.isfile(path), 'no args.gn in %s' % d):
            continue
        with open(path, encoding='utf-8', errors='replace') as f:
            effective = gn_arg_lines(f.read(), scrubber)
        gn_args.append({'outDir': out_dir_label(d, chromium_src),
                        'args': effective,
                        'matchesArgsFile': same_args(effective, config_args)})

    # Repository content, as committed.
    deps = []
    if commit:
        base = 'git+%s@%s#' % (REPO_URL, commit)
        for path in ('patches', gn_file, 'src/third_party/rust'):
            try:
                deps.append({'name': path, 'uri': base + path,
                             'digest': committed_digest(repo, commit, path)})
            except Error as e:
                need(False, str(e))
    if args.payload:
        deps.extend(bundled_models(args.payload, repo, release))

    # The builder.
    if args.builder_facts:
        with open(args.builder_facts, 'rb') as f:
            facts = check_builder_facts(_strict_json(f.read(), '--builder-facts'))
    else:
        facts = probe_builder(platform, chromium_src, args.depot_tools,
                              out_dirs, scrubber)
    bversion = facts['version']
    for key in ('clang', 'rustc') + (('xcode',) if platform == 'macos' else ()):
        need(key in bversion, 'could not find the %s version' % key)
    bdeps = []
    need(facts['depotTools'], 'could not find depot_tools (pass --depot-tools)')
    if facts['depotTools']:
        bdeps.append({'name': 'depot_tools', 'uri': DEPOT_TOOLS_GIT,
                      'digest': {'gitCommit': facts['depotTools']}})
    if args.container_image:
        m = CONTAINER_IMAGE.fullmatch(args.container_image)
        if not m:
            raise Error('--container-image must be NAME@sha256:<image id>')
        bdeps.append({'name': 'container-image:' + m.group(1),
                      'digest': {'sha256': m.group(2)}})

    # What the build produced.
    subjects = {}
    for path in args.assets:
        name = os.path.basename(path)
        if not PLAIN_NAME.fullmatch(name):
            raise Error('asset name %r must be a plain file name' % name)
        if name in NOT_BUILD_OUTPUTS or STATEMENT_NAME.fullmatch(name):
            raise Error('%s is not a build output' % name)
        if name in subjects:
            raise Error('asset %s given twice' % name)
        if not os.path.isfile(path) or os.path.getsize(path) == 0:
            raise Error('missing or empty asset: %s' % path)
        subjects[name] = sha256_file(path)

    statement = build_statement(
        tag=tag, version=version, platform=platform, arch=arch,
        gn_args_file=gn_file, release=release, commit=commit,
        uncommitted=uncommitted, chromium_version=pin_version,
        chromium_revision=chromium_revision, dependencies=deps,
        gn_args=gn_args, label=label, builder_version=bversion,
        builder_dependencies=bdeps, started_on=started, finished_on=finished,
        invocation_id=invocation, subjects=subjects.items())
    check_no_personal_data(statement, scrubber)
    data = serialize(statement)
    validate_statement(data, release=release)
    os.makedirs(args.out_dir, exist_ok=True)
    out = os.path.join(args.out_dir, statement_name(platform, arch))
    with open(out, 'wb') as f:
        f.write(data)
    print(out)


def summarize(facts):
    return ('%s-%s: MoltBrowser %s at %s, Chromium %s, builder %s, %s to %s'
            % (facts['platform'], facts['arch'], facts['tag'],
               (facts['commit'] or '?')[:12], facts['chromium_version'] or '?',
               facts['builder'], facts['started'], facts['finished']))


def verify_release_files(bundle_path, keys_b64, files, tag=None,
                         expect_commit=None, require_all=False):
    """Verifies a signed bundle against release files. Returns
    [(facts, key id, statement dict)]."""
    with open(bundle_path, 'rb') as f:
        data = f.read(MAX_BUNDLE_BYTES + 1)
    verified = verify_bundle(data, public_keys(keys_b64))
    statements = []
    by_name = {}
    for payload, kid in verified:
        st = validate_statement(payload, release=True)
        statements.append(st)
        by_name[statement_facts(st)['name']] = (payload, kid, st)
    assets = {}
    for path in files:
        name = os.path.basename(path)
        if name in assets:
            raise Error('%s given twice' % name)
        assets[name] = sha256_file(path)
    # A statement file is checked against its signed copy (with
    # REQUIRE_ALL, check_release() reports one that isn't signed).
    for path in files:
        name = os.path.basename(path)
        if not STATEMENT_NAME.fullmatch(name):
            continue
        if name not in by_name:
            if require_all:
                continue
            raise Error('%s is not in the signed provenance' % name)
        with open(path, 'rb') as f:
            if f.read(MAX_STATEMENT_BYTES + 1) != by_name[name][0]:
                raise Error('%s differs from its signed copy' % name)
    if require_all:
        facts = check_release(statements, assets, tag, expect_commit, True)
    else:
        # Only the given files: each must be a subject, and match. Files no
        # build produces (the manifest, its signature, the Linux repository,
        # the bundle) aren't covered; given nothing else, that is an error,
        # so verify never reports success for files it didn't check.
        not_covered = sorted(n for n in assets if n in NOT_BUILD_OUTPUTS)
        if assets and len(not_covered) == len(assets):
            raise Error('%s %s not a build output: the build provenance does '
                        'not cover it (check it with molt_release_sign.py '
                        'verify)' % (', '.join(not_covered),
                                     'is' if len(not_covered) == 1 else 'are'))
        subject_of = {}
        for st in statements:
            for name, digest in statement_facts(st)['subjects'].items():
                subject_of[name] = digest
        for name, digest in assets.items():
            if STATEMENT_NAME.fullmatch(name) or name in NOT_BUILD_OUTPUTS:
                continue
            if name not in subject_of:
                raise Error('%s is not a subject of the signed provenance' % name)
            if subject_of[name] != digest:
                raise Error('%s does not match its signed provenance' % name)
        facts = check_release(statements, subject_of, tag, expect_commit)
    return [(f, by_name[f['name']][1], by_name[f['name']][2]) for f in facts]


def cmd_verify(args):
    keys = [args.pubkey] if args.pubkey else trusted_keys()
    results = verify_release_files(args.bundle, keys, args.files, args.tag,
                                   args.expect_commit, args.require_all)
    if args.json:
        print(json.dumps([st for _, _, st in results], indent=2, sort_keys=True))
        return
    kids = sorted({kid for _, kid, _ in results})
    print('OK: build provenance signed by key %s' % ', '.join(kids))
    given = {os.path.basename(p) for p in args.files}
    for facts, _, _ in results:
        print('  ' + summarize(facts))
        for name in sorted(facts['subjects']):
            if name in given:
                print('    %s  %s' % (facts['subjects'][name], name))
    # Given, but no build produces them: not checked here.
    for name in sorted(given & (NOT_BUILD_OUTPUTS - {BUNDLE_NAME})):
        print('  not covered by the build provenance (check it with '
              'molt_release_sign.py verify): %s' % name)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = p.add_subparsers(dest='cmd', required=True)

    s = sub.add_parser('start', help='check the label and source tree; print '
                       'the build start time')
    s.add_argument('--builder-id', help='default: $MOLT_BUILDER_ID')
    s.add_argument('--version', help='the release being built: HEAD must be '
                   'its tag if the tag exists here')
    s.add_argument('--repo', default=REPO_ROOT, help=argparse.SUPPRESS)
    s.set_defaults(fn=cmd_start)

    s = sub.add_parser('probe', help="print this machine's toolchain as JSON")
    s.add_argument('--platform', required=True, choices=PLATFORMS)
    s.add_argument('--chromium-src', default=os.environ.get('CHROMIUM_SRC') or
                   os.path.join(REPO_ROOT, 'chromium', 'src'))
    s.add_argument('--depot-tools')
    s.add_argument('--gn-out-dir', action='append')
    s.add_argument('--repo', default=REPO_ROOT, help=argparse.SUPPRESS)
    s.set_defaults(fn=cmd_probe)

    s = sub.add_parser('generate', help='write the statement for one build')
    s.add_argument('--platform', required=True, choices=PLATFORMS)
    s.add_argument('--arch', required=True, choices=ARCHES)
    s.add_argument('--version', required=True, help='0.3.0 or v0.3.0')
    s.add_argument('--started-on', required=True, help='UTC, e.g. 2026-10-01T09:00:00Z')
    s.add_argument('--finished-on', required=True)
    s.add_argument('--builder-id', help='default: $MOLT_BUILDER_ID')
    s.add_argument('--chromium-src', help='default: $CHROMIUM_SRC or chromium/src')
    s.add_argument('--gn-out-dir', action='append',
                   help='default: $GN_OUT_DIR or <chromium-src>/out/MoltBrowser '
                        '(repeat for each architecture of a universal build)')
    s.add_argument('--gn-config', help='default: configs/<platform>-<arch>.gn')
    s.add_argument('--depot-tools')
    s.add_argument('--builder-facts', help='toolchain JSON from `probe` (e.g. '
                   'run inside the build container) instead of probing here')
    s.add_argument('--payload', help='the shipped app/staging folder, for '
                   'bundled models')
    s.add_argument('--container-image', help='NAME@sha256:<id> of the build '
                   'container')
    s.add_argument('--invocation-id', help='the CI run URL, if any')
    s.add_argument('--release', action='store_true',
                   help='refuse anything incomplete (release scripts set it)')
    s.add_argument('--out-dir', default='.')
    s.add_argument('--repo', default=REPO_ROOT, help=argparse.SUPPRESS)
    s.add_argument('assets', nargs='+', help="the build's release assets")
    s.set_defaults(fn=cmd_generate)

    s = sub.add_parser('verify', help='check a signed bundle and assets')
    s.add_argument('--bundle', default=BUNDLE_NAME)
    s.add_argument('--pubkey', help="verify with this key instead of the build's")
    s.add_argument('--tag', help='the release tag the statements must be for')
    s.add_argument('--expect-commit', help='the commit the tag points to')
    s.add_argument('--require-all', action='store_true',
                   help='FILES are the whole release: every build output must '
                        'be covered and every statement signed')
    s.add_argument('--json', action='store_true',
                   help='print the verified statements')
    s.add_argument('files', nargs='*')
    s.set_defaults(fn=cmd_verify)

    args = p.parse_args(argv)
    if getattr(args, 'expect_commit', None) and not GIT_OID.fullmatch(args.expect_commit):
        p.error('--expect-commit must be a full commit id')
    try:
        args.fn(args)
    except (Error, OSError) as e:
        print('error: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
