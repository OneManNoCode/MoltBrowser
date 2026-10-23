#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for the signed build provenance: molt_provenance.py, the `sign` step
of molt_release_sign.py, scripts/release-provenance.sh and
scripts/publish-release-signature.sh.

Builds a fake world: a MoltBrowser checkout, a Chromium checkout and
depot_tools (git repositories with fixed dates, so their commit ids never
change), plus this checkout's tools copied next to them. It generates
statements, signs them with a throwaway Ed25519 key made in the test, and
checks determinism, scrubbing, verification, tampering and the release
scripts. The private key never leaves the temporary folder.

  python3 tools/release_signing/molt_provenance_test.py
  MOLT_PROVENANCE_UPDATE_GOLDEN=1 python3 tools/release_signing/molt_provenance_test.py

With securesystemslib and in-toto-attestation importable, the bundle is also
verified by securesystemslib's DSSE code and every statement parsed with the
in-toto Statement and SLSA Provenance v1 protobuf schemas
(MOLT_REQUIRE_INTEROP=1, as in CI, makes that mandatory).
"""

import base64
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import molt_provenance as prov  # noqa: E402
import molt_release_sign as rs  # noqa: E402
from cryptography.hazmat.primitives import serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric.ed25519 import (  # noqa: E402
    Ed25519PrivateKey)

REPO_ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
GOLDEN = os.path.join(HERE, 'testdata', 'molt-provenance-linux-x64.json')
UPDATE_GOLDEN = os.environ.get('MOLT_PROVENANCE_UPDATE_GOLDEN') == '1'
INTEROP = (importlib.util.find_spec('securesystemslib') is not None and
           importlib.util.find_spec('in_toto_attestation') is not None)
if os.environ.get('MOLT_REQUIRE_INTEROP') == '1' and not INTEROP:
    sys.exit('MOLT_REQUIRE_INTEROP=1 but securesystemslib / '
             'in-toto-attestation are not installed')

PASSPHRASE = 'throwaway test passphrase'
STARTED = '2026-10-01T09:00:00Z'
FINISHED = '2026-10-01T13:30:00Z'
GIT_DATE = '@1790845200 +0000'  # 2026-10-01T09:00:00Z
LABEL = 'test-builder-1'
# A home directory and user the tests pretend to have.
FAKE_HOME = '/home/alice'
FAKE_USER = 'alice'

CONFIG_LINUX = '''# Test config for linux-x64
target_os = "linux"
target_cpu = "x64"
is_official_build = true   # release
google_api_key = ""
safe_browsing_mode = 1
'''
# What `gn gen --args=...` writes for it: sorted, no comments.
ARGS_LINUX = '''google_api_key = ""
is_official_build = true
safe_browsing_mode = 1
target_cpu = "x64"
target_os = "linux"
'''
CONFIG_MAC = '''is_official_build = true
target_os = "mac"
target_cpu = "arm64"
'''
FAKE_MODEL = b'GGUF fake tinyllama for tests\n'
REGISTRY = '''// Test registry.
const ModelRegistryEntry kModelRegistry[] = {
    {"tinyllama-1.1b",
     "TinyLlama 1.1B Chat",
     "TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF",
     "tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf",
     /*revision=*/"%s",
     /*sha256=*/"%s",
     /*size=*/%dULL,
     2147483648ULL,
     1,
     "Q4_K_M",
     "TinyLlama",
     "apache-2.0",
     false},
};
''' % ('f' * 40, hashlib.sha256(FAKE_MODEL).hexdigest(), len(FAKE_MODEL))
CLANG = ('clang version 23.0.0git (https://chromium.googlesource.com/a/'
         'external/github.com/llvm/llvm-project 53d18800)')
RUSTC = ('rustc 1.92.0 b998449636a4 (b998449636a48e2c4a362809085b600a0174e1f2-5'
         '-llvmorg-23-init-19482-g53d18800 chromium)')

LINUX_ASSETS = {
    'MoltBrowser-Linux-x64.deb': b'fake deb\n',
    'MoltBrowser-Linux-x64.rpm': b'fake rpm\n',
    'MoltBrowser-Linux-x64.tar.gz': b'fake tarball\n',
    'MoltBrowser-Linux-x64-deb.spdx.json': b'{"spdxVersion": "SPDX-2.3"}\n',
    'MoltBrowser-Linux-x64-rpm.spdx.json': b'{"spdxVersion": "SPDX-2.3", "r": 1}\n',
    'MoltBrowser-Linux-x64-tar-gz.spdx.json': b'{"spdxVersion": "SPDX-2.3", "t": 1}\n',
}
MAC_ASSETS = {
    'MoltBrowser-macOS-arm64.dmg': b'fake dmg\n',
    'MoltBrowser-macOS-arm64-dmg.spdx.json': b'{"spdxVersion": "SPDX-2.3", "m": 1}\n',
}


def git(cwd, *args):
    subprocess.run(['git', '-C', cwd, '-c', 'commit.gpgsign=false',
                    '-c', 'tag.gpgsign=false', '-c', 'core.autocrlf=false']
                   + list(args), check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


def git_out(cwd, *args):
    return subprocess.run(['git', '-C', cwd] + list(args), check=True,
                          stdout=subprocess.PIPE).stdout.decode().strip()


def write(path, data, mode=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb' if isinstance(data, bytes) else 'w') as f:
        f.write(data)
    if mode:
        os.chmod(path, mode)


def read(path):
    with open(path, 'rb') as f:
        return f.read()


def new_repo(path, files):
    for rel, data in files.items():
        write(os.path.join(path, rel), data)
    subprocess.run(['git', '-c', 'init.defaultBranch=main', 'init', '-q', path],
                   check=True)
    git(path, 'add', '-A')
    git(path, 'commit', '-q', '-m', 'fixture')
    return git_out(path, 'rev-parse', 'HEAD')


def sums(names_data):
    return ''.join('%s  %s\n' % (hashlib.sha256(d).hexdigest(), n)
                   for n, d in sorted(names_data.items())).encode()


class World:
    """A fake MoltBrowser checkout with Chromium, depot_tools and the tools."""

    def __init__(self, root):
        self.root = root
        self.repo = os.path.join(root, 'MoltBrowser')
        self.chromium = os.path.join(self.repo, 'chromium', 'src')
        self.depot = os.path.join(self.repo, 'depot_tools')
        self.dist = os.path.join(self.repo, 'dist')
        self.chromium_rev = new_repo(self.chromium, {
            'chrome/VERSION': 'MAJOR=152\nMINOR=0\nBUILD=7977\nPATCH=169\n',
            'third_party/rust-toolchain/VERSION': RUSTC + '\n',
            '.gitignore': '/out/\n/third_party/llvm-build/\n',
        })
        write(os.path.join(self.chromium, 'third_party', 'llvm-build',
                           'Release+Asserts', 'bin', 'clang'),
              '#!/bin/sh\necho "%s"\necho "InstalledDir: %s/bin"\n'
              % (CLANG, FAKE_HOME), 0o755)
        write(os.path.join(self.chromium, 'out', 'MoltBrowser', 'args.gn'),
              ARGS_LINUX)
        write(os.path.join(self.chromium, 'out', 'MoltBrowser-mac', 'args.gn'),
              CONFIG_MAC)
        self.depot_rev = new_repo(self.depot, {'gclient.py': '# fake\n'})
        self.commit = new_repo(self.repo, {
            '.gitignore': ('/chromium/\n/depot_tools/\n/dist/\n/scripts/\n'
                           '/tools/\n/src/chrome/browser/molt_ai/update/\n'
                           '__pycache__/\n'),
            'chromium_version.txt': ('# pin\nCHROMIUM_VERSION=152.0.7977.169\n'
                                     'CHROMIUM_REVISION=%s\n' % self.chromium_rev),
            'configs/linux-x64.gn': CONFIG_LINUX,
            'configs/macos-arm64.gn': CONFIG_MAC,
            'patches/series': 'base/a.h.patch\nchrome/b.cc.patch\n',
            'patches/base/a.h.patch': '--- a\n+++ b\n',
            'patches/chrome/b.cc.patch': '--- c\n+++ d\n',
            'src/third_party/rust/chromium_crates_io/Cargo.lock': '# lock\n',
            'src/third_party/rust/chromium_crates_io/vendor/adblock-0.10/src/'
            'lib.rs': 'pub fn f() {}\n',
            'src/chrome/browser/molt_ai/runtime/browser_ai_runtime.cc': REGISTRY,
            'website/downloads.json': '{}\n',
            'README.md': 'test\n',
        })
        git(self.repo, 'tag', 'v0.3.0')
        # This checkout's tools, as a release machine has them (ignored by
        # the fake checkout, like build outputs).
        for rel in ('tools/release_signing/molt_provenance.py',
                    'tools/release_signing/molt_release_sign.py',
                    'tools/sbom/molt_sbom.py',
                    'scripts/release-provenance.sh',
                    'scripts/publish-release-signature.sh'):
            dst = os.path.join(self.repo, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(os.path.join(REPO_ROOT, rel), dst)
        self.tool = os.path.join(self.repo, 'tools', 'release_signing',
                                 'molt_provenance.py')
        self.signer = os.path.join(self.repo, 'tools', 'release_signing',
                                   'molt_release_sign.py')
        # A throwaway release key, trusted by the fake checkout.
        self.key = Ed25519PrivateKey.generate()
        self.key_path = os.path.join(root, 'offline', 'release.key')
        write(self.key_path, self.key.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
            serialization.BestAvailableEncryption(PASSPHRASE.encode())), 0o600)
        self.pub_b64 = base64.b64encode(rs.public_raw(self.key.public_key())).decode()
        write(os.path.join(self.repo, 'src', 'chrome', 'browser', 'molt_ai',
                           'update', 'update_signing_keys.cc'),
              '%s\nconstexpr std::string_view kTrustedUpdateSigningKeys[] = {\n'
              '    "%s",\n};\n%s\n' % (rs.KEYS_BEGIN, self.pub_b64, rs.KEYS_END))
        os.makedirs(self.dist)
        # The macOS app payload and builder facts (no Xcode here).
        self.payload = os.path.join(root, 'payload', 'MoltBrowser.app')
        write(os.path.join(self.payload, 'Contents', 'Resources', 'molt_models',
                           'tinyllama-1.1b.gguf'), FAKE_MODEL)
        self.mac_facts = os.path.join(root, 'mac-facts.json')
        write(self.mac_facts, json.dumps({
            'version': {'clang': CLANG, 'rustc': RUSTC,
                        'xcode': '26.0.1 (17A400)', 'macosSdk': '26.0'},
            'depotTools': self.depot_rev}))

    def env(self, **extra):
        env = dict(os.environ)
        for k in ('MOLT_BUILD_STARTED_ON', 'GITHUB_RUN_ID', 'GITHUB_REPOSITORY',
                  'CHROMIUM_SRC', 'GN_OUT_DIR'):
            env.pop(k, None)
        env.update({'HOME': FAKE_HOME, 'USER': FAKE_USER, 'LOGNAME': FAKE_USER,
                    'MOLT_BUILDER_ID': LABEL, 'MOLT_RELEASE_BUILD': '1',
                    'PYTHONDONTWRITEBYTECODE': '1'})
        env.update(extra)
        return env

    def run(self, argv, env=None, cwd=None, check=True):
        r = subprocess.run(argv, env=env or self.env(), cwd=cwd or self.root,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if check and r.returncode != 0:
            raise AssertionError('%s failed (%d):\n%s' % (
                ' '.join(argv[:3]), r.returncode, r.stderr.decode()))
        return r

    def assets(self, files):
        paths = []
        for name, data in files.items():
            path = os.path.join(self.dist, name)
            write(path, data)
            paths.append(path)
        return paths

    def generate(self, platform='linux', arch='x64', assets=None, extra=(),
                 env=None, cwd=None, check=True, out_dir=None):
        if assets is None:
            files = dict(LINUX_ASSETS)
            files['SHA256SUMS-linux-x64'] = sums(LINUX_ASSETS)
            assets = self.assets(files)
        argv = [sys.executable, self.tool, 'generate', '--platform', platform,
                '--arch', arch, '--version', '0.3.0', '--started-on', STARTED,
                '--finished-on', FINISHED, '--chromium-src', self.chromium,
                '--depot-tools', self.depot, '--release',
                '--out-dir', out_dir or self.dist] + list(extra) + assets
        return self.run(argv, env=env, cwd=cwd, check=check)

    def generate_mac(self, **kw):
        files = dict(MAC_ASSETS)
        files['SHA256SUMS-macos-arm64'] = sums(MAC_ASSETS)
        return self.generate(
            'macos', 'arm64', self.assets(files),
            ['--builder-facts', self.mac_facts, '--payload', self.payload,
             '--gn-out-dir', os.path.join(self.chromium, 'out', 'MoltBrowser-mac')],
            **kw)


def capture(fn, *args):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = fn(*args)
    return code, out.getvalue(), err.getvalue()


class PatternTest(unittest.TestCase):
    """The checks take a value only as a whole, in ASCII."""

    def test_checks_refuse_a_trailing_newline_and_other_digits(self):
        arabic_two = '٢'  # ARABIC-INDIC DIGIT TWO: \d without re.ASCII
        cases = [
            (lambda v: prov.parse_time(v, 'startedOn'), STARTED,
             [STARTED + '\n', arabic_two + STARTED[1:]]),
            (prov.split_tag, 'v0.3.0', ['v0.3.0\n', 'v0.3.' + arabic_two]),
            (prov.check_label, LABEL, [LABEL + '\n']),
            (lambda v: prov._digest({'sha256': v}, 'subject'), 'a' * 64,
             ['a' * 64 + '\n']),
            (lambda v: prov._digest({'gitCommit': v}, 'subject'), 'b' * 40,
             ['b' * 40 + '\n']),
            (lambda v: prov._uri(v, 'uri'), 'https://example.com/x',
             ['https://example.com/é']),
            (lambda v: prov.check_builder_facts(
                {'version': {'clang': v}, 'depotTools': None}),
             'clang version 21.0.0', ['clang version 21.0.0\n']),
            (lambda v: prov.check_builder_facts(
                {'version': {}, 'depotTools': v}), 'c' * 40, ['c' * 40 + '\n']),
        ]
        for check, good, bads in cases:
            check(good)
            for bad in bads:
                with self.subTest(bad=bad), self.assertRaises(prov.Error):
                    check(bad)

    def test_patterns_are_ascii(self):
        for name in ('STATEMENT_NAME', 'PLAIN_NAME', 'LABEL', 'SHA256',
                     'GIT_OID', 'TIME', 'VERSION', 'CHROMIUM_VERSION',
                     'GN_ARGS_FILE', 'OUT_DIR', 'INVOCATION', 'CONTAINER_IMAGE',
                     'MODEL_ID', 'TOOL_VERSION', 'URI'):
            pattern = getattr(prov, name)
            self.assertTrue(pattern.flags & re.ASCII, name)
        self.assertIsNone(prov.CHROMIUM_VERSION.fullmatch(
            '152.0.7977.1' + arabic_digits('69')))
        self.assertIsNone(prov.STATEMENT_NAME.fullmatch(
            'molt-provenance-linux-x64.json\n'))
        self.assertIsNone(prov.PLAIN_NAME.fullmatch('MoltBrowser.dmg\n'))

    def test_manifest_version_and_names_are_whole_and_ascii(self):
        def manifest(version, name='MoltBrowser.dmg'):
            return json.dumps({
                'schema': rs.SCHEMA, 'product': rs.PRODUCT,
                'version': version, 'tag': 'v' + version,
                'assets': [{'name': name, 'size': 1, 'sha256': 'a' * 64}],
            }).encode()
        rs.validate_manifest(manifest('0.3.0'))
        for data in (manifest('0.3.0\n'), manifest('0.3.' + arabic_digits('1')),
                     manifest('0.3.0', name='MoltBrowser.dmg\n')):
            with self.subTest(data=data), self.assertRaises(prov.Error):
                rs.validate_manifest(data)


def arabic_digits(digits):
    return ''.join(chr(0x0660 + int(d)) for d in digits)


class ProvenanceTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.saved_env = dict(os.environ)
        os.environ.update({
            'GIT_AUTHOR_NAME': 'Fixture', 'GIT_AUTHOR_EMAIL': 'fixture@example.invalid',
            'GIT_COMMITTER_NAME': 'Fixture',
            'GIT_COMMITTER_EMAIL': 'fixture@example.invalid',
            'GIT_AUTHOR_DATE': GIT_DATE, 'GIT_COMMITTER_DATE': GIT_DATE,
            'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1'})

    @classmethod
    def tearDownClass(cls):
        os.environ.clear()
        os.environ.update(cls.saved_env)

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='molt-provenance-test-')
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.w = World(self.tmp)

    # ------------------------------------------------------------ helpers

    def statement(self, platform='linux', arch='x64'):
        path = os.path.join(self.w.dist, prov.statement_name(platform, arch))
        return json.loads(read(path))

    def release_dir(self):
        """A complete signed release, as `gh release download` gives it."""
        self.w.generate()
        self.w.generate_mac()
        write(os.path.join(self.w.dist, prov.LINUX_REPOSITORY_NAME),
              b'fake signed repository\n')
        files = sorted(os.path.join(self.w.dist, n) for n in os.listdir(self.w.dist))
        manifest = os.path.join(self.w.dist, rs.MANIFEST_NAME)
        with mock.patch('getpass.getpass', return_value=PASSPHRASE):
            code, out, err = capture(rs.main, ['manifest', '--tag', 'v0.3.0',
                                               '--out', manifest] + files)
            self.assertEqual(code, 0, err)
            code, out, err = capture(rs.main, [
                'sign', '--key', self.w.key_path, '--manifest', manifest,
                '--assets-dir', self.w.dist, '--yes'])
        self.assertEqual(code, 0, err)
        self.assertIn('linux-x64: MoltBrowser v0.3.0', out)
        self.assertIn('macos-arm64: MoltBrowser v0.3.0', out)
        return self.w.dist

    def all_files(self, d):
        return sorted(os.path.join(d, n) for n in os.listdir(d))

    def verify(self, d, files=None, key=None, **kw):
        return prov.verify_release_files(
            os.path.join(d, prov.BUNDLE_NAME), [key or self.w.pub_b64],
            self.all_files(d) if files is None else files, **kw)

    def bundle_lines(self, d):
        return read(os.path.join(d, prov.BUNDLE_NAME)).decode().splitlines()

    def write_bundle(self, d, lines):
        write(os.path.join(d, prov.BUNDLE_NAME), '\n'.join(lines) + '\n')

    def edit_payload(self, line, edit):
        env = json.loads(line)
        st = json.loads(base64.b64decode(env['payload']))
        edit(st)
        env['payload'] = base64.b64encode(prov.serialize(st)).decode()
        return json.dumps(env, sort_keys=True, separators=(',', ':'))

    # -------------------------------------------------------------- tests

    def test_pae_matches_the_dsse_spec(self):
        self.assertEqual(prov.pae('http://example.com/HelloWorld', b'hello world'),
                         b'DSSEv1 29 http://example.com/HelloWorld 11 hello world')

    def test_names_never_look_like_installers(self):
        names = [prov.BUNDLE_NAME] + [prov.statement_name(p, a)
                                      for p in prov.PLATFORMS for a in prov.ARCHES]
        for n in names:
            for m in prov.UPDATER_ASSET_MATCHERS:
                self.assertNotIn(m, n)
            self.assertTrue(prov.PLAIN_NAME.fullmatch(n))

    def test_generate_matches_golden_and_is_deterministic(self):
        out = self.w.generate().stdout.decode().strip()
        self.assertEqual(out, os.path.join(self.w.dist, 'molt-provenance-linux-x64.json'))
        first = read(out)
        # Another user, home, working directory and output folder: same bytes.
        other = os.path.join(self.tmp, 'elsewhere')
        os.makedirs(other)
        self.w.generate(env=self.w.env(HOME='/home/bob', USER='bob',
                                       LOGNAME='bob'), cwd=other, out_dir=other)
        self.assertEqual(read(os.path.join(other, os.path.basename(out))), first)
        if UPDATE_GOLDEN:
            write(GOLDEN, first)
        self.assertEqual(first, read(GOLDEN),
                         'statement differs from %s; if intended, rerun with '
                         'MOLT_PROVENANCE_UPDATE_GOLDEN=1 and review the diff'
                         % os.path.relpath(GOLDEN, REPO_ROOT))

    def test_statement_content(self):
        self.w.generate()
        st = self.statement()
        self.assertEqual(st['_type'], 'https://in-toto.io/Statement/v1')
        self.assertEqual(st['predicateType'], 'https://slsa.dev/provenance/v1')
        subjects = {s['name']: s['digest']['sha256'] for s in st['subject']}
        self.assertEqual(sorted(subjects), sorted(list(LINUX_ASSETS) +
                                                  ['SHA256SUMS-linux-x64']))
        self.assertEqual(subjects['MoltBrowser-Linux-x64.deb'],
                         hashlib.sha256(LINUX_ASSETS['MoltBrowser-Linux-x64.deb']).hexdigest())
        bd = st['predicate']['buildDefinition']
        self.assertEqual(bd['externalParameters']['source'],
                         'git+https://github.com/OneManNoCode/MoltBrowser@refs/tags/v0.3.0')
        deps = {d['name']: d for d in bd['resolvedDependencies']}
        self.assertEqual(deps['moltbrowser']['digest'], {'gitCommit': self.w.commit})
        self.assertEqual(deps['chromium']['digest'], {'gitCommit': self.w.chromium_rev})
        self.assertTrue(deps['chromium']['uri'].endswith('@refs/tags/152.0.7977.169'))
        # Directory digests: git's tree id, and the documented sha256 listing.
        self.assertEqual(deps['patches']['digest']['gitTree'],
                         git_out(self.w.repo, 'rev-parse', 'HEAD:patches'))
        listing = ''.join('%s  %s\n' % (hashlib.sha256(read(os.path.join(
            self.w.repo, 'patches', rel))).hexdigest(), rel)
            for rel in ('base/a.h.patch', 'chrome/b.cc.patch', 'series'))
        self.assertEqual(deps['patches']['digest']['sha256'],
                         hashlib.sha256(listing.encode()).hexdigest())
        cfg = deps['configs/linux-x64.gn']['digest']
        self.assertEqual(cfg['sha256'], hashlib.sha256(CONFIG_LINUX.encode()).hexdigest())
        self.assertEqual(cfg['gitBlob'], git_out(self.w.repo, 'rev-parse',
                                                 'HEAD:configs/linux-x64.gn'))
        self.assertIn('gitTree', deps['src/third_party/rust']['digest'])
        gn = bd['internalParameters']['gnArgs']
        self.assertEqual(gn, [{'outDir': 'out/MoltBrowser', 'matchesArgsFile': True,
                               'args': ARGS_LINUX.splitlines()}])
        rd = st['predicate']['runDetails']
        self.assertEqual(rd['builder']['id'],
                         'https://github.com/OneManNoCode/MoltBrowser/builders/' + LABEL)
        self.assertEqual(rd['builder']['version'], {'clang': CLANG, 'rustc': RUSTC})
        self.assertEqual(rd['builder']['builderDependencies'], [{
            'name': 'depot_tools', 'uri': prov.DEPOT_TOOLS_GIT,
            'digest': {'gitCommit': self.w.depot_rev}}])
        self.assertEqual(rd['metadata'], {'startedOn': STARTED,
                                          'finishedOn': FINISHED})
        text = read(os.path.join(self.w.dist, 'molt-provenance-linux-x64.json')).decode()
        for leak in (self.tmp, FAKE_HOME, FAKE_USER):
            self.assertNotIn(leak, text)

    def test_mac_statement_records_xcode_models_and_container(self):
        self.w.generate('macos', 'arm64', self.w.assets(MAC_ASSETS), [
            '--builder-facts', self.w.mac_facts, '--payload', self.w.payload,
            '--gn-out-dir', os.path.join(self.w.chromium, 'out', 'MoltBrowser-mac'),
            '--container-image', 'moltbrowser-linux@sha256:' + 'e' * 64])
        st = self.statement('macos', 'arm64')
        rd = st['predicate']['runDetails']['builder']
        self.assertEqual(rd['version']['xcode'], '26.0.1 (17A400)')
        self.assertIn({'name': 'container-image:moltbrowser-linux',
                       'digest': {'sha256': 'e' * 64}}, rd['builderDependencies'])
        deps = {d['name']: d for d in
                st['predicate']['buildDefinition']['resolvedDependencies']}
        self.assertEqual(deps['model:tinyllama-1.1b'], {
            'name': 'model:tinyllama-1.1b',
            'uri': 'https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF'
                   '/resolve/%s/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf' % ('f' * 40),
            'digest': {'sha256': hashlib.sha256(FAKE_MODEL).hexdigest()}})
        # A bundled model that doesn't match its pin is refused.
        write(os.path.join(self.w.payload, 'Contents', 'Resources', 'molt_models',
                           'tinyllama-1.1b.gguf'), b'other bytes')
        r = self.w.generate('macos', 'arm64', self.w.assets(MAC_ASSETS), [
            '--builder-facts', self.w.mac_facts, '--payload', self.w.payload,
            '--gn-out-dir', os.path.join(self.w.chromium, 'out', 'MoltBrowser-mac')],
            check=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b'does not match its pinned SHA-256', r.stderr)

    def test_scrubs_paths_users_and_secrets(self):
        args = (ARGS_LINUX +
                'cc_wrapper = "%s/.cache/sccache"\n' % FAKE_HOME +
                'custom_path = "C:\\\\Users\\\\Alice\\\\toolchain"\n'
                'mac_path = "/Users/alice/sdk"\n'
                'extra_dir = "%s/third_party/x"\n' % self.w.chromium +
                'google_api_key = "AIza%s"\n' % ('A1b2C3d4' * 5)[:35] +
                'my_token = "abc"  # a comment\n')
        write(os.path.join(self.w.chromium, 'out', 'MoltBrowser', 'args.gn'), args)
        self.w.generate()
        gn = self.statement()['predicate']['buildDefinition'][
            'internalParameters']['gnArgs'][0]
        self.assertFalse(gn['matchesArgsFile'])
        self.assertIn('cc_wrapper = "~/.cache/sccache"', gn['args'])
        self.assertIn('custom_path = "~\\\\toolchain"', gn['args'])
        self.assertIn('mac_path = "~/sdk"', gn['args'])
        self.assertIn('extra_dir = "<chromium-src>/third_party/x"', gn['args'])
        self.assertIn('google_api_key = "<redacted>"', gn['args'])
        self.assertIn('my_token = "<redacted>"', gn['args'])
        text = read(os.path.join(self.w.dist, 'molt-provenance-linux-x64.json')).decode()
        for leak in ('alice', 'Alice', 'AIza', self.tmp):
            self.assertNotIn(leak, text)

    def test_scrubber_unit(self):
        s = prov.Scrubber(places=[('/work/cr/src', '<chromium-src>')],
                          markers=(['/home/carol'], ['carol'], ['rajs-macbook']))
        self.assertEqual(s.text('/work/cr/src/out/x'), '<chromium-src>/out/x')
        self.assertEqual(s.text('/home/carol/bin and /home/dave/x'), '~/bin and ~/x')
        self.assertEqual(s.text('/Volumes/carol/x'), '/Volumes/<user>/x')
        self.assertEqual(s.text('built on rajs-macbook.local'), 'built on <host>.local')
        self.assertEqual(s.text('C:\\Users\\Carol\\x'), '~\\x')
        self.assertEqual(s.text('/root/.cargo'), '~/.cargo')
        self.assertEqual(s.text('rustc 1.92.0-dev /usr/lib'), 'rustc 1.92.0-dev /usr/lib')
        self.assertEqual(s.problems('/Users/x/y'), ['a home directory path'])
        self.assertEqual(s.problems('rajs-macbook'), ["this machine's hostname"])
        self.assertEqual(s.problems('/carol/'), ['your user name in a path'])
        self.assertEqual(s.problems('ghp_' + 'a' * 36),
                         ['something that looks like a credential'])
        self.assertEqual(s.problems('https://huggingface.co/home/x'), [])

    def test_refuses_personal_data_in_builder_facts(self):
        write(self.w.mac_facts, json.dumps({
            'version': {'clang': 'clang version 1 (/Users/bob/llvm)',
                        'rustc': RUSTC, 'xcode': '26.0'},
            'depotTools': self.w.depot_rev}))
        r = self.w.generate('macos', 'arm64', self.w.assets(MAC_ASSETS), [
            '--builder-facts', self.w.mac_facts,
            '--gn-out-dir', os.path.join(self.w.chromium, 'out', 'MoltBrowser-mac')],
            check=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b'refusing to write provenance', r.stderr)
        self.assertIn(b'a home directory path', r.stderr)

    def test_builder_label_rules(self):
        r = self.w.generate(env=self.w.env(MOLT_BUILDER_ID=''), check=False)
        self.assertIn(b'set MOLT_BUILDER_ID', r.stderr)
        r = self.w.generate(env=self.w.env(MOLT_BUILDER_ID='Has Spaces'), check=False)
        self.assertIn(b'lower-case letters', r.stderr)
        # The user name (or hostname) as the label: refused.
        r = self.w.generate(env=self.w.env(MOLT_BUILDER_ID=FAKE_USER), check=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b'user name', r.stderr)
        r = self.w.run([sys.executable, self.w.tool, 'start'],
                       env=self.w.env(MOLT_BUILDER_ID=FAKE_USER), check=False)
        self.assertIn(b'is your user name or hostname', r.stderr)

    def test_start(self):
        r = self.w.run([sys.executable, self.w.tool, 'start', '--version', '0.3.0'])
        self.assertTrue(prov.TIME.fullmatch(r.stdout.decode().strip()))
        r = self.w.run([sys.executable, self.w.tool, 'start'],
                       env=self.w.env(MOLT_BUILD_STARTED_ON=STARTED))
        self.assertEqual(r.stdout.decode().strip(), STARTED)
        r = self.w.run([sys.executable, self.w.tool, 'start'],
                       env=self.w.env(MOLT_BUILD_STARTED_ON='yesterday'), check=False)
        self.assertIn(b'MOLT_BUILD_STARTED_ON must be a UTC time', r.stderr)
        # A new tag on another commit: HEAD is not what is being released.
        write(os.path.join(self.w.repo, 'docs', 'x.txt'), 'x')
        git(self.w.repo, 'add', '-A')
        git(self.w.repo, 'commit', '-q', '-m', 'docs')
        r = self.w.run([sys.executable, self.w.tool, 'start', '--version', '0.3.0'],
                       check=False)
        self.assertIn(b'build the tagged commit', r.stderr)
        # Uncommitted source: refused; docs and website changes are fine.
        git(self.w.repo, 'tag', '-f', 'v0.3.0')
        write(os.path.join(self.w.repo, 'website', 'downloads.json'), '{"v": 1}\n')
        write(os.path.join(self.w.repo, 'docs', 'NOTES.md'), 'notes\n')
        self.w.run([sys.executable, self.w.tool, 'start', '--version', '0.3.0'])
        write(os.path.join(self.w.repo, 'src', 'new.cc'), '// new\n')
        r = self.w.run([sys.executable, self.w.tool, 'start'], check=False)
        self.assertIn(b'uncommitted changes', r.stderr)
        self.assertIn(b'src/new.cc', r.stderr)

    def test_release_refusals_and_dev_mode(self):
        write(os.path.join(self.w.repo, 'configs', 'linux-x64.gn'), CONFIG_LINUX + '# x\n')
        r = self.w.generate(check=False)
        self.assertIn(b'uncommitted changes', r.stderr)
        self.assertIn(b'configs/linux-x64.gn', r.stderr)
        # Without --release (a developer trying the scripts) it is written,
        # marked as such, and it would never verify.
        env = self.w.env(MOLT_RELEASE_BUILD='0')
        argv = [sys.executable, self.w.tool, 'generate', '--platform', 'linux',
                '--arch', 'x64', '--version', '0.3.0-dev', '--started-on', STARTED,
                '--finished-on', FINISHED, '--chromium-src', self.w.chromium,
                '--depot-tools', self.w.depot, '--out-dir', self.w.dist,
                os.path.join(self.w.dist, 'x.deb')]
        write(argv[-1], b'x')
        r = self.w.run(argv, env=env)
        self.assertIn(b'warning: uncommitted changes', r.stderr)
        data = read(os.path.join(self.w.dist, 'molt-provenance-linux-x64.json'))
        st = prov.validate_statement(data, release=False)
        self.assertFalse(st['predicate']['buildDefinition']['externalParameters']
                         ['releaseBuild'])
        self.assertEqual(st['predicate']['buildDefinition']['resolvedDependencies']
                         [0]['annotations'], {'uncommittedChanges': ['configs/linux-x64.gn']})
        with self.assertRaisesRegex(prov.Error, 'not a release build'):
            prov.validate_statement(data, release=True)
        git(self.w.repo, 'checkout', '--', 'configs')
        # Chromium not at the pin.
        write(os.path.join(self.w.chromium, 'chrome', 'VERSION'), 'MAJOR=153\n')
        git(self.w.chromium, 'commit', '-q', '-am', 'move')
        r = self.w.generate(check=False)
        self.assertIn(b'not the pin', r.stderr)
        git(self.w.chromium, 'reset', '-q', '--hard', 'HEAD~1')
        # No args.gn for the out dir.
        os.remove(os.path.join(self.w.chromium, 'out', 'MoltBrowser', 'args.gn'))
        r = self.w.generate(check=False)
        self.assertIn(b'no args.gn', r.stderr)
        write(os.path.join(self.w.chromium, 'out', 'MoltBrowser', 'args.gn'), ARGS_LINUX)
        # Release-only assets are not build outputs.
        r = self.w.generate(assets=self.w.assets({prov.MANIFEST_NAME: b'{}'}),
                            check=False)
        self.assertIn(b'is not a build output', r.stderr)
        self.w.generate()

    def test_sign_and_verify_a_release(self):
        d = self.release_dir()
        results = self.verify(d, tag='v0.3.0', expect_commit=self.w.commit,
                              require_all=True)
        self.assertEqual(sorted(f['name'] for f, _, _ in results),
                         ['molt-provenance-linux-x64.json',
                          'molt-provenance-macos-arm64.json'])
        self.assertEqual({kid for _, kid, _ in results},
                         {rs.key_id(rs.public_raw(self.w.key.public_key()))})
        # Deterministic: Ed25519 is, and the lines are in name order.
        lines = self.bundle_lines(d)
        self.assertEqual(len(lines), 2)
        self.assertEqual([prov.statement_facts(json.loads(base64.b64decode(
            json.loads(ln)['payload'])))['name'] for ln in lines],
            ['molt-provenance-linux-x64.json', 'molt-provenance-macos-arm64.json'])
        # The manifest tool ignores the bundle, like the signature.
        rs.verify_files(os.path.join(d, rs.MANIFEST_NAME),
                        os.path.join(d, rs.SIGNATURE_NAME), [self.w.pub_b64],
                        self.all_files(d), require_all=True)
        # One downloaded installer, checked by anyone (CLI, trusted keys of
        # the checkout).
        deb = os.path.join(d, 'MoltBrowser-Linux-x64.deb')
        r = self.w.run([sys.executable, self.w.tool, 'verify', '--bundle',
                        os.path.join(d, prov.BUNDLE_NAME), deb])
        self.assertIn(b'OK: build provenance signed by key', r.stdout)
        self.assertIn(b'MoltBrowser-Linux-x64.deb', r.stdout)
        r = self.w.run([sys.executable, self.w.tool, 'verify', '--json', '--bundle',
                        os.path.join(d, prov.BUNDLE_NAME), '--tag', 'v0.3.0',
                        '--expect-commit', self.w.commit, '--require-all']
                       + self.all_files(d))
        self.assertEqual(len(json.loads(r.stdout)), 2)
        if INTEROP:
            self.check_interop(d)

    def check_interop(self, d):
        from google.protobuf import json_format
        from in_toto_attestation.predicates.provenance.v1 import provenance_pb2
        from in_toto_attestation.v1 import statement_pb2
        from in_toto_attestation.v1.statement import Statement
        from securesystemslib.dsse import Envelope
        from securesystemslib.signer import SSlibKey
        kid = rs.key_id(rs.public_raw(self.w.key.public_key()))
        key = SSlibKey(kid, 'ed25519', 'ed25519', {'public': rs.public_raw(
            self.w.key.public_key()).hex()})
        for line in self.bundle_lines(d):
            env = Envelope.from_dict(json.loads(line))
            env.verify([key], 1)
            st = json.loads(env.payload)
            pb = json_format.ParseDict(st, statement_pb2.Statement())
            Statement.copy_from_pb(pb).validate()
            json_format.ParseDict(st['predicate'], provenance_pb2.Provenance())

    def test_tampering_is_caught(self):
        d = self.release_dir()
        good = self.bundle_lines(d)

        def expect(msg, lines=None, **kw):
            if lines is not None:
                self.write_bundle(d, lines)
            with self.assertRaisesRegex(prov.Error, msg):
                self.verify(d, **kw)
            self.write_bundle(d, good)

        def subject(st):
            st['subject'][0]['digest']['sha256'] = '0' * 64

        def builder(st):
            st['predicate']['runDetails']['builder']['id'] += 'x'

        def commit(st):
            st['predicate']['buildDefinition']['resolvedDependencies'][0][
                'digest']['gitCommit'] = 'a' * 40

        for edit in (subject, builder, commit):
            expect('signature does not match', [self.edit_payload(good[0], edit)]
                   + good[1:], require_all=True)
        # Another key.
        other = base64.b64encode(rs.public_raw(
            Ed25519PrivateKey.generate().public_key())).decode()
        with self.assertRaisesRegex(prov.Error, 'signature does not match'):
            self.verify(d, key=other)
        # A signature moved onto the other statement's payload.
        a, b = json.loads(good[0]), json.loads(good[1])
        a['signatures'] = b['signatures']
        expect('signature does not match', [json.dumps(a), good[1]])
        # The envelope says something else than in-toto.
        a = json.loads(good[0])
        a['payloadType'] = 'application/json'
        expect('payloadType', [json.dumps(a)] + good[1:])
        # Structure: duplicate keys, CRLF, junk, bad base64, extra fields.
        expect('duplicate key', [good[0][:-1] + ',"payloadType":"x"}'] + good[1:])
        with open(os.path.join(d, prov.BUNDLE_NAME), 'wb') as f:
            f.write(('\r\n'.join(good) + '\r\n').encode())
        with self.assertRaisesRegex(prov.Error, 'LF-terminated'):
            self.verify(d)
        expect('not valid JSON', good + ['garbage'])
        a = json.loads(good[0])
        a['payload'] = a['payload'][:-2] + '!!'
        expect('base64', [json.dumps(a)] + good[1:])
        a = json.loads(good[0])
        a['extra'] = 1
        expect('unexpected extra', [json.dumps(a)] + good[1:])
        expect('no envelopes', [''])
        # A release asset changed after signing.
        deb = os.path.join(d, 'MoltBrowser-Linux-x64.deb')
        original = read(deb)
        write(deb, original + b'!')
        expect('MoltBrowser-Linux-x64.deb does not match its signed provenance',
               require_all=True)
        expect('MoltBrowser-Linux-x64.deb does not match', files=[deb])
        write(deb, original)
        # The plain statement on the release differs from the signed one.
        path = os.path.join(d, 'molt-provenance-linux-x64.json')
        plain = read(path)
        write(path, plain.replace(FINISHED.encode(), b'2026-10-01T13:31:00Z'))
        expect('differs from its signed copy', require_all=True)
        write(path, plain)
        # Wrong tag or commit.
        expect('is for v0.3.0, not v0.3.1', tag='v0.3.1')
        expect('was built from', expect_commit='b' * 40)
        # An asset no build produced, and a statement nobody signed.
        write(os.path.join(d, 'extra.txt'), b'x')
        expect('no build provenance for: extra.txt', require_all=True)
        os.remove(os.path.join(d, 'extra.txt'))
        expect('unsigned: molt-provenance-macos-arm64.json', [good[0]],
               require_all=True)
        # A file that the provenance doesn't list.
        expect('is not a subject', files=[os.path.join(d, prov.LINUX_REPOSITORY_NAME),
                                          path, os.path.join(self.tmp, 'MoltBrowser',
                                                             'README.md')])
        # Given alone, a release file that no build produced is refused, not
        # passed over: verify never says OK about a file it didn't check.
        for name in (prov.LINUX_REPOSITORY_NAME, rs.MANIFEST_NAME,
                     rs.SIGNATURE_NAME):
            expect('%s is not a build output' % re.escape(name),
                   files=[os.path.join(d, name)])
        r = self.w.run([sys.executable, self.w.tool, 'verify', '--bundle',
                        os.path.join(d, prov.BUNDLE_NAME),
                        os.path.join(d, prov.LINUX_REPOSITORY_NAME)],
                       check=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn(b'OK', r.stdout)
        # With an installer, the installer is checked and the other file is
        # named as not covered.
        r = self.w.run([sys.executable, self.w.tool, 'verify', '--bundle',
                        os.path.join(d, prov.BUNDLE_NAME), deb,
                        os.path.join(d, prov.LINUX_REPOSITORY_NAME)])
        self.assertIn(b'MoltBrowser-Linux-x64.deb', r.stdout)
        self.assertIn(b'not covered by the build provenance (check it with '
                      b'molt_release_sign.py verify): '
                      b'MoltBrowser-Linux-repository.tar', r.stdout)
        # A statement given alone is checked against its signed copy.
        self.verify(d, files=[path])
        write(path, plain.replace(FINISHED.encode(), b'2026-10-01T13:31:00Z'))
        expect('differs from its signed copy', files=[path])
        write(path, plain)
        expect('molt-provenance-macos-arm64.json is not in the signed',
               [good[0]], files=[os.path.join(
                   d, 'molt-provenance-macos-arm64.json')])
        # Still good after all that.
        self.verify(d, tag='v0.3.0', expect_commit=self.w.commit, require_all=True)

    def test_statement_validation_is_strict(self):
        self.w.generate()
        data = read(os.path.join(self.w.dist, 'molt-provenance-linux-x64.json'))
        prov.validate_statement(data)

        def broken(edit, msg):
            st = json.loads(data)
            edit(st)
            with self.assertRaisesRegex(prov.Error, msg):
                prov.validate_statement(prov.serialize(st))

        broken(lambda st: st['predicate'].update(byproducts=[]), 'unexpected byproducts')
        broken(lambda st: st.update(_type='https://in-toto.io/Statement/v0.1'),
               'not an in-toto Statement v1')
        broken(lambda st: st['subject'].append(dict(st['subject'][0])), 'listed twice')
        broken(lambda st: st['subject'][0].update(name='../x'), 'malformed')
        broken(lambda st: st['subject'][0]['digest'].update(sha256='A' * 64), 'malformed')
        broken(lambda st: st['subject'][0].update(name=prov.MANIFEST_NAME),
               'not a build output')
        broken(lambda st: st['predicate']['buildDefinition']['externalParameters']
               .update(tag='v0.3.1'), 'tag and version disagree')
        broken(lambda st: st['predicate']['buildDefinition']['externalParameters']
               .update(releaseBuild=1), 'releaseBuild')
        broken(lambda st: st['predicate']['buildDefinition']['resolvedDependencies']
               .reverse(), 'must start with moltbrowser')
        broken(lambda st: st['predicate']['runDetails']['metadata']
               .update(finishedOn='2026-09-30T00:00:00Z'), 'before startedOn')
        broken(lambda st: st['predicate']['runDetails']['builder']['version']
               .update(gcc='13'), 'unknown key')
        broken(lambda st: st['predicate']['runDetails']['builder']
               .update(builderDependencies=[]), 'depot_tools revision is missing')
        broken(lambda st: st['predicate']['buildDefinition']['internalParameters']
               ['gnArgs'][0]['args'].append('x = "\x07"'), 'printable')
        with self.assertRaisesRegex(prov.Error, 'NaN'):
            prov.validate_statement(data.replace(b'"releaseBuild": true',
                                                 b'"releaseBuild": NaN'))
        with self.assertRaisesRegex(prov.Error, 'too large'):
            prov.validate_statement(b' ' * (prov.MAX_STATEMENT_BYTES + 1))

    def test_sign_refuses_incomplete_provenance(self):
        self.w.generate()
        manifest = os.path.join(self.tmp, rs.MANIFEST_NAME)

        def sign(files, *extra, tag='v0.3.0', edit=None):
            code, out, err = capture(rs.main, ['manifest', '--tag', tag,
                                               '--out', manifest] + files)
            self.assertEqual(code, 0, err)
            if edit:
                edit()
            with mock.patch('getpass.getpass', return_value=PASSPHRASE):
                return capture(rs.main, ['sign', '--key', self.w.key_path,
                                         '--manifest', manifest, '--yes'] + list(extra))

        linux = [os.path.join(self.w.dist, n) for n in sorted(os.listdir(self.w.dist))]
        installers = [p for p in linux
                      if not prov.STATEMENT_NAME.fullmatch(os.path.basename(p))]
        code, _, err = sign(installers, '--assets-dir', self.w.dist)
        self.assertEqual(code, 1)
        self.assertIn('lists no build provenance', err)
        code, out, err = sign(installers, '--no-provenance')
        self.assertEqual(code, 0, err)
        self.assertIn('WITHOUT build provenance', err)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, prov.BUNDLE_NAME)))
        code, _, err = sign(linux)
        self.assertIn('pass --assets-dir', err)
        # An installer the statements don't cover.
        extra = os.path.join(self.w.dist, 'MoltBrowser-Linux-x64.AppImage')
        write(extra, b'appimage')
        code, _, err = sign(linux + [extra], '--assets-dir', self.w.dist)
        self.assertIn('no build provenance for: MoltBrowser-Linux-x64.AppImage', err)
        os.remove(extra)
        # An installer rebuilt after the statement was written.
        deb = os.path.join(self.w.dist, 'MoltBrowser-Linux-x64.deb')
        write(deb, b'rebuilt deb\n')
        code, _, err = sign(linux, '--assets-dir', self.w.dist)
        self.assertIn('does not match its signed provenance', err)
        write(deb, LINUX_ASSETS['MoltBrowser-Linux-x64.deb'])
        # The statement in the folder is not the one the manifest lists.
        path = os.path.join(self.w.dist, 'molt-provenance-linux-x64.json')
        original = read(path)
        code, _, err = sign(linux, '--assets-dir', self.w.dist,
                            edit=lambda: write(path, original + b' '))
        self.assertIn('is not the file the manifest lists', err)
        write(path, original)
        # Statements for another version.
        code, _, err = sign(linux, '--assets-dir', self.w.dist, tag='v0.3.1')
        self.assertIn('is for v0.3.0, not v0.3.1', err)
        # And all is well again.
        code, _, err = sign(linux, '--assets-dir', self.w.dist)
        self.assertEqual(code, 0, err)
        self.assertTrue(os.path.exists(os.path.join(self.tmp, prov.BUNDLE_NAME)))

    def test_wrapper_script(self):
        script = os.path.join(self.w.repo, 'scripts', 'release-provenance.sh')
        env = self.w.env(CHROMIUM_SRC=self.w.chromium)
        env.pop('MOLT_BUILD_STARTED_ON', None)
        # A Linux build has its own out dir (scripts/configure.sh), which
        # the wrapper reads by default, not the macOS build's out/MoltBrowser.
        os.rename(os.path.join(self.w.chromium, 'out', 'MoltBrowser'),
                  os.path.join(self.w.chromium, 'out', 'MoltBrowser-linux'))
        r = self.w.run(['bash', script, '--start', '--version', '0.3.0'], env=env)
        started = r.stdout.decode().strip()
        self.assertTrue(prov.TIME.fullmatch(started))
        files = dict(LINUX_ASSETS)
        assets = self.w.assets(files)
        r = self.w.run(['bash', script, '--platform', 'linux', '--arch', 'x64',
                        '--version', '0.3.0', '--out-dir', self.w.dist] + assets,
                       env=env, check=False)
        self.assertIn(b'MOLT_BUILD_STARTED_ON is not set', r.stderr)
        env.update(MOLT_BUILD_STARTED_ON=started, GITHUB_RUN_ID='42',
                   GITHUB_REPOSITORY='OneManNoCode/MoltBrowser',
                   GITHUB_SERVER_URL='https://github.com', GITHUB_RUN_ATTEMPT='2')
        r = self.w.run(['bash', script, '--platform', 'linux', '--arch', 'x64',
                        '--version', '0.3.0', '--out-dir', self.w.dist] + assets,
                       env=env)
        self.assertEqual(r.stdout.decode().strip(),
                         os.path.join(self.w.dist, 'molt-provenance-linux-x64.json'))
        md = self.statement()['predicate']['runDetails']['metadata']
        self.assertEqual(md['startedOn'], started)
        self.assertEqual(md['invocationId'], 'https://github.com/OneManNoCode/'
                         'MoltBrowser/actions/runs/42/attempts/2')
        gn = self.statement()['predicate']['buildDefinition'][
            'internalParameters']['gnArgs']
        self.assertEqual([g['outDir'] for g in gn], ['out/MoltBrowser-linux'])
        # The repository's own depot_tools was found and used.
        deps = self.statement()['predicate']['runDetails']['builder'][
            'builderDependencies']
        self.assertEqual(deps[0]['digest']['gitCommit'], self.w.depot_rev)

    def test_publish_script(self):
        d = self.release_dir()
        release = os.path.join(self.tmp, 'gh-release')
        uploaded = os.path.join(self.tmp, 'gh-uploaded')
        os.makedirs(uploaded)
        shutil.copytree(d, release)
        offline = os.path.join(self.tmp, 'offline')
        for name in (rs.MANIFEST_NAME, rs.SIGNATURE_NAME, prov.BUNDLE_NAME):
            shutil.move(os.path.join(release, name), os.path.join(offline, name))
        bindir = os.path.join(self.tmp, 'bin')
        write(os.path.join(bindir, 'gh'), FAKE_GH, 0o755)
        # This Python (with "cryptography"), also when it is a virtualenv's.
        write(os.path.join(bindir, 'python3'),
              '#!/bin/sh\nexec "%s" "$@"\n' % sys.executable, 0o755)
        env = self.w.env(PATH=bindir + os.pathsep + os.environ['PATH'],
                         FAKE_GH_RELEASE=release, FAKE_GH_UPLOADED=uploaded,
                         FAKE_GH_COMMIT=self.w.commit)
        script = os.path.join(self.w.repo, 'scripts', 'publish-release-signature.sh')
        signed = [os.path.join(offline, n) for n in
                  (rs.MANIFEST_NAME, rs.SIGNATURE_NAME, prov.BUNDLE_NAME)]
        r = self.w.run(['bash', script, 'v0.3.0'] + signed[:2], env=env, check=False)
        self.assertEqual(r.returncode, 2)
        self.assertEqual(os.listdir(uploaded), [])
        # The tag points at another commit than the one built.
        r = self.w.run(['bash', script, 'v0.3.0'] + signed,
                       env=dict(env, FAKE_GH_COMMIT='c' * 40), check=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b'was built from', r.stderr)
        self.assertEqual(os.listdir(uploaded), [])
        # An asset replaced on the draft after signing.
        deb = os.path.join(release, 'MoltBrowser-Linux-x64.deb')
        write(deb, b'swapped\n')
        r = self.w.run(['bash', script, 'v0.3.0'] + signed, env=env, check=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(os.listdir(uploaded), [])
        write(deb, LINUX_ASSETS['MoltBrowser-Linux-x64.deb'])
        # A renamed bundle would be published under the wrong name.
        renamed = os.path.join(offline, 'provenance.jsonl')
        shutil.copy(signed[2], renamed)
        r = self.w.run(['bash', script, 'v0.3.0'] + signed[:2] + [renamed],
                       env=env, check=False)
        self.assertIn(b'must be named', r.stderr)
        r = self.w.run(['bash', script, 'v0.3.0'] + signed, env=env)
        self.assertEqual(sorted(os.listdir(uploaded)),
                         sorted([rs.MANIFEST_NAME, rs.SIGNATURE_NAME, prov.BUNDLE_NAME]))


FAKE_GH = r'''#!/bin/bash
# A stand-in for the GitHub CLI: serves $FAKE_GH_RELEASE as the draft.
set -euo pipefail
case "$1 $2" in
  "release download")
    while [ $# -gt 0 ]; do
      if [ "$1" = "--dir" ]; then dir="$2"; shift; fi
      shift
    done
    mkdir -p "$dir"
    cp "$FAKE_GH_RELEASE"/* "$dir"/ ;;
  "release upload")
    shift 3
    for f in "$@"; do
      if [ -f "$f" ]; then cp "$f" "$FAKE_GH_UPLOADED"/; fi
    done ;;
  "api repos/OneManNoCode/MoltBrowser/commits/v0.3.0")
    echo "$FAKE_GH_COMMIT" ;;
  *) echo "fake gh: unexpected: $*" >&2; exit 1 ;;
esac
'''


if __name__ == '__main__':
    unittest.main()
