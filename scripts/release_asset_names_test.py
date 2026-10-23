#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The release paths name the installers the way the updater expects.

The in-app updater downloads MoltBrowser-macOS-arm64.dmg or
MoltBrowser-Linux-x64.{deb,rpm,tar.gz} by name when the GitHub API is
unreachable (UpdateManager::PlatformAssetName), and otherwise picks the
asset whose name contains ".dmg", "Setup.exe", ".deb", ".rpm" or ".tar.gz"
(PlatformAssetMatcher). A release uploaded under other names installs
nowhere, and that is what release.sh and release-linux.sh did. With no
build, this checks:

  * scripts/lib-release-names.sh, which the release scripts source, against
    both functions in src/chrome/browser/molt_ai/update/update_manager.cc;
  * the matchers the provenance and SBOM tools keep their own files clear
    of, against PlatformAssetMatcher();
  * every name against the release tooling: a plain manifest name
    (molt_release_sign.py), and an SBOM name (molt_sbom.py);
  * package-linux.sh, run on a fake build in Linux's own out dir: it writes
    exactly those names and removes older packages;
  * the release scripts and workflows: no versioned installer name
    (MoltBrowser-${VERSION}-..., moltbrowser_${VERSION}_amd64.deb, ...), and
    every installer name they spell out is one of the names.

CI runs it in lint.yml with MOLT_REQUIRE_PACKAGE_TOOLS=1, which fails rather
than skips the package-linux.sh check when dpkg-deb or rpmbuild is missing.

  python3 scripts/release_asset_names_test.py
"""

import ast
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools', 'release_signing'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'sbom'))
import molt_provenance as prov  # noqa: E402
import molt_sbom as ms  # noqa: E402

UPDATE_MANAGER = os.path.join(REPO, 'src', 'chrome', 'browser', 'molt_ai',
                              'update', 'update_manager.cc')
NAMES_LIB = os.path.join(REPO, 'scripts', 'lib-release-names.sh')

# The scripts and workflows that build or upload release installers.
RELEASE_FILES = (
    'scripts/release.sh',
    'scripts/release-all.sh',
    'scripts/release-linux.sh',
    'scripts/release-win.sh',
    'scripts/package-linux.sh',
    'build/docker/build-linux.sh',
    'build/docker/build-linux-fresh.sh',
    '.github/workflows/release.yml',
    '.github/workflows/release-linux.yml',
    '.github/workflows/release-windows-fast.yml',
    '.github/workflows/release-windows-selfhosted.yml',
    '.github/workflows/release-windows.yml',
)

# Versioned file names a release script may still mention, and why: they
# are intermediate files, never uploaded.
VERSIONED_ALLOWED = {
    'scripts/release-win.sh': {
        # package-win.sh's and the NSIS script's outputs, renamed to the
        # fixed names before the upload.
        'MoltBrowser-$VERSION-win-x64-setup.exe',
        'MoltBrowser-$VERSION-win-x64-portable.zip',
    },
    'scripts/package-linux.sh': {
        # rpmbuild's source tarball, inside its build root.
        'moltbrowser-$VERSION.tar.gz',
    },
}

INSTALLER_END = r'(?:\.dmg|\.deb|\.rpm|\.tar\.gz|[Ss]etup\.exe|\.zip|\.AppImage)(?![\w.])'
# A path-like token ending in an installer's file name.
INSTALLER_PATH = re.compile(r'[\w./${}*-]*?' + INSTALLER_END)
# A version variable (shell, or a workflow expression).
VERSION_VAR = re.compile(r'\$\{?(?:VERSION|TAG|PKG_VERSION)\b|\$\{\{')
# A file name built in a variable first (release.sh had
# DMG_NAME="MoltBrowser-${VERSION}-macOS-arm64", then "${DMG_NAME}.dmg").
NAME_ASSIGNMENT = re.compile(r'\b[A-Z_]*NAME=(\S+)')
# An installer name spelled out (globs and variables aside).
LITERAL = re.compile(r'(?:MoltBrowser|moltbrowser)[\w.${}*-]*?' + INSTALLER_END)


def code_lines(rel):
    """(line number, line) of a script or workflow, without comment lines."""
    for i, line in enumerate(read(os.path.join(REPO, rel)).splitlines(), 1):
        if not line.lstrip().startswith('#'):
            yield i, line

REQUIRE_TOOLS = os.environ.get('MOLT_REQUIRE_PACKAGE_TOOLS') == '1'
DPKG_DEB = shutil.which('dpkg-deb')
RPMBUILD = shutil.which('rpmbuild')
if REQUIRE_TOOLS and not (DPKG_DEB and RPMBUILD):
    sys.exit('MOLT_REQUIRE_PACKAGE_TOOLS=1 but dpkg-deb or rpmbuild is missing')


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def release_names():
    """{'MOLT_ASSET_...': name} as scripts/lib-release-names.sh defines them."""
    out = subprocess.run(
        ['bash', '-c', 'source "$1"; for v in ${!MOLT_ASSET_@}; do '
         'printf "%s=%s\\n" "$v" "${!v}"; done', 'bash', NAMES_LIB],
        check=True, stdout=subprocess.PIPE, universal_newlines=True).stdout
    return dict(line.split('=', 1) for line in out.splitlines())


def function_literals(source, signature):
    """The string literals in one function body of update_manager.cc, minus
    the file system paths it probes."""
    start = source.index(signature)
    end = source.index('\n}\n', start)
    return [s for s in re.findall(r'"([^"\\]*)"', source[start:end])
            if s and not s.startswith('/')]


def matchers_in(name, matchers):
    return [m for m in matchers if m in name]


class NamesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.names = release_names()
        source = read(UPDATE_MANAGER)
        cls.updater_names = function_literals(
            source, 'std::string UpdateManager::PlatformAssetName() const {')
        cls.matchers = function_literals(
            source, 'std::string UpdateManager::PlatformAssetMatcher() const {')

    def test_the_lib_defines_every_platform(self):
        self.assertEqual(sorted(self.names), sorted([
            'MOLT_ASSET_MACOS_ARM64_DMG', 'MOLT_ASSET_MACOS_UNIVERSAL_DMG',
            'MOLT_ASSET_LINUX_DEB', 'MOLT_ASSET_LINUX_RPM', 'MOLT_ASSET_LINUX_TAR',
            'MOLT_ASSET_LINUX_APPIMAGE', 'MOLT_ASSET_WINDOWS_SETUP',
            'MOLT_ASSET_WINDOWS_ZIP']))
        self.assertEqual(len(set(self.names.values())), len(self.names))

    def test_names_are_the_ones_the_updater_downloads(self):
        n = self.names
        self.assertEqual(sorted(self.updater_names), sorted([
            n['MOLT_ASSET_MACOS_ARM64_DMG'], n['MOLT_ASSET_LINUX_DEB'],
            n['MOLT_ASSET_LINUX_RPM'], n['MOLT_ASSET_LINUX_TAR']]))

    def test_each_installer_matches_its_own_matcher_only(self):
        self.assertEqual(sorted(self.matchers),
                         sorted(['.dmg', 'Setup.exe', '.deb', '.rpm', '.tar.gz']))
        n = self.names
        for var, matcher in (('MOLT_ASSET_MACOS_ARM64_DMG', '.dmg'),
                             ('MOLT_ASSET_MACOS_UNIVERSAL_DMG', '.dmg'),
                             ('MOLT_ASSET_LINUX_DEB', '.deb'),
                             ('MOLT_ASSET_LINUX_RPM', '.rpm'),
                             ('MOLT_ASSET_LINUX_TAR', '.tar.gz'),
                             ('MOLT_ASSET_WINDOWS_SETUP', 'Setup.exe')):
            self.assertEqual(matchers_in(n[var], self.matchers), [matcher], var)
        # Never offered as an update.
        for var in ('MOLT_ASSET_LINUX_APPIMAGE', 'MOLT_ASSET_WINDOWS_ZIP'):
            self.assertEqual(matchers_in(n[var], self.matchers), [], var)

    def test_the_tools_keep_their_files_clear_of_the_same_matchers(self):
        self.assertEqual(sorted(prov.UPDATER_ASSET_MATCHERS), sorted(self.matchers))
        self.assertEqual(sorted(ms.UPDATER_ASSET_MATCHERS), sorted(self.matchers))

    def test_the_release_tooling_accepts_every_name(self):
        # molt_release_sign.py's manifest names (it needs "cryptography" to
        # import, so read its pattern from the source).
        pattern = re.search(r"^PLAIN_NAME = re\.compile\((r'[^']*')",
                            read(os.path.join(REPO, 'tools', 'release_signing',
                                              'molt_release_sign.py')), re.M)
        plain_name = re.compile(ast.literal_eval(pattern.group(1)), re.ASCII)
        for name in self.names.values():
            self.assertTrue(plain_name.fullmatch(name), name)
            self.assertTrue(prov.PLAIN_NAME.fullmatch(name), name)
            sbom = ms.sbom_name_for(name)   # raises if the updater could take it
            self.assertEqual(matchers_in(sbom, self.matchers), [], sbom)


class ReleaseFilesTest(unittest.TestCase):
    def test_no_versioned_installer_names(self):
        for rel in RELEASE_FILES:
            allowed = VERSIONED_ALLOWED.get(rel, set())
            for i, line in code_lines(rel):
                for m in INSTALLER_PATH.finditer(line):
                    # The file name only: a release URL has the tag in its path.
                    name = m.group(0).rsplit('/', 1)[-1]
                    if VERSION_VAR.search(name):
                        self.assertIn(name, allowed,
                                      '%s:%d names a versioned installer: %s'
                                      % (rel, i, line.strip()))
                for m in NAME_ASSIGNMENT.finditer(line):
                    self.assertIsNone(VERSION_VAR.search(m.group(1)),
                                      '%s:%d builds a versioned file name: %s'
                                      % (rel, i, line.strip()))

    def test_spelled_out_names_are_the_fixed_ones(self):
        names = set(release_names().values())
        for rel in RELEASE_FILES:
            for i, line in code_lines(rel):
                for m in LITERAL.finditer(line):
                    token = m.group(0)
                    if '*' in token or '$' in token:
                        continue   # cleanup globs; variables are checked above
                    self.assertIn(token, names, '%s:%d: %s' % (rel, i, line.strip()))

    def test_the_workflows_upload_the_updater_names(self):
        n = release_names()
        for rel, vars_ in (
                ('.github/workflows/release.yml', ('MOLT_ASSET_MACOS_ARM64_DMG',)),
                ('.github/workflows/release-linux.yml',
                 ('MOLT_ASSET_LINUX_DEB', 'MOLT_ASSET_LINUX_RPM',
                  'MOLT_ASSET_LINUX_TAR'))):
            text = read(os.path.join(REPO, rel))
            for var in vars_:
                self.assertIn('dist/' + n[var], text, rel)

    def test_release_scripts_source_the_names(self):
        for rel in ('scripts/release.sh', 'scripts/release-all.sh',
                    'scripts/release-linux.sh', 'scripts/release-win.sh',
                    'scripts/package-linux.sh', 'build/docker/build-linux-fresh.sh'):
            self.assertRegex(read(os.path.join(REPO, rel)),
                             r'\nsource "\$(?:SCRIPT_DIR|ROOT_DIR/scripts)/lib-release-names\.sh"\n',
                             rel)


class PackageLinuxTest(unittest.TestCase):
    """package-linux.sh on a fake build: the files it leaves in dist/."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='molt-names-')
        self.addCleanup(shutil.rmtree, self.tmp)
        os.makedirs(os.path.join(self.tmp, 'scripts'))
        for f in ('package-linux.sh', 'lib-release-names.sh'):
            shutil.copy2(os.path.join(REPO, 'scripts', f),
                         os.path.join(self.tmp, 'scripts', f))
        icon = os.path.join('branding', 'MoltBrowser.iconset', 'icon_256x256.png')
        os.makedirs(os.path.dirname(os.path.join(self.tmp, icon)))
        shutil.copy2(os.path.join(REPO, icon), os.path.join(self.tmp, icon))
        self.dist = os.path.join(self.tmp, 'dist')
        os.makedirs(self.dist)

    def fake_build(self, out_dir):
        d = os.path.join(self.tmp, 'chromium', 'src', 'out', out_dir)
        os.makedirs(os.path.join(d, 'locales'))
        with open(os.path.join(d, 'chrome'), 'wb') as f:
            f.write(b'#!/bin/sh\necho fake\n')
        os.chmod(os.path.join(d, 'chrome'), 0o755)
        with open(os.path.join(d, 'resources.pak'), 'wb') as f:
            f.write(b'pak')

    def package(self):
        env = dict(os.environ)
        env.pop('MOLT_RELEASE_BUILD', None)
        return subprocess.run(
            ['bash', os.path.join(self.tmp, 'scripts', 'package-linux.sh'),
             '--version', 'v0.3.0', '--format', 'all'],
            cwd=self.tmp, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True)

    def test_writes_the_fixed_names_from_linux_out_dir(self):
        self.fake_build('MoltBrowser-linux')
        # Left over from earlier runs and releases.
        for stale in ('moltbrowser_0.2.5_amd64.deb', 'moltbrowser-0.2.5-1.x86_64.rpm',
                      'MoltBrowser-0.2.5-linux-x64.tar.gz', 'MoltBrowser-Linux-x64.rpm'):
            open(os.path.join(self.dist, stale), 'w').close()
        r = self.package()
        self.assertEqual(r.returncode, 0, r.stdout[-3000:])
        n = release_names()
        expected = {n['MOLT_ASSET_LINUX_TAR']}
        if DPKG_DEB:
            expected.add(n['MOLT_ASSET_LINUX_DEB'])
        if RPMBUILD:
            expected.add(n['MOLT_ASSET_LINUX_RPM'])
        if shutil.which('appimagetool'):
            expected.add(n['MOLT_ASSET_LINUX_APPIMAGE'])
        made = {f for f in os.listdir(self.dist) if not f.startswith('.')}
        self.assertEqual(made, expected)
        if DPKG_DEB:
            version = subprocess.run(
                [DPKG_DEB, '-f', os.path.join(self.dist, n['MOLT_ASSET_LINUX_DEB']),
                 'Version'], check=True, stdout=subprocess.PIPE,
                universal_newlines=True).stdout.strip()
            self.assertEqual(version, '0.3.0')
        # The Flatpak manifest points at the tarball the release uploads.
        tar = os.path.join(self.dist, n['MOLT_ASSET_LINUX_TAR'])
        with open(tar, 'rb') as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        manifest = read(os.path.join(self.tmp, 'installer', 'linux', 'flatpak',
                                     'com.geneye.MoltBrowser.yml'))
        self.assertIn('/releases/download/v0.3.0/%s\n' % n['MOLT_ASSET_LINUX_TAR'],
                      manifest)
        self.assertIn('sha256: %s\n' % digest, manifest)

    def test_never_packages_the_macos_out_dir(self):
        # A Mac checkout's out/MoltBrowser holds the macOS build.
        self.fake_build('MoltBrowser')
        r = self.package()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('out/MoltBrowser-linux/chrome', r.stdout)
        self.assertEqual([f for f in os.listdir(self.dist) if not f.startswith('.')], [])


if __name__ == '__main__':
    unittest.main()
