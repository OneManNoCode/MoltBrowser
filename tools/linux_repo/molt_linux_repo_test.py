#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for molt_linux_repo.py, with throwaway keys.

Builds a tiny .deb (dpkg-deb) and .rpm (rpmbuild), a throwaway GnuPG key
(created here, in a temporary GNUPGHOME that is deleted afterwards), and
runs the whole flow: CI build -> unsigned bundle -> offline sign -> unpack
-> verify. Then checks the result with the tools users have: gpgv on every
signature, rpmkeys on the signed rpm, createrepo_c's metadata, and
`apt-get update` against the repository as a file: source in a scratch
directory (no chroot, no root needed). Tampering must be caught.

Tests skip when a tool is missing; the first line of output says which tools
were found. MOLT_REQUIRE_REPO_TOOLS=1 (CI) turns a missing tool into a
failure.

  python3 tools/linux_repo/molt_linux_repo_test.py -v
"""

import contextlib
import gzip
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import molt_linux_repo as mlr  # noqa: E402

TOOLS = ('gpg', 'gpgv', 'gpgconf', 'dpkg-deb', 'rpmbuild', 'rpm', 'rpmsign',
         'rpmkeys', 'createrepo_c', 'apt-get', 'apt-cache', 'apt-ftparchive',
         'dnf')
FOUND = {t: shutil.which(t) for t in TOOLS}
REQUIRED = ('gpg', 'gpgv', 'gpgconf', 'dpkg-deb', 'rpmbuild', 'rpm', 'rpmsign',
            'rpmkeys', 'createrepo_c', 'apt-get', 'apt-cache')
MISSING = [t for t in REQUIRED if not FOUND[t]]
if MISSING and os.environ.get('MOLT_REQUIRE_REPO_TOOLS') == '1':
    sys.exit('MOLT_REQUIRE_REPO_TOOLS=1 but missing: ' + ', '.join(MISSING))


def needs(*tools):
    missing = [t for t in tools if not FOUND[t]]
    return unittest.skipIf(missing, 'needs ' + ', '.join(missing))


def sh(cmd, **kw):
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, **kw)
    if proc.returncode != 0:
        raise AssertionError('%s failed:\n%s\n%s' % (' '.join(cmd), proc.stdout,
                                                     proc.stderr))
    return proc.stdout


def read(path, mode='rb'):
    with open(path, mode) as f:
        return f.read()


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb' if isinstance(data, bytes) else 'w') as f:
        f.write(data)


def run_main(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = mlr.main(argv)
    return code, out.getvalue(), err.getvalue()


# ----------------------------------------------------------- the fixtures


def build_deb(tmp, version='0.3.0', name='moltbrowser', payload=b'hello\n'):
    root = os.path.join(tmp, 'deb-%s-%s' % (name, version))
    write(os.path.join(root, 'opt', 'moltbrowser', 'hello'), payload)
    write(os.path.join(root, 'DEBIAN', 'control'),
          'Package: %s\nVersion: %s\nSection: web\nPriority: optional\n'
          'Architecture: amd64\nRecommends: tor\n'
          'Maintainer: GenEye AI Labs Inc. <support@geneye.ai>\n'
          'Description: MoltBrowser test package\n'
          ' A throwaway package for molt_linux_repo_test.py.\n'
          ' .\n Second paragraph.\n' % (name, version))
    out = os.path.join(tmp, '%s_%s_amd64.deb' % (name, version))
    sh(['dpkg-deb', '--root-owner-group', '--build', root, out])
    return out


def build_rpm(tmp, version='0.3.0'):
    top = os.path.join(tmp, 'rpmbuild')
    for d in ('BUILD', 'RPMS', 'SOURCES', 'SPECS', 'SRPMS', 'BUILDROOT'):
        os.makedirs(os.path.join(top, d), exist_ok=True)
    spec = os.path.join(top, 'SPECS', 'moltbrowser.spec')
    write(spec, """Name: moltbrowser
Version: %s
Release: 1
Summary: MoltBrowser test package
License: GPLv3
BuildArch: x86_64
%%global debug_package %%{nil}
%%global __os_install_post %%{nil}

%%description
A throwaway package for molt_linux_repo_test.py.

%%install
mkdir -p %%{buildroot}/opt/moltbrowser
echo hello > %%{buildroot}/opt/moltbrowser/hello

%%files
/opt/moltbrowser/hello
""" % version)
    sh(['rpmbuild', '--define', '_topdir ' + top, '--target', 'x86_64',
        '-bb', spec])
    built = os.path.join(top, 'RPMS', 'x86_64',
                         'moltbrowser-%s-1.x86_64.rpm' % version)
    out = os.path.join(tmp, 'MoltBrowser-Linux-x64.rpm')
    shutil.copyfile(built, out)
    return out


class ThrowawayKey:
    """A GnuPG home with a fresh, passphrase-less primary key (certify) and
    signing subkey. Created per test run, deleted afterwards."""

    def __init__(self, parent, uid):
        # Short path: gpg-agent's socket path has a length limit.
        self.home = tempfile.mkdtemp(prefix='g', dir=parent)
        os.chmod(self.home, 0o700)
        g = ['gpg', '--homedir', self.home, '--batch', '--pinentry-mode',
             'loopback', '--passphrase', '']
        sh(g + ['--quick-gen-key', uid, 'rsa2048', 'cert', '1d'])
        self.fpr = self._fpr()
        sh(g + ['--quick-add-key', self.fpr, 'rsa2048', 'sign', '1d'])

    def _fpr(self):
        out = sh(['gpg', '--homedir', self.home, '--with-colons', '--list-keys'])
        return [l.split(':')[9] for l in out.splitlines()
                if l.startswith('fpr:')][0]

    def export(self, path):
        data = subprocess.run(['gpg', '--homedir', self.home, '--export',
                               self.fpr], stdout=subprocess.PIPE,
                              check=True).stdout
        write(path, data)
        return path

    def close(self):
        subprocess.run(['gpgconf', '--homedir', self.home, '--kill', 'all'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shutil.rmtree(self.home, ignore_errors=True)


def apt_update(tmp, tree, keyring):
    """apt-get update + apt-cache show against TREE as a file: source, in a
    scratch apt root (no chroot, no root). Returns (ok, output)."""
    root = tempfile.mkdtemp(prefix='aptroot-', dir=tmp)
    for d in ('etc/apt/apt.conf.d', 'etc/apt/preferences.d',
              'etc/apt/sources.list.d', 'etc/apt/trusted.gpg.d',
              'var/lib/apt/lists/partial', 'var/cache/apt/archives/partial',
              'var/lib/dpkg', 'var/log/apt'):
        os.makedirs(os.path.join(root, d), exist_ok=True)
    write(os.path.join(root, 'var/lib/dpkg/status'), '')
    write(os.path.join(root, 'etc/apt/sources.list'),
          'deb [signed-by=%s] file:%s stable main\n'
          % (keyring, os.path.join(tree, 'apt')))
    opts = ['-o', 'Dir=' + root,
            '-o', 'Dir::State::status=' + os.path.join(root, 'var/lib/dpkg/status'),
            '-o', 'Dir::Etc::Parts=' + os.path.join(root, 'etc/apt/apt.conf.d'),
            '-o', 'Dir::Etc::TrustedParts=' + os.path.join(root, 'etc/apt/trusted.gpg.d'),
            '-o', 'Dir::Etc::Trusted=' + os.path.join(root, 'etc/apt/trusted.gpg'),
            '-o', 'APT::Architecture=amd64', '-o', 'APT::Architectures=amd64',
            '-o', 'Acquire::Languages=none', '-o', 'Debug::NoLocking=1',
            '-o', 'APT::Sandbox::User=root' if os.geteuid() == 0 else
            'APT::Sandbox::User=' + os.environ.get('USER', 'root')]
    up = subprocess.run(['apt-get', '-q'] + opts + ['update'],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        universal_newlines=True)
    out = up.stdout
    ok = up.returncode == 0 and 'W:' not in out and 'E:' not in out
    if ok:
        show = subprocess.run(['apt-cache'] + opts + ['show', 'moltbrowser'],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              universal_newlines=True)
        out += show.stdout
        ok = show.returncode == 0
    return ok, out


# ------------------------------------------------------------------ tests


def setUpModule():
    print('tools found: ' + ', '.join(
        '%s=%s' % (t, 'yes' if FOUND[t] else 'NO') for t in TOOLS),
        file=sys.stderr)


@needs('dpkg-deb', 'rpmbuild', 'rpm')
class BuildTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='mlr-')
        self.addCleanup(shutil.rmtree, self.tmp)
        self.assets = os.path.join(self.tmp, 'assets')
        os.makedirs(self.assets)
        self.deb = os.path.join(self.assets, 'MoltBrowser-Linux-x64.deb')
        shutil.copyfile(build_deb(self.tmp), self.deb)
        self.rpm = os.path.join(self.assets, 'MoltBrowser-Linux-x64.rpm')
        shutil.copyfile(build_rpm(self.tmp), self.rpm)

    def build(self, out, bundle=None, *extra):
        argv = ['build', '--out', out, '--deb', self.deb, '--rpm', self.rpm,
                '--date', '1790000000'] + list(extra)
        if bundle:
            argv += ['--pack', bundle]
        code, stdout, err = run_main(argv)
        self.assertEqual(code, 0, err)
        return stdout

    def test_unsigned_tree(self):
        tree = os.path.join(self.tmp, 'tree')
        self.build(tree)
        dist = os.path.join(tree, 'apt', 'dists', 'stable')
        packages = read(os.path.join(dist, 'main/binary-amd64/Packages'), 'r')
        self.assertIn('Filename: pool/main/m/moltbrowser/'
                      'moltbrowser_0.3.0_amd64.deb\n', packages)
        self.assertIn('SHA256: %s\n' % mlr.sha256_file(self.deb), packages)
        self.assertTrue(packages.rstrip('\n').endswith(
            ' A throwaway package for molt_linux_repo_test.py.\n .\n'
            ' Second paragraph.'))
        self.assertEqual(gzip.decompress(read(os.path.join(
            dist, 'main/binary-amd64/Packages.gz'))).decode(), packages)
        release = read(os.path.join(dist, 'Release'), 'r')
        self.assertIn('Date: Mon, 21 Sep 2026 14:13:20 UTC\n', release)
        self.assertNotIn('Valid-Until', release)
        self.assertIn('Architectures: amd64\n', release)
        self.assertEqual(sorted(mlr.release_hashes(release)), [
            'main/binary-amd64/Packages', 'main/binary-amd64/Packages.gz'])
        self.assertTrue(os.path.isfile(os.path.join(
            tree, 'rpm/packages/moltbrowser-0.3.0-1.x86_64.rpm')))
        summary = mlr.load_summary(tree)
        self.assertEqual([(e['type'], e['asset']) for e in summary['packages']],
                         [('deb', 'MoltBrowser-Linux-x64.deb'),
                          ('rpm', 'MoltBrowser-Linux-x64.rpm')])
        self.assertFalse(os.path.exists(os.path.join(dist, 'InRelease')))

    @needs('apt-ftparchive')
    def test_packages_agree_with_apt_ftparchive(self):
        tree = os.path.join(self.tmp, 'tree')
        self.build(tree)
        ours = mlr.parse_control(read(os.path.join(
            tree, 'apt/dists/stable/main/binary-amd64/Packages'), 'r'))
        theirs = mlr.parse_control(sh(['apt-ftparchive', 'packages', 'pool'],
                                      cwd=os.path.join(tree, 'apt')))
        theirs = dict(theirs)
        for name, value in ours:
            self.assertEqual(theirs.get(name), value, name)

    def test_valid_until(self):
        tree = os.path.join(self.tmp, 'tree')
        self.build(tree, None, '--valid-days', '30')
        release = read(os.path.join(tree, 'apt/dists/stable/Release'), 'r')
        self.assertIn('Valid-Until: Wed, 21 Oct 2026 14:13:20 UTC\n', release)

    def test_unsigned_bundle_is_small_and_reproducible(self):
        b1 = os.path.join(self.tmp, 'b1.tar')
        b2 = os.path.join(self.tmp, 'b2.tar')
        self.build(os.path.join(self.tmp, 't1'), b1)
        self.build(os.path.join(self.tmp, 't2'), b2)
        self.assertEqual(read(b1), read(b2))
        with tarfile.open(b1) as t:
            names = t.getnames()
        self.assertIn('repo-summary.json', names)
        self.assertIn('apt/dists/stable/Release', names)
        self.assertFalse([n for n in names if n.endswith(('.deb', '.rpm'))])

    def test_bad_inputs(self):
        tree = os.path.join(self.tmp, 'tree')
        os.makedirs(tree)
        write(os.path.join(tree, 'x'), 'not empty')
        code, _, err = run_main(['build', '--out', tree, '--deb', self.deb])
        self.assertEqual(code, 1)
        self.assertIn('not empty', err)
        code, _, err = run_main(['build', '--out', os.path.join(self.tmp, 'n')])
        self.assertEqual(code, 1)


class ControlTest(unittest.TestCase):
    def test_parse_and_stanza(self):
        fields = mlr.parse_control('Package: ab\nVersion: 1:2.0-1\n'
                                   'Architecture: amd64\nDescription: x\n'
                                   ' more\n .\n end\n')
        self.assertEqual(mlr.pool_path(fields, 'main'),
                         'pool/main/a/ab/ab_2.0-1_amd64.deb')
        stanza = mlr.packages_stanza(fields, 'pool/x.deb', 3, 'f' * 64)
        self.assertTrue(stanza.endswith('SHA256: %s\nDescription: x\n more\n'
                                        ' .\n end\n' % ('f' * 64)))
        self.assertEqual(mlr.pool_path([('Package', 'libfoo'),
                                        ('Version', '1'),
                                        ('Architecture', 'amd64')], 'main'),
                         'pool/main/libf/libfoo/libfoo_1_amd64.deb')
        for bad in ([('Package', '../x'), ('Version', '1'), ('Architecture', 'amd64')],
                    [('Package', 'x'), ('Version', '1/2'), ('Architecture', 'amd64')],
                    [('Package', 'x'), ('Version', '1'), ('Architecture', 's390x')]):
            with self.assertRaises(mlr.Error):
                mlr.pool_path(bad, 'main')

    def test_bundle_name_is_not_an_installer_name(self):
        # The in-app updater picks its installer from the release assets by
        # these substrings (UpdateManager::PlatformAssetMatcher).
        for m in ('.dmg', 'Setup.exe', '.deb', '.rpm', '.tar.gz'):
            self.assertNotIn(m, mlr.BUNDLE_NAME)

    def test_rfc2822(self):
        self.assertEqual(mlr.rfc2822(0), 'Thu, 01 Jan 1970 00:00:00 UTC')

    @needs('gpg')
    def test_armor_roundtrip(self):
        tmp = tempfile.mkdtemp(prefix='mlr-')
        self.addCleanup(shutil.rmtree, tmp)
        key = ThrowawayKey(tmp, 'Armor Test <armor@example.invalid>')
        self.addCleanup(key.close)
        binary = read(key.export(os.path.join(tmp, 'k.gpg')))
        armored = subprocess.run(['gpg', '--homedir', key.home, '--armor',
                                  '--export', key.fpr], stdout=subprocess.PIPE,
                                 check=True).stdout
        self.assertEqual(mlr.dearmor(armored), binary)
        self.assertEqual(mlr.dearmor(mlr.armor(binary)), binary)
        # gpg reads our armor back, checksum included.
        write(os.path.join(tmp, 'ours.asc'), mlr.armor(binary))
        back = subprocess.run(['gpg', '--homedir', key.home, '--dearmor',
                               '--output', '-', os.path.join(tmp, 'ours.asc')],
                              stdout=subprocess.PIPE, check=True).stdout
        self.assertEqual(back, binary)


class BundleSafetyTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='mlr-')
        self.addCleanup(shutil.rmtree, self.tmp)

    def tar(self, entries):
        path = os.path.join(self.tmp, 'evil.tar')
        with tarfile.open(path, 'w') as t:
            for name, kind in entries:
                info = tarfile.TarInfo(name)
                if kind == 'link':
                    info.type = tarfile.SYMTYPE
                    info.linkname = '/etc/passwd'
                    t.addfile(info)
                else:
                    info.size = 1
                    t.addfile(info, io.BytesIO(b'x'))
        return path

    def test_refuses_unsafe_members(self):
        for entries in ([('../escape', 'file')], [('/abs', 'file')],
                        [('a/../../b', 'file')], [('link', 'link')],
                        [('ok', 'file'), ('ok', 'file')],
                        [('we ird', 'file')]):
            dest = tempfile.mkdtemp(dir=self.tmp)
            with self.assertRaises((mlr.Error, FileExistsError), msg=entries):
                mlr.safe_extract(self.tar(entries), dest)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, 'escape')))


@needs('gpg', 'gpgv', 'gpgconf', 'dpkg-deb', 'rpmbuild', 'rpm', 'rpmsign',
       'rpmkeys', 'createrepo_c')
class SignedFlowTest(unittest.TestCase):
    """CI build -> unsigned bundle -> offline sign -> unpack -> verify."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='mlr-')
        cls.key = ThrowawayKey(cls.tmp, 'MoltBrowser Test Archive Key '
                                        '<archive-test@example.invalid>')
        cls.other = ThrowawayKey(cls.tmp, 'Someone Else <else@example.invalid>')
        cls.assets = os.path.join(cls.tmp, 'release-v0.3.0')
        os.makedirs(cls.assets)
        cls.deb = os.path.join(cls.assets, 'MoltBrowser-Linux-x64.deb')
        shutil.copyfile(build_deb(cls.tmp), cls.deb)
        cls.rpm = os.path.join(cls.assets, 'MoltBrowser-Linux-x64.rpm')
        shutil.copyfile(build_rpm(cls.tmp), cls.rpm)
        cls.unsigned_rpm_sha = mlr.sha256_file(cls.rpm)
        # CI: the unsigned bundle goes to the draft release with the packages.
        cls.bundle = os.path.join(cls.assets, mlr.BUNDLE_NAME)
        code, _, err = run_main(['build', '--out', os.path.join(cls.tmp, 'ci'),
                                 '--deb', cls.deb, '--rpm', cls.rpm,
                                 '--pack', cls.bundle])
        assert code == 0, err
        cls.unsigned_bundle = read(cls.bundle)
        # Offline: sign in place.
        code, out, err = run_main(['sign', '--bundle', cls.bundle, '--key',
                                   cls.key.fpr, '--gnupghome', cls.key.home,
                                   '--yes'])
        assert code == 0, err + out
        cls.sign_output = out
        # Online: unpack + verify into the tree to serve.
        cls.site = os.path.join(cls.tmp, 'site')
        cls.keyring = cls.key.export(os.path.join(cls.tmp, 'pinned.gpg'))
        code, _, err = run_main(['unpack', '--bundle', cls.bundle,
                                 '--assets-dir', cls.assets, '--out', cls.site,
                                 '--keyring', cls.keyring, '--fingerprint',
                                 cls.key.fpr])
        assert code == 0, err

    @classmethod
    def tearDownClass(cls):
        cls.key.close()
        cls.other.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def copy_site(self):
        dest = tempfile.mkdtemp(prefix='site-', dir=self.tmp)
        shutil.rmtree(dest)
        shutil.copytree(self.site, dest)
        return dest

    def verify(self, tree, keyring=None, fingerprint=None):
        return run_main(['verify', '--tree', tree, '--keyring',
                         keyring or self.keyring]
                        + (['--fingerprint', fingerprint] if fingerprint else []))

    def test_signed_bundle_contents(self):
        with tarfile.open(self.bundle) as t:
            names = t.getnames()
        for n in ('apt/dists/stable/InRelease', 'apt/dists/stable/Release.gpg',
                  'rpm/repodata/repomd.xml', 'rpm/repodata/repomd.xml.asc',
                  'rpm/packages/moltbrowser-0.3.0-1.x86_64.rpm',
                  mlr.KEYRING_GPG, mlr.KEYRING_ASC, 'moltbrowser.sources',
                  'moltbrowser.repo', 'repo-summary.json'):
            self.assertIn(n, names)
        self.assertFalse([n for n in names if n.endswith('.deb')])
        self.assertNotEqual(read(self.bundle), self.unsigned_bundle)
        self.assertIn('Signed and verified: 1 .deb, 1 .rpm', self.sign_output)

    def test_every_signature_with_gpgv(self):
        dist = os.path.join(self.site, 'apt/dists/stable')
        for sig, data in ((os.path.join(dist, 'Release.gpg'),
                           os.path.join(dist, 'Release')),
                          (os.path.join(dist, 'InRelease'), None),
                          (os.path.join(self.site, 'rpm/repodata/repomd.xml.asc'),
                           os.path.join(self.site, 'rpm/repodata/repomd.xml'))):
            sh(['gpgv', '--keyring', self.keyring, sig] + ([data] if data else []))
        # The published keyring is the throwaway public key, nothing else.
        out = sh(['gpg', '--homedir', self.key.home, '--with-colons',
                  '--show-keys', os.path.join(self.site, mlr.KEYRING_GPG)])
        records = [l.split(':') for l in out.splitlines()]
        self.assertEqual([r[0] for r in records if r[0] in ('pub', 'sec')],
                         ['pub'])
        self.assertEqual([r[9] for r in records if r[0] == 'fpr'][0], self.key.fpr)

    def test_rpm_signed_and_only_signature_changed(self):
        rpm = os.path.join(self.site, 'rpm/packages/moltbrowser-0.3.0-1.x86_64.rpm')
        mlr.rpm_check_signature(rpm, os.path.join(self.site, mlr.KEYRING_ASC))
        self.assertNotEqual(mlr.sha256_file(rpm), self.unsigned_rpm_sha)
        self.assertEqual(mlr.rpm_digests(rpm), mlr.rpm_digests(self.rpm))
        # The release asset itself stays unsigned.
        with self.assertRaises(mlr.Error):
            mlr.rpm_check_signature(self.rpm, os.path.join(self.site,
                                                           mlr.KEYRING_ASC))

    def test_rpm_metadata(self):
        listed = mlr.verify_rpm(self.site, None)
        self.assertEqual(listed, ['rpm/packages/moltbrowser-0.3.0-1.x86_64.rpm'])

    def test_verify_cli(self):
        code, out, err = self.verify(self.site, fingerprint=self.key.fpr)
        self.assertEqual(code, 0, err)
        self.assertIn('1 .deb and 1 .rpm verified', out)
        code, out, err = run_main(['verify', '--bundle', self.bundle,
                                   '--assets-dir', self.assets, '--keyring',
                                   os.path.join(self.site, mlr.KEYRING_ASC)])
        self.assertEqual(code, 0, err)

    @needs('apt-get', 'apt-cache')
    def test_apt_get_update_accepts_it(self):
        ok, out = apt_update(self.tmp, self.site,
                             os.path.join(self.site, mlr.KEYRING_GPG))
        self.assertTrue(ok, out)
        self.assertIn('Version: 0.3.0', out)
        self.assertIn('SHA256: %s' % mlr.sha256_file(self.deb), out)

    @needs('apt-get', 'apt-cache')
    def test_apt_get_update_refuses_tampering(self):
        site = self.copy_site()
        # apt fetches Packages.gz: change both, consistently.
        packages = os.path.join(site, 'apt/dists/stable/main/binary-amd64/Packages')
        evil = read(packages, 'r').replace('Section: web', 'Section: evil')
        write(packages, evil)
        write(packages + '.gz', gzip.compress(evil.encode()))
        ok, out = apt_update(self.tmp, site, os.path.join(site, mlr.KEYRING_GPG))
        self.assertFalse(ok, out)
        self.assertIn('Hash Sum mismatch', out)
        # Signed by someone else.
        site = self.copy_site()
        ok, out = apt_update(self.tmp, site,
                             self.other.export(os.path.join(self.tmp, 'o.gpg')))
        self.assertFalse(ok, out)
        self.assertIn('NO_PUBKEY', out)

    def test_tampering_is_caught(self):
        cases = {
            'Packages changed': lambda s: write(
                os.path.join(s, 'apt/dists/stable/main/binary-amd64/Packages'),
                read(os.path.join(s, 'apt/dists/stable/main/binary-amd64/'
                                     'Packages'), 'r') + '\n'),
            'Release changed': lambda s: write(
                os.path.join(s, 'apt/dists/stable/Release'),
                read(os.path.join(s, 'apt/dists/stable/Release'), 'r')
                .replace('Suite: stable', 'Suite: stab1e')),
            'InRelease removed': lambda s: os.remove(
                os.path.join(s, 'apt/dists/stable/InRelease')),
            'deb swapped': lambda s: shutil.copyfile(
                build_deb(tempfile.mkdtemp(dir=self.tmp), payload=b'evil\n'),
                os.path.join(s, 'apt/pool/main/m/moltbrowser/'
                                'moltbrowser_0.3.0_amd64.deb')),
            'rpm swapped for the unsigned one': lambda s: shutil.copyfile(
                self.rpm, os.path.join(s, 'rpm/packages/'
                                          'moltbrowser-0.3.0-1.x86_64.rpm')),
            'repomd.xml changed': lambda s: write(
                os.path.join(s, 'rpm/repodata/repomd.xml'),
                read(os.path.join(s, 'rpm/repodata/repomd.xml'), 'r') + ' '),
            'repomd.xml.asc removed': lambda s: os.remove(
                os.path.join(s, 'rpm/repodata/repomd.xml.asc')),
        }
        for what, mutate in cases.items():
            site = self.copy_site()
            mutate(site)
            code, _, err = self.verify(site)
            self.assertEqual(code, 1, what)
            self.assertIn('error:', err, what)

    def test_wrong_key_or_fingerprint(self):
        code, _, err = self.verify(self.site, keyring=self.other.export(
            os.path.join(self.tmp, 'other.gpg')))
        self.assertEqual(code, 1)
        self.assertIn('bad or missing signature', err)
        code, _, err = self.verify(self.site, fingerprint=self.other.fpr)
        self.assertEqual(code, 1)
        self.assertIn('not %s' % self.other.fpr, err)

    def test_unsigned_tree_fails_verification(self):
        tree = os.path.join(self.tmp, 'unsigned')
        if not os.path.exists(tree):
            os.makedirs(tree)
            with open(os.path.join(self.tmp, 'u.tar'), 'wb') as f:
                f.write(self.unsigned_bundle)
            mlr.safe_extract(os.path.join(self.tmp, 'u.tar'), tree)
            mlr.materialize(tree, self.assets, kinds=('deb', 'rpm'))
        code, _, err = self.verify(tree)
        self.assertEqual(code, 1)
        self.assertIn('bad or missing signature', err)

    def test_sign_refuses(self):
        # Already signed.
        code, _, err = run_main(['sign', '--bundle', self.bundle, '--key',
                                 self.key.fpr, '--gnupghome', self.key.home,
                                 '--yes', '--out', os.path.join(self.tmp, 'x.tar')])
        self.assertEqual(code, 1)
        self.assertIn('already signed', err)
        # A short key id instead of the fingerprint.
        unsigned = os.path.join(self.tmp, 'again', mlr.BUNDLE_NAME)
        write(unsigned, self.unsigned_bundle)
        code, _, err = run_main(['sign', '--bundle', unsigned, '--key',
                                 self.key.fpr[-16:], '--assets-dir', self.assets,
                                 '--yes'])
        self.assertEqual(code, 1)
        self.assertIn('40-hex', err)
        # The release assets are not the packages CI indexed.
        other_assets = os.path.join(self.tmp, 'other-assets')
        os.makedirs(other_assets, exist_ok=True)
        shutil.copyfile(build_deb(tempfile.mkdtemp(dir=self.tmp), payload=b'x\n'),
                        os.path.join(other_assets, 'MoltBrowser-Linux-x64.deb'))
        shutil.copyfile(self.rpm, os.path.join(other_assets,
                                               'MoltBrowser-Linux-x64.rpm'))
        code, _, err = run_main(['sign', '--bundle', unsigned, '--key',
                                 self.key.fpr, '--gnupghome', self.key.home,
                                 '--assets-dir', other_assets, '--yes'])
        self.assertEqual(code, 1)
        self.assertIn('no file in', err)
        self.assertEqual(read(unsigned), self.unsigned_bundle)


@unittest.skipUnless(shutil.which('bash'), 'needs bash')
@needs('gpg', 'gpgv', 'gpgconf', 'dpkg-deb', 'rpmbuild', 'rpm', 'rpmsign',
       'rpmkeys', 'createrepo_c')
class PublishScriptTest(unittest.TestCase):
    """scripts/linux-repo-publish.sh --to-dir (no GitHub needed)."""

    def test_to_dir(self):
        tmp = tempfile.mkdtemp(prefix='mlr-')
        self.addCleanup(shutil.rmtree, tmp)
        key = ThrowawayKey(tmp, 'Publish Test <publish@example.invalid>')
        self.addCleanup(key.close)
        assets = os.path.join(tmp, 'assets')
        os.makedirs(assets)
        deb = os.path.join(assets, 'MoltBrowser-Linux-x64.deb')
        shutil.copyfile(build_deb(tmp), deb)
        bundle = os.path.join(assets, mlr.BUNDLE_NAME)
        self.assertEqual(run_main(['build', '--out', os.path.join(tmp, 'ci'),
                                   '--deb', deb, '--pack', bundle])[0], 0)
        self.assertEqual(run_main(['sign', '--bundle', bundle, '--key', key.fpr,
                                   '--gnupghome', key.home, '--yes'])[0], 0)
        asc = os.path.join(tmp, 'pinned.asc')
        write(asc, subprocess.run(['gpg', '--homedir', key.home, '--armor',
                                   '--export', key.fpr], stdout=subprocess.PIPE,
                                  check=True).stdout)
        site = os.path.join(tmp, 'site')
        script = os.path.join(REPO_ROOT, 'scripts', 'linux-repo-publish.sh')
        proc = subprocess.run(['bash', script, '--bundle', bundle, '--assets-dir',
                               assets, '--to-dir', site],
                              env=dict(os.environ, MOLT_ARCHIVE_KEY=asc),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              universal_newlines=True)
        self.assertEqual(proc.returncode, 0, proc.stdout)
        self.assertTrue(os.path.isfile(os.path.join(
            site, 'apt/pool/main/m/moltbrowser/moltbrowser_0.3.0_amd64.deb')))
        # With a different pinned key it refuses.
        other = ThrowawayKey(tmp, 'Other <other@example.invalid>')
        self.addCleanup(other.close)
        write(asc, subprocess.run(['gpg', '--homedir', other.home, '--armor',
                                   '--export', other.fpr], stdout=subprocess.PIPE,
                                  check=True).stdout)
        proc = subprocess.run(['bash', script, '--bundle', bundle, '--assets-dir',
                               assets, '--to-dir', os.path.join(tmp, 'site2')],
                              env=dict(os.environ, MOLT_ARCHIVE_KEY=asc),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              universal_newlines=True)
        self.assertNotEqual(proc.returncode, 0, proc.stdout)
        self.assertFalse(os.path.exists(os.path.join(tmp, 'site2', 'apt')))


if __name__ == '__main__':
    unittest.main()
